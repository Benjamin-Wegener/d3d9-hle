/*
 * d3d9_ffp.c - Fixed Function Pipeline MSL generator.
 */
#include "d3d9_ffp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *buf; size_t len; size_t cap; int oom; } sbuf_t;
static void sb_init(sbuf_t *s){ s->cap=4096; s->len=0; s->oom=0; s->buf=(char*)malloc(s->cap); if(s->buf) s->buf[0]=0; else s->oom=1; }
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

static int is_bump(uint32_t op) {
    return op == D3D9_TEXOP_BUMPENVMAP || op == D3D9_TEXOP_BUMPENVMAPLUMINANCE;
}

static void unpack_color(uint32_t c, char *out, size_t sz) {
    /* returns float4 literal */
    float a = ((c >> 24) & 0xFFu) / 255.0f;
    float r = ((c >> 16) & 0xFFu) / 255.0f;
    float g = ((c >> 8) & 0xFFu) / 255.0f;
    float b = (c & 0xFFu) / 255.0f;
    snprintf(out, sz, "float4(%.6ff, %.6ff, %.6ff, %.6ff)", r, g, b, a);
}

/* Build expression string for a texture argument (returns float4). Caller provides buffer. */
static void arg_expr(uint32_t arg, uint32_t stage_const, int is_color, int has_diffuse, int has_specular,
                     const char *tex_var, const char *tfactor_expr, const char *constant_expr,
                     char *out, size_t sz) {
    uint32_t base = arg & D3D9_TA_SELECTMASK;
    uint32_t mod = arg & (D3D9_TA_COMPLEMENT | D3D9_TA_ALPHAREPLICATE);
    char baseexpr[128];
    switch (base) {
        case D3D9_TA_DIFFUSE:
            if (has_diffuse) snprintf(baseexpr, sizeof baseexpr, "in.diffuse");
            else snprintf(baseexpr, sizeof baseexpr, "float4(1.0)");
            break;
        case D3D9_TA_CURRENT:
            snprintf(baseexpr, sizeof baseexpr, "current");
            break;
        case D3D9_TA_TEXTURE:
            snprintf(baseexpr, sizeof baseexpr, "%s", tex_var);
            break;
        case D3D9_TA_TFACTOR:
            snprintf(baseexpr, sizeof baseexpr, "%s", tfactor_expr);
            break;
        case D3D9_TA_SPECULAR:
            if (has_specular) snprintf(baseexpr, sizeof baseexpr, "in.specular");
            else snprintf(baseexpr, sizeof baseexpr, "float4(1.0)");
            break;
        case D3D9_TA_TEMP:
            snprintf(baseexpr, sizeof baseexpr, "temp");
            break;
        case D3D9_TA_CONSTANT:
            snprintf(baseexpr, sizeof baseexpr, "%s", constant_expr);
            break;
        default:
            snprintf(baseexpr, sizeof baseexpr, "float4(1.0)");
            break;
    }
    /* modifiers */
    if (mod & D3D9_TA_COMPLEMENT) {
        if (mod & D3D9_TA_ALPHAREPLICATE) {
            snprintf(out, sz, "(float4(1.0) - float4(%s.a, %s.a, %s.a, %s.a))", baseexpr, baseexpr, baseexpr, baseexpr);
        } else {
            snprintf(out, sz, "(float4(1.0) - %s)", baseexpr);
        }
    } else if (mod & D3D9_TA_ALPHAREPLICATE) {
        snprintf(out, sz, "float4(%s.a, %s.a, %s.a, %s.a)", baseexpr, baseexpr, baseexpr, baseexpr);
    } else {
        snprintf(out, sz, "%s", baseexpr);
    }
    (void)is_color;
    (void)stage_const;
}

int d3d9_ffp_generate(const d3d9_ffp_desc_t *desc, char **out_msl, size_t *out_len, const char **err) {
    sbuf_t s;
    uint32_t fvf, texcount, i, j;
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
    if (texcount > 8u) { if (err) *err = "FVF: >8 texcoords not supported"; return -1; }
    if (desc->stage_count > 8u) { if (err) *err = ">8 texture stages not supported"; return -1; }

    /* Validate ops/args. Camera-space texgen modes that need a normal are
     * rejected here only when the FVF has no NORMAL element; otherwise the
     * generator below emits the public D3D9 formula. This is an honest error,
     * never a silent zero vector. */
    for (i = 0; i < desc->stage_count; i++) {
        const d3d9_ffp_stage_t *st = &desc->stages[i];
        uint32_t tci = st->texcoord_index & 0xF0000u;
        if ((tci == D3D9_TCI_CAMERASPACENORMAL ||
             tci == D3D9_TCI_CAMERASPACEREFLECTIONVECTOR ||
             tci == D3D9_TCI_SPHEREMAP) && !has_normal) {
            if (err) *err = "camera-space texcoord mode needs D3DFVF_NORMAL";
            return -1;
        }
        uint32_t cop = st->color_op ? st->color_op : D3D9_TEXOP_DISABLE;
        uint32_t aop = st->alpha_op ? st->alpha_op : D3D9_TEXOP_DISABLE;
        (void)cop; (void)aop;
        if (st->color_arg1 > 0x3Fu || st->color_arg2 > 0x3Fu || st->color_arg0 > 0x3Fu) { if (err) *err = "unsupported tex arg"; return -1; }
        if (st->alpha_arg1 > 0x3Fu || st->alpha_arg2 > 0x3Fu || st->alpha_arg0 > 0x3Fu) { if (err) *err = "unsupported tex arg"; return -1; }
    }
    if (desc->alpha_test_enable) {
        if (!cmp_op(desc->alpha_func)) { if (err) *err = "invalid alpha func"; return -1; }
    }
    if (desc->fog_mode > D3D9_FOG_LINEAR) { if (err) *err = "invalid fog mode"; return -1; }

    sb_init(&s);

    /* Attribute indices follow the D3DFVF element order used by the device:
     * position 0, normal (if any), diffuse, specular, then texcoord i.
     * Both the generated MSL and d3d9_idirect3ddevice9.c must agree. */
    {
        uint32_t attr = 1;
        int normal_attr = -1, diffuse_attr = -1, specular_attr = -1, tex_attr0;
        if (has_normal) normal_attr = (int)attr++;
        if (has_diffuse) diffuse_attr = (int)attr++;
        if (has_specular) specular_attr = (int)attr++;
        tex_attr0 = attr;

        sb_add(&s, "#include <metal_stdlib>\nusing namespace metal;\n\n");
        sb_add(&s, "struct FFPLight { float4 diffuse; float4 specular; float4 ambient; float4 pos; float4 dir; float4 params1; float4 params2; };\n");
        sb_add(&s, "struct FFPMaterial { float4 diffuse; float4 ambient; float4 specular; float4 emissive; float4 power_pad; };\n");
        /* Field order must match ffp_uniforms_payload_t in d3d9_metal.h. */
        sb_add(&s, "struct FFPUniforms { float4x4 mvp; float4 fogColor; float fogStart; float fogEnd; float fogDensity; float _pad; float4x4 world; float4x4 view; float4x4 proj; float4 textureFactor; float4 globalAmbient; FFPMaterial mat; FFPLight lights[8]; float4x4 texTransform[8]; float4 viewportXYWH; float4 viewportZ; };\n");
        sb_add(&s, "struct D3D9Clip { uint4 mask; float4 planes[6]; };\n");
        sb_add(&s, "struct VSIn {\n");
        sb_add(&s, "  float4 pos [[attribute(0)]];\n");
        if (normal_attr >= 0) sb_add(&s, "  float3 normal [[attribute(%d)]];\n", normal_attr);
        if (diffuse_attr >= 0) sb_add(&s, "  float4 diffuse [[attribute(%d)]];\n", diffuse_attr);
        if (specular_attr >= 0) sb_add(&s, "  float4 specular [[attribute(%d)]];\n", specular_attr);
        for (i = 0; i < texcount; i++) sb_add(&s, "  float2 uv%u [[attribute(%u)]];\n", i, tex_attr0 + i);
        sb_add(&s, "};\n");

        sb_add(&s, "struct VSOut {\n");
        sb_add(&s, "  float4 pos [[position]];\n");
        sb_add(&s, "  float4 diffuse;\n");
        sb_add(&s, "  float4 specular;\n");
        for (i = 0; i < texcount; i++) sb_add(&s, "  float2 uv%u;\n", i);
        for (i = 0; i < (desc->stage_count ? desc->stage_count : 1); i++) sb_add(&s, "  float2 uvs%u;\n", i);
        sb_add(&s, "  float fogFactor;\n");
        sb_add(&s, "  float4 clipPos;\n");
        sb_add(&s, "};\n\n");

        /* Emit the vertex shader body with access to the attribute indices. */
        sb_add(&s, "vertex VSOut ffp_vs(VSIn in [[stage_in]], constant FFPUniforms& u [[buffer(1)]],\n");
        sb_add(&s, "                      constant D3D9Clip& clip [[buffer(4)]]) {\n");
        sb_add(&s, "  VSOut o;\n");
        if (has_rhw) {
            /* XYZRHW: x,y are screen pixels, z is depth, w is rhw (1/w).
             * Convert to clip-space: x_ndc=2*(x-X)/W-1, y_ndc=1-2*(y-Y)/H,
             * w_clip=1/rhw. Pre-transformed z is already the depth value in
             * 0..1, which is also Metal's clip range: pass it through. */
            sb_add(&s, "  float w_clip = in.pos.w != 0.0 ? 1.0 / in.pos.w : 1.0;\n");
            sb_add(&s, "  float x_ndc = 2.0 * (in.pos.x - u.viewportXYWH.x) / u.viewportXYWH.z - 1.0;\n");
            sb_add(&s, "  float y_ndc = 1.0 - 2.0 * (in.pos.y - u.viewportXYWH.y) / u.viewportXYWH.w;\n");
            sb_add(&s, "  float z_ndc = in.pos.z;\n");
            sb_add(&s, "  o.pos = float4(x_ndc * w_clip, y_ndc * w_clip, z_ndc * w_clip, w_clip);\n");
        } else
            sb_add(&s, "  o.pos = u.mvp * in.pos;\n");
        if (desc->lighting) {
            /* Fixed-function lighting (public D3D9 docs): material sources,
             * COLORVERTEX, directional/point/spot with attenuation and specular. */
            const char *vcol = has_diffuse ? "in.diffuse" : "float4(1.0)";
            const char *vspec = has_specular ? "in.specular" : "float4(1.0)";
            if (desc->color_vertex) {
                if (desc->diff_mat_source == 1) sb_add(&s, "  float4 m_diff = %s;\n", vcol);
                else if (desc->diff_mat_source == 2) sb_add(&s, "  float4 m_diff = %s;\n", vspec);
                else sb_add(&s, "  float4 m_diff = u.mat.diffuse;\n");
                if (desc->amb_mat_source == 1) sb_add(&s, "  float4 m_amb = %s;\n", vcol);
                else if (desc->amb_mat_source == 2) sb_add(&s, "  float4 m_amb = %s;\n", vspec);
                else sb_add(&s, "  float4 m_amb = u.mat.ambient;\n");
                if (desc->spec_mat_source == 1) sb_add(&s, "  float4 m_spec = %s;\n", vcol);
                else if (desc->spec_mat_source == 2) sb_add(&s, "  float4 m_spec = %s;\n", vspec);
                else sb_add(&s, "  float4 m_spec = u.mat.specular;\n");
                if (desc->emiss_mat_source == 1) sb_add(&s, "  float4 m_emiss = %s;\n", vcol);
                else if (desc->emiss_mat_source == 2) sb_add(&s, "  float4 m_emiss = %s;\n", vspec);
                else sb_add(&s, "  float4 m_emiss = u.mat.emissive;\n");
            } else {
                sb_add(&s, "  float4 m_diff = u.mat.diffuse;\n");
                sb_add(&s, "  float4 m_amb = u.mat.ambient;\n");
                sb_add(&s, "  float4 m_spec = u.mat.specular;\n");
                sb_add(&s, "  float4 m_emiss = u.mat.emissive;\n");
            }
            sb_add(&s, "  float4 world_pos = u.world * in.pos;\n");
            if (has_normal) {
                if (desc->normalize_normals) sb_add(&s, "  float3 N = normalize((u.world * float4(in.normal, 0.0)).xyz);\n");
                else sb_add(&s, "  float3 N = (u.world * float4(in.normal, 0.0)).xyz;\n");
            } else {
                sb_add(&s, "  float3 N = float3(0.0, 0.0, -1.0);\n");
            }
            sb_add(&s, "  float3 V = %s;\n", desc->local_viewer ? "normalize(-world_pos.xyz)" : "float3(0.0, 0.0, -1.0)");
            sb_add(&s, "  float3 lit_diff = m_emiss.rgb + (m_amb.rgb * u.globalAmbient.rgb);\n");
            sb_add(&s, "  float3 lit_spec = float3(0.0);\n");
            sb_add(&s, "  for (int li = 0; li < 8; li++) {\n");
            sb_add(&s, "    int type = int(u.lights[li].params2.w);\n");
            sb_add(&s, "    if (type == 0) continue;\n");
            sb_add(&s, "    float3 L;\n");
            sb_add(&s, "    float att = 1.0;\n");
            sb_add(&s, "    float spot = 1.0;\n");
            sb_add(&s, "    if (type == 3) {\n");
            sb_add(&s, "      L = -normalize(u.lights[li].dir.xyz);\n");
            sb_add(&s, "    } else {\n");
            sb_add(&s, "      float3 d_vec = u.lights[li].pos.xyz - world_pos.xyz;\n");
            sb_add(&s, "      float d = length(d_vec);\n");
            sb_add(&s, "      if (d > u.lights[li].params1.x) continue;\n");
            sb_add(&s, "      L = d_vec / max(d, 1e-6);\n");
            sb_add(&s, "      att = 1.0 / max(u.lights[li].params2.x + u.lights[li].params2.y * d + u.lights[li].params2.z * d * d, 1e-6);\n");
            sb_add(&s, "      if (type == 2) {\n");
            sb_add(&s, "        float cos_a = dot(-L, normalize(u.lights[li].dir.xyz));\n");
            sb_add(&s, "        float cos_theta = cos(u.lights[li].params1.z * 0.5);\n");
            sb_add(&s, "        float cos_phi = cos(u.lights[li].params1.w * 0.5);\n");
            sb_add(&s, "        if (cos_a <= cos_phi) spot = 0.0;\n");
            sb_add(&s, "        else if (cos_a < cos_theta) {\n");
            sb_add(&s, "          float num = cos_a - cos_phi;\n");
            sb_add(&s, "          float den = max(cos_theta - cos_phi, 1e-6);\n");
            sb_add(&s, "          spot = pow(clamp(num / den, 0.0, 1.0), u.lights[li].params1.y);\n");
            sb_add(&s, "        }\n");
            sb_add(&s, "      }\n");
            sb_add(&s, "    }\n");
            sb_add(&s, "    float NdotL = max(dot(N, L), 0.0);\n");
            sb_add(&s, "    lit_diff += att * spot * (m_amb.rgb * u.lights[li].ambient.rgb + NdotL * m_diff.rgb * u.lights[li].diffuse.rgb);\n");
            if (desc->specular_enable) {
                sb_add(&s, "    if (NdotL > 0.0) {\n");
                sb_add(&s, "      float3 H = normalize(L + V);\n");
                sb_add(&s, "      float NdotH = max(dot(N, H), 0.0);\n");
                sb_add(&s, "      float spec_p = u.mat.power_pad.x > 0.0 ? pow(NdotH, u.mat.power_pad.x) : 1.0;\n");
                sb_add(&s, "      lit_spec += att * spot * spec_p * m_spec.rgb * u.lights[li].specular.rgb;\n");
                sb_add(&s, "    }\n");
            }
            sb_add(&s, "  }\n");
            sb_add(&s, "  o.diffuse = float4(clamp(lit_diff, 0.0, 1.0), m_diff.a);\n");
            sb_add(&s, "  o.specular = float4(clamp(lit_spec, 0.0, 1.0), 0.0);\n");
        } else {
            if (has_diffuse) sb_add(&s, "  o.diffuse = in.diffuse;\n");
            else sb_add(&s, "  o.diffuse = float4(1.0);\n");
            if (has_specular) sb_add(&s, "  o.specular = in.specular;\n");
            else sb_add(&s, "  o.specular = float4(0.0);\n");
        }
        for (i = 0; i < texcount; i++) sb_add(&s, "  o.uv%u = in.uv%u;\n", i, i);

        /* Camera-space normal (world*view transform; exact for orthonormal
         * matrices, the common fixed-function case) used by TCI texgen. */
        if (has_normal) sb_add(&s, "  float3 _csNormal = normalize((u.view * u.world * float4(in.normal, 0.0)).xyz);\n");
        sb_add(&s, "  float3 _csPos = (u.view * u.world * in.pos).xyz;\n");

        /* stage UVs */
        for (i = 0; i < (desc->stage_count ? desc->stage_count : 1); i++) {
            const d3d9_ffp_stage_t *st = (desc->stages && i < desc->stage_count) ? &desc->stages[i] : NULL;
            uint32_t tci = st ? st->texcoord_index : i;
            uint32_t flags = st ? st->texture_transform_flags : 0;
            uint32_t count = flags & 0xFu;
            int projected = (flags & D3D9_TTFF_PROJECTED) != 0;
            char src[192];

            switch (tci & 0xF0000u) {
                case D3D9_TCI_CAMERASPACEPOSITION:
                    snprintf(src, sizeof src, "_csPos");
                    break;
                case D3D9_TCI_CAMERASPACENORMAL:
                    snprintf(src, sizeof src, "_csNormal");
                    break;
                case D3D9_TCI_CAMERASPACEREFLECTIONVECTOR:
                    snprintf(src, sizeof src, "reflect(normalize(_csPos), _csNormal)");
                    break;
                case D3D9_TCI_SPHEREMAP:
                    /* handled below: needs a two-statement MSL sequence */
                    break;
                default: {
                    uint32_t idx = tci & 0x7u;
                    if (texcount == 0) snprintf(src, sizeof src, "float3(0.0)");
                    else { if (idx >= texcount) idx = 0; snprintf(src, sizeof src, "float3(o.uv%u.x, o.uv%u.y, 0.0)", idx, idx); }
                    break;
                }
            }

            if ((tci & 0xF0000u) == D3D9_TCI_SPHEREMAP) {
                sb_add(&s, "  { float3 _sr = reflect(normalize(_csPos), _csNormal);\n");
                sb_add(&s, "    float _sm = 2.0 * length(float3(_sr.x, _sr.y, _sr.z + 1.0));\n");
                snprintf(src, sizeof src, "float3(_sr.x / max(_sm, 1e-6) + 0.5, _sr.y / max(_sm, 1e-6) + 0.5, 0.0)");
                sb_add(&s, "  o.uvs%u = float2(%s.x, %s.y); }\n", i, src, src);
                continue;
            }

            if (count == 0) {
                sb_add(&s, "  o.uvs%u = float2(%s.x, %s.y);\n", i, src, src);
            } else {
                sb_add(&s, "  { float4 _t = u.texTransform[%u] * float4(%s.x, %s.y, %s.z, 1.0);\n", i, src, src, src);
                if (projected) {
                    /* D3DTTFF_PROJECTED divides by the LAST of the COUNTn
                     * components passed to the rasterizer (public docs). */
                    const char *div = count == 1 ? "_t.x" : count == 2 ? "_t.y" : count == 3 ? "_t.z" : "_t.w";
                    sb_add(&s, "    float _div = %s;\n", div);
                    sb_add(&s, "    if (abs(_div) > 1e-6) { _t.x /= _div; _t.y /= _div; }\n");
                }
                sb_add(&s, "    o.uvs%u = float2(_t.x, _t.y); }\n", i);
            }
        }
    } /* attribute-index scope */

    /* fog factor: for XYZRHW use the original z (screen depth), for XYZ use
     * NDC z (o.pos.z / o.pos.w). Public D3D9 docs: XYZRHW z is already the
     * viewport depth, no perspective divide applies. */
    if (desc->fog_mode == D3D9_FOG_NONE) {
        sb_add(&s, "  o.fogFactor = 1.0;\n");
    } else {
        const char *dist = has_rhw ? "in.pos.z" : "o.pos.z / max(o.pos.w, 1e-6)";
        if (desc->fog_mode == D3D9_FOG_LINEAR) {
            sb_add(&s, "  { float d = %s; o.fogFactor = clamp((u.fogEnd - d) / max(u.fogEnd - u.fogStart, 1e-6), 0.0, 1.0); }\n", dist);
        } else if (desc->fog_mode == D3D9_FOG_EXP) {
            sb_add(&s, "  { float d = %s; o.fogFactor = clamp(exp(-u.fogDensity * d), 0.0, 1.0); }\n", dist);
        } else {
            sb_add(&s, "  { float d = %s; float f = u.fogDensity * d; o.fogFactor = clamp(exp(-f*f), 0.0, 1.0); }\n", dist);
        }
    }
    /* XYZRHW has no world-space position; disable world-space clip planes. */
    if (has_rhw) sb_add(&s, "  o.clipPos = float4(0.0);\n");
    else sb_add(&s, "  o.clipPos = u.world * in.pos;\n");
    sb_add(&s, "  return o;\n}\n\n");

    /* fragment shader */
    sb_add(&s, "fragment float4 ffp_ps(VSOut in [[stage_in]], constant FFPUniforms& u [[buffer(1)]],\n");
    sb_add(&s, "                        constant D3D9Clip& clip [[buffer(4)]]");

    /* declare textures for active stages */
    /* stage active if color or alpha op != disable */
    for (i = 0; i < desc->stage_count; i++) {
        const d3d9_ffp_stage_t *st = &desc->stages[i];
        uint32_t cop = st->color_op ? st->color_op : D3D9_TEXOP_DISABLE;
        uint32_t aop = st->alpha_op ? st->alpha_op : D3D9_TEXOP_DISABLE;
        if (cop != D3D9_TEXOP_DISABLE || aop != D3D9_TEXOP_DISABLE) {
            sb_add(&s, ", texture2d<float> tex%u [[texture(%u)]], sampler samp%u [[sampler(%u)]]", i, i, i, i);
        }
    }
    sb_add(&s, ") {\n");
    sb_add(&s, "  for (uint i = 0u; i < 6u; ++i)\n"
                "    if ((clip.mask.x & (1u << i)) != 0u && dot(clip.planes[i], in.clipPos) < 0.0f)\n"
                "      discard_fragment();\n");

    /* With lighting, in.diffuse carries the lit color regardless of whether a
     * diffuse vertex attribute exists; the texture stages modulate from it. */
    int fdiff = has_diffuse || desc->lighting;
    int fspec = has_specular || desc->lighting;
    if (fdiff) sb_add(&s, "  float4 current = in.diffuse;\n");
    else sb_add(&s, "  float4 current = float4(1.0);\n");
    sb_add(&s, "  float4 temp = float4(0.0);\n");
    sb_add(&s, "  float2 _bump_delta = float2(0.0);\n");
    sb_add(&s, "  float _bump_lum = 1.0;\n");
    sb_add(&s, "  bool _has_bump_prev = false;\n");
    char tfactor_expr[64] = "u.textureFactor";

    /* stage constants default */
    for (i = 0; i < desc->stage_count; i++) {
        const d3d9_ffp_stage_t *st = &desc->stages[i];
        uint32_t cop = st->color_op ? st->color_op : D3D9_TEXOP_DISABLE;
        uint32_t aop = st->alpha_op ? st->alpha_op : D3D9_TEXOP_DISABLE;
        if (cop == D3D9_TEXOP_DISABLE) {
            /* D3D9: COLOROP DISABLE disables this stage and all subsequent stages. */
            sb_add(&s, "  // stage %u COLOROP DISABLE: remaining stages skipped\n", i);
            break;
        }
        char tex_var[32];
        snprintf(tex_var, sizeof tex_var, "texel%u", i);
        /* sample texture if declared */
        int stage_declared = 1;
        /* check if texture param declared: yes if active */
        sb_add(&s, "  float2 _uv%u = in.uvs%u;\n", i, i);
        sb_add(&s, "  if (_has_bump_prev) _uv%u += _bump_delta;\n", i);
        sb_add(&s, "  float4 texel%u = tex%u.sample(samp%u, _uv%u);\n", i, i, i, i);
        sb_add(&s, "  if (_has_bump_prev) { texel%u.rgb *= _bump_lum; _has_bump_prev = false; }\n", i);
        /* bump delta computation */
        if (cop == D3D9_TEXOP_BUMPENVMAP || cop == D3D9_TEXOP_BUMPENVMAPLUMINANCE) {
            sb_add(&s, "  {\n");
            sb_add(&s, "    float _du = texel%u.r * 2.0 - 1.0;\n", i);
            sb_add(&s, "    float _dv = texel%u.g * 2.0 - 1.0;\n", i);
            sb_add(&s, "    float _du2 = %.6ff * _du + %.6ff * _dv;\n", st->bumpenv_mat00, st->bumpenv_mat10);
            sb_add(&s, "    float _dv2 = %.6ff * _du + %.6ff * _dv;\n", st->bumpenv_mat01, st->bumpenv_mat11);
            sb_add(&s, "    _bump_delta = float2(_du2, _dv2);\n");
            if (cop == D3D9_TEXOP_BUMPENVMAPLUMINANCE) {
                sb_add(&s, "    _bump_lum = texel%u.b * %.6ff + %.6ff;\n", i, st->bumpenv_lscale, st->bumpenv_loffset);
            } else {
                sb_add(&s, "    _bump_lum = 1.0;\n");
            }
            sb_add(&s, "    _has_bump_prev = true;\n");
            sb_add(&s, "  }\n");
        }
        /* constant expr */
        char const_expr[128];
        unpack_color(st->constant, const_expr, sizeof const_expr);

        /* arg expressions */
        char ca0[256], ca1[256], ca2[256], aa0[256], aa1[256], aa2[256];
        uint32_t carg0 = st->color_arg0 ? st->color_arg0 : D3D9_TA_CURRENT;
        uint32_t carg1 = st->color_arg1 ? st->color_arg1 : D3D9_TA_TEXTURE;
        uint32_t carg2 = st->color_arg2 ? st->color_arg2 : D3D9_TA_CURRENT;
        uint32_t aarg0 = st->alpha_arg0 ? st->alpha_arg0 : D3D9_TA_CURRENT;
        uint32_t aarg1 = st->alpha_arg1 ? st->alpha_arg1 : D3D9_TA_TEXTURE;
        uint32_t aarg2 = st->alpha_arg2 ? st->alpha_arg2 : D3D9_TA_CURRENT;

        arg_expr(carg0, st->constant, 1, fdiff, fspec, tex_var, tfactor_expr, const_expr, ca0, sizeof ca0);
        arg_expr(carg1, st->constant, 1, fdiff, fspec, tex_var, tfactor_expr, const_expr, ca1, sizeof ca1);
        arg_expr(carg2, st->constant, 1, fdiff, fspec, tex_var, tfactor_expr, const_expr, ca2, sizeof ca2);
        arg_expr(aarg0, st->constant, 0, fdiff, fspec, tex_var, tfactor_expr, const_expr, aa0, sizeof aa0);
        arg_expr(aarg1, st->constant, 0, fdiff, fspec, tex_var, tfactor_expr, const_expr, aa1, sizeof aa1);
        arg_expr(aarg2, st->constant, 0, fdiff, fspec, tex_var, tfactor_expr, const_expr, aa2, sizeof aa2);

        /* compute new rgb/alpha */
        sb_add(&s, "  {\n");
        sb_add(&s, "    float4 _a0 = %s, _a1 = %s, _a2 = %s;\n", ca0, ca1, ca2);
        sb_add(&s, "    float4 _b0 = %s, _b1 = %s, _b2 = %s;\n", aa0, aa1, aa2);
        sb_add(&s, "    float3 nrgb = current.rgb;\n");
        sb_add(&s, "    float na = current.a;\n");

        /* color op */
        switch (cop) {
            case D3D9_TEXOP_DISABLE:
                break;
            case D3D9_TEXOP_SELECTARG1:
                sb_add(&s, "    nrgb = _a1.rgb;\n");
                break;
            case D3D9_TEXOP_SELECTARG2:
                sb_add(&s, "    nrgb = _a2.rgb;\n");
                break;
            case D3D9_TEXOP_MODULATE:
                sb_add(&s, "    nrgb = _a1.rgb * _a2.rgb;\n");
                break;
            case D3D9_TEXOP_MODULATE2X:
                sb_add(&s, "    nrgb = _a1.rgb * _a2.rgb * 2.0;\n");
                break;
            case D3D9_TEXOP_MODULATE4X:
                sb_add(&s, "    nrgb = _a1.rgb * _a2.rgb * 4.0;\n");
                break;
            case D3D9_TEXOP_ADD:
                sb_add(&s, "    nrgb = _a1.rgb + _a2.rgb;\n");
                break;
            case D3D9_TEXOP_ADDSIGNED:
                sb_add(&s, "    nrgb = _a1.rgb + _a2.rgb - 0.5;\n");
                break;
            case D3D9_TEXOP_ADDSIGNED2X:
                sb_add(&s, "    nrgb = (_a1.rgb + _a2.rgb - 0.5) * 2.0;\n");
                break;
            case D3D9_TEXOP_SUBTRACT:
                sb_add(&s, "    nrgb = _a1.rgb - _a2.rgb;\n");
                break;
            case D3D9_TEXOP_ADDSMOOTH:
                sb_add(&s, "    nrgb = _a1.rgb + _a2.rgb - _a1.rgb * _a2.rgb;\n");
                break;
            case D3D9_TEXOP_BLENDDIFFUSEALPHA:
                if (fdiff)
                    sb_add(&s, "    nrgb = _a1.rgb * in.diffuse.a + _a2.rgb * (1.0 - in.diffuse.a);\n");
                else
                    sb_add(&s, "    nrgb = _a1.rgb * 1.0 + _a2.rgb * 0.0;\n");
                break;
            case D3D9_TEXOP_BLENDTEXTUREALPHA:
                sb_add(&s, "    nrgb = _a1.rgb * texel%u.a + _a2.rgb * (1.0 - texel%u.a);\n", i, i);
                break;
            case D3D9_TEXOP_BLENDFACTORALPHA:
                sb_add(&s, "    nrgb = _a1.rgb * u.textureFactor.a + _a2.rgb * (1.0 - u.textureFactor.a);\n");
                break;
            case D3D9_TEXOP_BLENDTEXTUREALPHAPM:
                sb_add(&s, "    nrgb = _a1.rgb + _a2.rgb * (1.0 - texel%u.a);\n", i);
                break;
            case D3D9_TEXOP_BLENDCURRENTALPHA:
                sb_add(&s, "    nrgb = _a1.rgb * current.a + _a2.rgb * (1.0 - current.a);\n");
                break;
            case D3D9_TEXOP_PREMODULATE:
                sb_add(&s, "    nrgb = _a1.rgb;\n");
                break;
            case D3D9_TEXOP_MODULATEALPHA_ADDCOLOR:
                sb_add(&s, "    nrgb = _a1.rgb + _a1.a * _a2.rgb;\n");
                break;
            case D3D9_TEXOP_MODULATECOLOR_ADDALPHA:
                sb_add(&s, "    nrgb = _a1.rgb * _a2.rgb + _a1.a;\n");
                break;
            case D3D9_TEXOP_MODULATEINVALPHA_ADDCOLOR:
                sb_add(&s, "    nrgb = (1.0 - _a1.a) * _a2.rgb + _a1.rgb;\n");
                break;
            case D3D9_TEXOP_MODULATEINVCOLOR_ADDALPHA:
                sb_add(&s, "    nrgb = (1.0 - _a1.rgb) * _a2.rgb + _a1.a;\n");
                break;
            case D3D9_TEXOP_DOTPRODUCT3:
                sb_add(&s, "    { float3 _da = _a1.rgb * 2.0 - 1.0; float3 _db = _a2.rgb * 2.0 - 1.0; float _dd = clamp(dot(_da,_db), 0.0, 1.0); nrgb = float3(_dd); na = _dd; }\n");
                break;
            case D3D9_TEXOP_MULTIPLYADD:
                sb_add(&s, "    nrgb = _a0.rgb + _a1.rgb * _a2.rgb;\n");
                break;
            case D3D9_TEXOP_LERP:
                sb_add(&s, "    nrgb = _a0.rgb * _a1.rgb + (1.0 - _a0.rgb) * _a2.rgb;\n");
                break;
            case D3D9_TEXOP_BUMPENVMAP:
            case D3D9_TEXOP_BUMPENVMAPLUMINANCE:
                sb_add(&s, "    nrgb = current.rgb;\n");
                break;
            default:
                sb_add(&s, "    nrgb = _a1.rgb;\n");
                break;
        }

        /* alpha op */
        switch (aop) {
            case D3D9_TEXOP_DISABLE:
                break;
            case D3D9_TEXOP_SELECTARG1:
                sb_add(&s, "    na = _b1.a;\n");
                break;
            case D3D9_TEXOP_SELECTARG2:
                sb_add(&s, "    na = _b2.a;\n");
                break;
            case D3D9_TEXOP_MODULATE:
                sb_add(&s, "    na = _b1.a * _b2.a;\n");
                break;
            case D3D9_TEXOP_MODULATE2X:
                sb_add(&s, "    na = _b1.a * _b2.a * 2.0;\n");
                break;
            case D3D9_TEXOP_MODULATE4X:
                sb_add(&s, "    na = _b1.a * _b2.a * 4.0;\n");
                break;
            case D3D9_TEXOP_ADD:
                sb_add(&s, "    na = _b1.a + _b2.a;\n");
                break;
            case D3D9_TEXOP_ADDSIGNED:
                sb_add(&s, "    na = _b1.a + _b2.a - 0.5;\n");
                break;
            case D3D9_TEXOP_ADDSIGNED2X:
                sb_add(&s, "    na = (_b1.a + _b2.a - 0.5) * 2.0;\n");
                break;
            case D3D9_TEXOP_SUBTRACT:
                sb_add(&s, "    na = _b1.a - _b2.a;\n");
                break;
            case D3D9_TEXOP_ADDSMOOTH:
                sb_add(&s, "    na = _b1.a + _b2.a - _b1.a * _b2.a;\n");
                break;
            case D3D9_TEXOP_BLENDDIFFUSEALPHA:
                if (fdiff)
                    sb_add(&s, "    na = _b1.a * in.diffuse.a + _b2.a * (1.0 - in.diffuse.a);\n");
                else
                    sb_add(&s, "    na = _b1.a;\n");
                break;
            case D3D9_TEXOP_BLENDTEXTUREALPHA:
                sb_add(&s, "    na = _b1.a * texel%u.a + _b2.a * (1.0 - texel%u.a);\n", i, i);
                break;
            case D3D9_TEXOP_BLENDFACTORALPHA:
                sb_add(&s, "    na = _b1.a * u.textureFactor.a + _b2.a * (1.0 - u.textureFactor.a);\n");
                break;
            case D3D9_TEXOP_BLENDTEXTUREALPHAPM:
                sb_add(&s, "    na = _b1.a + _b2.a * (1.0 - texel%u.a);\n", i);
                break;
            case D3D9_TEXOP_BLENDCURRENTALPHA:
                sb_add(&s, "    na = _b1.a * current.a + _b2.a * (1.0 - current.a);\n");
                break;
            case D3D9_TEXOP_MODULATEALPHA_ADDCOLOR:
                /* color-only; alpha unchanged */
                break;
            case D3D9_TEXOP_MODULATECOLOR_ADDALPHA:
                /* alpha = arg1.a (already) maybe add? spec: color only; alpha unchanged */
                break;
            case D3D9_TEXOP_DOTPRODUCT3:
                /* alpha set in color op */
                break;
            case D3D9_TEXOP_MULTIPLYADD:
                sb_add(&s, "    na = _b0.a + _b1.a * _b2.a;\n");
                break;
            case D3D9_TEXOP_LERP:
                sb_add(&s, "    na = _b0.a * _b1.a + (1.0 - _b0.a) * _b2.a;\n");
                break;
            case D3D9_TEXOP_BUMPENVMAP:
            case D3D9_TEXOP_BUMPENVMAPLUMINANCE:
                sb_add(&s, "    na = current.a;\n");
                break;
            default:
                sb_add(&s, "    na = _b1.a;\n");
                break;
        }

        sb_add(&s, "    nrgb = clamp(nrgb, 0.0, 1.0);\n");
        sb_add(&s, "    na = clamp(na, 0.0, 1.0);\n");

        uint32_t result_arg = st->result_arg ? st->result_arg : D3D9_TA_CURRENT;
        if (result_arg == D3D9_TA_TEMP) {
            sb_add(&s, "    temp = float4(nrgb, na);\n");
        } else {
            sb_add(&s, "    current = float4(nrgb, na);\n");
        }
        sb_add(&s, "  }\n");
    }

    /* Specular from lighting is added after the texture stages (public D3D9). */
    if (desc->lighting && desc->specular_enable) {
        sb_add(&s, "  current.rgb = clamp(current.rgb + in.specular.rgb, 0.0, 1.0);\n");
    }

    /* fog */
    if (desc->fog_mode != D3D9_FOG_NONE) {
        sb_add(&s, "  current.rgb = mix(u.fogColor.rgb, current.rgb, in.fogFactor);\n");
    }
    /* alpha test */
    if (desc->alpha_test_enable) {
        const char *op = cmp_op(desc->alpha_func);
        if (op) {
            if (desc->alpha_func == D3D9_CMP_NEVER) sb_add(&s, "  discard_fragment();\n");
            else if (desc->alpha_func != D3D9_CMP_ALWAYS)
                sb_add(&s, "  if (!(current.a * 255.0 %s float(%u))) discard_fragment();\n", op, desc->alpha_ref & 0xFFu);
        }
    }
    sb_add(&s, "  return current;\n}\n");

    if (s.oom) { free(s.buf); if (err) *err = "oom"; return -1; }

    *out_msl = s.buf;
    if (out_len) *out_len = s.len;
    return 0;
}
