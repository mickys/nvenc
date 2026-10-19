// dec.h (M8): an H.264 decode session on NVDEC. Buffers are allocated once for the largest picture; each access unit
// (one coded frame) is parsed, set up (nvdec_h264_pic_s), decoded into one of 17 surfaces and the decoded reference
// picture marking (H.264 8.2.5) is applied afterwards. Frames only (no field pictures yet). Headers only: open-gpu-doc's
// nvdec_drv.h / clc9b0.h and the H.264 standard; what the headers do not say is in docs/tasks.md (T8.03, T8.04).
#pragma once
#include "rm.h"
#include "channel.h"
#include "h264parse.h"

enum { DEC_SURFACES = 17 };

typedef struct {
    int surf;                       // surface index (also the co-located MV slot)
    int frame_num, long_term_frame_idx;
    int top_poc, bot_poc;
    int ref;                        // 0 unused, 1 short-term, 2 long-term
} DecRef;

typedef struct {
    Rm *rm; Chan chan;
    unsigned maxW, maxH;            // allocation size (pixels, multiple of 16)
    size_t pitch;
    RmMem setup, bs, offs, status, coloc, hist, mbhist, sem;
    RmMem luma[DEC_SURFACES], chroma[DEC_SURFACES];
    bool busy[DEC_SURFACES];        // held by the caller until h264dec_release (decoded, not yet output)
    DecRef dpb[16]; int ndpb;
    H264Params params; H264PocState poc;
    int max_long_term_frame_idx;    // -1 = no long-term frame indices
    NvU32 semval;
    // the picture just decoded
    int cur_surf, cur_top, cur_bot, cur_w, cur_h;
    unsigned mbs_ok, mbs_err, err_status;
    double engine_ms;
    bool verbose;
} H264Dec;

int  h264dec_open(H264Dec *d, Rm *rm, unsigned maxW, unsigned maxH);
void h264dec_close(H264Dec *d);
// Feed one NAL unit (without start code). SPS/PPS are stored; slices are collected until the next picture starts.
// Returns 1 when a picture was decoded (the previous access unit, finished by this NAL), 0 if not, -1 on error.
// h264dec_flush decodes the last collected picture.
int  h264dec_nal(H264Dec *d, const uint8_t *nal, size_t len);
int  h264dec_flush(H264Dec *d);
// The decoded picture's surface stays out of reuse until released (it may also stay a reference afterwards).
void h264dec_release(H264Dec *d, int surf);
// Copy a decoded surface out as I420 (w x h, the cropped size).
void h264dec_read_i420(H264Dec *d, int surf, uint8_t *dst, unsigned w, unsigned h);
