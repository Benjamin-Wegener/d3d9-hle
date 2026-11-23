/*
 * d3d9_format.h - D3D9 pixel-format geometry and conversion to BGRA8.
 *
 * Layout follows the public D3D9 rules: a block-compressed surface is stored
 * as 4x4 blocks, ceil(width/4) blocks per row; every other supported format
 * as bytes_per_pixel * width per row. Widths and heights that are not a
 * multiple of four are padded to whole blocks on disk and clipped when they
 * are converted, so no pixel outside width x height is ever written.
 *
 * An unknown or unsupported format is an error, never a silent default:
 * pitch/level_size return 0 and to_bgra8 returns -1. DXT2/DXT4 are decoded
 * like DXT3/DXT5 (premultiplied alpha is preserved as stored).
 *
 * "BGRA8" is the D3DFMT_A8R8G8B8 memory order, B in the lowest byte; read as
 * a little-endian uint32_t it is 0xAARRGGBB, the value D3D9 colour arguments
 * use.
 */
#ifndef WIN32HLE_D3D9_FORMAT_H
#define WIN32HLE_D3D9_FORMAT_H

#include <stdint.h>

/* 1 when every function below supports the format. */
int d3d9_format_is_supported(uint32_t format);

/* 1 for DXT1/DXT3/DXT5, whose blocks are 8 or 16 bytes (see block_bytes). */
int d3d9_format_is_block_compressed(uint32_t format);

/* Bytes per 4x4 block for a block-compressed format, 0 otherwise. */
uint32_t d3d9_format_block_bytes(uint32_t format);

/* Bytes per pixel for an uncompressed format, 0 for block/unknown formats. */
uint32_t d3d9_format_bytes_per_pixel(uint32_t format);

/* Byte distance between two rows (for block formats: between block rows).
 * 0 for an unknown/unsupported format or width 0. */
uint32_t d3d9_format_pitch(uint32_t format, uint32_t width);

/* Bytes in one mip level: pitch * height, or for block formats
 * ceil(height/4) block rows. 0 for an unknown/unsupported format or size 0. */
uint32_t d3d9_format_level_size(uint32_t format, uint32_t width, uint32_t height);

/* Convert one level to tight or pitched BGRA8. A pitch of 0 means "tightly
 * packed" (the value the geometry functions return). Returns 0 on success and
 * -1 for an unknown format, a null pointer, a zero size, or a pitch smaller
 * than the packed row. On failure dst is not written. */
int d3d9_format_to_bgra8(uint32_t format, const void *src, uint32_t src_pitch,
                         uint32_t width, uint32_t height,
                         void *dst, uint32_t dst_pitch);

#endif /* WIN32HLE_D3D9_FORMAT_H */
