/*
 * d3dx9_fxsource.c - build ID3DXEffect objects from .fx SOURCE text.
 *
 * The text is parsed with d3dx9_fxtext, each pass's HLSL function is compiled
 * with D3DXCompileShader (vkd3d bridge) to legacy D3D9 bytecode, and the
 * resulting shader objects are bound by the effect state machine. Parameter
 * registers come from the compiler's ID3DXConstantTable, never from declaration
 * order. Defines, the include interface and compile flags are passed through
 * to every shader compile. The banned binary FX container (9b20a12 /
 * d3dx9_fxbin) is neither read nor registered. Anything the text parser or the
 * compiler cannot do is an error with a log.
 */
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3dx9_fxtext.h"
#include "d3dx9_effect_hle.h"
#include "d3dx9_fxvtbl.h"

extern uint32_t hle_d3d9fx_compile_source(const char *src, size_t src_len,
    const char *src_name, uint32_t macros_va, uint32_t include_va,
    const char *entry, const char *profile, uint32_t flags,
    uint32_t ppShader, uint32_t ppErr, uint32_t ppCT);

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define E_FAIL 0x80004005u
#define D3DXERR_NOTAVAILABLE 0x88760866u

static void set_err(uint32_t ppErr, const char *msg) {
    if (!ppErr) return;
    uint32_t s = hle_guest_strdup(msg ? msg : "fx source error");
    if (s) MEM32(ppErr) = s;
}

static const d3dx9_fx_function *find_function(const d3dx9_fx_effect *fx, const char *name) {
    if (!fx || !name) return NULL;
    for (int i = 0; i < fx->function_count; i++)
        if (strcmp(fx->functions[i].name, name) == 0) return &fx->functions[i];
    return NULL;
}

/* ------------------------------------------------------------ parameters */

/* Store a declared parameter in the effect pool. Samplers get the next free
 * sampler stage. Returns 0 on failure. */
static int declare_param(uint32_t pool, const d3dx9_fx_param *p, uint32_t *stage) {
    uint32_t h = hle_d3d9fx_pool_param(pool, p->name, 1);
    if (!h) return 0;
    int is_sampler = !strncmp(p->type, "sampler", 7);
    int is_texture = !strncmp(p->type, "texture", 7) || is_sampler;
    if (is_texture) {
        /* A plain `texture` declaration is just a resource; only a sampler
         * occupies a sampler stage (the shader's s0, s1, ...). */
        if (is_sampler) {
            hle_d3d9fx_param_set_stage(h, *stage);
            (*stage)++;
        }
        return hle_d3d9fx_param_set_texture(h, 0) == D3D_OK;
    }
    const char *s = p->default_value;
    while (*s == ' ' || *s == '\t') s++;
    if (!strncmp(p->type, "float4", 6)) {
        float f[4] = { 0, 0, 0, 0 };
        /* float4(a,b,c,d): take the numbers inside the constructor. */
        const char *q = strchr(s, '(');
        if (q) {
            for (int k = 0; k < 4; k++) {
                q++;
                while (*q == ' ' || *q == ',') q++;
                if (!isdigit((unsigned char)*q) && *q != '-' && *q != '.') break;
                f[k] = (float)atof(q);
                while (*q && *q != ',' && *q != ')') q++;
            }
        }
        return hle_d3d9fx_param_set_vector(h, f) == D3D_OK;
    }
    float v = 0.0f;
    if (*s) v = (float)atof(s);
    return hle_d3d9fx_param_set_float(h, v) == D3D_OK;
}

/* ------------------------------------------------------------ device states */

/* Public D3D9 render-state values used by the pass states we accept. */
enum {
    RS_ZENABLE = 7, RS_FILLMODE = 8, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15,
    RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23,
    RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_DITHERENABLE = 26, RS_ALPHABLENDENABLE = 27
};
/* Public D3DSAMP_* values. */
enum {
    SAMP_ADDRESSU = 1, SAMP_ADDRESSV = 2, SAMP_ADDRESSW = 3, SAMP_BORDERCOLOR = 4,
    SAMP_MAGFILTER = 5, SAMP_MINFILTER = 6, SAMP_MIPFILTER = 7, SAMP_MAXANISOTROPY = 8,
    SAMP_MAXMIPLEVEL = 9, SAMP_MIPMAPLODBIAS = 10, SAMP_SRGBTEXTURE = 11
};

static int parse_uint(const char *s, uint32_t *out) {
    if (!s || !*s) return 0;
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return 0;
    *out = (uint32_t)v;
    return 1;
}
static int map_bool(const char *s, uint32_t *out) {
    if (!strcmp(s, "true") || !strcmp(s, "TRUE") || !strcmp(s, "1")) { *out = 1; return 1; }
    if (!strcmp(s, "false") || !strcmp(s, "FALSE") || !strcmp(s, "0")) { *out = 0; return 1; }
    return 0;
}
static int map_enum(const char *s, const char *const *names, const uint32_t *vals,
                    int n, uint32_t *out) {
    for (int i = 0; i < n; i++)
        if (!strcmp(s, names[i])) { *out = vals[i]; return 1; }
    return 0;
}

static int render_value(const char *key, const char *val, uint32_t *state, uint32_t *value) {
    static const char *const cull_n[] = { "None", "CW", "CCW", NULL };
    static const uint32_t cull_v[]    = { 1, 2, 3 };
    static const char *const cmp_n[]  = { "Never", "Less", "Equal", "LessEqual", "Greater",
                                          "NotEqual", "GreaterEqual", "Always", NULL };
    static const uint32_t cmp_v[]     = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const char *const blend_n[]= { "Zero", "One", "SrcColor", "InvSrcColor",
                                          "SrcAlpha", "InvSrcAlpha", "DestAlpha", "InvDestAlpha",
                                          "DestColor", "InvDestColor", NULL };
    static const uint32_t blend_v[]   = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    static const char *const fill_n[] = { "Point", "Wireframe", "Solid", NULL };
    static const uint32_t fill_v[]    = { 1, 2, 3 };

    struct { const char *k; uint32_t rs; int kind; } tbl[] = {
        { "ZEnable", RS_ZENABLE, 0 }, { "ZWriteEnable", RS_ZWRITEENABLE, 0 },
        { "AlphaBlendEnable", RS_ALPHABLENDENABLE, 0 }, { "AlphaTestEnable", RS_ALPHATESTENABLE, 0 },
        { "DitherEnable", RS_DITHERENABLE, 0 },
        { "CullMode", RS_CULLMODE, 1 }, { "ZFunc", RS_ZFUNC, 2 }, { "AlphaFunc", RS_ALPHAFUNC, 2 },
        { "SrcBlend", RS_SRCBLEND, 3 }, { "DestBlend", RS_DESTBLEND, 3 },
        { "FillMode", RS_FILLMODE, 4 },
        { "AlphaRef", RS_ALPHAREF, 5 }
    };
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
        if (strcmp(key, tbl[i].k)) continue;
        *state = tbl[i].rs;
        switch (tbl[i].kind) {
        case 0: return map_bool(val, value);
        case 1: return map_enum(val, cull_n, cull_v, 3, value);
        case 2: return map_enum(val, cmp_n, cmp_v, 8, value);
        case 3: return map_enum(val, blend_n, blend_v, 10, value);
        case 4: return map_enum(val, fill_n, fill_v, 3, value);
        case 5: return parse_uint(val, value);
        }
    }
    return 0;
}

static int sampler_value(const char *key, const char *val, uint32_t *state, uint32_t *value) {
    static const char *const addr_n[] = { "Wrap", "Clamp", "Mirror", "Border", "MirrorOnce", NULL };
    static const uint32_t addr_v[]    = { 1, 2, 3, 4, 5 };
    static const char *const filt_n[] = { "None", "Point", "Linear", "Anisotropic", NULL };
    static const uint32_t filt_v[]    = { 0, 1, 2, 3 };
    struct { const char *k; uint32_t st; int kind; } tbl[] = {
        { "AddressU", SAMP_ADDRESSU, 0 }, { "AddressV", SAMP_ADDRESSV, 0 },
        { "AddressW", SAMP_ADDRESSW, 0 },
        { "MinFilter", SAMP_MINFILTER, 1 }, { "MagFilter", SAMP_MAGFILTER, 1 },
        { "MipFilter", SAMP_MIPFILTER, 1 },
        { "MaxAnisotropy", SAMP_MAXANISOTROPY, 2 }, { "MaxMipLevel", SAMP_MAXMIPLEVEL, 2 },
        { "MipMapLodBias", SAMP_MIPMAPLODBIAS, 2 },
        { "SRGBTexture", SAMP_SRGBTEXTURE, 3 }, { "BorderColor", SAMP_BORDERCOLOR, 2 }
    };
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
        if (strcmp(key, tbl[i].k)) continue;
        *state = tbl[i].st;
        switch (tbl[i].kind) {
        case 0: return map_enum(val, addr_n, addr_v, 5, value);
        case 1: return map_enum(val, filt_n, filt_v, 4, value);
        case 2: return parse_uint(val, value);
        case 3: return map_bool(val, value);
        }
    }
    return 0;
}

/* ------------------------------------------------------------ shader compile */

typedef struct { uint32_t code, ct; } compiled_t;

/* Compile one shader reference; returns 0 on failure. Defines, the include
 * interface and flags are forwarded unchanged to D3DXCompileShader. */
static int compile_ref(const d3dx9_fx_effect *fx, const d3dx9_fx_shader_ref *ref,
                       uint32_t macros, uint32_t include, uint32_t flags,
                       uint32_t ppErr, compiled_t *out) {
    out->code = out->ct = 0;
    if (!ref->entry[0] || !ref->profile[0]) return 0;
    const d3dx9_fx_function *fn = find_function(fx, ref->entry);
    if (!fn) return 0;
    /* Each function is compiled on its own, so the .fx global declarations
     * (textures, samplers, parameters) must be prepended to its source. */
    char full[32768];
    size_t n = 0;
    for (int i = 0; i < fx->param_count && n + 256 < sizeof full; i++) {
        const d3dx9_fx_param *p = &fx->params[i];
        n += (size_t)snprintf(full + n, sizeof full - n, "%s %s;\n", p->type, p->name);
    }
    if (n + strlen(fn->source) + 2 >= sizeof full) return 0;
    memcpy(full + n, fn->source, strlen(fn->source) + 1);

    uint32_t *pcode = (uint32_t *)hle_alloc(4), *pct = (uint32_t *)hle_alloc(4);
    if (!pcode || !pct) { if (pcode) hle_free(pcode); if (pct) hle_free(pct); return 0; }
    *pcode = *pct = 0;
    uint32_t rc = hle_d3d9fx_compile_source(full, strlen(full), "effect.fx",
        macros, include, ref->entry, ref->profile, flags,
        hle_va(pcode), ppErr, hle_va(pct));
    uint32_t codebuf = *pcode, ctbuf = *pct;
    hle_free(pcode); hle_free(pct);
    if (rc != D3D_OK || !codebuf) return 0;
    /* Create*Shader and the constant table are ID3DXBuffer objects; take the
     * token pointer, not the COM object itself. */
    uint32_t gp = hle_resolve("ID3DXBuffer::GetBufferPointer");
    out->code = hle_call_guest(gp, 1, (uint32_t[]){ codebuf });
    /* Keep the ID3DXConstantTable COM object itself: the effect queries it
     * through GetConstantByName/GetConstantDesc at commit time. Only the
     * shader bytecode is unwrapped to its token pointer. */
    out->ct = ctbuf;
    if (!out->ct) {
        fprintf(stderr, "[d3dx9] effect: shader %s/%s has no constant table\n",
                ref->entry, ref->profile);
        return 0;
    }
    return out->code != 0;
}

/* ------------------------------------------------------------- factory */

uint32_t hle_d3d9fx_effect_from_source(uint32_t dev, const char *src, size_t len,
                                       uint32_t pool, uint32_t macros, uint32_t include,
                                       uint32_t flags, uint32_t *out, uint32_t ppErr) {
    if (!dev || !src || !out) return D3DERR_INVALIDCALL;
    *out = 0;
    d3dx9_fx_effect fx;
    memset(&fx, 0, sizeof fx);
    if (d3dx9_fxtext_parse(src, &fx) != 0) {
        set_err(ppErr, fx.error);
        d3dx9_fxtext_free(&fx);
        return E_FAIL;
    }
    if (!fx.tech_count || !fx.functions) {
        set_err(ppErr, "effect has no technique or no shader functions");
        d3dx9_fxtext_free(&fx);
        return E_FAIL;
    }
    if (!pool) pool = hle_d3d9fx_pool_new();
    uint32_t stage = 0;
    for (int i = 0; i < fx.param_count; i++)
        if (!declare_param(pool, &fx.params[i], &stage)) {
            set_err(ppErr, "cannot declare effect parameter");
            d3dx9_fxtext_free(&fx);
            return E_FAIL;
        }

    uint32_t effect = hle_d3d9fx_effect_new(pool);
    hle_d3d9fx_effect_set_device(effect, dev);
    for (int t = 0; t < fx.tech_count; t++) {
        d3dx9_fx_technique *tech = &fx.techniques[t];
        const char *names[64];
        uint32_t vs[64], ps[64], vct[64], pct[64];
        if (tech->pass_count > 64) { set_err(ppErr, "too many passes"); d3dx9_fxtext_free(&fx); return E_FAIL; }
        for (int k = 0; k < tech->pass_count; k++) {
            d3dx9_fx_pass *pass = &tech->passes[k];
            names[k] = pass->name;
            compiled_t c;
            vs[k] = ps[k] = vct[k] = pct[k] = 0;
            if (pass->has_vs) {
                if (!compile_ref(&fx, &pass->vs, macros, include, flags, ppErr, &c)) {
                    set_err(ppErr, "vertex shader compile failed");
                    d3dx9_fxtext_free(&fx); return E_FAIL;
                }
                vs[k] = c.code; vct[k] = c.ct;
            }
            if (pass->has_ps) {
                if (!compile_ref(&fx, &pass->ps, macros, include, flags, ppErr, &c)) {
                    set_err(ppErr, "pixel shader compile failed");
                    d3dx9_fxtext_free(&fx); return E_FAIL;
                }
                ps[k] = c.code; pct[k] = c.ct;
            }
            /* Pass render states. Shader assignments are not states here. */
            for (int si = 0; si < pass->state_count; si++) {
                d3dx9_fx_state *st = &pass->states[si];
                if (!strcmp(st->key, "VertexShader") || !strcmp(st->key, "PixelShader"))
                    continue;
                uint32_t state, value;
                if (!render_value(st->key, st->value, &state, &value)) {
                    fprintf(stderr, "[d3dx9] effect: unsupported pass state %s = %s\n",
                            st->key, st->value);
                    set_err(ppErr, "unsupported pass state");
                    d3dx9_fxtext_free(&fx); return E_FAIL;
                }
                if (!hle_d3d9fx_effect_add_state(effect, 0, 0, state, value, t, k)) {
                    set_err(ppErr, "too many device states");
                    d3dx9_fxtext_free(&fx); return E_FAIL;
                }
            }
        }
        if (!hle_d3d9fx_effect_add_tech_ct(effect, tech->name, names, tech->pass_count,
                                            vs, ps, vct, pct)) {
            set_err(ppErr, "cannot add technique");
            d3dx9_fxtext_free(&fx);
            return E_FAIL;
        }
    }

    /* Sampler states from structured sampler_state blocks become D3DSAMP
     * states on the sampler's stage. Unknown keys/values are errors. */
    for (int i = 0; i < fx.param_count; i++) {
        d3dx9_fx_param *p = &fx.params[i];
        if (strncmp(p->type, "sampler", 7)) continue;
        uint32_t h = hle_d3d9fx_pool_param(pool, p->name, 0);
        if (!h) continue;
        uint32_t stg = hle_d3d9fx_param_get_stage(h), state, value;
        for (int k = 0; k < p->sampler_state_count; k++) {
            d3dx9_fx_state *ss = &p->sampler_states[k];
            if (!strcmp(ss->key, "Texture")) continue;  /* the bound texture is set via SetTexture */
            if (!sampler_value(ss->key, ss->value, &state, &value)) {
                fprintf(stderr, "[d3dx9] effect: unsupported sampler state %s = %s\n",
                        ss->key, ss->value);
                set_err(ppErr, "unsupported sampler state");
                d3dx9_fxtext_free(&fx); return E_FAIL;
            }
            if (!hle_d3d9fx_effect_add_state(effect, 1, stg, state, value, -1, -1)) {
                set_err(ppErr, "too many device states");
                d3dx9_fxtext_free(&fx); return E_FAIL;
            }
        }
    }

    d3dx9_fxtext_free(&fx);
    *out = effect;
    return D3D_OK;
}

/* ---- stdcall shims ---- */

static void s_D3DXCreateEffect(void) {   /* (device, src, len, defines, include, flags, pool, ppEffect, ppErrors) */
    uint32_t dev = A32(0), src = A32(1), len = A32(2), defs = A32(3), inc = A32(4),
             flags = A32(5), pool = A32(6), pp = A32(7), err = A32(8);
    if (pp) MEM32(pp) = 0;
    if (!src || !len) { set_err(err, "empty effect source"); RET(D3DERR_INVALIDCALL, 9); }
    RET(hle_d3d9fx_effect_from_source(dev, (const char *)hle_ptr(src), len, pool,
                                      defs, inc, flags, hle_ptr(pp), err), 9);
}

static void s_D3DXCreateEffectEx(void) { /* (device, src, len, defines, include, skip, flags, pool, ppEffect, ppErrors) */
    uint32_t dev = A32(0), src = A32(1), len = A32(2), defs = A32(3), inc = A32(4),
             skip = A32(5), flags = A32(6), pool = A32(7), pp = A32(8), err = A32(9);
    if (pp) MEM32(pp) = 0;
    if (!src || !len) { set_err(err, "empty effect source"); RET(D3DERR_INVALIDCALL, 10); }
    if (skip) {
        fprintf(stderr, "[d3dx9] D3DXCreateEffectEx: pSkipConstants not supported\n");
        set_err(err, "pSkipConstants not supported");
        RET(D3DXERR_NOTAVAILABLE, 10);
    }
    RET(hle_d3d9fx_effect_from_source(dev, (const char *)hle_ptr(src), len, pool,
                                      defs, inc, flags, hle_ptr(pp), err), 10);
}

static void s_D3DXCreateEffectFromFileA(void) { /* (device, file, defines, include, flags, pool, ppEffect, ppErrors) */
    uint32_t dev = A32(0), path = A32(1), defs = A32(2), inc = A32(3),
             flags = A32(4), pool = A32(5), pp = A32(6), err = A32(7);
    if (pp) MEM32(pp) = 0;
    const char *name = path ? (const char *)hle_ptr(path) : NULL;
    if (!name) { set_err(err, "no effect file"); RET(D3DERR_INVALIDCALL, 8); }
    FILE *f = fopen(name, "rb");
    if (!f) { set_err(err, "cannot open effect file"); RET(E_FAIL, 8); }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); RET(E_FAIL, 8); }
    long sz = ftell(f);
    if (sz <= 0 || sz > (long)D3DX9_FXTEXT_MAX_SRC) { fclose(f); set_err(err, "effect file too large"); RET(E_FAIL, 8); }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); RET(E_FAIL, 8); }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    uint32_t hr = hle_d3d9fx_effect_from_source(dev, buf, rd, pool, defs, inc, flags,
                                                hle_ptr(pp), err);
    free(buf);
    RET(hr, 8);
}

/* ------------------------------------------------------------- compiler */

/* A minimal ID3DXEffectCompiler. CompileEffect would produce the banned binary
 * FX container, so it is an explicit error; CompileShader is unsupported. */
static uint32_t g_vt_compiler;
typedef struct { uint32_t src, len, defs, inc, flags; } compiler_t;
#define COMP(o) ((compiler_t *)((char *)hle_ptr(o) + 8u))

static void cm_query(void) {
    uint32_t self = A32(0), ppv = A32(2);
    if (!ppv) { RET(0x80004003u /* E_POINTER */, 3); return; }
    MEM32(ppv) = self;
    MEM32(self + 4)++;
    RET(D3D_OK, 3);
}
static void cm_release(void) { uint32_t self = A32(0), n = --MEM32(self + 4); if (!n) hle_com_free(self); RET(n, 1); }
static void cm_compile_effect(void) {
    /* (this, Flags, ppEffect, ppErrorMsgs) */
    if (A32(2)) MEM32(A32(2)) = 0;
    if (A32(3)) MEM32(A32(3)) = 0;
    fprintf(stderr, "[d3dx9] ID3DXEffectCompiler::CompileEffect: binary FX format not supported\n");
    RET(D3DXERR_NOTAVAILABLE, 4);
}
static void cm_compile_shader(void) {
    /* (this, hFunction, pTarget, Flags, ppCode, ppErrorMsgs, ppConstantTable) */
    if (A32(4)) MEM32(A32(4)) = 0;
    if (A32(5)) MEM32(A32(5)) = 0;
    if (A32(6)) MEM32(A32(6)) = 0;
    fprintf(stderr, "[d3dx9] ID3DXEffectCompiler::CompileShader: not implemented\n");
    RET(D3DXERR_NOTAVAILABLE, 7);
}
/* ID3DXEffectCompiler: public 61-slot layout; only CompileEffect/CompileShader
 * are answered here (both report "not available"), the rest are the logged
 * "not supported" slots of d3dx9_fxvtbl.c. */
static const win32hle_shim s_compiler_impl[] = {
    { "QueryInterface", d3d_QueryInterface },
    { "AddRef", hle_com_AddRef },
    { "Release", cm_release },
    { "CompileEffect", cm_compile_effect },
    { "CompileShader", cm_compile_shader },
    { 0, 0 }
};

static void s_D3DXCreateEffectCompiler(void) { /* (src, len, defines, include, flags, ppCompiler, ppErrors) */
    uint32_t src = A32(0), len = A32(1), defs = A32(2), inc = A32(3), flags = A32(4),
             pp = A32(5), err = A32(6);
    if (pp) MEM32(pp) = 0;
    if (!src || !len) { set_err(err, "empty effect source"); RET(D3DERR_INVALIDCALL, 7); }
    if (!g_vt_compiler) g_vt_compiler = d3dx9_fx_vtable("ID3DXEffectCompiler", 1, s_compiler_impl);
    uint32_t o = hle_com_new(g_vt_compiler, 8u + sizeof(compiler_t));
    COMP(o)->src = src; COMP(o)->len = len;
    COMP(o)->defs = defs; COMP(o)->inc = inc; COMP(o)->flags = flags;
    if (pp) MEM32(pp) = o;
    RET(D3D_OK, 7);
}

const win32hle_shim win32hle_d3dx9_fxsource[] = {
    { "D3DXCreateEffect",        s_D3DXCreateEffect },
    { "D3DXCreateEffectEx",      s_D3DXCreateEffectEx },
    { "D3DXCreateEffectFromFileA", s_D3DXCreateEffectFromFileA },
    { "D3DXCreateEffectCompiler", s_D3DXCreateEffectCompiler },
    { 0, 0 }
};
