/*
 * d3dx9_shtools.c - D3DX9 shader tools not previously registered.
 *
 * Public sources (ABI, signatures and token layout only; no Microsoft DLL,
 * no foreign code): Wine's public include/d3dx9shader.h and d3dx9types.h,
 * and the documented D3D9 shader bytecode format in d3d9types.h
 * (D3DSHADER_INSTRUCTION_OPCODE_TYPE, operand tokens, D3DCAPS9 versions).
 *
 * Disassembly is the exact inverse of d3dx9_asm.c for the instruction subset
 * that assembler emits, so assemble -> disassemble -> assemble reproduces the
 * token stream. Anything not understood is an honest error with a log.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "win32hle.h"
#include "d3d9_hle.h"
#include "d3dx9_asm.h"
#include "d3dx9_shtools.h"

#define D3D_OK_          0x00000000u
#define D3DERR_INVALIDCALL_ 0x8876086Cu
#define E_FAIL_          0x80004005u
#define E_OUTOFMEMORY_   0x8007000Eu
#define D3DXERR_NOTAVAILABLE_ 0x88760866u

/* ---- token encoding (public d3d9types.h) ---- */
#define INSTLEN_SHIFT 24u
#define DEST_BIT      0x80000000u
#define WRITEMASK_SHIFT 16u
#define DSTMOD_SHIFT  20u
#define SRCMOD_SHIFT  24u
#define SWIZZLE_SHIFT 16u
#define SIO_DCL       0x1Fu
#define SIO_DEF       0x51u
#define SIO_END       0xFFFFu
#define SIO_COMMENT   0xFFFEu

typedef struct { const char *name; uint16_t op; } opname_t;
static const opname_t kOps[] = {
    { "nop",0x00 },{ "dcl",0x1F },{ "def",0x51 },{ "mov",0x01 },{ "mova",0x2E },{ "add",0x02 },{ "sub",0x03 },
    { "mad",0x04 },{ "mul",0x05 },{ "rcp",0x06 },{ "rsq",0x07 },{ "dp3",0x08 },
    { "dp4",0x09 },{ "min",0x0A },{ "max",0x0B },{ "slt",0x0C },{ "sge",0x0D },
    { "abs",0x23 },{ "exp",0x0E },{ "log",0x0F },{ "expp",0x4E },{ "logp",0x4F },
    { "lit",0x10 },{ "dst",0x11 },{ "lrp",0x12 },{ "frc",0x13 },{ "pow",0x20 },
    { "crs",0x21 },{ "sgn",0x22 },{ "nrm",0x24 },{ "sincos",0x25 },
    { "m4x4",0x14 },{ "m4x3",0x15 },{ "m3x4",0x16 },{ "m3x3",0x17 },{ "m3x2",0x18 },
    { "call",0x19 },{ "callnz",0x1A },{ "loop",0x1B },{ "ret",0x1C },{ "endloop",0x1D },
    { "label",0x1E },{ "rep",0x26 },{ "endrep",0x27 },{ "if",0x28 },{ "ifc",0x29 },
    { "else",0x2A },{ "endif",0x2B },{ "break",0x2C },{ "breakc",0x2D },
    { "breakp",0x60 },{ "setp",0x5E },{ "texcoord",0x40 },{ "texkill",0x41 },
    { "tex",0x42 },{ "texbem",0x43 },{ "texbeml",0x44 },{ "texreg2ar",0x45 },
    { "texreg2gb",0x46 },{ "texm3x2pad",0x47 },{ "texm3x2tex",0x48 },
    { "texm3x3pad",0x49 },{ "texm3x3tex",0x4A },{ "cmp",0x58 },{ "dp2add",0x5A },
    { "dsx",0x5B },{ "dsy",0x5C },{ "texldl",0x5F },{ "phase",0xFFFD },{ 0,0 }
};
static const char *op_name(uint16_t op) {
    for (const opname_t *o = kOps; o->name; o++) if (o->op == op) return o->name;
    return NULL;
}

static const char *const kUsage[] = {
    "position","blendweight","blendindices","normal","psize","texcoord",
    "tangent","binormal","tessfactor","positiont","color","fog","depth","sample"
};

typedef struct {
    char *p; size_t n, cap;
} strbuf_t;
static int sb_add(strbuf_t *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (b->n + (size_t)n + 1u > b->cap) {
        size_t nc = b->cap ? b->cap * 2u : 256u;
        while (nc < b->n + (size_t)n + 1u) nc *= 2u;
        char *q = (char *)realloc(b->p, nc);
        if (!q) return -1;
        b->p = q; b->cap = nc;
    }
    memcpy(b->p + b->n, tmp, (size_t)n + 1u);
    b->n += (size_t)n;
    return 0;
}

static uint32_t reg_type(uint32_t tok) {
    return ((tok >> 28u) & 7u) | (((tok >> 11u) & 3u) << 3u);
}
static uint32_t reg_num(uint32_t tok) { return tok & 0x7FFu; }

/* Emit a register name. `ps` selects pixel-shader register aliases. */
static int emit_reg(strbuf_t *b, uint32_t tok, int ps) {
    uint32_t t = reg_type(tok), n = reg_num(tok);
    switch (t) {
    case 0: return sb_add(b, "r%u", n);
    case 1: return sb_add(b, "v%u", n);
    case 2: return sb_add(b, "c%u", n);
    case 3: return sb_add(b, ps ? "t%u" : "a%u", n);
    case 4: if (n == 0) return sb_add(b, "oPos"); if (n == 1) return sb_add(b, "oFog"); if (n == 2) return sb_add(b, "oPts"); break;
    case 5: if (n == 0) return sb_add(b, "oD0"); if (n == 1) return sb_add(b, "oD1"); break;
    case 6: return sb_add(b, "oT%u", n);
    case 8: return sb_add(b, "oC%u", n);
    case 9: return sb_add(b, "oDepth");
    case 10: return sb_add(b, "s%u", n);
    case 14: return sb_add(b, "b%u", n);
    case 15: return sb_add(b, "aL");
    case 17: if (n == 0) return sb_add(b, "vPos"); if (n == 1) return sb_add(b, "vFace"); break;
    case 18: return sb_add(b, "l%u", n);
    default: break;
    }
    return -1;
}

static int emit_swizzle(strbuf_t *b, uint32_t sw, int is_dst) {
    if (is_dst) {
        uint32_t m = sw & 0xFu;
        if (m == 0xFu) return 0;
        const char c[4] = { 'x','y','z','w' };
        if (sb_add(b, ".") != 0) return -1;
        for (int i = 0; i < 4; i++) if (m & (1u << i)) if (sb_add(b, "%c", c[i]) != 0) return -1;
        return 0;
    }
    if (sw == 0xE4u) return 0;               /* .xyzw, the assembler's default */
    static const char cc[4] = { 'x','y','z','w' };
    if (sb_add(b, ".") != 0) return -1;
    for (int i = 0; i < 4; i++) {
        uint32_t k = (sw >> (2u * i)) & 3u;
        if (sb_add(b, "%c", cc[k]) != 0) return -1;
    }
    return 0;
}

int d3dx9_disassemble_text(const uint32_t *words, uint32_t count,
                           char **out, char *err, size_t err_max) {
    if (!words || count < 1 || !out) {
        if (err && err_max) snprintf(err, err_max, "invalid disassembly arguments");
        return -1;
    }
    uint32_t ver = words[0];
    int vs = (ver & 0xFFFF0000u) == 0xFFFE0000u;
    int ps = (ver & 0xFFFF0000u) == 0xFFFF0000u;
    if (!vs && !ps) { if (err && err_max) snprintf(err, err_max, "not a D3D9 shader"); return -1; }
    int major = (int)((ver >> 8u) & 0xFFu), minor = (int)(ver & 0xFFu);
    strbuf_t b = {0};
    if (sb_add(&b, "%s_%d_%d\n", ps ? "ps" : "vs", major, minor) != 0) goto fail;

    for (uint32_t i = 1; i < count; ) {
        uint32_t tok = words[i];
        uint32_t op = tok & 0xFFFFu;
        if (op == SIO_END) break;
        if (op == SIO_COMMENT) {               /* skip CTAB and other comments */
            uint32_t len = (tok >> 16u) & 0xFFFFu;
            i += 1u + len;
            continue;
        }
        uint32_t len = (tok >> INSTLEN_SHIFT) & 0xFu;
        if (len == 0 || i + len >= count) { if (err&&err_max) snprintf(err,err_max,"bad instruction length"); goto fail; }
        const char *name = op_name((uint16_t)op);
        if (!name) { if (err&&err_max) snprintf(err,err_max,"unknown opcode 0x%04X",op); goto fail; }

        if (op == SIO_DCL) {
            if (len != 2u) { if (err&&err_max) snprintf(err,err_max,"bad dcl length"); goto fail; }
            uint32_t usage = words[i+1u], regt = words[i+2u];
            if ((usage >> 27u) != 0u) { if (err&&err_max) snprintf(err,err_max,"sampler dcl unsupported"); goto fail; }
            uint32_t u = usage & 0xFFFFu, idx = (usage >> 16u) & 0xFFu;
            if (u >= sizeof kUsage / sizeof kUsage[0]) { if (err&&err_max) snprintf(err,err_max,"unknown dcl usage %u",u); goto fail; }
            if (sb_add(&b, "dcl_%s", kUsage[u]) != 0) goto fail;
            if (idx && sb_add(&b, "%u", idx) != 0) goto fail;
            if (sb_add(&b, " ") != 0 || emit_reg(&b, regt, ps) != 0 || sb_add(&b, "\n") != 0) goto fail;
        } else if (op == SIO_DEF) {
            uint32_t regt = words[i+1u];
            if (sb_add(&b, "def ") != 0 || emit_reg(&b, regt, ps) != 0) goto fail;
            for (uint32_t k = 0; k < 4u && i + 2u + k < count; k++) {
                float f; uint32_t bits = words[i+2u+k]; memcpy(&f, &bits, 4);
                if (sb_add(&b, "%s%g", k ? ", " : ", ", f) != 0) goto fail;
            }
            if (sb_add(&b, "\n") != 0) goto fail;
        } else {
            if (sb_add(&b, "%s", name) != 0) goto fail;
            uint32_t ni = len;
            for (uint32_t k = 0; k < ni; k++) {
                uint32_t otok = words[i+1u+k];
                int is_dst = (otok & DEST_BIT) != 0u;
                if (sb_add(&b, "%s", k ? ", " : " ") != 0) goto fail;
                if (!is_dst && ((otok >> SRCMOD_SHIFT) & 0xFFu) == 1u && sb_add(&b, "-") != 0) goto fail;
                if (emit_reg(&b, otok & ~DEST_BIT, ps) != 0) goto fail;
                if (emit_swizzle(&b, (otok >> SWIZZLE_SHIFT) & 0xFFu, is_dst) != 0) goto fail;
            }
            if (sb_add(&b, "\n") != 0) goto fail;
        }
        i += len + 1u;
    }
    if (sb_add(&b, "\n") != 0) goto fail;
    *out = b.p;
    return 0;
fail:
    free(b.p);
    if (err && err_max) snprintf(err, err_max, "disassembly failed");
    return -1;
}

/* ---- profile getters ---- */
uint32_t d3dx9_shader_profile(uint32_t dev, int pixel) {
    uint32_t out = 0;
    if (!dev) return 0;
    void *caps = hle_alloc(sizeof(D3DCAPS9));
    if (!caps) return 0;
    memset(caps, 0, sizeof(D3DCAPS9));
    uint32_t fn = hle_resolve("IDirect3DDevice9::GetDeviceCaps");
    uint32_t hr = fn ? hle_call_guest(fn, 2, (uint32_t[]){ dev, hle_va(caps) }) : 1u;
    if (hr == D3D_OK_) {
        D3DCAPS9 *c = (D3DCAPS9 *)caps;
        uint32_t v = pixel ? c->PixelShaderVersion : c->VertexShaderVersion;
        uint32_t maj = (v >> 8u) & 0xFFu, min = v & 0xFFu;
        if (maj == 3u && min == 0u) out = hle_guest_strdup(pixel ? "ps_3_0" : "vs_3_0");
        else if (maj == 2u && min == 0u) out = hle_guest_strdup(pixel ? "ps_2_0" : "vs_2_0");
        else if (maj == 1u && min == 1u) out = hle_guest_strdup(pixel ? "ps_1_1" : "vs_1_1");
    }
    hle_free(caps);
    return out;
}

/* ---- minimal public shader preprocessor ----
 * D3DXPreprocessShaderFromFileA expands object-like macros declared in the
 * source (#define/#undef) and honours #ifdef/#ifndef/#else/#endif. Other
 * directives (#include, #if, ...) are not supported and produce an honest
 * error, never a pass-through that would look like success. */
typedef struct { char name[64]; char value[256]; } shtool_macro_t;

static int ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static int macro_find(const shtool_macro_t *m, int n, const char *name, size_t len) {
    for (int i = 0; i < n; i++)
        if (strlen(m[i].name) == len && !memcmp(m[i].name, name, len)) return i;
    return -1;
}
static int append_expanded(strbuf_t *out, const char *line,
                           const shtool_macro_t *m, int nmacros) {
    for (const char *p = line; *p; ) {
        if (!ident_char(*p) || (*p >= '0' && *p <= '9')) {
            if (sb_add(out, "%c", *p++) != 0) return -1;
            continue;
        }
        const char *q = p;
        while (ident_char(*q)) q++;
        int k = macro_find(m, nmacros, p, (size_t)(q - p));
        if (k >= 0) {
            if (sb_add(out, "%s", m[k].value) != 0) return -1;
        } else {
            for (const char *r = p; r < q; r++) if (sb_add(out, "%c", *r) != 0) return -1;
        }
        p = q;
    }
    return 0;
}
static char *preprocess_text(const char *src, uint32_t len, char *err, size_t err_max) {
#define SHTOOL_MAX_MACROS 64
#define SHTOOL_MAX_COND   32
    shtool_macro_t macros[SHTOOL_MAX_MACROS];
    int nmacros = 0;
    struct { int parent, taken; } cond[SHTOOL_MAX_COND];
    int ncond = 0, active = 1;
    strbuf_t out = {0};
    const char *p = src, *end = src + (len ? (size_t)len : strlen(src));
    int line = 0;
#define PPERR(fmt, ...) do { if (err && err_max) \
        snprintf(err, err_max, "line %d: " fmt, line, __VA_ARGS__); \
        free(out.p); return NULL; } while (0)
    while (p < end) {
        const char *e = p;
        while (e < end && *e != '\n' && *e != '\r') e++;
        char buf[2048];
        size_t n = (size_t)(e - p);
        line++;
        if (n >= sizeof buf) PPERR("source line too long", "");
        memcpy(buf, p, n); buf[n] = 0;
        while (e < end && (*e == '\n' || *e == '\r')) e++;
        p = e;

        const char *s = buf;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#') {
            const char *d = s + 1;
            while (*d == ' ' || *d == '\t') d++;
            size_t dl = 0;
            while (ident_char(d[dl])) dl++;
            if (dl == 5 && !memcmp(d, "ifdef", 5)) {
                const char *name = d + 6;
                while (*name == ' ' || *name == '\t') name++;
                const char *ne = name;
                while (ident_char(*ne)) ne++;
                if (ne == name) PPERR("#ifdef needs a name", "");
                if (ncond >= SHTOOL_MAX_COND) PPERR("conditional nesting too deep", "");
                int taken = macro_find(macros, nmacros, name, (size_t)(ne - name)) >= 0;
                cond[ncond].parent = active; cond[ncond].taken = taken; ncond++;
                active = active && taken;
            } else if (dl == 6 && !memcmp(d, "ifndef", 6)) {
                const char *name = d + 7;
                while (*name == ' ' || *name == '\t') name++;
                const char *ne = name;
                while (ident_char(*ne)) ne++;
                if (ne == name) PPERR("#ifndef needs a name", "");
                if (ncond >= SHTOOL_MAX_COND) PPERR("conditional nesting too deep", "");
                int taken = macro_find(macros, nmacros, name, (size_t)(ne - name)) < 0;
                cond[ncond].parent = active; cond[ncond].taken = taken; ncond++;
                active = active && taken;
            } else if (dl == 4 && !memcmp(d, "else", 4)) {
                if (!ncond) PPERR("#else without #ifdef/#ifndef", "");
                active = cond[ncond - 1].parent && !cond[ncond - 1].taken;
            } else if (dl == 5 && !memcmp(d, "endif", 5)) {
                if (!ncond) PPERR("#endif without #ifdef/#ifndef", "");
                ncond--;
                active = cond[ncond].parent;
            } else if (!active) {
                /* Directives in a skipped branch are ignored. */
            } else if (dl == 6 && !memcmp(d, "define", 6)) {
                const char *name = d + 7;
                while (*name == ' ' || *name == '\t') name++;
                const char *ne = name;
                while (ident_char(*ne) && !(*ne >= '0' && *ne <= '9' && ne == name)) ne++;
                if (ne == name || *ne == '(') PPERR("unsupported macro definition", "");
                if (nmacros >= SHTOOL_MAX_MACROS) PPERR("too many macros", "");
                shtool_macro_t *m = &macros[nmacros];
                size_t ml = (size_t)(ne - name);
                if (ml >= sizeof m->name) PPERR("macro name too long", "");
                memcpy(m->name, name, ml); m->name[ml] = 0;
                const char *v = ne;
                while (*v == ' ' || *v == '\t') v++;
                size_t vl = strlen(v);
                while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                if (vl >= sizeof m->value) PPERR("macro value too long", "");
                memcpy(m->value, v, vl); m->value[vl] = 0;
                int k = macro_find(macros, nmacros, m->name, ml);
                if (k >= 0) macros[k] = *m; else nmacros++;
            } else if (dl == 5 && !memcmp(d, "undef", 5)) {
                const char *name = d + 6;
                while (*name == ' ' || *name == '\t') name++;
                const char *ne = name;
                while (ident_char(*ne)) ne++;
                int k = macro_find(macros, nmacros, name, (size_t)(ne - name));
                if (k >= 0) {
                    memmove(&macros[k], &macros[k + 1],
                            (size_t)(nmacros - k - 1) * sizeof macros[0]);
                    nmacros--;
                }
            } else {
                PPERR("unsupported preprocessing directive '%.*s'", (int)dl, d);
            }
        } else if (active) {
            if (append_expanded(&out, buf, macros, nmacros) != 0)
                PPERR("out of memory", "");
            if (sb_add(&out, "\n") != 0) PPERR("out of memory", "");
        }
    }
    if (ncond) PPERR("unclosed conditional", "");
    return out.p;
#undef PPERR
#undef SHTOOL_MAX_COND
#undef SHTOOL_MAX_MACROS
}

/* ---- file helpers ---- */
static char *read_text_file(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (1 << 20)) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    if (len) *len = (uint32_t)rd;
    return buf;
}

/* ---- stdcall entries ---- */
void s_D3DXDisassembleShader(void) {
    uint32_t shader = A32(0), color = A32(1), comments = A32(2), pp = A32(3);
    uint32_t hr = D3DERR_INVALIDCALL_;
    (void)color;
    if (pp) MEM32(pp) = 0;
    if (shader && pp) {
        uint32_t get_ptr = hle_resolve("ID3DXBuffer::GetBufferPointer");
        uint32_t get_size = hle_resolve("ID3DXBuffer::GetBufferSize");
        if (!get_ptr || !get_size) {
            fprintf(stderr, "[d3dx9] D3DXDisassembleShader: buffer vtable is unavailable\n");
            RET(D3DXERR_NOTAVAILABLE_, 4);
        }
        uint32_t pdata = hle_call_guest(get_ptr,1,(uint32_t[]){shader});
        uint32_t psize = hle_call_guest(get_size,1,(uint32_t[]){shader});
        if (pdata && psize >= 4u && (psize & 3u) == 0u) {
            char *text = NULL; char err[256];
            if (d3dx9_disassemble_text((const uint32_t *)hle_ptr(pdata), psize/4u, &text, err, sizeof err) == 0 && text) {
                uint32_t b = d3dx9_asm_make_buffer(text, strlen(text)+1u);
                free(text);
                if (b) { MEM32(pp) = b; hr = D3D_OK_; } else hr = E_OUTOFMEMORY_;
            } else {
                fprintf(stderr, "[d3dx9] D3DXDisassembleShader: %s\n", err);
                hr = E_FAIL_;
            }
        } else { fprintf(stderr, "[d3dx9] D3DXDisassembleShader: bad shader buffer\n"); hr = D3DERR_INVALIDCALL_; }
    }
    RET(hr, 4);
}

void s_D3DXGetVertexShaderProfile(void) { RET(d3dx9_shader_profile(A32(0), 0), 1); }
void s_D3DXGetPixelShaderProfile(void)  { RET(d3dx9_shader_profile(A32(0), 1), 1); }

static void assemble_file_common(uint32_t path, uint32_t defines, uint32_t include,
                                 uint32_t ppShader, uint32_t ppErr, int preprocess, int argc) {
    uint32_t hr = D3DERR_INVALIDCALL_;
    if (ppShader) MEM32(ppShader) = 0;
    if (ppErr) MEM32(ppErr) = 0;
    const char *name = path ? (const char *)hle_ptr(path) : NULL;
    if (!name) { RET(D3DERR_INVALIDCALL_, argc); }
    if (defines || include) {
        fprintf(stderr, "[d3dx9] shader file tools: defines/include not supported\n");
        RET(D3DXERR_NOTAVAILABLE_, argc);
    }
    uint32_t len = 0;
    char *src = read_text_file(name, &len);
    if (!src) {
        fprintf(stderr, "[d3dx9] shader file not readable: %s\n", name);
        RET(E_FAIL_, argc);
    }
    if (preprocess) {
        char perr[256];
        char *text = preprocess_text(src, len, perr, sizeof perr);
        if (text) {
            uint32_t b = d3dx9_asm_make_buffer(text, strlen(text) + 1u);
            free(text);
            if (b) { if (ppShader) MEM32(ppShader) = b; hr = D3D_OK_; } else hr = E_OUTOFMEMORY_;
        } else {
            fprintf(stderr, "[d3dx9] PreprocessShaderFromFile: %s\n", perr);
            uint32_t eb = d3dx9_asm_make_buffer(perr, strlen(perr) + 1u);
            if (eb && ppErr) MEM32(ppErr) = eb;
            hr = E_FAIL_;
        }
    } else {
        uint32_t *words = NULL; uint32_t nw = 0; char err[256];
        if (d3dx9_assemble(src, len, &words, &nw, err, sizeof err) == 0) {
            uint32_t b = d3dx9_asm_make_buffer(words, (size_t)nw * 4u);
            free(words);
            if (b) { if (ppShader) MEM32(ppShader) = b; hr = D3D_OK_; } else hr = E_OUTOFMEMORY_;
        } else {
            fprintf(stderr, "[d3dx9] AssembleShaderFromFile: %s\n", err);
            uint32_t eb = d3dx9_asm_make_buffer(err, strlen(err)+1u);
            if (eb && ppErr) MEM32(ppErr) = eb;
            hr = E_FAIL_;
        }
    }
    free(src);
    RET(hr, argc);
}
/* AssembleShaderFromFileA(filename, defines, include, flags, shader, error_messages) = 6 args */
void s_D3DXAssembleShaderFromFileA(void) { assemble_file_common(A32(0), A32(1), A32(2), A32(4), A32(5), 0, 6); }
void s_D3DXAssembleShaderFromFileW(void) {
    uint32_t fn = A32(0);
    uint32_t pva = 0;
    if (fn) {
        /* The guest path must live in guest memory, not on the host stack. */
        char *path = (char *)hle_alloc(1024);
        if (path) {
            uint16_t *w = (uint16_t *)hle_ptr(fn);
            size_t n = 0;
            while (n + 1u < 1024u && w[n]) { path[n] = (char)(w[n] < 128u ? w[n] : (uint16_t)'?'); n++; }
            path[n] = 0;
            pva = hle_va(path);
        }
    }
    assemble_file_common(pva, A32(1), A32(2), A32(4), A32(5), 0, 6);
    if (pva) hle_free(hle_ptr(pva));
}
/* PreprocessShaderFromFileA(filename, defines, include, shader, error_messages) = 5 args */
void s_D3DXPreprocessShaderFromFileA(void) { assemble_file_common(A32(0), A32(1), A32(2), A32(3), A32(4), 1, 5); }
