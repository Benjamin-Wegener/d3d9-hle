/*
 * d3dx9_mesh.c - ID3DXMesh: D3DXCreateMesh / D3DXCreateMeshFVF, attributes,
 * adjacency, CloneMesh(FVF), Optimize.
 *
 * Interface order and signatures: the public d3dx9mesh.h (ID3DXBaseMesh, then
 * ID3DXMesh); Wine's include/d3dx9mesh.h gives the same order. Behaviour is
 * the documented one ("ID3DXMesh Interface", "D3DXMESHOPT"); nothing comes
 * from a Microsoft DLL and no foreign code is used.
 *
 * What a mesh holds: one vertex stream in system memory (the vertex
 * declaration, all elements in stream 0), 16- or 32-bit triangle-list indices,
 * one attribute DWORD per face, and an optional attribute table. Vertex and
 * index buffers are plain guest memory; GetVertexBuffer/GetIndexBuffer would
 * have to hand out live IDirect3D*Buffer9 objects sharing that memory and
 * therefore report an error instead.
 *
 * Optimize supports D3DXMESHOPT_COMPACT, _ATTRSORT, _IGNOREVERTS and
 * _DONOTSPLIT. _VERTEXCACHE, _STRIPREORDER and _DEVICEINDEPENDENT would need a
 * real cache optimiser and are refused with an error rather than pretended.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3dx9_mesh.h"

#define E_OUTOFMEMORY_  0x8007000Eu
#define MESH_UNUSED     0xFFFFFFFFu   /* D3DX_DEFAULT: unused face / no neighbour */

/* D3DXMESH_* options (d3dx9mesh.h). */
#define OPT_32BIT        0x00000001u
#define OPT_DONOTCLIP    0x00000002u
#define OPT_POINTS       0x00000004u
#define OPT_RTPATCHES    0x00000008u
#define OPT_VB_SYSTEMMEM 0x00000010u
#define OPT_VB_MANAGED   0x00000020u
#define OPT_VB_WRITEONLY 0x00000040u
#define OPT_VB_DYNAMIC   0x00000080u
#define OPT_IB_SYSTEMMEM 0x00000100u
#define OPT_IB_MANAGED   0x00000200u
#define OPT_IB_WRITEONLY 0x00000400u
#define OPT_IB_DYNAMIC   0x00000800u
#define OPT_VB_SHARE     0x00001000u
#define OPT_USEHWONLY    0x00002000u
#define OPT_NPATCHES     0x00004000u
#define OPT_VB_SWPROC    0x00008000u
#define OPT_IB_SWPROC    0x00010000u
#define OPT_SUPPORTED    (OPT_32BIT | OPT_DONOTCLIP | OPT_VB_SYSTEMMEM | OPT_VB_MANAGED | \
                          OPT_VB_WRITEONLY | OPT_VB_DYNAMIC | OPT_IB_SYSTEMMEM | \
                          OPT_IB_MANAGED | OPT_IB_WRITEONLY | OPT_IB_DYNAMIC | OPT_VB_SHARE | \
                          OPT_USEHWONLY | OPT_VB_SWPROC | OPT_IB_SWPROC)
#define OPT_MESH_BITS    0x0001FFFFu   /* the D3DXMESH_* bits of an Optimize `Flags` */

/* D3DXMESHOPT_* (d3dx9mesh.h). */
#define MOPT_DEVICEINDEPENDENT 0x00400000u
#define MOPT_COMPACT           0x01000000u
#define MOPT_ATTRSORT          0x02000000u
#define MOPT_VERTEXCACHE       0x04000000u
#define MOPT_STRIPREORDER      0x08000000u
#define MOPT_IGNOREVERTS       0x10000000u
#define MOPT_DONOTSPLIT        0x20000000u
#define MOPT_MASK              0x3FC00000u

/* IDirect3DDevice9 vtable slots (public d3d9.h order). */
#define DEV_ADDREF                1
#define DEV_RELEASE               2
#define DEV_DRAWINDEXEDPRIMUP    84
#define DEV_CREATEVERTEXDECL     86
#define DEV_SETVERTEXDECL        87
#define DEV_SETFVF               89

typedef struct { uint32_t AttribId, FaceStart, FaceCount, VertexStart, VertexCount; } attr_range_t;

typedef struct {
    uint32_t dev, options;
    uint32_t numVerts, numFaces, fvf, stride, declCount;
    uint32_t verts, idx, attr;         /* guest addresses */
    uint32_t vlock, ilock, alock;
    uint32_t declObj;                  /* IDirect3DVertexDeclaration9 made for DrawSubset */
    uint32_t table, tableCount;        /* guest address of attr_range_t[tableCount] */
    D3DVERTEXELEMENT9 decl[D3DX9_MAX_FVF_DECL_SIZE];
} mesh_state_t;
#define MS(o) ((mesh_state_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_mesh, g_vt_buf;
static const win32hle_shim mesh_methods[];

static void note(const char *what, const char *why) {
    fprintf(stderr, "[d3dx9_mesh] %s: %s\n", what, why);
}

static uint32_t dev_call(uint32_t dev, int slot, int argc, const uint32_t *a) {
    return hle_call_guest(MEM32(MEM32(dev) + 4u * (uint32_t)slot), argc, a);
}

/* ------------------------------------------------------------ FVF / decl */

static int is_float(uint8_t t) { return t <= 3; }   /* D3DDECLTYPE_FLOAT1..4 */

/* D3DDECLTYPE: 0-3 FLOAT1-4, 4 D3DCOLOR, 5 UBYTE4, 6 SHORT2, 7 SHORT4, 8 UBYTE4N,
 * 9 SHORT2N, 10 SHORT4N, 11 USHORT2N, 12 USHORT4N, 13 UDEC3, 14 DEC3N, 15 FLOAT16_2, 16 FLOAT16_4 */
static uint32_t decl_type_size(uint8_t t) {
    static const uint8_t sz[17] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8 };
    return t < 17 ? sz[t] : 0;
}

uint32_t d3dx9_fvf_vertex_size(uint32_t fvf) {
    uint32_t size = 0, pos = fvf & 0xEu;
    if (fvf & (0x1u | 0x1000u | 0x2000u | 0x4000u)) return 0;   /* LASTBETA_*, XYZW, reserved */
    switch (pos) {
    case 0x2: size = 12; break;  case 0x4: size = 16; break;  case 0x6: size = 16; break;
    case 0x8: size = 20; break;  case 0xA: size = 24; break;  case 0xC: size = 28; break;
    default: return 0;                           /* no position, XYZB5 */
    }
    if (fvf & 0x10u) size += 12;
    if (fvf & 0x20u) size += 4;
    if (fvf & 0x40u) size += 4;
    if (fvf & 0x80u) size += 4;
    uint32_t ntc = (fvf >> 8) & 0xFu;
    if (ntc > 8) return 0;
    for (uint32_t i = 0; i < ntc; i++) {
        uint32_t tcs = (fvf >> (16u + i * 2u)) & 3u;   /* 0=2 floats, 1=3, 2=4, 3=1 */
        size += ((tcs == 0) ? 2u : (tcs == 1) ? 3u : (tcs == 2) ? 4u : 1u) * 4u;
    }
    if (ntc < 8 && (fvf >> (16u + ntc * 2u))) return 0;   /* coordinate formats of unused sets */
    return size;
}

static void put_elem(D3DVERTEXELEMENT9 *d, uint16_t off, uint8_t type, uint8_t usage, uint8_t uidx) {
    d->Stream = 0; d->Offset = off; d->Type = type; d->Method = 0; d->Usage = usage; d->UsageIndex = uidx;
}

uint32_t d3dx9_decl_from_fvf(uint32_t fvf, D3DVERTEXELEMENT9 *d) {
    uint32_t n = 0, off = 0, pos = fvf & 0xEu;
    if (!d3dx9_fvf_vertex_size(fvf)) return 0;
    if (pos == 0x4) { put_elem(&d[n++], (uint16_t)off, 3, 9, 0); off += 16; }          /* POSITIONT FLOAT4 */
    else {
        put_elem(&d[n++], (uint16_t)off, 2, 0, 0); off += 12;                          /* POSITION FLOAT3 */
        if (pos >= 0x6) {
            uint32_t w = (pos - 0x4) / 2;                                              /* 1..4 blend weights */
            put_elem(&d[n++], (uint16_t)off, (uint8_t)(w - 1), 1, 0); off += w * 4;    /* BLENDWEIGHT FLOATw */
        }
    }
    if (fvf & 0x10u) { put_elem(&d[n++], (uint16_t)off, 2, 3, 0); off += 12; }         /* NORMAL */
    if (fvf & 0x20u) { put_elem(&d[n++], (uint16_t)off, 0, 4, 0); off += 4; }          /* PSIZE */
    if (fvf & 0x40u) { put_elem(&d[n++], (uint16_t)off, 4, 10, 0); off += 4; }         /* COLOR0 */
    if (fvf & 0x80u) { put_elem(&d[n++], (uint16_t)off, 4, 10, 1); off += 4; }         /* COLOR1 */
    uint32_t ntc = (fvf >> 8) & 0xFu;
    for (uint32_t i = 0; i < ntc; i++) {
        uint32_t tcs = (fvf >> (16u + i * 2u)) & 3u;
        uint8_t type = (tcs == 0) ? 1 : (tcs == 1) ? 2 : (tcs == 2) ? 3 : 0;
        put_elem(&d[n++], (uint16_t)off, type, 5, (uint8_t)i);
        off += (uint32_t)(type + 1) * 4u;
    }
    d[n].Stream = 0xFF; d[n].Offset = 0; d[n].Type = 17; d[n].Method = 0; d[n].Usage = 0; d[n].UsageIndex = 0;
    return n;
}

/* The FVF a declaration corresponds to, or 0 when it has none. */
static uint32_t fvf_from_decl(const D3DVERTEXELEMENT9 *d, uint32_t n) {
    uint32_t fvf = 0, ntc = 0, weights = 0;
    for (uint32_t i = 0; i < n; i++) {
        const D3DVERTEXELEMENT9 *e = &d[i];
        if (e->Usage == 0 && e->UsageIndex == 0 && e->Type == 2) fvf |= 0x2;
        else if (e->Usage == 9 && e->UsageIndex == 0 && e->Type == 3) fvf |= 0x4;
        else if (e->Usage == 1 && e->UsageIndex == 0 && e->Type <= 3) weights = (uint32_t)e->Type + 1;
        else if (e->Usage == 3 && e->UsageIndex == 0 && e->Type == 2) fvf |= 0x10;
        else if (e->Usage == 4 && e->UsageIndex == 0 && e->Type == 0) fvf |= 0x20;
        else if (e->Usage == 10 && e->UsageIndex == 0 && e->Type == 4) fvf |= 0x40;
        else if (e->Usage == 10 && e->UsageIndex == 1 && e->Type == 4) fvf |= 0x80;
        else if (e->Usage == 5 && e->UsageIndex < 8 && e->Type <= 3) {
            static const uint32_t code[4] = { 3, 0, 1, 2 };   /* FLOAT1..4 -> D3DFVF_TEXCOORDSIZE bits */
            fvf |= code[e->Type] << (16u + e->UsageIndex * 2u);
            if (e->UsageIndex + 1 > ntc) ntc = e->UsageIndex + 1;
        } else return 0;
    }
    if (weights) { if ((fvf & 0xE) != 0x2) return 0; fvf = (fvf & ~0xEu) | (0x4u + 2u * weights); }
    fvf |= ntc << 8;
    D3DVERTEXELEMENT9 canon[D3DX9_MAX_FVF_DECL_SIZE];
    uint32_t cn = d3dx9_decl_from_fvf(fvf, canon);
    if (cn != n || memcmp(canon, d, n * sizeof *d) != 0) return 0;
    return fvf;
}

/* Number of elements before D3DDECL_END, or 0 when there is none within 64. */
static uint32_t decl_count(const D3DVERTEXELEMENT9 *d) {
    for (uint32_t i = 0; i < D3DX9_MAX_FVF_DECL_SIZE - 1u; i++) if (d[i].Stream == 0xFF) return i;
    return 0;
}

/* The vertex size of a declaration; 0 (with a log) if it is not representable. */
static uint32_t decl_stride(const D3DVERTEXELEMENT9 *d, uint32_t n, const char *who) {
    uint32_t stride = 0;
    if (!n) { note(who, "declaration is empty or not terminated"); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sz = decl_type_size(d[i].Type);
        if (d[i].Stream != 0) { note(who, "only vertex stream 0 is supported"); return 0; }
        if (!sz) { note(who, "unknown vertex element type"); return 0; }
        if (d[i].Offset + sz > stride) stride = d[i].Offset + sz;
    }
    return stride;
}

/* ------------------------------------------------------------- objects */

static uint32_t idx_size(const mesh_state_t *s) { return (s->options & OPT_32BIT) ? 4u : 2u; }
static uint32_t get_idx(const mesh_state_t *s, uint32_t i) {
    return (s->options & OPT_32BIT) ? ((const uint32_t *)hle_ptr(s->idx))[i]
                                    : ((const uint16_t *)hle_ptr(s->idx))[i];
}
static void put_idx(const mesh_state_t *s, uint32_t i, uint32_t v) {
    if (s->options & OPT_32BIT) ((uint32_t *)hle_ptr(s->idx))[i] = v;
    else ((uint16_t *)hle_ptr(s->idx))[i] = (uint16_t)v;
}

/* 0 when `options` can be honoured, else the D3DERR_* to answer. */
static uint32_t check_options(uint32_t options, const char *who) {
    if (options & ~(OPT_SUPPORTED | OPT_POINTS | OPT_RTPATCHES | OPT_NPATCHES)) return D3DERR_INVALIDCALL;
    if (options & (OPT_POINTS | OPT_RTPATCHES | OPT_NPATCHES)) {
        note(who, "D3DXMESH_POINTS / _RTPATCHES / _NPATCHES are not supported");
        return D3DERR_NOTAVAILABLE;
    }
    if ((options & (OPT_VB_SYSTEMMEM | OPT_VB_MANAGED)) == (OPT_VB_SYSTEMMEM | OPT_VB_MANAGED)) return D3DERR_INVALIDCALL;
    if ((options & (OPT_IB_SYSTEMMEM | OPT_IB_MANAGED)) == (OPT_IB_SYSTEMMEM | OPT_IB_MANAGED)) return D3DERR_INVALIDCALL;
    return 0;
}

static uint32_t buf_new(const void *data, uint32_t size);

static void mesh_destroy(uint32_t self) {
    mesh_state_t *s = MS(self);
    if (s->verts) hle_free(hle_ptr(s->verts));
    if (s->idx) hle_free(hle_ptr(s->idx));
    if (s->attr) hle_free(hle_ptr(s->attr));
    if (s->table) hle_free(hle_ptr(s->table));
    if (s->declObj) dev_call(s->declObj, 2, 1, (uint32_t[]){ s->declObj });
    if (s->dev) dev_call(s->dev, DEV_RELEASE, 1, (uint32_t[]){ s->dev });
    hle_com_free(self);
}

static uint32_t mesh_new(uint32_t dev, uint32_t options, const D3DVERTEXELEMENT9 *decl,
                         uint32_t nfaces, uint32_t nverts, uint32_t *hr, const char *who) {
    *hr = D3DERR_INVALIDCALL;
    if (!dev || !nfaces || !nverts) return 0;
    if ((*hr = check_options(options, who)) != 0) return 0;
    *hr = D3DERR_INVALIDCALL;
    uint32_t n = decl_count(decl), stride = decl_stride(decl, n, who);
    if (!stride) return 0;
    if (!(options & OPT_32BIT) && nverts > 0xFFFFu) { note(who, "more than 65535 vertices need D3DXMESH_32BIT"); return 0; }
    if (nfaces > 0x1FFFFFFFu || (uint64_t)nverts * stride > 0x7FFFFFFFu) return 0;
    *hr = E_OUTOFMEMORY_;
    if (!g_vt_mesh) g_vt_mesh = hle_com_vtable(mesh_methods);
    uint32_t obj = hle_com_new(g_vt_mesh, 8 + sizeof(mesh_state_t));
    if (!obj) return 0;
    mesh_state_t *s = MS(obj);
    memset(s, 0, sizeof *s);
    s->dev = dev; s->options = options;
    s->numVerts = nverts; s->numFaces = nfaces; s->stride = stride; s->declCount = n;
    memcpy(s->decl, decl, (n + 1) * sizeof *decl);
    s->fvf = fvf_from_decl(decl, n);
    size_t vb = (size_t)nverts * stride, ib = (size_t)nfaces * 3u * idx_size(s), ab = (size_t)nfaces * 4u;
    void *v = hle_alloc((uint32_t)vb), *i = hle_alloc((uint32_t)ib), *a = hle_alloc((uint32_t)ab);
    if (!v || !i || !a) {
        if (v) hle_free(v);
        if (i) hle_free(i);
        if (a) hle_free(a);
        hle_com_free(obj);
        return 0;
    }
    memset(v, 0, vb); memset(i, 0, ib); memset(a, 0, ab);
    s->verts = hle_va(v); s->idx = hle_va(i); s->attr = hle_va(a);
    dev_call(dev, DEV_ADDREF, 1, (uint32_t[]){ dev });
    *hr = D3D_OK;
    return obj;
}

/* Create*: (NumFaces, NumVertices, Options, FVF|pDeclaration, pDevice, ppMesh) */
void s_D3DXCreateMeshFVF(void) {
    uint32_t nf = A32(0), nv = A32(1), options = A32(2), fvf = A32(3), dev = A32(4), pp = A32(5), hr = D3DERR_INVALIDCALL;
    D3DVERTEXELEMENT9 d[D3DX9_MAX_FVF_DECL_SIZE];
    if (pp) MEM32(pp) = 0;
    if (!pp || !d3dx9_decl_from_fvf(fvf, d)) {
        if (pp) note("D3DXCreateMeshFVF", "FVF not supported (needs a position, no XYZB5/XYZW/LASTBETA)");
    } else {
        uint32_t m = mesh_new(dev, options, d, nf, nv, &hr, "D3DXCreateMeshFVF");
        if (m) MEM32(pp) = m;
    }
    RET(hr, 6);
}
void s_D3DXCreateMesh(void) {
    uint32_t nf = A32(0), nv = A32(1), options = A32(2), pdecl = A32(3), dev = A32(4), pp = A32(5), hr = D3DERR_INVALIDCALL;
    if (pp) MEM32(pp) = 0;
    if (pp && pdecl) {
        uint32_t m = mesh_new(dev, options, (const D3DVERTEXELEMENT9 *)hle_ptr(pdecl), nf, nv, &hr, "D3DXCreateMesh");
        if (m) MEM32(pp) = m;
    }
    RET(hr, 6);
}

uint32_t d3dx9_mesh_new_fvf(uint32_t dev, uint32_t fvf, uint32_t nfaces, uint32_t nverts) {
    D3DVERTEXELEMENT9 d[D3DX9_MAX_FVF_DECL_SIZE];
    uint32_t hr;
    if (!d3dx9_decl_from_fvf(fvf, d)) return 0;
    return mesh_new(dev, 0, d, nfaces, nverts, &hr, "mesh");
}
void d3dx9_mesh_data(uint32_t mesh, void **vertices, void **indices16) {
    mesh_state_t *s = MS(mesh);
    *vertices = hle_ptr(s->verts);
    *indices16 = hle_ptr(s->idx);
}

/* ------------------------------------------------- ID3DXBuffer (remaps) */

static void buf_get_ptr(void)  { RET(*(uint32_t *)((char *)hle_ptr(A32(0)) + 8), 1); }
static void buf_get_size(void) { RET(*(uint32_t *)((char *)hle_ptr(A32(0)) + 12), 1); }
static void buf_release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) {
        uint32_t d = *(uint32_t *)((char *)hle_ptr(self) + 8);
        if (d) hle_free(hle_ptr(d));
        hle_com_free(self);
    }
    RET(n, 1);
}
static void buf_addref(void)  { uint32_t self = A32(0); MEM32(self + 4)++; RET(MEM32(self + 4), 1); }
static void buf_qi(void) {
    uint32_t self = A32(0), pp = A32(2), hr = D3DERR_INVALIDCALL;
    if (pp) { MEM32(pp) = self; MEM32(self + 4)++; hr = D3D_OK; }
    RET(hr, 3);
}
static const win32hle_shim buf_methods[] = {
    { "ID3DXMeshBuffer::QueryInterface", d3d_QueryInterface },
    { "ID3DXMeshBuffer::AddRef", buf_addref },
    { "ID3DXMeshBuffer::Release", buf_release },
    { "ID3DXMeshBuffer::GetBufferPointer", buf_get_ptr },
    { "ID3DXMeshBuffer::GetBufferSize", buf_get_size },
    { 0, 0 }
};
/* An ID3DXBuffer holding a copy of `data`. */
static uint32_t buf_new(const void *data, uint32_t size) {
    if (!g_vt_buf) g_vt_buf = hle_com_vtable(buf_methods);
    void *g = hle_alloc(size ? size : 1);
    if (!g) return 0;
    if (data) memcpy(g, data, size); else memset(g, 0, size);
    uint32_t obj = hle_com_new(g_vt_buf, 16);
    if (!obj) { hle_free(g); return 0; }
    *(uint32_t *)((char *)hle_ptr(obj) + 8) = hle_va(g);
    *(uint32_t *)((char *)hle_ptr(obj) + 12) = size;
    return obj;
}

/* ------------------------------------------------- adjacency, point reps */

typedef struct { uint32_t a, b, face, edge, dir; } edge_rec_t;
static int edge_cmp(const void *x, const void *y) {
    const edge_rec_t *p = x, *q = y;
    if (p->a != q->a) return p->a < q->a ? -1 : 1;
    if (p->b != q->b) return p->b < q->b ? -1 : 1;
    if (p->face != q->face) return p->face < q->face ? -1 : 1;
    return p->edge < q->edge ? -1 : p->edge > q->edge;
}

/* Every face index must be inside the vertex buffer. */
static int faces_valid(const mesh_state_t *s) {
    for (uint32_t i = 0; i < s->numFaces * 3u; i++) if (get_idx(s, i) >= s->numVerts) return 0;
    return 1;
}

/* adj[3 * face + edge] = the face on the other side of edge (v[edge], v[edge+1]),
 * MESH_UNUSED if none. Vertices count as the same point when `rep` says so.
 * Of several candidates the face that runs the edge the opposite way wins. */
static int build_adjacency(const mesh_state_t *s, const uint32_t *rep, uint32_t *adj) {
    uint32_t nf = s->numFaces, n = nf * 3u;
    edge_rec_t *r = (edge_rec_t *)malloc((size_t)n * sizeof *r);
    if (!r) return 0;
    for (uint32_t f = 0; f < nf; f++)
        for (uint32_t e = 0; e < 3; e++) {
            uint32_t va = rep[get_idx(s, f * 3 + e)], vb = rep[get_idx(s, f * 3 + (e + 1) % 3)];
            edge_rec_t *x = &r[f * 3 + e];
            x->face = f; x->edge = e; x->dir = va > vb;
            if (va == vb) { x->a = x->b = MESH_UNUSED; }              /* collapsed edge */
            else { x->a = va < vb ? va : vb; x->b = va < vb ? vb : va; }
            adj[f * 3 + e] = MESH_UNUSED;
        }
    qsort(r, n, sizeof *r, edge_cmp);
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i;
        while (j < n && r[j].a == r[i].a && r[j].b == r[i].b) j++;
        if (r[i].a != MESH_UNUSED)
            for (uint32_t k = i; k < j; k++) {
                int best = -1;
                for (uint32_t m = i; m < j; m++) {
                    if (r[m].face == r[k].face) continue;
                    if (r[m].dir != r[k].dir) { best = (int)m; break; }
                    if (best < 0) best = (int)m;
                }
                if (best >= 0) adj[r[k].face * 3 + r[k].edge] = r[best].face;
            }
        i = j;
    }
    free(r);
    return 1;
}

static uint32_t uf_find(uint32_t *p, uint32_t x) {
    while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
    return x;
}
static void uf_union(uint32_t *p, uint32_t a, uint32_t b) {   /* the smaller index stays the root */
    a = uf_find(p, a); b = uf_find(p, b);
    if (a < b) p[b] = a; else if (b < a) p[a] = b;
}

/* GenerateAdjacency: positions within `eps` on every axis are the same point. */
static const float *g_sort_x;
static int by_x(const void *a, const void *b) {
    float x = g_sort_x[*(const uint32_t *)a], y = g_sort_x[*(const uint32_t *)b];
    return x < y ? -1 : x > y;
}
static int point_reps_from_positions(const mesh_state_t *s, float eps, uint32_t *rep, const char *who) {
    const D3DVERTEXELEMENT9 *pe = NULL;
    for (uint32_t i = 0; i < s->declCount; i++)
        if (s->decl[i].Usage == 0 && s->decl[i].UsageIndex == 0 && (s->decl[i].Type == 2 || s->decl[i].Type == 3)) { pe = &s->decl[i]; break; }
    if (!pe) { note(who, "the vertex declaration has no FLOAT3/FLOAT4 POSITION element"); return 0; }
    uint32_t nv = s->numVerts;
    float *pos = (float *)malloc((size_t)nv * 3 * sizeof(float));
    uint32_t *ord = (uint32_t *)malloc((size_t)nv * sizeof *ord);
    if (!pos || !ord) { free(pos); free(ord); return 0; }
    for (uint32_t v = 0; v < nv; v++) {
        memcpy(&pos[v * 3], (const char *)hle_ptr(s->verts) + (size_t)v * s->stride + pe->Offset, 12);
        rep[v] = v; ord[v] = v;
    }
    float *xs = (float *)malloc((size_t)nv * sizeof(float));
    if (!xs) { free(pos); free(ord); return 0; }
    for (uint32_t v = 0; v < nv; v++) xs[v] = pos[v * 3];
    g_sort_x = xs;
    qsort(ord, nv, sizeof *ord, by_x);
    for (uint32_t i = 0; i < nv; i++)
        for (uint32_t j = i + 1; j < nv && xs[ord[j]] - xs[ord[i]] <= eps; j++) {
            const float *p = &pos[ord[i] * 3], *q = &pos[ord[j] * 3];
            if (fabsf(p[1] - q[1]) <= eps && fabsf(p[2] - q[2]) <= eps) uf_union(rep, ord[i], ord[j]);
        }
    for (uint32_t v = 0; v < nv; v++) rep[v] = uf_find(rep, v);
    free(pos); free(ord); free(xs);
    return 1;
}

/* Adjacency of a mesh as an ID3DXBuffer for the shape generators. */
void d3dx9_mesh_write_adjacency(uint32_t mesh, uint32_t ppAdjacency) {
    mesh_state_t *s = MS(mesh);
    uint32_t nv = s->numVerts, nf = s->numFaces, out = 0;
    if (!ppAdjacency) return;
    uint32_t *rep = (uint32_t *)malloc((size_t)nv * 4), *adj = (uint32_t *)malloc((size_t)nf * 12);
    if (rep && adj && point_reps_from_positions(s, 1e-5f, rep, "shape adjacency") && build_adjacency(s, rep, adj))
        out = buf_new(adj, nf * 12u);
    free(rep); free(adj);
    MEM32(ppAdjacency) = out;
}

void mesh_GenerateAdjacency(void) {   /* (this, Epsilon, pAdjacency) */
    mesh_state_t *s = MS(A32(0));
    uint32_t eb = A32(1), pAdj = A32(2), hr = D3DERR_INVALIDCALL;
    float eps; memcpy(&eps, &eb, 4);
    if (pAdj && eps >= 0.0f && faces_valid(s)) {
        uint32_t *rep = (uint32_t *)malloc((size_t)s->numVerts * 4), *adj = (uint32_t *)malloc((size_t)s->numFaces * 12);
        if (rep && adj && point_reps_from_positions(s, eps, rep, "ID3DXMesh::GenerateAdjacency") && build_adjacency(s, rep, adj)) {
            memcpy(hle_ptr(pAdj), adj, (size_t)s->numFaces * 12);
            hr = D3D_OK;
        }
        free(rep); free(adj);
    }
    RET(hr, 3);
}

void mesh_ConvertPointRepsToAdjacency(void) {   /* (this, pPRep, pAdjacency); NULL pPRep: every vertex is its own point */
    mesh_state_t *s = MS(A32(0));
    uint32_t pRep = A32(1), pAdj = A32(2), hr = D3DERR_INVALIDCALL;
    if (pAdj && faces_valid(s)) {
        uint32_t *rep = (uint32_t *)malloc((size_t)s->numVerts * 4), *adj = (uint32_t *)malloc((size_t)s->numFaces * 12);
        int good = rep && adj;
        for (uint32_t v = 0; good && v < s->numVerts; v++) {
            rep[v] = pRep ? ((const uint32_t *)hle_ptr(pRep))[v] : v;
            if (rep[v] >= s->numVerts) good = 0;
        }
        if (good && build_adjacency(s, rep, adj)) { memcpy(hle_ptr(pAdj), adj, (size_t)s->numFaces * 12); hr = D3D_OK; }
        free(rep); free(adj);
    }
    RET(hr, 3);
}

void mesh_ConvertAdjacencyToPointReps(void) {   /* (this, pAdjacency, pPRep) */
    mesh_state_t *s = MS(A32(0));
    uint32_t pAdj = A32(1), pRep = A32(2), hr = D3DERR_INVALIDCALL;
    uint32_t nf = s->numFaces;
    if (pAdj && pRep && faces_valid(s)) {
        const uint32_t *adj = (const uint32_t *)hle_ptr(pAdj);
        uint32_t *p = (uint32_t *)malloc((size_t)s->numVerts * 4);
        int good = p != NULL;
        for (uint32_t v = 0; good && v < s->numVerts; v++) p[v] = v;
        for (uint32_t i = 0; good && i < nf * 3u; i++) if (adj[i] != MESH_UNUSED && adj[i] >= nf) good = 0;
        for (uint32_t f = 0; good && f < nf; f++)
            for (uint32_t e = 0; e < 3; e++) {
                uint32_t g = adj[f * 3 + e];
                if (g == MESH_UNUSED) continue;
                uint32_t e2 = 3;
                for (uint32_t k = 0; k < 3; k++) if (adj[g * 3 + k] == f) { e2 = k; break; }
                if (e2 == 3) continue;                       /* not mutual: no information */
                uint32_t a0 = get_idx(s, f * 3 + e), a1 = get_idx(s, f * 3 + (e + 1) % 3);
                uint32_t b0 = get_idx(s, g * 3 + e2), b1 = get_idx(s, g * 3 + (e2 + 1) % 3);
                uf_union(p, a0, b1); uf_union(p, a1, b0);    /* the neighbour runs the edge backwards */
            }
        if (good) {
            for (uint32_t v = 0; v < s->numVerts; v++) ((uint32_t *)hle_ptr(pRep))[v] = uf_find(p, v);
            hr = D3D_OK;
        }
        free(p);
    }
    RET(hr, 3);
}

/* ------------------------------------------------------------ plain methods */

void mesh_QueryInterface(void) {
    uint32_t self = A32(0), ppv = A32(2), hr = D3DERR_INVALIDCALL;
    if (ppv) { MEM32(ppv) = self; MEM32(self + 4)++; hr = D3D_OK; }
    RET(hr, 3);
}
void mesh_AddRef(void) { uint32_t self = A32(0); MEM32(self + 4)++; RET(MEM32(self + 4), 1); }
void mesh_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) mesh_destroy(self);
    RET(n, 1);
}
void mesh_GetNumFaces(void) { RET(MS(A32(0))->numFaces, 1); }
void mesh_GetNumVertices(void) { RET(MS(A32(0))->numVerts, 1); }
void mesh_GetFVF(void) { RET(MS(A32(0))->fvf, 1); }
void mesh_GetNumBytesPerVertex(void) { RET(MS(A32(0))->stride, 1); }
void mesh_GetOptions(void) { RET(MS(A32(0))->options, 1); }
void mesh_GetDeclaration(void) {   /* (this, D3DVERTEXELEMENT9 decl[MAX_FVF_DECL_SIZE]) */
    mesh_state_t *s = MS(A32(0));
    uint32_t p = A32(1), hr = D3DERR_INVALIDCALL;
    if (p) { memcpy(hle_ptr(p), s->decl, (s->declCount + 1) * sizeof(D3DVERTEXELEMENT9)); hr = D3D_OK; }
    RET(hr, 2);
}
void mesh_GetDevice(void) {   /* (this, ppDevice) */
    mesh_state_t *s = MS(A32(0));
    uint32_t pp = A32(1), hr = D3DERR_INVALIDCALL;
    if (pp) { MEM32(pp) = s->dev; dev_call(s->dev, DEV_ADDREF, 1, (uint32_t[]){ s->dev }); hr = D3D_OK; }
    RET(hr, 2);
}

/* DrawSubset(AttribId): the faces with that attribute, as one triangle list. */
void mesh_DrawSubset(void) {
    mesh_state_t *s = MS(A32(0));
    uint32_t attrib = A32(1), hr = D3D_OK, count = 0;
    const uint32_t *at = (const uint32_t *)hle_ptr(s->attr);
    for (uint32_t f = 0; f < s->numFaces; f++) count += at[f] == attrib;
    if (count) {                                   /* an empty subset draws nothing */
        uint32_t isz = idx_size(s);
        void *tmp = hle_alloc(count * 3u * isz);
        if (!tmp) hr = E_OUTOFMEMORY_;
        else {
            uint32_t k = 0;
            for (uint32_t f = 0; f < s->numFaces; f++)
                if (at[f] == attrib) memcpy((char *)tmp + (size_t)k++ * 3u * isz, (const char *)hle_ptr(s->idx) + (size_t)f * 3u * isz, 3u * isz);
            if (s->fvf) hr = dev_call(s->dev, DEV_SETFVF, 2, (uint32_t[]){ s->dev, s->fvf });
            else {
                if (!s->declObj) {
                    void *dv = hle_alloc((s->declCount + 1) * sizeof(D3DVERTEXELEMENT9));
                    void *out = hle_alloc(4);
                    if (dv && out) {
                        memcpy(dv, s->decl, (s->declCount + 1) * sizeof(D3DVERTEXELEMENT9));
                        *(uint32_t *)out = 0;
                        hr = dev_call(s->dev, DEV_CREATEVERTEXDECL, 3, (uint32_t[]){ s->dev, hle_va(dv), hle_va(out) });
                        if (hr == D3D_OK) s->declObj = *(uint32_t *)out;
                    } else hr = E_OUTOFMEMORY_;
                    if (dv) hle_free(dv);
                    if (out) hle_free(out);
                }
                if (hr == D3D_OK) hr = dev_call(s->dev, DEV_SETVERTEXDECL, 2, (uint32_t[]){ s->dev, s->declObj });
            }
            if (hr == D3D_OK) {
                /* (this, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData,
                 *  IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride) */
                uint32_t a[9] = { s->dev, 4u /* D3DPT_TRIANGLELIST */, 0, s->numVerts, count, hle_va(tmp),
                                  isz == 4 ? 102u : 101u /* D3DFMT_INDEX32 / INDEX16 */, s->verts, s->stride };
                hr = dev_call(s->dev, DEV_DRAWINDEXEDPRIMUP, 9, a);
            }
            hle_free(tmp);
        }
    }
    RET(hr, 2);
}

void mesh_GetVertexBuffer(void) {   /* (this, ppVB) */
    if (A32(1)) MEM32(A32(1)) = 0;
    note("ID3DXMesh::GetVertexBuffer", "not implemented: the mesh keeps its vertices in system memory, use LockVertexBuffer");
    RET(D3DERR_NOTAVAILABLE, 2);
}
void mesh_GetIndexBuffer(void) {
    if (A32(1)) MEM32(A32(1)) = 0;
    note("ID3DXMesh::GetIndexBuffer", "not implemented: the mesh keeps its indices in system memory, use LockIndexBuffer");
    RET(D3DERR_NOTAVAILABLE, 2);
}

static uint32_t lock_buf(uint32_t *flag, uint32_t va, uint32_t pp) {   /* (Flags is ignored: system memory) */
    if (!pp) return D3DERR_INVALIDCALL;
    MEM32(pp) = 0;
    if (*flag) return D3DERR_INVALIDCALL;
    *flag = 1;
    MEM32(pp) = va;
    return D3D_OK;
}
static uint32_t unlock_buf(uint32_t *flag) {
    if (!*flag) return D3DERR_INVALIDCALL;
    *flag = 0;
    return D3D_OK;
}
void mesh_LockVertexBuffer(void)   { mesh_state_t *s = MS(A32(0)); RET(lock_buf(&s->vlock, s->verts, A32(2)), 3); }
void mesh_UnlockVertexBuffer(void) { RET(unlock_buf(&MS(A32(0))->vlock), 1); }
void mesh_LockIndexBuffer(void)    { mesh_state_t *s = MS(A32(0)); RET(lock_buf(&s->ilock, s->idx, A32(2)), 3); }
void mesh_UnlockIndexBuffer(void)  { RET(unlock_buf(&MS(A32(0))->ilock), 1); }
void mesh_LockAttributeBuffer(void)   { mesh_state_t *s = MS(A32(0)); RET(lock_buf(&s->alock, s->attr, A32(2)), 3); }
void mesh_UnlockAttributeBuffer(void) { RET(unlock_buf(&MS(A32(0))->alock), 1); }

/* GetAttributeTable(pAttribTable, pAttribTableSize): with a NULL table only the
 * count is returned; otherwise the buffer must hold the whole table. A mesh has
 * no table until Optimize(ATTRSORT) or SetAttributeTable makes one. */
void mesh_GetAttributeTable(void) {
    mesh_state_t *s = MS(A32(0));
    uint32_t pTab = A32(1), pSize = A32(2), hr = D3DERR_INVALIDCALL;
    if (pSize) {
        if (!pTab) { MEM32(pSize) = s->tableCount; hr = D3D_OK; }
        else if (MEM32(pSize) >= s->tableCount) {
            if (s->tableCount) memcpy(hle_ptr(pTab), hle_ptr(s->table), s->tableCount * sizeof(attr_range_t));
            MEM32(pSize) = s->tableCount;
            hr = D3D_OK;
        }
    }
    RET(hr, 3);
}

static uint32_t set_table(mesh_state_t *s, const attr_range_t *t, uint32_t n) {
    if (s->table) { hle_free(hle_ptr(s->table)); s->table = 0; }
    s->tableCount = 0;
    if (!n) return D3D_OK;
    void *g = hle_alloc(n * (uint32_t)sizeof *t);
    if (!g) return E_OUTOFMEMORY_;
    memcpy(g, t, n * sizeof *t);
    s->table = hle_va(g); s->tableCount = n;
    return D3D_OK;
}
void mesh_SetAttributeTable(void) {   /* (this, pAttribTable, cAttribTableSize) */
    mesh_state_t *s = MS(A32(0));
    uint32_t pTab = A32(1), n = A32(2), hr = D3DERR_INVALIDCALL;
    if (n == 0) hr = set_table(s, NULL, 0);
    else if (pTab && n <= 0x100000u) {
        const attr_range_t *t = (const attr_range_t *)hle_ptr(pTab);
        int good = 1;
        for (uint32_t i = 0; i < n; i++)
            if ((uint64_t)t[i].FaceStart + t[i].FaceCount > s->numFaces ||
                (uint64_t)t[i].VertexStart + t[i].VertexCount > s->numVerts) good = 0;
        if (good) hr = set_table(s, t, n);
    }
    RET(hr, 3);
}

void mesh_UpdateSemantics(void) {   /* (this, pDeclaration): same layout, new semantics */
    mesh_state_t *s = MS(A32(0));
    uint32_t pd = A32(1), hr = D3DERR_INVALIDCALL;
    if (pd) {
        const D3DVERTEXELEMENT9 *d = (const D3DVERTEXELEMENT9 *)hle_ptr(pd);
        uint32_t n = decl_count(d);
        int same = n == s->declCount && n;
        for (uint32_t i = 0; same && i < n; i++)
            same = d[i].Stream == s->decl[i].Stream && d[i].Offset == s->decl[i].Offset && d[i].Type == s->decl[i].Type;
        if (same) {
            memcpy(s->decl, d, (n + 1) * sizeof *d);
            s->fvf = fvf_from_decl(s->decl, n);
            if (s->declObj) { dev_call(s->declObj, 2, 1, (uint32_t[]){ s->declObj }); s->declObj = 0; }
            hr = D3D_OK;
        } else note("ID3DXMesh::UpdateSemantics", "the new declaration must keep every element's stream, offset and type");
    }
    RET(hr, 2);
}

/* ------------------------------------------------------------------ clones */

/* A copy of `src` with another declaration/options, vertices converted per
 * element (same usage and index; identical type, or FLOATn <-> FLOATm with the
 * missing components 0); elements the source lacks stay 0. */
static uint32_t clone_mesh(uint32_t src, uint32_t options, const D3DVERTEXELEMENT9 *decl, uint32_t dev,
                           uint32_t *out, const char *who) {
    mesh_state_t *s = MS(src);
    uint32_t hr, m;
    *out = 0;
    if (!dev) dev = s->dev;
    m = mesh_new(dev, options, decl, s->numFaces, s->numVerts, &hr, who);
    if (!m) return hr;
    mesh_state_t *d = MS(m);
    for (uint32_t i = 0; i < d->declCount; i++) {
        const D3DVERTEXELEMENT9 *de = &d->decl[i], *se = NULL;
        for (uint32_t k = 0; k < s->declCount; k++)
            if (s->decl[k].Usage == de->Usage && s->decl[k].UsageIndex == de->UsageIndex) { se = &s->decl[k]; break; }
        if (!se) continue;
        if (se->Type != de->Type && !(is_float(se->Type) && is_float(de->Type))) {
            note(who, "unsupported vertex element conversion");
            mesh_destroy(m);
            return D3DERR_INVALIDCALL;
        }
        uint32_t ssz = decl_type_size(se->Type), dsz = decl_type_size(de->Type), cp = ssz < dsz ? ssz : dsz;
        for (uint32_t v = 0; v < s->numVerts; v++)
            memcpy((char *)hle_ptr(d->verts) + (size_t)v * d->stride + de->Offset,
                   (const char *)hle_ptr(s->verts) + (size_t)v * s->stride + se->Offset, cp);
    }
    for (uint32_t i = 0; i < s->numFaces * 3u; i++) put_idx(d, i, get_idx(s, i));
    memcpy(hle_ptr(d->attr), hle_ptr(s->attr), (size_t)s->numFaces * 4);
    if (s->tableCount) set_table(d, (const attr_range_t *)hle_ptr(s->table), s->tableCount);
    *out = m;
    return D3D_OK;
}

void mesh_CloneMeshFVF(void) {   /* (this, Options, FVF, pDevice, ppCloneMesh) */
    uint32_t pp = A32(4), hr = D3DERR_INVALIDCALL, m = 0;
    D3DVERTEXELEMENT9 d[D3DX9_MAX_FVF_DECL_SIZE];
    if (pp) {
        MEM32(pp) = 0;
        if (!d3dx9_decl_from_fvf(A32(2), d)) note("ID3DXMesh::CloneMeshFVF", "FVF not supported");
        else { hr = clone_mesh(A32(0), A32(1), d, A32(3), &m, "ID3DXMesh::CloneMeshFVF"); MEM32(pp) = m; }
    }
    RET(hr, 5);
}
void mesh_CloneMesh(void) {   /* (this, Options, pDeclaration, pDevice, ppCloneMesh) */
    uint32_t pp = A32(4), pd = A32(2), hr = D3DERR_INVALIDCALL, m = 0;
    if (pp) {
        MEM32(pp) = 0;
        if (pd) { hr = clone_mesh(A32(0), A32(1), (const D3DVERTEXELEMENT9 *)hle_ptr(pd), A32(3), &m, "ID3DXMesh::CloneMesh"); MEM32(pp) = m; }
    }
    RET(hr, 5);
}

/* ---------------------------------------------------------------- Optimize */

typedef struct { uint32_t f, key; } face_key_t;
static int face_cmp(const void *x, const void *y) {
    const face_key_t *p = x, *q = y;
    if (p->key != q->key) return p->key < q->key ? -1 : 1;
    return p->f < q->f ? -1 : p->f > q->f;
}

/* Build the optimized mesh. On success *out is a new mesh, fremap[new] = old
 * face and vremap[new] = old vertex (malloc'ed, caller frees). */
static uint32_t optimize_build(mesh_state_t *s, uint32_t opt, uint32_t newOptions, uint32_t *out,
                               uint32_t **fremap, uint32_t **vremap, const char *who) {
    uint32_t nf = s->numFaces, nv = s->numVerts, hr = D3DERR_INVALIDCALL, kept = 0;
    const uint32_t *at = (const uint32_t *)hle_ptr(s->attr);
    face_key_t *ord = (face_key_t *)malloc((size_t)nf * sizeof *ord);
    uint32_t *vmap = (uint32_t *)malloc((size_t)nv * 4), *fr = NULL, *vr = NULL;
    *out = 0;
    if (!ord || !vmap) { hr = E_OUTOFMEMORY_; goto done; }
    if (!faces_valid(s)) goto done;
    for (uint32_t f = 0; f < nf; f++) {
        if ((opt & MOPT_COMPACT) && at[f] == MESH_UNUSED) continue;     /* unused faces are dropped */
        ord[kept].f = f; ord[kept].key = (opt & MOPT_ATTRSORT) ? at[f] : 0; kept++;
    }
    if (opt & MOPT_ATTRSORT) qsort(ord, kept, sizeof *ord, face_cmp);
    uint32_t nnv = nv;
    for (uint32_t v = 0; v < nv; v++) vmap[v] = MESH_UNUSED;
    if ((opt & MOPT_COMPACT) && !(opt & MOPT_IGNOREVERTS)) {              /* renumber by first use, drop unused */
        nnv = 0;
        for (uint32_t k = 0; k < kept; k++)
            for (uint32_t e = 0; e < 3; e++) {
                uint32_t v = get_idx(s, ord[k].f * 3 + e);
                if (vmap[v] == MESH_UNUSED) vmap[v] = nnv++;
            }
    } else for (uint32_t v = 0; v < nv; v++) vmap[v] = v;
    if (!kept || !nnv) goto done;                                          /* nothing left: invalid */
    uint32_t m = 0;
    {
        uint32_t h2;
        m = mesh_new(s->dev, newOptions, s->decl, kept, nnv, &h2, who);
        if (!m) { hr = h2; goto done; }
    }
    mesh_state_t *d = MS(m);
    fr = (uint32_t *)malloc((size_t)kept * 4); vr = (uint32_t *)malloc((size_t)nnv * 4);
    if (!fr || !vr) { mesh_destroy(m); hr = E_OUTOFMEMORY_; goto done; }
    for (uint32_t v = 0; v < nv; v++)
        if (vmap[v] != MESH_UNUSED) {
            memcpy((char *)hle_ptr(d->verts) + (size_t)vmap[v] * d->stride, (const char *)hle_ptr(s->verts) + (size_t)v * s->stride, s->stride);
            vr[vmap[v]] = v;
        }
    for (uint32_t k = 0; k < kept; k++) {
        fr[k] = ord[k].f;
        for (uint32_t e = 0; e < 3; e++) put_idx(d, k * 3 + e, vmap[get_idx(s, ord[k].f * 3 + e)]);
        ((uint32_t *)hle_ptr(d->attr))[k] = at[ord[k].f];
    }
    if (opt & MOPT_ATTRSORT) {                                             /* the attribute table */
        attr_range_t *t = (attr_range_t *)malloc((size_t)kept * sizeof *t);
        uint32_t nt = 0;
        if (t) {
            for (uint32_t k = 0; k < kept;) {
                uint32_t id = ((uint32_t *)hle_ptr(d->attr))[k], e = k, lo = 0xFFFFFFFFu, hi = 0;
                for (; e < kept && ((uint32_t *)hle_ptr(d->attr))[e] == id; e++)
                    for (uint32_t c = 0; c < 3; c++) {
                        uint32_t v = get_idx(d, e * 3 + c);
                        if (v < lo) lo = v;
                        if (v > hi) hi = v;
                    }
                t[nt].AttribId = id; t[nt].FaceStart = k; t[nt].FaceCount = e - k;
                t[nt].VertexStart = lo; t[nt].VertexCount = hi - lo + 1;
                nt++; k = e;
            }
            set_table(d, t, nt);
            free(t);
        }
    }
    *out = m; *fremap = fr; *vremap = vr; fr = vr = NULL;
    hr = D3D_OK;
done:
    free(ord); free(vmap); free(fr); free(vr);
    return hr;
}

/* Optimize(Flags, pAdjacencyIn, pAdjacencyOut, pFaceRemap, ppVertexRemap, ppOptMesh)
 * OptimizeInplace(Flags, pAdjacencyIn, pAdjacencyOut, pFaceRemap, ppVertexRemap)
 * pFaceRemap[new face] = old face; the vertex remap buffer likewise new -> old. */
static void optimize_common(int inplace) {
    uint32_t self = A32(0), flags = A32(1), adjIn = A32(2), adjOut = A32(3), pFace = A32(4), ppV = A32(5);
    uint32_t ppMesh = inplace ? 0 : A32(6), argc = inplace ? 6 : 7, hr = D3DERR_INVALIDCALL;
    const char *who = inplace ? "ID3DXMesh::OptimizeInplace" : "ID3DXMesh::Optimize";
    mesh_state_t *s = MS(self);
    uint32_t opt = flags & MOPT_MASK, mopts = flags & OPT_MESH_BITS;
    uint32_t *fr = NULL, *vr = NULL, m = 0;
    if (flags & ~(MOPT_MASK | OPT_MESH_BITS)) goto out;
    if ((opt & MOPT_VERTEXCACHE) || (opt & MOPT_STRIPREORDER) || (opt & MOPT_DEVICEINDEPENDENT)) {
        note(who, "D3DXMESHOPT_VERTEXCACHE / _STRIPREORDER / _DEVICEINDEPENDENT are not supported");
        hr = D3DERR_NOTAVAILABLE; goto out;
    }
    if (!(opt & (MOPT_COMPACT | MOPT_ATTRSORT))) { note(who, "needs D3DXMESHOPT_COMPACT and/or _ATTRSORT"); goto out; }
    if (adjOut && !adjIn) goto out;
    if (!inplace && !ppMesh) goto out;
    if (inplace && mopts) goto out;
    if (s->vlock || s->ilock || s->alock) goto out;
    {
        uint32_t no = mopts ? mopts : s->options;
        if (!inplace && (hr = check_options(no, who)) != 0) goto out;
        hr = D3DERR_INVALIDCALL;
        if (adjIn) for (uint32_t i = 0; i < s->numFaces * 3u; i++) {
            uint32_t a = ((const uint32_t *)hle_ptr(adjIn))[i];
            if (a != MESH_UNUSED && a >= s->numFaces) goto out;
        }
        /* In place the index width stays; Optimize may switch to the width in Flags. */
        hr = optimize_build(s, opt, inplace ? s->options : no, &m, &fr, &vr, who);
    }
    if (hr != D3D_OK) goto out;
    {
        mesh_state_t *d = MS(m);
        if (adjOut) {                                           /* neighbours, renumbered */
            uint32_t *o2n = (uint32_t *)malloc((size_t)s->numFaces * 4);
            if (!o2n) { hr = E_OUTOFMEMORY_; mesh_destroy(m); goto out; }
            for (uint32_t f = 0; f < s->numFaces; f++) o2n[f] = MESH_UNUSED;
            for (uint32_t k = 0; k < d->numFaces; k++) o2n[fr[k]] = k;
            for (uint32_t k = 0; k < d->numFaces; k++)
                for (uint32_t e = 0; e < 3; e++) {
                    uint32_t a = ((const uint32_t *)hle_ptr(adjIn))[fr[k] * 3 + e];
                    ((uint32_t *)hle_ptr(adjOut))[k * 3 + e] = a == MESH_UNUSED ? MESH_UNUSED : o2n[a];
                }
            free(o2n);
        }
        if (pFace) memcpy(hle_ptr(pFace), fr, (size_t)d->numFaces * 4);
        if (ppV) MEM32(ppV) = buf_new(vr, d->numVerts * 4u);
        if (inplace) {                                          /* take the new data, free the old with the shell */
            uint32_t t;
            t = s->verts; s->verts = d->verts; d->verts = t;
            t = s->idx; s->idx = d->idx; d->idx = t;
            t = s->attr; s->attr = d->attr; d->attr = t;
            t = s->table; s->table = d->table; d->table = t;
            t = s->tableCount; s->tableCount = d->tableCount; d->tableCount = t;
            t = s->numVerts; s->numVerts = d->numVerts; d->numVerts = t;
            t = s->numFaces; s->numFaces = d->numFaces; d->numFaces = t;
            mesh_destroy(m);
        } else MEM32(ppMesh) = m;
    }
out:
    free(fr); free(vr);
    if (!inplace && ppMesh && hr != D3D_OK) MEM32(ppMesh) = 0;
    RET(hr, argc);
}
void mesh_Optimize(void)        { optimize_common(0); }
void mesh_OptimizeInplace(void) { optimize_common(1); }

/* The vtable: public d3dx9mesh.h order, ID3DXBaseMesh then ID3DXMesh. */
static const win32hle_shim mesh_methods[] = {
    { "ID3DXMesh::QueryInterface", d3d_QueryInterface },
    { "ID3DXMesh::AddRef", mesh_AddRef },
    { "ID3DXMesh::Release", mesh_Release },
    { "ID3DXMesh::DrawSubset", mesh_DrawSubset },
    { "ID3DXMesh::GetNumFaces", mesh_GetNumFaces },
    { "ID3DXMesh::GetNumVertices", mesh_GetNumVertices },
    { "ID3DXMesh::GetFVF", mesh_GetFVF },
    { "ID3DXMesh::GetDeclaration", mesh_GetDeclaration },
    { "ID3DXMesh::GetNumBytesPerVertex", mesh_GetNumBytesPerVertex },
    { "ID3DXMesh::GetOptions", mesh_GetOptions },
    { "ID3DXMesh::GetDevice", mesh_GetDevice },
    { "ID3DXMesh::CloneMeshFVF", mesh_CloneMeshFVF },
    { "ID3DXMesh::CloneMesh", mesh_CloneMesh },
    { "ID3DXMesh::GetVertexBuffer", mesh_GetVertexBuffer },
    { "ID3DXMesh::GetIndexBuffer", mesh_GetIndexBuffer },
    { "ID3DXMesh::LockVertexBuffer", mesh_LockVertexBuffer },
    { "ID3DXMesh::UnlockVertexBuffer", mesh_UnlockVertexBuffer },
    { "ID3DXMesh::LockIndexBuffer", mesh_LockIndexBuffer },
    { "ID3DXMesh::UnlockIndexBuffer", mesh_UnlockIndexBuffer },
    { "ID3DXMesh::GetAttributeTable", mesh_GetAttributeTable },
    { "ID3DXMesh::ConvertPointRepsToAdjacency", mesh_ConvertPointRepsToAdjacency },
    { "ID3DXMesh::ConvertAdjacencyToPointReps", mesh_ConvertAdjacencyToPointReps },
    { "ID3DXMesh::GenerateAdjacency", mesh_GenerateAdjacency },
    { "ID3DXMesh::UpdateSemantics", mesh_UpdateSemantics },
    { "ID3DXMesh::LockAttributeBuffer", mesh_LockAttributeBuffer },
    { "ID3DXMesh::UnlockAttributeBuffer", mesh_UnlockAttributeBuffer },
    { "ID3DXMesh::Optimize", mesh_Optimize },
    { "ID3DXMesh::OptimizeInplace", mesh_OptimizeInplace },
    { "ID3DXMesh::SetAttributeTable", mesh_SetAttributeTable },
    { 0, 0 }
};
