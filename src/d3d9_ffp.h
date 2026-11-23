/*
 * d3d9_ffp.h - Fixed Function Pipeline MSL generator (D3D9 semantics).
 *
 * Generates Metal Shading Language vertex+fragment shader source from D3D9
 * fixed-function state: FVF, texture stage states, fog, alpha test and light.
 * Unsupported combinations return an error (no silent fallback).
 *
 * Based on public D3D9 documentation (FVF, texture stage states, fog formulas,
 * alpha test, lighting). Integration into the Metal backend is a later step.
 */
#ifndef WIN32HLE_D3D9_FFP_H
#define WIN32HLE_D3D9_FFP_H

#include <stdint.h>
#include <stddef.h>

/* D3DFVF bits (public D3D9). */
#define D3D9_FVF_XYZ      0x002u
#define D3D9_FVF_XYZRHW   0x004u
#define D3D9_FFV_XYZB1    0x006u /* not supported */
#define D3D9_FVF_NORMAL   0x010u
#define D3D9_FVF_PSIZE    0x020u
#define D3D9_FVF_DIFFUSE  0x040u
#define D3D9_FVF_SPECULAR 0x080u
#define D3D9_FVF_TEXCOUNT_MASK 0xF00u
#define D3D9_FVF_TEXCOUNT_SHIFT 8u

/* Texture stage ops (subset; unsupported -> error). */
#define D3D9_TEXOP_DISABLE    1u
#define D3D9_TEXOP_SELECTARG1 2u
#define D3D9_TEXOP_MODULATE   4u

/* Texture args */
#define D3D9_TA_TEXTURE 2u
#define D3D9_TA_DIFFUSE 0u
#define D3D9_TA_CURRENT 1u

/* Fog modes */
#define D3D9_FOG_NONE   0u
#define D3D9_FOG_EXP    1u
#define D3D9_FOG_EXP2   2u
#define D3D9_FOG_LINEAR 3u

/* Alpha compare funcs (D3DCMPFUNC) */
#define D3D9_CMP_NEVER        1u
#define D3D9_CMP_LESS         2u
#define D3D9_CMP_EQUAL        3u
#define D3D9_CMP_LESSEQUAL    4u
#define D3D9_CMP_GREATER      5u
#define D3D9_CMP_NOTEQUAL     6u
#define D3D9_CMP_GREATEREQUAL 7u
#define D3D9_CMP_ALWAYS       8u

typedef struct {
    uint32_t color_op;   /* D3D9_TEXOP_* */
    uint32_t color_arg1; /* D3D9_TA_* */
    uint32_t color_arg2;
    uint32_t alpha_op;
    uint32_t alpha_arg1;
    uint32_t alpha_arg2;
} d3d9_ffp_stage_t;

typedef struct {
    uint32_t fvf;
    const d3d9_ffp_stage_t *stages; /* stage 0..stage_count-1 */
    uint32_t stage_count;
    /* fog */
    uint32_t fog_mode;      /* D3D9_FOG_* */
    float fog_start;
    float fog_end;
    float fog_density;
    uint32_t fog_color;     /* ARGB */
    /* alpha test */
    uint32_t alpha_test_enable;
    uint32_t alpha_func;    /* D3D9_CMP_* */
    uint32_t alpha_ref;     /* 0..255 */
    /* lighting */
    uint32_t lighting;      /* 0/1; only 0 (unlit) supported initially */
} d3d9_ffp_desc_t;

/* Generate MSL. On success returns 0 and writes malloc'ed NUL-terminated MSL to
 * *out_msl (caller frees) and optional length to *out_len. On failure returns
 * -1 and writes a static/constant error string to *err. */
int d3d9_ffp_generate(const d3d9_ffp_desc_t *desc, char **out_msl, size_t *out_len, const char **err);

#endif /* WIN32HLE_D3D9_FFP_H */
