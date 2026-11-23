/*
 * d3d9_shader.c - Direct3D 9 shader bytecode (vs_1_1 .. vs_3_0,
 * ps_1_1 .. ps_3_0) to Metal Shading Language translator.
 *
 * Token encoding follows Microsoft's public d3d9types.h (D3DSHADER_INSTRUCTION_OPCODE_TYPE,
 * D3DSHADER_PARAM_REGISTER_TYPE, D3DSHADER_PARAM_SRCMOD_TYPE,
 * D3DSHADER_PARAM_DSTMOD_TYPE, D3DSWIZZLE). Every opcode or encoding that is
 * not handled here is a hard error with a message in err_msg: generated MSL
 * never contains a placeholder comment for an unknown instruction.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "d3d9_shader.h"

/* ---- Instruction opcodes (Microsoft d3d9types.h) ---- */
#define D3DSIO_NOP          0x00u
#define D3DSIO_MOV          0x01u
#define D3DSIO_ADD          0x02u
#define D3DSIO_SUB          0x03u
#define D3DSIO_MAD          0x04u
#define D3DSIO_MUL          0x05u
#define D3DSIO_RCP          0x06u
#define D3DSIO_RSQ          0x07u
#define D3DSIO_DP3          0x08u
#define D3DSIO_DP4          0x09u
#define D3DSIO_MIN          0x0Au
#define D3DSIO_MAX          0x0Bu
#define D3DSIO_SLT          0x0Cu
#define D3DSIO_SGE          0x0Du
#define D3DSIO_EXP          0x0Eu
#define D3DSIO_LOG          0x0Fu
#define D3DSIO_LIT          0x10u
#define D3DSIO_DST          0x11u
#define D3DSIO_LRP          0x12u
#define D3DSIO_FRC          0x13u
#define D3DSIO_M4x4         0x14u
#define D3DSIO_M4x3         0x15u
#define D3DSIO_M3x4         0x16u
#define D3DSIO_M3x3         0x17u
#define D3DSIO_M3x2         0x18u
#define D3DSIO_CALL         0x19u
#define D3DSIO_CALLNZ       0x1Au
#define D3DSIO_LOOP         0x1Bu
#define D3DSIO_RET          0x1Cu
#define D3DSIO_ENDLOOP      0x1Du
#define D3DSIO_LABEL        0x1Eu
#define D3DSIO_DCL          0x1Fu
#define D3DSIO_POW          0x20u
#define D3DSIO_CRS          0x21u
#define D3DSIO_SGN          0x22u
#define D3DSIO_ABS          0x23u
#define D3DSIO_NRM          0x24u
#define D3DSIO_SINCOS       0x25u
#define D3DSIO_REP          0x26u
#define D3DSIO_ENDREP       0x27u
#define D3DSIO_IF           0x28u
#define D3DSIO_IFC          0x29u
#define D3DSIO_ELSE         0x2Au
#define D3DSIO_ENDIF        0x2Bu
#define D3DSIO_BREAK        0x2Cu
#define D3DSIO_BREAKC       0x2Du
#define D3DSIO_MOVA         0x2Eu
#define D3DSIO_DEFB         0x2Fu
#define D3DSIO_DEFI         0x30u
#define D3DSIO_TEXCOORD     0x40u
#define D3DSIO_TEXKILL      0x41u
#define D3DSIO_TEX          0x42u
#define D3DSIO_TEXBEM       0x43u
#define D3DSIO_TEXBEML      0x44u
#define D3DSIO_TEXREG2AR    0x45u
#define D3DSIO_TEXREG2GB    0x46u
#define D3DSIO_TEXM3x2PAD   0x47u
#define D3DSIO_TEXM3x2TEX   0x48u
#define D3DSIO_TEXM3x3PAD   0x49u
#define D3DSIO_TEXM3x3TEX   0x4Au
#define D3DSIO_TEXM3x3DIFF  0x4Bu
#define D3DSIO_TEXM3x3SPEC  0x4Cu
#define D3DSIO_TEXM3x3VSPEC 0x4Du
#define D3DSIO_EXPP         0x4Eu
#define D3DSIO_LOGP         0x4Fu
#define D3DSIO_CND          0x50u
#define D3DSIO_DEF          0x51u
#define D3DSIO_TEXREG2RGB   0x52u
#define D3DSIO_TEXDP3TEX    0x53u
#define D3DSIO_TEXM3x2DEPTH 0x54u
#define D3DSIO_TEXDP3       0x55u
#define D3DSIO_TEXM3x3      0x56u
#define D3DSIO_TEXDEPTH     0x57u
#define D3DSIO_CMP          0x58u
#define D3DSIO_BEM          0x59u
#define D3DSIO_DP2ADD       0x5Au
#define D3DSIO_DSX          0x5Bu
#define D3DSIO_DSY          0x5Cu
#define D3DSIO_TEXLDD       0x5Du
#define D3DSIO_SETP         0x5Eu
#define D3DSIO_TEXLDL       0x5Fu
#define D3DSIO_BREAKP       0x60u
#define D3DSIO_PHASE        0xFFFDu
#define D3DSIO_COMMENT      0xFFFEu
#define D3DSIO_END          0xFFFFu

/* ---- Register types (Microsoft d3d9types.h) ---- */
#define D3DSPR_TEMP         0u
#define D3DSPR_INPUT        1u
#define D3DSPR_CONST        2u
/* Register type 3 is D3DSPR_ADDR (a0, int) in a vertex shader, but
 * D3DSPR_TEXTURE (t#, float4 texture coordinate) in a pixel shader. */
#define D3DSPR_ADDR         3u
#define D3DSPR_TEXTURE      3u
#define D3DSPR_RASTOUT      4u
#define D3DSPR_ATTROUT      5u
#define D3DSPR_TEXCRDOUT    6u
#define D3DSPR_CONSTINT     7u
#define D3DSPR_COLOROUT     8u
#define D3DSPR_DEPTHOUT     9u
#define D3DSPR_SAMPLER      10u
#define D3DSPR_CONST2       11u
#define D3DSPR_CONST3       12u
#define D3DSPR_CONST4       13u
#define D3DSPR_CONSTBOOL    14u
#define D3DSPR_LOOP         15u
#define D3DSPR_TEMPFLOAT16  16u
#define D3DSPR_MISCTYPE     17u
#define D3DSPR_LABEL        18u
#define D3DSPR_PREDICATE    19u

#define D3DSP_NOSWIZZLE     0xE4u
#define D3DSI_INSTLENGTH_SHIFT 24u
#define D3DSP_DSTMOD_SHIFT  20u
#define D3DSP_SRCMOD_SHIFT  24u
#define D3DSP_TEXTURETYPE_SHIFT 27u
#define D3DSHADER_INSTRUCTION_PREDICATED 0x10000000u
#define D3DSI_TEXLD_PROJECT 0x00010000u
#define D3DSI_TEXLD_BIAS    0x00020000u
#define D3DSHADER_COMPARISON_SHIFT 16u

/* ---- Text buffer ---- */
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    int     overflow;
} strbuf_t;

static void sb(strbuf_t *sb_, const char *fmt, ...) {
    if (!sb_->buf || sb_->len >= sb_->cap) { sb_->overflow = 1; return; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb_->buf + sb_->len, sb_->cap - sb_->len, fmt, ap);
    va_end(ap);
    if (n < 0) { sb_->overflow = 1; return; }
    if ((size_t)n >= sb_->cap - sb_->len) { sb_->overflow = 1; sb_->len = sb_->cap; return; }
    sb_->len += (size_t)n;
}

typedef struct {
    char    tmp[65536];
    size_t  len;
} stmtbuf_t;

static void stmts(stmtbuf_t *t, const char *fmt, ...) {
    if (t->len + 512 > sizeof(t->tmp)) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t->tmp + t->len, sizeof(t->tmp) - t->len, fmt, ap);
    va_end(ap);
    if (n > 0) t->len += (size_t)n;
}

/* ---- Operand decoding ---- */
typedef struct {
    uint32_t type;          /* D3DSPR_* */
    uint32_t num;
    uint32_t mask;          /* destination write mask (bits 16..19) */
    uint32_t swizzle;       /* source swizzle (bits 16..23) */
    uint32_t mod;           /* source modifier (bits 24..27) */
    uint32_t dmod;          /* destination modifier (bits 20..23) */
    uint32_t shift;         /* destination shift (bits 24..27) */
    int      rel;           /* relative addressing (bit 13) */
} operand_t;

/* Operand tokens follow the Microsoft D3D9 format: register type in bits
 * 28..30 + 11..12, register number in bits 0..10, for every shader model. */
static operand_t parse_operand(uint32_t token, int is_dest) {
    operand_t op;
    memset(&op, 0, sizeof(op));
    op.type = ((token >> 28u) & 7u) | (((token >> 11u) & 3u) << 3u);
    op.num = token & 0x7FFu;
    op.rel = (int)((token >> 13u) & 1u);
    if (is_dest) {
        op.mask = (token >> 16u) & 0x0Fu;
        op.dmod = (token >> D3DSP_DSTMOD_SHIFT) & 0x0Fu;
        op.shift = (token >> 24u) & 0x0Fu;
    } else {
        op.swizzle = (token >> 16u) & 0xFFu;
        op.mod = (token >> D3DSP_SRCMOD_SHIFT) & 0x0Fu;
    }
    return op;
}

typedef struct {
    strbuf_t out;               /* generated MSL */
    char     err[512];
    int      failed;
    int      ps;
    unsigned major, minor;
    uint32_t inputs_mask;
    uint32_t samplers_mask;
    uint32_t sampler_kind[16];
    uint32_t def_f_set, def_i_set, def_b_set;
    float    def_f[256][4];
    int      def_i[16][4];
    int      def_b[16][4];
    int      has_label[256];
    int      label_order[256];
    int      label_count;
    int      has_calls;
    uint32_t cur_token;         /* token index, for error messages */
} ctx_t;

static void fail(ctx_t *c, const char *fmt, ...) {
    if (c->failed) return;
    c->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof(c->err), fmt, ap);
    va_end(ap);
    if (strchr(c->err, '\n')) *strchr(c->err, '\n') = '\0';
}

#define MAX(a, b) ((a) > (b) ? (a) : (b))

static int reg_max(ctx_t *c, uint32_t type) {
    switch (type) {
    case D3DSPR_TEMP: return 16;
    case D3DSPR_INPUT: return 16;
    case D3DSPR_CONST: return 256;
    /* Type 3 means a0 (int, 1 register) in a vertex shader, but t0..t7
     * (float4 texture coordinates, 8 registers) in a pixel shader. */
    case D3DSPR_ADDR: return c->ps ? 8 : 1;
    case D3DSPR_RASTOUT: return 3;
    case D3DSPR_ATTROUT: return 2;
    case D3DSPR_TEXCRDOUT: return 8;
    case D3DSPR_CONSTINT: return 16;
    case D3DSPR_COLOROUT: return 4;
    case D3DSPR_DEPTHOUT: return 1;
    case D3DSPR_SAMPLER: return 16;
    case D3DSPR_CONST2:
    case D3DSPR_CONST3:
    case D3DSPR_CONST4: return 256;
    case D3DSPR_CONSTBOOL: return 16;
    case D3DSPR_LOOP: return 16;
    case D3DSPR_TEMPFLOAT16: return 16;
    case D3DSPR_MISCTYPE: return 2;
    case D3DSPR_LABEL: return 256;
    case D3DSPR_PREDICATE: return 4;
    default: return 0;
    }
}

static int check_reg(ctx_t *c, const operand_t *op, const char *what) {
    int max = reg_max(c, op->type);
    if (!max) {
        fail(c, "token %u: %s uses unknown register type %u", c->cur_token, what, op->type);
        return -1;
    }
    if (op->num >= (uint32_t)max) {
        fail(c, "token %u: %s register number %u out of range (type %u)",
             c->cur_token, what, op->num, op->type);
        return -1;
    }
    if (op->type == D3DSPR_CONST2 || op->type == D3DSPR_CONST3 || op->type == D3DSPR_CONST4) {
        if (op->rel) { fail(c, "token %u: relative c#/i# addressing not supported", c->cur_token); return -1; }
    }
    return 0;
}

/* A register's MSL expression (no swizzle, no modifier). */
static void reg_expr(ctx_t *c, const operand_t *op, char *out, size_t n) {
    switch (op->type) {
    case D3DSPR_TEMP:          snprintf(out, n, "s.r[%u]", op->num); break;
    case D3DSPR_INPUT:         snprintf(out, n, "s.v[%u]", op->num); break;
    case D3DSPR_CONST:
        if (op->rel) snprintf(out, n, "c[int(s.a0.x) + %u]", op->num);
        else if (c->def_f_set & (1u << op->num)) {
            const float *v = c->def_f[op->num];
            snprintf(out, n, "float4(%.9g,%.9g,%.9g,%.9g)", v[0], v[1], v[2], v[3]);
        } else snprintf(out, n, "c[%u]", op->num);
        break;
    case D3DSPR_TEXTURE:       /* type 3: a0 in VS, t# in PS */
        if (c->ps) snprintf(out, n, "s.t[%u]", op->num);  /* t# = TEXCOORD#, float4 */
        else snprintf(out, n, "s.a0");
        break;
    case D3DSPR_RASTOUT:
        if (op->num == 0) snprintf(out, n, "s.oPos");
        else if (op->num == 1) snprintf(out, n, "s.oFog");
        else snprintf(out, n, "s.oPts");
        break;
    case D3DSPR_ATTROUT:       snprintf(out, n, "s.oD[%u]", op->num); break;
    case D3DSPR_TEXCRDOUT:
        if (c->ps) snprintf(out, n, "s.o[%u]", op->num);
        else snprintf(out, n, "s.oT[%u]", op->num);
        break;
    case D3DSPR_CONSTINT:
        if (op->rel) { snprintf(out, n, "ci[int(s.a0.x) + %u]", op->num); }
        else if (c->def_i_set & (1u << op->num)) {
            const int *v = c->def_i[op->num];
            snprintf(out, n, "int4(%d,%d,%d,%d)", v[0], v[1], v[2], v[3]);
        } else snprintf(out, n, "ci[%u]", op->num);
        break;
    case D3DSPR_COLOROUT:      snprintf(out, n, "s.oC[%u]", op->num); break;
    case D3DSPR_DEPTHOUT:      snprintf(out, n, "s.oDepth"); break;
    case D3DSPR_SAMPLER:       snprintf(out, n, "s%u", op->num); break;
    case D3DSPR_CONST2:        snprintf(out, n, "c[%u + 256]", op->num); break;
    case D3DSPR_CONST3:        snprintf(out, n, "c[%u + 512]", op->num); break;
    case D3DSPR_CONST4:        snprintf(out, n, "c[%u + 768]", op->num); break;
    case D3DSPR_CONSTBOOL:
        if (op->rel) { snprintf(out, n, "cb[int(s.a0.x) + %u]", op->num); }
        else if (c->def_b_set & (1u << op->num)) {
            const int *v = c->def_b[op->num];
            snprintf(out, n, "bool4(%s,%s,%s,%s)",
                     v[0] ? "true" : "false", v[1] ? "true" : "false",
                     v[2] ? "true" : "false", v[3] ? "true" : "false");
        } else snprintf(out, n, "cb[%u]", op->num);
        break;
    case D3DSPR_LOOP:          snprintf(out, n, "s.aL[%u]", op->num); break;
    case D3DSPR_TEMPFLOAT16:   snprintf(out, n, "s.r[%u]", op->num); break;
    case D3DSPR_MISCTYPE:
        snprintf(out, n, "%s", op->num == 0 ? "s.vPos" : "s.vFace");
        break;
    case D3DSPR_LABEL:         snprintf(out, n, "d3d9_label%u", op->num); break;
    case D3DSPR_PREDICATE:     snprintf(out, n, "s.p[%u]", op->num); break;
    default:                   snprintf(out, n, "s.r[%u]", op->num); break;
    }
}

static const char swz_char[4] = { 'x', 'y', 'z', 'w' };

static int src_expr(ctx_t *c, const operand_t *op, char *out, size_t n) {
    if (check_reg(c, op, "source") != 0) return -1;
    if (op->mod > 13) {
        fail(c, "token %u: unknown source modifier %u", c->cur_token, op->mod);
        return -1;
    }
    if (op->rel && op->type != D3DSPR_CONST && op->type != D3DSPR_CONSTINT &&
        op->type != D3DSPR_CONSTBOOL && op->type != D3DSPR_INPUT) {
        fail(c, "token %u: relative addressing is not valid for register type %u",
             c->cur_token, op->type);
        return -1;
    }
    if (op->rel && op->type == D3DSPR_INPUT && op->swizzle != D3DSP_NOSWIZZLE) {
        fail(c, "token %u: relative v# addressing requires .xyzw swizzle", c->cur_token);
        return -1;
    }

    char base[96];
    reg_expr(c, op, base, sizeof(base));
    if (op->rel && op->type == D3DSPR_INPUT) {
        /* v[a0.x + n] is read from the local input copy. */
        char idx[96];
        snprintf(idx, sizeof(idx), "s.v[int(s.a0.x) + %u]", op->num);
        snprintf(base, sizeof(base), "%s", idx);
    }
    char sw[16];
    if (op->swizzle == D3DSP_NOSWIZZLE) sw[0] = '\0';
    else {
        unsigned s = op->swizzle;
        snprintf(sw, sizeof(sw), ".%c%c%c%c", swz_char[s & 3],
                 swz_char[(s >> 2) & 3], swz_char[(s >> 4) & 3], swz_char[(s >> 6) & 3]);
    }
    char v[128];
    snprintf(v, sizeof(v), "%s%s", base, sw);

    switch (op->mod) {
    case 0:  snprintf(out, n, "%s", v); break;
    case 1:  snprintf(out, n, "(-%s)", v); break;
    case 2:  snprintf(out, n, "(%s - float4(0.5f))", v); break;
    case 3:  snprintf(out, n, "(-(%s - float4(0.5f)))", v); break;
    case 4:  snprintf(out, n, "d3d9_sign(%s)", v); break;
    case 5:  snprintf(out, n, "(-d3d9_sign(%s))", v); break;
    case 6:  snprintf(out, n, "(float4(1.0f) - %s)", v); break;
    case 7:  snprintf(out, n, "(2.0f * %s)", v); break;
    case 8:  snprintf(out, n, "(-2.0f * %s)", v); break;
    case 9:  snprintf(out, n, "d3d9_dz(%s)", v); break;
    case 10: snprintf(out, n, "d3d9_dw(%s)", v); break;
    case 11: snprintf(out, n, "abs(%s)", v); break;
    case 12: snprintf(out, n, "(-abs(%s))", v); break;
    case 13: snprintf(out, n, "d3d9_not(%s)", v); break;
    default: fail(c, "token %u: unknown source modifier %u", c->cur_token, op->mod); return -1;
    }
    return 0;
}

static int dst_expr(ctx_t *c, const operand_t *op, char *out, size_t n) {
    if (check_reg(c, op, "destination") != 0) return -1;
    if (op->rel) {
        fail(c, "token %u: relative destination addressing is not supported", c->cur_token);
        return -1;
    }
    if (op->dmod & ~(1u | 2u | 4u)) {
        fail(c, "token %u: unknown destination modifier 0x%X", c->cur_token, op->dmod);
        return -1;
    }
    reg_expr(c, op, out, n);
    return 0;
}

/* Register type 3 is an integer destination only in a vertex shader (a0).
 * In a pixel shader it is t#, a float4 texture-coordinate register. */
static int int_dst_register(const ctx_t *c, uint32_t type) {
    return type == D3DSPR_CONSTINT || type == D3DSPR_LOOP ||
           type == D3DSPR_LABEL || (!c->ps && type == D3DSPR_ADDR);
}

/* Append component-wise assignments for the destination write mask. */
static int assign(ctx_t *c, stmtbuf_t *t, const operand_t *dst, const char *expr) {
    char name[96];
    if (dst_expr(c, dst, name, sizeof(name)) != 0) return -1;
    const char *e = expr;
    char shifted[512], clamped[512];
    if (dst->shift) {
        if (dst->type == D3DSPR_CONSTBOOL || dst->type == D3DSPR_PREDICATE) {
            fail(c, "token %u: destination shift is not valid on a bool register", c->cur_token);
            return -1;
        }
        unsigned scale = 1u << dst->shift;
        if (int_dst_register(c, dst->type))
            snprintf(shifted, sizeof(shifted), "(%s) * int4(%u)", e, scale);
        else
            snprintf(shifted, sizeof(shifted), "(%s) * float(%u.0f)", e, scale);
        e = shifted;
    }
    if (dst->dmod & 1u) {
        if (dst->type == D3DSPR_CONSTINT || dst->type == D3DSPR_CONSTBOOL ||
            (!c->ps && dst->type == D3DSPR_ADDR) || dst->type == D3DSPR_LOOP ||
            dst->type == D3DSPR_LABEL || dst->type == D3DSPR_PREDICATE) {
            fail(c, "token %u: saturate is not valid on this destination register", c->cur_token);
            return -1;
        }
        snprintf(clamped, sizeof(clamped), "clamp(%s, float4(0.0f), float4(1.0f))", e);
        e = clamped;
    }
    if (!dst->mask) {
        fail(c, "token %u: destination has an empty write mask", c->cur_token);
        return -1;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!(dst->mask & (1u << i))) continue;
        if (int_dst_register(c, dst->type)) {
            stmts(t, "    %s.%c = int((%s).%c);\n", name, swz_char[i], e, swz_char[i]);
        } else if (dst->type == D3DSPR_CONSTBOOL || dst->type == D3DSPR_PREDICATE) {
            stmts(t, "    %s.%c = bool((%s).%c);\n", name, swz_char[i], e, swz_char[i]);
        } else {
            stmts(t, "    %s.%c = (%s).%c;\n", name, swz_char[i], e, swz_char[i]);
        }
    }
    return 0;
}

/* ---- Opcode arity and version ranges ------------------------------------ */
typedef struct {
    uint16_t op;
    uint8_t ndst, nsrc;    /* operand tokens after the opcode */
    uint8_t ps;            /* 0 = vertex shader, 1 = pixel shader */
    uint8_t minv, maxv;    /* shader model encoded as major * 16 + minor */
} opinfo_t;

/* Operand counts and valid shader models follow Microsoft's public opcode
 * table (as mirrored in Wine's shader_sm1.c). Some instructions have more
 * than one entry because their layout depends on the shader model. */
static const opinfo_t kOps[] = {
    { D3DSIO_NOP, 0, 0, 0, 0x00, 0xFF },
    { D3DSIO_MOV, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_MOVA, 1, 1, 0, 0x20, 0xFF },
    { D3DSIO_ADD, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_SUB, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_MAD, 1, 3, 0, 0x00, 0xFF },
    { D3DSIO_MUL, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_RCP, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_RSQ, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_DP3, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_DP4, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_MIN, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_MAX, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_SLT, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_SGE, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_ABS, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_EXP, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_LOG, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_EXPP, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_LOGP, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_LIT, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_DST, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_LRP, 1, 3, 0, 0x00, 0xFF },
    { D3DSIO_FRC, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_POW, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_CRS, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_SGN, 1, 3, 0, 0x20, 0x21 },
    { D3DSIO_SGN, 1, 1, 0, 0x30, 0xFF },
    { D3DSIO_NRM, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_SINCOS, 1, 3, 0, 0x20, 0x21 },
    { D3DSIO_SINCOS, 1, 1, 0, 0x30, 0xFF },
    { D3DSIO_M4x4, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_M4x3, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_M3x4, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_M3x3, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_M3x2, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_DCL, 0, 2, 0, 0x00, 0xFF },
    { D3DSIO_DEF, 1, 4, 0, 0x00, 0xFF },
    { D3DSIO_DEFB, 1, 1, 0, 0x00, 0xFF },
    { D3DSIO_DEFI, 1, 4, 0, 0x00, 0xFF },
    { D3DSIO_REP, 0, 1, 0, 0x20, 0xFF },
    { D3DSIO_ENDREP, 0, 0, 0, 0x20, 0xFF },
    { D3DSIO_IF, 0, 1, 0, 0x20, 0xFF },
    { D3DSIO_IFC, 0, 2, 0, 0x21, 0xFF },
    { D3DSIO_ELSE, 0, 0, 0, 0x20, 0xFF },
    { D3DSIO_ENDIF, 0, 0, 0, 0x20, 0xFF },
    { D3DSIO_BREAK, 0, 0, 0, 0x21, 0xFF },
    { D3DSIO_BREAKC, 0, 2, 0, 0x21, 0xFF },
    { D3DSIO_BREAKP, 0, 1, 0, 0x00, 0xFF },
    { D3DSIO_CALL, 0, 1, 0, 0x20, 0xFF },
    { D3DSIO_CALLNZ, 0, 2, 0, 0x20, 0xFF },
    { D3DSIO_LOOP, 0, 2, 0, 0x20, 0xFF },
    { D3DSIO_RET, 0, 0, 0, 0x20, 0xFF },
    { D3DSIO_ENDLOOP, 0, 0, 0, 0x20, 0xFF },
    { D3DSIO_LABEL, 0, 1, 0, 0x20, 0xFF },
    { D3DSIO_SETP, 1, 2, 0, 0x00, 0xFF },
    { D3DSIO_TEXLDL, 1, 2, 0, 0x30, 0xFF },
    { D3DSIO_NOP, 0, 0, 1, 0x00, 0xFF },
    { D3DSIO_MOV, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_ADD, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_SUB, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_MAD, 1, 3, 1, 0x00, 0xFF },
    { D3DSIO_MUL, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_RCP, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_RSQ, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_DP3, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_DP4, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_MIN, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_MAX, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_SLT, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_SGE, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_ABS, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_EXP, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_LOG, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_EXPP, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_LOGP, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_DST, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_LRP, 1, 3, 1, 0x00, 0xFF },
    { D3DSIO_FRC, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_CND, 1, 3, 1, 0x10, 0x14 },
    { D3DSIO_CMP, 1, 3, 1, 0x12, 0x30 },
    { D3DSIO_POW, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_CRS, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_NRM, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_SINCOS, 1, 3, 1, 0x20, 0x21 },
    { D3DSIO_SINCOS, 1, 1, 1, 0x30, 0xFF },
    { D3DSIO_DP2ADD, 1, 3, 1, 0x20, 0xFF },
    { D3DSIO_M4x4, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_M4x3, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_M3x4, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_M3x3, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_M3x2, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_DCL, 0, 2, 1, 0x00, 0xFF },
    { D3DSIO_REP, 0, 1, 1, 0x21, 0xFF },
    { D3DSIO_ENDREP, 0, 0, 1, 0x21, 0xFF },
    { D3DSIO_IF, 0, 1, 1, 0x21, 0xFF },
    { D3DSIO_IFC, 0, 2, 1, 0x21, 0xFF },
    { D3DSIO_ELSE, 0, 0, 1, 0x21, 0xFF },
    { D3DSIO_ENDIF, 0, 0, 1, 0x21, 0xFF },
    { D3DSIO_BREAK, 0, 0, 1, 0x21, 0xFF },
    { D3DSIO_BREAKC, 0, 2, 1, 0x21, 0xFF },
    { D3DSIO_BREAKP, 0, 1, 1, 0x00, 0xFF },
    { D3DSIO_CALL, 0, 1, 1, 0x21, 0xFF },
    { D3DSIO_CALLNZ, 0, 2, 1, 0x21, 0xFF },
    { D3DSIO_LOOP, 0, 2, 1, 0x30, 0xFF },
    { D3DSIO_RET, 0, 0, 1, 0x21, 0xFF },
    { D3DSIO_ENDLOOP, 0, 0, 1, 0x30, 0xFF },
    { D3DSIO_LABEL, 0, 1, 1, 0x21, 0xFF },
    { D3DSIO_DEF, 1, 4, 1, 0x00, 0xFF },
    { D3DSIO_DEFB, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_DEFI, 1, 4, 1, 0x00, 0xFF },
    { D3DSIO_TEXCOORD, 1, 0, 1, 0x00, 0x13 },
    { D3DSIO_TEXCOORD, 1, 1, 1, 0x14, 0x14 },
    { D3DSIO_TEXKILL, 1, 0, 1, 0x10, 0x30 },
    { D3DSIO_TEX, 1, 0, 1, 0x00, 0x13 },
    { D3DSIO_TEX, 1, 1, 1, 0x14, 0x14 },
    { D3DSIO_TEX, 1, 2, 1, 0x20, 0xFF },
    { D3DSIO_TEXBEM, 1, 1, 1, 0x00, 0x13 },
    { D3DSIO_TEXBEML, 1, 1, 1, 0x10, 0x13 },
    { D3DSIO_TEXREG2AR, 1, 1, 1, 0x10, 0x13 },
    { D3DSIO_TEXREG2GB, 1, 1, 1, 0x10, 0x13 },
    { D3DSIO_TEXREG2RGB, 1, 1, 1, 0x12, 0x13 },
    { D3DSIO_TEXM3x2PAD, 1, 1, 1, 0x10, 0x13 },
    { D3DSIO_TEXM3x2TEX, 1, 1, 1, 0x10, 0x13 },
    { D3DSIO_TEXM3x3PAD, 1, 1, 1, 0x10, 0x13 },
    { D3DSIO_TEXM3x3DIFF, 1, 1, 1, 0x00, 0xFF },
    { D3DSIO_TEXM3x3SPEC, 1, 2, 1, 0x10, 0x13 },
    { D3DSIO_TEXM3x3VSPEC, 1, 1, 1, 0x10, 0x13 },
    { D3DSIO_TEXM3x3TEX, 1, 1, 1, 0x10, 0x13 },
    { D3DSIO_TEXDP3TEX, 1, 1, 1, 0x12, 0x13 },
    { D3DSIO_TEXM3x2DEPTH, 1, 1, 1, 0x13, 0x13 },
    { D3DSIO_TEXDP3, 1, 1, 1, 0x12, 0x13 },
    { D3DSIO_TEXM3x3, 1, 1, 1, 0x12, 0x13 },
    { D3DSIO_TEXDEPTH, 1, 0, 1, 0x14, 0x14 },
    { D3DSIO_BEM, 1, 2, 1, 0x14, 0x14 },
    { D3DSIO_DSX, 1, 1, 1, 0x21, 0xFF },
    { D3DSIO_DSY, 1, 1, 1, 0x21, 0xFF },
    { D3DSIO_TEXLDD, 1, 4, 1, 0x21, 0xFF },
    { D3DSIO_SETP, 1, 2, 1, 0x00, 0xFF },
    { D3DSIO_TEXLDL, 1, 2, 1, 0x30, 0xFF },
    { D3DSIO_PHASE, 0, 0, 1, 0x00, 0xFF },
};

static const opinfo_t *op_info(uint16_t op) {
    for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++)
        if (kOps[i].op == op) return &kOps[i];
    return NULL;
}

/* Reject opcodes that Microsoft defines only for another shader model. */
static int version_supported(const ctx_t *c, uint16_t op) {
    uint8_t v = (uint8_t)(c->major * 16u + c->minor);
    for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++) {
        if (kOps[i].op != op || kOps[i].ps != (uint8_t)c->ps) continue;
        if (v >= kOps[i].minv && v <= kOps[i].maxv) return 1;
    }
    return 0;
}

/* Layout for the current shader model; first_pass rejects a version mismatch
 * before this is used. */
static opinfo_t effective_op_info(const ctx_t *c, uint16_t op) {
    uint8_t v = (uint8_t)(c->major * 16u + c->minor);
    for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++) {
        if (kOps[i].op != op || kOps[i].ps != (uint8_t)c->ps) continue;
        if (v >= kOps[i].minv && v <= kOps[i].maxv) return kOps[i];
    }
    const opinfo_t *base = op_info(op);
    return base ? *base : (opinfo_t){ 0, 0, 0, 0, 0, 0 };
}

/* Number of tokens following the opcode token. Relative-address tokens
 * (SM 2+) and the optional predicate token are included. */
static uint32_t operand_count(ctx_t *c, const uint32_t *tokens, uint32_t n,
                               uint32_t idx, const opinfo_t *info, int predicated) {
    /* DEF/DEFI/DEFB value tokens are raw 32-bit literals, not register
     * operands. Bit 13 of a float literal is part of its mantissa; treating
     * it as the relative-addressing bit inflates the count (e.g. 0.9f and
     * 0.7f both have bit 13 set) and desynchronises the instruction stream. */
    uint32_t op = tokens[idx] & 0xFFFFu;
    int literal_values = (op == D3DSIO_DEF || op == D3DSIO_DEFI || op == D3DSIO_DEFB);
    uint32_t pos = idx + 1, count = 0;
    for (uint32_t k = 0; k < info->ndst; k++) {
        if (pos >= n) return UINT32_MAX;
        operand_t d = parse_operand(tokens[pos++], 1);
        count++;
        if (!literal_values && d.rel && c->major >= 2) {
            if (pos >= n) return UINT32_MAX;
            pos++; count++;
        }
    }
    if (predicated) {
        if (pos >= n) return UINT32_MAX;
        pos++; count++;
    }
    for (uint32_t k = 0; k < info->nsrc; k++) {
        if (pos >= n) return UINT32_MAX;
        operand_t s = parse_operand(tokens[pos++], 0);
        count++;
        if (!literal_values && s.rel && c->major >= 2) {
            if (pos >= n) return UINT32_MAX;
            pos++; count++;
        }
    }
    return count;
}

static uint32_t predicate_token_index(ctx_t *c, const uint32_t *tokens,
                                       uint32_t idx, const opinfo_t *info) {
    uint32_t pos = idx + 1;
    for (uint32_t k = 0; k < info->ndst; k++) {
        operand_t d = parse_operand(tokens[pos++], 1);
        if (d.rel && c->major >= 2) pos++;
    }
    return pos;
}

/* ---- Fixed MSL declarations ---------------------------------------------- */
static void emit_preamble(ctx_t *c) {
    strbuf_t *b = &c->out;
    sb(b, "// Translated from D3D9 %s_%u_%u\n", c->ps ? "ps" : "vs", c->major, c->minor);
    sb(b, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    sb(b, "struct D3D9REGS {\n"
          "    float4 r[16]; float4 v[16]; float4 t[8]; float4 oT[8]; float4 oD[2];\n"
          "    float4 oC[4]; float4 o[16]; float4 oPos, oFog, oPts, oDepth;\n"
          "    float4 vPos, vFace;\n"
          "    int4 a0, aL[16], ci[16];\n"
          "    bool4 cb[16], p[4];\n"
          "};\n\n"
          "struct D3D9Clip { uint4 mask; float4 planes[6]; };\n\n");
    sb(b, "static float4 d3d9_sign(float4 v) { return sign(v); }\n"
          "static float4 d3d9_dz(float4 v) { return float4(v.z); }\n"
          "static float4 d3d9_dw(float4 v) { return float4(v.w); }\n"
          "static float4 d3d9_not(float4 v) { return select(float4(0.0f), float4(1.0f), !bool4(v)); }\n"
          "static float4 d3d9_lit(float4 v) {\n"
          "    float a = (v.x > 0.0f) ? v.y : 0.0f;\n"
          "    float b = (v.x > 0.0f) ? pow(max(v.z, 0.0f), v.w) : 0.0f;\n"
          "    return float4(1.0f, a, 0.0f, b);\n"
          "}\n"
          "static float4 d3d9_nrm(float4 v) { return float4(normalize(v.xyz), v.w); }\n"
          "static float4 d3d9_mat4x4(float4 v, constant float4 *m) {\n"
          "    return float4(dot(v, m[0]), dot(v, m[1]), dot(v, m[2]), dot(v, m[3]));\n"
          "}\n"
          "static float4 d3d9_mat4x3(float4 v, constant float4 *m) {\n"
          "    return float4(dot(v, m[0]), dot(v, m[1]), dot(v, m[2]), 1.0f);\n"
          "}\n"
          "static float4 d3d9_mat3x4(float4 v, constant float4 *m) {\n"
          "    return float4(dot(v.xyz, m[0].xyz), dot(v.xyz, m[1].xyz),\n"
          "                  dot(v.xyz, m[2].xyz), dot(v.xyz, m[3].xyz));\n"
          "}\n"
          "static float4 d3d9_mat3x3(float4 v, constant float4 *m) {\n"
          "    return float4(dot(v.xyz, m[0].xyz), dot(v.xyz, m[1].xyz),\n"
          "                  dot(v.xyz, m[2].xyz), 1.0f);\n"
          "}\n"
          "static float4 d3d9_mat3x2(float4 v, constant float4 *m) {\n"
          "    return float4(dot(v.xyz, m[0].xyz), dot(v.xyz, m[1].xyz), 0.0f, 1.0f);\n"
          "}\n\n");

    if (c->ps) {
        sb(b, "struct PS_IN {\n"
              "    float4 pos [[position]];\n"
              "    float4 color0 [[user(color0)]];\n"
              "    float4 color1 [[user(color1)]];\n");
        for (int i = 0; i < 8; i++) sb(b, "    float4 uv%d [[user(uv%d)]];\n", i, i);
        for (int i = 0; i < 16; i++) sb(b, "    float4 v%d [[user(v%d)]];\n", i, i);
        sb(b, "};\nstruct PS_OUT { float4 color0 [[color(0)]]; };\n\n");
        sb(b, "static PS_OUT d3d9_pack(thread D3D9REGS &s) {\n"
              "    PS_OUT o; o.color0 = s.oC[0]; return o;\n}\n\n");
    } else {
        sb(b, "struct VS_IN {\n");
        for (int i = 0; i < 16; i++) sb(b, "    float4 v%d [[attribute(%d)]];\n", i, i);
        sb(b, "};\nstruct VS_OUT {\n"
              "    float4 pos [[position]];\n"
              "    float4 color0 [[user(color0)]];\n"
              "    float4 color1 [[user(color1)]];\n");
        for (int i = 0; i < 8; i++) sb(b, "    float4 uv%d [[user(uv%d)]];\n", i, i);
        sb(b, "    float clip_distance [[clip_distance]] [6];\n");
        sb(b, "};\n");
        sb(b, "static VS_OUT d3d9_pack(thread D3D9REGS &s, constant D3D9Clip& clip) {\n"
              "    VS_OUT o; o.pos = s.oPos; o.color0 = s.oD[0]; o.color1 = s.oD[1];\n");
        for (int i = 0; i < 8; i++) sb(b, "    o.uv%d = s.oT[%d];\n", i, i);
        sb(b, "    for (uint i = 0u; i < 6u; ++i)\n"
              "        o.clip_distance[i] = ((clip.mask.x & (1u << i)) != 0u) ? dot(clip.planes[i], s.oPos) : 1.0f;\n");
        sb(b, "    return o;\n}\n\n");
    }
}

static const char *tex_type(uint32_t kind) {
    switch (kind) {
    case 1: return "texture1d<float>";
    case 2: return "texture2d<float>";
    case 3: return "texturecube<float>";
    case 4: return "texture3d<float>";
    default: return "texture2d<float>";
    }
}

/* Subroutine prototypes: Metal has no goto/labels, so D3D9 CALL/CALLNZ/LABEL
 * become real functions that share the register state by thread reference. */
static void emit_sub_prototypes(ctx_t *c) {
    if (!c->has_calls && !c->label_count) return;
    for (int i = 0; i < c->label_count; i++) {
        unsigned l = (unsigned)c->label_order[i];
        sb(&c->out, "static void d3d9_label%u(thread D3D9REGS &s, constant float4 *c,\n"
                    "                        constant int4 *ci, constant bool4 *cb", l);
        for (unsigned k = 0; k < 16; k++) {
            if (c->samplers_mask & (1u << k))
                sb(&c->out, ",\n                        %s tex%u, sampler samp%u",
                   tex_type(c->sampler_kind[k]), k, k);
        }
        sb(&c->out, ");\n");
    }
}

/* PS 1.x texture instructions select the sampler through the destination
 * texture register, without a dcl_s# token. */
static int uses_implicit_sampler(uint16_t op) {
    switch (op) {
    case D3DSIO_TEX:
    case D3DSIO_TEXBEM:
    case D3DSIO_TEXBEML:
    case D3DSIO_TEXREG2AR:
    case D3DSIO_TEXREG2GB:
    case D3DSIO_TEXREG2RGB:
    case D3DSIO_TEXM3x2PAD:
    case D3DSIO_TEXM3x2TEX:
    case D3DSIO_TEXM3x3PAD:
    case D3DSIO_TEXM3x3DIFF:
    case D3DSIO_TEXM3x3SPEC:
    case D3DSIO_TEXM3x3VSPEC:
    case D3DSIO_TEXM3x3TEX:
    case D3DSIO_TEXDP3TEX:
    case D3DSIO_TEXM3x2DEPTH:
    case D3DSIO_TEXDP3:
    case D3DSIO_TEXM3x3:
        return 1;
    default:
        return 0;
    }
}

/* ---- First pass: validate and collect declarations ----------------------- */
static int first_pass(ctx_t *c, const uint32_t *tokens, uint32_t n) {
    int saw_end = 0;
    for (uint32_t i = 1; i < n; i++) {
        c->cur_token = i;
        uint32_t tok = tokens[i];
        uint32_t op = tok & 0xFFFFu;
        if (op == D3DSIO_END) { saw_end = 1; break; }
        if (op == D3DSIO_COMMENT) {
            uint32_t words = (tok >> 16u) & 0x7FFFu;
            if (words > n - i - 2) { fail(c, "token %u: comment overruns bytecode", i); return -1; }
            i += words;   /* the for-loop below adds the opcode token */
            continue;
        }
        if (!op_info((uint16_t)op)) { fail(c, "token %u: unknown opcode 0x%04X", i, op); return -1; }
        if (!version_supported(c, (uint16_t)op)) {
            fail(c, "token %u: opcode 0x%04X is not supported in %s_%u_%u",
                 i, op, c->ps ? "ps" : "vs", c->major, c->minor);
            return -1;
        }
        opinfo_t info = effective_op_info(c, (uint16_t)op);
        int predicated = (tok & D3DSHADER_INSTRUCTION_PREDICATED) ? 1 : 0;
        uint32_t count = operand_count(c, tokens, n, i, &info, predicated);
        if (count == UINT32_MAX) { fail(c, "token %u: instruction overruns bytecode", i); return -1; }
        uint32_t declared = (tok >> D3DSI_INSTLENGTH_SHIFT) & 0x0Fu;
        if (declared && declared != count) {
            fail(c, "token %u: opcode 0x%04X declares %u operands, layout needs %u",
                 i, op, declared, count);
            return -1;
        }
        if (!declared && c->major >= 2 && count) {
            fail(c, "token %u: opcode 0x%04X has no instruction length", i, op);
            return -1;
        }
        if (i + 1 + count >= n) { fail(c, "token %u: instruction overruns bytecode", i); return -1; }

        if (op == D3DSIO_DCL) {
            uint32_t regtok = tokens[i + 2];
            operand_t d = parse_operand(regtok, 1);
            if (check_reg(c, &d, "dcl") != 0) return -1;
            if (d.type == D3DSPR_SAMPLER) {
                uint32_t kind = (tokens[i + 1] >> D3DSP_TEXTURETYPE_SHIFT) & 7u;
                if (kind > 4) { fail(c, "token %u: unknown sampler texture type %u", i, kind); return -1; }
                if (!kind) kind = 2;     /* D3DSTT_UNKNOWN defaults to 2D */
                c->sampler_kind[d.num] = kind;
                c->samplers_mask |= 1u << d.num;
            } else if (d.type == D3DSPR_INPUT) {
                c->inputs_mask |= 1u << d.num;
            }
        }
        if (c->ps && c->major == 1 && uses_implicit_sampler((uint16_t)op)) {
            operand_t d = parse_operand(tokens[i + 1], 1);
            if (check_reg(c, &d, "texture destination") != 0) return -1;
            c->samplers_mask |= 1u << d.num;
            if (!c->sampler_kind[d.num]) c->sampler_kind[d.num] = 2; /* 2D default */
        }
        if (op == D3DSIO_LABEL || op == D3DSIO_CALL || op == D3DSIO_CALLNZ) {
            uint32_t rtok = tokens[i + 1];
            operand_t l = parse_operand(rtok, 0);
            if (l.type != D3DSPR_LABEL) {
                fail(c, "token %u: call/label does not use a label register", i);
                return -1;
            }
            if (check_reg(c, &l, "label") != 0) return -1;
            if (op == D3DSIO_LABEL && !c->has_label[l.num]) {
                c->has_label[l.num] = 1;
                c->label_order[c->label_count++] = (int)l.num;
            }
            if (op != D3DSIO_LABEL) c->has_calls = 1;
        } else if (op == D3DSIO_DEF || op == D3DSIO_DEFI || op == D3DSIO_DEFB) {
            operand_t d = parse_operand(tokens[i + 1], 1);
            if (check_reg(c, &d, "def") != 0) return -1;
            if (op == D3DSIO_DEF) {
                if (d.type != D3DSPR_CONST) { fail(c, "token %u: def target is not c#", i); return -1; }
                for (int k = 0; k < 4; k++)
                    memcpy(&c->def_f[d.num][k], &tokens[i + 2 + k], 4);
                c->def_f_set |= 1u << d.num;
            } else {
                if (d.type != D3DSPR_CONSTINT && d.type != D3DSPR_CONSTBOOL) {
                    fail(c, "token %u: defi/defb target is not i#/b#", i); return -1;
                }
                int nvals = (op == D3DSIO_DEFB) ? 1 : 4;
                if ((uint32_t)(1 + nvals) > count) {
                    fail(c, "token %u: defi/defb has too few operands", i);
                    return -1;
                }
                for (int k = 0; k < 4; k++) {
                    uint32_t raw = (k < nvals) ? tokens[i + 2 + k] : 0u;
                    c->def_i[d.num][k] = (int)raw;
                    c->def_b[d.num][k] = raw & 1u;
                }
                if (d.type == D3DSPR_CONSTINT) c->def_i_set |= 1u << d.num;
                else c->def_b_set |= 1u << d.num;
            }
        }
        i += count;   /* the for-loop below adds the opcode token */
    }
    if (!saw_end) { fail(c, "shader has no D3DSIO_END token"); return -1; }
    for (int i = 0; i < c->label_count; i++) {
        unsigned l = (unsigned)c->label_order[i];
        if (!c->has_label[l]) { fail(c, "call to undefined label %u", l); return -1; }
    }
    return 0;
}

/* ---- Statements for one instruction -------------------------------------- */
static int compare_expr(uint32_t mode, const char *a, const char *b, char *out, size_t n) {
    const char *op;
    switch (mode) {
    case 1: op = ">";  break;   /* D3DSPC_GT */
    case 2: op = "=="; break;   /* D3DSPC_EQ */
    case 3: op = ">="; break;   /* D3DSPC_GE */
    case 4: op = "<";  break;   /* D3DSPC_LT */
    case 5: op = "!="; break;   /* D3DSPC_NE */
    case 6: op = "<="; break;   /* D3DSPC_LE */
    default: return -1;
    }
    /* D3D9 comparisons are scalar: only component .x decides the branch. */
    snprintf(out, n, "(%s).x %s (%s).x", a, op, b);
    return 0;
}

static int sampler_expr(ctx_t *c, uint32_t num, const char *uv, uint32_t flags,
                        char *out, size_t n) {
    if (!(c->samplers_mask & (1u << num))) {
        fail(c, "token %u: sampler s%u used without dcl_s#", c->cur_token, num);
        return -1;
    }
    uint32_t kind = c->sampler_kind[num];
    char coord[256];
    if (kind == 1) snprintf(coord, sizeof(coord), "(%s).x", uv);
    else if (kind == 3 || kind == 4) snprintf(coord, sizeof(coord), "(%s).xyz", uv);
    else if (flags & D3DSI_TEXLD_PROJECT) snprintf(coord, sizeof(coord), "((%s).xy / (%s).w)", uv, uv);
    else snprintf(coord, sizeof(coord), "(%s).xy", uv);

    if (flags & D3DSI_TEXLD_BIAS)
        snprintf(out, n, "tex%u.sample(samp%u, %s, float4(0.0f).x)", num, num, coord);
    else
        snprintf(out, n, "tex%u.sample(samp%u, %s)", num, num, coord);
    return 0;
}

/* PS 1.x has no sampler operand: the destination texture register number
 * selects the sampler. PS 1.0-1.3 also take the coordinates from that same
 * register when the opcode has no source operand at all. */
static operand_t dest_as_source(const operand_t *d) {
    operand_t s = *d;
    s.swizzle = D3DSP_NOSWIZZLE;
    s.mod = 0;
    s.mask = 0;
    s.dmod = 0;
    s.shift = 0;
    return s;
}

static int texture_sampler(ctx_t *c, const opinfo_t *info, const operand_t *dst,
                           const operand_t *src, uint32_t *sampler) {
    if (c->ps && c->major < 2) {
        *sampler = dst->num;
        return 0;
    }
    uint32_t idx = info->nsrc ? info->nsrc - 1u : 0u;
    if (check_reg(c, &src[idx], "sampler") != 0) return -1;
    if (src[idx].type != D3DSPR_SAMPLER) {
        fail(c, "token %u: texture instruction needs a sampler operand", c->cur_token);
        return -1;
    }
    *sampler = src[idx].num;
    return 0;
}

static int emit_one(ctx_t *c, const uint32_t *tokens, uint32_t idx,
                    stmtbuf_t *t, int in_sub) {
    uint32_t tok = tokens[idx];
    uint16_t op = (uint16_t)(tok & 0xFFFFu);
    if (!op_info(op)) { fail(c, "token %u: unknown opcode 0x%04X", idx, op); return -1; }
    if (!version_supported(c, op)) {
        fail(c, "token %u: opcode 0x%04X is not supported in %s_%u_%u",
             idx, op, c->ps ? "ps" : "vs", c->major, c->minor);
        return -1;
    }
    opinfo_t inf = effective_op_info(c, op);
    const opinfo_t *info = &inf;
    operand_t d[4], s[4];
    memset(d, 0, sizeof(d));
    memset(s, 0, sizeof(s));
    uint32_t pos = idx + 1;
    for (uint32_t k = 0; k < info->ndst; k++) {
        d[k] = parse_operand(tokens[pos++], 1);
        if (d[k].rel && c->major >= 2) pos++;  /* SM2+ relative address token */
    }
    if (tok & D3DSHADER_INSTRUCTION_PREDICATED) {
        pos++;  /* predicate register token (handled by emit_range) */
    }
    for (uint32_t k = 0; k < info->nsrc; k++) {
        s[k] = parse_operand(tokens[pos++], 0);
        if (s[k].rel && c->major >= 2) pos++;  /* SM2+ relative address token */
    }

    char a[256], b[256], e[512], cmpe[256], name[96];
    a[0] = b[0] = e[0] = cmpe[0] = name[0] = '\0';

    switch (op) {
    case D3DSIO_NOP:
    case D3DSIO_DCL:
    case D3DSIO_LABEL:
    case D3DSIO_PHASE:
    case D3DSIO_DEF:
    case D3DSIO_DEFI:
    case D3DSIO_DEFB:
        /* def/dcl/label were handled in the first pass / caller. */
        break;

    case D3DSIO_COMMENT:
        break;

    case D3DSIO_RET:
        if (in_sub) stmts(t, "    return;\n");
        else stmts(t, c->ps ? "    return d3d9_pack(s);\n" : "    return d3d9_pack(s, clip);\n");
        break;

    case D3DSIO_CALL: {
        char label[64];
        reg_expr(c, &s[0], label, sizeof(label));
        stmts(t, "    %s(s, c, ci, cb", label);
        for (unsigned k = 0; k < 16; k++)
            if (c->samplers_mask & (1u << k)) stmts(t, ", tex%u, samp%u", k, k);
        stmts(t, ");\n");
        break;
    }
    case D3DSIO_CALLNZ: {
        char label[64], cond[256];
        reg_expr(c, &s[0], label, sizeof(label));
        if (src_expr(c, &s[1], cond, sizeof(cond)) != 0) return -1;
        stmts(t, "    if (bool((%s).x)) { %s(s, c, ci, cb", cond, label);
        for (unsigned k = 0; k < 16; k++)
            if (c->samplers_mask & (1u << k)) stmts(t, ", tex%u, samp%u", k, k);
        stmts(t, "); }\n");
        break;
    }
    case D3DSIO_LOOP: {
        char base[96];
        if (check_reg(c, &s[1], "loop count") != 0) return -1;
        reg_expr(c, &s[1], base, sizeof(base));
        uint32_t ln = s[0].num;
        /* D3D9 "loop aL, i#": i#.x is the iteration count, i#.y the initial
         * value of aL and i#.z the step. The old code compared the whole
         * int4 counter against i#, so a normal "defi i0, 3, 0, 1, 0" ran
         * zero iterations (0 < 0 is false in the .y lane). */
        stmts(t, "    for (int it%u = 0; it%u < (%s).x; ++it%u) {\n",
              ln, ln, base, ln);
        stmts(t, "        s.aL[%u] = int4((%s).y + it%u * (%s).z);\n",
              ln, base, ln, base);
        break;
    }
    case D3DSIO_ENDLOOP:
    case D3DSIO_ENDREP:
    case D3DSIO_ENDIF:
        stmts(t, "    }\n");
        break;
    case D3DSIO_REP: {
        char count[96];
        if (check_reg(c, &s[0], "rep count") != 0) return -1;
        reg_expr(c, &s[0], count, sizeof(count));
        stmts(t, "    for (int rep = %s.x; rep > 0; --rep) {\n", count);
        break;
    }
    case D3DSIO_IF:
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        stmts(t, "    if (bool((%s).x)) {\n", a);
        break;
    case D3DSIO_IFC: {
        uint32_t mode = (tok >> D3DSHADER_COMPARISON_SHIFT) & 0xFFu;
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        if (src_expr(c, &s[1], b, sizeof(b)) != 0) return -1;
        if (compare_expr(mode, a, b, cmpe, sizeof(cmpe)) != 0) {
            fail(c, "token %u: unknown comparison mode %u", idx, mode);
            return -1;
        }
        stmts(t, "    if (%s) {\n", cmpe);
        break;
    }
    case D3DSIO_ELSE:
        stmts(t, "    } else {\n");
        break;
    case D3DSIO_BREAK:
        stmts(t, "    break;\n");
        break;
    case D3DSIO_BREAKC: {
        uint32_t mode = (tok >> D3DSHADER_COMPARISON_SHIFT) & 0xFFu;
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        if (src_expr(c, &s[1], b, sizeof(b)) != 0) return -1;
        if (compare_expr(mode, a, b, cmpe, sizeof(cmpe)) != 0) {
            fail(c, "token %u: unknown comparison mode %u", idx, mode);
            return -1;
        }
        stmts(t, "    if (%s) break;\n", cmpe);
        break;
    }
    case D3DSIO_BREAKP:
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        stmts(t, "    if (bool((%s).x)) break;\n", a);
        break;

    case D3DSIO_MOV:
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        if (assign(c, t, &d[0], a) != 0) return -1;
        break;
    case D3DSIO_MOVA:
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        snprintf(e, sizeof(e), "floor(%s + float4(0.5f))", a);
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    case D3DSIO_ADD:
    case D3DSIO_SUB:
    case D3DSIO_MUL:
    case D3DSIO_MIN:
    case D3DSIO_MAX:
    case D3DSIO_DP3:
    case D3DSIO_DP4:
    case D3DSIO_SLT:
    case D3DSIO_SGE:
    case D3DSIO_POW:
    case D3DSIO_CRS:
    case D3DSIO_LRP:
    case D3DSIO_CMP:
    case D3DSIO_DST:
    case D3DSIO_DP2ADD: {
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        if (src_expr(c, &s[1], b, sizeof(b)) != 0) return -1;
        switch (op) {
        case D3DSIO_ADD:      snprintf(e, sizeof(e), "(%s + %s)", a, b); break;
        case D3DSIO_SUB:      snprintf(e, sizeof(e), "(%s - %s)", a, b); break;
        case D3DSIO_MUL:      snprintf(e, sizeof(e), "(%s * %s)", a, b); break;
        case D3DSIO_MIN:      snprintf(e, sizeof(e), "min(%s, %s)", a, b); break;
        case D3DSIO_MAX:      snprintf(e, sizeof(e), "max(%s, %s)", a, b); break;
        case D3DSIO_DP3:      snprintf(e, sizeof(e), "float4(dot((%s).xyz, (%s).xyz))", a, b); break;
        case D3DSIO_DP4:      snprintf(e, sizeof(e), "float4(dot(%s, %s))", a, b); break;
        case D3DSIO_DP2ADD: {
            char cc[256];
            if (src_expr(c, &s[2], cc, sizeof(cc)) != 0) return -1;
            /* D3D9 dp2add: src2 + src0.x*src1.x + src0.y*src1.y. */
            snprintf(e, sizeof(e), "(%s + ((%s).x * (%s).x + (%s).y * (%s).y))",
                     cc, a, b, a, b);
            break;
        }
        case D3DSIO_SLT:      snprintf(e, sizeof(e), "select(float4(0.0f), float4(1.0f), %s < %s)", a, b); break;
        case D3DSIO_SGE:      snprintf(e, sizeof(e), "select(float4(0.0f), float4(1.0f), %s >= %s)", a, b); break;
        case D3DSIO_POW:      snprintf(e, sizeof(e), "pow(%s, %s)", a, b); break;
        case D3DSIO_CRS:      snprintf(e, sizeof(e), "float4(cross((%s).xyz, (%s).xyz), 0.0f)", a, b); break;
        case D3DSIO_LRP: {
            char cc[256];
            if (src_expr(c, &s[2], cc, sizeof(cc)) != 0) return -1;
            snprintf(e, sizeof(e), "mix(%s, %s, %s)", cc, b, a);
            break;
        }
        case D3DSIO_DST:
            snprintf(e, sizeof(e), "float4(1.0f, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a, b);
            break;
        case D3DSIO_CMP: {
            char cc[256];
            if (src_expr(c, &s[2], cc, sizeof(cc)) != 0) return -1;
            snprintf(e, sizeof(e), "select(%s, %s, bool4(%s >= float4(0.0f)))", cc, b, a);
            break;
        }
        default: break;
        }
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    }
    case D3DSIO_MAD: {
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        if (src_expr(c, &s[1], b, sizeof(b)) != 0) return -1;
        char cc[256];
        if (src_expr(c, &s[2], cc, sizeof(cc)) != 0) return -1;
        snprintf(e, sizeof(e), "(%s * %s + %s)", a, b, cc);
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    }
    case D3DSIO_CND: {
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        if (src_expr(c, &s[1], b, sizeof(b)) != 0) return -1;
        char cc[256];
        if (src_expr(c, &s[2], cc, sizeof(cc)) != 0) return -1;
        /* D3D9 cnd: src1 where src0 > 0.5, src2 otherwise (per component). */
        snprintf(e, sizeof(e), "select(%s, %s, bool4(%s > float4(0.5f)))", cc, b, a);
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    }
    case D3DSIO_RCP:
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        snprintf(e, sizeof(e), "(float4(1.0f) / %s)", a);
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    case D3DSIO_RSQ:
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        snprintf(e, sizeof(e), "rsqrt(abs(%s))", a);
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    case D3DSIO_ABS:
    case D3DSIO_SGN:
    case D3DSIO_FRC:
    case D3DSIO_NRM:
    case D3DSIO_EXP:
    case D3DSIO_LOG:
    case D3DSIO_EXPP:
    case D3DSIO_LOGP:
    case D3DSIO_LIT:
    case D3DSIO_DSX:
    case D3DSIO_DSY:
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        if (op == D3DSIO_SGN && info->nsrc == 3) {
            /* vs_2_0 sgn has two scratch operands which do not affect the result. */
            char scratch[128];
            for (uint32_t k = 1; k < 3; k++)
                if (src_expr(c, &s[k], scratch, sizeof(scratch)) != 0) return -1;
        }
        switch (op) {
        case D3DSIO_ABS:  snprintf(e, sizeof(e), "abs(%s)", a); break;
        case D3DSIO_SGN:  snprintf(e, sizeof(e), "sign(%s)", a); break;
        case D3DSIO_FRC:  snprintf(e, sizeof(e), "fract(%s)", a); break;
        case D3DSIO_NRM:  snprintf(e, sizeof(e), "d3d9_nrm(%s)", a); break;
        case D3DSIO_EXP:
        case D3DSIO_EXPP: snprintf(e, sizeof(e), "float4(exp2((%s).w))", a); break;
        case D3DSIO_LOG:
        case D3DSIO_LOGP: snprintf(e, sizeof(e), "float4(log2(max((%s).x, 1e-20f)))", a); break;
        case D3DSIO_LIT:  snprintf(e, sizeof(e), "d3d9_lit(%s)", a); break;
        case D3DSIO_DSX:  snprintf(e, sizeof(e), "dfdx(%s)", a); break;
        case D3DSIO_DSY:  snprintf(e, sizeof(e), "dfdy(%s)", a); break;
        default: break;
        }
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    case D3DSIO_SINCOS: {
        /* SM 2.0/2.1 carry two extra scratch operands; SM 3.0 does not.
         * In both cases dst.x receives cos(src0.x) and dst.y sin(src0.x). */
        char scratch[128];
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        for (uint32_t k = 1; k < info->nsrc; k++)
            if (src_expr(c, &s[k], scratch, sizeof(scratch)) != 0) return -1;
        if (d[0].mask & ~3u) {
            fail(c, "token %u: sincos write mask must be .x, .y or .xy", idx);
            return -1;
        }
        snprintf(e, sizeof(e), "float4(cos((%s).x), sin((%s).x), 0.0f, 0.0f)", a, a);
        operand_t dd = d[0];
        dd.mask &= 3u;
        if (assign(c, t, &dd, e) != 0) return -1;
        break;
    }
    case D3DSIO_SETP: {
        uint32_t mode = (tok >> D3DSHADER_COMPARISON_SHIFT) & 0xFFu;
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        if (src_expr(c, &s[1], b, sizeof(b)) != 0) return -1;
        if (compare_expr(mode, a, b, cmpe, sizeof(cmpe)) != 0) {
            fail(c, "token %u: unknown comparison mode %u", idx, mode);
            return -1;
        }
        snprintf(e, sizeof(e), "bool4(%s)", cmpe);
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    }
    case D3DSIO_M4x4:
    case D3DSIO_M4x3:
    case D3DSIO_M3x4:
    case D3DSIO_M3x3:
    case D3DSIO_M3x2: {
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        if (check_reg(c, &s[1], "matrix") != 0) return -1;
        if (s[1].type != D3DSPR_CONST && s[1].type != D3DSPR_CONST2 &&
            s[1].type != D3DSPR_CONST3 && s[1].type != D3DSPR_CONST4) {
            fail(c, "token %u: matrix operand must be a constant register", idx);
            return -1;
        }
        if (s[1].rel || (c->def_f_set & (1u << s[1].num))) {
            fail(c, "token %u: relative or def-backed matrix operands are not supported", idx);
            return -1;
        }
        unsigned mbase = s[1].num;
        if (s[1].type == D3DSPR_CONST2) mbase += 256;
        else if (s[1].type == D3DSPR_CONST3) mbase += 512;
        else if (s[1].type == D3DSPR_CONST4) mbase += 768;
        const char *fn = op == D3DSIO_M4x4 ? "d3d9_mat4x4" :
                         op == D3DSIO_M4x3 ? "d3d9_mat4x3" :
                         op == D3DSIO_M3x4 ? "d3d9_mat3x4" :
                         op == D3DSIO_M3x3 ? "d3d9_mat3x3" : "d3d9_mat3x2";
        snprintf(e, sizeof(e), "%s(%s, c + %u)", fn, a, mbase);
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    }
    case D3DSIO_TEXCOORD: {
        operand_t uvop = info->nsrc ? s[0] : dest_as_source(&d[0]);
        if (src_expr(c, &uvop, a, sizeof(a)) != 0) return -1;
        if (assign(c, t, &d[0], a) != 0) return -1;
        break;
    }
    case D3DSIO_TEXKILL:
        if (!c->ps) {
            fail(c, "token %u: texkill is only valid in pixel shaders", idx);
            return -1;
        }
        /* TEXKILL has one destination-style operand; its write mask selects
         * the components tested for negative values. */
        if (dst_expr(c, &d[0], name, sizeof(name)) != 0) return -1;
        {
            char cond[512];
            if (!d[0].mask) { fail(c, "token %u: texkill has empty mask", idx); return -1; }
            cond[0] = '\0';
            int first = 1;
            for (unsigned i = 0; i < 4; i++) {
                if (!(d[0].mask & (1u << i))) continue;
                snprintf(cond + strlen(cond), sizeof(cond) - strlen(cond), "%s(%s).%c < 0.0f",
                         first ? "" : " || ", name, swz_char[i]);
                first = 0;
            }
            stmts(t, "    if (%s) discard_fragment();\n", cond);
        }
        break;
    case D3DSIO_TEX:
    case D3DSIO_TEXBEM:
    case D3DSIO_TEXBEML:
    case D3DSIO_TEXREG2AR:
    case D3DSIO_TEXREG2GB:
    case D3DSIO_TEXREG2RGB:
    case D3DSIO_TEXDP3:
    case D3DSIO_TEXDP3TEX:
    case D3DSIO_TEXM3x2TEX:
    case D3DSIO_TEXM3x3TEX:
    case D3DSIO_TEXM3x3DIFF:
    case D3DSIO_TEXM3x3SPEC:
    case D3DSIO_TEXM3x3VSPEC:
    case D3DSIO_TEXM3x3: {
        operand_t uvop = info->nsrc ? s[0] : dest_as_source(&d[0]);
        if (src_expr(c, &uvop, a, sizeof(a)) != 0) return -1;
        uint32_t sampler;
        if (texture_sampler(c, info, &d[0], s, &sampler) != 0) return -1;
        if (sampler_expr(c, sampler, a, tok & (D3DSI_TEXLD_PROJECT | D3DSI_TEXLD_BIAS),
                         e, sizeof(e)) != 0) return -1;
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    }
    case D3DSIO_TEXLDL:
    case D3DSIO_TEXLDD: {
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        uint32_t sampler;
        if (texture_sampler(c, info, &d[0], s, &sampler) != 0) return -1;
        if (sampler_expr(c, sampler, a, tok & (D3DSI_TEXLD_PROJECT | D3DSI_TEXLD_BIAS),
                         e, sizeof(e)) != 0) return -1;
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    }
    case D3DSIO_TEXM3x2PAD:
    case D3DSIO_TEXM3x3PAD:
    case D3DSIO_TEXM3x2DEPTH:
        /* These only set up the next texture stage; nothing to compute in MSL. */
        break;
    case D3DSIO_TEXDEPTH:
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        snprintf(e, sizeof(e), "float4((%s).z / max((%s).w, 1e-20f), 0.0f, 0.0f, 1.0f)", a, a);
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    case D3DSIO_BEM:
        if (src_expr(c, &s[0], a, sizeof(a)) != 0) return -1;
        snprintf(e, sizeof(e), "float4((%s).xy * 2.0f - 1.0f, 0.0f, 1.0f)", a);
        if (assign(c, t, &d[0], e) != 0) return -1;
        break;
    default:
        fail(c, "token %u: opcode 0x%04X has no emitter", idx, op);
        return -1;
    }
    return 0;
}

/* Emit the body of one function (main or a subroutine). */
static int emit_range(ctx_t *c, const uint32_t *tokens, uint32_t start, uint32_t end,
                      int in_sub) {
    for (uint32_t i = start; i < end; ) {
        c->cur_token = i;
        uint32_t tok = tokens[i];
        uint32_t op = tok & 0xFFFFu;
        if (op == D3DSIO_END || op == D3DSIO_LABEL) break;
        if (op == D3DSIO_COMMENT) { i += 1 + ((tok >> 16u) & 0x7FFFu); continue; }
        opinfo_t info = effective_op_info(c, (uint16_t)op);
        int predicated = (tok & D3DSHADER_INSTRUCTION_PREDICATED) ? 1 : 0;
        uint32_t count = operand_count(c, tokens, end, i, &info, predicated);
        if (count == UINT32_MAX) { fail(c, "token %u: instruction overruns bytecode", i); return -1; }

        stmtbuf_t t;
        memset(&t, 0, sizeof(t));
        if (predicated) {
            uint32_t pidx = predicate_token_index(c, tokens, i, &info);
            operand_t pred = parse_operand(tokens[pidx], 0);
            if (pred.type != D3DSPR_PREDICATE) {
                fail(c, "token %u: predicate operand is not a predicate register", i);
                return -1;
            }
            char cond[128];
            if (src_expr(c, &pred, cond, sizeof(cond)) != 0) return -1;
            stmts(&t, "    if (bool((%s).x)) {\n", cond);
        }
        if (emit_one(c, tokens, i, &t, in_sub) != 0) return -1;
        if (predicated) stmts(&t, "    }\n");
        sb(&c->out, "%s", t.tmp);
        if (c->out.overflow) return -1;
        i += 1 + count;
    }
    return 0;
}

/* Emit a subroutine definition for the label at token index idx. */
static int emit_sub(ctx_t *c, const uint32_t *tokens, uint32_t token_count, uint32_t idx) {
    uint32_t tok = tokens[idx];
    operand_t l = parse_operand(tokens[idx + 1], 0);
    uint32_t label = l.num;
    opinfo_t lab_info = effective_op_info(c, (uint16_t)(tok & 0xFFFFu));
    int lab_pred = (tok & D3DSHADER_INSTRUCTION_PREDICATED) ? 1 : 0;
    uint32_t lab_count = operand_count(c, tokens, token_count, idx, &lab_info, lab_pred);
    if (lab_count == UINT32_MAX) { fail(c, "token %u: label overruns bytecode", idx); return -1; }
    uint32_t start = idx + 1 + lab_count;
    uint32_t end = token_count;
    /* Find the next LABEL or END. */
    for (uint32_t i = start; i < token_count; ) {
        uint32_t t = tokens[i];
        uint32_t op = t & 0xFFFFu;
        if (op == D3DSIO_END || op == D3DSIO_LABEL) { end = i; break; }
        if (op == D3DSIO_COMMENT) { i += 1 + ((t >> 16u) & 0x7FFFu); continue; }
        opinfo_t inf = effective_op_info(c, (uint16_t)op);
        int predicated = (t & D3DSHADER_INSTRUCTION_PREDICATED) ? 1 : 0;
        uint32_t count = operand_count(c, tokens, token_count, i, &inf, predicated);
        if (count == UINT32_MAX) { fail(c, "token %u: instruction overruns bytecode", i); return -1; }
        i += 1 + count;
    }
    sb(&c->out, "static void d3d9_label%u(thread D3D9REGS &s, constant float4 *c,\n"
                "                        constant int4 *ci, constant bool4 *cb", label);
    for (unsigned k = 0; k < 16; k++) {
        if (c->samplers_mask & (1u << k))
            sb(&c->out, ",\n                        %s tex%u, sampler samp%u",
               tex_type(c->sampler_kind[k]), k, k);
    }
    sb(&c->out, ") {\n");
    if (emit_range(c, tokens, start, end, 1) != 0) return -1;
    sb(&c->out, "}\n\n");
    return 0;
}

/* ---- Public entry point -------------------------------------------------- */
int d3d9_translate_shader_to_msl(const uint32_t *tokens,
                                 uint32_t token_count,
                                 int is_pixel_shader,
                                 char *out_msl,
                                 size_t out_max,
                                 char *err_msg,
                                 size_t err_max) {
    if (!tokens || token_count < 2 || !out_msl || out_max < 256) {
        if (out_msl && out_max) out_msl[0] = '\0';
        if (err_msg && err_max)
            snprintf(err_msg, err_max, "Invalid parameters to d3d9_translate_shader_to_msl");
        return -1;
    }
    if (err_msg && err_max) err_msg[0] = '\0';

    uint32_t ver_type = tokens[0] >> 16u;
    unsigned maj = (tokens[0] >> 8u) & 0xFFu;
    unsigned min = tokens[0] & 0xFFu;
    if ((ver_type != 0xFFFEu && ver_type != 0xFFFFu) ||
        maj < 1 || maj > 3 ||
        (maj == 1 && min < 1) || (maj == 1 && min > 4) ||
        (maj > 1 && min > 1) ||
        (ver_type == 0xFFFFu ? 1 : 0) != (is_pixel_shader ? 1 : 0)) {
        out_msl[0] = '\0';
        if (err_msg && err_max)
            snprintf(err_msg, err_max, "Unsupported shader version 0x%08X (is_pixel_shader=%d)",
                     tokens[0], is_pixel_shader);
        return -1;
    }

    ctx_t c;
    memset(&c, 0, sizeof(c));
    c.out.buf = out_msl;
    c.out.cap = out_max;
    c.ps = is_pixel_shader ? 1 : 0;
    c.major = maj;
    c.minor = min;

    if (first_pass(&c, tokens, token_count) != 0) goto fail;
    emit_preamble(&c);
    emit_sub_prototypes(&c);

    /* Function signatures for main and for each subroutine. */
    sb(&c.out, "%s %s_main(%s_IN in [[stage_in]],\n"
               "          constant float4 *c [[buffer(1)]],\n"
               "          constant int4 *ci [[buffer(2)]],\n"
               "          constant bool4 *cb [[buffer(3)]],\n"
"          constant D3D9Clip& clip [[buffer(4)]]",
       c.ps ? "fragment PS_OUT" : "vertex VS_OUT", c.ps ? "ps" : "vs", c.ps ? "PS" : "VS");
    for (unsigned k = 0; k < 16; k++) {
        if (c.samplers_mask & (1u << k))
            sb(&c.out, ",\n          %s tex%u [[texture(%u)]],\n"
                       "          sampler samp%u [[sampler(%u)]]",
               tex_type(c.sampler_kind[k]), k, k, k, k);
    }
    sb(&c.out, ") {\n");
    if (c.ps) sb(&c.out, "    (void)clip;\n");
    sb(&c.out, "    D3D9REGS s;\n"
               "    for (int i = 0; i < 16; ++i) {\n"
               "        s.r[i] = float4(0.0f); s.o[i] = float4(0.0f);\n"
               "        s.aL[i] = int4(0); s.ci[i] = int4(0); s.cb[i] = bool4(false);\n"
               "    }\n");
    for (int i = 0; i < 16; i++)
        sb(&c.out, "    s.v[%d] = in.v%d;\n", i, i);
    /* t# is the pixel shader's texture-coordinate input: t0 is TEXCOORD0.
     * It is a real float4, not a cast of the vertex shader's integer a0. */
    for (int i = 0; i < 8; i++) {
        if (c.ps) sb(&c.out, "    s.t[%d] = in.uv%d;\n", i, i);
        else      sb(&c.out, "    s.t[%d] = float4(0.0f);\n", i);
    }
    sb(&c.out, "    for (int i = 0; i < 8; ++i) s.oT[i] = float4(0.0f);\n"
               "    for (int i = 0; i < 4; ++i) s.oC[i] = float4(0.0f);\n"
               "    for (int i = 0; i < 2; ++i) s.oD[i] = float4(0.0f);\n"
               "    for (int i = 0; i < 4; ++i) s.p[i] = bool4(false);\n"
               "    s.oPos = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
               "    s.oFog = float4(0.0f); s.oPts = float4(0.0f); s.oDepth = float4(0.0f);\n"
               "    s.a0 = int4(0); s.vFace = float4(0.0f);\n"
               "    s.vPos = %s;\n", c.ps ? "in.pos" : "float4(0.0f)");

    /* main(): all instructions up to the first LABEL (or END). */
    uint32_t main_end = token_count;
    for (uint32_t i = 1; i < token_count; ) {
        uint32_t t = tokens[i];
        uint32_t op = t & 0xFFFFu;
        if (op == D3DSIO_LABEL || op == D3DSIO_END) { main_end = i; break; }
        if (op == D3DSIO_COMMENT) { i += 1 + ((t >> 16u) & 0x7FFFu); continue; }
        opinfo_t inf = effective_op_info(&c, (uint16_t)op);
        int predicated = (t & D3DSHADER_INSTRUCTION_PREDICATED) ? 1 : 0;
        uint32_t count = operand_count(&c, tokens, token_count, i, &inf, predicated);
        if (count == UINT32_MAX) { c.failed = 1; snprintf(c.err, sizeof(c.err), "token %u: instruction overruns bytecode", i); goto fail; }
        i += 1 + count;
    }
    if (emit_range(&c, tokens, 1, main_end, 0) != 0) goto fail;
    sb(&c.out, c.ps ? "    return d3d9_pack(s);\n}\n\n" : "    return d3d9_pack(s, clip);\n}\n\n");

    /* Subroutine definitions (after main so forward calls see prototypes). */
    for (uint32_t i = 1; i < token_count; ) {
        uint32_t t = tokens[i];
        uint32_t op = t & 0xFFFFu;
        if (op == D3DSIO_END) break;
        if (op == D3DSIO_LABEL) {
            if (emit_sub(&c, tokens, token_count, i) != 0) goto fail;
        }
        if (op == D3DSIO_COMMENT) { i += 1 + ((t >> 16u) & 0x7FFFu); continue; }
        opinfo_t inf = effective_op_info(&c, (uint16_t)op);
        int predicated = (t & D3DSHADER_INSTRUCTION_PREDICATED) ? 1 : 0;
        uint32_t count = operand_count(&c, tokens, token_count, i, &inf, predicated);
        if (count == UINT32_MAX) { c.failed = 1; snprintf(c.err, sizeof(c.err), "token %u: instruction overruns bytecode", i); goto fail; }
        i += 1 + count;
    }

    if (c.out.overflow || c.failed) goto fail;
    out_msl[c.out.len] = '\0';
    return 0;

fail:
    out_msl[0] = '\0';
    if (err_msg && err_max) {
        if (c.failed) snprintf(err_msg, err_max, "D3D9 shader translation: %s", c.err);
        else snprintf(err_msg, err_max, "D3D9 shader translation: output buffer too small");
    }
    return -1;
}
