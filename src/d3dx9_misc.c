/*
 * d3dx9_misc.c - Implementation of miscellaneous D3DX9 utilities:
 * D3DXCheckVersion, D3DXDebugMute, and CreateEffectFromFileExA/W.
 *
 * Source: Public Microsoft DirectX 9 SDK documentation & headers (d3dx9core.h,
 * d3dx9effect.h) and Wine (dlls/d3dx9_36/d3dx9_36_main.c, dlls/d3dx9_36/effect.c).
 * No code copied from Microsoft DLLs or proprietary sources.
 */

#include "d3dx9_misc.h"
#include "d3d9_hle.h"
#include "d3dx9_fxtext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D3DXERR_NOTAVAILABLE 0x88760866u
#define E_FAIL_              0x80004005u
#define E_OUTOFMEMORY_       0x8007000Eu

uint32_t hle_d3d9fx_effect_from_source(uint32_t dev, const char *src, size_t len,
                                       uint32_t pool, uint32_t macros, uint32_t include,
                                       uint32_t flags, uint32_t *out, uint32_t ppErr);

/*
 * D3DXCheckVersion(UINT D3DSDKVersion, UINT D3DXSDKVersion) -> BOOL
 *
 * Checks that the application was compiled against compatible DirectX/D3DX headers.
 * D3DSDKVersion must match D3D_SDK_VERSION (32).
 * D3DXSDKVersion: accepts any D3DX9 release version 24..43, or D3DX_SDK_VERSION (43).
 * Returns TRUE (1) on match, FALSE (0) on mismatch.
 * Stdcall: 2 arguments (RET 2).
 */
void s_D3DXCheckVersion(void) {
    uint32_t d3d_ver = A32(0);
    uint32_t d3dx_ver = A32(1);

    if (d3d_ver != D3D_SDK_VERSION) {
        fprintf(stderr, "[d3dx9] D3DXCheckVersion: D3D SDK version mismatch (got %u, expected %u)\n",
                d3d_ver, D3D_SDK_VERSION);
        RET(0, 2);
        return;
    }

    if (d3dx_ver < 24u || d3dx_ver > 43u) {
        fprintf(stderr, "[d3dx9] D3DXCheckVersion: D3DX SDK version out of supported range 24..43 (got %u)\n",
                d3dx_ver);
        RET(0, 2);
        return;
    }

    RET(1, 2);
}

/*
 * D3DXDebugMute(BOOL Mute) -> BOOL
 *
 * Turns on or off D3DX debug output.
 * Returns the previous value of the Mute state.
 * Stdcall: 1 argument (RET 1).
 */
static int s_debug_muted = 0;

void s_D3DXDebugMute(void) {
    uint32_t mute = A32(0);
    uint32_t prev = s_debug_muted ? 1u : 0u;
    s_debug_muted = (mute != 0);
    RET(prev, 1);
}

/* Helper to convert UTF-16 to UTF-8 */
static size_t misc_utf16_to_utf8(const uint16_t *src, int count, char *dst, size_t cap) {
    size_t n = 0;
    if (!src || cap == 0) return 0;
    if (count < 0) { count = 0; while (src[count]) count++; }
    for (int i = 0; i < count; i++) {
        uint32_t cp = src[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < count &&
            src[i + 1] >= 0xDC00u && src[i + 1] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (src[i + 1] - 0xDC00u);
            i++;
        } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;
        }
        if (cp < 0x80u) {
            if (n + 1 >= cap) break;
            dst[n++] = (char)cp;
        } else if (cp < 0x800u) {
            if (n + 2 >= cap) break;
            dst[n++] = (char)(0xC0u | (cp >> 6));
            dst[n++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (n + 3 >= cap) break;
            dst[n++] = (char)(0xE0u | (cp >> 12));
            dst[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[n++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (n + 4 >= cap) break;
            dst[n++] = (char)(0xF0u | (cp >> 18));
            dst[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            dst[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[n++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    if (n < cap) dst[n] = 0;
    else dst[cap - 1] = 0;
    return n;
}

static void set_effect_err(uint32_t ppErr, const char *msg) {
    if (!ppErr) return;
    uint32_t s = hle_guest_strdup(msg ? msg : "effect file error");
    if (s) MEM32(ppErr) = s;
}

/*
 * Core helper: creates effect from a file path using hle_d3d9fx_effect_from_source.
 * Respects pSkipConstants: if non-NULL, logs and returns D3DXERR_NOTAVAILABLE.
 */
static uint32_t create_effect_from_filepath(uint32_t dev, const char *name,
                                           uint32_t defs, uint32_t inc,
                                           uint32_t skip, uint32_t flags,
                                           uint32_t pool, uint32_t pp, uint32_t err) {
    if (pp) MEM32(pp) = 0;
    if (!name || name[0] == '\0') {
        set_effect_err(err, "no effect file");
        return D3DERR_INVALIDCALL;
    }
    if (skip) {
        fprintf(stderr, "[d3dx9] CreateEffectFromFileEx: pSkipConstants not supported\n");
        set_effect_err(err, "pSkipConstants not supported");
        return D3DXERR_NOTAVAILABLE;
    }

    /* Look up file path via HLE filesystem */
    char host_path[1024];
    const char *open_path = name;
    if (hle_host_path(name, host_path, sizeof host_path)) {
        open_path = host_path;
    }

    FILE *f = fopen(open_path, "rb");
    if (!f) {
        if (open_path != name) f = fopen(name, "rb");
    }
    if (!f) {
        set_effect_err(err, "cannot open effect file");
        return E_FAIL_;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_effect_err(err, "cannot seek effect file");
        return E_FAIL_;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > (long)D3DX9_FXTEXT_MAX_SRC) {
        fclose(f);
        set_effect_err(err, "effect file too large or empty");
        return D3DERR_INVALIDCALL;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        set_effect_err(err, "out of memory");
        return E_OUTOFMEMORY_;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    /* Delegate to the source compiler (same engine backing D3DXCreateEffectEx) */
    uint32_t hr = hle_d3d9fx_effect_from_source(dev, buf, (uint32_t)rd, pool,
                                                defs, inc, flags, hle_ptr(pp), err);
    free(buf);
    return hr;
}

/*
 * D3DXCreateEffectFromFileExA(pDevice, pSrcFile, pDefines, pInclude,
 *                             pSkipConstants, Flags, pPool, ppEffect,
 *                             ppCompilationErrors) -> HRESULT
 * Stdcall: 9 arguments (RET 9).
 */
void s_D3DXCreateEffectFromFileExA(void) {
    uint32_t dev   = A32(0);
    uint32_t path  = A32(1);
    uint32_t defs  = A32(2);
    uint32_t inc   = A32(3);
    uint32_t skip  = A32(4);
    uint32_t flags = A32(5);
    uint32_t pool  = A32(6);
    uint32_t pp    = A32(7);
    uint32_t err   = A32(8);

    const char *name = path ? (const char *)hle_ptr(path) : NULL;
    uint32_t hr = create_effect_from_filepath(dev, name, defs, inc, skip, flags, pool, pp, err);
    RET(hr, 9);
}

/*
 * D3DXCreateEffectFromFileExW(pDevice, pSrcFile, pDefines, pInclude,
 *                             pSkipConstants, Flags, pPool, ppEffect,
 *                             ppCompilationErrors) -> HRESULT
 * Stdcall: 9 arguments (RET 9).
 */
void s_D3DXCreateEffectFromFileExW(void) {
    uint32_t dev   = A32(0);
    uint32_t path  = A32(1);
    uint32_t defs  = A32(2);
    uint32_t inc   = A32(3);
    uint32_t skip  = A32(4);
    uint32_t flags = A32(5);
    uint32_t pool  = A32(6);
    uint32_t pp    = A32(7);
    uint32_t err   = A32(8);

    const uint16_t *wpath = path ? (const uint16_t *)hle_ptr(path) : NULL;
    char name[1024];
    name[0] = 0;
    if (wpath) {
        misc_utf16_to_utf8(wpath, -1, name, sizeof name);
    }
    uint32_t hr = create_effect_from_filepath(dev, wpath ? name : NULL, defs, inc, skip, flags, pool, pp, err);
    RET(hr, 9);
}

/*
 * D3DXCreateEffectFromFileW(pDevice, pSrcFile, pDefines, pInclude,
 *                           Flags, pPool, ppEffect, ppCompilationErrors) -> HRESULT
 * Stdcall: 8 arguments (RET 8).
 */
void s_D3DXCreateEffectFromFileW(void) {
    uint32_t dev   = A32(0);
    uint32_t path  = A32(1);
    uint32_t defs  = A32(2);
    uint32_t inc   = A32(3);
    uint32_t flags = A32(4);
    uint32_t pool  = A32(5);
    uint32_t pp    = A32(6);
    uint32_t err   = A32(7);

    const uint16_t *wpath = path ? (const uint16_t *)hle_ptr(path) : NULL;
    char name[1024];
    name[0] = 0;
    if (wpath) {
        misc_utf16_to_utf8(wpath, -1, name, sizeof name);
    }
    uint32_t hr = create_effect_from_filepath(dev, wpath ? name : NULL, defs, inc, 0, flags, pool, pp, err);
    RET(hr, 8);
}
