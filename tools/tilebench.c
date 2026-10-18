// tilebench.c: per-frame cost of the CPU side vs the engine at a given size (M5, T5.01): enc_load_nv12 (tiling into the
// input surface) and enc_encode (doorbell to semaphore), averaged over N frames.
//   ./tilebench W H N
#include "enc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: tilebench W H N\n"); return 2; }
    int W = atoi(argv[1]), H = atoi(argv[2]), N = atoi(argv[3]);
    uint8_t *y = malloc((size_t)W * H), *uv = malloc((size_t)W * H / 2);
    for (size_t i = 0; i < (size_t)W * H; i++) y[i] = (uint8_t)(i * 7 >> 3);
    for (size_t i = 0; i < (size_t)W * H / 2; i++) uv[i] = (uint8_t)(i * 5 >> 2);
    Enc e; EncConfig c = { .width = W, .height = H, .qp_i = 26, .qp_p = 28, .num_refs = 1 };
    if (enc_open(&e, &c)) return 1;
    double tl = 0, te = 0, tlMax = 0;
    for (int f = 0; f < N; f++) {
        y[f] ^= 0xff;
        double t0 = now_ms(); enc_load_nv12(&e, y, W, uv, W); double t1 = now_ms();
        EncFrame fr; if (enc_encode(&e, false, &fr)) { printf("encode failed\n"); return 1; }
        double t2 = now_ms();
        if (f >= 2) { tl += t1 - t0; te += t2 - t1; if (t1 - t0 > tlMax) tlMax = t1 - t0; }
    }
    int n = N - 2;
    printf("%dx%d: tiling %.3f ms avg (max %.3f), encode %.3f ms avg, total %.3f ms per frame\n", W, H, tl / n, tlMax, te / n, (tl + te) / n);
    enc_close(&e);
    return 0;
}
