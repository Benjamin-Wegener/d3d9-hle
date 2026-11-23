/*
 * d3dx9_xfile.c - D3DXLoadMeshFromXA/W/InMemory.
 *
 * Parses the documented DirectX .x text format (xof ...txt ...) with Mesh,
 * MeshNormals, MeshTextureCoords and MeshMaterialList. Other formats or
 * unsupported templates report an error with log and correct HRESULT (no false OK).
 *
 * Source: public Microsoft D3DX9 documentation (D3DXLoadMeshFromX*,
 * D3DXMATERIAL, D3DMATERIAL9, .x file text format) and Wine d3dx9mesh.h for
 * ABI/signature/order only. No Microsoft DLLs, no foreign code copied.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include "win32hle.h"
#include "d3d9_hle.h"
#include "d3dx9_mesh.h"
#include "d3dx9_xfile.h"

#define D3D_OK_              0x00000000u
#define D3DERR_INVALIDCALL_  0x8876086Cu
#define E_OUTOFMEMORY_       0x8007000Eu
#define E_FAIL_              0x80004005u

#ifndef D3DFVF_XYZ
#define D3DFVF_XYZ    0x002u
#define D3DFVF_NORMAL 0x010u
#define D3DFVF_TEX1   0x100u
#endif

/* D3DXMATERIAL on 32-bit guest: D3DMATERIAL9 (68 bytes) + char* pTextureFilename (4). */
#define D3DMATERIAL9_SIZE    68u
#define D3DXMATERIAL_SIZE    72u

typedef struct {
    const char *p;
    const char *end;
    int line;
} xcur;

static void skip_ws_com(xcur *c) {
    for (;;) {
        while (c->p < c->end && (unsigned char)*c->p <= ' ') {
            if (*c->p == '\n') c->line++;
            c->p++;
        }
        if (c->p + 1 < c->end && c->p[0] == '/' && c->p[1] == '/') {
            while (c->p < c->end && *c->p != '\n') c->p++;
            continue;
        }
        if (c->p + 1 < c->end && c->p[0] == '#') {
            while (c->p < c->end && *c->p != '\n') c->p++;
            continue;
        }
        break;
    }
}

static int is_name_char(int ch) { return isalnum(ch) || ch == '_' || ch == '-'; }

static int read_token(xcur *c, char *out, size_t outsz) {
    skip_ws_com(c);
    if (c->p >= c->end) return 0;
    size_t n = 0;
    if (is_name_char((unsigned char)*c->p)) {
        while (c->p < c->end && is_name_char((unsigned char)*c->p)) {
            if (n + 1 < outsz) out[n] = *c->p;
            n++; c->p++;
        }
    } else {
        if (n + 1 < outsz) out[n] = *c->p;
        n++; c->p++;
    }
    if (outsz) out[n < outsz ? n : outsz - 1] = '\0';
    return 1;
}

static int peek_char(xcur *c) { skip_ws_com(c); return (c->p < c->end) ? (unsigned char)*c->p : -1; }
static int eat(xcur *c, char ch) { skip_ws_com(c); if (c->p < c->end && *c->p == ch) { c->p++; return 1; } return 0; }

/* strtol/strtod require a NUL-terminated string and would otherwise read past
 * the caller's buffer when a .x file is not NUL-terminated (the API takes an
 * explicit byte length). Copy only the numeric characters, bounded by c->end
 * and by the buffer, into a local NUL-terminated token first. A token longer
 * than the buffer is rejected rather than silently truncated. */
static int number_token(xcur *c, char *buf, size_t bufsz) {
    skip_ws_com(c);
    size_t n = 0;
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p;
        int numch = (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' ||
                    ch == '.' || ch == 'e' || ch == 'E';
        if (!numch) break;
        if (n + 1 >= bufsz) return 0;       /* leave room for the terminating NUL */
        buf[n++] = (char)ch;
        c->p++;
    }
    buf[n] = '\0';
    return n != 0;
}
static int read_int(xcur *c, int *out) {
    char tok[64];
    if (!number_token(c, tok, sizeof tok)) return 0;
    errno = 0;
    char *e = NULL; long v = strtol(tok, &e, 10);
    if (e == tok || *e != '\0') return 0;
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        fprintf(stderr, "[d3dx9_xfile] integer out of range: %s\n", tok);
        return 0;
    }
    *out = (int)v; return 1;
}
static int read_uint(xcur *c, uint32_t *out) { int v; if (!read_int(c, &v) || v < 0) return 0; *out = (uint32_t)v; return 1; }
static int read_float(xcur *c, float *out) {
    char tok[64];
    if (!number_token(c, tok, sizeof tok)) return 0;
    char *e = NULL; double v = strtod(tok, &e);
    if (e == tok) return 0;
    *out = (float)v; return 1;
}
static int read_string(xcur *c, char *out, size_t outsz) {
    skip_ws_com(c);
    if (c->p >= c->end || *c->p != '"') return 0;
    c->p++;
    size_t n = 0;
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p++;
        if (ch == '\\' && c->p < c->end) ch = *c->p++;
        if (n + 1 < outsz) out[n] = ch;
        n++;
    }
    if (c->p < c->end && *c->p == '"') c->p++;
    if (outsz) out[n < outsz ? n : outsz - 1] = '\0';
    return 1;
}

static int skip_braced(xcur *c) {
    if (!eat(c, '{')) return 0;
    int depth = 1;
    while (c->p < c->end && depth > 0) {
        char ch = *c->p++;
        if (ch == '\n') c->line++;
        else if (ch == '"') { while (c->p < c->end && *c->p != '"') { if (*c->p=='\\' && c->p+1<c->end) c->p++; c->p++; } if (c->p < c->end) c->p++; }
        else if (ch == '/' && c->p < c->end && *c->p == '/') { while (c->p < c->end && *c->p != '\n') c->p++; }
        else if (ch == '{') depth++;
        else if (ch == '}') depth--;
    }
    return depth == 0;
}

static int skip_data_object(xcur *c);

typedef struct { float x,y,z; } vec3;
typedef struct { float r,g,b,a; float power; float spec[3]; float emis[3]; char tex[260]; } matinfo;
typedef struct {
    vec3 *pos; uint32_t npos;
    uint32_t *idx; uint32_t nfaces; /* 3*nfaces */
    int has_normals;
    vec3 *normals; uint32_t nnormals;
    uint32_t *nidx; uint32_t nnfaces;
    int has_uv;
    float *uv; uint32_t nuv; /* u,v per vertex */
    int has_materials;
    uint32_t nmaterials;
    uint32_t *face_mat; uint32_t nface_mat;
    matinfo *mats;
} meshdata;

static void meshdata_free(meshdata *m) {
    if (!m) return;
    free(m->pos); free(m->idx); free(m->normals); free(m->nidx); free(m->uv);
    free(m->face_mat); free(m->mats);
    memset(m, 0, sizeof(*m));
}

static int skip_unknown_object(xcur *c) {
    /* consume optional name, then braced body */
    int pc = peek_char(c);
    if (pc != '{') {
        char maybe[64];
        xcur save = *c;
        if (read_token(c, maybe, sizeof(maybe)) && peek_char(c) == '{') { /* name consumed */ }
        else { *c = save; }
    }
    return skip_braced(c);
}

static int parse_mesh_normals(xcur *c, meshdata *m) {
    if (!eat(c, '{')) return 0;
    uint32_t n; if (!read_uint(c, &n)) return 0; eat(c, ';');
    vec3 *norm = (vec3 *)calloc(n ? n : 1, sizeof(vec3));
    if (!norm) return 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!read_float(c, &norm[i].x) || !eat(c, ';') ||
            !read_float(c, &norm[i].y) || !eat(c, ';') ||
            !read_float(c, &norm[i].z) || !eat(c, ';')) { free(norm); return 0; }
        eat(c, ','); eat(c, ';');
    }
    uint32_t nf; if (!read_uint(c, &nf)) { free(norm); return 0; } eat(c, ';');
    uint32_t *ni = (uint32_t *)malloc((size_t)nf * 3u * sizeof(uint32_t));
    if (!ni) { free(norm); return 0; }
    for (uint32_t f = 0; f < nf; f++) {
        uint32_t cnt; if (!read_uint(c, &cnt) || cnt != 3) { free(norm); free(ni); return 0; } eat(c, ';');
        for (uint32_t j = 0; j < 3; j++) { uint32_t v; if (!read_uint(c, &v)) { free(norm); free(ni); return 0; } ni[f*3+j]=v; eat(c, ','); }
        eat(c, ';'); eat(c, ','); eat(c, ';');
    }
    while (peek_char(c) != '}' && peek_char(c) != -1) {
        char tok[64]; xcur save = *c;
        if (!read_token(c, tok, sizeof(tok))) { free(norm); free(ni); return 0; }
        if (isalpha((unsigned char)tok[0])) { *c = save; if (!skip_data_object(c)) { free(norm); free(ni); return 0; } }
    }
    if (!eat(c, '}')) { free(norm); free(ni); return 0; }
    m->normals = norm; m->nnormals = n; m->nidx = ni; m->nnfaces = nf; m->has_normals = 1;
    return 1;
}

static int parse_mesh_texcoords(xcur *c, meshdata *m) {
    if (!eat(c, '{')) return 0;
    uint32_t n; if (!read_uint(c, &n)) return 0; eat(c, ';');
    float *uv = (float *)calloc(n ? n : 1, 2u * sizeof(float));
    if (!uv) return 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!read_float(c, &uv[i*2]) || !eat(c, ';') || !read_float(c, &uv[i*2+1]) || !eat(c, ';')) { free(uv); return 0; }
        eat(c, ','); eat(c, ';');
    }
    while (peek_char(c) != '}' && peek_char(c) != -1) {
        char tok[64]; xcur save = *c;
        if (!read_token(c, tok, sizeof(tok))) { free(uv); return 0; }
        if (isalpha((unsigned char)tok[0])) { *c = save; if (!skip_data_object(c)) { free(uv); return 0; } }
    }
    if (!eat(c, '}')) { free(uv); return 0; }
    m->uv = uv; m->nuv = n; m->has_uv = 1;
    return 1;
}

static int parse_material(xcur *c, matinfo *mi) {
    if (!eat(c, '{')) return 0;
    if (!read_float(c, &mi->r) || !eat(c, ';') || !read_float(c, &mi->g) || !eat(c, ';') ||
        !read_float(c, &mi->b) || !eat(c, ';') || !read_float(c, &mi->a) || !eat(c, ';')) return 0;
    eat(c, ','); eat(c, ';');
    if (!read_float(c, &mi->power) || !eat(c, ';')) return 0; eat(c, ','); eat(c, ';');
    if (!read_float(c, &mi->spec[0]) || !eat(c, ';') || !read_float(c, &mi->spec[1]) || !eat(c, ';') || !read_float(c, &mi->spec[2]) || !eat(c, ';')) return 0;
    eat(c, ','); eat(c, ';');
    if (!read_float(c, &mi->emis[0]) || !eat(c, ';') || !read_float(c, &mi->emis[1]) || !eat(c, ';') || !read_float(c, &mi->emis[2]) || !eat(c, ';')) return 0;
    eat(c, ','); eat(c, ';');
    mi->tex[0] = '\0';
    while (peek_char(c) != '}' && peek_char(c) != -1) {
        char tok[64]; xcur save = *c;
        if (!read_token(c, tok, sizeof(tok))) return 0;
        if (strcmp(tok, "TextureFilename") == 0) {
            if (!eat(c, '{') || !read_string(c, mi->tex, sizeof(mi->tex)) || !eat(c, ';') || !eat(c, '}')) return 0;
        } else if (isalpha((unsigned char)tok[0])) {
            *c = save; if (!skip_data_object(c)) return 0;
        } else { /* ignore */ }
    }
    return eat(c, '}');
}

static int parse_mesh_material_list(xcur *c, meshdata *m) {
    if (!eat(c, '{')) return 0;
    uint32_t nmat, nfaceidx;
    if (!read_uint(c, &nmat) || !eat(c, ';')) return 0;
    if (!read_uint(c, &nfaceidx) || !eat(c, ';')) return 0;
    uint32_t *fm = (uint32_t *)malloc((size_t)nfaceidx * sizeof(uint32_t));
    if (!fm) return 0;
    for (uint32_t i = 0; i < nfaceidx; i++) {
        int v; if (!read_int(c, &v)) { free(fm); return 0; }
        fm[i] = (v < 0) ? 0u : (uint32_t)v;
        eat(c, ','); eat(c, ';');
    }
    eat(c, ';');
    matinfo *mi = (matinfo *)calloc(nmat ? nmat : 1, sizeof(matinfo));
    if (!mi) { free(fm); return 0; }
    uint32_t got = 0;
    while (peek_char(c) != '}' && peek_char(c) != -1) {
        char tok[64]; xcur save = *c;
        if (!read_token(c, tok, sizeof(tok))) { free(fm); free(mi); return 0; }
        if (strcmp(tok, "Material") == 0) {
            if (got < nmat) {
                if (!parse_material(c, &mi[got])) { free(fm); free(mi); return 0; }
            } else {
                matinfo tmp; memset(&tmp, 0, sizeof(tmp));
                if (!parse_material(c, &tmp)) { free(fm); free(mi); return 0; }
            }
            got++;
        } else if (isalpha((unsigned char)tok[0])) {
            *c = save; if (!skip_data_object(c)) { free(fm); free(mi); return 0; }
        } else { /* ignore */ }
    }
    if (!eat(c, '}')) { free(fm); free(mi); return 0; }
    m->has_materials = 1; m->nmaterials = nmat; m->face_mat = fm; m->nface_mat = nfaceidx; m->mats = mi;
    return 1;
}

static int parse_mesh(xcur *c, meshdata *m) {
    if (!eat(c, '{')) return 0;
    uint32_t nv; if (!read_uint(c, &nv)) return 0; eat(c, ';');
    vec3 *pos = (vec3 *)calloc(nv ? nv : 1, sizeof(vec3));
    if (!pos) return 0;
    for (uint32_t i = 0; i < nv; i++) {
        if (!read_float(c, &pos[i].x) || !eat(c, ';') ||
            !read_float(c, &pos[i].y) || !eat(c, ';') ||
            !read_float(c, &pos[i].z) || !eat(c, ';')) { free(pos); return 0; }
        eat(c, ','); eat(c, ';');
    }
    uint32_t nf; if (!read_uint(c, &nf)) { free(pos); return 0; } eat(c, ';');
    uint32_t *idx = (uint32_t *)malloc((size_t)nf * 3u * sizeof(uint32_t));
    if (!idx) { free(pos); return 0; }
    for (uint32_t f = 0; f < nf; f++) {
        uint32_t cnt; if (!read_uint(c, &cnt) || cnt != 3) { free(pos); free(idx); return 0; } eat(c, ';');
        for (uint32_t j = 0; j < 3; j++) { uint32_t v; if (!read_uint(c, &v)) { free(pos); free(idx); return 0; } idx[f*3+j]=v; eat(c, ','); }
        eat(c, ';'); eat(c, ','); eat(c, ';');
    }
    m->pos = pos; m->npos = nv; m->idx = idx; m->nfaces = nf;
    /* nested objects */
    while (peek_char(c) != '}' && peek_char(c) != -1) {
        char tok[64]; xcur save = *c;
        if (!read_token(c, tok, sizeof(tok))) return 0;
        if (strcmp(tok, "MeshNormals") == 0) {
            if (!parse_mesh_normals(c, m)) return 0;
        } else if (strcmp(tok, "MeshTextureCoords") == 0) {
            if (!parse_mesh_texcoords(c, m)) return 0;
        } else if (strcmp(tok, "MeshMaterialList") == 0) {
            if (!parse_mesh_material_list(c, m)) return 0;
        } else if (isalpha((unsigned char)tok[0])) {
            *c = save;
            /* other nested objects (VertexDuplicationIndices, etc.) - unsupported for now: skip? task says "anderes meldet Fehler" */
            /* but some benign nested objects exist; task is strict. Skip only known-irrelevant? Better be strict for unknown. */
            /* Try to skip - if it's an unknown template data, fail. */
            /* We'll be strict: unknown nested object in Mesh is an error. */
            fprintf(stderr, "[d3dx9_xfile] unsupported Mesh child object '%s'\n", tok);
            return 0;
        } else {
            /* ignore stray punctuation */
        }
    }
    return eat(c, '}');
}

static int skip_template(xcur *c) {
    char name[64];
    if (!read_token(c, name, sizeof(name))) return 0;
    return skip_braced(c);
}

static int skip_data_object(xcur *c) {
    char tok[64]; xcur save = *c;
    if (!read_token(c, tok, sizeof(tok))) return 0;
    if (strcmp(tok, "template") == 0) return skip_template(c);
    /* data object: optional name, optional reference, or braced body */
    if (peek_char(c) == '{') return skip_braced(c);
    /* could be `Name { ... }` or `Type Name { ... }` */
    char tok2[64]; xcur save2 = *c;
    if (read_token(c, tok2, sizeof(tok2))) {
        if (strcmp(tok2, "{") == 0) { *c = save2; return skip_braced(c); }
        if (peek_char(c) == '{') return skip_braced(c);
        /* maybe `Type Name {`? we already consumed type in tok */
        char tok3[64]; xcur save3 = *c;
        if (read_token(c, tok3, sizeof(tok3)) && peek_char(c) == '{') return skip_braced(c);
        *c = save3;
        if (peek_char(c) == '{') return skip_braced(c);
    }
    *c = save2;
    if (peek_char(c) == '{') return skip_braced(c);
    /* reference object like { name } or empty; try skip braced */
    if (peek_char(c) == '{') return skip_braced(c);
    /* could be a reference object: just '}' will be eaten by parent */
    return 1;
}

static int parse_x_text(const char *data, size_t size, meshdata *out) {
    xcur c = { data, data + size, 1 };
    skip_ws_com(&c);
    /* header: xof 0302txt 0064 */
    char magic[8];
    if (c.p + 4 > c.end) return 0;
    memcpy(magic, c.p, 4); magic[4] = 0;
    if (strncmp(magic, "xof ", 4) != 0) {
        fprintf(stderr, "[d3dx9_xfile] bad header\n");
        return 0;
    }
    c.p += 4;
    char ver[16], fmt[16], fsz[16];
    /* read header tokens - space separated */
    skip_ws_com(&c);
    /* version like 0302txt 0064 or 0302txt 0032 etc; sometimes "0302txt0064" */
    size_t hn = 0;
    while (c.p < c.end && (unsigned char)*c.p > ' ' && hn < 12) { ver[hn++] = *c.p++; }
    ver[hn] = 0;
    if (strlen(ver) < 7) { fprintf(stderr, "[d3dx9_xfile] bad header\n"); return 0; }
    /* ver[4..6] should be txt/bin/tzip/bzip */
    if (strncmp(ver + 4, "txt", 3) != 0) {
        fprintf(stderr, "[d3dx9_xfile] unsupported xof format '%s' (text only)\n", ver);
        return 0;
    }
    skip_ws_com(&c);
    hn = 0; while (c.p < c.end && (unsigned char)*c.p > ' ' && hn < 8) { fsz[hn++] = *c.p++; } fsz[hn] = 0;
    (void)fsz; (void)fmt;

    memset(out, 0, sizeof(*out));
    int found = 0;
    while (peek_char(&c) != -1) {
        char tok[64]; xcur save = c;
        if (!read_token(&c, tok, sizeof(tok))) break;
        if (strcmp(tok, "template") == 0) {
            if (!skip_template(&c)) { meshdata_free(out); return 0; }
        } else if (strcmp(tok, "Mesh") == 0) {
            /* optional name */
            if (peek_char(&c) != '{') { char nm[64]; read_token(&c, nm, sizeof(nm)); }
            if (found) { fprintf(stderr, "[d3dx9_xfile] multiple Mesh objects unsupported\n"); meshdata_free(out); return 0; }
            if (!parse_mesh(&c, out)) { meshdata_free(out); return 0; }
            found = 1;
        } else if (isalpha((unsigned char)tok[0])) {
            c = save;
            if (!skip_data_object(&c)) { meshdata_free(out); return 0; }
        } else {
            /* ignore */
        }
    }
    if (!found) { fprintf(stderr, "[d3dx9_xfile] no Mesh object found\n"); meshdata_free(out); return 0; }
    return 1;
}

/* ---- mesh creation ---- */

static uint32_t make_dxbuffer(const void *data, uint32_t size) {
    uint32_t args[2] = { size, 0 };
    void *outp = hle_alloc(4);
    if (!outp) return 0;
    uint32_t outva = hle_va(outp);
    MEM32(outva) = 0;
    args[1] = outva;
    uint32_t hr = hle_call_guest(hle_resolve("D3DXCreateBuffer"), 2, args);
    uint32_t obj = MEM32(outva);
    hle_free(outp);
    if (hr != D3D_OK_ || !obj) return 0;
    if (data && size) {
        /* get buffer pointer */
        uint32_t ptr = hle_call_guest(hle_resolve("dxb_GetBufferPointer"), 1, (uint32_t[]){ obj });
        if (ptr) memcpy(hle_ptr(ptr), data, size);
    }
    return obj;
}

static int build_fvf(const meshdata *m, uint32_t *fvf, uint32_t *stride) {
    uint32_t f = D3DFVF_XYZ;
    uint32_t s = 12;
    if (m->has_normals) { f |= D3DFVF_NORMAL; s += 12; }
    if (m->has_uv) { f |= D3DFVF_TEX1; s += 8; }
    *fvf = f; *stride = s;
    /* validate consistency */
    if (m->has_normals && m->nnfaces != m->nfaces) return 0;
    if (m->has_uv && m->nuv != m->npos) return 0;
    return 1;
}

static uint32_t create_mesh_from_data(uint32_t dev, uint32_t options, const meshdata *m,
                                      uint32_t ppAdj, uint32_t ppMat, uint32_t ppEff, uint32_t pNumMat) {
    uint32_t fvf, stride;
    if (!build_fvf(m, &fvf, &stride)) { fprintf(stderr, "[d3dx9_xfile] inconsistent mesh data\n"); return 0; }
    /* create mesh */
    void *outp = hle_alloc(4);
    if (!outp) return 0;
    uint32_t outva = hle_va(outp); MEM32(outva) = 0;
    uint32_t args[6] = { m->nfaces, m->npos, options, fvf, dev, outva };
    uint32_t hr = hle_call_guest(hle_resolve("D3DXCreateMeshFVF"), 6, args);
    uint32_t mesh = MEM32(outva); hle_free(outp);
    if (hr != D3D_OK_ || !mesh) { fprintf(stderr, "[d3dx9_xfile] D3DXCreateMeshFVF failed 0x%08x\n", hr); return 0; }

    /* lock VB */
    void *ppdata = hle_alloc(4); if (!ppdata) return 0;
    uint32_t ppva = hle_va(ppdata); MEM32(ppva) = 0;
    hr = hle_call_guest(hle_resolve("ID3DXMesh::LockVertexBuffer"), 3, (uint32_t[]){ mesh, 0, ppva });
    uint32_t vbptr = MEM32(ppva); hle_free(ppdata);
    if (hr != D3D_OK_ || !vbptr) { fprintf(stderr, "[d3dx9_xfile] LockVertexBuffer failed\n"); return 0; }
    uint8_t *vb = (uint8_t *)hle_ptr(vbptr);
    memset(vb, 0, (size_t)m->npos * stride);
    for (uint32_t i = 0; i < m->npos; i++) {
        uint8_t *v = vb + (size_t)i * stride;
        uint32_t off = 0;
        memcpy(v + off, &m->pos[i], 12); off += 12;
        if (m->has_normals) { /* normals will be filled per-vertex later? normals are per-face-vertex in .x */ off += 12; }
        if (m->has_uv) { memcpy(v + off, &m->uv[i*2], 8); off += 8; }
    }
    hle_call_guest(hle_resolve("ID3DXMesh::UnlockVertexBuffer"), 1, (uint32_t[]){ mesh });

    /* lock IB */
    void *ppib = hle_alloc(4); if (!ppib) return 0;
    uint32_t ibva = hle_va(ppib); MEM32(ibva) = 0;
    hr = hle_call_guest(hle_resolve("ID3DXMesh::LockIndexBuffer"), 3, (uint32_t[]){ mesh, 0, ibva });
    uint32_t ibptr = MEM32(ibva); hle_free(ppib);
    if (hr != D3D_OK_ || !ibptr) { fprintf(stderr, "[d3dx9_xfile] LockIndexBuffer failed\n"); return 0; }
    uint16_t *ib = (uint16_t *)hle_ptr(ibptr);
    for (uint32_t i = 0; i < m->nfaces * 3u; i++) {
        if (m->idx[i] >= m->npos || m->idx[i] > 0xFFFFu) { fprintf(stderr, "[d3dx9_xfile] bad index\n"); return 0; }
        ib[i] = (uint16_t)m->idx[i];
    }
    hle_call_guest(hle_resolve("ID3DXMesh::UnlockIndexBuffer"), 1, (uint32_t[]){ mesh });

    /* normals: .x MeshNormals are per-face-vertex; our FVF has per-vertex NORMAL. */
    if (m->has_normals) {
        int same = (m->nnfaces == m->nfaces && m->nnormals == m->npos);
        if (same) {
            for (uint32_t i = 0; i < m->nfaces * 3u; i++) {
                if (m->nidx[i] != m->idx[i]) { same = 0; break; }
            }
        }
        if (same) {
            /* direct per-vertex normals */
            void *ppn = hle_alloc(4); if (!ppn) return 0;
            uint32_t ppnva = hle_va(ppn); MEM32(ppnva) = 0;
            hr = hle_call_guest(hle_resolve("ID3DXMesh::LockVertexBuffer"), 3, (uint32_t[]){ mesh, 0, ppnva });
            uint8_t *vbn = (uint8_t *)hle_ptr(MEM32(ppnva)); hle_free(ppn);
            if (hr == D3D_OK_ && vbn) {
                for (uint32_t i = 0; i < m->npos; i++) {
                    uint8_t *v = vbn + (size_t)i * stride;
                    uint32_t off = 12; /* after position */
                    if (i < m->nnormals) memcpy(v + off, &m->normals[i], 12);
                }
                hle_call_guest(hle_resolve("ID3DXMesh::UnlockVertexBuffer"), 1, (uint32_t[]){ mesh });
            }
        } else {
            /* expand: 3 vertices per face */
            uint32_t nnv = m->nfaces * 3u;
            void *out2p = hle_alloc(4); if (!out2p) return 0;
            uint32_t out2va = hle_va(out2p); MEM32(out2va) = 0;
            uint32_t hr2 = hle_call_guest(hle_resolve("D3DXCreateMeshFVF"), 6, (uint32_t[]){ m->nfaces, nnv, options, fvf, dev, out2va });
            uint32_t mesh2 = MEM32(out2va); hle_free(out2p);
            if (hr2 != D3D_OK_ || !mesh2) { fprintf(stderr, "[d3dx9_xfile] expand mesh failed\n"); return 0; }
            void *pp2 = hle_alloc(4); uint32_t pp2va = hle_va(pp2); MEM32(pp2va)=0;
            hle_call_guest(hle_resolve("ID3DXMesh::LockVertexBuffer"), 3, (uint32_t[]){ mesh2, 0, pp2va });
            uint8_t *vb2 = (uint8_t *)hle_ptr(MEM32(pp2va)); hle_free(pp2);
            void *pi2 = hle_alloc(4); uint32_t pi2va = hle_va(pi2); MEM32(pi2va)=0;
            hle_call_guest(hle_resolve("ID3DXMesh::LockIndexBuffer"), 3, (uint32_t[]){ mesh2, 0, pi2va });
            uint16_t *ib2 = (uint16_t *)hle_ptr(MEM32(pi2va)); hle_free(pi2);
            for (uint32_t f = 0; f < m->nfaces; f++) {
                for (uint32_t j = 0; j < 3; j++) {
                    uint32_t vi = m->idx[f*3+j];
                    uint32_t ni = (m->nidx && f < m->nnfaces) ? m->nidx[f*3+j] : vi;
                    uint32_t outvi = f*3+j;
                    uint8_t *v = vb2 + (size_t)outvi * stride;
                    uint32_t off = 0;
                    if (vi < m->npos) memcpy(v+off, &m->pos[vi], 12); off += 12;
                    if (ni < m->nnormals) memcpy(v+off, &m->normals[ni], 12); else memset(v+off, 0, 12); off += 12;
                    if (m->has_uv && vi < m->nuv) memcpy(v+off, &m->uv[vi*2], 8);
                    ib2[f*3+j] = (uint16_t)outvi;
                }
            }
            hle_call_guest(hle_resolve("ID3DXMesh::UnlockVertexBuffer"), 1, (uint32_t[]){ mesh2 });
            hle_call_guest(hle_resolve("ID3DXMesh::UnlockIndexBuffer"), 1, (uint32_t[]){ mesh2 });
            hle_call_guest(hle_resolve("ID3DXMesh::Release"), 1, (uint32_t[]){ mesh });
            mesh = mesh2;
        }
    }

    /* materials */
    if (ppMat) MEM32(ppMat) = 0;
    if (ppEff) MEM32(ppEff) = 0;
    if (pNumMat) MEM32(pNumMat) = 0;
    if (m->has_materials && ppMat && pNumMat) {
        uint32_t nmat = m->nmaterials ? m->nmaterials : 1;
        /* compute buffer size: nmat*D3DXMATERIAL + strings */
        uint32_t sz = nmat * D3DXMATERIAL_SIZE;
        for (uint32_t i = 0; i < m->nmaterials; i++) sz += (uint32_t)strlen(m->mats[i].tex) + 1u;
        uint32_t mbuf = make_dxbuffer(NULL, sz);
        if (mbuf) {
            uint32_t ptr = hle_call_guest(hle_resolve("dxb_GetBufferPointer"), 1, (uint32_t[]){ mbuf });
            uint8_t *base = (uint8_t *)hle_ptr(ptr);
            memset(base, 0, sz);
            uint32_t str_off = nmat * D3DXMATERIAL_SIZE;
            for (uint32_t i = 0; i < nmat; i++) {
                uint8_t *dm = base + i * D3DXMATERIAL_SIZE;
                /* D3DMATERIAL9: Diffuse,Ambient,Specular,Emissive,Power */
                float *f = (float *)dm;
                if (i < m->nmaterials) {
                    f[0]=m->mats[i].r; f[1]=m->mats[i].g; f[2]=m->mats[i].b; f[3]=m->mats[i].a; /* Diffuse */
                    f[4]=f[0]; f[5]=f[1]; f[6]=f[2]; f[7]=f[3]; /* Ambient = Diffuse */
                    f[8]=m->mats[i].spec[0]; f[9]=m->mats[i].spec[1]; f[10]=m->mats[i].spec[2]; f[11]=1.0f;
                    f[12]=m->mats[i].emis[0]; f[13]=m->mats[i].emis[1]; f[14]=m->mats[i].emis[2]; f[15]=1.0f;
                    f[16]=m->mats[i].power;
                    if (m->mats[i].tex[0]) {
                        uint32_t slen = (uint32_t)strlen(m->mats[i].tex) + 1u;
                        memcpy(base + str_off, m->mats[i].tex, slen);
                        MEM32(hle_va(dm + D3DMATERIAL9_SIZE)) = ptr + str_off;
                        str_off += slen;
                    } else {
                        MEM32(hle_va(dm + D3DMATERIAL9_SIZE)) = 0;
                    }
                } else {
                    f[0]=f[1]=f[2]=f[3]=1.0f; f[4]=f[5]=f[6]=f[7]=1.0f; f[11]=1.0f; f[15]=1.0f; f[16]=0.0f;
                    MEM32(hle_va(dm + D3DMATERIAL9_SIZE)) = 0;
                }
            }
            MEM32(ppMat) = mbuf;
            MEM32(pNumMat) = nmat;
        }
    } else {
        if (pNumMat) MEM32(pNumMat) = 0;
        if (ppMat) MEM32(ppMat) = 0;
    }
    /* effect instances */
    if (ppEff) MEM32(ppEff) = 0;

    /* adjacency */
    if (ppAdj) d3dx9_mesh_write_adjacency(mesh, ppAdj);

    /* attribute table - set face attributes from material list */
    if (m->has_materials && m->face_mat && m->nface_mat == m->nfaces) {
        /* lock attribute buffer and set per-face attribute */
        void *ppa = hle_alloc(4); if (ppa) {
            uint32_t ppava = hle_va(ppa); MEM32(ppava)=0;
            hr = hle_call_guest(hle_resolve("ID3DXMesh::LockAttributeBuffer"), 3, (uint32_t[]){ mesh, 0, ppava });
            uint32_t abuf = MEM32(ppava); hle_free(ppa);
            if (hr == D3D_OK_ && abuf) {
                uint32_t *attr = (uint32_t *)hle_ptr(abuf);
                for (uint32_t f = 0; f < m->nfaces; f++) attr[f] = m->face_mat[f];
                hle_call_guest(hle_resolve("ID3DXMesh::UnlockAttributeBuffer"), 1, (uint32_t[]){ mesh });
            }
        }
    }

    return mesh;
}

static uint32_t load_from_memory(const void *data, uint32_t size, uint32_t options, uint32_t dev,
                                 uint32_t ppAdj, uint32_t ppMat, uint32_t ppEff, uint32_t pNumMat, uint32_t ppMesh) {
    if (!data || !size || !ppMesh) { fprintf(stderr, "[d3dx9_xfile] invalid args\n"); return D3DERR_INVALIDCALL_; }
    if (ppMesh) MEM32(ppMesh) = 0;
    if (pNumMat) MEM32(pNumMat) = 0;
    if (ppAdj) MEM32(ppAdj) = 0;
    if (ppMat) MEM32(ppMat) = 0;
    if (ppEff) MEM32(ppEff) = 0;
    meshdata m;
    if (!parse_x_text((const char *)data, size, &m)) {
        fprintf(stderr, "[d3dx9_xfile] parse failed\n");
        return E_FAIL_;
    }
    uint32_t mesh = create_mesh_from_data(dev, options, &m, ppAdj, ppMat, ppEff, pNumMat);
    meshdata_free(&m);
    if (!mesh) return E_FAIL_;
    MEM32(ppMesh) = mesh;
    return D3D_OK_;
}

void s_D3DXLoadMeshFromXInMemory(void) {
    /* (Memory, SizeOfMemory, Options, pD3DDevice, ppAdjacency, ppMaterials, ppEffectInstances, pNumMaterials, ppMesh) */
    uint32_t mem = A32(0), sz = A32(1), opt = A32(2), dev = A32(3);
    uint32_t ppAdj = A32(4), ppMat = A32(5), ppEff = A32(6), pNum = A32(7), ppMesh = A32(8);
    void *ptr = mem ? hle_ptr(mem) : NULL;
    uint32_t hr = load_from_memory(ptr, sz, opt, dev, ppAdj, ppMat, ppEff, pNum, ppMesh);
    RET(hr, 9);
}

static uint32_t load_from_file_a(const char *path, uint32_t options, uint32_t dev,
                                 uint32_t ppAdj, uint32_t ppMat, uint32_t ppEff, uint32_t pNumMat, uint32_t ppMesh) {
    if (!path) { fprintf(stderr, "[d3dx9_xfile] NULL filename\n"); return D3DERR_INVALIDCALL_; }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[d3dx9_xfile] cannot open '%s'\n", path); return D3DERR_INVALIDCALL_; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)(16*1024*1024)) { fclose(f); fprintf(stderr, "[d3dx9_xfile] bad file size\n"); return E_FAIL_; }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return E_OUTOFMEMORY_; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return E_FAIL_; }
    fclose(f);
    uint32_t hr = load_from_memory(buf, (uint32_t)sz, options, dev, ppAdj, ppMat, ppEff, pNumMat, ppMesh);
    free(buf);
    return hr;
}

void s_D3DXLoadMeshFromXA(void) {
    /* (pFilename, Options, pD3DDevice, ppAdjacency, ppMaterials, ppEffectInstances, pNumMaterials, ppMesh) */
    uint32_t fn = A32(0), opt = A32(1), dev = A32(2);
    uint32_t ppAdj = A32(3), ppMat = A32(4), ppEff = A32(5), pNum = A32(6), ppMesh = A32(7);
    const char *path = fn ? (const char *)hle_ptr(fn) : NULL;
    uint32_t hr = load_from_file_a(path, opt, dev, ppAdj, ppMat, ppEff, pNum, ppMesh);
    RET(hr, 8);
}

void s_D3DXLoadMeshFromXW(void) {
    /* (pFilename, Options, pD3DDevice, ppAdjacency, ppMaterials, ppEffectInstances, pNumMaterials, ppMesh) */
    uint32_t fn = A32(0), opt = A32(1), dev = A32(2);
    uint32_t ppAdj = A32(3), ppMat = A32(4), ppEff = A32(5), pNum = A32(6), ppMesh = A32(7);
    if (!fn) { RET(D3DERR_INVALIDCALL_, 8); }
    /* simple wide-to-ASCII/UTF-8-ish conversion for path */
    uint16_t *w = (uint16_t *)hle_ptr(fn);
    char path[1024]; size_t n = 0;
    while (w[n] && n < sizeof(path)-1) { uint16_t c = w[n]; path[n] = (c < 128) ? (char)c : '?'; n++; }
    path[n] = '\0';
    uint32_t hr = load_from_file_a(path, opt, dev, ppAdj, ppMat, ppEff, pNum, ppMesh);
    RET(hr, 8);
}

/* ---- Hierarchy (D3DXLoadMeshHierarchyFromX / D3DXFrameDestroy) ---- */

typedef struct xframe_s {
    char name[256];
    float transform[16];
    meshdata mesh;              /* optional mesh in this frame */
    int has_mesh;
    struct xframe_s *child;
    struct xframe_s *sibling;
} xframe;

static void xframe_init(xframe *f) {
    memset(f, 0, sizeof(*f));
    /* identity matrix */
    f->transform[0]=f->transform[5]=f->transform[10]=f->transform[15]=1.0f;
}

static void xframe_free(xframe *f) {
    while (f) {
        xframe *next = f->sibling;
        xframe_free(f->child);
        if (f->has_mesh) meshdata_free(&f->mesh);
        free(f);
        f = next;
    }
}

static int parse_frame_transform(xcur *c, float m[16]) {
    if (!eat(c, '{')) return 0;
    for (int i = 0; i < 16; i++) {
        if (!read_float(c, &m[i])) return 0;
        eat(c, ','); eat(c, ';');
    }
    while (eat(c, ',') || eat(c, ';')) {}
    eat(c, '}');
    return 1;
}

static int parse_frame(xcur *c, xframe *out);

static int parse_frame_body(xcur *c, xframe *f) {
    if (!eat(c, '{')) return 0;
    xframe *last_child = NULL;
    while (peek_char(c) != -1) {
        if (eat(c, '}')) return 1;
        char tok[64]; xcur save = *c;
        if (!read_token(c, tok, sizeof(tok))) break;
        if (strcmp(tok, "FrameTransformMatrix") == 0) {
            /* optional name */
            if (peek_char(c) != '{') { char nm[64]; read_token(c, nm, sizeof(nm)); }
            if (!parse_frame_transform(c, f->transform)) return 0;
        } else if (strcmp(tok, "Frame") == 0) {
            xframe *ch = (xframe *)calloc(1, sizeof(xframe));
            if (!ch) return 0;
            xframe_init(ch);
            /* optional name */
            if (peek_char(c) != '{') { char nm[256]; if (read_token(c, nm, sizeof(nm))) strncpy(ch->name, nm, sizeof(ch->name)-1); }
            if (!parse_frame_body(c, ch)) { xframe_free(ch); return 0; }
            if (!f->child) f->child = ch;
            else last_child->sibling = ch;
            last_child = ch;
        } else if (strcmp(tok, "Mesh") == 0) {
            if (peek_char(c) != '{') { char nm[64]; read_token(c, nm, sizeof(nm)); }
            if (f->has_mesh) { fprintf(stderr, "[d3dx9_xfile] multiple Mesh in Frame unsupported\n"); return 0; }
            memset(&f->mesh, 0, sizeof(f->mesh));
            if (!parse_mesh(c, &f->mesh)) return 0;
            f->has_mesh = 1;
        } else if (strcmp(tok, "template") == 0) {
            if (!skip_template(c)) return 0;
        } else if (isalpha((unsigned char)tok[0])) {
            *c = save;
            if (!skip_data_object(c)) return 0;
        } else {
            /* ignore */
        }
    }
    return 0;
}

static int parse_frame(xcur *c, xframe *out) {
    xframe_init(out);
    if (peek_char(c) != '{') { char nm[256]; if (read_token(c, nm, sizeof(nm))) strncpy(out->name, nm, sizeof(out->name)-1); }
    return parse_frame_body(c, out);
}

static int parse_x_hierarchy(const char *data, size_t size, xframe **out_root) {
    xcur c = { data, data + size, 1 };
    skip_ws_com(&c);
    char magic[8];
    if (c.p + 4 > c.end) return 0;
    memcpy(magic, c.p, 4); magic[4] = 0;
    if (strncmp(magic, "xof ", 4) != 0) { fprintf(stderr, "[d3dx9_xfile] bad header\n"); return 0; }
    c.p += 4;
    skip_ws_com(&c);
    char ver[16]; size_t hn = 0;
    while (c.p < c.end && (unsigned char)*c.p > ' ' && hn < 12) { ver[hn++] = *c.p++; } ver[hn] = 0;
    if (strlen(ver) < 7 || strncmp(ver + 4, "txt", 3) != 0) {
        fprintf(stderr, "[d3dx9_xfile] unsupported xof format\n"); return 0;
    }
    skip_ws_com(&c); char fsz[16]; hn = 0; while (c.p < c.end && (unsigned char)*c.p > ' ' && hn < 8) { fsz[hn++] = *c.p++; } fsz[hn] = 0; (void)fsz;

    xframe *root = NULL, *last = NULL;
    int found = 0;
    while (peek_char(&c) != -1) {
        char tok[64]; xcur save = c;
        if (!read_token(&c, tok, sizeof(tok))) break;
        if (strcmp(tok, "template") == 0) {
            if (!skip_template(&c)) { xframe_free(root); return 0; }
        } else if (strcmp(tok, "Frame") == 0) {
            xframe *f = (xframe *)calloc(1, sizeof(xframe));
            if (!f) { xframe_free(root); return 0; }
            if (!parse_frame(&c, f)) { free(f); xframe_free(root); return 0; }
            if (!root) root = f; else last->sibling = f;
            last = f; found = 1;
        } else if (strcmp(tok, "Mesh") == 0) {
            /* top-level mesh: wrap in anonymous frame */
            xframe *f = (xframe *)calloc(1, sizeof(xframe));
            if (!f) { xframe_free(root); return 0; }
            xframe_init(f);
            if (peek_char(&c) != '{') { char nm[64]; read_token(&c, nm, sizeof(nm)); }
            memset(&f->mesh, 0, sizeof(f->mesh));
            if (!parse_mesh(&c, &f->mesh)) { free(f); xframe_free(root); return 0; }
            f->has_mesh = 1;
            if (!root) root = f; else last->sibling = f;
            last = f; found = 1;
        } else if (isalpha((unsigned char)tok[0])) {
            c = save;
            if (!skip_data_object(&c)) { xframe_free(root); return 0; }
        }
    }
    if (!found) { fprintf(stderr, "[d3dx9_xfile] no Frame/Mesh found\n"); xframe_free(root); return 0; }
    *out_root = root;
    return 1;
}

static uint32_t guest_alloc_str(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    void *p = hle_alloc(n);
    if (!p) return 0;
    memcpy(p, s, n);
    return hle_va(p);
}

static uint32_t call_alloc_create_frame(uint32_t alloc, const char *name, uint32_t *out_frame) {
    uint32_t vt = alloc ? MEM32(alloc) : 0;
    if (!vt) return E_FAIL_;
    uint32_t fn = MEM32(vt + 0 * 4); /* CreateFrame */
    uint32_t name_va = guest_alloc_str(name ? name : "");
    uint32_t outp = hle_va(hle_alloc(4));
    MEM32(outp) = 0;
    uint32_t hr = hle_call_guest(fn, 3, (uint32_t[]){ alloc, name_va, outp });
    *out_frame = MEM32(outp);
    hle_free(hle_ptr(outp));
    if (name_va) hle_free(hle_ptr(name_va));
    return hr;
}

static uint32_t call_alloc_destroy_frame(uint32_t alloc, uint32_t frame) {
    uint32_t vt = alloc ? MEM32(alloc) : 0;
    if (!vt) return E_FAIL_;
    uint32_t fn = MEM32(vt + 2 * 4); /* DestroyFrame */
    return hle_call_guest(fn, 2, (uint32_t[]){ alloc, frame });
}

static uint32_t call_alloc_destroy_mesh_container(uint32_t alloc, uint32_t mc) {
    uint32_t vt = alloc ? MEM32(alloc) : 0;
    if (!vt) return E_FAIL_;
    uint32_t fn = MEM32(vt + 3 * 4); /* DestroyMeshContainer */
    return hle_call_guest(fn, 2, (uint32_t[]){ alloc, mc });
}

static uint32_t create_mesh_container_for_frame(uint32_t dev, uint32_t alloc, xframe *f, uint32_t *out_mc) {
    *out_mc = 0;
    if (!f->has_mesh) return D3D_OK_;
    /* build mesh and materials/adjacency */
    uint32_t ppMesh = hle_va(hle_alloc(4)); MEM32(ppMesh) = 0;
    uint32_t ppAdj = hle_va(hle_alloc(4)); MEM32(ppAdj) = 0;
    uint32_t ppMat = hle_va(hle_alloc(4)); MEM32(ppMat) = 0;
    uint32_t pNum  = hle_va(hle_alloc(4)); MEM32(pNum) = 0;
    uint32_t ppEff = hle_va(hle_alloc(4)); MEM32(ppEff) = 0;
    uint32_t mesh = create_mesh_from_data(dev, 0, &f->mesh, ppAdj, ppMat, ppEff, pNum);
    if (!mesh) {
        hle_free(hle_ptr(ppMesh)); hle_free(hle_ptr(ppAdj)); hle_free(hle_ptr(ppMat)); hle_free(hle_ptr(pNum)); hle_free(hle_ptr(ppEff));
        return E_FAIL_;
    }
    MEM32(ppMesh) = mesh;
    uint32_t adj_buf = MEM32(ppAdj);
    uint32_t mat_buf = MEM32(ppMat);
    uint32_t num = MEM32(pNum);
    uint32_t mat_ptr = 0, adj_ptr = 0;
    if (mat_buf) mat_ptr = hle_call_guest(hle_resolve("ID3DXBuffer::GetBufferPointer"), 1, (uint32_t[]){ mat_buf });
    if (adj_buf) adj_ptr = hle_call_guest(hle_resolve("ID3DXBuffer::GetBufferPointer"), 1, (uint32_t[]){ adj_buf });

    /* D3DXMESHDATA: Type(4) + pMesh(4) = 8 bytes on 32-bit */
    uint32_t meshdata_va = hle_va(hle_alloc(8));
    MEM32(meshdata_va + 0) = 1; /* D3DXMESHTYPE_MESH */
    MEM32(meshdata_va + 4) = mesh;

    uint32_t vt = MEM32(alloc);
    uint32_t fn = MEM32(vt + 1 * 4); /* CreateMeshContainer */
    uint32_t name_va = guest_alloc_str(f->name[0] ? f->name : "");
    uint32_t outp = hle_va(hle_alloc(4)); MEM32(outp) = 0;
    /* CreateMeshContainer(this, name, mesh_data, materials, effect_instances, num_materials, adjacency, skin_info, new_mesh_container) */
    uint32_t hr = hle_call_guest(fn, 9, (uint32_t[]){ alloc, name_va, meshdata_va, mat_ptr, 0, num, adj_ptr, 0, outp });
    *out_mc = MEM32(outp);
    hle_free(hle_ptr(outp));
    if (name_va) hle_free(hle_ptr(name_va));
    hle_free(hle_ptr(meshdata_va));
    hle_free(hle_ptr(ppMesh)); hle_free(hle_ptr(ppAdj)); hle_free(hle_ptr(ppMat)); hle_free(hle_ptr(pNum)); hle_free(hle_ptr(ppEff));
    /* Release temp buffers we created; allocator should have copied what it needs. */
    if (adj_buf) { uint32_t vt2=MEM32(adj_buf); hle_call_guest(MEM32(vt2+8),1,(uint32_t[]){adj_buf}); }
    if (mat_buf) { uint32_t vt2=MEM32(mat_buf); hle_call_guest(MEM32(vt2+8),1,(uint32_t[]){mat_buf}); }
    if (hr != D3D_OK_) {
        uint32_t vt2=MEM32(mesh); hle_call_guest(MEM32(vt2+8),1,(uint32_t[]){mesh});
        *out_mc = 0;
    }
    return hr;
}

static uint32_t build_frame_guest(uint32_t dev, uint32_t alloc, xframe *f, uint32_t *out_frame) {
    uint32_t hr = call_alloc_create_frame(alloc, f->name, out_frame);
    if (hr != D3D_OK_ || !*out_frame) {
        fprintf(stderr, "[d3dx9_xfile] CreateFrame failed hr=0x%08x\n", hr);
        return hr ? hr : E_FAIL_;
    }
    uint32_t gf = *out_frame;
    /* D3DXFRAME: Name(0), TransformationMatrix(4, 64 bytes), pMeshContainer(68), pFrameSibling(72), pFrameFirstChild(76) */
    /* Name already set by allocator usually, but we set it if needed */
    if (f->name[0]) {
        /* don't overwrite allocator name blindly; only if null */
        if (MEM32(gf + 0) == 0) MEM32(gf + 0) = guest_alloc_str(f->name);
    }
    /* TransformationMatrix */
    for (int i = 0; i < 16; i++) MEM32(gf + 4 + i*4) = ((uint32_t *)f->transform)[i];

    /* mesh container */
    uint32_t mc = 0;
    hr = create_mesh_container_for_frame(dev, alloc, f, &mc);
    if (hr != D3D_OK_) { call_alloc_destroy_frame(alloc, gf); *out_frame = 0; return hr; }
    MEM32(gf + 68) = mc;

    /* children */
    uint32_t first_child = 0, prev_child = 0;
    for (xframe *ch = f->child; ch; ch = ch->sibling) {
        uint32_t gch = 0;
        hr = build_frame_guest(dev, alloc, ch, &gch);
        if (hr != D3D_OK_) { call_alloc_destroy_frame(alloc, gf); *out_frame = 0; return hr; }
        if (!first_child) first_child = gch;
        if (prev_child) MEM32(prev_child + 72) = gch; /* sibling */
        prev_child = gch;
    }
    MEM32(gf + 76) = first_child;
    MEM32(gf + 72) = 0; /* sibling set by parent */
    return D3D_OK_;
}

static uint32_t load_hierarchy_from_memory(const void *data, uint32_t size, uint32_t flags, uint32_t dev,
                                           uint32_t alloc, uint32_t user_data, uint32_t ppFrame, uint32_t ppAnim) {
    (void)flags; (void)user_data;
    if (!data || !size || !alloc || !ppFrame) { fprintf(stderr, "[d3dx9_xfile] hierarchy: invalid args\n"); return D3DERR_INVALIDCALL_; }
    MEM32(ppFrame) = 0;
    if (ppAnim) MEM32(ppAnim) = 0;
    xframe *root = NULL;
    if (!parse_x_hierarchy((const char *)data, size, &root)) return E_FAIL_;
    uint32_t gf = 0;
    uint32_t hr = build_frame_guest(dev, alloc, root, &gf);
    xframe_free(root);
    if (hr != D3D_OK_) return hr;
    MEM32(ppFrame) = gf;
    if (ppAnim) MEM32(ppAnim) = 0; /* animation controller not supported */
    return D3D_OK_;
}

void s_D3DXLoadMeshHierarchyFromXInMemory(void) {
    /* (Memory, SizeOfMemory, Flags, pD3DDevice, pAlloc, pUserDataLoader, ppFrameHierarchy, ppAnimController) */
    uint32_t mem = A32(0), sz = A32(1), flags = A32(2), dev = A32(3);
    uint32_t alloc = A32(4), user = A32(5), ppFrame = A32(6), ppAnim = A32(7);
    void *ptr = mem ? hle_ptr(mem) : NULL;
    uint32_t hr = load_hierarchy_from_memory(ptr, sz, flags, dev, alloc, user, ppFrame, ppAnim);
    RET(hr, 8);
}

void s_D3DXLoadMeshHierarchyFromXA(void) {
    /* (pFilename, Flags, pD3DDevice, pAlloc, pUserDataLoader, ppFrameHierarchy, ppAnimController) */
    uint32_t fn = A32(0), flags = A32(1), dev = A32(2);
    uint32_t alloc = A32(3), user = A32(4), ppFrame = A32(5), ppAnim = A32(6);
    const char *path = fn ? (const char *)hle_ptr(fn) : NULL;
    if (!path) { if (ppFrame) MEM32(ppFrame) = 0; if (ppAnim) MEM32(ppAnim) = 0; RET(D3DERR_INVALIDCALL_, 7); }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[d3dx9_xfile] hierarchy: cannot open '%s'\n", path); if (ppFrame) MEM32(ppFrame) = 0; if (ppAnim) MEM32(ppAnim) = 0; RET(D3DERR_INVALIDCALL_, 7); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)(16*1024*1024)) { fclose(f); if (ppFrame) MEM32(ppFrame) = 0; if (ppAnim) MEM32(ppAnim) = 0; RET(E_FAIL_, 7); }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); RET(E_OUTOFMEMORY_, 7); }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); RET(E_FAIL_, 7); }
    fclose(f);
    uint32_t hr = load_hierarchy_from_memory(buf, (uint32_t)sz, flags, dev, alloc, user, ppFrame, ppAnim);
    free(buf);
    RET(hr, 7);
}

void s_D3DXLoadMeshHierarchyFromXW(void) {
    /* (pFilename, Flags, pD3DDevice, pAlloc, pUserDataLoader, ppFrameHierarchy, ppAnimController) */
    uint32_t fn = A32(0), flags = A32(1), dev = A32(2);
    uint32_t alloc = A32(3), user = A32(4), ppFrame = A32(5), ppAnim = A32(6);
    if (!fn) { if (ppFrame) MEM32(ppFrame) = 0; if (ppAnim) MEM32(ppAnim) = 0; RET(D3DERR_INVALIDCALL_, 7); }
    uint16_t *w = (uint16_t *)hle_ptr(fn);
    char path[1024]; size_t n = 0;
    while (w[n] && n < sizeof(path)-1) { uint16_t ch = w[n]; path[n] = (ch < 128) ? (char)ch : '?'; n++; }
    path[n] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[d3dx9_xfile] hierarchy: cannot open '%s'\n", path); if (ppFrame) MEM32(ppFrame) = 0; if (ppAnim) MEM32(ppAnim) = 0; RET(D3DERR_INVALIDCALL_, 7); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)(16*1024*1024)) { fclose(f); if (ppFrame) MEM32(ppFrame) = 0; if (ppAnim) MEM32(ppAnim) = 0; RET(E_FAIL_, 7); }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); RET(E_OUTOFMEMORY_, 7); }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); RET(E_FAIL_, 7); }
    fclose(f);
    uint32_t hr = load_hierarchy_from_memory(buf, (uint32_t)sz, flags, dev, alloc, user, ppFrame, ppAnim);
    free(buf);
    RET(hr, 7);
}

static void frame_destroy_recursive(uint32_t alloc, uint32_t frame) {
    if (!frame) return;
    /* D3DXFRAME layout: Name(0), Transform(4..67), pMeshContainer(68), pFrameSibling(72), pFrameFirstChild(76) */
    uint32_t sibling = MEM32(frame + 72);
    uint32_t child = MEM32(frame + 76);
    uint32_t mc = MEM32(frame + 68);
    /* destroy children first */
    while (child) {
        uint32_t next = MEM32(child + 72);
        frame_destroy_recursive(alloc, child);
        child = next;
    }
    /* destroy mesh containers */
    while (mc) {
        /* D3DXMESHCONTAINER: ... pNextMeshContainer at offset 32 */
        uint32_t next_mc = MEM32(mc + 32);
        if (alloc) call_alloc_destroy_mesh_container(alloc, mc);
        mc = next_mc;
    }
    if (alloc) call_alloc_destroy_frame(alloc, frame);
    /* siblings handled by caller */
    (void)sibling;
}

void s_D3DXFrameDestroy(void) {
    /* (frame_root, alloc) = 2 args */
    uint32_t frame = A32(0);
    uint32_t alloc = A32(1);
    if (!frame) { RET(D3D_OK_, 2); }
    if (!alloc) { fprintf(stderr, "[d3dx9_xfile] FrameDestroy: NULL alloc\n"); RET(D3DERR_INVALIDCALL_, 2); }
    /* destroy siblings chain at root level too? D3DXFrameDestroy destroys frame_root and its children/subtrees;
     * siblings of frame_root are not destroyed by FrameDestroy per docs - it destroys the hierarchy rooted at frame_root. */
    frame_destroy_recursive(alloc, frame);
    RET(D3D_OK_, 2);
}

/* Host-only entry for the robustness harness: parse and free without a device.
 * Returns 1 for a supported text .x mesh, 0 for any rejected/broken input.
 * This lets the ASan harness link only the parser's direct dependencies,
 * without SDL2 (whose dyld constructor blocks under sanitizers). */
int d3dx9_xfile_parse_check(const char *data, size_t size) {
    meshdata m;
    if (!parse_x_text(data, size, &m)) return 0;
    meshdata_free(&m);
    return 1;
}
