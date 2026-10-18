// enc.h: an H.264 encode session on one NVENC engine: buffers, channel, picture setup per frame, IDR and P pictures
// with constant QP. Synchronous: enc_encode() returns when the engine is done. Structures: NVIDIA open-gpu-doc
// nvenc_drv.h / clc9b7.h; what the docs leave out is in docs/design.md ("M2 notes", "M3 notes").
#pragma once
#include "channel.h"
#include <stdint.h>

#define ENC_MAX_REFS 3

typedef struct {
    int width, height;              // display size; coded size is whole macroblocks
    int qp_i, qp_p;                 // constant QP for I/IDR and P pictures
    int num_refs;                   // L0 reference frames, 1..ENC_MAX_REFS
    int engine;                     // 0 = NVENC0, 1 = NVENC1
    int idr_period;                 // 0 = only the first picture (and enc_encode(force_idr))
    bool verbose;
} EncConfig;

typedef struct {
    const uint8_t *data;            // Annex B slice NAL(s) inside the session's bitstream buffer (valid until the next encode)
    size_t size;
    bool idr;
    unsigned frame_num, poc;
    unsigned bits, ucode_error, avg_qp, intra_mbs, inter_mbs;
    double ms;                      // doorbell to semaphore
} EncFrame;

typedef struct {
    EncConfig cfg;
    Rm rm;
    Chan chan;
    int wmb, hmb;
    size_t pitch, lumaSz, chromaSz; // input picture (NVENC input layout)
    size_t rPitch, rLuma, rSlot;    // reconstructed pictures: one slot per DPB entry
    RmMem in, recon, setup, bs, status, hist, coloc, mepred[2], sem;
    NvU32 seq;                      // frames submitted (semaphore payload)
    unsigned frame_num, sinceIdr;   // frame_num of the next picture, pictures since the last IDR
    int slotOf[ENC_MAX_REFS];       // DPB slots of the reference frames, most recent first
    int nRefs;                      // valid entries in slotOf
    int nextSlot;
} Enc;

int  enc_open(Enc *e, const EncConfig *cfg);                    // 0 = ok
void enc_close(Enc *e);
size_t enc_headers(Enc *e, uint8_t *out);                       // SPS + PPS, Annex B (~64 bytes)
// copy an NV12 frame (pitch-linear planes) into the input surface
void enc_load_nv12(Enc *e, const uint8_t *y, size_t yStride, const uint8_t *uv, size_t uvStride);
int  enc_encode(Enc *e, bool force_idr, EncFrame *out);         // encode the loaded frame; 0 = ok
