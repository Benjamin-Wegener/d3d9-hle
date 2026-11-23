/*
 * d3d9_format.c - D3D9 pixel-format geometry and conversion to BGRA8.
 *
 * The geometry and the block layouts are the public ones: S3TC/DXT blocks of
 * 4x4 texels (8 bytes for DXT1, 16 for DXT2-5, colour always as 16-bit
 * RGB565 endpoints), and the documented uncompressed/float layouts.
 * Nothing here is specific to an application or a DLL version; an unknown format is
 * reported as an error instead of being guessed.
 */
#include <string.h>
#include "d3d9_hle.h"
#include "d3d9_format.h"

/* ------------------------------------------------------------- geometry */

int d3d9_format_is_block_compressed(uint32_t format)
{
    return format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ||
           format == D3DFMT_DXT4 || format == D3DFMT_DXT5;
}

uint32_t d3d9_format_block_bytes(uint32_t format)
{
    switch (format) {
    case D3DFMT_DXT1: return 8;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5: return 16;
    default:          return 0;
    }
}

uint32_t d3d9_format_bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_A4L4:            return 1;
    case D3DFMT_A8L8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_V8U8:
    case D3DFMT_L16:
    case D3DFMT_R16F:            return 2;
    case D3DFMT_R8G8B8:          return 3;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_G16R16:
    case D3DFMT_G16R16F:
    case D3DFMT_R32F:            return 4;
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F:         return 8;
    case D3DFMT_A32B32G32R32F:   return 16;
    default:                     return 0;   /* block-compressed or unknown */
    }
}

int d3d9_format_is_supported(uint32_t format)
{
    return d3d9_format_block_bytes(format) != 0 || d3d9_format_bytes_per_pixel(format) != 0;
}

uint32_t d3d9_format_pitch(uint32_t format, uint32_t width)
{
    uint32_t block = d3d9_format_block_bytes(format);
    if (width == 0) return 0;
    if (block) return ((width + 3u) / 4u) * block;
    return d3d9_format_bytes_per_pixel(format) * width;  /* 0 when unknown */
}

uint32_t d3d9_format_level_size(uint32_t format, uint32_t width, uint32_t height)
{
    uint32_t pitch = d3d9_format_pitch(format, width);
    if (pitch == 0 || height == 0) return 0;
    if (d3d9_format_is_block_compressed(format))
        return pitch * ((height + 3u) / 4u);
    return pitch * height;
}

/* ------------------------------------------------------------ DXT decode */

static uint32_t bgra8(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }

/* RGB565 -> four BGRA8 palette entries. DXT1 may use the 3-colour mode with a
 * transparent black index 3; DXT3/DXT5 always use the four-colour mode. */
static void color_palette(const uint8_t *blk, int four_color_only, uint32_t pal[4])
{
    uint32_t c0 = (uint32_t)blk[0] | ((uint32_t)blk[1] << 8);
    uint32_t c1 = (uint32_t)blk[2] | ((uint32_t)blk[3] << 8);
    uint32_t r0 = expand5((c0 >> 11) & 0x1Fu), g0 = expand6((c0 >> 5) & 0x3Fu),
             b0 = expand5(c0 & 0x1Fu);
    uint32_t r1 = expand5((c1 >> 11) & 0x1Fu), g1 = expand6((c1 >> 5) & 0x3Fu),
             b1 = expand5(c1 & 0x1Fu);

    pal[0] = bgra8(r0, g0, b0, 255);
    pal[1] = bgra8(r1, g1, b1, 255);
    if (four_color_only || c0 > c1) {
        pal[2] = bgra8((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        pal[3] = bgra8((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        pal[2] = bgra8((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        pal[3] = 0;     /* transparent black */
    }
}

/* DXT5's eight interpolated alpha values. */
static void alpha_palette(uint32_t a0, uint32_t a1, uint8_t out[8])
{
    out[0] = (uint8_t)a0;
    out[1] = (uint8_t)a1;
    if (a0 > a1) {
        out[2] = (uint8_t)((6 * a0 + 1 * a1) / 7);
        out[3] = (uint8_t)((5 * a0 + 2 * a1) / 7);
        out[4] = (uint8_t)((4 * a0 + 3 * a1) / 7);
        out[5] = (uint8_t)((3 * a0 + 4 * a1) / 7);
        out[6] = (uint8_t)((2 * a0 + 5 * a1) / 7);
        out[7] = (uint8_t)((1 * a0 + 6 * a1) / 7);
    } else {
        out[2] = (uint8_t)((4 * a0 + 1 * a1) / 5);
        out[3] = (uint8_t)((3 * a0 + 2 * a1) / 5);
        out[4] = (uint8_t)((2 * a0 + 3 * a1) / 5);
        out[5] = (uint8_t)((1 * a0 + 4 * a1) / 5);
        out[6] = 0;
        out[7] = 255;
    }
}

static void put_bgra8(void *row, uint32_t x, uint32_t value)
{
    memcpy((uint8_t *)row + (size_t)x * 4u, &value, 4);
}

/* One block at src, clipped to the level's width/height. */
static void convert_block(uint32_t format, const uint8_t *blk,
                          void *dst, uint32_t dst_pitch,
                          uint32_t clip_w, uint32_t clip_h)
{
    uint32_t pal[4];
    uint8_t apal[16];   /* DXT4/5: per-pixel alpha, folded from the palette */
    uint32_t index_bits = 0;
    int is_dxt1 = format == D3DFMT_DXT1;
    int has_explicit_alpha = (format == D3DFMT_DXT2 || format == D3DFMT_DXT3);
    int has_interp_alpha = (format == D3DFMT_DXT4 || format == D3DFMT_DXT5);

    if (is_dxt1) {
        color_palette(blk, 0, pal);
        index_bits = (uint32_t)blk[4] | ((uint32_t)blk[5] << 8) |
                     ((uint32_t)blk[6] << 16) | ((uint32_t)blk[7] << 24);
    } else {
        color_palette(blk + 8, 1, pal);
        index_bits = (uint32_t)blk[12] | ((uint32_t)blk[13] << 8) |
                     ((uint32_t)blk[14] << 16) | ((uint32_t)blk[15] << 24);
        if (has_interp_alpha) {
            uint64_t bits = 0;
            uint8_t values[8];
            for (int i = 0; i < 6; i++) bits |= (uint64_t)blk[2 + i] << (8 * i);
            alpha_palette(blk[0], blk[1], values);
            for (uint32_t i = 0; i < 16; i++)
                apal[i] = values[(bits >> (3 * i)) & 7u];
        }
    }

    for (uint32_t py = 0; py < clip_h; py++) {
        uint8_t *row = (uint8_t *)dst + (size_t)py * dst_pitch;
        for (uint32_t px = 0; px < clip_w; px++) {
            uint32_t i = py * 4u + px;
            uint32_t value = pal[(index_bits >> (2 * i)) & 3u];
            if (has_explicit_alpha) {
                uint32_t nibble = ((uint32_t)blk[i >> 1] >> ((i & 1u) * 4u)) & 0xFu;
                value = (value & 0x00FFFFFFu) | (nibble * 17u << 24);
            } else if (has_interp_alpha) {
                value = (value & 0x00FFFFFFu) | ((uint32_t)apal[i] << 24);
            }
            put_bgra8(row, px, value);
        }
    }
}

static uint8_t clamp_u8(float f)
{
    if (f <= 0.0f) return 0;
    if (f >= 1.0f) return 255;
    return (uint8_t)(f * 255.0f + 0.5f);
}

static float f16_to_f32(uint16_t h)
{
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) { f = sign; }
        else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; exp--; }
            mant &= 0x3FFu;
            f = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000u | (mant << 13);
    } else {
        f = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
    }
    float out; memcpy(&out, &f, 4); return out;
}

static uint8_t f16_to_u8(uint16_t h) { return clamp_u8(f16_to_f32(h)); }
static uint8_t f32_to_u8(const uint8_t *p) { float f; memcpy(&f, p, 4); return clamp_u8(f); }

/* ------------------------------------------------------- uncompressed */

static uint32_t convert_pixel(uint32_t format, const uint8_t *p)
{
    switch (format) {
    case D3DFMT_A8R8G8B8: {
        uint32_t v; memcpy(&v, p, 4); return v;
    }
    case D3DFMT_X8R8G8B8: {
        uint32_t v; memcpy(&v, p, 4); return (v & 0x00FFFFFFu) | 0xFF000000u;
    }
    case D3DFMT_R5G6B5: {
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        return bgra8(expand5((v >> 11) & 0x1Fu), expand6((v >> 5) & 0x3Fu),
                     expand5(v & 0x1Fu), 255);
    }
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: {
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        uint32_t a = format == D3DFMT_A1R5G5B5 ? ((v >> 15) & 1u) * 255u : 255u;
        return bgra8(expand5((v >> 10) & 0x1Fu), expand5((v >> 5) & 0x1Fu),
                     expand5(v & 0x1Fu), a);
    }
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A4R4G4B4: {
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        uint32_t a = format == D3DFMT_A4R4G4B4 ? ((v >> 12) & 0xFu) * 17u : 255u;
        return bgra8(((v >> 8) & 0xFu) * 17u, ((v >> 4) & 0xFu) * 17u,
                     (v & 0xFu) * 17u, a);
    }
    case D3DFMT_A8:  return bgra8(0, 0, 0, p[0]);
    case D3DFMT_L8:  return bgra8(p[0], p[0], p[0], 255);
    case D3DFMT_A8L8: return bgra8(p[0], p[0], p[0], p[1]);
    case D3DFMT_A4L4: {
        uint32_t l = (p[0] & 0xFu) * 17u, a = ((p[0] >> 4) & 0xFu) * 17u;
        return bgra8(l, l, l, a);
    }
    case D3DFMT_L16: {
        uint32_t l = ((uint32_t)p[0] | ((uint32_t)p[1] << 8)) >> 8; /* top 8 bits */
        return bgra8(l, l, l, 255);
    }
    case D3DFMT_R8G8B8: return bgra8(p[2], p[1], p[0], 255);
    case D3DFMT_A2R10G10B10: {
        uint32_t v; memcpy(&v, p, 4);
        uint32_t r = ((v >> 20) & 0x3FFu) * 255u / 1023u;
        uint32_t g = ((v >> 10) & 0x3FFu) * 255u / 1023u;
        uint32_t b = (v & 0x3FFu) * 255u / 1023u;
        uint32_t a = ((v >> 30) & 0x3u) * 85u; /* 0,85,170,255 */
        return bgra8(r, g, b, a);
    }
    case D3DFMT_G16R16: {
        uint32_t r = ((uint32_t)p[0] | ((uint32_t)p[1] << 8)) >> 8;
        uint32_t g = ((uint32_t)p[2] | ((uint32_t)p[3] << 8)) >> 8;
        return bgra8(r, g, 0, 255);
    }
    case D3DFMT_A16B16G16R16: {
        uint32_t r = ((uint32_t)p[0] | ((uint32_t)p[1] << 8)) >> 8;
        uint32_t g = ((uint32_t)p[2] | ((uint32_t)p[3] << 8)) >> 8;
        uint32_t b = ((uint32_t)p[4] | ((uint32_t)p[5] << 8)) >> 8;
        uint32_t a = ((uint32_t)p[6] | ((uint32_t)p[7] << 8)) >> 8;
        return bgra8(r, g, b, a);
    }
    case D3DFMT_V8U8: {
        /* signed bytes per D3D9 doc: U at byte 0, V at byte 1. Map [-128,127] to [0,255]. */
        uint32_t u = (uint32_t)(uint8_t)((int8_t)p[0] + 128);
        uint32_t v = (uint32_t)(uint8_t)((int8_t)p[1] + 128);
        return bgra8(u, v, 0, 255);
    }
    case D3DFMT_R16F:
        return bgra8(f16_to_u8((uint16_t)(p[0] | (p[1] << 8))), 0, 0, 255);
    case D3DFMT_G16R16F:
        return bgra8(f16_to_u8((uint16_t)(p[0] | (p[1] << 8))),
                     f16_to_u8((uint16_t)(p[2] | (p[3] << 8))), 0, 255);
    case D3DFMT_A16B16G16R16F:
        return bgra8(f16_to_u8((uint16_t)(p[0] | (p[1] << 8))),
                     f16_to_u8((uint16_t)(p[2] | (p[3] << 8))),
                     f16_to_u8((uint16_t)(p[4] | (p[5] << 8))),
                     f16_to_u8((uint16_t)(p[6] | (p[7] << 8))));
    case D3DFMT_R32F:
        return bgra8(f32_to_u8(p), 0, 0, 255);
    case D3DFMT_G32R32F:
        return bgra8(f32_to_u8(p), f32_to_u8(p + 4), 0, 255);
    case D3DFMT_A32B32G32R32F:
        return bgra8(f32_to_u8(p), f32_to_u8(p + 4), f32_to_u8(p + 8), f32_to_u8(p + 12));
    default:         return 0;   /* callers reject unknown formats first */
    }
}

/* ------------------------------------------------------------- convert */

int d3d9_format_to_bgra8(uint32_t format, const void *src, uint32_t src_pitch,
                         uint32_t width, uint32_t height,
                         void *dst, uint32_t dst_pitch)
{
    uint32_t packed_src, packed_dst;

    if (!src || !dst || width == 0 || height == 0) return -1;

    packed_src = d3d9_format_pitch(format, width);
    if (packed_src == 0) return -1; /* unknown/unsupported */
    packed_dst = width * 4u;
    if (src_pitch == 0) src_pitch = packed_src;
    if (dst_pitch == 0) dst_pitch = packed_dst;
    if (src_pitch < packed_src || dst_pitch < packed_dst) return -1;

    if (d3d9_format_is_block_compressed(format)) {
        uint32_t block = d3d9_format_block_bytes(format);
        uint32_t bw = (width + 3u) / 4u, bh = (height + 3u) / 4u;
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t *blk = (const uint8_t *)src +
                                     (size_t)by * src_pitch + (size_t)bx * block;
                uint32_t clip_w = width - bx * 4u, clip_h = height - by * 4u;
                void *out = (uint8_t *)dst + (size_t)(by * 4u) * dst_pitch +
                            (size_t)(bx * 4u) * 4u;
                if (clip_w > 4u) clip_w = 4u;
                if (clip_h > 4u) clip_h = 4u;
                convert_block(format, blk, out, dst_pitch, clip_w, clip_h);
            }
        }
        return 0;
    }

    {
        uint32_t bpp = d3d9_format_bytes_per_pixel(format);
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t *srow = (const uint8_t *)src + (size_t)y * src_pitch;
            uint8_t *drow = (uint8_t *)dst + (size_t)y * dst_pitch;
            for (uint32_t x = 0; x < width; x++)
                put_bgra8(drow, x, convert_pixel(format, srow + (size_t)x * bpp));
        }
    }
    return 0;
}
