/*
 * d3dx9_asm.c - D3DXAssembleShader for D3D9 shader assembly.
 *
 * This is a self-contained assembler for the public vs_1_1 .. vs_3_0 and
 * ps_1_1 .. ps_3_0 assembly languages.  Token layout, opcode numbers,
 * register types and modifiers come solely from Microsoft's public
 * d3d9types.h / d3dx9shader.h (mirrored in Wine's include/ directory);
 * no assembler library, no Wine code and no Microsoft binary is used.
 *
 * The emitted DWORD stream is exactly what d3d9_shader.c translates:
 * version token, instruction tokens with the length in bits 24..27,
 * operand tokens and a final D3DSIO_END (0x0000FFFF).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "win32hle.h"
#include "d3dx9_buffer.h"
#include "d3dx9_asm.h"

#define D3DVS_VERSION(m, n) (0xFFFE0000u | ((uint32_t)(m) << 8u) | (uint32_t)(n))
#define D3DPS_VERSION(m, n) (0xFFFF0000u | ((uint32_t)(m) << 8u) | (uint32_t)(n))

/* Exact values from the public D3DSHADER_INSTRUCTION_OPCODE_TYPE enum. */
#define SIO_NOP          0x00u
#define SIO_MOV          0x01u
#define SIO_ADD          0x02u
#define SIO_SUB          0x03u
#define SIO_MAD          0x04u
#define SIO_MUL          0x05u
#define SIO_RCP          0x06u
#define SIO_RSQ          0x07u
#define SIO_DP3          0x08u
#define SIO_DP4          0x09u
#define SIO_MIN          0x0Au
#define SIO_MAX          0x0Bu
#define SIO_SLT          0x0Cu
#define SIO_SGE          0x0Du
#define SIO_EXP          0x0Eu
#define SIO_LOG          0x0Fu
#define SIO_LIT          0x10u
#define SIO_DST          0x11u
#define SIO_LRP          0x12u
#define SIO_FRC          0x13u
#define SIO_M4x4         0x14u
#define SIO_M4x3         0x15u
#define SIO_M3x4         0x16u
#define SIO_M3x3         0x17u
#define SIO_M3x2         0x18u
#define SIO_CALL         0x19u
#define SIO_CALLNZ       0x1Au
#define SIO_LOOP         0x1Bu
#define SIO_RET          0x1Cu
#define SIO_ENDLOOP      0x1Du
#define SIO_LABEL        0x1Eu
#define SIO_DCL          0x1Fu
#define SIO_POW          0x20u
#define SIO_CRS          0x21u
#define SIO_SGN          0x22u
#define SIO_ABS          0x23u
#define SIO_NRM          0x24u
#define SIO_SINCOS       0x25u
#define SIO_REP          0x26u
#define SIO_ENDREP       0x27u
#define SIO_IF           0x28u
#define SIO_IFC          0x29u
#define SIO_ELSE         0x2Au
#define SIO_ENDIF        0x2Bu
#define SIO_BREAK        0x2Cu
#define SIO_BREAKC       0x2Du
#define SIO_MOVA         0x2Eu
#define SIO_DEFB         0x2Fu
#define SIO_DEFI         0x30u
#define SIO_TEXCOORD     0x40u
#define SIO_TEXKILL      0x41u
#define SIO_TEX          0x42u
#define SIO_TEXBEM       0x43u
#define SIO_TEXBEML      0x44u
#define SIO_TEXREG2AR    0x45u
#define SIO_TEXREG2GB    0x46u
#define SIO_TEXM3x2PAD   0x47u
#define SIO_TEXM3x2TEX   0x48u
#define SIO_TEXM3x3PAD   0x49u
#define SIO_TEXM3x3TEX   0x4Au
#define SIO_TEXM3x3DIFF  0x4Bu
#define SIO_TEXM3x3SPEC  0x4Cu
#define SIO_TEXM3x3VSPEC 0x4Du
#define SIO_EXPP         0x4Eu
#define SIO_LOGP         0x4Fu
#define SIO_CND          0x50u
#define SIO_DEF          0x51u
#define SIO_TEXREG2RGB   0x52u
#define SIO_TEXDP3TEX    0x53u
#define SIO_TEXM3x2DEPTH 0x54u
#define SIO_TEXDP3       0x55u
#define SIO_TEXM3x3      0x56u
#define SIO_TEXDEPTH     0x57u
#define SIO_CMP          0x58u
#define SIO_BEM          0x59u
#define SIO_DP2ADD       0x5Au
#define SIO_DSX          0x5Bu
#define SIO_DSY          0x5Cu
#define SIO_TEXLDD       0x5Du
#define SIO_SETP         0x5Eu
#define SIO_TEXLDL       0x5Fu
#define SIO_BREAKP       0x60u
#define SIO_PHASE        0xFFFDu
#define SIO_END          0xFFFFu

/* Public D3D9 register types (d3d9types.h). */
enum {
    PR_TEMP = 0, PR_INPUT = 1, PR_CONST = 2, PR_ADDR = 3, PR_TEXTURE = 3,
    PR_RASTOUT = 4, PR_ATTROUT = 5, PR_TEXCRDOUT = 6, PR_CONSTINT = 7,
    PR_COLOROUT = 8, PR_DEPTHOUT = 9, PR_SAMPLER = 10,
    PR_CONSTBOOL = 14, PR_LOOP = 15, PR_MISCTYPE = 17,
    PR_LABEL = 18, PR_PREDICATE = 19
};

#define INSTLEN_SHIFT  24u
#define DEST_BIT       0x80000000u
#define WRITEMASK_SHIFT 16u
#define DSTMOD_SHIFT   20u
#define SRCMOD_SHIFT   24u
#define SWIZZLE_SHIFT  16u
#define NOSWIZZLE      0xE4u
#define REL_BIT        (1u << 13u)
#define PRED_BIT       0x10000000u
#define COMPARISON_SHIFT 16u
#define TEXLD_PROJECT  0x00010000u
#define TEXLD_BIAS     0x00020000u

typedef struct {
    uint32_t *w;
    uint32_t n, cap;
    int ps;
    unsigned maj, min;
    int line;
    char err[256];
    int saw_end;
} asm_t;

static int fail(asm_t *a, const char *fmt, ...) {
    if (!a->err[0]) {
        va_list ap;
        size_t n;
        snprintf(a->err, sizeof(a->err), "line %d: ", a->line);
        n = strlen(a->err);
        va_start(ap, fmt);
        vsnprintf(a->err + n, sizeof(a->err) - n, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int emit(asm_t *a, uint32_t t) {
    if (a->n == a->cap) {
        uint32_t nc = a->cap ? a->cap * 2u : 64u;
        uint32_t *nw = (uint32_t *)realloc(a->w, (size_t)nc * sizeof(uint32_t));
        if (!nw) return fail(a, "out of memory");
        a->w = nw; a->cap = nc;
    }
    a->w[a->n++] = t;
    return 0;
}

static uint32_t reg_token(uint32_t type, uint32_t num) {
    return (((type) & 7u) << 28u) | ((((type) >> 3u) & 3u) << 11u) | ((num) & 0x7FFu);
}

/* ---- opcode table: mnemonic -> public opcode and default arity ---------- */
typedef struct {
    const char *name;
    uint16_t op;
    uint8_t ndst, nsrc;
} optab_t;

static const optab_t kOps[] = {
    { "nop", SIO_NOP, 0, 0 },
    { "mov", SIO_MOV, 1, 1 },
    { "mova", SIO_MOVA, 1, 1 },
    { "add", SIO_ADD, 1, 2 },
    { "sub", SIO_SUB, 1, 2 },
    { "mad", SIO_MAD, 1, 3 },
    { "mul", SIO_MUL, 1, 2 },
    { "rcp", SIO_RCP, 1, 1 },
    { "rsq", SIO_RSQ, 1, 1 },
    { "dp3", SIO_DP3, 1, 2 },
    { "dp4", SIO_DP4, 1, 2 },
    { "min", SIO_MIN, 1, 2 },
    { "max", SIO_MAX, 1, 2 },
    { "slt", SIO_SLT, 1, 2 },
    { "sge", SIO_SGE, 1, 2 },
    { "abs", SIO_ABS, 1, 1 },
    { "exp", SIO_EXP, 1, 1 },
    { "log", SIO_LOG, 1, 1 },
    { "expp", SIO_EXPP, 1, 1 },
    { "logp", SIO_LOGP, 1, 1 },
    { "lit", SIO_LIT, 1, 1 },
    { "dst", SIO_DST, 1, 2 },
    { "lrp", SIO_LRP, 1, 3 },
    { "frc", SIO_FRC, 1, 1 },
    { "pow", SIO_POW, 1, 2 },
    { "crs", SIO_CRS, 1, 2 },
    { "sgn", SIO_SGN, 1, 3 },
    { "nrm", SIO_NRM, 1, 1 },
    { "sincos", SIO_SINCOS, 1, 3 },
    { "m4x4", SIO_M4x4, 1, 2 },
    { "m4x3", SIO_M4x3, 1, 2 },
    { "m3x4", SIO_M3x4, 1, 2 },
    { "m3x3", SIO_M3x3, 1, 2 },
    { "m3x2", SIO_M3x2, 1, 2 },
    { "call", SIO_CALL, 0, 1 },
    { "callnz", SIO_CALLNZ, 0, 2 },
    { "loop", SIO_LOOP, 0, 2 },
    { "ret", SIO_RET, 0, 0 },
    { "endloop", SIO_ENDLOOP, 0, 0 },
    { "label", SIO_LABEL, 0, 1 },
    { "rep", SIO_REP, 0, 1 },
    { "endrep", SIO_ENDREP, 0, 0 },
    { "if", SIO_IF, 0, 1 },
    { "ifc", SIO_IFC, 0, 2 },
    { "else", SIO_ELSE, 0, 0 },
    { "endif", SIO_ENDIF, 0, 0 },
    { "break", SIO_BREAK, 0, 0 },
    { "breakc", SIO_BREAKC, 0, 2 },
    { "breakp", SIO_BREAKP, 0, 1 },
    { "setp", SIO_SETP, 1, 2 },
    { "texcoord", SIO_TEXCOORD, 1, 0 },
    { "texkill", SIO_TEXKILL, 1, 0 },
    { "tex", SIO_TEX, 1, 0 },
    { "texbem", SIO_TEXBEM, 1, 1 },
    { "texbeml", SIO_TEXBEML, 1, 1 },
    { "texreg2ar", SIO_TEXREG2AR, 1, 1 },
    { "texreg2gb", SIO_TEXREG2GB, 1, 1 },
    { "texm3x2pad", SIO_TEXM3x2PAD, 1, 1 },
    { "texm3x2tex", SIO_TEXM3x2TEX, 1, 1 },
    { "texm3x3pad", SIO_TEXM3x3PAD, 1, 1 },
    { "texm3x3tex", SIO_TEXM3x3TEX, 1, 1 },
    { "texm3x3diff", SIO_TEXM3x3DIFF, 1, 1 },
    { "texm3x3spec", SIO_TEXM3x3SPEC, 1, 2 },
    { "texm3x3vspec", SIO_TEXM3x3VSPEC, 1, 1 },
    { "texreg2rgb", SIO_TEXREG2RGB, 1, 1 },
    { "texdp3tex", SIO_TEXDP3TEX, 1, 1 },
    { "texm3x2depth", SIO_TEXM3x2DEPTH, 1, 1 },
    { "texdp3", SIO_TEXDP3, 1, 1 },
    { "texm3x3", SIO_TEXM3x3, 1, 1 },
    { "texdepth", SIO_TEXDEPTH, 1, 0 },
    { "cmp", SIO_CMP, 1, 3 },
    { "bem", SIO_BEM, 1, 2 },
    { "dp2add", SIO_DP2ADD, 1, 3 },
    { "dsx", SIO_DSX, 1, 1 },
    { "dsy", SIO_DSY, 1, 1 },
    { "texldd", SIO_TEXLDD, 1, 4 },
    { "texldl", SIO_TEXLDL, 1, 2 },
    { "texldp", SIO_TEXLDL, 1, 2 },
    { "texldb", SIO_TEXLDL, 1, 2 },
    { "phase", SIO_PHASE, 0, 0 },
    { 0, 0, 0, 0 }
};

static const optab_t *find_op(const char *name) {
    for (const optab_t *o = kOps; o->name; o++)
        if (!strcmp(o->name, name)) return o;
    return NULL;
}

/* Version-dependent operand counts, matching Microsoft's public opcode
 * table as consumed by d3d9_shader.c.  ver is major*16+minor. */
static void arity(asm_t *a, uint16_t op, uint8_t *ndst, uint8_t *nsrc) {
    unsigned v = (a->maj << 4u) | a->min;
    switch (op) {
    case SIO_TEXCOORD: *ndst = 1; *nsrc = (v == 0x14u) ? 1 : 0; break;
    case SIO_TEX:      *ndst = 1; *nsrc = (v == 0x14u) ? 1 : (v >= 0x20u ? 2 : 0); break;
    case SIO_SGN:
    case SIO_SINCOS:    *nsrc = (v == 0x20u || v == 0x21u) ? 3 : 1; break;
    case SIO_CND:       *nsrc = 3; break;
    case SIO_CMP:       *nsrc = 3; break;
    default: break;
    }
}

/* ---- operand parsing ----------------------------------------------------- */
typedef struct { const char *p; } cur_t;

static void ws(cur_t *c) {
    while (*c->p == ' ' || *c->p == '\t' || *c->p == ',') c->p++;
}

static int ident(cur_t *c, char *buf, size_t n) {
    size_t k = 0;
    ws(c);
    if (!((*c->p >= 'A' && *c->p <= 'Z') || (*c->p >= 'a' && *c->p <= 'z') || *c->p == '_'))
        return -1;
    while (k + 1 < n && ((*c->p >= 'a' && *c->p <= 'z') ||
                         (*c->p >= 'A' && *c->p <= 'Z') ||
                         (*c->p >= '0' && *c->p <= '9') || *c->p == '_'))
        buf[k++] = *c->p++;
    buf[k] = '\0';
    return 0;
}

static int all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* Parse a register name into type/number.  Accepts the public D3D9 names. */
static int reg_ident(cur_t *c, char *buf, size_t n) {
    size_t k = 0;
    ws(c);
    if (!((*c->p >= 'A' && *c->p <= 'Z') || (*c->p >= 'a' && *c->p <= 'z')))
        return -1;
    while (k + 1 < n && ((*c->p >= 'a' && *c->p <= 'z') ||
                         (*c->p >= 'A' && *c->p <= 'Z') ||
                         (*c->p >= '0' && *c->p <= '9')))
        buf[k++] = *c->p++;
    buf[k] = 0;
    return 0;
}

static int reg(asm_t *a, cur_t *c, uint32_t *type, uint32_t *num) {
    char name[32];
    const char *start;
    int got_num = 0;

    ws(c);
    start = c->p;
    if (reg_ident(c, name, sizeof(name)) != 0) return fail(a, "expected register");

    /* Multi-letter fixed-function output/input aliases. */
    if (!strcmp(name, "oPos"))   { *type = PR_RASTOUT;  *num = 0; return 0; }
    if (!strcmp(name, "oFog"))   { *type = PR_RASTOUT;  *num = 1; return 0; }
    if (!strcmp(name, "oPts"))   { *type = PR_RASTOUT;  *num = 2; return 0; }
    if (!strcmp(name, "oD0"))    { *type = PR_ATTROUT;  *num = 0; return 0; }
    if (!strcmp(name, "oD1"))    { *type = PR_ATTROUT;  *num = 1; return 0; }
    if (!strcmp(name, "oDepth")) { *type = PR_DEPTHOUT; *num = 0; return 0; }
    if (!strcmp(name, "vPos"))   { *type = PR_MISCTYPE; *num = 0; return 0; }
    if (!strcmp(name, "vFace"))  { *type = PR_MISCTYPE; *num = 1; return 0; }
    if (!strcmp(name, "aL"))     { *type = PR_LOOP;     *num = 0; return 0; }

    /* Were digits part of the identifier? */
    { size_t k = 0; while (name[k] && !(name[k] >= '0' && name[k] <= '9')) k++;
      if (name[k]) {
          if (!all_digits(name + k)) return fail(a, "bad register number in %s", name);
          *num = (uint32_t)strtoul(name + k, NULL, 10);
          name[k] = '\0'; got_num = 1;
      }
    }
    if (!got_num) {
        /* A bare letter register with digits separated by [ for relative */
        if (c->p == start) return fail(a, "expected register");
    }

    switch (name[0]) {
    case 'r': *type = PR_TEMP; break;
    case 'v': *type = PR_INPUT; break;
    case 'c': *type = PR_CONST; break;
    case 'i': *type = PR_CONSTINT; break;
    case 'b': *type = PR_CONSTBOOL; break;
    case 's': *type = PR_SAMPLER; break;
    case 'l': *type = PR_LABEL; break;
    case 'p': *type = PR_PREDICATE; break;
    /* Type 3 is the integer address register a0 in a vertex shader and the
     * float texture coordinate t# in a pixel shader; the other use is not a
     * register in that stage and is rejected instead of silently encoded. */
    case 'a':
        if (a->ps) return fail(a, "a0 is not a pixel-shader register");
        *type = PR_ADDR; break;
    case 't':
        if (!a->ps) return fail(a, "t# is not a vertex-shader register");
        *type = PR_TEXTURE; break;
    case 'o':
        if (!strcmp(name, "oT")) *type = PR_TEXCRDOUT;
        else if (!strcmp(name, "oC")) *type = PR_COLOROUT;
        else return fail(a, "unknown output register %s", name);
        break;
    default: return fail(a, "unknown register %s", name);
    }
    return 0;
}

/* Optional relative addressing: [a0] or [a0+3].  Returns rel flag and
 * replaces num with the constant offset. */
static int relative(asm_t *a, cur_t *c, uint32_t *num) {
    ws(c);
    if (*c->p != '[') return 0;
    c->p++;
    ws(c);
    { char name[32];
      if (ident(c, name, sizeof(name)) != 0 || strcmp(name, "a0"))
          return fail(a, "relative addressing must use a0");
    }
    ws(c);
    if (*c->p == '+') {
        c->p++; ws(c);
        if (!(*c->p >= '0' && *c->p <= '9')) return fail(a, "expected offset after +");
        *num = (uint32_t)strtoul(c->p, (char **)&c->p, 10);
    }
    ws(c);
    if (*c->p != ']') return fail(a, "missing ] in relative addressing");
    c->p++;
    return 1;
}

/* Parse a swizzle .xyzw (up to four components, last one replicated). */
static int swizzle(asm_t *a, cur_t *c, uint32_t *out) {
    uint32_t s = 0;
    int comps[4] = { 0, 0, 0, 0 }, n = 0;
    ws(c);
    *out = NOSWIZZLE;
    if (*c->p != '.') return 0;
    c->p++;
    while (*c->p && n < 4) {
        int v;
        switch (*c->p) {
        case 'x': case 'r': v = 0; break;
        case 'y': case 'g': v = 1; break;
        case 'z': case 'b': v = 2; break;
        case 'w': case 'a': v = 3; break;
        default: return fail(a, "invalid swizzle component '%c'", *c->p);
        }
        comps[n++] = v; c->p++;
    }
    if (!n) return fail(a, "empty swizzle");
    for (int i = n; i < 4; i++) comps[i] = comps[n - 1];
    for (int i = 0; i < 4; i++) s |= (uint32_t)comps[i] << (2 * i);
    *out = s;
    return 0;
}

/* Parse a destination write mask .xyzw / .rgba.  Returns 0xF if absent. */
static uint32_t writemask(asm_t *a, cur_t *c) {
    uint32_t m = 0;
    int n = 0;
    ws(c);
    if (*c->p != '.') return 0xFu;
    const char *save = c->p;
    c->p++;
    while (*c->p && n < 4) {
        uint32_t b;
        switch (*c->p) {
        case 'x': case 'r': b = 1; break;
        case 'y': case 'g': b = 2; break;
        case 'z': case 'b': b = 4; break;
        case 'w': case 'a': b = 8; break;
        default: goto done;
        }
        m |= b; n++; c->p++;
    }
done:
    if (!n) { c->p = save; return 0xFu; }
    return m;
}

/* Source modifier suffixes from D3DSHADER_PARAM_SRCMOD_TYPE. */
static int srcmod_value(const char *s) {
    struct { const char *n; uint32_t v; } t[] = {
        { "bias",  2u }, { "biasneg", 3u }, { "sign", 4u },
        { "signneg", 5u }, { "comp", 6u }, { "x2", 7u },
        { "x2neg", 8u }, { "dz", 9u }, { "dw", 10u },
        { "abs", 11u }, { "absneg", 12u }, { "not", 13u }
    };
    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++)
        if (!strcmp(s, t[i].n)) return (int)t[i].v;
    return -1;
}

static int parse_dst(asm_t *a, cur_t *c, uint32_t *tok) {
    uint32_t type, num = 0, m, dmod = 0;
    if (reg(a, c, &type, &num) != 0) return -1;
    if (relative(a, c, &num)) return fail(a, "relative addressing is not valid on a destination");
    m = writemask(a, c);
    ws(c);
    if (*c->p == '_') {
        char name[24];
        const char *save = c->p;
        c->p++;
        if (ident(c, name, sizeof(name)) == 0) {
            if (!strcmp(name, "sat")) dmod = 1u;
            else if (!strcmp(name, "pp")) dmod = 2u;
            else if (!strcmp(name, "centroid")) dmod = 4u;
            else c->p = save;
        } else c->p = save;
    }
    *tok = DEST_BIT | reg_token(type, num) | (m << WRITEMASK_SHIFT) | (dmod << DSTMOD_SHIFT);
    return 0;
}

static int parse_src(asm_t *a, cur_t *c, uint32_t *tok) {
    uint32_t type, num = 0, sw, mod = 0;
    int neg = 0;
    ws(c);
    if (*c->p == '-') { neg = 1; c->p++; ws(c); }
    if (reg(a, c, &type, &num) != 0) return -1;
    int rel = relative(a, c, &num);
    if (swizzle(a, c, &sw) != 0) return -1;
    ws(c);
    if (*c->p == '_') {
        char name[24];
        c->p++;
        if (ident(c, name, sizeof(name)) == 0) {
            int mv = srcmod_value(name);
            if (mv >= 0) {
                mod = (uint32_t)mv;
                /* A leading '-' combined with a modifier has dedicated
                 * encodings in the public enum (neg = 1). */
                if (neg) {
                    if (mod == 2u) mod = 3u;       /* biasneg */
                    else if (mod == 4u) mod = 5u;  /* signneg */
                    else if (mod == 7u) mod = 8u;  /* x2neg */
                    else if (mod == 11u) mod = 12u;/* absneg */
                    else return fail(a, "modifier cannot be combined with -");
                    neg = 0;
                }
            } else return fail(a, "unknown source modifier _%s", name);
        } else return fail(a, "stray _ after source operand");
    }
    if (neg) mod = 1u; /* D3DSPSM_NEG */
    *tok = reg_token(type, num) | (sw << SWIZZLE_SHIFT) | (mod << SRCMOD_SHIFT);
    if (rel) *tok |= REL_BIT;
    return 0;
}

/* ---- declarations -------------------------------------------------------- */
typedef struct { const char *n; uint32_t v; } nmap_t;
static const nmap_t kUsage[] = {
    { "position", 0 }, { "blendweight", 1 }, { "blendindices", 2 },
    { "normal", 3 }, { "psize", 4 }, { "texcoord", 5 }, { "tangent", 6 },
    { "binormal", 7 }, { "tessfactor", 8 }, { "positiont", 9 },
    { "color", 10 }, { "fog", 11 }, { "depth", 12 }, { "sample", 13 }
};
static const nmap_t kSampler[] = {
    { "1d", 1 }, { "2d", 2 }, { "cube", 3 }, { "volume", 4 }
};

static int parse_dcl(asm_t *a, cur_t *c, const char *mnemonic) {
    uint32_t type, num = 0, usage = 0, regt;
    const char *p = mnemonic + 3; /* after "dcl" */
    if (*p == '_') {
        char word[40];
        size_t k = 0;
        p++;
        while (*p && k + 1 < sizeof(word) && *p != '.') word[k++] = *p++;
        word[k] = '\0';
        unsigned i;
        int found = 0;
        /* Sampler texture types start with a digit ("2d", "1d") and have
         * no usage index, so they are matched before splitting digits. */
        for (i = 0; i < sizeof(kSampler)/sizeof(kSampler[0]); i++)
            if (!strcmp(word, kSampler[i].n)) {
                usage = kSampler[i].v << 27u; found = 1; break;
            }
        if (!found) {
            size_t d = 0;
            char base[40];
            while (word[d] && !(word[d] >= '0' && word[d] <= '9')) d++;
            memcpy(base, word, d); base[d] = '\0';
            uint32_t idx = word[d] ? (uint32_t)strtoul(word + d, NULL, 10) : 0;
            for (i = 0; i < sizeof(kUsage)/sizeof(kUsage[0]); i++)
                if (!strcmp(base, kUsage[i].n)) {
                    usage = kUsage[i].v | (idx << 16u); found = 1; break;
                }
            if (!found) return fail(a, "unknown dcl type %s", word);
        }
    }
    /* The declaration's register is a plain register token (no dest bit). */
    if (reg(a, c, &type, &num) != 0) return -1;
    regt = reg_token(type, num);
    if (emit(a, SIO_DCL | (2u << INSTLEN_SHIFT)) != 0) return -1;
    if (emit(a, usage) != 0) return -1;
    if (emit(a, regt) != 0) return -1;
    return 0;
}

static int parse_def(asm_t *a, cur_t *c, uint16_t op) {
    uint32_t dtok, val;
    int nvals = (op == SIO_DEF) ? 4 : (op == SIO_DEFB ? 1 : 4);
    if (parse_dst(a, c, &dtok) != 0) return -1;
    /* parse_dst sets bit 31; DEF keeps it, matching the public layout. */
    if (emit(a, (uint32_t)op | ((uint32_t)(1 + nvals) << INSTLEN_SHIFT)) != 0) return -1;
    if (emit(a, dtok) != 0) return -1;
    for (int i = 0; i < nvals; i++) {
        ws(c);
        if (!*c->p) return fail(a, "too few values in def");
        if (op == SIO_DEF) {
            float f = strtof(c->p, (char **)&c->p);
            memcpy(&val, &f, 4);
        } else {
            char *end;
            long v = strtol(c->p, &end, 0);
            if (end == c->p) return fail(a, "bad integer in defi/defb");
            c->p = end;
            val = (uint32_t)v;
        }
        if (emit(a, val) != 0) return -1;
    }
    return 0;
}

/* ---- one source line ----------------------------------------------------- */
static int assemble_line(asm_t *a, cur_t *c) {
    char mnem[40];
    const optab_t *o;
    uint32_t inst, tmp;
    uint8_t ndst, nsrc;
    uint32_t ops[8];
    int nops = 0;

    ws(c);
    if (!*c->p) return 0;

    if (ident(c, mnem, sizeof(mnem)) != 0) return fail(a, "expected instruction");

    /* end is optional but may appear once. */
    if (!strcmp(mnem, "end")) {
        ws(c);
        if (*c->p) return fail(a, "trailing text after end");
        a->saw_end = 1;
        return 0;
    }
    if (!strncmp(mnem, "dcl", 3)) {
        if (parse_dcl(a, c, mnem) != 0) return -1;
        return 0;
    }
    if (!strcmp(mnem, "def") || !strcmp(mnem, "defi") || !strcmp(mnem, "defb")) {
        uint16_t op = !strcmp(mnem, "def") ? SIO_DEF :
                      (!strcmp(mnem, "defi") ? SIO_DEFI : SIO_DEFB);
        if (parse_def(a, c, op) != 0) return -1;
        return 0;
    }

    o = find_op(mnem);
    if (!o) {
        /* ifc_ge / breakc_lt are the base opcode plus a comparison suffix. */
        const char *u = strchr(mnem, '_');
        if (u) {
            char base[40];
            size_t bl = (size_t)(u - mnem);
            if (bl < sizeof(base)) {
                memcpy(base, mnem, bl);
                base[bl] = '\0';
                o = find_op(base);
            }
        }
    }
    if (!o) return fail(a, "unknown instruction %s", mnem);
    ndst = o->ndst; nsrc = o->nsrc;
    arity(a, o->op, &ndst, &nsrc);

    inst = (uint32_t)o->op;
    /* texldp / texldb use the public TEXLDL opcode and set control bits. */
    if (o->op == SIO_TEXLDL) {
        if (!strcmp(mnem, "texldp")) inst |= TEXLD_PROJECT;
        else if (!strcmp(mnem, "texldb")) inst |= TEXLD_BIAS;
    }
    /* Comparison-form control flow: if_gt, breakc_lt, ... */
    if (o->op == SIO_IFC || o->op == SIO_BREAKC) {
        static const nmap_t cmps[] = {
            { "gt", 1 }, { "eq", 2 }, { "ge", 3 }, { "lt", 4 },
            { "ne", 5 }, { "le", 6 }
        };
        const char *u = strchr(mnem, '_');
        unsigned i;
        int found = 0;
        if (!u) return fail(a, "%s needs a comparison suffix", mnem);
        for (i = 0; i < sizeof(cmps)/sizeof(cmps[0]); i++)
            if (!strcmp(u + 1, cmps[i].n)) {
                inst |= cmps[i].v << COMPARISON_SHIFT; found = 1; break;
            }
        if (!found) return fail(a, "unknown comparison %s", u + 1);
    }
    for (int i = 0; i < ndst; i++) {
        if (parse_dst(a, c, &tmp) != 0) return -1;
        ops[nops++] = tmp;
    }
    for (int i = 0; i < nsrc; i++) {
        if (parse_src(a, c, &tmp) != 0) return -1;
        ops[nops++] = tmp;
        if ((tmp & REL_BIT) && a->maj >= 2) {
            /* D3D9 SM2+ requires an extra address-register token following a relative operand.
             * Format: bit 31 set | reg_token(PR_ADDR, 0) | swizzle .x (swizzle 0 on ADDR register). */
            ops[nops++] = 0x80000000u | reg_token(PR_ADDR, 0);
        }
    }
    ws(c);
    if (*c->p && *c->p != ';' && *c->p != '/')
        return fail(a, "unexpected text at end of instruction: %s", c->p);

    inst |= (uint32_t)nops << INSTLEN_SHIFT;
    if (emit(a, inst) != 0) return -1;
    for (int i = 0; i < nops; i++)
        if (emit(a, ops[i]) != 0) return -1;
    return 0;
}

/* ---- version directive --------------------------------------------------- */
static int parse_version(asm_t *a, cur_t *c) {
    char name[32];
    int is_ps;
    unsigned maj, min;
    if (ident(c, name, sizeof(name)) != 0) return fail(a, "missing version directive");
    if (name[0] == 'v' && name[1] == 's') is_ps = 0;
    else if (name[0] == 'p' && name[1] == 's') is_ps = 1;
    else return fail(a, "expected vs_x_y or ps_x_y, got %s", name);
    if (name[2] != '_') return fail(a, "bad version %s", name);
    {
        const char *p = name + 3;
        if (!(*p >= '0' && *p <= '9')) return fail(a, "bad version %s", name);
        maj = (unsigned)strtoul(p, (char **)&p, 10);
        if (*p != '_') return fail(a, "bad version %s", name);
        p++;
        if (!(*p >= '0' && *p <= '9')) return fail(a, "bad version %s", name);
        min = (unsigned)strtoul(p, (char **)&p, 10);
        if (*p) return fail(a, "bad version %s", name);
    }
    /* The documented models are: vs_1_1, vs_2_0, vs_2_1, vs_3_0 and
     * ps_1_1..ps_1_4, ps_2_0, ps_2_1, ps_3_0. */
    if (!is_ps) {
        if (!((maj == 1 && min == 1) || (maj == 2 && (min == 0 || min == 1)) ||
              (maj == 3 && min == 0)))
            return fail(a, "unsupported shader model %s", name);
    } else {
        if (!((maj == 1 && min >= 1 && min <= 4) ||
              (maj == 2 && (min == 0 || min == 1)) ||
              (maj == 3 && min == 0)))
            return fail(a, "unsupported shader model %s", name);
    }
    a->ps = is_ps; a->maj = maj; a->min = min;
    return 0;
}

/* ---- top level ----------------------------------------------------------- */
int d3dx9_assemble(const char *src, uint32_t len,
                   uint32_t **out_words, uint32_t *out_count,
                   char *err, size_t err_max) {
    asm_t a;
    int rc = -1, saw_version = 0;
    const char *p, *end;
    uint32_t ver;

    if (out_words) *out_words = NULL;
    if (out_count) *out_count = 0;
    if (err && err_max) err[0] = '\0';
    if (!src || !out_words || !out_count) {
        if (err && err_max) snprintf(err, err_max, "line 0: invalid parameters");
        return -1;
    }
    memset(&a, 0, sizeof(a));
    end = src + (len ? (size_t)len : strlen(src));
    p = src;

    ver = 0;
    while (p <= end) {
        const char *line = p;
        const char *eol = p;
        const char *le = p;
        char buf[2048];
        size_t n = 0;
        cur_t c;
        while (eol < end && *eol != '\n' && *eol != '\r') eol++;
        le = eol;
        while (le < end && (*le == '\n' || *le == '\r')) le++;
        a.line++;
        while (line < le && (size_t)(le - line) > n && (*line == ' ' || *line == '\t')) line++;
        if (*line == '/' && *(line + 1) == '/') line = le;
        else if (*line == ';') line = le;
        else {
            const char *q = line;
            while (q < eol && n + 1 < sizeof(buf)) buf[n++] = *q++;
            buf[n] = '\0';
            /* strip trailing // or ; comment */
            for (size_t i = 0; i < n; i++) {
                if (buf[i] == ';' || (buf[i] == '/' && buf[i+1] == '/')) { buf[i] = '\0'; break; }
            }
            c.p = buf;
            ws(&c);
            if (*c.p) {
                if (!saw_version) {
                    if (parse_version(&a, &c) != 0) goto done;
                    saw_version = 1;
                    ver = a.ps ? D3DPS_VERSION(a.maj, a.min)
                               : D3DVS_VERSION(a.maj, a.min);
                    if (emit(&a, ver) != 0) goto done;
                } else {
                    if (assemble_line(&a, &c) != 0) goto done;
                }
            }
        }
        p = le;
        if (le >= end) break;
    }

    if (!saw_version) { fail(&a, "missing version directive"); goto done; }
    if (emit(&a, SIO_END) != 0) goto done;
    if (a.err[0]) goto done;

    *out_words = a.w;
    *out_count = a.n;
    a.w = NULL;
    rc = 0;

done:
    if (a.w) free(a.w);
    if (rc != 0 && err && err_max)
        snprintf(err, err_max, "%s", a.err[0] ? a.err : "line 0: assembly failed");
    return rc;
}

/* ================= HLE shim: ID3DXBuffer + D3DXAssembleShader ============= */
#define D3D_OK                  0x00000000u
#define D3DERR_INVALIDCALL      0x8876086Cu
#define E_FAIL                  0x80004005u
#define E_OUTOFMEMORY           0x8007000Eu

/* The standalone assembler robustness harness links d3dx9_asm.c without
 * d3dx9_buffer.c. This weak fallback gives it a real ID3DXBuffer; in the full
 * host the strong definition from d3dx9_buffer.c wins, so the same five shims
 * are not registered twice in the fixed 2048-entry registry. The five slots,
 * their order and the stdcall argc follow Wine's include/d3dx9core.h. */
typedef struct { uint32_t data_va, size; } dxbuf_state_t;
#define BUF(o) ((dxbuf_state_t *)((char *)hle_ptr(o) + 8u))

static void asmbuf_QueryInterface(void) {            /* (this, riid, &out) */
    static const uint8_t iid_unknown[16] = {
        0,0,0,0, 0,0,0,0, 0xC0,0,0,0, 0,0,0,0x46 };
    static const uint8_t iid_buffer[16] = {
        0x08,0xFB,0xA5,0x8B, 0x95,0x51, 0xE2,0x40, 0xAC,0x58, 0x0D,0x98,0x9C,0x3A,0x01,0x02 };
    uint32_t self = A32(0), riid = A32(1), ppv = A32(2);
    if (!ppv) RET(D3DERR_INVALIDCALL, 3);
    MEM32(ppv) = 0;
    const uint8_t *iid = riid ? (const uint8_t *)hle_ptr(riid) : NULL;
    if (!iid || (memcmp(iid, iid_unknown, 16) && memcmp(iid, iid_buffer, 16))) {
        fprintf(stderr, "[d3dx9] ID3DXBuffer (assembler): interface not supported\n");
        RET(0x80004002u, 3);                        /* E_NOINTERFACE */
    }
    MEM32(ppv) = self;
    MEM32(self + 4)++;
    RET(D3D_OK, 3);
}
static void asmbuf_AddRef(void) { RET(++MEM32(A32(0) + 4u), 1); }
static void asmbuf_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4u);
    if (!n) {
        dxbuf_state_t *s = BUF(self);
        if (s && s->data_va) hle_free(hle_ptr(s->data_va));
        hle_com_free(self);
    }
    RET(n, 1);
}
static void asmbuf_GetBufferPointer(void) {
    dxbuf_state_t *s = BUF(A32(0));
    RET(s ? s->data_va : 0u, 1);
}
static void asmbuf_GetBufferSize(void) {
    dxbuf_state_t *s = BUF(A32(0));
    RET(s ? s->size : 0u, 1);
}
static const win32hle_shim buf_methods[] = {
    { "ID3DXBuffer::QueryInterface",  asmbuf_QueryInterface },
    { "ID3DXBuffer::AddRef",          asmbuf_AddRef },
    { "ID3DXBuffer::Release",         asmbuf_Release },
    { "ID3DXBuffer::GetBufferPointer", asmbuf_GetBufferPointer },
    { "ID3DXBuffer::GetBufferSize",   asmbuf_GetBufferSize },
    { 0, 0 }
};
static uint32_t g_vt_buf;
__attribute__((weak)) uint32_t d3dx9_buffer_new(const void *data, size_t size) {
    void *g;
    uint32_t obj;
    if (!g_vt_buf) g_vt_buf = hle_com_vtable(buf_methods);
    g = hle_alloc((uint32_t)(size ? size : 1));
    if (!g) return 0;
    if (data && size) memcpy(g, data, size);
    obj = hle_com_new(g_vt_buf, 8u + sizeof(dxbuf_state_t));
    if (!obj) { hle_free(g); return 0; }
    BUF(obj)->data_va = hle_va(g);
    BUF(obj)->size = (uint32_t)size;
    return obj;
}

static uint32_t make_buf(const void *data, size_t size) {
    return d3dx9_buffer_new(data, size);
}

uint32_t d3dx9_asm_make_buffer(const void *data, size_t size) {
    return make_buf(data, size);
}

void s_D3DXAssembleShader(void) {
    /* (pSrcData, SrcDataLen, pDefines, pInclude, Flags, ppShader, ppErrorMsgs) */
    uint32_t src = A32(0), len = A32(1);
    uint32_t ppShader = A32(5), ppErr = A32(6);
    uint32_t *words = NULL, nwords = 0, hr = D3DERR_INVALIDCALL;
    char errmsg[256];

    if (ppShader) MEM32(ppShader) = 0;
    if (ppErr) MEM32(ppErr) = 0;
    if (src) {
        int rc = d3dx9_assemble((const char *)hle_ptr(src), len, &words, &nwords,
                                errmsg, sizeof(errmsg));
        if (rc != 0) {
            hr = E_FAIL;
            if (ppErr) {
                uint32_t eb = make_buf(errmsg, strlen(errmsg) + 1);
                if (eb) MEM32(ppErr) = eb; else hr = E_OUTOFMEMORY;
            }
        } else {
            hr = D3D_OK;
            if (ppShader) {
                uint32_t sb = make_buf(words, (size_t)nwords * 4u);
                if (sb) MEM32(ppShader) = sb; else hr = E_OUTOFMEMORY;
            }
            free(words);
            if (ppErr) MEM32(ppErr) = make_buf("", 1);
        }
    }
    RET(hr, 7);
}

