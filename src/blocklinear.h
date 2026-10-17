// blocklinear.h: NVIDIA block-linear surface layout (Fermi and later, "GPU BL"): 64-byte x 8-row GOBs of 512 bytes;
// a block is one GOB wide and 2^block_height GOBs tall; blocks are stored row by row.
#pragma once
#include <stddef.h>
#include <stdint.h>

// byte offset of (x bytes, y rows) in a surface whose rows are pitch bytes (multiple of 64)
size_t bl_offset(size_t x, size_t y, size_t pitch, unsigned block_height_log2);
// size of a plane (rows rounded up to the block height)
size_t bl_plane_size(size_t pitch, size_t rows, unsigned block_height_log2);
// copy a pitch-linear plane (src_stride) of width x rows bytes into block-linear dst
void bl_tile_plane(uint8_t *dst, const uint8_t *src, size_t src_stride, size_t width, size_t rows, size_t pitch, unsigned bh);
