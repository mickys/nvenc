// encode1.c (milestone M2): encode one H.264 IDR frame on NVENC from a synthetic NV12 picture.
//   ./encode1 [-v] W H QP out.h264 out.nv12
// Writes an Annex B stream (our SPS + PPS + the slice NALs NVENC produced) and the source frame (tightly packed NV12)
// for h264check. Structures and methods: NVIDIA open-gpu-doc nvenc_drv.h / clc9b7.h.
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

#define BH 1   // block height: 2 GOBs = 16 rows, one macroblock row per block row

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

static const char *gVar = "";
static int var(const char *k) { const char *p = strstr(gVar, k); return p && (p == gVar || p[-1] == ',') && (p[strlen(k)] == 0 || p[strlen(k)] == ',' || p[strlen(k)] == '='); }
static int varint(const char *k, int dflt) { const char *p = strstr(gVar, k); if (!p) return dflt; p += strlen(k); return *p == '=' ? atoi(p + 1) : dflt; }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main(int argc, char **argv)
{
    Rm rm = { 0 }; int ai = 1;
    if (argc > 1 && !strcmp(argv[1], "-v")) { rm.verbose = true; ai++; }
    if (argc - ai < 5) { fprintf(stderr, "usage: encode1 [-v] W H QP out.h264 out.nv12\n"); return 2; }
    int W = atoi(argv[ai]), H = atoi(argv[ai + 1]), QP = atoi(argv[ai + 2]);
    const char *outH264 = argv[ai + 3], *outYuv = argv[ai + 4];
    int wmb = (W + 15) / 16, hmb = (H + 15) / 16, mbs = wmb * hmb;
    if (getenv("NVENC_VAR")) gVar = getenv("NVENC_VAR");
    unsigned bh = (unsigned)varint("bh", BH);
    printf("variant: %s\n", *gVar ? gVar : "(base)");

    bool v = rm.verbose;
    if (rm_open(&rm)) return 1; rm.verbose = v;
    if (rm_va_init(&rm)) return 1;

    // source picture, pitch-linear NV12 (kept for the quality check)
    uint8_t *srcY = malloc((size_t)W * H), *srcUV = malloc((size_t)W * ((H + 1) / 2));
    pattern(srcY, srcUV, W, H);
    FILE *f = fopen(outYuv, "wb"); fwrite(srcY, 1, (size_t)W * H, f); fwrite(srcUV, 1, (size_t)W * ((H + 1) / 2), f); fclose(f);

    // input picture: block-linear NV12 (luma, then chroma), coded size = whole macroblocks
    size_t pitch = ((size_t)wmb * 16 + 63) & ~(size_t)63;
    size_t lumaSz = bl_plane_size(pitch, (size_t)hmb * 16, bh), chromaSz = bl_plane_size(pitch, (size_t)hmb * 8, bh);
    lumaSz = (lumaSz + 255) & ~(size_t)255;
    RmMem in, setup, bs, status, hist, recon, coloc, sem, scratch;
    size_t inPad = (size_t)varint("inpad", 64) << 10;  // the engine reads 8-12 KB past a 256x128 picture (T2.04)
    if (rm_mem_alloc(&rm, &in, lumaSz + chromaSz + inPad, 0x1000, !(var("vid") || var("allvid")), true, true)) return 1;
    memset(in.cpu, 0, in.size);
    if (var("tileada")) {   // the measured Ada NVENC input layout (T2.05d)
        nvenc_in_tile_plane(in.cpu, srcY, W, W, H, pitch);
        nvenc_in_tile_plane((uint8_t *)in.cpu + lumaSz, srcUV, W, W, (H + 1) / 2, pitch);
    } else {
        bl_tile_plane(in.cpu, srcY, W, W, H, pitch, bh);
        bl_tile_plane((uint8_t *)in.cpu + lumaSz, srcUV, W, W, (H + 1) / 2, pitch, bh);
    }

    // reconstructed (reference) picture, written by NVENC in its 16x16-tiled format
    size_t rPitch = ((size_t)wmb * 16 + 255) & ~(size_t)255;
    // the engine writes the planes with their rows rounded up (720p faulted past an exact-size buffer): 32-row planes + slack
    size_t rLuma = (rPitch * (((size_t)hmb * 16 + 31) & ~(size_t)31) + 255) & ~(size_t)255;
    size_t rChroma = rPitch * (((size_t)hmb * 8 + 31) & ~(size_t)31) + (64 << 10);
    size_t histSz = (((size_t)(W - 1) + 16) / 16 + 16) * 128 * 4;
    if (rm_mem_alloc(&rm, &recon, rLuma + rChroma, 0x1000, false, false, true) ||
        rm_mem_alloc(&rm, &setup, 0x10000, 0x1000, !var("allvid"), true, true) ||
        rm_mem_alloc(&rm, &bs, 8 << 20, 0x1000, !var("allvid"), true, true) ||
        rm_mem_alloc(&rm, &status, 1 << 20, 0x1000, !var("allvid"), true, true) ||
        rm_mem_alloc(&rm, &hist, (histSz + 0xffff) & ~(size_t)0xffff, 0x1000, false, false, true) ||
        rm_mem_alloc(&rm, &coloc, ((size_t)mbs * 64 + 0xffff) & ~(size_t)0xffff, 0x1000, false, false, true) ||
        rm_mem_alloc(&rm, &sem, 0x1000, 0x1000, true, true, true) ||
        rm_mem_alloc(&rm, &scratch, 64 << 20, 0x1000, !var("allvid"), true, true)) { fprintf(stderr, "allocation failed\n"); return 1; }
    memset(scratch.cpu, 0, scratch.size);
    memset(setup.cpu, 0, setup.size); RmMem rcdata, rcproc;
    if (rm_mem_alloc(&rm, &rcdata, 0x10000, 0x1000, false, true, true) || rm_mem_alloc(&rm, &rcproc, 0x100000, 0x1000, false, true, true)) return 1;
    memset(rcdata.cpu, 0, rcdata.size); memset(rcproc.cpu, 0, rcproc.size); memset(status.cpu, 0, 4096); memset(bs.cpu, 0, 4096);
    volatile NvU32 *sv = sem.cpu; sv[0] = 0;

    // ---- picture setup: the 512-byte header, then one slice/me/md/quant control each ----
    unsigned OFF_SLICE = 512, OFF_ME = 640, OFF_MD = 832, OFF_Q = 960;
    if (var("refoffs")) { OFF_ME = 0x3000; OFF_MD = 0x3c00; OFF_Q = 0x4400; OFF_SLICE = 0x5000; }   // the reference's control offsets
    nvenc_h264_drv_pic_setup_s *ps = setup.cpu;
    ps->magic = getenv("NVENC_MAGIC") ? (unsigned)strtoul(getenv("NVENC_MAGIC"), NULL, 0) : NV_NVENC_DRV_MAGIC_VALUE;
    nvenc_h264_surface_cfg_s *ic = &ps->input_cfg;
    ic->frame_width_minus1 = (unsigned short)(W - 1); ic->frame_height_minus1 = (unsigned short)(H - 1);
    ic->sfc_pitch = (unsigned short)pitch; ic->sfc_pitch_chroma = (unsigned short)pitch;
    ic->luma_top_frm_offset = 0; ic->chroma_top_frm_offset = (unsigned)(lumaSz >> 8);
    ic->block_height = bh; ic->tiled_16x16 = 0; ic->memory_mode = 0; ic->input_bl_mode = 0;
    if (var("intile")) ic->tiled_16x16 = 1;                          // T2.05c: input format experiments
    if (varint("inbl", -1) >= 0) ic->input_bl_mode = (unsigned)varint("inbl", 0);
    if (varint("inbhf", -1) >= 0) ic->block_height = (unsigned)varint("inbhf", 0);   // field only, tiling unchanged
    nvenc_h264_surface_cfg_s rc = { .frame_width_minus1 = (unsigned short)(W - 1), .frame_height_minus1 = (unsigned short)(H - 1),
        .sfc_pitch = (unsigned short)rPitch, .sfc_pitch_chroma = (unsigned short)rPitch, .luma_top_frm_offset = 0,
        .chroma_top_frm_offset = (unsigned)(rLuma >> 8), .tiled_16x16 = 1 };
    if (var("reconbl")) { rc.tiled_16x16 = 0; rc.block_height = 2; }   // T2.05e: the reference's recon format
    if (var("refmthd")) { rc.chroma_top_frm_offset = 0; ic->chroma_top_frm_offset = 0; }   // chroma planes come from their own methods
    ps->refpic_cfg = rc; ps->outputpic_cfg = rc;
    ps->sps_data.profile_idc = 77; ps->sps_data.level_idc = 51; ps->sps_data.chroma_format_idc = 1;
    ps->sps_data.pic_order_cnt_type = 0; ps->sps_data.log2_max_frame_num_minus4 = 0; ps->sps_data.log2_max_pic_order_cnt_lsb_minus4 = 2;
    ps->sps_data.frame_mbs_only = 1;
    ps->pps_data.entropy_coding_mode_flag = 0; ps->pps_data.pic_init_qp_minus26 = 0;
    ps->pps_data.deblocking_filter_control_present_flag = 1;
    nvenc_h264_rc_s *rcs = &ps->rate_control;
    for (int i = 0; i < 3; i++) { rcs->QP[i] = (unsigned char)QP; rcs->minQP[i] = 0; rcs->maxQP[i] = 51; rcs->rhopbi[i] = 256; }
    rcs->framerate = 60; rcs->gop_length = 0xffffffffu;
    if (var("rc")) {   // plausible sequence-level RC values instead of zeros (20 Mbit/s, 60 fps, 1 s buffer, GOP 60)
        int br = 20000000; rcs->hrd_type = 0; rcs->buffersize = (unsigned)br; rcs->nal_cpb_size = br; rcs->vcl_cpb_size = br;
        rcs->nal_bitrate = br; rcs->vcl_bitrate = br; rcs->gop_length = 60; rcs->Np = 59 << 4; rcs->Bmin = 0;
        rcs->Ravg = (int)(((long long)br / 60) << 8); rcs->R = rcs->Ravg; rcs->iSizeRatioX = 1; rcs->iSizeRatioY = 1; rcs->maxQPD = 51; rcs->baseQPD = 1;
    }
    nvenc_h264_pic_control_s *pc = &ps->pic_control;
    memset(pc->l0, -1, sizeof pc->l0); memset(pc->l1, -1, sizeof pc->l1);
    pc->frame_num = 0; pc->pic_order_cnt_lsb = 0; pc->idr_pic_id = 0;
    pc->num_forced_slices_minus1 = 0; pc->num_me_controls_minus1 = 0; pc->num_md_controls_minus1 = 0; pc->num_q_controls_minus1 = 0;
    pc->slice_control_offset = OFF_SLICE; pc->me_control_offset = OFF_ME; pc->md_control_offset = OFF_MD; pc->q_control_offset = OFF_Q;
    pc->hist_buf_size = (unsigned)hist.size; pc->bitstream_buf_size = (unsigned)bs.size; pc->bitstream_start_pos = 0;
    pc->pic_struct = 0; pc->pic_type = 3; pc->ref_pic_flag = 1; pc->slice_mode = 1;
    pc->e4byteStartCode = 1;
    pc->codec = (unsigned char)varint("codec", 0);
    if (var("rows")) { pc->slice_encoding_row_start = 0; pc->slice_encoding_row_num = (unsigned short)hmb; }   // T2.05 candidates
    if (var("strips")) { pc->stripID = 0; pc->strips_in_frame = 1; }
    if (var("maxbytes")) pc->max_byte_count_before_resid_zero = (unsigned)bs.size;
    if (var("maxslice")) pc->max_slice_size = (unsigned)bs.size;
    if (var("sliceauto")) pc->slice_mode = 0;
    if (var("noref")) pc->ref_pic_flag = 0;
    ps->gpTimer_timeout_val = (unsigned)strtoul(strstr(gVar, "gptimer=") ? strstr(gVar, "gptimer=") + 8 : "0", NULL, 0);

    nvenc_h264_slice_control_s *sc = (void *)((NvU8 *)setup.cpu + OFF_SLICE);
    sc->num_mb = (unsigned)mbs; sc->qp_avr = (unsigned)QP; sc->qp_slice_min = 0; sc->qp_slice_max = 51; sc->force_intra = 1;
    nvenc_h264_me_control_s *me = (void *)((NvU8 *)setup.cpu + OFF_ME);
    me->refinement_mode = 1;
    if (var("hints")) { me->hint_type0 = 0; me->hint_type1 = 1; me->hint_type2 = 2; me->hint_type3 = 3; me->hint_type4 = 4; }   // must cover 0x1f
    if (varint("mbc", -1) >= 0) me->mbc_mb_size = (unsigned)varint("mbc", 0);
    nvenc_h264_md_control_s *md = (void *)((NvU8 *)setup.cpu + OFF_MD);
    md->intra_luma4x4_mode_enable = 0x1ff; md->intra_luma16x16_mode_enable = 0xf; md->intra_chroma_mode_enable = 0xf;
    md->l0_part_16x16_enable = 1; md->l0_part_16x8_enable = 1; md->l0_part_8x16_enable = 1; md->l0_part_8x8_enable = 1;
    md->pskip_enable = 1; md->mv_cost_enable = 1; md->ip_search_mode = 5; md->early_intra_mode_control = 3;
    if (varint("pictype", -1) >= 0) pc->pic_type = (unsigned)varint("pictype", 3);
    if (var("ssd")) { md->intra_ssd_cnt_4x4 = 1; md->intra_ssd_cnt_8x8 = 1; md->intra_ssd_cnt_16x16 = 1; }   // T2.05e culprits
    if (var("ssd16")) md->intra_ssd_cnt_16x16 = 1;
    if (var("prio")) md->priority_ipred_type_ip1 = 1;
    if (var("skipeval")) md->skip_evaluate_enable = 1;
    if (var("czero")) pc->cabac_zero_word_enable = 1;
    if (var("qpfifo")) pc->qpfifo = 1;
    if (var("ipcm")) md->force_ipcm = 1;                                // T2.05c: switch pipeline stages off
    if (var("nodeblock")) sc->disable_deblocking_filter_idc = 1;
    if (var("cabac")) ps->pps_data.entropy_coding_mode_flag = 1;
    if (var("meonly")) { me->me_only_mode = 1; pc->me_candidates_output_enable = 0; }
    (void)OFF_Q;

    if (getenv("NVENC_DUMPSETUP")) { FILE *df = fopen(getenv("NVENC_DUMPSETUP"), "wb"); fwrite(setup.cpu, 1, 0x8000, df); fclose(df); }
    // T2.05d: NVENC_REPLAY=<blob> replaces our picture setup with one captured from a working client (nvtrace)
    const char *replay = getenv("NVENC_REPLAY");
    if (replay) {
        FILE *rf = fopen(replay, "rb"); if (!rf) { fprintf(stderr, "cannot read %s\n", replay); return 1; }
        memset(setup.cpu, 0, setup.size); size_t got = fread(setup.cpu, 1, 0x8000, rf); fclose(rf);
        printf("replaying %s (%zu bytes): magic 0x%08x, input bh %u tiled %u, recon bh %u tiled %u\n", replay, got, ps->magic,
               ps->input_cfg.block_height, ps->input_cfg.tiled_16x16, ps->outputpic_cfg.block_height, ps->outputpic_cfg.tiled_16x16);
    }

    // ---- channel + methods ----
    Chan c; NV_NVENC_ALLOCATION_PARAMETERS ep = { .size = sizeof ep, .engineInstance = 0 };
    if (chan_create(&rm, &c, NV2080_ENGINE_TYPE_NVENC0, NVC9B7_VIDEO_ENCODER, &ep)) return 1;
    chan_mthd(&c, 0, NVC56F_SET_OBJECT, 1, (NvU32)NVC9B7_VIDEO_ENCODER);
    // every other buffer method H.264 might touch gets its own zeroed 2 MB slice of scratch (NVENC_MINBUF=1: skip them)
    static const struct { NvU32 m; const char *n; } extra[] = {
        { NVC9B7_SET_IN_RCDATA, "IN_RCDATA" }, { NVC9B7_SET_IO_RC_PROCESS, "IO_RC_PROCESS" }, { NVC9B7_SET_IN_COLOC_DATA, "IN_COLOC" },
        { NVC9B7_SET_IN_MEPRED_DATA, "IN_MEPRED" }, { NVC9B7_SET_OUT_MEPRED_DATA, "OUT_MEPRED" }, { NVC9B7_SET_IN_QP_MAP, "IN_QP_MAP" },
        { NVC9B7_SET_IN_PARTITION_BUF, "PARTITION" }, { NVC9B7_SET_IN_CUR_PIC_TASK_STATUS, "CUR_TASK" },
        { NVC9B7_SET_IN_REF_PIC_TASK_STATUS, "REF_TASK" }, { NVC9B7_SET_OUT_TASK_STATUS, "OUT_TASK" },
        { NVC9B7_SET_IN_MV_HINTS_TASK_STATUS, "MV_TASK" }, { NVC9B7_SET_IO_DEBUG_STATUS, "DEBUG_STATUS" },
        { NVC9B7_SET_IN_LAMBDA_MAP, "LAMBDA_MAP" }, { NVC9B7_SET_OUT_HYBRID_RES, "HYBRID_RES" } };
    int nextra = getenv("NVENC_MINBUF") ? 0 : (int)(sizeof extra / sizeof extra[0]);
    int stop = varint("stop", 99);
    if (stop >= 1) chan_mthd(&c, 0, NVC9B7_SET_APPLICATION_ID, 1, (NvU32)NVC9B7_SET_APPLICATION_ID_ID_NVENC_H264);
    if (stop >= 2) {
    if (varint("wd", -1) >= 0) chan_mthd(&c, 0, NVC9B7_SET_WATCHDOG_TIMER, 1, (NvU32)strtoul(strstr(gVar, "wd=") + 3, NULL, 0));
    if (varint("cores", -1) >= 0) chan_mthd(&c, 0, NVC9B7_SET_TOTAL_CORE_NUM, 1, (NvU32)varint("cores", 1));
    chan_mthd(&c, 0, NVC9B7_SET_CONTROL_PARAMS, 1, DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _CODEC_TYPE, NVC9B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264) |
              DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _FORCE_OUT_PIC, var("nofout") ? 0 : 1) | DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _TOPLEVEL, var("toplevel") ? 1 : 0) | DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _DEBUG_MODE, getenv("NVENC_DEBUG") ? 1 : 0) |
              DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _GPTIMER_ON, var("gpton") ? 1 : 0) | DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _MEONLY, var("meonly") ? 1 : 0) | DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _DUMP_CYCLE_COUNT, var("cycles") ? 1 : 0) |
              DRF_NUM(C9B7, _SET_CONTROL_PARAMS, _RCMODE, NVC9B7_SET_CONTROL_PARAMS_RCMODE_CONSTQP));
    }
    if (var("bare")) {   // T2.05: only the status buffer before EXECUTE
        chan_mthd(&c, 0, NVC9B7_SET_OUT_ENC_STATUS, 1, (NvU32)(status.gpu >> 8));
        stop = stop < 3 ? stop : 3; if (stop == 3) { stop = 4; goto exec; }
    }
    if (replay || var("refmthd")) {   // the reference client's method set and buffer layout (T2.05d)
        NvU32 cp = (NvU32)strtoul(strstr(gVar, "cp=") ? strstr(gVar, "cp=") + 3 : "0x1303", NULL, 0);
        chan_mthd(&c, 0, NVC9B7_SET_CONTROL_PARAMS, 1, cp);
        struct { NvU32 m; NvU64 a; } rm_[] = {
            { NVC9B7_SET_PICTURE_INDEX, 0 }, { NVC9B7_SET_IN_DRV_PIC_SETUP, setup.gpu }, { NVC9B7_SET_IOHISTORY, hist.gpu },
            { NVC9B7_SET_OUT_COLOC_DATA, coloc.gpu }, { NVC9B7_SET_IO_RC_PROCESS, rcproc.gpu }, { NVC9B7_SET_OUT_REF_PIC_LUMA, recon.gpu },
            { NVC9B7_SET_OUT_REF_PIC_CHROMA, recon.gpu + rLuma }, { NVC9B7_SET_OUT_ENC_STATUS, status.gpu }, { NVC9B7_SET_OUT_BITSTREAM, bs.gpu },
            { NVC9B7_SET_IN_CUR_PIC, in.gpu }, { NVC9B7_SET_IN_CUR_PIC_CHROMA_U, in.gpu + lumaSz }, { NVC9B7_SET_IN_CUR_PIC_CHROMA_V, in.gpu + lumaSz },
            { NVC9B7_SET_IN_RCDATA, rcdata.gpu } };
        for (size_t i = 0; i < sizeof rm_ / sizeof rm_[0]; i++) chan_mthd(&c, 0, rm_[i].m, 1, (NvU32)(rm_[i].a >> 8));
        stop = 4; goto exec;
    }
    if (stop >= 3) {
        // buffer methods in order; NVENC_VAR=upto=N sends only the first N (T2.05 bisection)
        struct { NvU32 m; NvU64 a; } bm[] = {
            { NVC9B7_SET_PICTURE_INDEX, 0 }, { NVC9B7_SET_OUT_ENC_STATUS, status.gpu }, { NVC9B7_SET_IN_DRV_PIC_SETUP, setup.gpu },
            { NVC9B7_SET_OUT_BITSTREAM, bs.gpu }, { NVC9B7_SET_IOHISTORY, hist.gpu }, { NVC9B7_SET_IN_CUR_PIC, in.gpu },
            { NVC9B7_SET_IN_CUR_PIC_CHROMA_U, in.gpu }, { NVC9B7_SET_OUT_REF_PIC_LUMA, recon.gpu }, { NVC9B7_SET_OUT_REF_PIC_CHROMA, recon.gpu },
            { NVC9B7_SET_OUT_COLOC_DATA, coloc.gpu } };
        int nbm = (int)(sizeof bm / sizeof bm[0]), upto = varint("upto", 99);
        for (int i = 0; i < nbm && i < upto; i++) chan_mthd(&c, 0, bm[i].m, 1, (NvU32)(bm[i].a >> 8));
        for (int i = 0; i < nextra && nbm + i < upto; i++) chan_mthd(&c, 0, extra[i].m, 1, (NvU32)((scratch.gpu + (NvU64)i * (2 << 20)) >> 8));
    }
exec:
    if (stop >= 4) chan_mthd(&c, 0, NVC9B7_EXECUTE, 1, 0u);
    NvU64 a = sem.gpu;
    chan_mthd(&c, 0, NVC9B7_SEMAPHORE_A, 3, (NvU32)(a >> 32), (NvU32)a, 1u);
    chan_mthd(&c, 0, NVC9B7_SEMAPHORE_D, 1, DRF_DEF(C9B7, _SEMAPHORE_D, _STRUCTURE_SIZE, _ONE) |
              DRF_DEF(C9B7, _SEMAPHORE_D, _OPERATION, _RELEASE) | DRF_DEF(C9B7, _SEMAPHORE_D, _PAYLOAD_SIZE, _32BIT));
    double t0 = now_ms(); chan_submit(&c);
    double tErr = -1;   // when RM's error notifier was set (T2.05c: timeout or immediate?)
    while (sv[0] != 1u && now_ms() - t0 < 3000) { if (tErr < 0 && chan_error(&c)) { tErr = now_ms() - t0; break; } }
    double ms = now_ms() - t0;
    if (tErr >= 0) printf("error notifier set %.3f ms after the doorbell\n", tErr);

    nvenc_pic_stat_s *st = status.cpu;
    volatile NvNotification *en = c.notifier.cpu;
    printf("%dx%d QP %d: %s after %.3f ms; GPGet %u/%u, channel error notifier: status 0x%x info32 0x%x info16 0x%x\n", W, H, QP,
           sv[0] == 1u ? "done" : "TIMEOUT", ms, chan_gpget(&c), c.gpPut, en[0].status, en[0].info32, en[0].info16);
    static const char *errs[] = { "E_NONE", "E_GPTMR_TIMEOUT", "E_MAGIC_NUMBER_MISMATCH", "E_PICSETUP_OFFSET", "E_ENC_STATUS_OUTPUT_OFFSET",
        "E_OUTPUT_BITSTREAM_OFFSET", "E_INSUFFICIENT_WRDMA_BUF", "E_MDP_ERROR", "E_RDO_ERROR", "E_ME_PROGRAMMING", "E_ME_HW", "E_TFBIF_TIMEOUT",
        "E_SKIP_ENCODING", "E_INPUT_BITSTREAM_OFFSET", "E_INPUT_HISTORY_OFFSET", "E_MEPRED_OFFSET", "E_EXTHINT_OFFSET", "E_REFPIC_OFFSET",
        "E_QPMAP_OFFSET", "E_COLLOC_OFFSET", "E_PROB_OFFSET", "E_ENTROPY_OFFSET", "E_FILTER_OFFSET", "E_SSIM_RDO_OFFSET", "E_RC_ERROR",
        "E_SMALLE_BS_OVERFLOW", "E_SMALLE_METHODSERROR", "E_ENTROPY_ERR", "E_RECON_OVERFLOW" };
    printf("status: picture_index %u error_status %u ucode_error_status 0x%x total_bits %u type1_bits %u pic_type %u slices %u avgQP %u "
           "start %u last_valid %u intra_mbs %u inter_mbs %u\n", st->picture_index, st->error_status, st->ucode_error_status,
           st->total_bit_count, st->type1_bit_count, st->pic_type, st->num_slices, st->avgQP, st->bitstream_start_pos,
           st->last_valid_byte_offset, st->intra_mb_count, st->inter_mb_count);
    if (en[0].status) {   // where did the engine fault? (T2.03)
        struct { const char *n; RmMem *m; } map[] = { { "input", &in }, { "picsetup", &setup }, { "bitstream", &bs }, { "status", &status },
            { "history", &hist }, { "recon", &recon }, { "coloc", &coloc }, { "semaphore", &sem }, { "scratch", &scratch },
            { "push", &c.push }, { "gpfifo", &c.gpfifo }, { "userd", &c.userd }, { "notifier", &c.notifier } };
        NvU64 fa = 0; NvU32 ft = 0; char fs[32];
        NV_STATUS fst = chan_fault(&c, &fa, &ft, fs);
        printf("MMU fault info -> 0x%x: address 0x%llx type %u \"%s\"\n", fst, (unsigned long long)fa, ft, fs);
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
            NvU64 lo = map[i].m->gpu, hi = lo + map[i].m->size;
            printf("  %-10s 0x%010llx..0x%010llx %s%s\n", map[i].n, (unsigned long long)lo, (unsigned long long)hi,
                   map[i].m->sysmem ? "sys" : "vid", fa >= lo && fa < hi ? "   <-- fault" : "");
        }
    }
    printf("status words:"); for (int i = 0; i < 32; i++) printf(" %x", ((const NvU32 *)status.cpu)[i]); printf("\n");
    if (st->ucode_error_status) printf("ucode error: %s\n", st->ucode_error_status < sizeof errs / sizeof errs[0] ? errs[st->ucode_error_status] : "(unknown code)");
    for (int i = 0; i < nextra; i++) {
        const NvU32 *w = (const NvU32 *)((const NvU8 *)scratch.cpu + (size_t)i * (2 << 20)); int nz = 0;
        for (int k = 0; k < 4096; k++) if (w[k]) nz++;
        if (nz) { printf("%-12s %d nonzero words in the first 16 KB:", extra[i].n, nz); for (int k = 0; k < 16; k++) printf(" %08x", w[k]); printf("\n"); }
    }
    const uint8_t *b = bs.cpu; printf("bitstream head:"); for (int i = 0; i < 16; i++) printf(" %02x", b[i]); printf("\n");

    size_t bytes = st->total_bit_count / 8;
    int ok = sv[0] == 1u && !st->ucode_error_status && bytes > 0 && bytes < bs.size;   // error_status 2 with no ucode error is seen on good frames too
    if (ok) {
        H264Params hp = { .width = W, .height = H, .profile_idc = ps->sps_data.profile_idc, .level_idc = ps->sps_data.level_idc,
                          .log2_max_frame_num_minus4 = ps->sps_data.log2_max_frame_num_minus4,
                          .log2_max_poc_lsb_minus4 = ps->sps_data.log2_max_pic_order_cnt_lsb_minus4, .poc_type = ps->sps_data.pic_order_cnt_type,
                          .num_ref_frames = ps->pps_data.num_ref_idx_l0_active_minus1 + 1, .num_ref_idx_l0_minus1 = ps->pps_data.num_ref_idx_l0_active_minus1,
                          .entropy_cabac = ps->pps_data.entropy_coding_mode_flag, .pic_init_qp = 26 + ps->pps_data.pic_init_qp_minus26,
                          .transform_8x8 = ps->pps_data.transform_8x8_mode_flag };
        printf("SPS/PPS from the setup: profile %d level %d poc_type %d log2_max_frame_num-4 %d refs %d cabac %d init_qp %d 8x8 %d\n", hp.profile_idc,
               hp.level_idc, hp.poc_type, hp.log2_max_frame_num_minus4, hp.num_ref_frames, hp.entropy_cabac, hp.pic_init_qp, hp.transform_8x8);
        uint8_t hdr[256]; size_t n = h264_write_sps(&hp, hdr); n += h264_write_pps(&hp, hdr + n);
        f = fopen(outH264, "wb"); fwrite(hdr, 1, n, f); fwrite(b + st->bitstream_start_pos, 1, bytes, f); fclose(f);
        printf("wrote %s: %zu header + %zu slice bytes\n", outH264, n, bytes);
    }
    chan_destroy(&c);
    rm_mem_free(&rm, &in); rm_mem_free(&rm, &setup); rm_mem_free(&rm, &bs); rm_mem_free(&rm, &status);
    rm_mem_free(&rm, &hist); rm_mem_free(&rm, &recon); rm_mem_free(&rm, &coloc); rm_mem_free(&rm, &sem); rm_mem_free(&rm, &scratch); rm_mem_free(&rm, &rcdata); rm_mem_free(&rm, &rcproc);
    rm_close(&rm);
    return ok ? 0 : 1;
}
