/*
 * d3d9_ffp.c - Fixed Function Pipeline MSL generator.
 * Public D3D9 FFP semantics: FVF, texture stage ops, fog, alpha test, lighting.
 * Unsupported features return an error.
 */
#include "d3d9_ffp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *buf; size_t len; size_t cap; int oom; } sbuf_t;
static void sb_init(sbuf_t *s){ s->cap=1024; s->len=0; s->oom=0; s->buf=(char*)malloc(s->cap); if(s->buf) s->buf[0]=0; else s->oom=1; }
static void sb_add(sbuf_t *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0) { s->oom=1; return; }
    if (s->len + (size_t)n + 1 > s->cap) {
        size_t nc = s->cap * 2; while (nc < s->len + (size_t)n + 1) nc *= 2;
        char *nb = (char*)realloc(s->buf, nc); if (!nb) { s->oom=1; return; } s->buf=nb; s->cap=nc;
    }
    va_start(ap, fmt); vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap); va_end(ap);
    s->len += (size_t)n;
}

static const char *cmp_op(uint32_t f) {
    switch (f) {
    case D3D9_CMP_NEVER: return "false";
    case D3D9_CMP_LESS: return "<";
    case D3D9_CMP_EQUAL: return "==";
    case D3D9_CMP_LESSEQUAL: return "<=";
    case D3D9_CMP_GREATER: return ">";
    case D3D9_CMP_NOTEQUAL: return "!=";
    case D3D9_CMP_GREATEREQUAL: return ">=";
    case D3D9_CMP_ALWAYS: return "true";
    default: return NULL;
    }
}

int d3d9_ffp_generate(const d3d9_ffp_desc_t *desc, char **out_msl, size_t *out_len, const char **err) {
    sbuf_t s; uint32_t fvf, texcount, i;
    int has_xyz, has_rhw, has_normal, has_diffuse, has_specular;
    if (out_msl) *out_msl = NULL; if (out_len) *out_len = 0; if (err) *err = NULL;
    if (!desc || !out_msl) { if (err) *err = "invalid args"; return -1; }
    fvf = desc->fvf;
    has_xyz = (fvf & D3D9_FVF_XYZ) != 0;
    has_rhw = (fvf & D3D9_FVF_XYZRHW) != 0;
    has_normal = (fvf & D3D9_FVF_NORMAL) != 0;
    has_diffuse = (fvf & D3D9_FVF_DIFFUSE) != 0;
    has_specular = (fvf & D3D9_FVF_SPECULAR) != 0;
    texcount = (fvf & D3D9_FVF_TEXCOUNT_MASK) >> D3D9_FVF_TEXCOUNT_SHIFT;

    if (has_xyz && has_rhw) { if (err) *err = "FVF: XYZ and XYZRHW mutually exclusive"; return -1; }
    if (!has_xyz && !has_rhw) { if (err) *err = "FVF: missing position type"; return -1; }
    if ((fvf & 0x006u) == 0x006u && !has_rhw) { if (err) *err = "FVF: XYZB1 not supported"; return -1; }
    if (has_normal) { if (err) *err = "FVF: NORMAL not supported yet"; return -1; }
    if (texcount > 2u) { if (err) *err = "FVF: >2 texcoords not supported"; return -1; }
    if (desc->lighting) { if (err) *err = "lighting not supported"; return -1; }
    if (desc->stage_count > 2u) { if (err) *err = ">2 texture stages not supported"; return -1; }
    if (desc->alpha_test_enable) {
        if (!cmp_op(desc->alpha_func)) { if (err) *err = "invalid alpha func"; return -1; }
    }
    if (desc->fog_mode > D3D9_FOG_LINEAR) { if (err) *err = "invalid fog mode"; return -1; }
    for (i = 0; i < desc->stage_count; i++) {
        const d3d9_ffp_stage_t *st = &desc->stages[i];
        uint32_t op = st->color_op ? st->color_op : D3D9_TEXOP_DISABLE;
        uint32_t aop = st->alpha_op ? st->alpha_op : D3D9_TEXOP_DISABLE;
        if (op != D3D9_TEXOP_DISABLE && op != D3D9_TEXOP_SELECTARG1 && op != D3D9_TEXOP_MODULATE) { if (err) *err = "unsupported color op"; return -1; }
        if (aop != D3D9_TEXOP_DISABLE && aop != D3D9_TEXOP_SELECTARG1 && aop != D3D9_TEXOP_MODULATE) { if (err) *err = "unsupported alpha op"; return -1; }
        if (st->color_arg1 > 2u || st->color_arg2 > 2u || st->alpha_arg1 > 2u || st->alpha_arg2 > 2u) { if (err) *err = "unsupported tex arg"; return -1; }
    }

    sb_init(&s);
    sb_add(&s, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    sb_add(&s, "struct FFPUniforms { float4x4 mvp; float4 fogColor; float fogStart; float fogEnd; float fogDensity; float _pad; float4x4 world; };\n");
    /* User clip planes are in world space for the fixed-function pipeline
     * (public SetClipPlane docs); the world matrix produces that position. */
    sb_add(&s, "struct D3D9Clip { uint4 mask; float4 planes[6]; };\n");
    sb_add(&s, "struct VSIn {\n");
    sb_add(&s, "  float4 pos [[attribute(0)]];\n");
    if (has_diffuse) sb_add(&s, "  float4 diffuse [[attribute(1)]];\n");
    for (i = 0; i < texcount; i++) sb_add(&s, "  float2 uv%u [[attribute(%u)]];\n", i, 2u + i);
    sb_add(&s, "};\n");
    sb_add(&s, "struct VSOut {\n");
    sb_add(&s, "  float4 pos [[position]];\n");
    if (has_diffuse) sb_add(&s, "  float4 diffuse;\n");
    for (i = 0; i < texcount; i++) sb_add(&s, "  float2 uv%u;\n", i);
    sb_add(&s, "  float fogFactor;\n");
    sb_add(&s, "  float4 clipPos;\n");
    sb_add(&s, "};\n\n");

    sb_add(&s, "vertex VSOut ffp_vs(VSIn in [[stage_in]], constant FFPUniforms& u [[buffer(1)]],\n");
    sb_add(&s, "                      constant D3D9Clip& clip [[buffer(4)]]) {\n");
    sb_add(&s, "  VSOut o;\n");
    if (has_rhw) sb_add(&s, "  o.pos = in.pos;\n");
    else sb_add(&s, "  o.pos = u.mvp * in.pos;\n");
    if (has_diffuse) sb_add(&s, "  o.diffuse = in.diffuse;\n");
    else sb_add(&s, "  // no diffuse\n");
    for (i = 0; i < texcount; i++) sb_add(&s, "  o.uv%u = in.uv%u;\n", i, i);
    /* fog factor: linear uses z/w in clip space approximation; keep simple */
    if (desc->fog_mode == D3D9_FOG_NONE) {
        sb_add(&s, "  o.fogFactor = 1.0;\n");
    } else if (desc->fog_mode == D3D9_FOG_LINEAR) {
        sb_add(&s, "  { float d = o.pos.z / max(o.pos.w, 1e-6); o.fogFactor = clamp((u.fogEnd - d) / max(u.fogEnd - u.fogStart, 1e-6), 0.0, 1.0); }\n");
    } else if (desc->fog_mode == D3D9_FOG_EXP) {
        sb_add(&s, "  { float d = o.pos.z / max(o.pos.w, 1e-6); o.fogFactor = clamp(exp(-u.fogDensity * d), 0.0, 1.0); }\n");
    } else {
        sb_add(&s, "  { float d = o.pos.z / max(o.pos.w, 1e-6); float f = u.fogDensity * d; o.fogFactor = clamp(exp(-f*f), 0.0, 1.0); }\n");
    }
    sb_add(&s, "  o.clipPos = u.world * in.pos;\n");
    sb_add(&s, "  return o;\n}\n\n");

    /* fragment */
    sb_add(&s, "fragment float4 ffp_ps(VSOut in [[stage_in]], constant FFPUniforms& u [[buffer(1)]],\n");
    sb_add(&s, "                        constant D3D9Clip& clip [[buffer(4)]]");
    for (i = 0; i < texcount && i < desc->stage_count; i++) {
        if (desc->stages[i].color_op == D3D9_TEXOP_DISABLE && desc->stages[i].alpha_op == D3D9_TEXOP_DISABLE) continue;
        sb_add(&s, ", texture2d<float> tex%u [[texture(%u)]], sampler samp%u [[sampler(%u)]]", i, i, i, i);
    }
    sb_add(&s, ") {\n");
    sb_add(&s, "  for (uint i = 0u; i < 6u; ++i)\n"
                "    if ((clip.mask.x & (1u << i)) != 0u && dot(clip.planes[i], in.clipPos) < 0.0f)\n"
                "      discard_fragment();\n");
    sb_add(&s, "  float4 current = %s;\n", has_diffuse ? "in.diffuse" : "float4(1.0)");
    for (i = 0; i < desc->stage_count; i++) {
        const d3d9_ffp_stage_t *st = &desc->stages[i];
        uint32_t cop = st->color_op ? st->color_op : D3D9_TEXOP_DISABLE;
        uint32_t aop = st->alpha_op ? st->alpha_op : D3D9_TEXOP_DISABLE;
        const char *ca1 = st->color_arg1 == D3D9_TA_TEXTURE ? "tex.rgb" : (st->color_arg1 == D3D9_TA_DIFFUSE ? (has_diffuse?"in.diffuse.rgb":"float3(1.0)") : "current.rgb");
        const char *ca2 = st->color_arg2 == D3D9_TA_TEXTURE ? "tex.rgb" : (st->color_arg2 == D3D9_TA_DIFFUSE ? (has_diffuse?"in.diffuse.rgb":"float3(1.0)") : "current.rgb");
        const char *aa1 = st->alpha_arg1 == D3D9_TA_TEXTURE ? "tex.a" : (st->alpha_arg1 == D3D9_TA_DIFFUSE ? (has_diffuse?"in.diffuse.a":"1.0") : "current.a");
        const char *aa2 = st->alpha_arg2 == D3D9_TA_TEXTURE ? "tex.a" : (st->alpha_arg2 == D3D9_TA_DIFFUSE ? (has_diffuse?"in.diffuse.a":"1.0") : "current.a");
        int need_tex = (cop != D3D9_TEXOP_DISABLE || aop != D3D9_TEXOP_DISABLE) && i < texcount;
        if (cop == D3D9_TEXOP_DISABLE && aop == D3D9_TEXOP_DISABLE) continue;
        if (need_tex) sb_add(&s, "  float4 tex = tex%u.sample(samp%u, in.uv%u);\n", i, i, i);
        else sb_add(&s, "  float4 tex = float4(1.0);\n");
        if (cop == D3D9_TEXOP_SELECTARG1) sb_add(&s, "  current.rgb = %s;\n", ca1);
        else if (cop == D3D9_TEXOP_MODULATE) sb_add(&s, "  current.rgb = %s * %s;\n", ca1, ca2);
        if (aop == D3D9_TEXOP_SELECTARG1) sb_add(&s, "  current.a = %s;\n", aa1);
        else if (aop == D3D9_TEXOP_MODULATE) sb_add(&s, "  current.a = %s * %s;\n", aa1, aa2);
    }
    if (desc->fog_mode != D3D9_FOG_NONE) {
        sb_add(&s, "  current.rgb = mix(u.fogColor.rgb, current.rgb, in.fogFactor);\n");
    }
    if (desc->alpha_test_enable) {
        const char *op = cmp_op(desc->alpha_func);
        if (op) {
            if (desc->alpha_func == D3D9_CMP_NEVER) sb_add(&s, "  discard_fragment();\n");
            else if (desc->alpha_func != D3D9_CMP_ALWAYS)
                sb_add(&s, "  if (!(current.a * 255.0 %s float(%u))) discard_fragment();\n", op, desc->alpha_ref & 0xFFu);
        }
    }
    sb_add(&s, "  return current;\n}\n");
    (void)has_specular;

    if (s.oom) { free(s.buf); if (err) *err = "oom"; return -1; }
    *out_msl = s.buf;
    if (out_len) *out_len = s.len;
    return 0;
}
