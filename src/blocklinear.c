// blocklinear.c: see blocklinear.h. Inside a GOB: 2 column halves of 32 bytes (256 bytes each), 4 row pairs of 64
// bytes, 2 column quarters of 16 bytes (32 bytes each), 2 rows of 16 bytes.
#include "blocklinear.h"
#include <string.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <emmintrin.h>
#include <Accelerate/Accelerate.h>

size_t bl_offset(size_t x, size_t y, size_t pitch, unsigned bh)
{
    size_t gob = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
    size_t blockRows = (size_t)8 << bh, blocksPerRow = pitch / 64;
    size_t block = (y / blockRows) * blocksPerRow + x / 64;
    return block * ((size_t)512 << bh) + ((y / 8) % ((size_t)1 << bh)) * 512 + gob;
}

size_t bl_plane_size(size_t pitch, size_t rows, unsigned bh)
{
    size_t blockRows = (size_t)8 << bh;
    return pitch * ((rows + blockRows - 1) / blockRows) * blockRows;
}

void bl_tile_plane(uint8_t *dst, const uint8_t *src, size_t src_stride, size_t width, size_t rows, size_t pitch, unsigned bh)
{
    // 16-byte runs are contiguous in a GOB: copy those
    for (size_t y = 0; y < rows; y++)
        for (size_t x = 0; x < width; x += 16) {
            size_t n = width - x < 16 ? width - x : 16;
            memcpy(dst + bl_offset(x, y, pitch, bh), src + y * src_stride + x, n);
        }
}

size_t nvenc_in_offset(size_t x, size_t y, size_t pitch)
{
    size_t in = (x % 16) + (y % 4) * 16 + ((x / 16) % 2) * 64 + ((y / 4) % 2) * 128 + ((x / 32) % 2) * 256 + ((y / 8) % 2) * 512;
    return (y / 16) * pitch * 16 + (x / 64) * 1024 + in;
}

size_t nvenc_in_plane_size(size_t pitch, size_t rows) { return pitch * ((rows + 15) / 16) * 16; }

static void tile_band_slow(uint8_t *dst, const uint8_t *src, size_t src_stride, size_t x0, size_t width, size_t y0, size_t y1, size_t pitch)
{
    for (size_t y = y0; y < y1; y++)
        for (size_t x = x0; x < width; x += 16) {
            size_t n = width - x < 16 ? width - x : 16;
            memcpy(dst + nvenc_in_offset(x, y, pitch), src + y * src_stride + x, n);
        }
}

// one band of 16 rows: full 64-byte tiles with SSE (non-temporal stores, in destination order), the rest generic
static void tile_band(uint8_t *dst, const uint8_t *src, size_t src_stride, size_t width, size_t band, size_t rows, size_t pitch)
{
    size_t y0 = band * 16, y1 = y0 + 16 < rows ? y0 + 16 : rows;
    if (y1 - y0 < 16) { tile_band_slow(dst, src, src_stride, 0, width, y0, y1, pitch); return; }
    size_t full = width / 64;
    const uint8_t *s0 = src + y0 * src_stride;
    for (size_t t = 0; t < full; t++) {
        __m128i *d = (__m128i *)(dst + band * pitch * 16 + t * 1024);
        const uint8_t *s = s0 + t * 64;
        // destination order: y8, x32, y4, x16, y%4  (offset = 16*(y%4) + 64*x16 + 128*y4 + 256*x32 + 512*y8)
        for (int y8 = 0; y8 < 2; y8++)
            for (int x32 = 0; x32 < 2; x32++)
                for (int y4 = 0; y4 < 2; y4++)
                    for (int x16 = 0; x16 < 2; x16++) {
                        const uint8_t *r = s + (size_t)(y8 * 8 + y4 * 4) * src_stride + x32 * 32 + x16 * 16;
                        _mm_stream_si128(d++, _mm_loadu_si128((const __m128i *)r));
                        _mm_stream_si128(d++, _mm_loadu_si128((const __m128i *)(r + src_stride)));
                        _mm_stream_si128(d++, _mm_loadu_si128((const __m128i *)(r + 2 * src_stride)));
                        _mm_stream_si128(d++, _mm_loadu_si128((const __m128i *)(r + 3 * src_stride)));
                    }
    }
    if (full * 64 < width) tile_band_slow(dst, src, src_stride, full * 64, width, y0, y1, pitch);
}

void nvenc_in_tile_plane(uint8_t *dst, const uint8_t *src, size_t src_stride, size_t width, size_t rows, size_t pitch)
{
    size_t bands = (rows + 15) / 16;
    if ((uintptr_t)dst & 15) { tile_band_slow(dst, src, src_stride, 0, width, 0, rows, pitch); return; }
    if (bands >= 32) {   // big planes: two halves in parallel (4K: 2 chunks 0.65 ms, 4: 0.68, 8: 0.79, 16: 1.03; memory bound)
        size_t chunks = 2, per = (bands + chunks - 1) / chunks;
        dispatch_apply(chunks, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t k) {
            for (size_t b = k * per; b < (k + 1) * per && b < bands; b++) tile_band(dst, src, src_stride, width, b, rows, pitch); });
    } else
        for (size_t b = 0; b < bands; b++) tile_band(dst, src, src_stride, width, b, rows, pitch);
    _mm_sfence();
}

// BGRA -> NV12 (BT.709 video range) with Accelerate's vImage, one band at a time into a cache-sized buffer
static vImage_ARGBToYpCbCr gBgraInfo;
static void bgra_info_init(void)
{
    vImage_YpCbCrPixelRange range = { 16, 128, 235, 240, 235, 16, 240, 16 };   // video range
    vImageConvert_ARGBToYpCbCr_GenerateConversion(kvImage_ARGBToYpCbCrMatrix_ITU_R_709_2, &range, &gBgraInfo,
                                                  kvImageARGB8888, kvImage420Yp8_CbCr8, kvImageNoFlags);
}

void nvenc_in_tile_bgra(uint8_t *dst_luma, uint8_t *dst_chroma, const uint8_t *src, size_t src_stride, size_t width, size_t rows, size_t pitch)
{
    static dispatch_once_t once; dispatch_once(&once, ^{ bgra_info_init(); });
    // super-bands of 32 luma rows = 2 luma tile rows + 1 chroma tile row: convert into a buffer (in cache), then tile
    size_t sbands = (rows + 31) / 32, bw = (((width + 1) & ~(size_t)1) + 63) & ~(size_t)63, crows = (rows + 1) / 2;
    size_t chunks = sbands >= 16 ? 2 : 1, per = (sbands + chunks - 1) / chunks;
    dispatch_apply(chunks, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t k) {
        uint8_t *buf = aligned_alloc(64, bw * 48);   // 32 luma rows + 16 chroma rows (bw is a multiple of 64)
        if (!buf) return;
        uint8_t *uvb = buf + bw * 32;
        const uint8_t permute[4] = { 3, 2, 1, 0 };   // BGRA in memory -> ARGB order for vImage
        for (size_t sb = k * per; sb < (k + 1) * per && sb < sbands; sb++) {
            size_t r0 = sb * 32, n = (r0 + 32 < rows ? 32 : rows - r0) & ~(size_t)1;
            if (!n) n = 2;   // a last single row: convert it twice (vImage needs even sizes)
            vImage_Buffer in = { (void *)(src + r0 * src_stride), n, width & ~(size_t)1, src_stride };
            vImage_Buffer y = { buf, n, width & ~(size_t)1, bw }, uv = { uvb, n / 2, (width & ~(size_t)1) / 2, bw };
            vImageConvert_ARGB8888To420Yp8_CbCr8(&in, &y, &uv, &gBgraInfo, permute, kvImageDoNotTile);
            // tile_band reads rows band*16.. of its source: point the source so that row r0 is the buffer's first row
            for (size_t b = 2 * sb; b < 2 * sb + 2 && b * 16 < rows; b++) tile_band(dst_luma, buf - r0 * bw, bw, width, b, rows, pitch);
            tile_band(dst_chroma, uvb - (r0 / 2) * bw, bw, width, sb, crows, pitch);
        }
        free(buf);
    });
    _mm_sfence();
}
