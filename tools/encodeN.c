// encodeN.c (milestone M3): encode N frames of moving synthetic NV12 content through src/enc.c (IDR + P, constant QP).
//   ./encodeN [-v] W H N QP_I QP_P REFS out.h264 out.nv12
// Environment: IDR_PERIOD=<n> (an IDR every n frames), RC=<RCMODE> BITRATE=<bit/s> MAXRATE=<bit/s> VBV=<bits>,
// SLICES=<n>.
// Writes the Annex B stream (SPS + PPS once, then every picture) and all source frames for h264check.
#include "enc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void frame(uint8_t *y, uint8_t *uv, int W, int H, int f)
{
    // moving diagonal gradient + 64 px checkerboard scrolling right + a sweeping bar
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            int v = ((c + r + f * 8) & 255) / 2 + (((((c - f * 4) >> 6) ^ (r >> 6)) & 1) ? 90 : 0);
            if (abs(c - (f * 24) % W) < 16) v = 235;
            y[r * W + c] = (uint8_t)(16 + v * 219 / 255);
        }
    for (int r = 0; r < (H + 1) / 2; r++)
        for (int c = 0; c < W / 2; c++) {
            uv[r * W + 2 * c] = (uint8_t)(128 + 60 * sin((c + f) * 0.02));
            uv[r * W + 2 * c + 1] = (uint8_t)(128 + 60 * cos((r + f) * 0.03));
        }
}

int main(int argc, char **argv)
{
    int ai = 1; bool verbose = false;
    if (argc > 1 && !strcmp(argv[1], "-v")) { verbose = true; ai++; }
    if (argc - ai < 8) { fprintf(stderr, "usage: encodeN [-v] W H N QP_I QP_P REFS out.h264 out.nv12\n"); return 2; }
    int W = atoi(argv[ai]), H = atoi(argv[ai + 1]), N = atoi(argv[ai + 2]), CH = (H + 1) / 2;
    EncConfig cfg = { .width = W, .height = H, .qp_i = atoi(argv[ai + 3]), .qp_p = atoi(argv[ai + 4]), .num_refs = atoi(argv[ai + 5]),
                      .idr_period = getenv("IDR_PERIOD") ? atoi(getenv("IDR_PERIOD")) : 0, .verbose = verbose,
                      .rc_mode = getenv("RC") ? atoi(getenv("RC")) : 0, .bitrate = getenv("BITRATE") ? atoi(getenv("BITRATE")) : 0,
                      .max_bitrate = getenv("MAXRATE") ? atoi(getenv("MAXRATE")) : 0, .vbv_bits = getenv("VBV") ? atoi(getenv("VBV")) : 0,
                      .slices = getenv("SLICES") ? atoi(getenv("SLICES")) : 1 };
    Enc e; if (enc_open(&e, &cfg)) return 1;
    FILE *fh = fopen(argv[ai + 6], "wb"), *fs = fopen(argv[ai + 7], "wb");
    uint8_t hdr[256]; fwrite(hdr, 1, enc_headers(&e, hdr), fh);
    uint8_t *y = malloc((size_t)W * H), *uv = malloc((size_t)W * CH);
    double bits = 0, ms = 0, msMax = 0, bitsSteady = 0, pMax = 0; int fails = 0, nSteady = 0, qpMin = 99, qpMax = 0;
    for (int f = 0; f < N; f++) {
        frame(y, uv, W, H, f);
        fwrite(y, 1, (size_t)W * H, fs); fwrite(uv, 1, (size_t)W * CH, fs);
        enc_load_nv12(&e, y, W, uv, W);
        EncFrame fr;
        if (enc_encode(&e, false, &fr)) { printf("frame %d FAILED (ucode error %u)\n", f, fr.ucode_error); fails++; break; }
        fwrite(fr.data, 1, fr.size, fh);
        bits += fr.bits; ms += fr.ms; if (fr.ms > msMax) msMax = fr.ms;
        if (f >= 10) { bitsSteady += fr.bits; nSteady++; if (!fr.idr && fr.bits > pMax) pMax = fr.bits;
                       if ((int)fr.avg_qp < qpMin) qpMin = (int)fr.avg_qp; if ((int)fr.avg_qp > qpMax) qpMax = (int)fr.avg_qp; }
        if (verbose || f < 4 || f == N - 1)
            printf("frame %3d %s frame_num %3u poc %3u: %8u bits, %.3f ms, avgQP %u, intra %u inter %u MBs\n", f, fr.idr ? "IDR" : "P  ",
                   fr.frame_num, fr.poc, fr.bits, fr.ms, fr.avg_qp, fr.intra_mbs, fr.inter_mbs);
    }
    fclose(fh); fclose(fs);
    printf("%d frames %dx%d: %.1f kbit/frame, %.2f Mbit/s at 60 fps, encode %.3f ms avg %.3f ms max, %d failed\n", N, W, H,
           bits / N / 1000, bits / N * 60 / 1e6, ms / N, msMax, fails);
    if (nSteady) printf("frames 10..%d: %.2f Mbit/s at 60 fps (%.1f kbit/frame), largest P %.1f kbit, avgQP %d..%d\n", N - 1,
                        bitsSteady / nSteady * 60 / 1e6, bitsSteady / nSteady / 1000, pMax / 1000, qpMin, qpMax);
    enc_close(&e);
    return fails ? 1 : 0;
}
