/*
 * d3dx9_surface.c - D3DX9 surface helpers with the *public* stdcall ABI:
 *
 *   D3DXLoadSurfaceFromMemory(dst, dstpal, dstrect, srcmem, srcfmt, srcpitch,
 *                             srcpal, srcrect, filter, colorkey)         10 args
 *   D3DXLoadSurfaceFromSurface(dst, dstpal, dstrect, src, srcpal, srcrect,
 *                              filter, colorkey)                          8 args
 *   D3DXFilterTexture(texture, palette, srclevel, filter)                 4 args
 *   D3DXSaveSurfaceToFileA(file, fileformat, surface, palette, srcrect)   5 args
 *
 * Argument positions and constants follow D3dx9tex.h/D3dx9.h (checked against
 * Wine's headers): D3DX_FILTER_NONE is 1, D3DX_DEFAULT is 0xFFFFFFFF and
 * D3DXIFF_DDS is 4. Pixels go through d3d9_format; rectangles are validated
 * against the surface; unsupported palettes, formats and file types are
 * reported as errors, never silently accepted.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "d3d9_hle.h"
#include "d3d9_format.h"
#include "d3d9_internal.h"
#include "d3dx9_surface.h"

#ifndef D3D_OK
#define D3D_OK 0u
#endif
#ifndef D3DERR_INVALIDCALL
#define D3DERR_INVALIDCALL 0x8876086Cu
#endif
#ifndef D3DERR_NOTAVAILABLE
#define D3DERR_NOTAVAILABLE 0x8876086Au
#endif
#ifndef D3DERR_OUTOFVIDEOMEMORY
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu
#endif
#ifndef D3DXERR_INVALIDDATA
#define D3DXERR_INVALIDDATA 0x88760B59u
#endif

#define TAG "d3dx9_surface: "

static void log_err(const char *what) { fprintf(stderr, "%s%s\n", TAG, what); }

/* ------------------------------------------------------------- type checks */

/* The vtable pointer decides what an object really is; a cubemap, a device
 * handle or garbage must not be mistaken for a surface. */
static int is_surf9(uint32_t o) { return o && MEM32(o) == g_vt_surface9; }
static int is_tex9(uint32_t o)  { return o && MEM32(o) == g_vt_tex9; }

/* ------------------------------------------------------------- D3D9 objects */

static uint32_t surf_get_desc(uint32_t surf, D3DSURFACE_DESC *desc) {
    void *h = hle_alloc(sizeof(D3DSURFACE_DESC));
    if (!h) return D3DERR_INVALIDCALL;
    uint32_t a[2] = { surf, hle_va(h) };
    uint32_t rc = hle_call_guest(hle_resolve("IDirect3DSurface9::GetDesc"), 2, a);
    if (rc == D3D_OK) memcpy(desc, h, sizeof(D3DSURFACE_DESC));
    hle_free(h);
    return rc;
}

/* IDirect3DSurface9::LockRect(this, pLockedRect, pRect, Flags) */
static uint32_t surf_lock_rect(uint32_t surf, int flags, D3DLOCKED_RECT *locked) {
    void *h = hle_alloc(sizeof(D3DLOCKED_RECT));
    if (!h) return D3DERR_INVALIDCALL;
    uint32_t a[4] = { surf, hle_va(h), 0, (uint32_t)flags };
    uint32_t rc = hle_call_guest(hle_resolve("IDirect3DSurface9::LockRect"), 4, a);
    if (rc == D3D_OK) memcpy(locked, h, sizeof(D3DLOCKED_RECT));
    hle_free(h);
    return rc;
}
static void surf_unlock_rect(uint32_t surf) {
    uint32_t a[1] = { surf };
    hle_call_guest(hle_resolve("IDirect3DSurface9::UnlockRect"), 1, a);
}

/* Texture level helpers (IDirect3DTexture9, not IDirect3DSurface9). */
static uint32_t tex_level_count(uint32_t tex) {
    uint32_t a[1] = { tex };
    return hle_call_guest(hle_resolve("IDirect3DTexture9::GetLevelCount"), 1, a);
}
static int tex_level_desc(uint32_t tex, uint32_t level, D3DSURFACE_DESC *desc) {
    void *h = hle_alloc(sizeof(D3DSURFACE_DESC));
    if (!h) return -1;
    uint32_t a[3] = { tex, level, hle_va(h) };
    uint32_t rc = hle_call_guest(hle_resolve("IDirect3DTexture9::GetLevelDesc"), 3, a);
    if (rc == D3D_OK) memcpy(desc, h, sizeof *desc);
    hle_free(h);
    return rc == D3D_OK ? 0 : -1;
}
static int tex_lock_level(uint32_t tex, uint32_t level, D3DLOCKED_RECT *lr) {
    void *h = hle_alloc(sizeof(D3DLOCKED_RECT));
    if (!h) return -1;
    uint32_t a[5] = { tex, level, hle_va(h), 0, 0 };
    uint32_t rc = hle_call_guest(hle_resolve("IDirect3DTexture9::LockRect"), 5, a);
    if (rc == D3D_OK) memcpy(lr, h, sizeof *lr);
    hle_free(h);
    return rc == D3D_OK && lr->pBits ? 0 : -1;
}
static void tex_unlock_level(uint32_t tex, uint32_t level) {
    uint32_t a[2] = { tex, level };
    hle_call_guest(hle_resolve("IDirect3DTexture9::UnlockRect"), 2, a);
}

/* ------------------------------------------------------------- filter value */

#define D3DX_FILTER_KNOWN_BITS (0x00000007u | 0x007F0000u)

/* Wine's d3dx_validate_filter: the low three bits select one of the five
 * filters; the high bits are mirror/dither/sRGB hints. Anything else is an
 * invalid call, not a default. D3DX_DEFAULT means BOX. */
static int filter_valid(uint32_t f) {
    if (f == D3DX_DEFAULT) return 1;
    if (f & ~D3DX_FILTER_KNOWN_BITS) return 0;
    return (f & 0x7u) <= D3DX_FILTER_BOX;
}
static uint32_t filter_resolve(uint32_t f) {
    return f == D3DX_DEFAULT ? D3DX_FILTER_BOX : f;
}
/* NONE and POINT are nearest; LINEAR/TRIANGLE/BOX/DEFAULT use a box average.
 * The higher filter flags are hints we do not implement and never claim to. */
static int filter_mode(uint32_t resolved) {
    return (resolved == D3DX_FILTER_NONE || resolved == D3DX_FILTER_POINT)
         ? 0 : 1;
}

/* ------------------------------------------------------------- rectangles */

/* A surface rect: NULL means the whole surface. Out-of-bounds / inverted
 * rects are invalid calls; an empty one is a successful no-op. */
static int surface_rect(uint32_t va, uint32_t w, uint32_t h, D3DRECT *r, int *empty) {
    *empty = 0;
    if (!va) { r->x1 = 0; r->y1 = 0; r->x2 = (int32_t)w; r->y2 = (int32_t)h; }
    else     *r = *(const D3DRECT *)hle_ptr(va);
    if (r->x1 < 0 || r->y1 < 0 || r->x2 < r->x1 || r->y2 < r->y1) return 0;
    if ((uint32_t)r->x2 > w || (uint32_t)r->y2 > h) return 0;
    if (r->x1 == r->x2 || r->y1 == r->y2) *empty = 1;
    return 1;
}

/* D3DXLoadSurfaceFromMemory requires pSrcRect (Wine: D3DERR_INVALIDCALL for
 * NULL). Negative coordinates are rejected rather than reading before the
 * caller's buffer. */
static int memory_rect(uint32_t va, uint32_t *w, uint32_t *h,
                       uint32_t *px, uint32_t *py) {
    if (!va) return 0;
    const D3DRECT *r = (const D3DRECT *)hle_ptr(va);
    if (r->x1 < 0 || r->y1 < 0 || r->x2 <= r->x1 || r->y2 <= r->y1) return 0;
    *px = (uint32_t)r->x1;
    *py = (uint32_t)r->y1;
    *w  = (uint32_t)(r->x2 - r->x1);
    *h  = (uint32_t)(r->y2 - r->y1);
    return 1;
}

/* ------------------------------------------------------------- pixel output */

/* Byte offset of a rect origin in a source level. Block-compressed formats can
 * only be addressed on 4x4 block boundaries; a misaligned origin is an error,
 * never a silent misread. */
static int src_offset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t fmt,
                      size_t *off) {
    if (d3d9_format_is_block_compressed(fmt)) {
        if ((x & 3u) || (y & 3u)) return 0;
        *off = (size_t)(y / 4u) * pitch +
               (size_t)(x / 4u) * d3d9_format_block_bytes(fmt);
    } else {
        *off = (size_t)y * pitch + (size_t)x * d3d9_format_bytes_per_pixel(fmt);
    }
    return 1;
}

/* Bytes per pixel we can write into a destination format. */
static uint32_t dst_bpp(uint32_t fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:     return 4;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8L8:         return 2;
    case D3DFMT_A8:
    case D3DFMT_L8:           return 1;
    default:                  return 0;   /* not writable here */
    }
}

/* Write one 0xAARRGGBB pixel in the destination format. */
static void write_pixel(uint32_t fmt, void *p, uint32_t bgra8) {
    uint8_t a = (uint8_t)(bgra8 >> 24), r = (uint8_t)(bgra8 >> 16),
            g = (uint8_t)(bgra8 >> 8),  b = (uint8_t)bgra8;
    uint8_t *dp = (uint8_t *)p;
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        *(uint32_t *)dp = bgra8;
        break;
    case D3DFMT_R5G6B5: {
        uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        memcpy(dp, &v, 2);
        break;
    }
    case D3DFMT_X1R5G5B5: {
        uint16_t v = (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        memcpy(dp, &v, 2);
        break;
    }
    case D3DFMT_A1R5G5B5: {
        uint16_t v = (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3) |
                                (a ? 0x8000u : 0u));
        memcpy(dp, &v, 2);
        break;
    }
    case D3DFMT_X4R4G4B4: {
        uint16_t v = (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
        memcpy(dp, &v, 2);
        break;
    }
    case D3DFMT_A4R4G4B4: {
        uint16_t v = (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4) |
                                ((a >> 4) << 12));
        memcpy(dp, &v, 2);
        break;
    }
    case D3DFMT_A8:
        *dp = a;
        break;
    case D3DFMT_L8:
        *dp = (uint8_t)((299u * r + 587u * g + 114u * b + 500u) / 1000u);
        break;
    case D3DFMT_A8L8: {
        uint8_t lum = (uint8_t)((299u * r + 587u * g + 114u * b + 500u) / 1000u);
        dp[0] = lum; dp[1] = a;     /* L in the low byte, A in the high byte */
        break;
    }
    default:
        break;
    }
}

/* Convert an sw x sh source rect once to tight BGRA8, then resample into the
 * dw x dh destination rect. src already points at the rect's first pixel.
 * Returns 0 on success, -1 on an unsupported format or conversion failure. */
static int copy_rect(const void *src, uint32_t src_pitch, uint32_t src_fmt,
                     void *dst, uint32_t dst_pitch, uint32_t dst_fmt,
                     uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                     uint32_t colorkey, int have_key, int mode) {
    uint32_t dbpp = dst_bpp(dst_fmt);
    if (!dbpp) { log_err("Zielformat wird nicht zum Schreiben unterstuetzt"); return -1; }
    if (!d3d9_format_is_supported(src_fmt)) {
        log_err("Quellformat wird nicht zum Lesen unterstuetzt");
        return -1;
    }
    if (!sw || !sh || !dw || !dh) return 0;     /* empty rect: nothing to do */

    size_t cells = (size_t)sw * (size_t)sh;
    if (cells / (size_t)sw != (size_t)sh || cells > (size_t)0x2000000u) {
        log_err("Quellrechteck zu gross");
        return -1;
    }
    uint8_t *bgra = (uint8_t *)malloc(cells * 4u);
    if (!bgra) return -1;

    int rc = 0;
    if (d3d9_format_to_bgra8(src_fmt, src, src_pitch, sw, sh, bgra, sw * 4u) != 0) {
        log_err("Quellzeilen ließen sich nicht konvertieren");
        rc = -1;
    } else {
        for (uint32_t y = 0; y < dh; y++) {
            uint8_t *drow = (uint8_t *)dst + (size_t)y * dst_pitch;
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t px;
                if (!mode) {
                    uint32_t sx = (uint32_t)((uint64_t)x * sw / dw);
                    uint32_t sy = (uint32_t)((uint64_t)y * sh / dh);
                    memcpy(&px, bgra + ((size_t)sy * sw + sx) * 4u, 4);
                } else {
                    uint32_t y0 = (uint32_t)((uint64_t)y * sh / dh);
                    uint32_t y1 = (uint32_t)((uint64_t)(y + 1u) * sh / dh);
                    uint32_t x0 = (uint32_t)((uint64_t)x * sw / dw);
                    uint32_t x1 = (uint32_t)((uint64_t)(x + 1u) * sw / dw);
                    if (y1 <= y0) y1 = y0 + 1u;
                    if (x1 <= x0) x1 = x0 + 1u;
                    if (y1 > sh) y1 = sh;
                    if (x1 > sw) x1 = sw;
                    uint64_t a = 0, r = 0, g = 0, b = 0;
                    uint64_t n = 0;
                    for (uint32_t sy = y0; sy < y1; sy++) {
                        for (uint32_t sx = x0; sx < x1; sx++) {
                            uint32_t p;
                            memcpy(&p, bgra + ((size_t)sy * sw + sx) * 4u, 4);
                            a += p >> 24;
                            r += (p >> 16) & 0xFFu;
                            g += (p >> 8) & 0xFFu;
                            b += p & 0xFFu;
                            n++;
                        }
                    }
                    px = (uint32_t)((a / n) << 24 | (r / n) << 16 |
                                    (g / n) << 8 | (b / n));
                }
                if (have_key && (px & 0x00FFFFFFu) == (colorkey & 0x00FFFFFFu))
                    continue;
                write_pixel(dst_fmt, drow + (size_t)x * dbpp, px);
            }
        }
    }
    free(bgra);
    return rc;
}

/* ------------------------------------------------------------- FromMemory */

/* (dst, dstpal, dstrect, srcmem, srcfmt, srcpitch, srcpal, srcrect, filter,
 *  colorkey) -- 10 arguments, D3dx9tex.h. */
static uint32_t load_surface_from_memory_core(uint32_t surf, uint32_t dstpal, uint32_t drect_va,
                                              const void *srcmem_host, uint32_t src_fmt, uint32_t src_pitch,
                                              uint32_t srcpal, uint32_t srect_va, uint32_t filter, uint32_t colorkey)
{
    if (!is_surf9(surf) || !srcmem_host || !srect_va) {
        log_err("LoadSurfaceFromMemory: ungültiger Oberflächen-/Speicher-/Rechteckzeiger");
        return D3DERR_INVALIDCALL;
    }
    if (dstpal || srcpal) {
        log_err("LoadSurfaceFromMemory: Paletten nicht unterstützt");
        return D3DERR_NOTAVAILABLE;
    }
    if (!filter_valid(filter)) {
        log_err("LoadSurfaceFromMemory: ungültiger Filter");
        return D3DERR_INVALIDCALL;
    }
    if (!d3d9_format_is_supported(src_fmt) || src_pitch == 0) {
        log_err("LoadSurfaceFromMemory: ungültiges Quellformat oder Pitch");
        return D3DERR_INVALIDCALL;
    }

    D3DSURFACE_DESC desc;
    if (surf_get_desc(surf, &desc) != D3D_OK) {
        log_err("LoadSurfaceFromMemory: GetDesc fehlgeschlagen");
        return D3DERR_INVALIDCALL;
    }
    D3DRECT drect;
    int dempty;
    if (!surface_rect(drect_va, desc.Width, desc.Height, &drect, &dempty)) {
        log_err("LoadSurfaceFromMemory: ungültiges Zielrechteck");
        return D3DERR_INVALIDCALL;
    }
    if (dempty) return D3D_OK;

    uint32_t sw, sh, sx, sy;
    if (!memory_rect(srect_va, &sw, &sh, &sx, &sy)) {
        log_err("LoadSurfaceFromMemory: ungültiges Quellrechteck");
        return D3DERR_INVALIDCALL;
    }
    uint32_t dw = (uint32_t)(drect.x2 - drect.x1);
    uint32_t dh = (uint32_t)(drect.y2 - drect.y1);

    D3DLOCKED_RECT lr;
    if (surf_lock_rect(surf, 0, &lr) != D3D_OK || !lr.pBits) {
        log_err("LoadSurfaceFromMemory: LockRect fehlgeschlagen");
        return D3DERR_INVALIDCALL;
    }
    size_t soff;
    if (!src_offset(sx, sy, src_pitch, src_fmt, &soff)) {
        surf_unlock_rect(surf);
        log_err("LoadSurfaceFromMemory: block-komprimiertes Quellrechteck nicht blockausgerichtet");
        return D3DERR_INVALIDCALL;
    }
    const uint8_t *srow = (const uint8_t *)srcmem_host + soff;
    uint8_t *drow = (uint8_t *)hle_ptr(lr.pBits) + (size_t)drect.y1 * (size_t)lr.Pitch +
                    (size_t)drect.x1 * dst_bpp(desc.Format);
    int rc = copy_rect(srow, src_pitch, src_fmt, drow, (uint32_t)lr.Pitch,
                       desc.Format, sw, sh, dw, dh, colorkey, colorkey != 0u,
                       filter_mode(filter_resolve(filter)));
    surf_unlock_rect(surf);
    return rc == 0 ? D3D_OK : D3DERR_NOTAVAILABLE;
}

void s_D3DXLoadSurfaceFromMemory(void) {
    uint32_t surf      = A32(0);
    uint32_t dstpal    = A32(1);
    uint32_t drect_va  = A32(2);
    uint32_t srcmem    = A32(3);
    uint32_t src_fmt   = A32(4);
    uint32_t src_pitch = A32(5);
    uint32_t srcpal    = A32(6);
    uint32_t srect_va  = A32(7);
    uint32_t filter    = A32(8);
    uint32_t colorkey  = A32(9);
    if (!srcmem) {
        log_err("LoadSurfaceFromMemory: ungültiger Oberflächen-/Speicher-/Rechteckzeiger");
        RET(D3DERR_INVALIDCALL, 10);
    }
    RET(load_surface_from_memory_core(surf, dstpal, drect_va, hle_ptr(srcmem), src_fmt, src_pitch,
                                      srcpal, srect_va, filter, colorkey), 10);
}

void s_D3DXLoadSurfaceFromFileInMemory(void) {
    /* (pDestSurface, pDestPalette, pDestRect, pSrcData, SrcDataSize, pSrcRect, Filter, Colorkey, pSrcInfo) */
    uint32_t surf     = A32(0);
    uint32_t dstpal   = A32(1);
    uint32_t drect_va = A32(2);
    uint32_t srcdata  = A32(3);
    uint32_t srcsize  = A32(4);
    uint32_t srect_va = A32(5);
    uint32_t filter   = A32(6);
    uint32_t colorkey = A32(7);
    uint32_t srcinfo_va = A32(8);
    if (!srcdata || !srcsize) {
        log_err("LoadSurfaceFromFileInMemory: kein Bildspeicher");
        RET(D3DERR_INVALIDCALL, 9);
    }
    d3dx9_texture_info info;
    const uint8_t *top = NULL; uint32_t pitch = 0, fmt = 0; uint8_t *decoded = NULL;
    int hr = d3dx9_texture_decode_top_level(hle_ptr(srcdata), srcsize, &info, &top, &pitch, &fmt, &decoded);
    if (srcinfo_va && hr == 0) *(d3dx9_texture_info *)hle_ptr(srcinfo_va) = info;
    else if (srcinfo_va) memset(hle_ptr(srcinfo_va), 0, sizeof(d3dx9_texture_info));
    if (hr) {
        fprintf(stderr, "%sLoadSurfaceFromFileInMemory: Bilddaten ungueltig (hr=0x%08x)\n", TAG, hr);
        free(decoded); RET(hr, 9);
    }
    void *rect_mem = NULL;
    if (!srect_va) {
        rect_mem = hle_alloc(sizeof(D3DRECT));
        if (!rect_mem) {
            log_err("LoadSurfaceFromFileInMemory: kein Speicher fuer Quellrechteck");
            free(decoded); RET(D3DERR_OUTOFVIDEOMEMORY, 9);
        }
        D3DRECT r = { 0, 0, (int32_t)info.Width, (int32_t)info.Height };
        memcpy(rect_mem, &r, sizeof(r));
        srect_va = hle_va(rect_mem);
    }
    hr = load_surface_from_memory_core(surf, dstpal, drect_va, top, fmt, pitch, 0, srect_va, filter, colorkey);
    if (rect_mem) hle_free(rect_mem);
    free(decoded);
    RET(hr, 9);
}

void s_D3DXLoadSurfaceFromFileA(void) {
    /* (pDestSurface, pDestPalette, pDestRect, pSrcFile, pSrcRect, Filter, Colorkey, pSrcInfo) */
    uint32_t surf     = A32(0);
    uint32_t dstpal   = A32(1);
    uint32_t drect_va = A32(2);
    const char *path  = ASTR(3);
    uint32_t srect_va = A32(4);
    uint32_t filter   = A32(5);
    uint32_t colorkey = A32(6);
    uint32_t srcinfo_va = A32(7);
    if (!path) { log_err("LoadSurfaceFromFileA: kein Dateiname"); RET(D3DERR_INVALIDCALL, 8); }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%sLoadSurfaceFromFileA: '%s' nicht lesbar\n", TAG, path);
        if (srcinfo_va) memset(hle_ptr(srcinfo_va),0,sizeof(d3dx9_texture_info));
        RET(D3DXERR_INVALIDDATA, 8);
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)D3D9_TEX_MAX_SRC) {
        fclose(f); fprintf(stderr, "%sLoadSurfaceFromFileA: '%s' hat ungueltige Groesse %ld\n", TAG, path, sz);
        RET(D3DERR_INVALIDCALL, 8);
    }
    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); log_err("LoadSurfaceFromFileA: kein Speicher fuer Bilddaten"); RET(D3DERR_OUTOFVIDEOMEMORY, 8); }
    if (fread(data,1,(size_t)sz,f)!=(size_t)sz) {
        free(data); fclose(f); log_err("LoadSurfaceFromFileA: kurze Datei"); RET(D3DERR_INVALIDCALL, 8);
    }
    fclose(f);
    d3dx9_texture_info info;
    const uint8_t *top=NULL; uint32_t pitch=0, fmt=0; uint8_t *decoded=NULL;
    int hr = d3dx9_texture_decode_top_level(data,(uint32_t)sz,&info,&top,&pitch,&fmt,&decoded);
    if (srcinfo_va && hr==0) *(d3dx9_texture_info*)hle_ptr(srcinfo_va)=info;
    else if (srcinfo_va) memset(hle_ptr(srcinfo_va),0,sizeof(d3dx9_texture_info));
    if (hr) {
        fprintf(stderr, "%sLoadSurfaceFromFileA: Bilddaten ungueltig (hr=0x%08x)\n", TAG, hr);
        free(decoded); free(data); RET(hr,8);
    }
    void *rect_mem=NULL;
    if (!srect_va) {
        rect_mem = hle_alloc(sizeof(D3DRECT));
        if (!rect_mem) {
            log_err("LoadSurfaceFromFileA: kein Speicher fuer Quellrechteck");
            free(decoded); free(data); RET(D3DERR_OUTOFVIDEOMEMORY,8);
        }
        D3DRECT r={0,0,(int32_t)info.Width,(int32_t)info.Height};
        memcpy(rect_mem,&r,sizeof(r)); srect_va=hle_va(rect_mem);
    }
    hr = load_surface_from_memory_core(surf,dstpal,drect_va,top,fmt,pitch,0,srect_va,filter,colorkey);
    if (rect_mem) hle_free(rect_mem);
    free(decoded); free(data);
    RET(hr,8);
}

void s_D3DXLoadVolumeFromMemory(void) {
    /* (pDestVolume, pDestPalette, pDestBox, pSrcMemory, SrcFormat, SrcRowPitch, SrcSlicePitch, pSrcPalette, pSrcBox, Filter, ColorKey) */
    uint32_t vol       = A32(0);
    uint32_t dstpal    = A32(1);
    uint32_t dbox_va   = A32(2);
    uint32_t srcmem    = A32(3);
    uint32_t src_fmt   = A32(4);
    uint32_t src_row   = A32(5);
    uint32_t src_slice = A32(6);
    uint32_t srcpal    = A32(7);
    uint32_t sbox_va   = A32(8);
    uint32_t filter    = A32(9);
    uint32_t colorkey  = A32(10);
    if (!vol || !srcmem || !sbox_va) {
        log_err("LoadVolumeFromMemory: ungueltiger Ziel-, Speicher- oder Boxzeiger");
        RET(D3DERR_INVALIDCALL, 11);
    }
    if (dstpal || srcpal) { log_err("LoadVolumeFromMemory: Paletten nicht unterstützt"); RET(D3DERR_NOTAVAILABLE, 11); }
    if (colorkey) { log_err("LoadVolumeFromMemory: ColorKey nicht unterstützt"); RET(D3DERR_NOTAVAILABLE, 11); }
    if (!filter_valid(filter)) { log_err("LoadVolumeFromMemory: ungueltiger Filter"); RET(D3DERR_INVALIDCALL, 11); }
    if (!d3d9_format_is_supported(src_fmt) || src_row == 0 || src_slice == 0) {
        log_err("LoadVolumeFromMemory: ungueltiges Quellformat oder Pitch");
        RET(D3DERR_INVALIDCALL, 11);
    }
    if (d3d9_format_is_block_compressed(src_fmt)) { log_err("LoadVolumeFromMemory: block-compressed format not supported"); RET(D3DERR_NOTAVAILABLE, 11); }

    void *desc_mem = hle_alloc(sizeof(D3DVOLUME_DESC));
    void *lb_mem = hle_alloc(sizeof(D3DLOCKED_BOX));
    if (!desc_mem || !lb_mem) { if(desc_mem) hle_free(desc_mem); if(lb_mem) hle_free(lb_mem); RET(D3DERR_OUTOFVIDEOMEMORY, 11); }
    uint32_t desc_va = hle_va(desc_mem), lb_va = hle_va(lb_mem);
    uint32_t hr = hle_call_guest(hle_resolve("IDirect3DVolume9::GetDesc"), 2, (uint32_t[]){vol, desc_va});
    if (hr) {
        log_err("LoadVolumeFromMemory: GetDesc fehlgeschlagen");
        hle_free(desc_mem); hle_free(lb_mem); RET(D3DERR_INVALIDCALL, 11);
    }
    D3DVOLUME_DESC desc; memcpy(&desc, desc_mem, sizeof(desc));

    const D3DBOX *sb = (const D3DBOX *)hle_ptr(sbox_va);
    if (sb->Right <= sb->Left || sb->Bottom <= sb->Top || sb->Back <= sb->Front) {
        log_err("LoadVolumeFromMemory: ungueltige Quellbox");
        hle_free(desc_mem); hle_free(lb_mem); RET(D3DERR_INVALIDCALL, 11);
    }
    uint32_t sw = sb->Right - sb->Left, sh = sb->Bottom - sb->Top, sd = sb->Back - sb->Front;
    D3DBOX db;
    if (dbox_va) { memcpy(&db, hle_ptr(dbox_va), sizeof(db)); }
    else { db.Left=0; db.Top=0; db.Front=0; db.Right=desc.Width; db.Bottom=desc.Height; db.Back=desc.Depth; }
    if (db.Right <= db.Left || db.Bottom <= db.Top || db.Back <= db.Front) {
        log_err("LoadVolumeFromMemory: ungueltige Zielbox");
        hle_free(desc_mem); hle_free(lb_mem); RET(D3DERR_INVALIDCALL, 11);
    }
    uint32_t dw = db.Right - db.Left, dh = db.Bottom - db.Top, dd = db.Back - db.Front;
    if (dw != sw || dh != sh || dd != sd) { log_err("LoadVolumeFromMemory: scaling not supported"); hle_free(desc_mem); hle_free(lb_mem); RET(D3DERR_NOTAVAILABLE, 11); }
    if (db.Right > desc.Width || db.Bottom > desc.Height || db.Back > desc.Depth) {
        log_err("LoadVolumeFromMemory: Zielbox ausserhalb des Volumes");
        hle_free(desc_mem); hle_free(lb_mem); RET(D3DERR_INVALIDCALL, 11);
    }

    hr = hle_call_guest(hle_resolve("IDirect3DVolume9::LockBox"), 4, (uint32_t[]){vol, lb_va, dbox_va, 0});
    if (hr) {
        log_err("LoadVolumeFromMemory: LockBox fehlgeschlagen");
        hle_free(desc_mem); hle_free(lb_mem); RET(hr, 11);
    }
    D3DLOCKED_BOX lb; memcpy(&lb, lb_mem, sizeof(lb));
    uint32_t bpp = d3d9_format_bytes_per_pixel(src_fmt);
    uint32_t dbpp = d3d9_format_bytes_per_pixel(desc.Format);
    if (!bpp || !dbpp) {
        log_err("LoadVolumeFromMemory: Volumenformat nicht direkt kopierbar");
        hle_call_guest(hle_resolve("IDirect3DVolume9::UnlockBox"), 1, (uint32_t[]){vol});
        hle_free(desc_mem); hle_free(lb_mem); RET(D3DERR_NOTAVAILABLE, 11);
    }
    /* only same-format copy for now */
    if (src_fmt != desc.Format) {
        log_err("LoadVolumeFromMemory: format conversion not supported");
        hle_call_guest(hle_resolve("IDirect3DVolume9::UnlockBox"), 1, (uint32_t[]){vol});
        hle_free(desc_mem); hle_free(lb_mem); RET(D3DERR_NOTAVAILABLE, 11);
    }
    const uint8_t *src = (const uint8_t *)hle_ptr(srcmem);
    uint8_t *dst = (uint8_t *)hle_ptr(lb.pBits);
    for (uint32_t z = 0; z < dd; z++) {
        for (uint32_t y = 0; y < dh; y++) {
            const uint8_t *srow = src + (size_t)(sb->Front + z) * src_slice + (size_t)(sb->Top + y) * src_row + (size_t)sb->Left * bpp;
            uint8_t *drow = dst + (size_t)z * (uint32_t)lb.SlicePitch + (size_t)y * (uint32_t)lb.RowPitch;
            memcpy(drow, srow, (size_t)dw * bpp);
        }
    }
    hle_call_guest(hle_resolve("IDirect3DVolume9::UnlockBox"), 1, (uint32_t[]){vol});
    hle_free(desc_mem); hle_free(lb_mem);
    RET(D3D_OK, 11);
}

/* ------------------------------------------------------------- FromSurface */

/* (dst, dstpal, dstrect, src, srcpal, srcrect, filter, colorkey) -- 8 args. */
void s_D3DXLoadSurfaceFromSurface(void) {
    uint32_t dsrf     = A32(0);
    uint32_t dstpal   = A32(1);
    uint32_t drect_va = A32(2);
    uint32_t ssrf     = A32(3);
    uint32_t srcpal   = A32(4);
    uint32_t srect_va = A32(5);
    uint32_t filter   = A32(6);
    uint32_t colorkey = A32(7);

    if (!is_surf9(dsrf) || !is_surf9(ssrf)) {
        log_err("LoadSurfaceFromSurface: kein gültiges Oberflächen-Objekt");
        RET(D3DERR_INVALIDCALL, 8);
    }
    if (dstpal || srcpal) {
        log_err("LoadSurfaceFromSurface: Paletten nicht unterstützt");
        RET(D3DERR_NOTAVAILABLE, 8);
    }
    if (!filter_valid(filter)) {
        log_err("LoadSurfaceFromSurface: ungültiger Filter");
        RET(D3DERR_INVALIDCALL, 8);
    }

    D3DSURFACE_DESC dd, sd;
    if (surf_get_desc(dsrf, &dd) != D3D_OK || surf_get_desc(ssrf, &sd) != D3D_OK) {
        log_err("LoadSurfaceFromSurface: GetDesc fehlgeschlagen");
        RET(D3DERR_INVALIDCALL, 8);
    }
    D3DRECT drect, srect;
    int dempty, sempty;
    if (!surface_rect(drect_va, dd.Width, dd.Height, &drect, &dempty) ||
        !surface_rect(srect_va, sd.Width, sd.Height, &srect, &sempty)) {
        log_err("LoadSurfaceFromSurface: ungültiges Rechteck");
        RET(D3DERR_INVALIDCALL, 8);
    }
    if (dempty || sempty) RET(D3D_OK, 8);

    D3DLOCKED_RECT sl, dl;
    /* Both are IDirect3DSurface9 objects: LockRect(this, out, rect, flags). */
    if (surf_lock_rect(ssrf, 0x10u /* D3DLOCK_READONLY */, &sl) != D3D_OK || !sl.pBits) {
        log_err("LoadSurfaceFromSurface: Quelle nicht sperrbar");
        RET(D3DERR_INVALIDCALL, 8);
    }
    if (surf_lock_rect(dsrf, 0, &dl) != D3D_OK || !dl.pBits) {
        surf_unlock_rect(ssrf);
        log_err("LoadSurfaceFromSurface: Ziel nicht sperrbar");
        RET(D3DERR_INVALIDCALL, 8);
    }
    size_t soff;
    if (!src_offset((uint32_t)srect.x1, (uint32_t)srect.y1,
                    (uint32_t)sl.Pitch, sd.Format, &soff)) {
        surf_unlock_rect(dsrf);
        surf_unlock_rect(ssrf);
        log_err("LoadSurfaceFromSurface: block-komprimiertes Quellrechteck nicht blockausgerichtet");
        RET(D3DERR_INVALIDCALL, 8);
    }
    int rc = copy_rect((const uint8_t *)hle_ptr(sl.pBits) + soff,
                       (uint32_t)sl.Pitch, sd.Format,
                       (uint8_t *)hle_ptr(dl.pBits) +
                           (size_t)drect.y1 * (size_t)dl.Pitch +
                           (size_t)drect.x1 * dst_bpp(dd.Format),
                       (uint32_t)dl.Pitch, dd.Format,
                       (uint32_t)(srect.x2 - srect.x1), (uint32_t)(srect.y2 - srect.y1),
                       (uint32_t)(drect.x2 - drect.x1), (uint32_t)(drect.y2 - drect.y1),
                       colorkey, colorkey != 0u,
                       filter_mode(filter_resolve(filter)));
    surf_unlock_rect(dsrf);
    surf_unlock_rect(ssrf);
    RET(rc == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 8);
}

/* ------------------------------------------------------------- FilterTexture */

/* D3DXFilterTexture(texture, palette, srclevel, filter) -- 4 args.
 * Box-filter each lower mip level from the one above it. */
void s_D3DXFilterTexture(void) {
    uint32_t tex      = A32(0);
    uint32_t palette  = A32(1);
    uint32_t srclevel = A32(2);
    uint32_t filter   = A32(3);

    if (!is_tex9(tex)) {
        log_err("FilterTexture: keine Textur");
        RET(D3DERR_INVALIDCALL, 4);
    }
    if (palette) {
        log_err("FilterTexture: Palette nicht unterstützt");
        RET(D3DERR_NOTAVAILABLE, 4);
    }
    if (!filter_valid(filter)) {
        log_err("FilterTexture: ungültiger Filter");
        RET(D3DERR_INVALIDCALL, 4);
    }

    uint32_t levels = tex_level_count(tex);
    if (srclevel == D3DX_DEFAULT) srclevel = 0;
    if (!levels || srclevel >= levels) {
        log_err("FilterTexture: ungültiges SrcLevel");
        RET(D3DERR_INVALIDCALL, 4);
    }
    if (srclevel + 1 >= levels) RET(D3D_OK, 4);      /* no lower levels */

    D3DSURFACE_DESC d0;
    if (tex_level_desc(tex, srclevel, &d0) != 0 ||
        !d3d9_format_is_supported(d0.Format) || !dst_bpp(d0.Format)) {
        log_err("FilterTexture: Format nicht filterbar");
        RET(D3DERR_NOTAVAILABLE, 4);
    }

    int mode = filter_mode(filter_resolve(filter));
    for (uint32_t m = srclevel + 1u; m < levels; m++) {
        D3DSURFACE_DESC sm, dm;
        if (tex_level_desc(tex, m - 1u, &sm) != 0 || tex_level_desc(tex, m, &dm) != 0 ||
            sm.Format != dm.Format) {
            log_err("FilterTexture: Mip-Level passt nicht");
            RET(D3DERR_INVALIDCALL, 4);
        }
        D3DLOCKED_RECT sl, dl;
        if (tex_lock_level(tex, m - 1u, &sl) != 0) {
            log_err("FilterTexture: LockRect (Quelle) fehlgeschlagen");
            RET(D3DERR_INVALIDCALL, 4);
        }
        if (tex_lock_level(tex, m, &dl) != 0) {
            tex_unlock_level(tex, m - 1u);
            log_err("FilterTexture: LockRect (Ziel) fehlgeschlagen");
            RET(D3DERR_INVALIDCALL, 4);
        }
        uint32_t sw = sm.Width, sh = sm.Height, dw = dm.Width, dh = dm.Height;
        uint32_t src_pitch = (uint32_t)sl.Pitch, dst_pitch = (uint32_t)dl.Pitch;
        const uint8_t *srcbits = (const uint8_t *)hle_ptr(sl.pBits);
        uint8_t *dstbits = (uint8_t *)hle_ptr(dl.pBits);

        /* For a 2:1 mip chain the box average of the aligned 2x2 source
         * neighbourhood is exactly what copy_rect(mode=1) computes.
         * Copy while both levels are locked; UnlockRect invalidates pBits. */
        int rc = mode
            ? copy_rect(srcbits, src_pitch, sm.Format, dstbits, dst_pitch,
                        dm.Format, sw, sh, dw, dh, 0, 0, 1)
            : copy_rect(srcbits, src_pitch, sm.Format, dstbits, dst_pitch,
                        dm.Format, sw, sh, dw, dh, 0, 0, 0);
        tex_unlock_level(tex, m);
        tex_unlock_level(tex, m - 1u);
        if (rc != 0) {
            log_err("FilterTexture: Mip-Level konnte nicht gefiltert werden");
            RET(D3DERR_NOTAVAILABLE, 4);
        }
    }
    RET(D3D_OK, 4);
}

/* ------------------------------------------------------------- file output */

/* 24-bit BMP: BGR rows bottom-to-top, each row padded to a multiple of 4. */
static int save_bmp(const char *name, const D3DSURFACE_DESC *d,
                    const uint8_t *src, uint32_t pitch,
                    uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t rowsz = (w * 3u + 3u) & ~3u;
    size_t off;
    if (!src_offset(x, y, pitch, d->Format, &off)) return -1;
    uint8_t *row = (uint8_t *)malloc((size_t)w * 4u ? (size_t)w * 4u : 4);
    /* zeroed so BMP row padding bytes are deterministic */
    uint8_t *out = (uint8_t *)calloc(1, (size_t)rowsz * h ? (size_t)rowsz * h : 1);
    if (!row || !out) { free(row); free(out); return -1; }
    int bad = 0;
    for (uint32_t j = 0; j < h; j++) {
        const uint8_t *srow = src + off + (size_t)j * pitch;
        if (d3d9_format_to_bgra8(d->Format, srow, pitch, w, 1, row, w * 4u) != 0) {
            bad = 1;
            break;
        }
        uint8_t *o = out + (size_t)(h - 1u - j) * rowsz;      /* bottom-up */
        for (uint32_t i = 0; i < w; i++) {
            /* d3d9_format_to_bgra8 returns B in the lowest byte. */
            o[i * 3u + 0u] = row[i * 4u + 0u];    /* B */
            o[i * 3u + 1u] = row[i * 4u + 1u];    /* G */
            o[i * 3u + 2u] = row[i * 4u + 2u];    /* R */
        }
    }
    free(row);
    if (bad) { free(out); return -1; }

    FILE *f = fopen(name, "wb");
    if (!f) { free(out); return -1; }
    uint32_t imgsz = rowsz * h, fsize = 54u + imgsz;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);                    /* reserved fields must be 0 */
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)fsize;        hdr[3] = (uint8_t)(fsize >> 8);
    hdr[4] = (uint8_t)(fsize >> 16); hdr[5] = (uint8_t)(fsize >> 24);
    hdr[10] = 54;                                  /* offset to pixel data */
    hdr[14] = 40;                                  /* BITMAPINFOHEADER size */
    uint32_t iw = w, ih = h;
    memcpy(hdr + 18, &iw, 4); memcpy(hdr + 22, &ih, 4);
    uint16_t pl = 1, bc = 24;
    uint32_t comp = 0;
    memcpy(hdr + 26, &pl, 2); memcpy(hdr + 28, &bc, 2); memcpy(hdr + 30, &comp, 4);
    memcpy(hdr + 34, &imgsz, 4);
    int32_t ppm = 2837911;                          /* 96 dpi */
    memcpy(hdr + 38, &ppm, 4); memcpy(hdr + 42, &ppm, 4);
    int ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr &&
             fwrite(out, 1, imgsz, f) == imgsz;
    free(out);
    fclose(f);
    return ok ? 0 : -1;
}

/* DDS_PIXELFORMAT: size, flags, fourCC, RGBBitCount, R/G/B/A masks. */
static int dds_pixfmt(uint32_t fmt, uint8_t *pf) {
    /* DDPF_ALPHAPIXELS 0x1, DDPF_ALPHA 0x2, DDPF_FOURCC 0x4,
     * DDPF_RGB 0x40, DDPF_LUMINANCE 0x20000. */
    uint32_t v[8] = { 32, 0, 0, 0, 0, 0, 0, 0 };
    switch (fmt) {
    case D3DFMT_A8R8G8B8: v[1]=0x41;    v[3]=32; v[4]=0x00FF0000u; v[5]=0x0000FF00u; v[6]=0x000000FFu; v[7]=0xFF000000u; break;
    case D3DFMT_X8R8G8B8: v[1]=0x40;    v[3]=32; v[4]=0x00FF0000u; v[5]=0x0000FF00u; v[6]=0x000000FFu; break;
    case D3DFMT_R5G6B5:   v[1]=0x40;    v[3]=16; v[4]=0x0000F800u; v[5]=0x000007E0u; v[6]=0x0000001Fu; break;
    case D3DFMT_X1R5G5B5: v[1]=0x40;    v[3]=15; v[4]=0x00007C00u; v[5]=0x000003E0u; v[6]=0x0000001Fu; break;
    case D3DFMT_A1R5G5B5: v[1]=0x41;    v[3]=16; v[4]=0x00007C00u; v[5]=0x000003E0u; v[6]=0x0000001Fu; v[7]=0x00008000u; break;
    case D3DFMT_X4R4G4B4: v[1]=0x40;    v[3]=12; v[4]=0x00000F00u; v[5]=0x000000F0u; v[6]=0x0000000Fu; break;
    case D3DFMT_A4R4G4B4: v[1]=0x41;    v[3]=16; v[4]=0x00000F00u; v[5]=0x000000F0u; v[6]=0x0000000Fu; v[7]=0x0000F000u; break;
    case D3DFMT_A8:       v[1]=0x2;     v[3]=8;  v[7]=0x000000FFu; break;
    case D3DFMT_L8:       v[1]=0x20000; v[3]=8;  v[4]=0x000000FFu; break;
    case D3DFMT_A8L8:     v[1]=0x20001; v[3]=16; v[4]=0x000000FFu; v[7]=0x0000FF00u; break;
    case D3DFMT_DXT1:
    case D3DFMT_DXT3:
    case D3DFMT_DXT5:
        v[1]=0x4; v[2]=fmt; v[4]=0; v[5]=0; v[6]=0; v[7]=0;
        break;
    default: return -1;
    }
    memcpy(pf, v, 32);
    return 0;
}

static int save_dds(const char *name, const D3DSURFACE_DESC *d,
                    const uint8_t *src, uint32_t pitch,
                    uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t fmt = d->Format;
    size_t off;
    uint32_t lin = 0, row_bytes = 0;
    int block = d3d9_format_is_block_compressed(fmt);

    /* A subrect must start at a pixel (or block) boundary, and the format has
     * to be one whose raw bytes are meaningful in a DDS file. */
    if (!src_offset(x, y, pitch, fmt, &off)) return -1;
    if (block) {
        lin = d3d9_format_level_size(fmt, w, h);
        if (!lin) return -1;
    } else {
        uint32_t bpp = d3d9_format_bytes_per_pixel(fmt);
        if (!bpp || w > 0xFFFFFFFFu / bpp) return -1;
        row_bytes = w * bpp;                        /* tight rows, not source pitch */
    }
    uint8_t pf[32];
    if (dds_pixfmt(fmt, pf) != 0) return -1;

    uint8_t hdr[124];
    memset(hdr, 0, sizeof hdr);
    hdr[0]='D'; hdr[1]='D'; hdr[2]='S'; hdr[3]=' ';
    uint32_t p124 = 124;
    memcpy(hdr + 4, &p124, 4);
    uint32_t flags = 0xFu;                          /* CAPS|HEIGHT|WIDTH|PITCH */
    memcpy(hdr + 8, &flags, 4);
    memcpy(hdr + 12, &h, 4); memcpy(hdr + 16, &w, 4);
    memcpy(hdr + 26, block ? &lin : &row_bytes, 4);
    memcpy(hdr + 76, pf, sizeof pf);
    uint32_t caps = 0x1000u;                        /* DDSCAPS_TEXTURE */
    memcpy(hdr + 116, &caps, 4);

    FILE *f = fopen(name, "wb");
    if (!f) return -1;
    int ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    if (block) {
        if (ok) ok = fwrite(src + off, 1, lin, f) == lin;
    } else {
        for (uint32_t j = 0; ok && j < h; j++)
            ok = fwrite(src + off + (size_t)j * pitch, 1, row_bytes, f) == row_bytes;
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* (file, fileformat, surface, palette, srcrect) -- 5 args. The 5th argument
 * is a RECT*, NOT a filter; there is no filter parameter on this function. */
void s_D3DXSaveSurfaceToFileA(void) {
    const char *name   = ASTR(0);
    uint32_t ffmt      = A32(1);
    uint32_t surf      = A32(2);
    uint32_t palette   = A32(3);
    uint32_t srect_va  = A32(4);

    if (!name || !is_surf9(surf)) {
        log_err("SaveSurfaceToFileA: kein gültiges Oberflächen-Objekt");
        RET(D3DERR_INVALIDCALL, 5);
    }
    if (palette) {
        log_err("SaveSurfaceToFileA: Palette nicht unterstützt");
        RET(D3DERR_NOTAVAILABLE, 5);
    }
    if (ffmt != D3DXIFF_BMP && ffmt != D3DXIFF_DDS) {
        log_err("SaveSurfaceToFileA: Dateiformat nicht unterstützt");
        RET(D3DERR_NOTAVAILABLE, 5);
    }

    D3DSURFACE_DESC desc;
    if (surf_get_desc(surf, &desc) != D3D_OK) {
        log_err("SaveSurfaceToFileA: GetDesc fehlgeschlagen");
        RET(D3DERR_INVALIDCALL, 5);
    }
    D3DRECT srect;
    int sempty;
    if (!surface_rect(srect_va, desc.Width, desc.Height, &srect, &sempty) || sempty) {
        log_err("SaveSurfaceToFileA: ungültiges Quellrechteck");
        RET(D3DERR_INVALIDCALL, 5);
    }

    D3DLOCKED_RECT lr;
    if (surf_lock_rect(surf, 0x10u, &lr) != D3D_OK || !lr.pBits) {
        log_err("SaveSurfaceToFileA: LockRect fehlgeschlagen");
        RET(D3DERR_INVALIDCALL, 5);
    }
    uint32_t x = (uint32_t)srect.x1, y = (uint32_t)srect.y1;
    uint32_t w = (uint32_t)(srect.x2 - srect.x1), h = (uint32_t)(srect.y2 - srect.y1);
    int rc = ffmt == D3DXIFF_BMP
        ? save_bmp(name, &desc, hle_ptr(lr.pBits), (uint32_t)lr.Pitch, x, y, w, h)
        : save_dds(name, &desc, hle_ptr(lr.pBits), (uint32_t)lr.Pitch, x, y, w, h);
    surf_unlock_rect(surf);
    if (rc) log_err("SaveSurfaceToFileA: Schreiben fehlgeschlagen");
    RET(rc == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 5);
}
