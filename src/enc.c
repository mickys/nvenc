// enc.c: see enc.h.
#include "enc.h"
#include "blocklinear.h"
#include "h264hdr.h"
#include "hevchdr.h"
#include "av1hdr.h"
#include "av1probs.h"
#include "av1enctmpl.h"
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
enum { OFF_ME = 0x400, OFF_MD = 0x500, OFF_Q = 0x600, OFF_SLICE = 0x700, OFF_AV1_TILE = 0x800, SETUP_SIZE = 0x1000 };   // room for ENC_MAX_SLICES controls
enum { LOG2_MAX_FRAME_NUM = 8 };

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

// AV1: the RC data buffer's activity and pass-2 areas (the driver's offsets: 0x100 + n, 0x100 + 2n; n = 64 bytes per MB, in pages)
static size_t av1_rc_area(const Enc *e) { return (((size_t)e->wmb * e->hmb * 64) + 0xfff) & ~(size_t)0xfff; }

int enc_open(Enc *e, const EncConfig *cfg)
{
    memset(e, 0, sizeof *e);
    e->cfg = *cfg;
    if (e->cfg.num_refs < 1) e->cfg.num_refs = 1;
    if (e->cfg.num_refs > ENC_MAX_REFS) e->cfg.num_refs = ENC_MAX_REFS;
    // H.264 4:4:4: the engine's CABAC coder only knows 4:2:0 (broken IDRs, then an engine exception, RM error 0x2f on P
    // frames); CAVLC 4:4:4 is correct (M13 T13.02)
    if (cfg->codec == ENC_H264 && (cfg->chroma444 || cfg->lossless)) e->cfg.cabac = false;
    if (cfg->lossless) { e->cfg.qp_i = e->cfg.qp_p = 0; e->cfg.rc_mode = 0; }   // lossless: constant QP 0
    int W = cfg->width, H = cfg->height;
    e->wmb = (W + 15) / 16; e->hmb = (H + 15) / 16;
    Rm *rm = &e->rm; rm->verbose = cfg->verbose;
    if (rm_open(rm)) return -1;
    rm->verbose = cfg->verbose;
    if (rm_va_init(rm)) return -1;

    // input: NVENC's input layout (block_height 2); the engine reads 8-12 KB past the chroma plane, hence the slack.
    // HEVC: planes cover whole 32-row CTBs (the engine pads the source to the CTB grid itself, but reads that far)
    bool hevc = cfg->codec == ENC_HEVC, av1 = cfg->codec == ENC_AV1;
    // 10-bit (HEVC Main 10): input and reconstructed samples are 16 bits; the surface configs keep the pitch in samples,
    // the planes and field offsets double (nullmoth's libnvenc_hevc, docs/reports/T6.14.md)
    if ((!hevc && !av1) || e->cfg.bit_depth != 10) e->cfg.bit_depth = 8;
    e->bps = e->cfg.bit_depth > 8 ? 2 : 1;
    // AV1: whole 64x64 superblocks
    size_t inRows = av1 ? (((size_t)H + 63) & ~(size_t)63) : hevc ? (((size_t)e->hmb * 16 + 31) & ~(size_t)31) : (size_t)e->hmb * 16;
    e->pitch = ((size_t)e->wmb * 16 + 63) & ~(size_t)63;
    e->lumaSz = (nvenc_in_plane_size(e->pitch * e->bps, inRows) + 255) & ~(size_t)255;
    e->chromaSz = nvenc_in_plane_size(e->pitch * e->bps, inRows / 2);
    if (cfg->chroma444 && !av1) e->chromaSz = 2 * e->lumaSz;   // 4:4:4: two full-size planes (or one interleaved plane twice as wide)
    // reconstructed pictures: block-linear block_height 2, rows rounded up to 32 (the engine writes that far)
    e->rPitch = ((size_t)e->wmb * 16 + 255) & ~(size_t)255;
    const size_t rRows = av1 ? (((size_t)H + 63) & ~(size_t)63) : (((size_t)e->hmb * 16 + 31) & ~(size_t)31);
    e->rLuma = (e->rPitch * e->bps * rRows + 255) & ~(size_t)255;
    e->rSlot = (e->rLuma + e->rPitch * e->bps * (av1 ? rRows / 2 : (((size_t)e->hmb * 8 + 31) & ~(size_t)31)) + 0xffff) & ~(size_t)0xffff;
    if (cfg->chroma444 && !av1) e->rSlot = (3 * e->rLuma + 0xffff) & ~(size_t)0xffff;   // 4:4:4: chroma twice the luma plane
    if (hevc || av1) e->rSlot += 0x10000;   // nullmoth's HEVC slots are 64 KB larger than the planes (T6.03); what goes there is unknown
    int slots = e->cfg.num_refs + 1;
    size_t mbs = (size_t)e->wmb * e->hmb;
    size_t histSz = ((((size_t)(W - 1) + 16) / 16 + 16) * H264_HIST_BLOCK_SIZE + 0xffff) & ~(size_t)0xffff;
    if (hevc) histSz = ((size_t)(W + 31) / 32 * 768 + 0xffff) & ~(size_t)0xffff;   // HEVC: 768 bytes per CTB column (nullmoth's size, T6.06)
    e->av1SbCols = (W + 63) / 64; e->av1SbRows = (H + 63) / 64;
    if (av1)   // nvenc_drv.h: AV1_HIST_PIXEL_FRAME_SIZE + AV1_HIST_MV_SB_SIZE per superblock (generous: per superblock of the frame)
        histSz = ((size_t)AV1_HIST_PIXEL_FRAME_SIZE + (size_t)e->av1SbCols * e->av1SbRows * AV1_HIST_MV_SB_SIZE + (1 << 20) + 0xffff) & ~(size_t)0xffff;
    size_t meSz = (mbs * 64 + 0xffff) & ~(size_t)0xffff;   // ME predictor data per MB (the reference client uses 4 MB at any size)
    if (e->cfg.depth < 1) e->cfg.depth = 1;
    if (e->cfg.depth > ENC_MAX_DEPTH) e->cfg.depth = ENC_MAX_DEPTH;
    for (int d = 0; d < e->cfg.depth; d++)
        if (rm_mem_alloc(rm, &e->in[d], e->lumaSz + e->chromaSz + (64 << 10), 0x1000, true, true, true) ||
            rm_mem_alloc(rm, &e->setup[d], SETUP_SIZE, 0x1000, true, true, true) ||
            rm_mem_alloc(rm, &e->bs[d], (mbs * 384 + (1 << 20) + 0xffff) & ~(size_t)0xffff, 0x1000, true, true, true) ||
            rm_mem_alloc(rm, &e->status[d], av1 ? 0x40000 : 0x10000, 0x1000, true, true, true)) { fprintf(stderr, "enc: allocation failed\n"); return -1; }
        else memset(e->in[d].cpu, 0, e->in[d].size);
    if (rm_mem_alloc(rm, &e->recon, e->rSlot * (size_t)slots, 0x10000, false, false, true) ||
        rm_mem_alloc(rm, &e->hist, histSz, 0x1000, false, false, true) ||
        rm_mem_alloc(rm, &e->coloc, (mbs * 64 + 0xffff) & ~(size_t)0xffff, 0x1000, false, false, true) ||
        rm_mem_alloc(rm, &e->mepred[0], meSz, 0x1000, false, false, true) ||
        rm_mem_alloc(rm, &e->mepred[1], meSz, 0x1000, false, false, true) ||
        rm_mem_alloc(rm, &e->rcproc, 0x10000, 0x1000, true, true, true) ||
        rm_mem_alloc(rm, &e->rcdata, av1 ? 0x100 + 3 * av1_rc_area(e) + 0x1000 : 0x10000, 0x1000, true, true, true) ||
        rm_mem_alloc(rm, &e->sem, 0x1000, 0x1000, true, true, true)) { fprintf(stderr, "enc: allocation failed\n"); return -1; }
    memset(e->rcproc.cpu, 0, e->rcproc.size); memset(e->rcdata.cpu, 0, e->rcdata.size);
    if (av1) {
        // as NVIDIA's Linux driver sets them up (docs/traces/av1-linux, M12): the four default CDF tables (av1probs.c, the
        // same layout NVDEC uses) 0x3000 apart, the microcode picking one by quantizer (prob_tables_offset); two buffers for
        // the tables the engine writes (a P frame reads the previous frame's); the entropy / filter line buffers; the RC
        // process buffer with the words the driver puts there before the first frame
        if (rm_mem_alloc(rm, &e->av1ProbIn, 4 * 0x3000, 0x1000, true, true, true) || rm_mem_alloc(rm, &e->av1ProbOut, 0x10000, 0x1000, true, true, true) ||
            rm_mem_alloc(rm, &e->av1ProbOut2, 0x10000, 0x1000, true, true, true)) { fprintf(stderr, "enc: allocation failed\n"); return -1; }
        memset(e->av1ProbIn.cpu, 0, e->av1ProbIn.size); memset(e->av1ProbOut.cpu, 0, e->av1ProbOut.size); memset(e->av1ProbOut2.cpu, 0, e->av1ProbOut2.size);
        for (int q = 0; q < 4; q++) av1_default_probs(q, (uint8_t *)e->av1ProbIn.cpu + q * 0x3000);
        for (int i = 0; i < 6; i++) {
            if (rm_mem_alloc(rm, &e->av1Line[i], 1 << 20, 0x1000, true, true, true)) { fprintf(stderr, "enc: allocation failed\n"); return -1; }
            memset(e->av1Line[i].cpu, 0, e->av1Line[i].size);
        }
        memcpy(e->rcproc.cpu, av1_tmpl_rcprocess, sizeof av1_tmpl_rcprocess);
        e->av1TuCap = e->bs[0].size + 4096;
        for (int d = 0; d < e->cfg.depth; d++) if (!(e->av1Tu[d] = malloc(e->av1TuCap))) return -1;
    }
    ((volatile NvU32 *)e->sem.cpu)[0] = 0;
    if (!e->cfg.fps) e->cfg.fps = 60;
    if (!e->cfg.vbv_bits) e->cfg.vbv_bits = e->cfg.bitrate;
    if (!e->cfg.max_bitrate) e->cfg.max_bitrate = e->cfg.bitrate;

    NV_NVENC_ALLOCATION_PARAMETERS ep = { .size = sizeof ep, .engineInstance = (NvU32)cfg->engine };
    if (chan_create(rm, &e->chan, cfg->engine ? NV2080_ENGINE_TYPE_NVENC1 : NV2080_ENGINE_TYPE_NVENC0, NVC9B7_VIDEO_ENCODER, &ep)) return -1;
    chan_mthd(&e->chan, 0, NVC56F_SET_OBJECT, 1, (NvU32)NVC9B7_VIDEO_ENCODER);
    // completion event (see enc_encode)
    if (!cfg->spin && rm_event_open(rm, &e->ev, cfg->engine ? NV2080_NOTIFIERS_NVENC1 : NV2080_NOTIFIERS_NVENC0)) {
        if (cfg->verbose) fprintf(stderr, "enc: no NVENC event - spinning\n");
        memset(&e->ev, 0, sizeof e->ev);
    }
    return 0;
}

void enc_close(Enc *e)
{
    if (e->ev.h) rm_event_close(&e->rm, &e->ev);
    chan_destroy(&e->chan);
    RmMem *m[] = { &e->recon, &e->hist, &e->coloc, &e->mepred[0], &e->mepred[1], &e->sem, &e->rcproc, &e->rcdata, &e->av1ProbIn, &e->av1ProbOut,
                   &e->av1ProbOut2, &e->av1Line[0], &e->av1Line[1], &e->av1Line[2], &e->av1Line[3], &e->av1Line[4], &e->av1Line[5] };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (m[i]->hPhys) rm_mem_free(&e->rm, m[i]);
    for (int d = 0; d < ENC_MAX_DEPTH; d++) { free(e->av1Tu[d]); e->av1Tu[d] = NULL; }
    for (int d = 0; d < ENC_MAX_DEPTH; d++) {
        RmMem *pm[] = { &e->in[d], &e->setup[d], &e->bs[d], &e->status[d] };
        for (int i = 0; i < 4; i++) if (pm[i]->hPhys) rm_mem_free(&e->rm, pm[i]);
    }
    rm_close(&e->rm);
}

static H264Params hdr_params(const Enc *e)
{
    return (H264Params){ .width = e->cfg.width, .height = e->cfg.height, .profile_idc = e->cfg.chroma444 || e->cfg.lossless ? 244 : e->cfg.h264_profile == 100 ? 100 : 77, .level_idc = 51, .chroma444 = e->cfg.chroma444, .lossless = e->cfg.lossless,
        .log2_max_frame_num_minus4 = LOG2_MAX_FRAME_NUM - 4, .poc_type = 2, .num_ref_frames = e->cfg.num_refs,
        .num_ref_idx_l0_minus1 = e->cfg.num_refs - 1, .entropy_cabac = e->cfg.cabac, .transform_8x8 = e->cfg.h264_profile == 100 || e->cfg.chroma444, .pic_init_qp = 26, .vui = 1, .full_range = e->cfg.full_range };
}

// ENC_HEVC_OFF=ts,cqd,sao,amp,sis: experiment switches (T6.05), applied to the headers and the engine setup alike
static int hevc_off(const char *k) { const char *v = getenv("ENC_HEVC_OFF"); return v && strstr(v, k) != NULL; }

static HevcParams hevc_params(const Enc *e)
{
    return (HevcParams){ .width = e->cfg.width, .height = e->cfg.height, .log2_max_poc_lsb_minus4 = 4,
        .max_dec_pic_buffering = e->cfg.num_refs + 1 + e->cfg.dpb_extra, .sps_rps_negative = e->cfg.num_refs,
        .num_ref_idx_l0_default = e->cfg.num_refs, .pic_init_qp = 26, .vui = 1, .full_range = e->cfg.full_range,
        .bit_depth = e->cfg.bit_depth, .chroma444 = e->cfg.chroma444, .lossless = e->cfg.lossless,
        .no_sdh = 1,   /* the engine does not apply sign data hiding: with it signalled, every residual decodes wrong (T6.05) */
        .no_ts = hevc_off("ts") || e->cfg.bit_depth > 8,   /* nullmoth's 10-bit setup turns transform skip off */ .no_cqd = hevc_off("cqd"), .no_sao = hevc_off("sao"), .no_amp = hevc_off("amp"),
        .sis = hevc_off("sis") };
}

size_t enc_headers(Enc *e, uint8_t *out)
{
    if (e->cfg.codec == ENC_AV1) return 0;   // the sequence header travels in each key frame's temporal unit
    if (e->cfg.codec == ENC_HEVC) {
        HevcParams hp = hevc_params(e);
        size_t n = hevc_write_vps(&hp, out);
        n += hevc_write_sps(&hp, out + n);
        return n + hevc_write_pps(&hp, out + n);
    }
    H264Params hp = hdr_params(e);
    size_t n = h264_write_sps(&hp, out);
    return n + h264_write_pps(&hp, out + n);
}

// 8-bit rows widened to 16-bit samples (value << 8) for a 10-bit session, then tiled like P010
static void load_widened(Enc *e, uint8_t *dst, const uint8_t *src, size_t stride, size_t width, size_t rows)
{
    uint16_t *buf = malloc(width * 2 * rows);
    if (!buf) return;
    for (size_t r = 0; r < rows; r++) for (size_t c = 0; c < width; c++) buf[r * width + c] = (uint16_t)(src[r * stride + c] << 8);
    // the plane's pitch: the luma pitch, or twice it for a row wider than that (4:4:4 interleaved chroma)
    nvenc_in_tile_plane(dst, (const uint8_t *)buf, width * 2, width * 2, rows, e->pitch * 2 * (width > e->pitch ? 2 : 1));
    free(buf);
}

// 16-bit samples as P010 has them (10 bits in the high bits): NVENC takes them as they are, in the 8-bit layout with
// twice the bytes per row (low-bit alignment gives garbage, T6.14)
static void load_16(Enc *e, uint8_t *dst, const uint16_t *src, size_t stride, size_t width, size_t rows)
{
    nvenc_in_tile_plane(dst, (const uint8_t *)src, stride, width * 2, rows, e->pitch * 2);
}

void enc_load_p010(Enc *e, const uint16_t *y, size_t yStride, const uint16_t *uv, size_t uvStride)
{
    RmMem *in = &e->in[e->seq % (NvU32)e->cfg.depth];
    load_16(e, in->cpu, y, yStride, (size_t)e->cfg.width, (size_t)e->cfg.height);
    load_16(e, (uint8_t *)in->cpu + e->lumaSz, uv, uvStride, (size_t)(e->cfg.width + 1) & ~(size_t)1, (size_t)(e->cfg.height + 1) / 2);
}

void enc_load_nv12(Enc *e, const uint8_t *y, size_t yStride, const uint8_t *uv, size_t uvStride)
{
    RmMem *in = &e->in[e->seq % (NvU32)e->cfg.depth];
    if (e->bps == 2) {
        load_widened(e, in->cpu, y, yStride, (size_t)e->cfg.width, (size_t)e->cfg.height);
        load_widened(e, (uint8_t *)in->cpu + e->lumaSz, uv, uvStride, (size_t)(e->cfg.width + 1) & ~(size_t)1, (size_t)(e->cfg.height + 1) / 2);
        return;
    }
    nvenc_in_tile_plane(in->cpu, y, yStride, (size_t)e->cfg.width, (size_t)e->cfg.height, e->pitch);
    nvenc_in_tile_plane((uint8_t *)in->cpu + e->lumaSz, uv, uvStride, (size_t)e->cfg.width, (size_t)(e->cfg.height + 1) / 2, e->pitch);
}

void enc_load_yuv444(Enc *e, const uint8_t *y, size_t ys, const uint8_t *u, size_t us, const uint8_t *v, size_t vs)
{
    RmMem *in = &e->in[e->seq % (NvU32)e->cfg.depth];
    const size_t W = (size_t)e->cfg.width, H = (size_t)e->cfg.height;
    uint8_t *buf = malloc(2 * W * H); if (!buf) return;           // Cb Cr interleaved, twice the pitch
    for (size_t r = 0; r < H; r++) for (size_t c = 0; c < W; c++) { buf[r * 2 * W + 2 * c] = u[r * us + c]; buf[r * 2 * W + 2 * c + 1] = v[r * vs + c]; }
    if (e->bps == 2) {   // Main 4:4:4 10 from 8-bit planes: widened like enc_load_nv12 does
        load_widened(e, in->cpu, y, ys, W, H); load_widened(e, (uint8_t *)in->cpu + e->lumaSz, buf, 2 * W, 2 * W, H);
        free(buf); return;
    }
    nvenc_in_tile_plane(in->cpu, y, ys, W, H, e->pitch);
    nvenc_in_tile_plane((uint8_t *)in->cpu + e->lumaSz, buf, 2 * W, 2 * W, H, 2 * e->pitch);
    free(buf);
}
void enc_load_nv24(Enc *e, const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs)
{
    RmMem *in = &e->in[e->seq % (NvU32)e->cfg.depth];
    nvenc_in_tile_plane(in->cpu, y, ys, (size_t)e->cfg.width, (size_t)e->cfg.height, e->pitch);
    nvenc_in_tile_plane((uint8_t *)in->cpu + e->lumaSz, uv, uvs, 2 * (size_t)e->cfg.width, (size_t)e->cfg.height, 2 * e->pitch);
}

// x444 / xf44: 16-bit samples (10 bits high) in two planes, the chroma plane Cb Cr interleaved: HEVC Main 4:4:4 10
void enc_load_x444(Enc *e, const uint16_t *y, size_t ys, const uint16_t *uv, size_t uvs)
{
    RmMem *in = &e->in[e->seq % (NvU32)e->cfg.depth];
    load_16(e, in->cpu, y, ys, (size_t)e->cfg.width, (size_t)e->cfg.height);
    nvenc_in_tile_plane((uint8_t *)in->cpu + e->lumaSz, (const uint8_t *)uv, uvs, 4 * (size_t)e->cfg.width, (size_t)e->cfg.height, 4 * e->pitch);
}

void enc_load_l10r(Enc *e, const uint8_t *src, size_t stride)
{
    RmMem *in = &e->in[e->seq % (NvU32)e->cfg.depth];
    nvenc_in_tile_l10r(in->cpu, (uint8_t *)in->cpu + e->lumaSz, src, stride, (size_t)e->cfg.width, (size_t)e->cfg.height, e->pitch * 2);
}

void enc_load_bgra(Enc *e, const uint8_t *bgra, size_t stride)
{
    RmMem *in = &e->in[e->seq % (NvU32)e->cfg.depth];
    nvenc_in_tile_bgra(in->cpu, (uint8_t *)in->cpu + e->lumaSz, bgra, stride, (size_t)e->cfg.width, (size_t)e->cfg.height, e->pitch);
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
static void setup_md(nvenc_h264_md_control_s *md, bool p, bool high)
{
    if (high) { md->intra_luma8x8_mode_enable = 0x1ff; md->priority_ipred_type_ip1 = 3; }   // High: all nine 8x8 intra modes (T7.02)
    md->intra_luma16x16_mode_enable = 0xf; md->intra_chroma_mode_enable = 0xf; md->inter_penalty_factor_for_ip1 = 16;
    md->l0_part_16x16_enable = 1; md->l0_part_16x8_enable = 1; md->l0_part_8x16_enable = 1; md->l0_part_8x8_enable = 1;
    md->pskip_enable = 1;
    md->bias_inter_16x8 = -1; md->bias_inter_8x16 = -1; md->bias_inter_8x8 = -1; md->bias_pskip = -1; md->bias_bdir = 2;
    md->bias_intra_over_inter = -2; md->bias_intra_16x16 = -4; md->bias_intra_8x8 = -2; md->bias_intra_4x4 = -1;
    md->intra_nxn_bias_multiplier = 32; md->intra_most_prob_bias_multiplier = 3; md->pskip_bias_multiplier = 2;
    md->pskip_esc_threshold = 9; md->bdirect_esc_threshold = 19;
    md->tempmv_wt_spread_threshold = 0xffff; md->tempmv_wt_distort_threshold = 0xffff;
    md->mv_cost_enable = 1; md->early_intra_mode_type_16x16dc = 1; md->early_intra_mode_type_16x16h = 1; md->early_intra_mode_type_16x16v = 1;
    md->ip_search_mode = 7; md->multiply_bias_with_lambda = 1; md->early_termination_ip1 = 1; if (!high) md->priority_ipred_type_ip1 = 1;
    md->intra_ssd_cnt_4x4 = 1; md->intra_ssd_cnt_8x8 = 1; md->intra_ssd_cnt_16x16 = 1; md->skip_evaluate_enable = 1;
    md->rdo_level = 1; md->tu_search_num = 3; md->num_1div8_lambda_ssd = 1;
    md->luma_residual_zero_eval = p ? 2 : 0;
}

// HEVC controls: the values nullmoth's libnvenc_hevc uses (docs/traces/vttest-hevc-256x128-controls.txt, T6.03/T6.05);
// IDR and P pictures use the same ME, MD and quant controls.
static void setup_me_hevc(nvenc_h264_me_control_s *me)
{
    me->refinement_mode = 1; me->refine_on_search_enable = 1; me->fps_mvcost = 1; me->sps_mvcost = 1; me->sps_cost_func = 1;
    nvenc_h264_me_hint_cfg_s *h = &me->predsrc;
    h->self_temporal_stamp_l0 = 1; h->self_temporal_stamp_l1 = 1; h->self_temporal_search = 1; h->self_temporal_enable = 1;
    h->self_spatial_search = 1; h->self_spatial_enable = 1;
    h->const_mv_stamp_l0 = 2; h->const_mv_stamp_l1 = 2; h->const_mv_search = 1; h->const_mv_enable = 1;
    h->stamp_refidx1_stamp = 3; h->stamp_refidx2_stamp = 3; h->stamp_refidx3_stamp = 3;
    nvenc_h264_me_stamp_s *sh = &me->shape0;
    sh[0].bitmask[0] = 0xff0c0c00; sh[0].bitmask[1] = 0xc0c; sh[0].hor_adjust = 1;
    sh[1].bitmask[0] = 0x1e1e0c00; sh[1].bitmask[1] = 0xc; sh[1].hor_adjust = 1; sh[1].ver_adjust = 1;
    sh[2].bitmask[0] = 0xff0c0c00; sh[2].bitmask[1] = 0xc0c; sh[2].hor_adjust = 1;
    sh[3].bitmask[0] = 0x1e0c0000; sh[3].bitmask[1] = 0xc; sh[3].hor_adjust = 1;
    me->partDecisionMadeByFPP = 1; me->penalty_factor_FPP = 15; me->spatial_hint_pattern = 6; me->temporal_hint_pattern = 4;
    me->Cu16partDecisionMadeByFPP = 1; me->fbm_op_winner_num_p_frame = 1; me->fbm_op_winner_num_b_frame_l0 = 1;
    me->fbm_op_winner_num_b_frame_l1 = 1; me->fbm_select_best_cu32_parttype_num = 3; me->sps_evaluate_merge_cand_num = 3;
    me->fps_quad_thresh_hold = 50; me->external_hint_order = 1; me->coloc_hint_order = 1;
    me->hint_type0 = 0; me->hint_type1 = 1; me->hint_type2 = 2; me->hint_type3 = 3; me->hint_type4 = 4;
}

static void setup_md_hevc(nvenc_h265_md_control_s *md)
{
    md->h265_intra_luma4x4_mode_enable = md->h265_intra_luma8x8_mode_enable = 0x55555557;
    md->h265_intra_luma16x16_mode_enable = md->h265_intra_luma32x32_mode_enable = 0x55555557;
    md->h265_intra_luma_mode_enable_leftbits = 0xb6d;
    md->cu16x16_part_2Nx2N_enable = 1; md->cu16x16_part_2NxN_enable = 1; md->cu16x16_part_Nx2N_enable = 1; md->cu16x16_part_NxN_enable = 1;
    md->cu32x32_part_2Nx2N_enable = 1; md->cu32x32_part_2NxN_enable = 1; md->cu32x32_part_Nx2N_enable = 1;
    md->cu32x32_part_2NxnU_enable = 1; md->cu32x32_part_2NxnD_enable = 1; md->cu32x32_part_nLx2N_enable = 1; md->cu32x32_part_nRx2N_enable = 1;
    md->intra_mode_4x4_enable = 1; md->intra_mode_8x8_enable = 1; md->intra_mode_16x16_enable = 1; md->intra_mode_32x32_enable = 1;
    md->h265_intra_chroma_mode_enable = 0x1f;
    md->tusize_4x4_enable = 1; md->tusize_8x8_enable = 1; md->tusize_16x16_enable = 1; md->tusize_32x32_enable = 1;
    md->pskip_enable = 1; md->mv_cost_enable = 1; md->ip_search_mode = 7; md->tu_search_num_cu16 = 2; md->tu_search_num_cu32 = 1;
    md->intra_ssd_cnt_4x4 = 1; md->intra_ssd_cnt_8x8 = 1; md->intra_ssd_cnt_16x16 = 1; md->intra_ssd_cnt_32x32 = 1;
}

static void setup_q_hevc(nvenc_h265_quant_control_s *q)
{
    q->qpp_run_vector_4x4 = 0x56b; q->qpp_run_vector_8x8[0] = 0xaaff; q->qpp_run_vector_8x8[1] = 0xffaa; q->qpp_run_vector_8x8[2] = 0xf;
    q->qpp_luma8x8_cost = 15; q->qpp_luma16x16_cost = 15; q->qpp_chroma_cost = 15;
}

// the H.265 picture setup (nvenc_h265_drv_pic_setup_s, 1024 bytes) and its controls
static void setup_hevc(Enc *e, RmMem *setup, RmMem *bs, bool idr, int nRefs)
{
    int W = e->cfg.width, H = e->cfg.height;
    nvenc_h265_drv_pic_setup_s *ps = setup->cpu;
    ps->magic = NV_NVENC_DRV_MAGIC_VALUE;
    nvenc_h265_surface_cfg_s ic = { .frame_width_minus1 = (unsigned short)(W - 1), .frame_height_minus1 = (unsigned short)(H - 1),
        .sfc_pitch = (unsigned short)e->pitch, .sfc_pitch_chroma = (unsigned short)e->pitch, .block_height = 2 };
    // reference/output pictures: the coded (CTB-aligned) size; the input keeps the display size and the engine pads it
    HevcParams hp = hevc_params(e);
    int cw = (int)hevc_coded_width(&hp), ch = (int)hevc_coded_height(&hp);
    nvenc_h265_surface_cfg_s rc = ic; rc.sfc_pitch = rc.sfc_pitch_chroma = (unsigned short)e->rPitch;
    rc.frame_width_minus1 = (unsigned short)(cw - 1); rc.frame_height_minus1 = (unsigned short)(ch - 1);
    // nullmoth's sets the field offsets of the reference/output surfaces to half a plane (unused in frame mode)
    rc.luma_bot_offset = (unsigned)((size_t)ch / 2 * e->rPitch * e->bps / 256);
    rc.chroma_bot_offset = (unsigned)((((size_t)ch / 2 + 31) & ~(size_t)31) / 2 * e->rPitch * e->bps / 256);
    // 4:4:4 (T13.01): chroma Cb / Cr interleaved at full width (like NV24), input and reconstruction alike, the chroma
    // pitch twice the luma's. (Planar input via memory_mode 1 and the CHROMA_U / _V methods, or a planar reconstruction,
    // gave 8-18 dB chroma.)
    if (e->cfg.chroma444) { ic.sfc_pitch_chroma = (unsigned short)(2 * e->pitch); rc.sfc_pitch_chroma = (unsigned short)(2 * e->rPitch); }
    ps->input_cfg = ic; ps->refpic_cfg = rc; ps->outputpic_cfg = rc;
    nvenc_h265_sps_data_s *sp = &ps->sps_data;
    sp->chroma_format_idc = e->cfg.chroma444 ? 3 : 1; sp->log2_max_pic_order_cnt_lsb_minus4 = 4; sp->min_cu_size = 1; sp->max_cu_size = 2;
    sp->min_tu_size = 0; sp->max_tu_size = 3; sp->max_transform_depth_inter = 3; sp->max_transform_depth_intra = 3;
    sp->amp_enabled_flag = !hp.no_amp; sp->num_short_term_ref_pic_sets = 1; sp->sample_adaptive_offset_enabled_flag = !hp.no_sao;
    sp->strong_intra_smoothing_enabled_flag = hp.sis ? 1 : 0;
    sp->bit_depth_minus_8 = (unsigned)(e->cfg.bit_depth - 8);
    nvenc_h265_pps_data_s *pp = &ps->pps_data;   // matches hevc_write_pps()
    pp->num_ref_idx_l0_active_minus1 = (unsigned)(e->cfg.num_refs - 1); pp->transform_skip_enabled_flag = !hp.no_ts;
    pp->transquant_bypass_enabled_flag = e->cfg.lossless;
    pp->pps_loop_filter_across_slices_enabled_flag = 1; pp->cu_qp_delta_enabled_flag = !hp.no_cqd; pp->cabac_init_present_flag = 1;
    pp->sign_data_hiding_enabled_flag = !hp.no_sdh;
    if (e->cfg.bit_depth > 8) {
        // nullmoth's 10-bit setup also sets an undocumented bit (pps_data reserved1, byte 7 bit 3) and the reserved word at
        // 0x314 to 0x0222 (0x0999 in 8-bit, which we leave 0); meanings unknown (T6.14)
        ((NvU8 *)pp)[7] |= 0x08;
        ((NvU8 *)ps)[0x314] = 0x22; ((NvU8 *)ps)[0x315] = 0x02;
    }
    nvenc_h265_rc_s *r = &ps->rate_control;
    r->QP[0] = (unsigned char)e->cfg.qp_p; r->QP[1] = (unsigned char)e->cfg.qp_p; r->QP[2] = (unsigned char)e->cfg.qp_i;
    // 10-bit extends the QP range down by 6 * (bit_depth - 8): nullmoth's sets minQP -12
    for (int i = 0; i < 3; i++) { r->minQP[i] = (unsigned char)(e->cfg.bit_depth > 8 ? -12 : 0); r->maxQP[i] = 51; r->rhopbi[i] = 256; }
    r->framerate = e->cfg.fps; r->gop_length = e->cfg.idr_period > 0 ? (unsigned)e->cfg.idr_period : 0xffffffffu;
    if (e->cfg.rc_mode) {   // as the H.264 path (M3, T3.06-T3.08): NAL HRD, fixed-point rate per pixel and frame
        double pixels = (double)W * H, perFrame = (double)e->cfg.bitrate / e->cfg.fps;
        r->hrd_type = 2; r->maxQPD = 6; r->baseQPD = 3;
        r->framerate = e->cfg.fps << 8;
        r->buffersize = (unsigned)e->cfg.vbv_bits; r->nal_cpb_size = r->vcl_cpb_size = e->cfg.vbv_bits;
        r->nal_bitrate = r->vcl_bitrate = e->cfg.max_bitrate;
        r->Np = e->cfg.idr_period > 0 ? (e->cfg.idr_period - 1) << 4 : 0x7fffffff;
        r->Ravg = r->R = (int)(perFrame / pixels * 256);
        r->Bmin = 0; r->i_size_ratio_x = 1; r->i_size_ratio_y = 1;   // single_frame_vbv stays 0 (T3.08)
    }

    int ctbW = (W + 31) / 32, ctbH = (H + 31) / 32, ctbs = ctbW * ctbH, lg = 0;
    while ((1 << lg) < ctbs) lg++;
    nvenc_h265_pic_control_s *pc = &ps->pic_control;
    memset(pc->l0, -2, sizeof pc->l0); memset(pc->l1, -2, sizeof pc->l1);
    memset(pc->poc_entry_l0, -1, sizeof pc->poc_entry_l0); memset(pc->poc_entry_l1, -1, sizeof pc->poc_entry_l1);
    pc->pic_output_flag = 1;
    pc->slice_control_offset = OFF_SLICE; pc->me_control_offset = OFF_ME; pc->md_control_offset = OFF_MD; pc->q_control_offset = OFF_Q;
    pc->hist_buf_size = (unsigned)e->hist.size; pc->bitstream_buf_size = (unsigned)bs->size;
    pc->pic_type = idr ? 3 : 0; pc->ref_pic_flag = 1; pc->slice_mode = 0;
    pc->codec = 5; pc->ceil_log2_pic_size_in_ctbs = (unsigned)lg; pc->sao_luma_mode = hp.no_sao ? 0 : 31; pc->sao_chroma_mode = hp.no_sao ? 0 : 31;
    pc->nal_unit_type = idr ? 19 : 1; pc->nuh_temporal_id_plus1 = 1; pc->e4byteStartCode = 1;
    pc->qpfifo = 1; pc->enable_source_image_padding = 1; pc->strips_in_frame = 1; pc->chroma_skip_threshold_4x4 = 8;
    pc->pic_order_cnt_lsb = (unsigned short)(e->sinceIdr & 0xff);
    if (!idr) {   // inline short-term RPS: the references, most recent first, one POC apart (T6.07, T6.11)
        nvenc_h265_strps_s *rps = &pc->short_term_rps;
        rps->num_negative_pics = (short)nRefs;
        memset(pc->temp_dist_l0, -1, sizeof pc->temp_dist_l0);                    // unused entries -1, as nullmoth's
        for (int r = 1; r < 8; r++) for (int c2 = 0; c2 < 8; c2++) pc->dist_scale_factor[r][c2] = -1;   // unused rows -1
        for (int i = 0; i < nRefs; i++) {
            // l0: 2 x DPB slot; the picture itself goes to SET_IN_REF_PIC<i> (list position, poc_entry_l0[i] = i)
            pc->l0[i] = (char)(2 * e->slotOf[i]); pc->poc_entry_l0[i] = (char)i; pc->temp_dist_l0[i] = (char)(i + 1);
            rps->delta_poc_s0_minus1[i] = 0; rps->used_by_curr_pic_s0_flag |= (unsigned short)(1u << i);
        }
        // MV scaling between references (8.5.3.2.7): distScaleFactor = (tb * tx + 32) >> 6, tx = (16384 + td / 2) / td,
        // stored [td reference][tb reference]; 256 = 1.0
        for (int i = 0; i < nRefs; i++) for (int j = 0; j < nRefs; j++) {
            int tb = i + 1, td = j + 1, tx = (16384 + td / 2) / td, f = (tb * tx + 32) >> 6;
            pc->dist_scale_factor[j][i] = (short)(f < -4096 ? -4096 : f > 4095 ? 4095 : f);
        }
        // same_poc: an 8x8 bit matrix over pairs of references, bit i * 8 + j set when references i and j are the same
        // picture (the diagonal: 0x201 for two). With only bit 0 (nullmoth's value for one reference) the engine scaled MVs
        // between a reference and itself, and multi-reference streams decoded wrong (T6.11)
        for (int i = 0; i < nRefs; i++) pc->same_poc[(i * 9) / 32] |= 1u << ((i * 9) % 32);
        pc->diff_pic_order_cnt_zero[0] = pc->diff_pic_order_cnt_zero[1] = 0xfefefefeu;   // as nullmoth's
    }
    nvenc_h265_slice_control_s *sc = (void *)((NvU8 *)setup->cpu + OFF_SLICE);
    pc->num_forced_slices_minus1 = 0;
    // slice_type numbered like pic_type (0 P, 1 B, 2 I), not as the header's comment says (0 B, 1 P): with 1 the engine
    // faulted on the first P picture; nullmoth's library sends 0 (T6.07)
    sc->num_ctus = (unsigned)ctbs; sc->qp_avr = (unsigned)(idr ? e->cfg.qp_i : e->cfg.qp_p); sc->slice_type = idr ? 2 : 0;
    sc->slice_loop_filter_across_slices_enabled_flag = 1; sc->slice_sao_luma_flag = !hp.no_sao; sc->slice_sao_chroma_flag = !hp.no_sao;
    if (!idr) { sc->num_ref_idx_active_override_flag = 1; sc->num_ref_idx_l0_active_minus1 = (unsigned)(nRefs - 1); }   // as for H.264
    setup_me_hevc((void *)((NvU8 *)setup->cpu + OFF_ME));
    setup_md_hevc((void *)((NvU8 *)setup->cpu + OFF_MD));
    setup_q_hevc((void *)((NvU8 *)setup->cpu + OFF_Q));
}

// ---- AV1 (M12): NVIDIA's Linux driver's picture setups (src/av1enctmpl.h, traced at 256x128), patched in the fields that
// change with the picture size and the frame (found by comparing traces at 256x128, 640x360 and 1920x1080 and across
// frames). Ada reads nvenc_av1_drv_pic_setup_s with a fourth 32-byte surface config at 0x64 (a copy of the reference
// picture's), so the offsets below are the header's plus 0x20 from there on. One reference (LAST, the previous frame),
// as the driver's low-latency P frames; CDEF and loop restoration on (the engine picks CDEF per block from two strengths
// and leaves restoration off); constant quantizer per frame type (rate control: later, T12.03).
static void put32(void *b, size_t off, NvU32 v) { memcpy((NvU8 *)b + off, &v, 4); }
static void put16(void *b, size_t off, NvU16 v) { memcpy((NvU8 *)b + off, &v, 2); }
// loop filter level from the base quantizer index: the driver's choices were 7 at 80 (key) and 13 at 100 (P); libaom's
// "pick from q" rule rounded the same way (av1_pick_filter_level, 8-bit), minus 4 on key frames
static int av1_lf_level(int q, bool key)
{
    static const int ac[] = { 4, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22 };   // Ac_Qlookup below 16
    const int acq = q < 16 ? ac[q] : (q * 1191 + 1250) / 1000;   // close to Ac_Qlookup[q] in the low range (8-bit)
    int l = (acq * 20723 + 1015158 + (1 << 17)) >> 18;
    if (key) l -= 4;
    return l < 0 ? 0 : l > 63 ? 63 : l;
}

static void setup_av1(Enc *e, RmMem *setup, RmMem *bs, bool idr, int q, int lf)
{
    NvU8 *b = setup->cpu;
    const int W = e->cfg.width, H = e->cfg.height;
    const size_t rows = ((size_t)H + 63) & ~(size_t)63;
    memcpy(b, idr ? av1_tmpl_key : av1_tmpl_p, sizeof av1_tmpl_key);
    memcpy(b + OFF_AV1_TILE, idr ? av1_tmpl_key_tile : av1_tmpl_p_tile, sizeof av1_tmpl_key_tile);
    // surface configs: reference, input, output, and the extra one (dims, pitch, plane offsets in units of 256: luma
    // "bottom" = half a plane, chroma after the luma plane, chroma "bottom" half a chroma plane on)
    // pitches in samples, plane offsets in bytes (10-bit: 16-bit samples, offsets double; as HEVC Main 10)
    const NvU32 dims = (NvU32)(W - 1) | (NvU32)(H - 1) << 16, rp = (NvU32)e->rPitch, ip = (NvU32)e->pitch, rpb = rp * (NvU32)e->bps;
    for (int k = 0; k < 4; k++) {
        const size_t o = 4 + 0x20 * (size_t)k; const bool in = k == 1;
        put32(b, o, dims); put32(b, o + 4, in ? ip | ip << 16 : rp | rp << 16);
        if (in) { put32(b, o + 0x14, (NvU32)(e->lumaSz >> 8)); continue; }
        put32(b, o + 0x10, (NvU32)(rows / 2 * rpb >> 8)); put32(b, o + 0x14, (NvU32)(e->rLuma >> 8));
        put32(b, o + 0x18, (NvU32)((e->rLuma + rows / 4 * rpb) >> 8));
    }
    if (e->cfg.bit_depth > 8) put32(b, 0x84, *(NvU32 *)(b + 0x84) | 2u << 7 | 2u << 11);   // seqhdr: bit depth, input bit depth - 8
    for (int i = 0; i < 4; i++) b[0x98 + i] = (NvU8)lf;            // frame header: loop filter levels
    b[0xc0] = (NvU8)q;                                              // frame header: base_q_idx
    b[0xc9] = (NvU8)e->cfg.qp_p; b[0xca] = (NvU8)e->cfg.qp_p; b[0xcb] = (NvU8)e->cfg.qp_i;   // RC: QP[P, B, I]
    put32(b, 0x154, (NvU32)e->hist.size); put32(b, 0x158, (NvU32)bs->size); put32(b, 0x15c, 0);   // pic_control buffer sizes
    put32(b, 0x16c, 0x1000); put32(b, 0x18c, 0x1000);              // mpec / stats fifo offsets in the status (not enabled)
    put32(b, 0x178, 0x100 + (NvU32)av1_rc_area(e)); put32(b, 0x174, 0x100 + 2 * (NvU32)av1_rc_area(e));   // act / pass-2 in RC data
    put32(b, 0x180, 0x100u | (NvU32)e->av1SbCols << 16); put32(b, 0x184, (NvU32)e->av1SbRows);   // one strip: the whole frame
    put32(b, 0x31c, 0x80000000u | (NvU32)((size_t)e->wmb * e->hmb * 56 > 202500 ? (size_t)e->wmb * e->hmb * 56 : 202500));   // gpTimer
    NvU8 *t = b + OFF_AV1_TILE;                                     // tile control: one tile, corners inclusive, next = this
    put16(t, 4, (NvU16)(e->av1SbCols - 1)); put16(t, 6, (NvU16)(e->av1SbRows - 1));
    put16(t, 0x14, (NvU16)(e->av1SbCols - 1)); put16(t, 0x16, (NvU16)(e->av1SbRows - 1));
    t[0xa] = (NvU8)q;                                               // qp_avr
    if (e->cfg.rc_mode) {   // rate control as the driver sets it up (docs/traces/av1-linux cbr / vbr; M12, T12.03)
        put32(b, 0x88, *(NvU32 *)(b + 0x88) | 0x2000);               // frame header: delta_q_present (block-level deltas)
        nvenc_av1_rc_s r; memset(&r, 0, sizeof r);
        const bool vbr = e->cfg.rc_mode == 1 || e->cfg.rc_mode == 17;
        const double pixels = (double)W * H, perFrame = (double)e->cfg.bitrate / e->cfg.fps;
        r.hrd_type = 2;
        r.QP[0] = (unsigned char)e->cfg.qp_p; r.QP[1] = (unsigned char)(e->cfg.qp_p + 16 > 255 ? 255 : e->cfg.qp_p + 16); r.QP[2] = (unsigned char)e->cfg.qp_i;
        for (int i = 0; i < 3; i++) { r.minQP[i] = 0; r.maxQP[i] = 255; }
        r.maxQPD = 32; r.baseQPD = 16; r.rhopbi[0] = vbr ? 316 : 294; r.rhopbi[1] = vbr ? 414 : 330; r.rhopbi[2] = 256;
        r.framerate = (unsigned)e->cfg.fps << 8;
        r.buffersize = r.nal_cpb_size = r.vcl_cpb_size = (unsigned)e->cfg.vbv_bits;
        r.nal_bitrate = r.vcl_bitrate = (unsigned)(vbr ? e->cfg.max_bitrate : e->cfg.bitrate);
        r.gop_length = e->cfg.idr_period > 0 ? (unsigned)e->cfg.idr_period : 0xffffffffu;
        r.Np = e->cfg.idr_period > 0 ? (unsigned)e->cfg.idr_period - 1 : 0;
        r.Ravg = (unsigned)(perFrame / pixels * 256); r.R = (unsigned)((double)r.nal_bitrate / e->cfg.fps / pixels * 256);
        r.Bmin = vbr ? 0 : r.buffersize / 81920;                     // the driver's CBR value (122 for a 10 Mbit buffer)
        r.i_size_ratio_x = 1; r.i_size_ratio_y = 1;
        memcpy(b + 0xc8, &r, sizeof r);
    }
}

// AV1 picture setup and methods, in the driver's order (docs/traces/av1-linux): codec type 5 with force-output, tile
// statistics, the app timer and sub-frame mode 2; no colocated data; luma and chroma by one address each (the surface
// configs give the chroma planes); on P frames the ME predictors and reference picture 0. Probability tables: key frames
// read the default tables (by quantizer), P frames the tables the previous frame wrote.
static void submit_av1(Enc *e, int ds, bool idr, int slot, int nRefs)
{
    Chan *c = &e->chan;
    RmMem *setup = &e->setup[ds], *in = &e->in[ds], *bs = &e->bs[ds], *status = &e->status[ds];
    const int q = idr ? e->cfg.qp_i : e->cfg.qp_p, lf = av1_lf_level(q, idr);
    setup_av1(e, setup, bs, idr, q, lf);
    const NvU64 recOut = e->recon.gpu + (NvU64)slot * e->rSlot, inVa = e->extInput ? e->extInput : in->gpu;
    RmMem *probOut = e->av1OutIdx ? &e->av1ProbOut2 : &e->av1ProbOut, *probPrev = e->av1OutIdx ? &e->av1ProbOut : &e->av1ProbOut2;
    RmMem *meIn = &e->mepred[e->seq & 1], *meOut = &e->mepred[(e->seq + 1) & 1];
    chan_mthd(c, 0, NVC9B7_SET_APPLICATION_ID, 1, (NvU32)NVC9B7_SET_APPLICATION_ID_ID_NVENC_AV1);
    // rate control: the driver uses the MB-row modes (17 VBR, 18 CBR)
    const NvU32 rcm = e->cfg.rc_mode == 1 ? 17u : e->cfg.rc_mode == 2 ? 18u : (NvU32)e->cfg.rc_mode;
    chan_mthd(c, 0, NVC9B7_SET_CONTROL_PARAMS, 1, 0x00041905u | DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _RCMODE, rcm));
    chan_mthd(c, 0, NVC9B7_SET_PICTURE_INDEX, 1, e->seq);
    chan_mthd(c, 0, NVC9B7_SET_IN_DRV_PIC_SETUP, 1, (NvU32)(setup->gpu >> 8));
    chan_mthd(c, 0, NVC9B7_SET_IOHISTORY, 1, (NvU32)(e->hist.gpu >> 8));
    if (!idr) { chan_mthd(c, 0, NVC9B7_SET_IN_MEPRED_DATA, 1, (NvU32)(meIn->gpu >> 8)); chan_mthd(c, 0, NVC9B7_SET_OUT_MEPRED_DATA, 1, (NvU32)(meOut->gpu >> 8)); }
    chan_mthd(c, 0, NVC9B7_SET_IO_RC_PROCESS, 1, (NvU32)(e->rcproc.gpu >> 8));
    chan_mthd(c, 0, NVC9B7_SET_OUT_REF_PIC_LUMA, 1, (NvU32)(recOut >> 8)); chan_mthd(c, 0, NVC9B7_SET_OUT_REF_PIC_CHROMA, 1, (NvU32)(recOut >> 8));
    chan_mthd(c, 0, NVC9B7_SET_OUT_ENC_STATUS, 1, (NvU32)(status->gpu >> 8));
    chan_mthd(c, 0, NVC9B7_SET_OUT_BITSTREAM, 1, (NvU32)(bs->gpu >> 8));
    chan_mthd(c, 0, NVC9B7_SET_IN_CUR_PIC, 1, (NvU32)(inVa >> 8));
    chan_mthd(c, 0, NVC9B7_SET_IN_CUR_PIC_CHROMA_U, 1, (NvU32)(inVa >> 8)); chan_mthd(c, 0, NVC9B7_SET_IN_CUR_PIC_CHROMA_V, 1, (NvU32)(inVa >> 8));
    chan_mthd(c, 0, NVC9B7_SET_IN_RCDATA, 1, (NvU32)(e->rcdata.gpu >> 8));
    chan_mthd(c, 0, NVC9B7_SET_IN_AV1_PROBABILITY_DATA, 1, (NvU32)((idr ? e->av1ProbIn.gpu : probPrev->gpu) >> 8));
    chan_mthd(c, 0, NVC9B7_SET_OUT_AV1_PROBABILITY_DATA, 1, (NvU32)(probOut->gpu >> 8));
    static const NvU32 lm[6] = { NVC9B7_SET_IOENTROPY_ABOVE_MV_DATA, NVC9B7_SET_IOENTROPY_ABOVE_CTX_DATA, NVC9B7_SET_IODEBLOCK_TOPROW,
                                 NVC9B7_SET_IODEBLOCK_LEFTCOLUMN, NVC9B7_SET_IOCDEF_LEFTCOLUMN, NVC9B7_SET_IOLR_LEFTCOLUMN };
    for (int i = 0; i < 6; i++) chan_mthd(c, 0, lm[i], 1, (NvU32)(e->av1Line[i].gpu >> 8));
    if (!idr && nRefs > 0) {
        const NvU64 ra = e->recon.gpu + (NvU64)e->slotOf[0] * e->rSlot;
        chan_mthd(c, 0, NVC9B7_SET_IN_REF_PIC0_LUMA, 1, (NvU32)(ra >> 8)); chan_mthd(c, 0, NVC9B7_SET_IN_REF_PIC0_CHROMA, 1, (NvU32)(ra >> 8));
    }
    e->av1OutIdx ^= 1;
    e->pend[ds].lf = lf;
}

int enc_submit(Enc *e, bool force_idr, NvU32 *ticket)
{
    Chan *c = &e->chan;
    int W = e->cfg.width, H = e->cfg.height, ds = (int)(e->seq % (NvU32)e->cfg.depth);
    RmMem *setup = &e->setup[ds], *in = &e->in[ds], *bs = &e->bs[ds], *status = &e->status[ds];
    if (enc_inflight(e) >= e->cfg.depth) return -1;
    bool idr = force_idr || e->forceIdr || e->seq == 0 || (e->cfg.idr_period > 0 && e->sinceIdr >= (unsigned)e->cfg.idr_period);
    e->forceIdr = false;
    if (idr) { e->frame_num = 0; e->sinceIdr = 0; e->nRefs = 0; }
    int slot = e->nextSlot;                                   // where this picture's reconstruction goes
    int nRefs = idr ? 0 : e->nRefs;

    // ---- picture setup ----
    memset(setup->cpu, 0, SETUP_SIZE);
    bool hevc = e->cfg.codec == ENC_HEVC;
    const bool av1 = e->cfg.codec == ENC_AV1;
    if (hevc) { setup_hevc(e, setup, bs, idr, nRefs); goto methods; }
    {
    nvenc_h264_drv_pic_setup_s *ps = setup->cpu;
    ps->magic = NV_NVENC_DRV_MAGIC_VALUE;
    nvenc_h264_surface_cfg_s ic = { .frame_width_minus1 = (unsigned short)(W - 1), .frame_height_minus1 = (unsigned short)(H - 1),
        .sfc_pitch = (unsigned short)e->pitch, .sfc_pitch_chroma = (unsigned short)e->pitch, .block_height = 2 };
    nvenc_h264_surface_cfg_s rc = ic; rc.sfc_pitch = rc.sfc_pitch_chroma = (unsigned short)e->rPitch;
    if (e->cfg.chroma444) { ic.sfc_pitch_chroma = (unsigned short)(2 * e->pitch); rc.sfc_pitch_chroma = (unsigned short)(2 * e->rPitch); }   // as HEVC 4:4:4
    ps->input_cfg = ic; ps->refpic_cfg = rc; ps->outputpic_cfg = rc;   // plane offsets 0: each plane has its own method
    H264Params hp = hdr_params(e);
    ps->sps_data.profile_idc = (unsigned)hp.profile_idc; ps->sps_data.level_idc = (unsigned)hp.level_idc; ps->sps_data.chroma_format_idc = e->cfg.chroma444 ? 3 : 1;
    ps->sps_data.lossless_qpprime_flag = e->cfg.lossless;
    ps->sps_data.pic_order_cnt_type = 2; ps->sps_data.log2_max_frame_num_minus4 = LOG2_MAX_FRAME_NUM - 4;
    ps->sps_data.frame_mbs_only = 1;
    ps->pps_data.num_ref_idx_l0_active_minus1 = (unsigned)(e->cfg.num_refs - 1);
    ps->pps_data.deblocking_filter_control_present_flag = 1;
    // High / CABAC as nullmoth's libnvenc_h264 sets them (profile_idc, entropy and 8x8 transform flags; T7.02)
    ps->pps_data.entropy_coding_mode_flag = e->cfg.cabac; ps->pps_data.transform_8x8_mode_flag = e->cfg.h264_profile == 100 || e->cfg.chroma444;
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
    pc->hist_buf_size = (unsigned)e->hist.size; pc->bitstream_buf_size = (unsigned)bs->size;
    pc->pic_type = idr ? 3 : 0; pc->ref_pic_flag = 1; pc->cabac_zero_word_enable = 1; pc->e4byteStartCode = 1;
    pc->slice_mode = 0;   // 1 hangs the engine (T2.05e); one control with all MBs = one slice

    // slices: one slice control per slice, whole MB rows, under slice_mode 0 (slice_mode 1 hangs the engine: T2.05e, T3.09)
    int nsl = e->cfg.slices < 1 ? 1 : e->cfg.slices > ENC_MAX_SLICES ? ENC_MAX_SLICES : e->cfg.slices;
    if (nsl > e->hmb) nsl = e->hmb;
    pc->num_forced_slices_minus1 = (unsigned char)(nsl - 1);
    nvenc_h264_slice_control_s *sc = (void *)((NvU8 *)setup->cpu + OFF_SLICE);
    for (int k = 0; k < nsl; k++) {
        int mb = ((k + 1) * e->hmb / nsl - k * e->hmb / nsl) * e->wmb;   // rows spread evenly
        sc[k].num_mb = (unsigned)mb; sc[k].qp_avr = (unsigned)(idr ? e->cfg.qp_i : e->cfg.qp_p); sc[k].qp_slice_min = 0; sc[k].qp_slice_max = 51;
        if (!idr) { sc[k].num_ref_idx_active_override_flag = 1; sc[k].num_ref_idx_l0_active_minus1 = (unsigned)(nRefs - 1); }
    }

    setup_me((void *)((NvU8 *)setup->cpu + OFF_ME), !idr);
    setup_md((void *)((NvU8 *)setup->cpu + OFF_MD), !idr, e->cfg.h264_profile == 100 || e->cfg.chroma444);
    }

    // ---- methods ----
methods:
    if (av1) { submit_av1(e, ds, idr, slot, nRefs); goto execute; }
    if (chan_push_used(c) > 0x8000) chan_rewind(c);
    NvU64 recOut = e->recon.gpu + (NvU64)slot * e->rSlot;
    NvU64 inVa = e->extInput ? e->extInput : in->gpu;
    RmMem *meIn = &e->mepred[e->seq & 1], *meOut = &e->mepred[(e->seq + 1) & 1];
    // HEVC: application ID 3 and codec type 4 (not in clc9b7.h; seen in nullmoth's library, T6.03)
    NvU32 appId = hevc ? (NvU32)NVC9B7_SET_APPLICATION_ID_ID_NVENC_H265 : (NvU32)NVC9B7_SET_APPLICATION_ID_ID_NVENC_H264;
    if (getenv("ENC_PROBE_APPID")) appId = (NvU32)atoi(getenv("ENC_PROBE_APPID"));   // T13.03 probe: documented IDs only (2 VP8, 4 VP9)
    chan_mthd(c, 0, NVC9B7_SET_APPLICATION_ID, 1, appId);
    chan_mthd(c, 0, NVC9B7_SET_CONTROL_PARAMS, 1, DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _CODEC_TYPE, hevc ? 4u : NVC9B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264) |
              DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _FORCE_OUT_PIC, 1) | DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _RCMODE, (NvU32)e->cfg.rc_mode));
    chan_mthd(c, 0, NVC9B7_SET_PICTURE_INDEX, 1, e->seq);
    struct { NvU32 m; NvU64 a; } bm[] = {
        { NVC9B7_SET_IN_DRV_PIC_SETUP, setup->gpu }, { NVC9B7_SET_IOHISTORY, e->hist.gpu },
        { hevc ? NVC9B7_SET_OUT_REF_PIC_LUMA : NVC9B7_SET_OUT_COLOC_DATA, hevc ? recOut : e->coloc.gpu },   // HEVC: no colocated data
        { NVC9B7_SET_OUT_REF_PIC_LUMA, recOut }, { NVC9B7_SET_OUT_REF_PIC_CHROMA, recOut + e->rLuma },
        { NVC9B7_SET_OUT_ENC_STATUS, status->gpu }, { NVC9B7_SET_OUT_BITSTREAM, bs->gpu },
        { NVC9B7_SET_IN_CUR_PIC, inVa }, { NVC9B7_SET_IN_CUR_PIC_CHROMA_U, inVa + e->lumaSz },
        { NVC9B7_SET_IN_CUR_PIC_CHROMA_V, inVa + e->lumaSz }, { NVC9B7_SET_IO_RC_PROCESS, e->rcproc.gpu },
        { NVC9B7_SET_IN_RCDATA, e->rcdata.gpu } };
    for (size_t i = 0; i < sizeof bm / sizeof bm[0]; i++) chan_mthd(c, 0, bm[i].m, 1, (NvU32)(bm[i].a >> 8));
    if (!idr) {
        if (!hevc) chan_mthd(c, 0, NVC9B7_SET_IN_COLOC_DATA, 1, (NvU32)(e->coloc.gpu >> 8));
        chan_mthd(c, 0, NVC9B7_SET_IN_MEPRED_DATA, 1, (NvU32)(meIn->gpu >> 8));
        chan_mthd(c, 0, NVC9B7_SET_OUT_MEPRED_DATA, 1, (NvU32)(meOut->gpu >> 8));
        for (int i = 0; i < nRefs; i++) {
            // H.264: REF_PIC<n> by DPB slot (l0 = 2 x slot). HEVC: by position in list 0 (poc_entry_l0[i] = i), while l0
            // still carries 2 x slot (nullmoth's third picture: l0[0] 2, the reference in REF_PIC0; T6.07)
            NvU32 n = hevc ? (NvU32)i : (NvU32)e->slotOf[i];
            NvU64 ra = e->recon.gpu + (NvU64)e->slotOf[i] * e->rSlot;
            chan_mthd(c, 0, NVC9B7_SET_IN_REF_PIC0_LUMA + 4 * n, 1, (NvU32)(ra >> 8));
            chan_mthd(c, 0, NVC9B7_SET_IN_REF_PIC0_CHROMA + 4 * n, 1, (NvU32)((ra + e->rLuma) >> 8));
        }
    }
execute:
    chan_mthd(c, 0, NVC9B7_EXECUTE, 1, 0u);
    NvU32 payload = ++e->seq;
    chan_mthd(c, 0, NVC9B7_SEMAPHORE_A, 3, (NvU32)(e->sem.gpu >> 32), (NvU32)e->sem.gpu, payload);
    chan_mthd(c, 0, NVC9B7_SEMAPHORE_D, 1, DRF_DEF(C9B7, _SEMAPHORE_D, _STRUCTURE_SIZE, _ONE) |
              DRF_DEF(C9B7, _SEMAPHORE_D, _OPERATION, _RELEASE) | DRF_DEF(C9B7, _SEMAPHORE_D, _PAYLOAD_SIZE, _32BIT));
    // event: the host waits for that release, then raises the channel's non-stall interrupt (NVK's way), which RM
    // counts on the NVENC<n> notifier. (The engine's own SEMAPHORE_D AWAKEN bit produced no event: T5.04.)
    if (e->ev.h) {
        chan_mthd(c, 0, NVC56F_SEM_ADDR_LO, 5, (NvU32)e->sem.gpu, (NvU32)(e->sem.gpu >> 32), payload, 0u,
                  DRF_DEF(C56F, _SEM_EXECUTE, _OPERATION, _ACQ_CIRC_GEQ) | DRF_DEF(C56F, _SEM_EXECUTE, _PAYLOAD_SIZE, _32BIT));
        chan_mthd(c, 0, NVC56F_NON_STALL_INTERRUPT, 1, 0u);
    }
    memset(status->cpu, 0, av1 ? 0x880 : 256);
    e->pend[ds].idr = idr; e->pend[ds].frame_num = e->frame_num; e->pend[ds].poc = av1 ? e->sinceIdr : 2 * e->sinceIdr;
    e->pend[ds].t0 = now_ms(); e->pend[ds].slot = slot; e->pend[ds].nRefs = nRefs;
    for (int i = 0; i < nRefs; i++) e->pend[ds].refs[i] = e->slotOf[i];
    chan_submit(c);
    *ticket = payload;

    // DPB (decided by submission order): this picture becomes the most recent reference; the oldest drops out past num_refs
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

// AV1: the temporal unit around the engine's tiles, with the headers NVIDIA's driver writes for this setup (docs/traces/
// av1-linux): sequence header with CDEF and restoration on; frame header with the engine's base quantizer, loop filter
// level and CDEF damping (status), the setup's two CDEF strengths (0, and luma primary 4), restoration off on every
// plane, TX_MODE_SELECT; inter frames: switchable interpolation filters, reference_select, CDFs from LAST (primary
// reference 0) and saved at the end of the frame. Reference slots = reconstruction slots; every reference index names the
// previous frame (LAST).
static Av1SeqParams av1_seq(const Enc *e)
{
    return (Av1SeqParams){ .width = e->cfg.width, .height = e->cfg.height, .bit_depth = e->cfg.bit_depth, .full_range = e->cfg.full_range,
                           .enable_cdef = true, .enable_restoration = true };
}
size_t enc_av1_config(Enc *e, uint8_t *out) { Av1SeqParams sp = av1_seq(e); return av1_write_sequence_header(&sp, out); }

static int av1_assemble(Enc *e, int ds, EncFrame *out)
{
    const NvU8 *sb = e->status[ds].cpu;
    const nvenc_av1_pic_stat_s *st = (const void *)sb;
    const NvU32 *ts = (const NvU32 *)(sb + 0x100);                 // tile statistics (tile_stat_offset 0x100): offset, size
    const bool key = e->pend[ds].idr;
    Av1SeqParams sp = av1_seq(e);
    const int lf = st->loop_filter_level;
    Av1FrameParams fp = { .frame_type = key ? 0 : 1, .order_hint = (int)e->pend[ds].poc, .refresh_frame_flags = 1 << e->pend[ds].slot,
        .primary_ref_frame = key ? 7 : 0, .base_q_idx = st->av1_start_QP, .lf_level = { lf, lf, lf, lf },
        .cdef_damping = (int)st->cdef_damping_minus_3 + 3, .cdef_bits = 1, .cdef_y = { 0, 16 }, .cdef_uv = { 0, 0 },
        .tx_mode_select = true, .reference_select = !key, .interp_filter = 4, .tile_size_bytes = 4, .delta_q_present = e->cfg.rc_mode != 0 };
    for (int i = 0; i < 7; i++) { fp.ref_frame_idx[i] = e->pend[ds].nRefs ? e->pend[ds].refs[0] : 0; fp.ref_order_hint[i] = (int)e->pend[ds].poc - 1; }
    const uint8_t *tile = (const uint8_t *)e->bs[ds].cpu + ts[0]; size_t tsz = ts[1];
    if (getenv("ENC_AV1_LOG")) fprintf(stderr, "av1: frame %u %s: %u bits, q %u, lf %u (asked %d), cdef damping %u, %u tiles, tile 0 at %u size %u\n",
                                       e->pend[ds].poc, key ? "key" : "inter", st->total_bit_count, st->av1_start_QP, lf, e->pend[ds].lf,
                                       st->cdef_damping_minus_3 + 3, st->num_tiles, ts[0], ts[1]);
    if (st->num_tiles != 1 || !tsz || ts[0] + tsz > e->bs[ds].size || tsz + 1024 > e->av1TuCap) return -1;
    uint8_t *o = e->av1Tu[ds]; size_t n = av1_write_temporal_delimiter(o);
    if (key) n += av1_write_sequence_header(&sp, o + n);
    n += av1_write_frame(&sp, &fp, &tile, &tsz, 1, o + n);
    out->data = o; out->size = n;
    return 0;
}

int enc_wait(Enc *e, NvU32 ticket, EncFrame *out)
{
    memset(out, 0, sizeof *out);
    Chan *c = &e->chan;
    int ds = (int)((ticket - 1) % (NvU32)e->cfg.depth);
    volatile NvU32 *sv = e->sem.cpu;
    double t0 = e->pend[ds].t0;
    // spin 50 us, then sleep on the event (re-checking the semaphore between reading and waiting on the count); later
    // frames release higher payloads
    for (;;) {
        if ((int32_t)(*sv - ticket) >= 0 || chan_error(c)) break;
        double el = now_ms() - t0;
        if (el > 3000) break;
        if (e->ev.h && el > 0.05) {
            int64_t seen = rm_event_wait(&e->ev, 0, 0);
            if ((int32_t)(*sv - ticket) >= 0) break;
            if (seen >= 0) rm_event_wait(&e->ev, (uint32_t)seen, 2000);
        }
    }
    out->ms = now_ms() - t0;
    e->waited = ticket;

    nvenc_pic_stat_s *st = e->status[ds].cpu;
    if (getenv("ENC_AV1_LOG") && e->cfg.codec == ENC_AV1) {
        const NvU32 *w = e->status[ds].cpu; fprintf(stderr, "status:");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%s%08x", i % 8 ? " " : "\n  ", w[i]);
        fprintf(stderr, "\n");
    }
    out->idr = e->pend[ds].idr; out->frame_num = e->pend[ds].frame_num; out->poc = e->pend[ds].poc;
    out->bits = st->total_bit_count; out->ucode_error = st->ucode_error_status; out->avg_qp = st->avgQP;
    out->intra_mbs = st->intra_mb_count; out->inter_mbs = st->inter_mb_count;
    if ((int32_t)(*sv - ticket) < 0) {
        fprintf(stderr, "enc: frame %u %s (notifier 0x%x info32 0x%x)\n", ticket, chan_error(c) ? "engine error" : "timeout",
                chan_error(c), ((volatile NvNotification *)c->notifier.cpu)[0].info32);
        NvU64 fa; NvU32 ft; char fs[32];
        if (chan_error(c) && !chan_fault(c, &fa, &ft, fs)) fprintf(stderr, "enc: MMU fault at 0x%llx type %u %s\n", (unsigned long long)fa, ft, fs);
        e->forceIdr = true;
        return -1;
    }
    // error_status is 2 on good frames too: judge by the microcode error and the bit count
    if (st->ucode_error_status || !st->total_bit_count || st->total_bit_count / 8 >= e->bs[ds].size) { e->forceIdr = true; return -1; }
    out->data = (const uint8_t *)e->bs[ds].cpu + st->bitstream_start_pos; out->size = st->total_bit_count / 8;
    if (e->cfg.codec == ENC_AV1) return av1_assemble(e, ds, out);
    return 0;
}

int enc_encode(Enc *e, bool force_idr, EncFrame *out)
{
    NvU32 t;
    if (enc_submit(e, force_idr, &t)) { memset(out, 0, sizeof *out); return -1; }
    return enc_wait(e, t, out);
}
