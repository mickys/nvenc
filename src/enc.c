// enc.c: see enc.h.
#include "enc.h"
#include "blocklinear.h"
#include "h264hdr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "class/clc56f.h"
#include "class/cl2080_notification.h"
#include "clc9b7.h"
#define NV_NVENC_8_2 1
#include "nvenc_drv.h"

// picture setup buffer: the 512-byte header, then one control of each kind
enum { OFF_ME = 0x400, OFF_MD = 0x500, OFF_Q = 0x600, OFF_SLICE = 0x700, SETUP_SIZE = 0x1000 };   // room for ENC_MAX_SLICES controls
enum { LOG2_MAX_FRAME_NUM = 8 };

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int enc_open(Enc *e, const EncConfig *cfg)
{
    memset(e, 0, sizeof *e);
    e->cfg = *cfg;
    if (e->cfg.num_refs < 1) e->cfg.num_refs = 1;
    if (e->cfg.num_refs > ENC_MAX_REFS) e->cfg.num_refs = ENC_MAX_REFS;
    int W = cfg->width, H = cfg->height;
    e->wmb = (W + 15) / 16; e->hmb = (H + 15) / 16;
    Rm *rm = &e->rm; rm->verbose = cfg->verbose;
    if (rm_open(rm)) return -1;
    rm->verbose = cfg->verbose;
    if (rm_va_init(rm)) return -1;

    // input: NVENC's input layout (block_height 2); the engine reads 8-12 KB past the chroma plane, hence the slack
    e->pitch = ((size_t)e->wmb * 16 + 63) & ~(size_t)63;
    e->lumaSz = (nvenc_in_plane_size(e->pitch, (size_t)e->hmb * 16) + 255) & ~(size_t)255;
    e->chromaSz = nvenc_in_plane_size(e->pitch, (size_t)e->hmb * 8);
    // reconstructed pictures: block-linear block_height 2, rows rounded up to 32 (the engine writes that far)
    e->rPitch = ((size_t)e->wmb * 16 + 255) & ~(size_t)255;
    e->rLuma = (e->rPitch * (((size_t)e->hmb * 16 + 31) & ~(size_t)31) + 255) & ~(size_t)255;
    e->rSlot = (e->rLuma + e->rPitch * (((size_t)e->hmb * 8 + 31) & ~(size_t)31) + 0xffff) & ~(size_t)0xffff;
    int slots = e->cfg.num_refs + 1;
    size_t mbs = (size_t)e->wmb * e->hmb;
    size_t histSz = ((((size_t)(W - 1) + 16) / 16 + 16) * H264_HIST_BLOCK_SIZE + 0xffff) & ~(size_t)0xffff;
    size_t meSz = (mbs * 64 + 0xffff) & ~(size_t)0xffff;   // ME predictor data per MB (the reference client uses 4 MB at any size)
    if (rm_mem_alloc(rm, &e->in, e->lumaSz + e->chromaSz + (64 << 10), 0x1000, true, true, true) ||
        rm_mem_alloc(rm, &e->recon, e->rSlot * (size_t)slots, 0x10000, false, false, true) ||
        rm_mem_alloc(rm, &e->setup, SETUP_SIZE, 0x1000, true, true, true) ||
        rm_mem_alloc(rm, &e->bs, (mbs * 384 + (1 << 20) + 0xffff) & ~(size_t)0xffff, 0x1000, true, true, true) ||
        rm_mem_alloc(rm, &e->status, 0x10000, 0x1000, true, true, true) ||
        rm_mem_alloc(rm, &e->hist, histSz, 0x1000, false, false, true) ||
        rm_mem_alloc(rm, &e->coloc, (mbs * 64 + 0xffff) & ~(size_t)0xffff, 0x1000, false, false, true) ||
        rm_mem_alloc(rm, &e->mepred[0], meSz, 0x1000, false, false, true) ||
        rm_mem_alloc(rm, &e->mepred[1], meSz, 0x1000, false, false, true) ||
        rm_mem_alloc(rm, &e->rcproc, 0x10000, 0x1000, true, true, true) ||
        rm_mem_alloc(rm, &e->rcdata, 0x10000, 0x1000, true, true, true) ||
        rm_mem_alloc(rm, &e->sem, 0x1000, 0x1000, true, true, true)) { fprintf(stderr, "enc: allocation failed\n"); return -1; }
    memset(e->in.cpu, 0, e->in.size); memset(e->rcproc.cpu, 0, e->rcproc.size); memset(e->rcdata.cpu, 0, e->rcdata.size);
    ((volatile NvU32 *)e->sem.cpu)[0] = 0;
    if (!e->cfg.fps) e->cfg.fps = 60;
    if (!e->cfg.vbv_bits) e->cfg.vbv_bits = e->cfg.bitrate;
    if (!e->cfg.max_bitrate) e->cfg.max_bitrate = e->cfg.bitrate;

    NV_NVENC_ALLOCATION_PARAMETERS ep = { .size = sizeof ep, .engineInstance = (NvU32)cfg->engine };
    if (chan_create(rm, &e->chan, cfg->engine ? NV2080_ENGINE_TYPE_NVENC1 : NV2080_ENGINE_TYPE_NVENC0, NVC9B7_VIDEO_ENCODER, &ep)) return -1;
    chan_mthd(&e->chan, 0, NVC56F_SET_OBJECT, 1, (NvU32)NVC9B7_VIDEO_ENCODER);
    return 0;
}

void enc_close(Enc *e)
{
    chan_destroy(&e->chan);
    RmMem *m[] = { &e->in, &e->recon, &e->setup, &e->bs, &e->status, &e->hist, &e->coloc, &e->mepred[0], &e->mepred[1], &e->sem,
                   &e->rcproc, &e->rcdata };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (m[i]->hPhys) rm_mem_free(&e->rm, m[i]);
    rm_close(&e->rm);
}

static H264Params hdr_params(const Enc *e)
{
    return (H264Params){ .width = e->cfg.width, .height = e->cfg.height, .profile_idc = 77, .level_idc = 51,
        .log2_max_frame_num_minus4 = LOG2_MAX_FRAME_NUM - 4, .poc_type = 2, .num_ref_frames = e->cfg.num_refs,
        .num_ref_idx_l0_minus1 = e->cfg.num_refs - 1, .entropy_cabac = 0, .pic_init_qp = 26 };
}

size_t enc_headers(Enc *e, uint8_t *out)
{
    H264Params hp = hdr_params(e);
    size_t n = h264_write_sps(&hp, out);
    return n + h264_write_pps(&hp, out + n);
}

void enc_load_nv12(Enc *e, const uint8_t *y, size_t yStride, const uint8_t *uv, size_t uvStride)
{
    nvenc_in_tile_plane(e->in.cpu, y, yStride, (size_t)e->cfg.width, (size_t)e->cfg.height, e->pitch);
    nvenc_in_tile_plane((uint8_t *)e->in.cpu + e->lumaSz, uv, uvStride, (size_t)e->cfg.width, (size_t)(e->cfg.height + 1) / 2, e->pitch);
}

// ME control: predictor sources, search stamps and FBM settings as the reference client sets them (docs/traces,
// T3.02); an IDR only uses the spatial and constant hints.
static void setup_me(nvenc_h264_me_control_s *me, bool p)
{
    me->refinement_mode = 1; me->refine_on_search_enable = 1;
    me->fps_mvcost = 1; me->sps_mvcost = 1; me->sps_cost_func = 1; me->partDecisionMadeByFPP = 1;
    nvenc_h264_me_hint_cfg_s *h = &me->predsrc;
    h->self_spatial_search = 1; h->self_spatial_enable = 1; h->const_mv_search = 1; h->const_mv_enable = 1;
    nvenc_h264_me_stamp_s *sh = &me->shape0;
    if (!p) { sh[0].bitmask[0] = 0x1c1c0000; sh[0].bitmask[1] = 0x1c; return; }
    h->self_temporal_stamp_l0 = 4; h->self_temporal_stamp_l1 = 5; h->self_temporal_search = 1; h->self_temporal_enable = 1;
    h->coloc_stamp_l0 = 1; h->coloc_stamp_l1 = 1;
    h->self_spatial_stamp_l0 = 2; h->self_spatial_stamp_l1 = 3; h->const_mv_stamp_l1 = 1;
    h->stamp_refidx1_stamp = 6; h->stamp_refidx2_stamp = 6; h->stamp_refidx3_stamp = 6;
    for (int i = 0; i < 7; i++) { sh[i].bitmask[0] = 0x1e0c0000; sh[i].bitmask[1] = 0x0c; sh[i].hor_adjust = 1; }
    me->spatial_hint_pattern = 6; me->temporal_hint_pattern = 4; me->Cu16partDecisionMadeByFPP = 1;
    me->fbm_op_winner_num_p_frame = 1; me->fbm_op_winner_num_b_frame_l0 = 1; me->fbm_op_winner_num_b_frame_l1 = 1;
    me->fbm_select_best_cu32_parttype_num = 3; me->sps_evaluate_merge_cand_num = 5; me->fps_quad_thresh_hold = 50;
    me->external_hint_order = 1; me->coloc_hint_order = 1;
    me->hint_type0 = 0; me->hint_type1 = 1; me->hint_type2 = 2; me->hint_type3 = 3; me->hint_type4 = 4;   // must cover 0x1f
}

// MD control: mode decision as the reference client sets it. intra_ssd_cnt_* must be >= 1 or the engine hangs (T2.05e).
static void setup_md(nvenc_h264_md_control_s *md, bool p)
{
    md->intra_luma16x16_mode_enable = 0xf; md->intra_chroma_mode_enable = 0xf; md->inter_penalty_factor_for_ip1 = 16;
    md->l0_part_16x16_enable = 1; md->l0_part_16x8_enable = 1; md->l0_part_8x16_enable = 1; md->l0_part_8x8_enable = 1;
    md->pskip_enable = 1;
    md->bias_inter_16x8 = -1; md->bias_inter_8x16 = -1; md->bias_inter_8x8 = -1; md->bias_pskip = -1; md->bias_bdir = 2;
    md->bias_intra_over_inter = -2; md->bias_intra_16x16 = -4; md->bias_intra_8x8 = -2; md->bias_intra_4x4 = -1;
    md->intra_nxn_bias_multiplier = 32; md->intra_most_prob_bias_multiplier = 3; md->pskip_bias_multiplier = 2;
    md->pskip_esc_threshold = 9; md->bdirect_esc_threshold = 19;
    md->tempmv_wt_spread_threshold = 0xffff; md->tempmv_wt_distort_threshold = 0xffff;
    md->mv_cost_enable = 1; md->early_intra_mode_type_16x16dc = 1; md->early_intra_mode_type_16x16h = 1; md->early_intra_mode_type_16x16v = 1;
    md->ip_search_mode = 7; md->multiply_bias_with_lambda = 1; md->early_termination_ip1 = 1; md->priority_ipred_type_ip1 = 1;
    md->intra_ssd_cnt_4x4 = 1; md->intra_ssd_cnt_8x8 = 1; md->intra_ssd_cnt_16x16 = 1; md->skip_evaluate_enable = 1;
    md->rdo_level = 1; md->tu_search_num = 3; md->num_1div8_lambda_ssd = 1;
    md->luma_residual_zero_eval = p ? 2 : 0;
}

int enc_encode(Enc *e, bool force_idr, EncFrame *out)
{
    memset(out, 0, sizeof *out);
    Chan *c = &e->chan;
    int W = e->cfg.width, H = e->cfg.height;
    bool idr = force_idr || e->seq == 0 || (e->cfg.idr_period > 0 && e->sinceIdr >= (unsigned)e->cfg.idr_period);
    if (idr) { e->frame_num = 0; e->sinceIdr = 0; e->nRefs = 0; }
    int slot = e->nextSlot;                                   // where this picture's reconstruction goes
    int nRefs = idr ? 0 : e->nRefs;

    // ---- picture setup ----
    memset(e->setup.cpu, 0, SETUP_SIZE);
    nvenc_h264_drv_pic_setup_s *ps = e->setup.cpu;
    ps->magic = NV_NVENC_DRV_MAGIC_VALUE;
    nvenc_h264_surface_cfg_s ic = { .frame_width_minus1 = (unsigned short)(W - 1), .frame_height_minus1 = (unsigned short)(H - 1),
        .sfc_pitch = (unsigned short)e->pitch, .sfc_pitch_chroma = (unsigned short)e->pitch, .block_height = 2 };
    nvenc_h264_surface_cfg_s rc = ic; rc.sfc_pitch = rc.sfc_pitch_chroma = (unsigned short)e->rPitch;
    ps->input_cfg = ic; ps->refpic_cfg = rc; ps->outputpic_cfg = rc;   // plane offsets 0: each plane has its own method
    H264Params hp = hdr_params(e);
    ps->sps_data.profile_idc = (unsigned)hp.profile_idc; ps->sps_data.level_idc = (unsigned)hp.level_idc; ps->sps_data.chroma_format_idc = 1;
    ps->sps_data.pic_order_cnt_type = 2; ps->sps_data.log2_max_frame_num_minus4 = LOG2_MAX_FRAME_NUM - 4;
    ps->sps_data.frame_mbs_only = 1;
    ps->pps_data.num_ref_idx_l0_active_minus1 = (unsigned)(e->cfg.num_refs - 1);
    ps->pps_data.deblocking_filter_control_present_flag = 1;
    nvenc_h264_rc_s *r = &ps->rate_control;
    r->QP[0] = (unsigned char)e->cfg.qp_p; r->QP[1] = (unsigned char)e->cfg.qp_p; r->QP[2] = (unsigned char)e->cfg.qp_i;
    for (int i = 0; i < 3; i++) { r->minQP[i] = 0; r->maxQP[i] = 51; r->rhopbi[i] = 256; }
    r->framerate = e->cfg.fps; r->gop_length = 0xffffffffu;
    if (e->cfg.rc_mode) {   // field encodings as the reference client fills them (it runs const QP, though): T3.06
        double pixels = (double)W * H, perFrame = (double)e->cfg.bitrate / e->cfg.fps;
        r->hrd_type = 2;                                     // NAL HRD
        r->maxQPD = 6; r->baseQPD = 3;
        r->framerate = e->cfg.fps << 8;
        r->buffersize = (unsigned)e->cfg.vbv_bits; r->nal_cpb_size = r->vcl_cpb_size = e->cfg.vbv_bits;
        r->nal_bitrate = e->cfg.max_bitrate; r->vcl_bitrate = e->cfg.max_bitrate;
        r->gop_length = e->cfg.idr_period > 0 ? (unsigned)e->cfg.idr_period : 0xffffffffu;
        r->Np = e->cfg.idr_period > 0 ? (e->cfg.idr_period - 1) << 4 : 0x7fffffff;
        r->Ravg = r->R = (int)(perFrame / pixels * 256);       // bits per pixel per frame, 23.8
        r->Bmin = 0; r->iSizeRatioX = 1; r->iSizeRatioY = 1;
        // single_frame_VBV stays 0: set to 1 (with a one-frame VBV) the rate went to 3x the target at QP 5 (T3.08). A
        // one-frame VBV by itself works: frames stay under bitrate/fps.
    }

    nvenc_h264_pic_control_s *pc = &ps->pic_control;
    memset(pc->l0, -2, sizeof pc->l0); memset(pc->l1, -2, sizeof pc->l1);   // -2 = unused, as the reference client
    for (int i = 0; i < nRefs; i++) { pc->l0[i] = (char)(2 * e->slotOf[i]); pc->temp_dist_l0[i] = (char)(i + 1); }   // field-indexed DPB slots
    pc->frame_num = (unsigned short)e->frame_num; pc->pic_order_cnt_lsb = (unsigned short)(2 * e->sinceIdr);
    pc->slice_control_offset = OFF_SLICE; pc->me_control_offset = OFF_ME; pc->md_control_offset = OFF_MD; pc->q_control_offset = OFF_Q;
    pc->hist_buf_size = (unsigned)e->hist.size; pc->bitstream_buf_size = (unsigned)e->bs.size;
    pc->pic_type = idr ? 3 : 0; pc->ref_pic_flag = 1; pc->cabac_zero_word_enable = 1; pc->e4byteStartCode = 1;
    pc->slice_mode = 0;   // 1 hangs the engine (T2.05e); one control with all MBs = one slice

    // slices: one slice control per slice, whole MB rows, under slice_mode 0 (slice_mode 1 hangs the engine: T2.05e, T3.09)
    int nsl = e->cfg.slices < 1 ? 1 : e->cfg.slices > ENC_MAX_SLICES ? ENC_MAX_SLICES : e->cfg.slices;
    if (nsl > e->hmb) nsl = e->hmb;
    pc->num_forced_slices_minus1 = (unsigned char)(nsl - 1);
    nvenc_h264_slice_control_s *sc = (void *)((NvU8 *)e->setup.cpu + OFF_SLICE);
    for (int k = 0; k < nsl; k++) {
        int mb = ((k + 1) * e->hmb / nsl - k * e->hmb / nsl) * e->wmb;   // rows spread evenly
        sc[k].num_mb = (unsigned)mb; sc[k].qp_avr = (unsigned)(idr ? e->cfg.qp_i : e->cfg.qp_p); sc[k].qp_slice_min = 0; sc[k].qp_slice_max = 51;
        if (!idr) { sc[k].num_ref_idx_active_override_flag = 1; sc[k].num_ref_idx_l0_active_minus1 = (unsigned)(nRefs - 1); }
    }

    setup_me((void *)((NvU8 *)e->setup.cpu + OFF_ME), !idr);
    setup_md((void *)((NvU8 *)e->setup.cpu + OFF_MD), !idr);

    // ---- methods ----
    if (chan_push_used(c) > 0x8000) chan_rewind(c);
    NvU64 recOut = e->recon.gpu + (NvU64)slot * e->rSlot;
    RmMem *meIn = &e->mepred[e->seq & 1], *meOut = &e->mepred[(e->seq + 1) & 1];
    chan_mthd(c, 0, NVC9B7_SET_APPLICATION_ID, 1, (NvU32)NVC9B7_SET_APPLICATION_ID_ID_NVENC_H264);
    chan_mthd(c, 0, NVC9B7_SET_CONTROL_PARAMS, 1, DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _CODEC_TYPE, NVC9B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264) |
              DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _FORCE_OUT_PIC, 1) | DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _RCMODE, (NvU32)e->cfg.rc_mode));
    chan_mthd(c, 0, NVC9B7_SET_PICTURE_INDEX, 1, e->seq);
    struct { NvU32 m; NvU64 a; } bm[] = {
        { NVC9B7_SET_IN_DRV_PIC_SETUP, e->setup.gpu }, { NVC9B7_SET_IOHISTORY, e->hist.gpu },
        { NVC9B7_SET_OUT_COLOC_DATA, e->coloc.gpu }, { NVC9B7_SET_OUT_REF_PIC_LUMA, recOut }, { NVC9B7_SET_OUT_REF_PIC_CHROMA, recOut + e->rLuma },
        { NVC9B7_SET_OUT_ENC_STATUS, e->status.gpu }, { NVC9B7_SET_OUT_BITSTREAM, e->bs.gpu },
        { NVC9B7_SET_IN_CUR_PIC, e->in.gpu }, { NVC9B7_SET_IN_CUR_PIC_CHROMA_U, e->in.gpu + e->lumaSz },
        { NVC9B7_SET_IN_CUR_PIC_CHROMA_V, e->in.gpu + e->lumaSz }, { NVC9B7_SET_IO_RC_PROCESS, e->rcproc.gpu },
        { NVC9B7_SET_IN_RCDATA, e->rcdata.gpu } };
    for (size_t i = 0; i < sizeof bm / sizeof bm[0]; i++) chan_mthd(c, 0, bm[i].m, 1, (NvU32)(bm[i].a >> 8));
    if (!idr) {
        chan_mthd(c, 0, NVC9B7_SET_IN_COLOC_DATA, 1, (NvU32)(e->coloc.gpu >> 8));
        chan_mthd(c, 0, NVC9B7_SET_IN_MEPRED_DATA, 1, (NvU32)(meIn->gpu >> 8));
        chan_mthd(c, 0, NVC9B7_SET_OUT_MEPRED_DATA, 1, (NvU32)(meOut->gpu >> 8));
        for (int i = 0; i < nRefs; i++) {
            NvU64 ra = e->recon.gpu + (NvU64)e->slotOf[i] * e->rSlot;
            chan_mthd(c, 0, NVC9B7_SET_IN_REF_PIC0_LUMA + 4 * (NvU32)e->slotOf[i], 1, (NvU32)(ra >> 8));
            chan_mthd(c, 0, NVC9B7_SET_IN_REF_PIC0_CHROMA + 4 * (NvU32)e->slotOf[i], 1, (NvU32)((ra + e->rLuma) >> 8));
        }
    }
    chan_mthd(c, 0, NVC9B7_EXECUTE, 1, 0u);
    NvU32 payload = ++e->seq;
    chan_mthd(c, 0, NVC9B7_SEMAPHORE_A, 3, (NvU32)(e->sem.gpu >> 32), (NvU32)e->sem.gpu, payload);
    chan_mthd(c, 0, NVC9B7_SEMAPHORE_D, 1, DRF_DEF(C9B7, _SEMAPHORE_D, _STRUCTURE_SIZE, _ONE) |
              DRF_DEF(C9B7, _SEMAPHORE_D, _OPERATION, _RELEASE) | DRF_DEF(C9B7, _SEMAPHORE_D, _PAYLOAD_SIZE, _32BIT));
    volatile NvU32 *sv = e->sem.cpu;
    double t0 = now_ms(); chan_submit(c);
    while (*sv != payload && !chan_error(c) && now_ms() - t0 < 3000) { }
    out->ms = now_ms() - t0;

    nvenc_pic_stat_s *st = e->status.cpu;
    out->idr = idr; out->frame_num = e->frame_num; out->poc = 2 * e->sinceIdr;
    out->bits = st->total_bit_count; out->ucode_error = st->ucode_error_status; out->avg_qp = st->avgQP;
    out->intra_mbs = st->intra_mb_count; out->inter_mbs = st->inter_mb_count;
    if (*sv != payload) {
        fprintf(stderr, "enc: frame %u %s (notifier 0x%x info32 0x%x)\n", payload, chan_error(c) ? "engine error" : "timeout",
                chan_error(c), ((volatile NvNotification *)c->notifier.cpu)[0].info32);
        return -1;
    }
    // error_status is 2 on good frames too: judge by the microcode error and the bit count
    if (st->ucode_error_status || !st->total_bit_count || st->total_bit_count / 8 >= e->bs.size) return -1;
    out->data = (const uint8_t *)e->bs.cpu + st->bitstream_start_pos; out->size = st->total_bit_count / 8;

    // DPB: this picture becomes the most recent reference; the oldest drops out past num_refs
    int keep = e->nRefs < e->cfg.num_refs ? e->nRefs : e->cfg.num_refs - 1;
    if (idr) keep = 0;
    memmove(e->slotOf + 1, e->slotOf, sizeof e->slotOf[0] * (size_t)keep);
    e->slotOf[0] = slot; e->nRefs = keep + 1;
    for (int s = 0; s <= e->cfg.num_refs; s++) {             // next free slot: one not in the DPB
        int used = 0; for (int i = 0; i < e->nRefs; i++) used |= e->slotOf[i] == s;
        if (!used) { e->nextSlot = s; break; }
    }
    e->frame_num = (e->frame_num + 1) % (1u << LOG2_MAX_FRAME_NUM);
    e->sinceIdr++;
    return 0;
}
