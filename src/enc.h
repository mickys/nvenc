// enc.h: an H.264, HEVC or AV1 encode session on one NVENC engine: buffers, channel, picture setup per frame, IDR and P pictures,
// rate control, slices. enc_encode() is synchronous; enc_submit()/enc_wait() keep up to `depth` frames in flight. Structures: NVIDIA open-gpu-doc
// nvenc_drv.h / clc9b7.h; what the docs leave out is in docs/design.md ("M2 notes", "M3 notes").
#pragma once
#include "channel.h"
#include <stdint.h>

#define ENC_MAX_REFS 3
#define ENC_MAX_SLICES 16
#define ENC_MAX_DEPTH 3

enum { ENC_H264 = 0, ENC_HEVC = 1, ENC_AV1 = 2 };

typedef struct {
    int codec;                      // ENC_H264, ENC_HEVC or ENC_AV1 (qp_i / qp_p are then AV1 quantizer indices, 0..255)
    int width, height;              // display size; coded size is whole macroblocks (H.264) / 16-pixel CBs (HEVC)
    int qp_i, qp_p;                 // constant QP for I/IDR and P pictures
    int num_refs;                   // L0 reference frames, 1..ENC_MAX_REFS
    int dpb_extra;                  // extra pictures announced in the HEVC VPS/SPS decoded picture buffer size, 0..2
                                    // (refs + 1 + 2 <= 6 = the smallest MaxDpbSize of any level)
    int engine;                     // 0 = NVENC0, 1 = NVENC1
    int idr_period;                 // 0 = only the first picture (and enc_encode(force_idr))
    int rc_mode;                    // SET_CONTROL_PARAMS RCMODE: 0 const QP, 1 VBR, 2 CBR, 17/18 MB-row VBR/CBR
    int bitrate, max_bitrate;       // bits/s (rate control modes)
    int vbv_bits;                   // CPB size in bits (0 = one second)
    int fps;                        // frame rate for rate control (default 60)
    int slices;                     // slices per picture, whole MB rows (1..ENC_MAX_SLICES)
    bool spin;                      // wait for the engine by spinning only (default: spin 50 us, then sleep on the NVENC event)
    int depth;                      // frames in flight with enc_submit (1..ENC_MAX_DEPTH, default 1)
    int h264_profile;               // H.264: 77 Main (default) or 100 High (adds the 8x8 transform and 8x8 intra modes)
    bool cabac;                     // H.264: CABAC entropy coding (else CAVLC)
    int bit_depth;                  // HEVC: 8 (Main, default) or 10 (Main 10: 16-bit input samples, enc_load_p010)
    bool full_range;                // input is full-range (VUI video_full_range_flag); BGRA input is converted to video range
    bool verbose;
} EncConfig;

typedef struct {
    const uint8_t *data;            // Annex B slice NAL(s) in the session's bitstream buffer (valid until that slot is reused);
                                    // AV1: a temporal unit (temporal delimiter, sequence header on key frames, frame OBU)
    size_t size;
    bool idr;
    unsigned frame_num, poc;
    unsigned bits, ucode_error, avg_qp, intra_mbs, inter_mbs;
    double ms;                      // doorbell to completion seen
} EncFrame;

typedef struct {
    EncConfig cfg;
    Rm rm;
    Chan chan;
    int wmb, hmb;
    size_t pitch, lumaSz, chromaSz; // input picture (NVENC input layout); pitch in samples
    size_t rPitch, rLuma, rSlot;    // reconstructed pictures: one slot per DPB entry; rPitch in samples
    size_t bps;                     // bytes per sample: 1, or 2 for 10-bit (the surface pitch fields stay in samples)
    // per frame in flight (slot = ticket % depth): input picture, picture setup, bitstream, status
    RmMem in[ENC_MAX_DEPTH], setup[ENC_MAX_DEPTH], bs[ENC_MAX_DEPTH], status[ENC_MAX_DEPTH];
    struct { bool idr; unsigned frame_num, poc; double t0; int slot, nRefs, refs[ENC_MAX_REFS]; } pend[ENC_MAX_DEPTH];
    RmMem recon, hist, coloc, mepred[2], sem, rcproc, rcdata;
    NvU32 seq;                      // frames submitted (semaphore payload of the last one)
    NvU32 waited;                   // frames waited for
    bool forceIdr;                  // after a failed frame
    NvU64 extInput;                 // != 0: GPU address of an input picture in NVENC's layout (luma, chroma at +lumaSz) to use instead of in[]
    unsigned frame_num, sinceIdr;   // frame_num of the next picture, pictures since the last IDR
    int slotOf[ENC_MAX_REFS];       // DPB slots of the reference frames, most recent first
    int nRefs;                      // valid entries in slotOf
    int nextSlot;
    RmEvent ev;                     // NVENC non-stall event (ev.h == 0: none, spin)
    // AV1: default probability tables, the tables the engine wrote, entropy / filter line buffers, assembled temporal units
    RmMem av1ProbIn, av1ProbOut, av1Line[6], av1Half;
    uint8_t *av1Tu[ENC_MAX_DEPTH]; size_t av1TuCap;
    int av1SbCols, av1SbRows;
} Enc;

int  enc_open(Enc *e, const EncConfig *cfg);                    // 0 = ok
void enc_close(Enc *e);
size_t enc_headers(Enc *e, uint8_t *out);                       // SPS + PPS (HEVC: VPS + SPS + PPS), Annex B (~64 / ~96 bytes)
// copy an NV12 frame (pitch-linear planes) into the next frame's input surface (needs enc_inflight() < depth)
void enc_load_nv12(Enc *e, const uint8_t *y, size_t yStride, const uint8_t *uv, size_t uvStride);
// copy a 16-bit 4:2:0 frame (P010-style: 10 bits in the high bits of each 16-bit sample) into a 10-bit session's input
void enc_load_p010(Enc *e, const uint16_t *y, size_t yStride, const uint16_t *uv, size_t uvStride);
// convert an l10r frame (ARGB2101010 LE, ScreenCaptureKit's 10-bit format) into a 10-bit session's input (BT.709 video range)
void enc_load_l10r(Enc *e, const uint8_t *src, size_t stride);
// convert a BGRA frame (BT.709 video range) into the next frame's input surface
void enc_load_bgra(Enc *e, const uint8_t *bgra, size_t stride);
int  enc_encode(Enc *e, bool force_idr, EncFrame *out);         // encode the loaded frame and wait; 0 = ok
int  enc_submit(Enc *e, bool force_idr, NvU32 *ticket);         // start encoding the loaded frame; 0 = ok
int  enc_wait(Enc *e, NvU32 ticket, EncFrame *out);             // wait for a submitted frame (in submission order); 0 = ok
static inline int enc_inflight(const Enc *e) { return (int)(e->seq - e->waited); }
