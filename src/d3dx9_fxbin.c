/*
 * d3dx9_fxbin.c - parser for compiled D3D9 effect containers and the
 * D3DXCreateEffectFromMemory entry point. The byte layout is the one observed
 * in the shipped d3dx9_34.dll parser; see d3dx9_fxbin_hle.h.
 *
 * Untrusted input: every word, offset, count and string is bounded against
 * the buffer before use. No code from another runtime was copied.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "d3dx9_fxbin_hle.h"

#define D3DERR_INVALIDCALL 0x8876086Cu
#define FX_MAX_CTAB        (16u << 20)
#define FX_MAX_ANNOT       65536u
#define FX_MAX_STATE       65536u
#define FX_MAX_COUNT       65536u

typedef struct { const uint8_t *d; uint32_t n, w; } reader_t;

static int word(reader_t *r, uint32_t *out) {
    uint32_t o = r->w * 4u;
    if (r->w > (r->n >> 2) || o > r->n - 4u) return 0;
    const uint8_t *p = r->d + o;
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    r->w++;
    return 1;
}
static int skip(reader_t *r, uint32_t n) {
    if (n > 0x3FFFFFFFu || r->w > (0x3FFFFFFFu - n)) return 0;
    uint32_t end = r->w + n;
    if (end > (r->n >> 2)) return 0;
    r->w = end;
    return 1;
}

/* A name is a byte offset into the CTAB blob; offset 0 is its first string. */
static const char *ctab_name(const uint8_t *ctab, uint32_t size, uint32_t off) {
    if (off >= size) return 0;
    const char *s = (const char *)ctab + off;
    if (!memchr(s, 0, (size_t)(size - off))) return 0;
    return s;
}

/* Consume annotation_count pairs of DWORDs, validating them on the way. */
static int skip_annots(reader_t *r, const uint8_t *ctab, uint32_t csize, uint32_t count) {
    if (count > FX_MAX_ANNOT) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t name, value;
        if (!word(r, &name) || !word(r, &value)) return 0;
        if (!ctab_name(ctab, csize, name)) return 0;
    }
    return 1;
}

int hle_d3d9fxbin_parse(const uint8_t *d, uint32_t n, fxbin_desc_t *out) {
    reader_t r;
    uint32_t token, csize, nparam, ntech, nobj, ngrp;
    const uint8_t *ctab;
    if (!d || !out || n < 24u) return 0;
    memset(out, 0, sizeof *out);
    r.d = d, r.n = n, r.w = 0;

    if (!word(&r, &token)) return 0;
    if (token == D3DXFX_TOKEN_XNA4) {               /* XNA 4 wrapper */
        uint32_t off;
        if (!word(&r, &off) || off < 8u || (off & 3u) || off > n - 8u) return 0;
        r.d += off, r.n -= off, r.w = 0;
        if (!word(&r, &token)) return 0;
    }
    if (token != D3DXFX_TOKEN) return 0;
    if (!word(&r, &csize) || csize > FX_MAX_CTAB || csize > r.n) return 0;
    ctab = r.d + r.w * 4u;                          /* CTAB starts at the current DWORD */
    if (!skip(&r, (csize + 3u) >> 2)) return 0;
    if (!word(&r, &nparam) || !word(&r, &ntech) || !word(&r, &nobj) || !word(&r, &ngrp)) return 0;
    if (nparam > FX_MAX_COUNT || ntech > FX_MAX_COUNT || nobj > FX_MAX_COUNT || ngrp > FX_MAX_COUNT) return 0;

    /* Parameters: name, semantic, type flags, annotation count. The table here
     * is deliberately small; a bigger effect is refused, not silently chopped. */
    if (nparam > D3DXFX_MAX_PARAM) return 0;
    for (uint32_t i = 0; i < nparam; i++) {
        uint32_t name, semantic, flags, annot;
        if (!word(&r, &name) || !word(&r, &semantic) || !word(&r, &flags) || !word(&r, &annot)) return 0;
        const char *s = ctab_name(ctab, csize, name);
        if (!s) return 0;
        snprintf(out->param[out->nparam++], D3DXFX_NAME_LEN, "%s", s);
        if (!skip_annots(&r, ctab, csize, annot)) return 0;
    }
    /* Techniques: name, annotation count, pass count, then their passes. */
    if (ntech > D3DXFX_MAX_TECH) return 0;
    for (uint32_t t = 0; t < ntech; t++) {
        uint32_t name, annot, npass;
        if (!word(&r, &name) || !word(&r, &annot) || !word(&r, &npass)) return 0;
        const char *s = ctab_name(ctab, csize, name);
        if (!s) return 0;
        snprintf(out->tech[out->ntech], D3DXFX_NAME_LEN, "%s", s);
        if (npass > D3DXFX_MAX_PASS) return 0;
        if (!skip_annots(&r, ctab, csize, annot)) return 0;
        for (uint32_t p = 0; p < npass; p++) {
            uint32_t pname, pannot, nstate;
            if (!word(&r, &pname) || !word(&r, &pannot) || !word(&r, &nstate)) return 0;
            const char *ps = ctab_name(ctab, csize, pname);
            if (!ps) return 0;
            snprintf(out->pass[out->ntech][out->npass[out->ntech]++], D3DXFX_NAME_LEN, "%s", ps);
            if (!skip_annots(&r, ctab, csize, pannot)) return 0;
            if (nstate > FX_MAX_STATE) return 0;
            for (uint32_t k = 0; k < nstate; k++) {
                uint32_t stype, vtype, sname, svalue;
                if (!word(&r, &stype) || !word(&r, &vtype) || !word(&r, &sname) || !word(&r, &svalue)) return 0;
            }
        }
        out->ntech++;
    }
    return 1;
}

uint32_t hle_d3d9fx_effect_from_desc(uint32_t pool, const fxbin_desc_t *desc) {
    uint32_t effect;
    if (!desc) return 0;
    effect = hle_d3d9fx_effect_new(pool);
    if (!effect) return 0;
    for (uint32_t p = 0; p < desc->nparam; p++)
        hle_d3d9fx_pool_param(pool, desc->param[p], 1);
    for (uint32_t t = 0; t < desc->ntech; t++) {
        const char *passes[D3DXFX_MAX_PASS];
        for (uint32_t k = 0; k < desc->npass[t]; k++) passes[k] = desc->pass[t][k];
        hle_d3d9fx_effect_add_tech(effect, desc->tech[t], passes, (int)desc->npass[t]);
    }
    return effect;
}

static void f_create_from_memory(void) {            /* D3DXCreateEffectFromMemory: 8 args */
    uint32_t data = A32(0), size = A32(1), pool = A32(5), out = A32(6);
    uint32_t r = D3DERR_INVALIDCALL;
    if (data && size && out) {
        fxbin_desc_t desc;
        if (hle_d3d9fxbin_parse((const uint8_t *)hle_ptr(data), size, &desc)) {
            uint32_t effect = hle_d3d9fx_effect_from_desc(pool, &desc);
            if (effect) { MEM32(out) = effect, r = 0u; }
        }
    }
    RET(r, 8);
}

const win32hle_shim win32hle_d3d9fxbin[] = {
    { "D3DXCreateEffectFromMemory", f_create_from_memory },
    { 0, 0 }
};
