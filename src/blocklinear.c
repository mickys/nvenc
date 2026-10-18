// blocklinear.c: see blocklinear.h. Inside a GOB: 2 column halves of 32 bytes (256 bytes each), 4 row pairs of 64
// bytes, 2 column quarters of 16 bytes (32 bytes each), 2 rows of 16 bytes.
#include "blocklinear.h"
#include <string.h>

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

void nvenc_in_tile_plane(uint8_t *dst, const uint8_t *src, size_t src_stride, size_t width, size_t rows, size_t pitch)
{
    for (size_t y = 0; y < rows; y++)
        for (size_t x = 0; x < width; x += 16) {
            size_t n = width - x < 16 ? width - x : 16;
            memcpy(dst + nvenc_in_offset(x, y, pitch), src + y * src_stride + x, n);
        }
}
