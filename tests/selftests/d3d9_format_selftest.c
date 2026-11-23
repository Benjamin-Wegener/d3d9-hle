/*
 * d3d9_format_selftest.c - fixed vectors for d3d9_format: pitch and mip-level
 * sizes, the block layouts of DXT1/3/5, the 16-bit formats, A8, L8, A8L8,
 * A8R8G8B8 and X8R8G8B8, clipping of padded blocks (< 4 and odd sizes), and
 * unknown formats as errors. Expected values are worked out from the public
 * format definitions, not from this implementation.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "d3d9_hle.h"
#include "d3d9_format.h"

/* No guest code: the layer links against an empty dispatch table. */
const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void check(const char *what, int cond)
{
    if (!cond) { printf("FAIL %s\n", what); fails++; }
}

#define PITCH(fmt, w, want) \
    check("pitch " #fmt " w=" #w, d3d9_format_pitch(fmt, w) == (uint32_t)(want))
#define LEVEL(fmt, w, h, want) \
    check("level " #fmt " " #w "x" #h, d3d9_format_level_size(fmt, w, h) == (uint32_t)(want))

static uint32_t px(const void *buf, uint32_t pitch, uint32_t x, uint32_t y)
{
    uint32_t v;
    memcpy(&v, (const uint8_t *)buf + (size_t)y * pitch + (size_t)x * 4, 4);
    return v;
}

static void dxt1_block(uint8_t b[8], uint16_t c0, uint16_t c1, uint32_t indices)
{
    b[0] = (uint8_t)c0; b[1] = (uint8_t)(c0 >> 8);
    b[2] = (uint8_t)c1; b[3] = (uint8_t)(c1 >> 8);
    b[4] = (uint8_t)indices; b[5] = (uint8_t)(indices >> 8);
    b[6] = (uint8_t)(indices >> 16); b[7] = (uint8_t)(indices >> 24);
}

#define SENTINEL 0xCDCDCDCDu
#define EXPECT(tag, x, y, want) do { \
    uint32_t _v = px(out, 16, (x), (y)); \
    if (_v != (uint32_t)(want)) { \
        printf("FAIL %s (%u,%u) got 0x%08X want 0x%08X\n", (tag), (x), (y), _v, (uint32_t)(want)); \
        fails++; \
    } \
} while (0)

static void test_geometry(void)
{
    check("DXT1 supported", d3d9_format_is_supported(D3DFMT_DXT1));
    check("DXT2 supported", d3d9_format_is_supported(D3DFMT_DXT2));
    check("DXT4 supported", d3d9_format_is_supported(D3DFMT_DXT4));
    check("X8R8G8B8 supported", d3d9_format_is_supported(D3DFMT_X8R8G8B8));
    check("R16F supported", d3d9_format_is_supported(D3DFMT_R16F));
    check("DXT1 block", d3d9_format_block_bytes(D3DFMT_DXT1) == 8);
    check("DXT2 block", d3d9_format_block_bytes(D3DFMT_DXT2) == 16);
    check("DXT3 block", d3d9_format_block_bytes(D3DFMT_DXT3) == 16);
    check("DXT4 block", d3d9_format_block_bytes(D3DFMT_DXT4) == 16);
    check("DXT5 block", d3d9_format_block_bytes(D3DFMT_DXT5) == 16);
    check("A8R8G8B8 bpp", d3d9_format_bytes_per_pixel(D3DFMT_A8R8G8B8) == 4);
    check("A8L8 bpp", d3d9_format_bytes_per_pixel(D3DFMT_A8L8) == 2);
    check("L8 bpp", d3d9_format_bytes_per_pixel(D3DFMT_L8) == 1);
    check("A4L4 bpp", d3d9_format_bytes_per_pixel(D3DFMT_A4L4) == 1);
    check("L16 bpp", d3d9_format_bytes_per_pixel(D3DFMT_L16) == 2);
    check("V8U8 bpp", d3d9_format_bytes_per_pixel(D3DFMT_V8U8) == 2);
    check("R8G8B8 bpp", d3d9_format_bytes_per_pixel(D3DFMT_R8G8B8) == 3);
    check("A2R10G10B10 bpp", d3d9_format_bytes_per_pixel(D3DFMT_A2R10G10B10) == 4);
    check("G16R16 bpp", d3d9_format_bytes_per_pixel(D3DFMT_G16R16) == 4);
    check("A16B16G16R16 bpp", d3d9_format_bytes_per_pixel(D3DFMT_A16B16G16R16) == 8);
    check("R16F bpp", d3d9_format_bytes_per_pixel(D3DFMT_R16F) == 2);
    check("G16R16F bpp", d3d9_format_bytes_per_pixel(D3DFMT_G16R16F) == 4);
    check("A16B16G16R16F bpp", d3d9_format_bytes_per_pixel(D3DFMT_A16B16G16R16F) == 8);
    check("R32F bpp", d3d9_format_bytes_per_pixel(D3DFMT_R32F) == 4);
    check("G32R32F bpp", d3d9_format_bytes_per_pixel(D3DFMT_G32R32F) == 8);
    check("A32B32G32R32F bpp", d3d9_format_bytes_per_pixel(D3DFMT_A32B32G32R32F) == 16);
    check("DXT1 not bpp", d3d9_format_bytes_per_pixel(D3DFMT_DXT1) == 0);

    PITCH(D3DFMT_DXT1, 1, 8);   PITCH(D3DFMT_DXT1, 3, 8);
    PITCH(D3DFMT_DXT1, 4, 8);   PITCH(D3DFMT_DXT1, 5, 16);
    PITCH(D3DFMT_DXT1, 7, 16);  PITCH(D3DFMT_DXT1, 8, 16);
    PITCH(D3DFMT_DXT1, 9, 24);
    PITCH(D3DFMT_DXT3, 1, 16);  PITCH(D3DFMT_DXT3, 4, 16);
    PITCH(D3DFMT_DXT3, 5, 32);  PITCH(D3DFMT_DXT5, 9, 48);
    PITCH(D3DFMT_R5G6B5, 3, 6); PITCH(D3DFMT_R5G6B5, 5, 10);
    PITCH(D3DFMT_A1R5G5B5, 1, 2); PITCH(D3DFMT_A4R4G4B4, 2, 4);
    PITCH(D3DFMT_A8L8, 7, 14);  PITCH(D3DFMT_A8, 9, 9);
    PITCH(D3DFMT_L8, 1, 1);     PITCH(D3DFMT_X8R8G8B8, 3, 12);
    PITCH(D3DFMT_DXT1, 0, 0);

    LEVEL(D3DFMT_DXT1, 1, 1, 8);    LEVEL(D3DFMT_DXT1, 5, 3, 16);
    LEVEL(D3DFMT_DXT1, 8, 8, 32);   LEVEL(D3DFMT_DXT3, 3, 3, 16);
    LEVEL(D3DFMT_DXT5, 9, 5, 96);   LEVEL(D3DFMT_R5G6B5, 3, 5, 30);
    LEVEL(D3DFMT_A8L8, 1, 9, 18);   LEVEL(D3DFMT_A8, 7, 3, 21);
    LEVEL(D3DFMT_X8R8G8B8, 2, 2, 16);
    LEVEL(D3DFMT_DXT1, 4, 0, 0);

    /* new formats: pitch and odd sizes */
    PITCH(D3DFMT_R8G8B8, 1, 3);  PITCH(D3DFMT_R8G8B8, 3, 9);
    PITCH(D3DFMT_A2R10G10B10, 1, 4); PITCH(D3DFMT_A2R10G10B10, 3, 12);
    PITCH(D3DFMT_G16R16, 3, 12); PITCH(D3DFMT_A16B16G16R16, 3, 24);
    PITCH(D3DFMT_V8U8, 5, 10); PITCH(D3DFMT_L16, 3, 6);
    PITCH(D3DFMT_A4L4, 7, 7);
    PITCH(D3DFMT_R16F, 3, 6); PITCH(D3DFMT_G16R16F, 3, 12);
    PITCH(D3DFMT_A16B16G16R16F, 1, 8); PITCH(D3DFMT_R32F, 3, 12);
    PITCH(D3DFMT_G32R32F, 1, 8); PITCH(D3DFMT_A32B32G32R32F, 1, 16);
    LEVEL(D3DFMT_R8G8B8, 3, 5, 45);
    LEVEL(D3DFMT_A2R10G10B10, 3, 2, 24);
    LEVEL(D3DFMT_G16R16, 1, 3, 12);
    LEVEL(D3DFMT_A16B16G16R16, 2, 1, 16);
    LEVEL(D3DFMT_R16F, 3, 3, 18);
    LEVEL(D3DFMT_A32B32G32R32F, 2, 2, 64);
    PITCH(D3DFMT_DXT2, 5, 32); LEVEL(D3DFMT_DXT2, 5, 3, 32);
    PITCH(D3DFMT_DXT4, 5, 32); LEVEL(D3DFMT_DXT4, 5, 3, 32);
}

static void test_dxt1(void)
{
    uint8_t blk[8];
    uint32_t out[16];

    /* 0xF800 (red) > 0x07E0 (green): four-colour mode, indices 0,1,2,3. */
    dxt1_block(blk, 0xF800, 0x07E0, 0x000000E4u);
    memset(out, 0xCD, sizeof out);
    check("DXT1 4x1 converts", d3d9_format_to_bgra8(D3DFMT_DXT1, blk, 8, 4, 1, out, 16) == 0);
    EXPECT("DXT1 4-color 0", 0, 0, 0xFFFF0000u);
    EXPECT("DXT1 4-color 1", 1, 0, 0xFF00FF00u);
    EXPECT("DXT1 4-color 2", 2, 0, 0xFFAA5500u);   /* (2*255+0)/3, (2*0+255)/3 */
    EXPECT("DXT1 4-color 3", 3, 0, 0xFF55AA00u);   /* (255+2*0)/3, (0+2*255)/3 */

    /* Same block as 2x2: the padded columns must stay untouched. */
    memset(out, 0xCD, sizeof out);
    check("DXT1 2x2 clips", d3d9_format_to_bgra8(D3DFMT_DXT1, blk, 8, 2, 2, out, 16) == 0);
    EXPECT("DXT1 clip (0,0)", 0, 0, 0xFFFF0000u);
    EXPECT("DXT1 clip (1,0)", 1, 0, 0xFF00FF00u);
    EXPECT("DXT1 clip (0,1)", 0, 1, 0xFFFF0000u);   /* indices 4..7 are 0 */
    EXPECT("DXT1 clip (1,1)", 1, 1, 0xFFFF0000u);
    check("DXT1 clip padding untouched", out[2] == SENTINEL && out[6] == SENTINEL);

    /* c0 < c1: three-colour mode, index 3 is transparent black. */
    dxt1_block(blk, 0x07E0, 0xF800, 0x000000E4u);
    memset(out, 0xCD, sizeof out);
    check("DXT1 3-color converts", d3d9_format_to_bgra8(D3DFMT_DXT1, blk, 8, 4, 1, out, 16) == 0);
    EXPECT("DXT1 3-color 0", 0, 0, 0xFF00FF00u);
    EXPECT("DXT1 3-color 1", 1, 0, 0xFFFF0000u);
    EXPECT("DXT1 3-color 2", 2, 0, 0xFF7F7F00u);   /* (0+255)/2 in both channels */
    EXPECT("DXT1 3-color 3", 3, 0, 0x00000000u);
}

static void test_dxt3(void)
{
    uint8_t blk[16];
    uint32_t out[16];

    /* Explicit 4-bit alpha: pixel 0 = 0xF, pixel 1 = 0, the rest 0. */
    memset(blk, 0, sizeof blk);
    blk[0] = 0x0F;
    dxt1_block(blk + 8, 0xF800, 0x07E0, 0x000000E4u); /* dxt1_block writes 8 bytes */
    memset(out, 0xCD, sizeof out);
    check("DXT3 2x1 converts", d3d9_format_to_bgra8(D3DFMT_DXT3, blk, 16, 2, 1, out, 16) == 0);
    EXPECT("DXT3 alpha 0xF", 0, 0, 0xFFFF0000u);
    EXPECT("DXT3 alpha 0", 1, 0, 0x0000FF00u);
    check("DXT3 padding untouched", out[2] == SENTINEL);

    /* DXT3 always uses four colours: with c0 < c1 index 3 stays opaque. */
    memset(blk, 0xFF, 8);                             /* every alpha nibble 0xF -> 255 */
    dxt1_block(blk + 8, 0x07E0, 0xF800, 0x000000E4u);
    memset(out, 0xCD, sizeof out);
    check("DXT3 c0<c1 converts", d3d9_format_to_bgra8(D3DFMT_DXT3, blk, 16, 4, 1, out, 16) == 0);
    EXPECT("DXT3 four-color 2", 2, 0, 0xFF55AA00u);
    /* index 3 = (0+2*255)/3 = 170, (255+2*0)/3 = 85 */
    EXPECT("DXT3 four-color 3", 3, 0, 0xFFAA5500u);
}

static void test_dxt5(void)
{
    uint8_t blk[16];
    uint32_t out[16];

    /* a0=255 > a1=0, indices 0,2,4,6; colour all index 0 (red). */
    memset(blk, 0, sizeof blk);
    blk[0] = 255; blk[1] = 0;
    blk[2] = 0x10; blk[3] = 0x0D;   /* 0xD10: 0 | 2<<3 | 4<<6 | 6<<9 */
    dxt1_block(blk + 8, 0xF800, 0x07E0, 0);
    memset(out, 0xCD, sizeof out);
    check("DXT5 4x1 converts", d3d9_format_to_bgra8(D3DFMT_DXT5, blk, 16, 4, 1, out, 16) == 0);
    EXPECT("DXT5 a0", 0, 0, 0xFFFF0000u);
    EXPECT("DXT5 a2=(6*a0+a1)/7", 1, 0, 0xDAFF0000u);
    EXPECT("DXT5 a4=(4*a0+3*a1)/7", 2, 0, 0x91FF0000u);
    EXPECT("DXT5 a6=(2*a0+5*a1)/7", 3, 0, 0x48FF0000u);

    /* a0=0 <= a1=255: six-value mode, indices 2,3,5,7. */
    memset(blk, 0, sizeof blk);
    blk[0] = 0; blk[1] = 255;
    blk[2] = 0x5A; blk[3] = 0x0F;   /* 0xF5A: 2 | 3<<3 | 5<<6 | 7<<9 */
    dxt1_block(blk + 8, 0xF800, 0x07E0, 0);
    memset(out, 0xCD, sizeof out);
    check("DXT5 a0<=a1 converts", d3d9_format_to_bgra8(D3DFMT_DXT5, blk, 16, 4, 1, out, 16) == 0);
    EXPECT("DXT5 a2=(4*a0+a1)/5", 0, 0, 0x33FF0000u);
    EXPECT("DXT5 a3=(3*a0+2*a1)/5", 1, 0, 0x66FF0000u);
    EXPECT("DXT5 a5=(a0+4*a1)/5", 2, 0, 0xCCFF0000u);
    EXPECT("DXT5 a7=255", 3, 0, 0xFFFF0000u);

    /* 3x2: only the real pixels are written, the padded fourth column is not. */
    memset(blk, 0, sizeof blk);
    blk[0] = 200; blk[1] = 100;   /* index 0 everywhere -> 200 */
    dxt1_block(blk + 8, 0xF800, 0xF800, 0);
    memset(out, 0xCD, sizeof out);
    check("DXT5 3x2 clips", d3d9_format_to_bgra8(D3DFMT_DXT5, blk, 16, 3, 2, out, 16) == 0);
    EXPECT("DXT5 clip (2,0)", 2, 0, 0xC8FF0000u);
    EXPECT("DXT5 clip (2,1)", 2, 1, 0xC8FF0000u);
    check("DXT5 clip padding untouched", out[3] == SENTINEL && out[7] == SENTINEL);
}

static void test_uncompressed(void)
{
    uint32_t out[16];

    {
        uint16_t s[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
        check("R5G6B5 converts", d3d9_format_to_bgra8(D3DFMT_R5G6B5, s, 8, 4, 1, out, 16) == 0);
        EXPECT("R5G6B5 red", 0, 0, 0xFFFF0000u);
        EXPECT("R5G6B5 green", 1, 0, 0xFF00FF00u);
        EXPECT("R5G6B5 blue", 2, 0, 0xFF0000FFu);
        EXPECT("R5G6B5 white", 3, 0, 0xFFFFFFFFu);
    }
    {
        uint16_t s[4] = { 0xFC00, 0x801F, 0x7FFF, 0x0000 };
        check("A1R5G5B5 converts", d3d9_format_to_bgra8(D3DFMT_A1R5G5B5, s, 8, 4, 1, out, 16) == 0);
        EXPECT("A1R5G5B5 opaque red", 0, 0, 0xFFFF0000u);
        EXPECT("A1R5G5B5 opaque blue", 1, 0, 0xFF0000FFu);
        EXPECT("A1R5G5B5 transparent white", 2, 0, 0x00FFFFFFu);
        EXPECT("A1R5G5B5 transparent black", 3, 0, 0x00000000u);
    }
    {
        uint16_t s[4] = { 0x001F, 0x7C00, 0x0000, 0xFFFF };
        check("X1R5G5B5 converts", d3d9_format_to_bgra8(D3DFMT_X1R5G5B5, s, 8, 4, 1, out, 16) == 0);
        EXPECT("X1R5G5B5 blue", 0, 0, 0xFF0000FFu);
        EXPECT("X1R5G5B5 red", 1, 0, 0xFFFF0000u);
        EXPECT("X1R5G5B5 black opaque", 2, 0, 0xFF000000u);
        EXPECT("X1R5G5B5 white", 3, 0, 0xFFFFFFFFu);
    }
    {
        uint16_t s[4] = { 0x8F00, 0x0123, 0xFFFF, 0x0000 };
        check("A4R4G4B4 converts", d3d9_format_to_bgra8(D3DFMT_A4R4G4B4, s, 8, 4, 1, out, 16) == 0);
        EXPECT("A4R4G4B4 alpha 8", 0, 0, 0x88FF0000u);
        EXPECT("A4R4G4B4 a=0", 1, 0, 0x00112233u);
        EXPECT("A4R4G4B4 white", 2, 0, 0xFFFFFFFFu);
        EXPECT("A4R4G4B4 black", 3, 0, 0x00000000u);
    }
    {
        uint16_t s[4] = { 0x0F00, 0x0123, 0x0000, 0xFFFF };
        check("X4R4G4B4 converts", d3d9_format_to_bgra8(D3DFMT_X4R4G4B4, s, 8, 4, 1, out, 16) == 0);
        EXPECT("X4R4G4B4 red opaque", 0, 0, 0xFFFF0000u);
        EXPECT("X4R4G4B4 a=255", 1, 0, 0xFF112233u);
        EXPECT("X4R4G4B4 black", 2, 0, 0xFF000000u);
        EXPECT("X4R4G4B4 white", 3, 0, 0xFFFFFFFFu);
    }
    {
        uint32_t s[2] = { 0x80402010u, 0x00112233u };
        check("A8R8G8B8 converts", d3d9_format_to_bgra8(D3DFMT_A8R8G8B8, s, 8, 2, 1, out, 16) == 0);
        EXPECT("A8R8G8B8 keeps alpha", 0, 0, 0x80402010u);
        EXPECT("A8R8G8B8 alpha 0", 1, 0, 0x00112233u);
        check("X8R8G8B8 converts", d3d9_format_to_bgra8(D3DFMT_X8R8G8B8, s, 8, 2, 1, out, 16) == 0);
        EXPECT("X8R8G8B8 forces alpha", 0, 0, 0xFF402010u);
        EXPECT("X8R8G8B8 forces alpha 2", 1, 0, 0xFF112233u);
    }
    {
        uint8_t a[4] = { 0x00, 0x7F, 0x80, 0xFF };
        check("A8 converts", d3d9_format_to_bgra8(D3DFMT_A8, a, 4, 4, 1, out, 16) == 0);
        EXPECT("A8 0", 0, 0, 0x00000000u);
        EXPECT("A8 0x7F", 1, 0, 0x7F000000u);
        EXPECT("A8 0x80", 2, 0, 0x80000000u);
        EXPECT("A8 0xFF", 3, 0, 0xFF000000u);
    }
    {
        uint8_t l[4] = { 0x00, 0x40, 0x80, 0xFF };
        check("L8 converts", d3d9_format_to_bgra8(D3DFMT_L8, l, 4, 4, 1, out, 16) == 0);
        EXPECT("L8 0", 0, 0, 0xFF000000u);
        EXPECT("L8 0x40", 1, 0, 0xFF404040u);
        EXPECT("L8 0x80", 2, 0, 0xFF808080u);
        EXPECT("L8 0xFF", 3, 0, 0xFFFFFFFFu);
    }
    {
        uint8_t s[4] = { 0x40, 0x80, 0xFF, 0x00 };
        check("A8L8 converts", d3d9_format_to_bgra8(D3DFMT_A8L8, s, 4, 2, 1, out, 16) == 0);
        EXPECT("A8L8 L=0x40 A=0x80", 0, 0, 0x80404040u);
        EXPECT("A8L8 L=0xFF A=0", 1, 0, 0x00FFFFFFu);
    }
}

static void test_odd_sizes(void)
{
    uint8_t src[5 * 6];
    uint32_t out[5 * 4 + 4];

    /* A8L8 3x5, tight pitch 6; luminance = y*10 + x, alpha = 0x80 + y. */
    memset(src, 0, sizeof src);
    for (uint32_t y = 0; y < 5; y++)
        for (uint32_t x = 0; x < 3; x++) {
            src[y * 6 + x * 2] = (uint8_t)(y * 10 + x);
            src[y * 6 + x * 2 + 1] = (uint8_t)(0x80 + y);
        }
    memset(out, 0xCD, sizeof out);
    check("A8L8 3x5 converts", d3d9_format_to_bgra8(D3DFMT_A8L8, src, 6, 3, 5, out, 16) == 0);
    check("A8L8 3x5 first", out[0] == 0x80000000u);
    check("A8L8 3x5 last", out[4 * 4 + 2] == 0x842A2A2Au);   /* L=42=0x2A, A=0x84 */
    check("A8L8 3x5 row padding untouched", out[3] == SENTINEL && out[4 * 4 + 3] == SENTINEL);
}

static void test_errors(void)
{
    uint8_t src[64], dst[64];
    uint32_t guard[4] = { SENTINEL, SENTINEL, SENTINEL, SENTINEL };

    memset(src, 0x5A, sizeof src);
    memset(dst, 0xCD, sizeof dst);

    check("unknown format unsupported", !d3d9_format_is_supported(12345u));
    check("unknown format pitch 0", d3d9_format_pitch(12345u, 8) == 0);
    check("unknown format level 0", d3d9_format_level_size(12345u, 8, 8) == 0);
    /* DXT2/DXT4 are now supported (premultiplied alpha preserved). */
    check("unknown format conversion fails",
          d3d9_format_to_bgra8(12345u, src, 32, 4, 1, guard, 16) == -1);
    check("dst untouched after unknown format", guard[0] == SENTINEL);
    check("null src fails", d3d9_format_to_bgra8(D3DFMT_L8, NULL, 0, 4, 1, dst, 16) == -1);
    check("null dst fails", d3d9_format_to_bgra8(D3DFMT_L8, src, 0, 4, 1, NULL, 16) == -1);
    check("zero width fails", d3d9_format_to_bgra8(D3DFMT_L8, src, 0, 0, 1, dst, 16) == -1);
    check("zero height fails", d3d9_format_to_bgra8(D3DFMT_L8, src, 0, 4, 0, dst, 16) == -1);
    check("short src pitch fails",
          d3d9_format_to_bgra8(D3DFMT_DXT1, src, 4, 4, 1, dst, 16) == -1);
    check("short dst pitch fails",
          d3d9_format_to_bgra8(D3DFMT_A8R8G8B8, src, 16, 4, 1, dst, 12) == -1);
    check("tight pitch accepted",
          d3d9_format_to_bgra8(D3DFMT_A8R8G8B8, src, 0, 4, 1, dst, 0) == 0);
}

static void test_dxt2_dxt4(void)
{
    uint8_t blk[16];
    uint32_t out[16];
    /* DXT2 same block layout as DXT3 (premultiplied alpha preserved). */
    memset(blk, 0, sizeof blk);
    blk[0] = 0x0F; /* alpha: pixel0=0xF, pixel1=0 */
    dxt1_block(blk + 8, 0xF800, 0x07E0, 0x000000E4u);
    memset(out, 0xCD, sizeof out);
    check("DXT2 converts", d3d9_format_to_bgra8(D3DFMT_DXT2, blk, 16, 2, 1, out, 16) == 0);
    EXPECT("DXT2 alpha 0xF", 0, 0, 0xFFFF0000u);
    EXPECT("DXT2 alpha 0", 1, 0, 0x0000FF00u);
    check("DXT2 padding untouched", out[2] == SENTINEL);

    /* DXT4 same as DXT5. */
    memset(blk, 0, sizeof blk);
    blk[0] = 255; blk[1] = 0;
    blk[2] = 0x10; blk[3] = 0x0D;
    dxt1_block(blk + 8, 0xF800, 0x07E0, 0);
    memset(out, 0xCD, sizeof out);
    check("DXT4 converts", d3d9_format_to_bgra8(D3DFMT_DXT4, blk, 16, 4, 1, out, 16) == 0);
    EXPECT("DXT4 a0", 0, 0, 0xFFFF0000u);
    EXPECT("DXT4 a2", 1, 0, 0xDAFF0000u);
}

static void test_new_formats(void)
{
    uint32_t out[16];
    /* R8G8B8: B,G,R in memory */
    { uint8_t s[6] = { 0x10,0x20,0x30, 0xFF,0x00,0x80 };
      check("R8G8B8 converts", d3d9_format_to_bgra8(D3DFMT_R8G8B8, s, 6, 2, 1, out, 16) == 0);
      EXPECT("R8G8B8 px0", 0, 0, 0xFF302010u);
      EXPECT("R8G8B8 px1", 1, 0, 0xFF8000FFu); }

    /* A2R10G10B10: A=3 -> 255, R=1023, G=0, B=512 -> 127/128 */
    { uint32_t s[2] = { 0xFFFFFFFFu, 0x00000000u };
      /* 0xFFFFFFFF: A=3,R=1023,G=1023,B=1023 */
      check("A2R10G10B10 converts", d3d9_format_to_bgra8(D3DFMT_A2R10G10B10, s, 8, 2, 1, out, 16) == 0);
      EXPECT("A2R10G10B10 white", 0, 0, 0xFFFFFFFFu);
      EXPECT("A2R10G10B10 black", 1, 0, 0x00000000u); }
    { uint32_t s[1] = { (1u<<30) | (512u<<20) | (256u<<10) | 128u };
      check("A2R10G10B10 mid converts", d3d9_format_to_bgra8(D3DFMT_A2R10G10B10, s, 4, 1, 1, out, 16) == 0);
      EXPECT("A2R10G10B10 mid", 0, 0, 0x557F3F1Fu); } /* A=85, R=127, G=63, B=31 */

    /* G16R16: R low word, G high word; top 8 bits kept */
    { uint16_t s[4] = { 0xFF00, 0x8000, 0x0000, 0xFFFF };
      check("G16R16 converts", d3d9_format_to_bgra8(D3DFMT_G16R16, s, 8, 2, 1, out, 16) == 0);
      EXPECT("G16R16 px0", 0, 0, 0xFFFF8000u); /* R=FF,G=80,B=0 */
      EXPECT("G16R16 px1", 1, 0, 0xFF00FF00u); }

    /* A16B16G16R16 */
    { uint16_t s[8] = { 0xFFFF,0x0000,0x0000,0xFFFF,  0x8000,0x8000,0x8000,0x8000 };
      check("A16B16G16R16 converts", d3d9_format_to_bgra8(D3DFMT_A16B16G16R16, s, 16, 2, 1, out, 16) == 0);
      EXPECT("A16B16G16R16 red", 0, 0, 0xFFFF0000u);
      EXPECT("A16B16G16R16 grey", 1, 0, 0x80808080u); }

    /* V8U8: signed bytes per D3D9 doc: U at byte 0, V at byte 1 -> mapped to [0,255] */
    { uint8_t s[4] = { 0x00, 0x00, 0x7F, 0x80 }; /* px0: U=0,V=0 -> R=128,G=128; px1: U=127,V=-128 -> R=255,G=0 */
      check("V8U8 converts", d3d9_format_to_bgra8(D3DFMT_V8U8, s, 4, 2, 1, out, 16) == 0);
      EXPECT("V8U8 zero", 0, 0, 0xFF808000u);
      EXPECT("V8U8 minmax", 1, 0, 0xFFFF0000u); }

    /* L16 */
    { uint8_t s[4] = { 0x00,0xFF, 0x00,0x80 };
      check("L16 converts", d3d9_format_to_bgra8(D3DFMT_L16, s, 4, 2, 1, out, 16) == 0);
      EXPECT("L16 FF", 0, 0, 0xFFFFFFFFu);
      EXPECT("L16 80", 1, 0, 0xFF808080u); }

    /* A4L4 */
    { uint8_t s[2] = { 0xF0, 0x8F }; /* L=0,A=F ; L=F,A=8 */
      check("A4L4 converts", d3d9_format_to_bgra8(D3DFMT_A4L4, s, 2, 2, 1, out, 16) == 0);
      EXPECT("A4L4 A=F L=0", 0, 0, 0xFF000000u);
      EXPECT("A4L4 A=8 L=F", 1, 0, 0x88FFFFFFu); }

    /* Float formats */
    { uint16_t s[2] = { 0x3C00, 0x0000 }; /* 1.0, 0.0 */
      check("R16F converts", d3d9_format_to_bgra8(D3DFMT_R16F, s, 4, 2, 1, out, 16) == 0);
      EXPECT("R16F 1.0", 0, 0, 0xFFFF0000u);
      EXPECT("R16F 0.0", 1, 0, 0xFF000000u); }
    { uint16_t s[4] = { 0x3C00,0x3800, 0x0000,0x3C00 }; /* (1,0.5), (0,1) */
      check("G16R16F converts", d3d9_format_to_bgra8(D3DFMT_G16R16F, s, 8, 2, 1, out, 16) == 0);
      EXPECT("G16R16F px0", 0, 0, 0xFFFF8000u);
      EXPECT("G16R16F px1", 1, 0, 0xFF00FF00u); }
    { uint16_t s[8] = { 0x3C00,0x0000,0x0000,0x3C00,  0,0,0,0 };
      check("A16B16G16R16F converts", d3d9_format_to_bgra8(D3DFMT_A16B16G16R16F, s, 16, 2, 1, out, 16) == 0);
      EXPECT("A16B16G16R16F red", 0, 0, 0xFFFF0000u);
      EXPECT("A16B16G16R16F zero", 1, 0, 0x00000000u); }
    { float s[2] = { 1.0f, 0.5f };
      check("R32F converts", d3d9_format_to_bgra8(D3DFMT_R32F, s, 8, 2, 1, out, 16) == 0);
      EXPECT("R32F 1.0", 0, 0, 0xFFFF0000u);
      EXPECT("R32F 0.5", 1, 0, 0xFF800000u); }
    { float s[4] = { 1.0f,0.0f, 0.0f,1.0f };
      check("G32R32F converts", d3d9_format_to_bgra8(D3DFMT_G32R32F, s, 16, 2, 1, out, 16) == 0);
      EXPECT("G32R32F px0", 0, 0, 0xFFFF0000u);
      EXPECT("G32R32F px1", 1, 0, 0xFF00FF00u); }
    { float s[8] = { 0.0f,0.0f,1.0f,1.0f, 1,1,1,0 };
      check("A32B32G32R32F converts", d3d9_format_to_bgra8(D3DFMT_A32B32G32R32F, s, 32, 2, 1, out, 16) == 0);
      EXPECT("A32B32G32R32F blue", 0, 0, 0xFF0000FFu);
      EXPECT("A32B32G32R32F white0", 1, 0, 0x00FFFFFFu); }

    /* odd sizes for new formats */
    { uint8_t s[9]; memset(s, 0, sizeof s);
      s[0]=10; s[1]=20; s[2]=30; s[3]=40; s[4]=50; s[5]=60; s[6]=70; s[7]=80; s[8]=90;
      memset(out, 0xCD, sizeof out);
      check("R8G8B8 3x1 odd", d3d9_format_to_bgra8(D3DFMT_R8G8B8, s, 9, 3, 1, out, 16) == 0);
      EXPECT("R8G8B8 odd0", 0, 0, 0xFF1E140Au);
      EXPECT("R8G8B8 odd2", 2, 0, 0xFF5A5046u);
      check("R8G8B8 odd pad", out[3] == SENTINEL); }
}

int main(void)
{
    test_geometry();
    test_dxt1();
    test_dxt3();
    test_dxt5();
    test_dxt2_dxt4();
    test_uncompressed();
    test_odd_sizes();
    test_new_formats();
    test_errors();

    if (!fails) printf("d3d9_format_selftest: all checks passed\n");
    return fails != 0;
}
