// encode1.c (milestone M2): encode one H.264 IDR frame on NVENC from a synthetic NV12 picture.
//   ./encode1 [-v] W H QP out.h264 out.nv12
// Writes an Annex B stream (our SPS + PPS + the slice NAL NVENC produced) and the source frame (tightly packed NV12)
// for h264check. Structures and methods: NVIDIA open-gpu-doc nvenc_drv.h / clc9b7.h; what the docs leave out is in
// docs/reports/T2.05d.md and T2.05e.md (input layout, slice_mode, intra_ssd_cnt).
//   NVENC_REPLAY=<blob>      use a captured 32 KB picture setup (scripts/trace2blob.py) instead of ours
//   NVENC_DUMPSETUP=<file>   write the picture setup that is submitted
#include "channel.h"
#include "h264hdr.h"
#include "blocklinear.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "class/clc56f.h"
#include "class/cl2080_notification.h"
#include "clc9b7.h"
#define NV_NVENC_8_2 1
#include "nvenc_drv.h"

static void pattern(uint8_t *y, uint8_t *uv, int W, int H)
{
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            int v = ((c + r) & 255) / 2 + ((((c >> 6) ^ (r >> 6)) & 1) ? 90 : 0);
            if (abs(c - W / 3) < 16) v = 235;
            y[r * W + c] = (uint8_t)(16 + v * 219 / 255);
        }
    for (int r = 0; r < (H + 1) / 2; r++)
        for (int c = 0; c < W / 2; c++) {
            uv[r * W + 2 * c] = (uint8_t)(128 + ((c * 3) % 120) - 60);
            uv[r * W + 2 * c + 1] = (uint8_t)(128 + ((r * 2) % 120) - 60);
        }
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static const char *ucode_err(unsigned e)
{
    static const char *n[] = { "E_NONE", "E_GPTMR_TIMEOUT", "E_MAGIC_NUMBER_MISMATCH", "E_PICSETUP_OFFSET", "E_ENC_STATUS_OUTPUT_OFFSET",
        "E_OUTPUT_BITSTREAM_OFFSET", "E_INSUFFICIENT_WRDMA_BUF", "E_MDP_ERROR", "E_RDO_ERROR", "E_ME_PROGRAMMING", "E_ME_HW", "E_TFBIF_TIMEOUT",
        "E_SKIP_ENCODING", "E_INPUT_BITSTREAM_OFFSET", "E_INPUT_HISTORY_OFFSET", "E_MEPRED_OFFSET", "E_EXTHINT_OFFSET", "E_REFPIC_OFFSET",
        "E_QPMAP_OFFSET", "E_COLLOC_OFFSET", "E_PROB_OFFSET", "E_ENTROPY_OFFSET", "E_FILTER_OFFSET", "E_SSIM_RDO_OFFSET", "E_RC_ERROR",
        "E_SMALLE_BS_OVERFLOW", "E_SMALLE_METHODSERROR", "E_ENTROPY_ERR", "E_RECON_OVERFLOW" };
    return e < sizeof n / sizeof n[0] ? n[e] : "(unknown)";
}

int main(int argc, char **argv)
{
    Rm rm = { 0 }; int ai = 1;
    if (argc > 1 && !strcmp(argv[1], "-v")) { rm.verbose = true; ai++; }
    if (argc - ai < 5) { fprintf(stderr, "usage: encode1 [-v] W H QP out.h264 out.nv12\n"); return 2; }
    int W = atoi(argv[ai]), H = atoi(argv[ai + 1]), QP = atoi(argv[ai + 2]);
    const char *outH264 = argv[ai + 3], *outYuv = argv[ai + 4];
    int wmb = (W + 15) / 16, hmb = (H + 15) / 16, mbs = wmb * hmb, CH = (H + 1) / 2;

    bool v = rm.verbose;
    if (rm_open(&rm)) return 1; rm.verbose = v;
    if (rm_va_init(&rm)) return 1;

    // source picture, pitch-linear NV12 (kept for the quality check)
    uint8_t *srcY = malloc((size_t)W * H), *srcUV = malloc((size_t)W * CH);
    pattern(srcY, srcUV, W, H);
    FILE *f = fopen(outYuv, "wb"); fwrite(srcY, 1, (size_t)W * H, f); fwrite(srcUV, 1, (size_t)W * CH, f); fclose(f);

    // input picture: NV12 in NVENC's input layout (block_height 2), coded size = whole macroblocks. The engine reads
    // 8-12 KB past the chroma plane (T2.04), hence the slack.
    size_t pitch = ((size_t)wmb * 16 + 63) & ~(size_t)63;
    size_t lumaSz = (nvenc_in_plane_size(pitch, (size_t)hmb * 16) + 255) & ~(size_t)255;
    size_t chromaSz = nvenc_in_plane_size(pitch, (size_t)hmb * 8);
    // reconstructed picture: block-linear like the input; the engine writes rows rounded up (T2.09), hence 32-row planes
    size_t rPitch = ((size_t)wmb * 16 + 255) & ~(size_t)255;
    size_t rLuma = (rPitch * (((size_t)hmb * 16 + 31) & ~(size_t)31) + 255) & ~(size_t)255;
    size_t rChroma = rPitch * (((size_t)hmb * 8 + 31) & ~(size_t)31);
    size_t histSz = ((((size_t)(W - 1) + 16) / 16 + 16) * H264_HIST_BLOCK_SIZE + 0xffff) & ~(size_t)0xffff;

    RmMem in, setup, bs, status, hist, recon, coloc, sem;
    if (rm_mem_alloc(&rm, &in, lumaSz + chromaSz + (64 << 10), 0x1000, true, true, true) ||
        rm_mem_alloc(&rm, &recon, rLuma + rChroma + (64 << 10), 0x1000, false, false, true) ||
        rm_mem_alloc(&rm, &setup, 0x10000, 0x1000, true, true, true) ||
        rm_mem_alloc(&rm, &bs, 8 << 20, 0x1000, true, true, true) ||
        rm_mem_alloc(&rm, &status, 1 << 20, 0x1000, true, true, true) ||
        rm_mem_alloc(&rm, &hist, histSz, 0x1000, false, false, true) ||
        rm_mem_alloc(&rm, &coloc, ((size_t)mbs * 64 + 0xffff) & ~(size_t)0xffff, 0x1000, false, false, true) ||
        rm_mem_alloc(&rm, &sem, 0x1000, 0x1000, true, true, true)) { fprintf(stderr, "allocation failed\n"); return 1; }
    memset(in.cpu, 0, in.size);
    nvenc_in_tile_plane(in.cpu, srcY, W, W, H, pitch);
    nvenc_in_tile_plane((uint8_t *)in.cpu + lumaSz, srcUV, W, W, CH, pitch);
    memset(setup.cpu, 0, setup.size); memset(status.cpu, 0, 4096); memset(bs.cpu, 0, 4096);
    volatile NvU32 *sv = sem.cpu; sv[0] = 0;

    // ---- picture setup: the 512-byte header, then one slice/me/md/quant control each ----
    enum { OFF_SLICE = 512, OFF_ME = 640, OFF_MD = 832, OFF_Q = 960 };
    nvenc_h264_drv_pic_setup_s *ps = setup.cpu;
    ps->magic = NV_NVENC_DRV_MAGIC_VALUE;
    // planes come from their own methods (SET_IN_CUR_PIC / _CHROMA_U, SET_OUT_REF_PIC_LUMA / _CHROMA): offsets 0
    nvenc_h264_surface_cfg_s ic = { .frame_width_minus1 = (unsigned short)(W - 1), .frame_height_minus1 = (unsigned short)(H - 1),
        .sfc_pitch = (unsigned short)pitch, .sfc_pitch_chroma = (unsigned short)pitch, .block_height = 2 };
    nvenc_h264_surface_cfg_s rc = ic; rc.sfc_pitch = rc.sfc_pitch_chroma = (unsigned short)rPitch;
    ps->input_cfg = ic; ps->refpic_cfg = rc; ps->outputpic_cfg = rc;
    ps->sps_data.profile_idc = 77; ps->sps_data.level_idc = 51; ps->sps_data.chroma_format_idc = 1;
    ps->sps_data.pic_order_cnt_type = 0; ps->sps_data.log2_max_frame_num_minus4 = 0; ps->sps_data.log2_max_pic_order_cnt_lsb_minus4 = 2;
    ps->sps_data.frame_mbs_only = 1;
    ps->pps_data.entropy_coding_mode_flag = 0; ps->pps_data.pic_init_qp_minus26 = 0;
    ps->pps_data.deblocking_filter_control_present_flag = 1;
    nvenc_h264_rc_s *rcs = &ps->rate_control;
    for (int i = 0; i < 3; i++) { rcs->QP[i] = (unsigned char)QP; rcs->minQP[i] = 0; rcs->maxQP[i] = 51; rcs->rhopbi[i] = 256; }
    rcs->framerate = 60; rcs->gop_length = 0xffffffffu;
    nvenc_h264_pic_control_s *pc = &ps->pic_control;
    memset(pc->l0, -1, sizeof pc->l0); memset(pc->l1, -1, sizeof pc->l1);
    pc->slice_control_offset = OFF_SLICE; pc->me_control_offset = OFF_ME; pc->md_control_offset = OFF_MD; pc->q_control_offset = OFF_Q;
    pc->hist_buf_size = (unsigned)hist.size; pc->bitstream_buf_size = (unsigned)bs.size;
    pc->pic_type = 3; pc->ref_pic_flag = 1; pc->e4byteStartCode = 1;
    pc->slice_mode = 0;   // 1 (static slices from the control array) hangs the engine (T2.05e); one control with all MBs = one slice

    nvenc_h264_slice_control_s *sc = (void *)((NvU8 *)setup.cpu + OFF_SLICE);
    sc->num_mb = (unsigned)mbs; sc->qp_avr = (unsigned)QP; sc->qp_slice_min = 0; sc->qp_slice_max = 51; sc->force_intra = 1;
    nvenc_h264_me_control_s *me = (void *)((NvU8 *)setup.cpu + OFF_ME);
    me->refinement_mode = 1;
    nvenc_h264_md_control_s *md = (void *)((NvU8 *)setup.cpu + OFF_MD);
    md->intra_luma4x4_mode_enable = 0x1ff; md->intra_luma16x16_mode_enable = 0xf; md->intra_chroma_mode_enable = 0xf;
    md->l0_part_16x16_enable = 1; md->l0_part_16x8_enable = 1; md->l0_part_8x16_enable = 1; md->l0_part_8x8_enable = 1;
    md->pskip_enable = 1; md->mv_cost_enable = 1; md->ip_search_mode = 5; md->early_intra_mode_control = 3;
    md->intra_ssd_cnt_4x4 = 1; md->intra_ssd_cnt_8x8 = 1; md->intra_ssd_cnt_16x16 = 1;   // 0 hangs the engine (T2.05e)

    const char *replay = getenv("NVENC_REPLAY");
    if (replay) {
        FILE *rf = fopen(replay, "rb"); if (!rf) { fprintf(stderr, "cannot read %s\n", replay); return 1; }
        memset(setup.cpu, 0, setup.size); size_t got = fread(setup.cpu, 1, 0x8000, rf); fclose(rf);
        printf("replaying %s (%zu bytes)\n", replay, got);
    }
    if (getenv("NVENC_DUMPSETUP")) { FILE *df = fopen(getenv("NVENC_DUMPSETUP"), "wb"); fwrite(setup.cpu, 1, 0x8000, df); fclose(df); }

    // ---- channel + methods ----
    Chan c; NV_NVENC_ALLOCATION_PARAMETERS ep = { .size = sizeof ep, .engineInstance = 0 };
    if (chan_create(&rm, &c, NV2080_ENGINE_TYPE_NVENC0, NVC9B7_VIDEO_ENCODER, &ep)) return 1;
    chan_mthd(&c, 0, NVC56F_SET_OBJECT, 1, (NvU32)NVC9B7_VIDEO_ENCODER);
    chan_mthd(&c, 0, NVC9B7_SET_APPLICATION_ID, 1, (NvU32)NVC9B7_SET_APPLICATION_ID_ID_NVENC_H264);
    chan_mthd(&c, 0, NVC9B7_SET_CONTROL_PARAMS, 1, DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _CODEC_TYPE, NVC9B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264) |
              DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _FORCE_OUT_PIC, 1) | DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _RCMODE, NVC9B7_SET_CONTROL_PARAMS_RCMODE_CONSTQP));
    struct { NvU32 m; NvU64 a; } bm[] = {
        { NVC9B7_SET_IN_DRV_PIC_SETUP, setup.gpu }, { NVC9B7_SET_OUT_ENC_STATUS, status.gpu }, { NVC9B7_SET_OUT_BITSTREAM, bs.gpu },
        { NVC9B7_SET_IOHISTORY, hist.gpu }, { NVC9B7_SET_IN_CUR_PIC, in.gpu }, { NVC9B7_SET_IN_CUR_PIC_CHROMA_U, in.gpu + lumaSz },
        { NVC9B7_SET_IN_CUR_PIC_CHROMA_V, in.gpu + lumaSz }, { NVC9B7_SET_OUT_REF_PIC_LUMA, recon.gpu },
        { NVC9B7_SET_OUT_REF_PIC_CHROMA, recon.gpu + rLuma }, { NVC9B7_SET_OUT_COLOC_DATA, coloc.gpu } };
    chan_mthd(&c, 0, NVC9B7_SET_PICTURE_INDEX, 1, 0u);
    for (size_t i = 0; i < sizeof bm / sizeof bm[0]; i++) chan_mthd(&c, 0, bm[i].m, 1, (NvU32)(bm[i].a >> 8));
    chan_mthd(&c, 0, NVC9B7_EXECUTE, 1, 0u);
    NvU64 a = sem.gpu;
    chan_mthd(&c, 0, NVC9B7_SEMAPHORE_A, 3, (NvU32)(a >> 32), (NvU32)a, 1u);
    chan_mthd(&c, 0, NVC9B7_SEMAPHORE_D, 1, DRF_DEF(C9B7, _SEMAPHORE_D, _STRUCTURE_SIZE, _ONE) |
              DRF_DEF(C9B7, _SEMAPHORE_D, _OPERATION, _RELEASE) | DRF_DEF(C9B7, _SEMAPHORE_D, _PAYLOAD_SIZE, _32BIT));
    double t0 = now_ms(); chan_submit(&c);
    while (sv[0] != 1u && !chan_error(&c) && now_ms() - t0 < 3000) { }
    double ms = now_ms() - t0;

    nvenc_pic_stat_s *st = status.cpu;
    volatile NvNotification *en = c.notifier.cpu;
    printf("%dx%d QP %d: %s after %.3f ms; channel error notifier: status 0x%x info32 0x%x\n", W, H, QP,
           sv[0] == 1u ? "done" : en[0].status ? "ERROR" : "TIMEOUT", ms, en[0].status, en[0].info32);
    printf("status: ucode_error_status %u (%s) total_bits %u slices %u avgQP %u intra_mbs %u\n", st->ucode_error_status,
           ucode_err(st->ucode_error_status), st->total_bit_count, st->num_slices, st->avgQP, st->intra_mb_count);
    if (en[0].status) {   // where did the engine fault? (a timeout after ~1 s is the engine hanging: info32 0x2f)
        struct { const char *n; RmMem *m; } map[] = { { "input", &in }, { "picsetup", &setup }, { "bitstream", &bs }, { "status", &status },
            { "history", &hist }, { "recon", &recon }, { "coloc", &coloc }, { "semaphore", &sem },
            { "push", &c.push }, { "gpfifo", &c.gpfifo }, { "userd", &c.userd }, { "notifier", &c.notifier } };
        NvU64 fa = 0; NvU32 ft = 0; char fs[32];
        NV_STATUS fst = chan_fault(&c, &fa, &ft, fs);
        printf("MMU fault info -> 0x%x: address 0x%llx type %u \"%s\"\n", fst, (unsigned long long)fa, ft, fs);
        for (size_t i = 0; fa && i < sizeof map / sizeof map[0]; i++) {
            NvU64 lo = map[i].m->gpu, hi = lo + map[i].m->size;
            printf("  %-10s 0x%010llx..0x%010llx %s%s\n", map[i].n, (unsigned long long)lo, (unsigned long long)hi,
                   map[i].m->sysmem ? "sys" : "vid", fa >= lo && fa < hi ? "   <-- fault" : "");
        }
    }

    // error_status is 2 on good frames too (the reference client's included): judge by the ucode error and the bit count
    size_t bytes = st->total_bit_count / 8;
    int ok = sv[0] == 1u && !st->ucode_error_status && bytes > 0 && bytes < bs.size;
    if (ok) {
        H264Params hp = { .width = W, .height = H, .profile_idc = ps->sps_data.profile_idc, .level_idc = ps->sps_data.level_idc,
                          .log2_max_frame_num_minus4 = ps->sps_data.log2_max_frame_num_minus4,
                          .log2_max_poc_lsb_minus4 = ps->sps_data.log2_max_pic_order_cnt_lsb_minus4, .poc_type = ps->sps_data.pic_order_cnt_type,
                          .num_ref_frames = ps->pps_data.num_ref_idx_l0_active_minus1 + 1, .num_ref_idx_l0_minus1 = ps->pps_data.num_ref_idx_l0_active_minus1,
                          .entropy_cabac = ps->pps_data.entropy_coding_mode_flag, .pic_init_qp = 26 + ps->pps_data.pic_init_qp_minus26,
                          .transform_8x8 = ps->pps_data.transform_8x8_mode_flag };
        uint8_t hdr[256]; size_t n = h264_write_sps(&hp, hdr); n += h264_write_pps(&hp, hdr + n);
        f = fopen(outH264, "wb"); fwrite(hdr, 1, n, f); fwrite((const uint8_t *)bs.cpu + st->bitstream_start_pos, 1, bytes, f); fclose(f);
        printf("wrote %s: %zu header + %zu slice bytes\n", outH264, n, bytes);
    }
    chan_destroy(&c);
    rm_mem_free(&rm, &in); rm_mem_free(&rm, &setup); rm_mem_free(&rm, &bs); rm_mem_free(&rm, &status);
    rm_mem_free(&rm, &hist); rm_mem_free(&rm, &recon); rm_mem_free(&rm, &coloc); rm_mem_free(&rm, &sem);
    rm_close(&rm);
    return ok ? 0 : 1;
}
