// h264hdr.h: H.264 SPS/PPS NAL units (ITU-T H.264 7.3.2.1 / 7.3.2.2), matching what we tell NVENC in
// nvenc_h264_sps_data_s / nvenc_h264_pps_data_s, so the slices the engine writes decode against them.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height;              // display size; coded size is rounded up to whole macroblocks (cropping signalled)
    int profile_idc, level_idc;     // e.g. 77 main / 100 high, 51 = level 5.1
    int log2_max_frame_num_minus4, log2_max_poc_lsb_minus4;
    int poc_type;                   // 0 or 2
    int num_ref_idx_l0_minus1;      // PPS default
    int num_ref_frames;
    int entropy_cabac;              // 0 CAVLC, 1 CABAC
    int pic_init_qp;                // 26 + pic_init_qp_minus26
    int transform_8x8;              // high profile only
    int vui;                        // write VUI: BT.709 colour description, no frame reordering (max_num_reorder_frames 0)
    int full_range;                 // VUI video_full_range_flag
} H264Params;

// Annex B (start code 00 00 00 01) SPS and PPS; returns bytes written into out (needs ~64 bytes)
size_t h264_write_sps(const H264Params *p, uint8_t *out);
size_t h264_write_pps(const H264Params *p, uint8_t *out);
