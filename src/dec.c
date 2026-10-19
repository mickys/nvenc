// dec.c: see dec.h. Section numbers refer to ITU-T H.264.
#include "dec.h"
#include "blocklinear.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "class/clc56f.h"
#include "class/cl2080_notification.h"
#include "clc9b0.h"
#include "nvdec_drv.h"

static NvU32 hi(NvU64 a) { return (NvU32)(a >> 8); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int h264dec_open(H264Dec *d, Rm *rm, unsigned maxW, unsigned maxH)
{
    memset(d, 0, sizeof *d);
    d->rm = rm; d->maxW = (maxW + 15) & ~15u; d->maxH = (maxH + 15) & ~15u; d->max_long_term_frame_idx = -1;
    d->pitch = ((size_t)d->maxW + 63) & ~(size_t)63;
    NV_NVDEC_ALLOCATION_PARAMETERS dp = { .size = sizeof dp, .prohibitMultipleInstances = 0, .engineInstance = 0 };
    if (chan_create(rm, &d->chan, NV2080_ENGINE_TYPE_NVDEC0, NVC9B0_VIDEO_DECODER, &dp)) { fprintf(stderr, "dec: channel failed\n"); return -1; }
    const size_t mbs = (size_t)(d->maxW / 16) * (d->maxH / 16);
    d->auCap = (size_t)d->maxW * d->maxH * 2 > (4u << 20) ? (size_t)d->maxW * d->maxH * 2 : (4u << 20);
    if (!(d->au = malloc(d->auCap))) return -1;
    const size_t luma = nvenc_in_plane_size(d->pitch, d->maxH) + 65536, chroma = nvenc_in_plane_size(d->pitch, d->maxH / 2) + 65536;
    struct { RmMem *m; size_t size; } b[] = {
        { &d->setup, 0x10000 }, { &d->bs, d->auCap + 0x10000 }, { &d->offs, 0x2000 }, { &d->status, 0x1000 },
        { &d->coloc, mbs * 256 * DEC_SURFACES }, { &d->hist, 8u << 20 }, { &d->mbhist, 8u << 20 }, { &d->sem, 0x1000 } };
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++)
        if (rm_mem_alloc(rm, b[i].m, (b[i].size + 0xfff) & ~(size_t)0xfff, 0x1000, true, true, true)) { fprintf(stderr, "dec: alloc failed\n"); return -1; }
    for (int i = 0; i < DEC_SURFACES; i++)
        if (rm_mem_alloc(rm, &d->luma[i], (luma + 0xfff) & ~(size_t)0xfff, 0x1000, true, true, true) ||
            rm_mem_alloc(rm, &d->chroma[i], (chroma + 0xfff) & ~(size_t)0xfff, 0x1000, true, true, true)) { fprintf(stderr, "dec: surface alloc failed\n"); return -1; }
    memset(d->coloc.cpu, 0, d->coloc.size); memset(d->hist.cpu, 0, d->hist.size); memset(d->mbhist.cpu, 0, d->mbhist.size);
    chan_mthd(&d->chan, 0, NVC56F_SET_OBJECT, 1, (NvU32)NVC9B0_VIDEO_DECODER);
    d->auN = 0; d->auSlices = 0;
    return 0;
}

void h264dec_close(H264Dec *d)
{
    chan_destroy(&d->chan);
    RmMem *m[] = { &d->setup, &d->bs, &d->offs, &d->status, &d->coloc, &d->hist, &d->mbhist, &d->sem };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) rm_mem_free(d->rm, m[i]);
    for (int i = 0; i < DEC_SURFACES; i++) { rm_mem_free(d->rm, &d->luma[i]); rm_mem_free(d->rm, &d->chroma[i]); }
    free(d->au); d->au = NULL;
}

void h264dec_release(H264Dec *d, int surf) { if (surf >= 0 && surf < DEC_SURFACES) d->busy[surf] = false; }

void h264dec_read_nv12(H264Dec *d, int surf, uint8_t *y, size_t ystride, uint8_t *uv, size_t uvstride, unsigned w, unsigned h)
{
    nvenc_in_untile_plane(y, ystride, d->luma[surf].cpu, w, h, d->pitch);
    nvenc_in_untile_plane(uv, uvstride, d->chroma[surf].cpu, w, h / 2, d->pitch);
}

void h264dec_read_i420(H264Dec *d, int surf, uint8_t *dst, unsigned w, unsigned h)
{
    const uint8_t *L = d->luma[surf].cpu, *C = d->chroma[surf].cpu;
    uint8_t *u = dst + (size_t)w * h, *v = u + (size_t)w * h / 4;
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x += 16) {
        unsigned n = w - x < 16 ? w - x : 16;
        memcpy(dst + (size_t)y * w + x, L + nvenc_in_offset(x, y, d->pitch), n);
    }
    for (unsigned y = 0; y < h / 2; y++) for (unsigned x = 0; x < w; x++) {
        uint8_t c = C[nvenc_in_offset(x, y, d->pitch)];
        if (x & 1) v[(size_t)y * (w / 2) + x / 2] = c; else u[(size_t)y * (w / 2) + x / 2] = c;
    }
}

// ---- decoded reference picture marking (8.2.5) ----
static int max_frame_num(const H264Sps *s) { return 1 << (s->log2_max_frame_num_minus4 + 4); }
static int pic_num(const H264Dec *d, const DecRef *r, int cur_fn, int maxfn) { (void)d; return r->frame_num > cur_fn ? r->frame_num - maxfn : r->frame_num; }
// A reference keeps its dpb[] slot for as long as it is a reference: the co-located data of a picture names its
// references by slot, so moving entries broke temporal direct prediction (T8.04). Empty slots have ref 0; ndpb is the
// number of slots in use up to the highest one.
static void trim(H264Dec *d) { while (d->ndpb > 0 && !d->dpb[d->ndpb - 1].ref) d->ndpb--; }

static void mark(H264Dec *d, const H264Sps *s, const H264Slice *sl, int surf, int top, int bot)
{
    if (!sl->nal_ref_idc) return;
    const int maxfn = max_frame_num(s), cur = sl->frame_num;
    DecRef me = { surf, sl->frame_num, 0, top, bot, 1 };
    if (sl->idr) {
        for (int i = 0; i < d->ndpb; i++) d->dpb[i].ref = 0;
        if (sl->long_term_reference_flag) { me.ref = 2; me.long_term_frame_idx = 0; d->max_long_term_frame_idx = 0; }
        else d->max_long_term_frame_idx = -1;
    } else if (sl->adaptive_ref_pic_marking_mode_flag) {
        for (int k = 0; k < sl->n_mmco; k++) {
            const H264Mmco *m = &sl->mmco[k];
            if (m->op == 1 || m->op == 3) {                     // picNumX = CurrPicNum - (difference_of_pic_nums_minus1 + 1)
                int x = cur - (m->difference_of_pic_nums_minus1 + 1);
                for (int i = 0; i < d->ndpb; i++) if (d->dpb[i].ref == 1 && pic_num(d, &d->dpb[i], cur, maxfn) == x) {
                    if (m->op == 1) d->dpb[i].ref = 0;
                    else {
                        for (int j = 0; j < d->ndpb; j++) if (j != i && d->dpb[j].ref == 2 && d->dpb[j].long_term_frame_idx == m->long_term_frame_idx) d->dpb[j].ref = 0;
                        d->dpb[i].ref = 2; d->dpb[i].long_term_frame_idx = m->long_term_frame_idx;
                    }
                    break;
                }
            } else if (m->op == 2) {
                for (int i = 0; i < d->ndpb; i++) if (d->dpb[i].ref == 2 && d->dpb[i].long_term_frame_idx == m->long_term_pic_num) d->dpb[i].ref = 0;
            } else if (m->op == 4) {
                d->max_long_term_frame_idx = m->max_long_term_frame_idx_plus1 - 1;
                for (int i = 0; i < d->ndpb; i++) if (d->dpb[i].ref == 2 && d->dpb[i].long_term_frame_idx > d->max_long_term_frame_idx) d->dpb[i].ref = 0;
            } else if (m->op == 5) {
                for (int i = 0; i < d->ndpb; i++) d->dpb[i].ref = 0;
                d->max_long_term_frame_idx = -1;
                me.frame_num = 0;                               // the current picture becomes frame_num 0, POC relative to itself
                int mn = top < bot ? top : bot; me.top_poc = top - mn; me.bot_poc = bot - mn;
            } else if (m->op == 6) {
                for (int i = 0; i < d->ndpb; i++) if (d->dpb[i].ref == 2 && d->dpb[i].long_term_frame_idx == m->long_term_frame_idx) d->dpb[i].ref = 0;
                me.ref = 2; me.long_term_frame_idx = m->long_term_frame_idx;
            }
        }
    } else {                                                    // sliding window (8.2.5.3)
        int ns = 0, nl = 0;
        for (int i = 0; i < d->ndpb; i++) { if (d->dpb[i].ref == 1) ns++; else if (d->dpb[i].ref == 2) nl++; }
        int maxref = s->max_num_ref_frames > 0 ? s->max_num_ref_frames : 1;
        if (ns + nl >= maxref && ns > 0) {
            int best = -1, bw = 0;
            for (int i = 0; i < d->ndpb; i++) if (d->dpb[i].ref == 1) {
                int w = pic_num(d, &d->dpb[i], cur, maxfn);
                if (best < 0 || w < bw) { best = i; bw = w; }
            }
            d->dpb[best].ref = 0;
        }
    }
    int slot = 0; while (slot < d->ndpb && d->dpb[slot].ref) slot++;
    if (slot < 16) { d->dpb[slot] = me; if (slot == d->ndpb) d->ndpb++; }
    trim(d);
}

// ---- one access unit ----
static int decode_pending(H264Dec *d)
{
    if (!d->auSlices) return 0;
    const H264Slice *sl = &d->auFirst;
    const H264Pps *pps = &d->params.pps[sl->pps_id]; const H264Sps *sps = &d->params.sps[pps->sps_id];
    const unsigned wmb = (unsigned)sps->pic_width_in_mbs_minus1 + 1, hmb = (unsigned)(sps->pic_height_in_map_units_minus1 + 1) * (2u - (unsigned)sps->frame_mbs_only_flag);
    if (wmb * 16 > d->maxW || hmb * 16 > d->maxH) { fprintf(stderr, "dec: %ux%u is larger than the session (%ux%u)\n", wmb * 16, hmb * 16, d->maxW, d->maxH); return -1; }
    if (sl->field_pic_flag) { fprintf(stderr, "dec: field pictures are not supported yet\n"); return -1; }
    int top, bot; h264_poc(&d->poc, sps, sl, &top, &bot);
    // a free surface: not held by the caller, not a reference
    int surf = -1;
    for (int i = 0; i < DEC_SURFACES && surf < 0; i++) {
        bool used = d->busy[i];
        for (int k = 0; k < d->ndpb; k++) if (d->dpb[k].ref && d->dpb[k].surf == i) used = true;
        if (!used) surf = i;
    }
    if (surf < 0) { fprintf(stderr, "dec: no free surface (the caller holds too many)\n"); return -1; }

    // end of picture: eos[] + explicitEOSPresentFlag (T8.03)
    static const uint8_t eos[16] = { 0, 0, 1, 0x0b, 0, 0, 1, 0x0b };
    memcpy(d->bs.cpu, d->au, d->auN);
    memcpy(d->offs.cpu, d->auOff, sizeof(NvU32) * (size_t)d->auSlices);
    nvdec_h264_pic_s *ps = d->setup.cpu; memset(ps, 0, sizeof *ps);
    memcpy(ps->eos, eos, 16); ps->explicitEOSPresentFlag = 1;
    ps->stream_len = (unsigned)d->auN; ps->slice_count = (unsigned)d->auSlices;
    ps->mbhist_buffer_size = (unsigned)d->mbhist.size;
    ps->log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
    ps->delta_pic_order_always_zero_flag = sps->delta_pic_order_always_zero_flag;
    ps->frame_mbs_only_flag = sps->frame_mbs_only_flag;
    ps->PicWidthInMbs = (int)wmb; ps->FrameHeightInMbs = (int)hmb;
    ps->tileFormat = 1; ps->gob_height = 0;
    ps->entropy_coding_mode_flag = pps->entropy_coding_mode_flag;
    ps->pic_order_present_flag = pps->bottom_field_pic_order_in_frame_present_flag;
    ps->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
    ps->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;
    ps->deblocking_filter_control_present_flag = pps->deblocking_filter_control_present_flag;
    ps->redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present_flag;
    ps->transform_8x8_mode_flag = pps->transform_8x8_mode_flag;
    ps->pitch_luma = ps->pitch_chroma = (unsigned)d->pitch;
    ps->HistBufferSize = (unsigned)(d->hist.size >> 8);
    ps->MbaffFrameFlag = sps->mb_adaptive_frame_field_flag;
    ps->direct_8x8_inference_flag = sps->direct_8x8_inference_flag;
    ps->weighted_pred_flag = pps->weighted_pred_flag; ps->weighted_bipred_idc = pps->weighted_bipred_idc;
    ps->constrained_intra_pred_flag = pps->constrained_intra_pred_flag;
    ps->ref_pic_flag = sl->nal_ref_idc != 0;
    ps->log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
    ps->chroma_format_idc = sps->chroma_format_idc;
    ps->pic_order_cnt_type = sps->pic_order_cnt_type;
    ps->pic_init_qp_minus26 = pps->pic_init_qp_minus26;
    ps->chroma_qp_index_offset = pps->chroma_qp_index_offset;
    ps->second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;
    ps->CurrPicIdx = (unsigned)surf; ps->CurrColIdx = (unsigned)surf;
    ps->frame_num = (unsigned)sl->frame_num; ps->frame_surfaces = 1;
    ps->CurrFieldOrderCnt[0] = top; ps->CurrFieldOrderCnt[1] = bot;
    int nref = 0;
    for (int i = 0; i < d->ndpb; i++) {                        // each reference in its own stable slot; empty slots stay zero
        const DecRef *r = &d->dpb[i]; nvdec_dpb_entry_s *e = &ps->dpb[i];
        if (!r->ref) continue;
        nref++;
        e->index = (unsigned)r->surf; e->col_idx = (unsigned)r->surf; e->state = 3;
        e->is_long_term = r->ref == 2;
        e->top_field_marking = e->bottom_field_marking = (unsigned)r->ref;
        e->FieldOrderCnt[0] = (unsigned)r->top_poc; e->FieldOrderCnt[1] = (unsigned)r->bot_poc;
        e->FrameIdx = r->ref == 2 ? r->long_term_frame_idx : r->frame_num;
    }
    for (int i = 0; i < 6; i++) for (int k = 0; k < 16; k++) ((uint8_t *)ps->WeightScale[i])[h264_zigzag4x4[k]] = pps->scaling4x4[i][k];
    for (int i = 0; i < 2; i++) for (int k = 0; k < 64; k++) ((uint8_t *)ps->WeightScale8x8[i])[h264_zigzag8x8[k]] = pps->scaling8x8[i][k];
    ps->qpprime_y_zero_transform_bypass_flag = sps->qpprime_y_zero_transform_bypass_flag;

    Chan *c = &d->chan;
    chan_rewind(c);
    chan_mthd(c, 0, NVC9B0_SET_APPLICATION_ID, 1, (NvU32)NVC9B0_SET_APPLICATION_ID_ID_H264);
    chan_mthd(c, 0, NVC9B0_SET_CONTROL_PARAMS, 1, DRF_NUM(C9B0, _SET_CONTROL_PARAMS, _CODEC_TYPE, NVDEC_CODEC_H264) |
              DRF_NUM(C9B0, _SET_CONTROL_PARAMS, _GPTIMER_ON, 1));
    chan_mthd(c, 0, NVC9B0_SET_DRV_PIC_SETUP_OFFSET, 1, hi(d->setup.gpu));
    chan_mthd(c, 0, NVC9B0_SET_IN_BUF_BASE_OFFSET, 1, hi(d->bs.gpu));
    chan_mthd(c, 0, NVC9B0_SET_PICTURE_INDEX, 1, (NvU32)surf);
    chan_mthd(c, 0, NVC9B0_SET_SLICE_OFFSETS_BUF_OFFSET, 1, hi(d->offs.gpu));
    chan_mthd(c, 0, NVC9B0_SET_COLOC_DATA_OFFSET, 1, hi(d->coloc.gpu));
    chan_mthd(c, 0, NVC9B0_SET_HISTORY_OFFSET, 1, hi(d->hist.gpu));
    chan_mthd(c, 0, NVC9B0_SET_NVDEC_STATUS_OFFSET, 1, hi(d->status.gpu));
    chan_mthd(c, 0, NVC9B0_H264_SET_MBHIST_BUF_OFFSET, 1, hi(d->mbhist.gpu));
    for (int i = 0; i < DEC_SURFACES; i++) {
        chan_mthd(c, 0, NVC9B0_SET_PICTURE_LUMA_OFFSET0 + 4 * (NvU32)i, 1, hi(d->luma[i].gpu));
        chan_mthd(c, 0, NVC9B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * (NvU32)i, 1, hi(d->chroma[i].gpu));
    }
    chan_mthd(c, 0, NVC9B0_EXECUTE, 1, 0u);
    const NvU32 val = ++d->semval; volatile NvU32 *sv = d->sem.cpu;
    chan_mthd(c, 0, NVC9B0_SEMAPHORE_A, 3, (NvU32)(d->sem.gpu >> 32), (NvU32)d->sem.gpu, val);
    chan_mthd(c, 0, NVC9B0_SEMAPHORE_D, 1, DRF_DEF(C9B0, _SEMAPHORE_D, _STRUCTURE_SIZE, _ONE) |
              DRF_DEF(C9B0, _SEMAPHORE_D, _OPERATION, _RELEASE) | DRF_DEF(C9B0, _SEMAPHORE_D, _PAYLOAD_SIZE, _32BIT));
    const double t0 = now_ms();
    chan_submit(c);
    while (sv[0] != val && now_ms() - t0 < 1000) { }
    d->engine_ms = now_ms() - t0;
    if (sv[0] != val) { fprintf(stderr, "dec: no completion (error notifier 0x%x)\n", chan_error(c)); return -1; }
    const nvdec_status_s *st = d->status.cpu;
    d->mbs_ok = st->mbs_correctly_decoded; d->mbs_err = st->mbs_in_error; d->err_status = st->error_status;
    if (d->verbose)
        printf("  dec: surf %2d %c frame_num %3d POC %4d refs %d -> %u MBs ok, %u err, status 0x%x, %.2f ms\n", surf, "PBIps"[sl->slice_type],
               sl->frame_num, top, nref, d->mbs_ok, d->mbs_err, d->err_status, d->engine_ms);
    mark(d, sps, sl, surf, top, bot);
    d->busy[surf] = true; d->cur_surf = surf; d->cur_top = top; d->cur_bot = bot; d->cur_w = sps->width; d->cur_h = sps->height;
    d->auN = 0; d->auSlices = 0;
    return 1;
}

int h264dec_flush(H264Dec *d) { return decode_pending(d); }

int h264dec_nal(H264Dec *d, const uint8_t *nal, size_t len)
{
    if (!len) return 0;
    const int t = nal[0] & 31;
    if (t == H264_NAL_SLICE || t == H264_NAL_IDR) {
        H264Slice s;
        if (h264_parse_slice_header(&d->params, nal, len, &s)) { fprintf(stderr, "dec: slice header parse failed\n"); return -1; }
        int r = 0;
        if (d->auSlices && s.first_mb_in_slice == 0) r = decode_pending(d);   // a new picture starts
        if (r < 0) return r;
        if (!d->auSlices) d->auFirst = s;
        if (d->auSlices >= 1024 || d->auN + len + 3 > d->auCap) { fprintf(stderr, "dec: access unit too large\n"); return -1; }
        d->auOff[d->auSlices++] = (NvU32)d->auN;
        d->au[d->auN++] = 0; d->au[d->auN++] = 0; d->au[d->auN++] = 1; memcpy(d->au + d->auN, nal, len); d->auN += len;
        return r;
    }
    // any other NAL unit before a picture's slices ends the previous picture
    int r = (t == H264_NAL_SPS || t == H264_NAL_PPS || t == H264_NAL_AUD || t == H264_NAL_SEI || t == H264_NAL_EOSEQ) ? decode_pending(d) : 0;
    if (t == H264_NAL_SPS) h264_parse_sps(&d->params, nal, len);
    else if (t == H264_NAL_PPS) h264_parse_pps(&d->params, nal, len);
    return r;
}
