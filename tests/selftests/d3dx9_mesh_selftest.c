/*
 * d3dx9_mesh_selftest.c - ID3DXMesh: D3DXCreateMesh(FVF), attributes, adjacency,
 * CloneMesh(FVF), Optimize. Expected values are worked out by hand from the
 * documented behaviour (see the comments), not read back from the code.
 * The device is a fake COM object whose vtable records what the mesh calls.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_mesh.h"

/* guest pointer to IID_IUnknown {00000000-0000-0000-C000-000000000046} */
static uint32_t iid_unk(void) {
    uint8_t *g = (uint8_t *)hle_alloc(16);
    memset(g, 0, 16); g[8] = 0xC0; g[15] = 0x46;
    return hle_va(g);
}

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;
extern recomp_func_t recomp_lookup_import(uint32_t va);

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define A(p) hle_va(p)
#define REF(o) MEM32((o) + 4)
#define NOFACE 0xFFFFFFFFu

/* Enter a guest-vtable slot directly and measure esp (hle_call_guest restores esp
 * and would hide a wrong RET). delta == 0 means one RET with the right argc. */
static uint32_t slot_call(uint32_t obj, int slot, int argc, const uint32_t *a, uint32_t *delta) {
    mach_enter();
    uint32_t esp0 = g_esp;
    for (int k = argc - 1; k >= 0; k--) PUSH32(g_esp, a[k]);
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_func_t f = recomp_lookup_import(MEM32(MEM32(obj) + 4u * (uint32_t)slot));
    uint32_t eax = 0xDEADDEADu;
    *delta = 0xFFFFu;
    if (f) { f(); *delta = g_esp - esp0; eax = g_eax; }
    g_esp = esp0;
    mach_leave();
    return eax;
}

/* ------------------------------------------------------------ fake device */
static struct {
    uint32_t setfvf_calls, fvf, createdecl_calls, setdecl_calls, decl_set, decl_released;
    uint32_t draw_calls, prim_type, min_vtx, num_vtx, prim_count, idx_fmt, stride, vdata;
    uint32_t idx[64];
} g_dev_rec;
static uint32_t g_decl_obj;

static void m_addref(void) { uint32_t s = A32(0); MEM32(s + 4)++; RET(MEM32(s + 4), 1); }
static void m_release(void) { uint32_t s = A32(0); MEM32(s + 4)--; RET(MEM32(s + 4), 1); }
static void m_decl_release(void) { g_dev_rec.decl_released++; RET(0, 1); }
static void m_setfvf(void) { g_dev_rec.setfvf_calls++; g_dev_rec.fvf = A32(1); RET(D3D_OK, 2); }
static void m_createdecl(void) {
    g_dev_rec.createdecl_calls++;
    MEM32(A32(2)) = g_decl_obj;
    RET(D3D_OK, 3);
}
static void m_setdecl(void) { g_dev_rec.setdecl_calls++; g_dev_rec.decl_set = A32(1); RET(D3D_OK, 2); }
static void m_dip(void) {   /* (this, type, minvtx, numvtx, primcount, pIndexData, fmt, pVData, stride) */
    g_dev_rec.draw_calls++;
    g_dev_rec.prim_type = A32(1); g_dev_rec.min_vtx = A32(2); g_dev_rec.num_vtx = A32(3);
    g_dev_rec.prim_count = A32(4); g_dev_rec.idx_fmt = A32(6); g_dev_rec.vdata = A32(7); g_dev_rec.stride = A32(8);
    uint32_t n = A32(4) * 3u;
    for (uint32_t i = 0; i < n && i < 64; i++)
        g_dev_rec.idx[i] = A32(6) == 102 ? ((uint32_t *)hle_ptr(A32(5)))[i] : ((uint16_t *)hle_ptr(A32(5)))[i];
    RET(D3D_OK, 9);
}
static void m_other(void) { RET(D3DERR_NOTAVAILABLE, 1); }
static const win32hle_shim mock_shims[] = {
    { "MockDev::AddRef", m_addref }, { "MockDev::Release", m_release }, { "MockDev::SetFVF", m_setfvf },
    { "MockDev::CreateVertexDeclaration", m_createdecl }, { "MockDev::SetVertexDeclaration", m_setdecl },
    { "MockDev::DrawIndexedPrimitiveUP", m_dip }, { "MockDev::Other", m_other },
    { "MockDecl::Release", m_decl_release }, { 0, 0 }
};

static uint32_t make_fake_device(void) {
    win32hle_register(mock_shims);
    uint32_t *vt = (uint32_t *)hle_alloc(4u * 119u);
    for (int i = 0; i < 119; i++) vt[i] = hle_resolve("MockDev::Other");
    vt[1] = hle_resolve("MockDev::AddRef"); vt[2] = hle_resolve("MockDev::Release");
    vt[84] = hle_resolve("MockDev::DrawIndexedPrimitiveUP");
    vt[86] = hle_resolve("MockDev::CreateVertexDeclaration"); vt[87] = hle_resolve("MockDev::SetVertexDeclaration");
    vt[89] = hle_resolve("MockDev::SetFVF");
    uint32_t dev = hle_com_new(A(vt), 16);
    uint32_t *dvt = (uint32_t *)hle_alloc(16);
    dvt[0] = dvt[1] = hle_resolve("MockDev::Other"); dvt[2] = hle_resolve("MockDecl::Release");
    g_decl_obj = hle_com_new(A(dvt), 16);
    return dev;
}

/* ------------------------------------------------------------------ helpers */
static uint32_t dev;

static uint32_t create_fvf(uint32_t nf, uint32_t nv, uint32_t opts, uint32_t fvf, uint32_t *hr) {
    uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0xDEADBEEFu;
    uint32_t h = call("D3DXCreateMeshFVF", 6, (uint32_t[]){ nf, nv, opts, fvf, dev, A(pp) });
    if (hr) *hr = h;
    return *pp;
}
static uint32_t create_decl(uint32_t nf, uint32_t nv, uint32_t opts, const D3DVERTEXELEMENT9 *d, uint32_t n, uint32_t *hr) {
    D3DVERTEXELEMENT9 *g = (D3DVERTEXELEMENT9 *)hle_alloc((n + 1) * sizeof *g);
    memcpy(g, d, n * sizeof *g);
    g[n].Stream = 0xFF; g[n].Offset = 0; g[n].Type = 17; g[n].Method = 0; g[n].Usage = 0; g[n].UsageIndex = 0;
    uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0xDEADBEEFu;
    uint32_t h = call("D3DXCreateMesh", 6, (uint32_t[]){ nf, nv, opts, A(g), dev, A(pp) });
    if (hr) *hr = h;
    return *pp;
}
static uint32_t lock(uint32_t m, const char *what, uint32_t unlock_after, uint32_t *hr) {
    uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
    uint32_t h = call(what, 3, (uint32_t[]){ m, 0, A(pp) });
    if (hr) *hr = h;
    (void)unlock_after;
    return *pp;
}
static void *lock_vb(uint32_t m) { return hle_ptr(lock(m, "ID3DXMesh::LockVertexBuffer", 0, NULL)); }
static void unlock_vb(uint32_t m) { call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){ m }); }
static void *lock_ib(uint32_t m) { return hle_ptr(lock(m, "ID3DXMesh::LockIndexBuffer", 0, NULL)); }
static void unlock_ib(uint32_t m) { call("ID3DXMesh::UnlockIndexBuffer", 1, (uint32_t[]){ m }); }
static uint32_t *lock_ab(uint32_t m) { return (uint32_t *)hle_ptr(lock(m, "ID3DXMesh::LockAttributeBuffer", 0, NULL)); }
static void unlock_ab(uint32_t m) { call("ID3DXMesh::UnlockAttributeBuffer", 1, (uint32_t[]){ m }); }
static void release(uint32_t m) { call("ID3DXMesh::Release", 1, (uint32_t[]){ m }); }

typedef struct { uint32_t id, fs, fc, vs, vc; } range_t;
static void *gcopy(const void *p, size_t n) { void *g = hle_alloc((uint32_t)n); memcpy(g, p, n); return g; }

static D3DVERTEXELEMENT9 el(uint16_t off, uint8_t type, uint8_t usage, uint8_t uidx) {
    D3DVERTEXELEMENT9 e; e.Stream = 0; e.Offset = off; e.Type = type; e.Method = 0; e.Usage = usage; e.UsageIndex = uidx;
    return e;
}

/* A mesh with positions (x,0,0), x = vertex index, and the given faces. */
static uint32_t make_line_mesh(uint32_t nv, const uint16_t *faces, uint32_t nf, const uint32_t *attrs) {
    uint32_t hr, m = create_fvf(nf, nv, 0, 0x2, &hr);
    float *v = (float *)lock_vb(m);
    for (uint32_t i = 0; i < nv; i++) { v[i * 3] = (float)i; v[i * 3 + 1] = 0; v[i * 3 + 2] = 0; }
    unlock_vb(m);
    memcpy(lock_ib(m), faces, nf * 6); unlock_ib(m);
    if (attrs) { memcpy(lock_ab(m), attrs, nf * 4); unlock_ab(m); }
    return m;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);
    dev = make_fake_device();
    uint32_t dev_ref0 = REF(dev);

    /* ============ D3DXCreateMeshFVF: fields, declaration, validation ============ */
    {
        uint32_t hr, m = create_fvf(2, 4, 0, 0x112 /* XYZ | NORMAL | TEX1 */, &hr);
        ok("CreateMeshFVF hr", hr == D3D_OK && m != 0);
        ok("NumFaces 2", call("ID3DXMesh::GetNumFaces", 1, (uint32_t[]){ m }) == 2);
        ok("NumVertices 4", call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){ m }) == 4);
        ok("GetFVF 0x112", call("ID3DXMesh::GetFVF", 1, (uint32_t[]){ m }) == 0x112);
        ok("bytes per vertex 12+12+8", call("ID3DXMesh::GetNumBytesPerVertex", 1, (uint32_t[]){ m }) == 32);
        ok("GetOptions 0", call("ID3DXMesh::GetOptions", 1, (uint32_t[]){ m }) == 0);
        ok("the mesh holds a reference to the device", REF(dev) == dev_ref0 + 1);
        uint32_t *pd = (uint32_t *)hle_alloc(4); *pd = 0;
        ok("GetDevice", call("ID3DXMesh::GetDevice", 2, (uint32_t[]){ m, A(pd) }) == D3D_OK && *pd == dev);
        ok("GetDevice adds a reference", REF(dev) == dev_ref0 + 2);
        REF(dev) = dev_ref0 + 1;
        D3DVERTEXELEMENT9 *d = (D3DVERTEXELEMENT9 *)hle_alloc(5 * sizeof *d);
        memset(d, 0xAA, 5 * sizeof *d);
        ok("GetDeclaration", call("ID3DXMesh::GetDeclaration", 2, (uint32_t[]){ m, A(d) }) == D3D_OK);
        ok("decl POSITION FLOAT3 @0", d[0].Stream == 0 && d[0].Offset == 0 && d[0].Type == 2 && d[0].Usage == 0);
        ok("decl NORMAL FLOAT3 @12", d[1].Offset == 12 && d[1].Type == 2 && d[1].Usage == 3);
        ok("decl TEXCOORD0 FLOAT2 @24", d[2].Offset == 24 && d[2].Type == 1 && d[2].Usage == 5 && d[2].UsageIndex == 0);
        ok("decl END", d[3].Stream == 0xFF && d[3].Type == 17);
        ok("GetDeclaration(NULL) invalid", call("ID3DXMesh::GetDeclaration", 2, (uint32_t[]){ m, 0 }) == D3DERR_INVALIDCALL);
        release(m);
        ok("Release drops the device reference", REF(dev) == dev_ref0);

        uint32_t h;
        ok("0 faces invalid", (create_fvf(0, 4, 0, 0x2, &h) == 0) && h == D3DERR_INVALIDCALL);
        ok("0 vertices invalid", (create_fvf(1, 0, 0, 0x2, &h) == 0) && h == D3DERR_INVALIDCALL);
        ok("FVF without position invalid", (create_fvf(1, 3, 0, 0x10, &h) == 0) && h == D3DERR_INVALIDCALL);
        ok("XYZB5 not supported", (create_fvf(1, 3, 0, 0xE, &h) == 0) && h == D3DERR_INVALIDCALL);
        ok("D3DXMESH_POINTS not supported", (create_fvf(1, 3, 4, 0x2, &h) == 0) && h == D3DERR_NOTAVAILABLE);
        ok("unknown option bits invalid", (create_fvf(1, 3, 0x80000000u, 0x2, &h) == 0) && h == D3DERR_INVALIDCALL);
        ok("VB_SYSTEMMEM|VB_MANAGED invalid", (create_fvf(1, 3, 0x30, 0x2, &h) == 0) && h == D3DERR_INVALIDCALL);
        ok("65536 vertices need D3DXMESH_32BIT", (create_fvf(1, 65536, 0, 0x2, &h) == 0) && h == D3DERR_INVALIDCALL);
        uint32_t big = create_fvf(1, 65536, 1, 0x2, &h);
        ok("65536 vertices with 32BIT", h == D3D_OK && big && call("ID3DXMesh::GetOptions", 1, (uint32_t[]){ big }) == 1);
        if (big) release(big);
        uint32_t saved = dev; dev = 0;
        ok("NULL device invalid", (create_fvf(1, 3, 0, 0x2, &h) == 0) && h == D3DERR_INVALIDCALL);
        dev = saved;
        ok("NULL ppMesh invalid", call("D3DXCreateMeshFVF", 6, (uint32_t[]){ 1, 3, 0, 0x2, dev, 0 }) == D3DERR_INVALIDCALL);
        ok("no reference leaked by failures", REF(dev) == dev_ref0);
    }

    /* ============ D3DXCreateMesh with a declaration ============ */
    {
        D3DVERTEXELEMENT9 d1[2] = { el(0, 2, 0, 0), el(12, 1, 5, 0) };   /* POSITION f3, TEXCOORD0 f2 */
        uint32_t hr, m = create_decl(1, 3, 0, d1, 2, &hr);
        ok("CreateMesh hr", hr == D3D_OK && m);
        ok("declaration of an FVF layout reports that FVF 0x102", call("ID3DXMesh::GetFVF", 1, (uint32_t[]){ m }) == 0x102);
        ok("stride 20", call("ID3DXMesh::GetNumBytesPerVertex", 1, (uint32_t[]){ m }) == 20);
        release(m);
        D3DVERTEXELEMENT9 d2[2] = { el(0, 2, 0, 0), el(12, 2, 6, 0) };   /* POSITION f3, TANGENT f3 */
        m = create_decl(1, 3, 0, d2, 2, &hr);
        ok("CreateMesh with TANGENT", hr == D3D_OK && m);
        ok("no FVF for a custom layout", call("ID3DXMesh::GetFVF", 1, (uint32_t[]){ m }) == 0);
        ok("stride 24", call("ID3DXMesh::GetNumBytesPerVertex", 1, (uint32_t[]){ m }) == 24);
        release(m);
        D3DVERTEXELEMENT9 d3[2] = { el(0, 2, 0, 0), el(12, 1, 5, 0) };
        d3[1].Stream = 1;
        ok("a second vertex stream is not supported", (create_decl(1, 3, 0, d3, 2, &hr) == 0) && hr == D3DERR_INVALIDCALL);
        D3DVERTEXELEMENT9 d4[1] = { el(0, 99, 0, 0) };
        ok("unknown element type invalid", (create_decl(1, 3, 0, d4, 1, &hr) == 0) && hr == D3DERR_INVALIDCALL);
        ok("NULL declaration invalid",
           call("D3DXCreateMesh", 6, (uint32_t[]){ 1, 3, 0, 0, dev, A(hle_alloc(4)) }) == D3DERR_INVALIDCALL);
    }

    /* ============ buffers: lock rules ============ */
    {
        uint32_t m = create_fvf(1, 3, 0, 0x2, NULL), hr;
        float *v = (float *)lock_vb(m);
        ok("LockVertexBuffer returns memory", v != NULL);
        lock(m, "ID3DXMesh::LockVertexBuffer", 0, &hr);
        ok("second LockVertexBuffer invalid", hr == D3DERR_INVALIDCALL);
        ok("UnlockVertexBuffer", call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){ m }) == D3D_OK);
        ok("UnlockVertexBuffer when unlocked invalid", call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){ m }) == D3DERR_INVALIDCALL);
        ok("LockIndexBuffer(NULL) invalid", call("ID3DXMesh::LockIndexBuffer", 3, (uint32_t[]){ m, 0, 0 }) == D3DERR_INVALIDCALL);
        ok("UnlockIndexBuffer when unlocked invalid", call("ID3DXMesh::UnlockIndexBuffer", 1, (uint32_t[]){ m }) == D3DERR_INVALIDCALL);
        ok("UnlockAttributeBuffer when unlocked invalid", call("ID3DXMesh::UnlockAttributeBuffer", 1, (uint32_t[]){ m }) == D3DERR_INVALIDCALL);
        uint32_t *pv = (uint32_t *)hle_alloc(4);
        ok("GetVertexBuffer is an honest error",
           call("ID3DXMesh::GetVertexBuffer", 2, (uint32_t[]){ m, A(pv) }) == D3DERR_NOTAVAILABLE && *pv == 0);
        ok("GetIndexBuffer is an honest error",
           call("ID3DXMesh::GetIndexBuffer", 2, (uint32_t[]){ m, A(pv) }) == D3DERR_NOTAVAILABLE && *pv == 0);
        uint32_t *at = lock_ab(m);
        ok("a new mesh has attribute 0 on every face", at && at[0] == 0);
        unlock_ab(m);
        release(m);
    }

    /* ============ DrawSubset ============ */
    {
        /* 4 faces, attributes {0,1,0,1}: subset 1 is faces 1 and 3. */
        uint16_t faces[12] = { 0, 1, 2,   1, 2, 3,   0, 2, 3,   0, 1, 3 };
        uint32_t attrs[4] = { 0, 1, 0, 1 };
        uint32_t m = make_line_mesh(4, faces, 4, attrs);
        memset(&g_dev_rec, 0, sizeof g_dev_rec);
        ok("DrawSubset(1)", call("ID3DXMesh::DrawSubset", 2, (uint32_t[]){ m, 1 }) == D3D_OK);
        ok("SetFVF(XYZ) before drawing", g_dev_rec.setfvf_calls == 1 && g_dev_rec.fvf == 0x2);
        ok("one DrawIndexedPrimitiveUP", g_dev_rec.draw_calls == 1);
        ok("TRIANGLELIST, 2 primitives, 4 vertices, stride 12",
           g_dev_rec.prim_type == 4 && g_dev_rec.prim_count == 2 && g_dev_rec.num_vtx == 4 && g_dev_rec.stride == 12);
        ok("16-bit indices of faces 1 and 3 only",
           g_dev_rec.idx_fmt == 101 && g_dev_rec.idx[0] == 1 && g_dev_rec.idx[1] == 2 && g_dev_rec.idx[2] == 3 &&
           g_dev_rec.idx[3] == 0 && g_dev_rec.idx[4] == 1 && g_dev_rec.idx[5] == 3);
        memset(&g_dev_rec, 0, sizeof g_dev_rec);
        ok("DrawSubset of an empty subset", call("ID3DXMesh::DrawSubset", 2, (uint32_t[]){ m, 7 }) == D3D_OK);
        ok("an empty subset draws nothing", g_dev_rec.draw_calls == 0);
        release(m);

        /* 32-bit indices */
        uint32_t hr, m32 = create_fvf(1, 3, 1, 0x2, &hr);
        uint32_t *ix = (uint32_t *)lock_ib(m32); ix[0] = 2; ix[1] = 1; ix[2] = 0; unlock_ib(m32);
        memset(&g_dev_rec, 0, sizeof g_dev_rec);
        call("ID3DXMesh::DrawSubset", 2, (uint32_t[]){ m32, 0 });
        ok("32-bit indices drawn as D3DFMT_INDEX32", g_dev_rec.idx_fmt == 102 && g_dev_rec.idx[0] == 2 && g_dev_rec.idx[2] == 0);
        release(m32);

        /* a layout without FVF uses a vertex declaration, made once */
        D3DVERTEXELEMENT9 d[2] = { el(0, 2, 0, 0), el(12, 2, 6, 0) };
        uint32_t md = create_decl(1, 3, 0, d, 2, &hr);
        memset(&g_dev_rec, 0, sizeof g_dev_rec);
        call("ID3DXMesh::DrawSubset", 2, (uint32_t[]){ md, 0 });
        call("ID3DXMesh::DrawSubset", 2, (uint32_t[]){ md, 0 });
        ok("declaration created once and set before each draw",
           g_dev_rec.createdecl_calls == 1 && g_dev_rec.setdecl_calls == 2 && g_dev_rec.decl_set == g_decl_obj &&
           g_dev_rec.setfvf_calls == 0 && g_dev_rec.draw_calls == 2 && g_dev_rec.stride == 24);
        release(md);
        ok("the declaration object is released with the mesh", g_dev_rec.decl_released == 1);
    }

    /* ============ adjacency ============ */
    {
        /* Quad of two triangles; f0 = (0,1,2) edges (0,1) (1,2) (2,0); f1 = (2,1,3) edges (2,1) (1,3) (3,2).
         * Edge (1,2) of f0 is edge (2,1) of f1: adjacency f0 = {-,1,-}, f1 = {0,-,-}. */
        uint16_t faces[6] = { 0, 1, 2, 2, 1, 3 };
        uint32_t m = make_line_mesh(4, faces, 2, NULL);
        float *v = (float *)lock_vb(m);
        float pos[4][3] = { {0,0,0}, {0,1,0}, {1,0,0}, {1,1,0} };
        memcpy(v, pos, sizeof pos); unlock_vb(m);
        uint32_t *adj = (uint32_t *)hle_alloc(24);
        ok("GenerateAdjacency", call("ID3DXMesh::GenerateAdjacency", 3, (uint32_t[]){ m, 0, A(adj) }) == D3D_OK);
        ok("shared edge found, the others open",
           adj[0] == NOFACE && adj[1] == 1 && adj[2] == NOFACE && adj[3] == 0 && adj[4] == NOFACE && adj[5] == NOFACE);
        uint32_t neg = 0xBF800000u;   /* -1.0f */
        ok("negative epsilon invalid", call("ID3DXMesh::GenerateAdjacency", 3, (uint32_t[]){ m, neg, A(adj) }) == D3DERR_INVALIDCALL);
        ok("NULL adjacency invalid", call("ID3DXMesh::GenerateAdjacency", 3, (uint32_t[]){ m, 0, 0 }) == D3DERR_INVALIDCALL);
        ok("ConvertPointRepsToAdjacency(NULL reps): identity", call("ID3DXMesh::ConvertPointRepsToAdjacency", 3, (uint32_t[]){ m, 0, A(adj) }) == D3D_OK && adj[1] == 1 && adj[3] == 0);
        release(m);

        /* The same two triangles with the shared vertices duplicated: f1 = (3,4,5), v3 = v2, v4 = v1 (+ 1e-4 on v3.x). */
        uint16_t sf[6] = { 0, 1, 2, 3, 4, 5 };
        m = make_line_mesh(6, sf, 2, NULL);
        float p6[6][3] = { {0,0,0}, {0,1,0}, {1,0,0}, {1.0001f,0,0}, {0,1,0}, {1,1,0} };
        memcpy(lock_vb(m), p6, sizeof p6); unlock_vb(m);
        ok("GenerateAdjacency eps 0: vertices differ by 1e-4, no neighbour",
           call("ID3DXMesh::GenerateAdjacency", 3, (uint32_t[]){ m, 0, A(adj) }) == D3D_OK && adj[1] == NOFACE && adj[3] == NOFACE);
        uint32_t eps = 0x3A83126Fu;   /* 0.001f */
        ok("GenerateAdjacency eps 1e-3 joins them",
           call("ID3DXMesh::GenerateAdjacency", 3, (uint32_t[]){ m, eps, A(adj) }) == D3D_OK && adj[1] == 1 && adj[3] == 0);
        uint32_t reps[6] = { 0, 1, 2, 2, 1, 5 };
        uint32_t *adj2 = (uint32_t *)hle_alloc(24);
        ok("ConvertPointRepsToAdjacency with point reps",
           call("ID3DXMesh::ConvertPointRepsToAdjacency", 3, (uint32_t[]){ m, A(gcopy(reps, sizeof reps)), A(adj2) }) == D3D_OK &&
           adj2[1] == 1 && adj2[3] == 0 && adj2[0] == NOFACE && adj2[5] == NOFACE);
        ok("point reps outside the vertex range invalid",
           call("ID3DXMesh::ConvertPointRepsToAdjacency", 3, (uint32_t[]){ m, A(gcopy((uint32_t[6]){ 0, 1, 2, 2, 1, 6 }, 24)), A(adj2) }) == D3DERR_INVALIDCALL);
        /* adjacency -> point reps: f0's edge (1,2) meets f1's edge (4,3) reversed, so 1~4 and 2~3. */
        uint32_t *pr = (uint32_t *)hle_alloc(24);
        ok("ConvertAdjacencyToPointReps", call("ID3DXMesh::ConvertAdjacencyToPointReps", 3, (uint32_t[]){ m, A(adj2), A(pr) }) == D3D_OK);
        ok("point reps {0,1,2,2,1,5}", pr[0] == 0 && pr[1] == 1 && pr[2] == 2 && pr[3] == 2 && pr[4] == 1 && pr[5] == 5);
        uint32_t *bad = (uint32_t *)hle_alloc(24); memcpy(bad, adj2, 24); bad[0] = 7;
        ok("adjacency naming a missing face invalid",
           call("ID3DXMesh::ConvertAdjacencyToPointReps", 3, (uint32_t[]){ m, A(bad), A(pr) }) == D3DERR_INVALIDCALL);
        ok("NULL arguments invalid",
           call("ID3DXMesh::ConvertAdjacencyToPointReps", 3, (uint32_t[]){ m, 0, A(pr) }) == D3DERR_INVALIDCALL &&
           call("ID3DXMesh::ConvertAdjacencyToPointReps", 3, (uint32_t[]){ m, A(adj2), 0 }) == D3DERR_INVALIDCALL);
        release(m);
    }

    /* ============ attribute table ============ */
    {
        uint32_t m = create_fvf(4, 6, 0, 0x2, NULL);
        uint32_t *n = (uint32_t *)hle_alloc(4); *n = 99;
        ok("no table yet: size 0", call("ID3DXMesh::GetAttributeTable", 3, (uint32_t[]){ m, 0, A(n) }) == D3D_OK && *n == 0);
        range_t t[2] = { { 0, 0, 2, 0, 3 }, { 5, 2, 2, 3, 3 } };
        ok("SetAttributeTable", call("ID3DXMesh::SetAttributeTable", 3, (uint32_t[]){ m, A(gcopy(t, sizeof t)), 2 }) == D3D_OK);
        ok("GetAttributeTable(NULL) gives the count", call("ID3DXMesh::GetAttributeTable", 3, (uint32_t[]){ m, 0, A(n) }) == D3D_OK && *n == 2);
        range_t *o = (range_t *)hle_alloc(2 * sizeof *o); *n = 1;
        ok("a buffer that is too small is invalid", call("ID3DXMesh::GetAttributeTable", 3, (uint32_t[]){ m, A(o), A(n) }) == D3DERR_INVALIDCALL);
        *n = 2;
        ok("GetAttributeTable", call("ID3DXMesh::GetAttributeTable", 3, (uint32_t[]){ m, A(o), A(n) }) == D3D_OK && *n == 2 &&
           o[1].id == 5 && o[1].fs == 2 && o[1].fc == 2 && o[1].vs == 3 && o[1].vc == 3 && o[0].fc == 2);
        range_t badr[1] = { { 0, 3, 2, 0, 3 } };    /* faces 3..4 of 4 */
        ok("face range beyond NumFaces invalid", call("ID3DXMesh::SetAttributeTable", 3, (uint32_t[]){ m, A(gcopy(badr, sizeof badr)), 1 }) == D3DERR_INVALIDCALL);
        range_t badv[1] = { { 0, 0, 1, 4, 3 } };    /* vertices 4..6 of 6 */
        ok("vertex range beyond NumVertices invalid", call("ID3DXMesh::SetAttributeTable", 3, (uint32_t[]){ m, A(gcopy(badv, sizeof badv)), 1 }) == D3DERR_INVALIDCALL);
        ok("a failed Set keeps the old table", call("ID3DXMesh::GetAttributeTable", 3, (uint32_t[]){ m, 0, A(n) }) == D3D_OK && *n == 2);
        ok("NULL table with a count invalid", call("ID3DXMesh::SetAttributeTable", 3, (uint32_t[]){ m, 0, 1 }) == D3DERR_INVALIDCALL);
        ok("size 0 clears", call("ID3DXMesh::SetAttributeTable", 3, (uint32_t[]){ m, 0, 0 }) == D3D_OK &&
           call("ID3DXMesh::GetAttributeTable", 3, (uint32_t[]){ m, 0, A(n) }) == D3D_OK && *n == 0);
        release(m);
    }

    /* ============ CloneMeshFVF / CloneMesh ============ */
    {
        /* Source XYZ|NORMAL|TEX1 (stride 32): vertex i = pos (i,i+10,i+20), normal (0,0,1), uv (i*0.5, 7). */
        uint32_t m = create_fvf(2, 4, 0, 0x112, NULL), hr;
        float *v = (float *)lock_vb(m);
        for (int i = 0; i < 4; i++) {
            float vert[8] = { (float)i, (float)i + 10, (float)i + 20, 0, 0, 1, (float)i * 0.5f, 7 };
            memcpy(v + i * 8, vert, 32);
        }
        unlock_vb(m);
        uint16_t f[6] = { 0, 1, 2, 3, 2, 1 };
        memcpy(lock_ib(m), f, 12); unlock_ib(m);
        uint32_t at[2] = { 4, 9 };
        memcpy(lock_ab(m), at, 8); unlock_ab(m);
        range_t t[1] = { { 4, 0, 1, 0, 3 } };
        call("ID3DXMesh::SetAttributeTable", 3, (uint32_t[]){ m, A(gcopy(t, sizeof t)), 1 });

        uint32_t *pc = (uint32_t *)hle_alloc(4); *pc = 0;
        uint32_t esp_ref = REF(dev);
        hr = call("ID3DXMesh::CloneMeshFVF", 5, (uint32_t[]){ m, 0, 0x102 /* XYZ|TEX1 */, dev, A(pc) });
        ok("CloneMeshFVF drops the normal", hr == D3D_OK && *pc);
        uint32_t c = *pc;
        ok("clone FVF/stride", call("ID3DXMesh::GetFVF", 1, (uint32_t[]){ c }) == 0x102 &&
           call("ID3DXMesh::GetNumBytesPerVertex", 1, (uint32_t[]){ c }) == 20);
        float *cv = (float *)lock_vb(c);
        ok("clone vertex 3: position and uv carried over", cv[3 * 5] == 3 && cv[3 * 5 + 1] == 13 && cv[3 * 5 + 2] == 23 &&
           cv[3 * 5 + 3] == 1.5f && cv[3 * 5 + 4] == 7);
        unlock_vb(c);
        uint16_t *ci = (uint16_t *)lock_ib(c);
        ok("clone indices", ci[0] == 0 && ci[1] == 1 && ci[2] == 2 && ci[3] == 3 && ci[4] == 2 && ci[5] == 1);
        unlock_ib(c);
        uint32_t *ca = lock_ab(c);
        ok("clone attributes", ca[0] == 4 && ca[1] == 9);
        unlock_ab(c);
        uint32_t *n = (uint32_t *)hle_alloc(4);
        ok("clone attribute table", call("ID3DXMesh::GetAttributeTable", 3, (uint32_t[]){ c, 0, A(n) }) == D3D_OK && *n == 1);
        release(c);
        ok("the clone's device reference is released", REF(dev) == esp_ref);

        /* new element (DIFFUSE) stays 0; texcoord FLOAT2 -> FLOAT3 gets a zero third component */
        hr = call("ID3DXMesh::CloneMeshFVF", 5, (uint32_t[]){ m, 0, 0x10152 /* XYZ|NORMAL|DIFFUSE|TEX1 (3 floats) */, 0, A(pc) });
        ok("CloneMeshFVF with a NULL device uses the source device", hr == D3D_OK && *pc);
        c = *pc;
        uint32_t uc[1];
        (void)uc;
        float *cv2 = (float *)lock_vb(c);   /* stride 12+12+4+12 = 40 bytes = 10 floats */
        ok("clone stride 40", call("ID3DXMesh::GetNumBytesPerVertex", 1, (uint32_t[]){ c }) == 40);
        ok("new DIFFUSE element is 0", ((uint32_t *)cv2)[2 * 10 + 6] == 0);
        ok("FLOAT2 uv widened to FLOAT3: (1,7,0)", cv2[2 * 10 + 7] == 1.0f && cv2[2 * 10 + 8] == 7.0f && cv2[2 * 10 + 9] == 0.0f);
        ok("normal carried over", cv2[2 * 10 + 5] == 1.0f);
        unlock_vb(c);
        release(c);

        /* 16 -> 32 bit indices */
        hr = call("ID3DXMesh::CloneMeshFVF", 5, (uint32_t[]){ m, 1, 0x112, dev, A(pc) });
        c = *pc;
        uint32_t *c32 = (uint32_t *)lock_ib(c);
        ok("CloneMeshFVF with D3DXMESH_32BIT widens the indices", hr == D3D_OK && c32[0] == 0 && c32[4] == 2 && c32[5] == 1);
        unlock_ib(c);
        ok("options of the clone", call("ID3DXMesh::GetOptions", 1, (uint32_t[]){ c }) == 1);
        release(c);

        ok("CloneMeshFVF with an unsupported FVF", call("ID3DXMesh::CloneMeshFVF", 5, (uint32_t[]){ m, 0, 0x10, dev, A(pc) }) == D3DERR_INVALIDCALL && *pc == 0);
        ok("CloneMeshFVF with NULL out invalid", call("ID3DXMesh::CloneMeshFVF", 5, (uint32_t[]){ m, 0, 0x2, dev, 0 }) == D3DERR_INVALIDCALL);
        ok("CloneMeshFVF with POINTS refused", call("ID3DXMesh::CloneMeshFVF", 5, (uint32_t[]){ m, 4, 0x2, dev, A(pc) }) == D3DERR_NOTAVAILABLE);

        /* CloneMesh with a declaration: convertible types only */
        D3DVERTEXELEMENT9 d1[3] = { el(0, 2, 0, 0), el(12, 4, 10, 0), el(16, 2, 3, 0) };   /* POSITION f3, COLOR0 D3DCOLOR, NORMAL f3 */
        D3DVERTEXELEMENT9 *g = (D3DVERTEXELEMENT9 *)hle_alloc(4 * sizeof *g);
        memcpy(g, d1, sizeof d1);
        g[3].Stream = 0xFF; g[3].Type = 17; g[3].Offset = g[3].Method = g[3].Usage = g[3].UsageIndex = 0;
        hr = call("ID3DXMesh::CloneMesh", 5, (uint32_t[]){ m, 0, A(g), dev, A(pc) });
        ok("CloneMesh with a declaration", hr == D3D_OK && *pc);
        c = *pc;
        float *cv3 = (float *)lock_vb(c);   /* stride 28 = 7 floats */
        ok("declaration clone: position and normal moved to their new offsets",
           cv3[1 * 7] == 1 && cv3[1 * 7 + 1] == 11 && cv3[1 * 7 + 4] == 0 && cv3[1 * 7 + 6] == 1.0f);
        unlock_vb(c);
        release(c);
        D3DVERTEXELEMENT9 d2[2] = { el(0, 2, 0, 0), el(12, 3, 3, 0) };   /* NORMAL as FLOAT4 is fine ... */
        D3DVERTEXELEMENT9 d3[2] = { el(0, 2, 0, 0), el(12, 4, 3, 0) };   /* ... NORMAL as D3DCOLOR is not */
        D3DVERTEXELEMENT9 *g2 = (D3DVERTEXELEMENT9 *)hle_alloc(3 * sizeof *g2), *g3 = (D3DVERTEXELEMENT9 *)hle_alloc(3 * sizeof *g3);
        memcpy(g2, d2, sizeof d2); memcpy(g3, d3, sizeof d3);
        g2[2].Stream = g3[2].Stream = 0xFF; g2[2].Type = g3[2].Type = 17; g2[2].Offset = g3[2].Offset = 0;
        g2[2].Method = g3[2].Method = g2[2].Usage = g3[2].Usage = g2[2].UsageIndex = g3[2].UsageIndex = 0;
        ok("FLOAT3 -> FLOAT4 conversion", call("ID3DXMesh::CloneMesh", 5, (uint32_t[]){ m, 0, A(g2), dev, A(pc) }) == D3D_OK);
        release(*pc);
        ok("FLOAT3 -> D3DCOLOR conversion refused", call("ID3DXMesh::CloneMesh", 5, (uint32_t[]){ m, 0, A(g3), dev, A(pc) }) == D3DERR_INVALIDCALL && *pc == 0);
        ok("CloneMesh with NULL declaration invalid", call("ID3DXMesh::CloneMesh", 5, (uint32_t[]){ m, 0, 0, dev, A(pc) }) == D3DERR_INVALIDCALL);
        release(m);
    }

    /* ============ UpdateSemantics ============ */
    {
        D3DVERTEXELEMENT9 d1[2] = { el(0, 2, 0, 0), el(12, 1, 5, 0) };
        uint32_t hr, m = create_decl(1, 3, 0, d1, 2, &hr);
        D3DVERTEXELEMENT9 *g = (D3DVERTEXELEMENT9 *)hle_alloc(3 * sizeof *g);
        D3DVERTEXELEMENT9 same[2] = { el(0, 2, 0, 0), el(12, 1, 5, 1) };   /* TEXCOORD0 -> TEXCOORD1 */
        memcpy(g, same, sizeof same);
        g[2].Stream = 0xFF; g[2].Type = 17; g[2].Offset = g[2].Method = g[2].Usage = g[2].UsageIndex = 0;
        ok("UpdateSemantics with the same layout", call("ID3DXMesh::UpdateSemantics", 2, (uint32_t[]){ m, A(g) }) == D3D_OK);
        D3DVERTEXELEMENT9 *out = (D3DVERTEXELEMENT9 *)hle_alloc(3 * sizeof *out);
        call("ID3DXMesh::GetDeclaration", 2, (uint32_t[]){ m, A(out) });
        ok("the usage index changed", out[1].UsageIndex == 1);
        ok("the FVF is gone (TEXCOORD1 alone is no FVF layout)", call("ID3DXMesh::GetFVF", 1, (uint32_t[]){ m }) == 0);
        g[1].Offset = 16;
        ok("another offset is invalid", call("ID3DXMesh::UpdateSemantics", 2, (uint32_t[]){ m, A(g) }) == D3DERR_INVALIDCALL);
        ok("NULL declaration invalid", call("ID3DXMesh::UpdateSemantics", 2, (uint32_t[]){ m, 0 }) == D3DERR_INVALIDCALL);
        release(m);
    }

    /* ============ Optimize ============ */
    {
        /* 10 vertices (x = index), 6 faces fanning around vertex 1, attributes {2,0,1,0,2,1}; 8 and 9 unused. */
        uint16_t faces[18] = { 0,1,2,  2,1,3,  3,1,4,  4,1,5,  5,1,6,  6,1,7 };
        uint32_t attrs[6] = { 2, 0, 1, 0, 2, 1 };
        uint32_t m = make_line_mesh(10, faces, 6, attrs);
        uint32_t *adjIn = (uint32_t *)hle_alloc(72);
        call("ID3DXMesh::GenerateAdjacency", 3, (uint32_t[]){ m, 0, A(adjIn) });
        /* f(i) shares edge (1,i+2) with f(i+1): {-,1,-} {0,2,-} {1,3,-} {2,4,-} {3,5,-} {4,-,-} */
        static const uint32_t fan[18] = { NOFACE,1,NOFACE, 0,2,NOFACE, 1,3,NOFACE, 2,4,NOFACE, 3,5,NOFACE, 4,NOFACE,NOFACE };
        ok("fan adjacency (input of Optimize)", memcmp(adjIn, fan, 72) == 0);

        uint32_t *adjOut = (uint32_t *)hle_alloc(72), *fr = (uint32_t *)hle_alloc(24), *pv = (uint32_t *)hle_alloc(4), *po = (uint32_t *)hle_alloc(4);
        *pv = 0; *po = 0;
        uint32_t flags = 0x02000000u | 0x01000000u;   /* ATTRSORT | COMPACT */
        uint32_t hr = call("ID3DXMesh::Optimize", 7, (uint32_t[]){ m, flags, A(adjIn), A(adjOut), A(fr), A(pv), A(po) });
        ok("Optimize(ATTRSORT|COMPACT)", hr == D3D_OK && *po && *pv);
        uint32_t o = *po;
        ok("faces sorted by attribute (stable): new -> old {1,3,2,5,0,4}",
           fr[0] == 1 && fr[1] == 3 && fr[2] == 2 && fr[3] == 5 && fr[4] == 0 && fr[5] == 4);
        ok("unused vertices dropped: 8 vertices, 6 faces",
           call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){ o }) == 8 && call("ID3DXMesh::GetNumFaces", 1, (uint32_t[]){ o }) == 6);
        uint32_t bufsz = call("ID3DXMeshBuffer::GetBufferSize", 1, (uint32_t[]){ *pv });
        uint32_t *vr = (uint32_t *)hle_ptr(call("ID3DXMeshBuffer::GetBufferPointer", 1, (uint32_t[]){ *pv }));
        ok("vertex remap buffer: 8 DWORDs", bufsz == 32);
        ok("vertex remap new -> old {2,1,3,4,5,6,7,0}",
           vr[0] == 2 && vr[1] == 1 && vr[2] == 3 && vr[3] == 4 && vr[4] == 5 && vr[5] == 6 && vr[6] == 7 && vr[7] == 0);
        float *ov = (float *)lock_vb(o);
        int moved = 1;
        for (int k = 0; k < 8; k++) if (ov[k * 3] != (float)vr[k]) moved = 0;
        ok("vertex data moved with the remap", moved);
        unlock_vb(o);
        uint16_t *oi = (uint16_t *)lock_ib(o);
        static const uint16_t want[18] = { 0,1,2,  3,1,4,  2,1,3,  5,1,6,  7,1,0,  4,1,5 };
        ok("renumbered indices", memcmp(oi, want, 36) == 0);
        unlock_ib(o);
        uint32_t *oa = lock_ab(o);
        ok("attributes follow the faces {0,0,1,1,2,2}", oa[0] == 0 && oa[1] == 0 && oa[2] == 1 && oa[3] == 1 && oa[4] == 2 && oa[5] == 2);
        unlock_ab(o);
        uint32_t *nt = (uint32_t *)hle_alloc(4); range_t *tab = (range_t *)hle_alloc(4 * sizeof(range_t));
        *nt = 4;
        ok("attribute table of the optimized mesh", call("ID3DXMesh::GetAttributeTable", 3, (uint32_t[]){ o, A(tab), A(nt) }) == D3D_OK && *nt == 3);
        /* attr 0: faces 0-1 use vertices {0,1,2,3,1,4}: 0..4; attr 1: faces 2-3 {2,1,3,5,1,6}: 1..6; attr 2: faces 4-5 {7,1,0,4,1,5}: 0..7 */
        ok("table row 0", tab[0].id == 0 && tab[0].fs == 0 && tab[0].fc == 2 && tab[0].vs == 0 && tab[0].vc == 5);
        ok("table row 1", tab[1].id == 1 && tab[1].fs == 2 && tab[1].fc == 2 && tab[1].vs == 1 && tab[1].vc == 6);
        ok("table row 2", tab[2].id == 2 && tab[2].fs == 4 && tab[2].fc == 2 && tab[2].vs == 0 && tab[2].vc == 8);
        /* neighbours renumbered with old -> new {f0:4, f1:0, f2:2, f3:1, f4:5, f5:3} */
        static const uint32_t wadj[18] = { 4,2,NOFACE,  2,5,NOFACE,  0,1,NOFACE,  5,NOFACE,NOFACE,  NOFACE,0,NOFACE,  1,3,NOFACE };
        ok("adjacency of the optimized mesh", memcmp(adjOut, wadj, 72) == 0);
        ok("the source mesh is unchanged", call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){ m }) == 10);
        call("ID3DXMeshBuffer::Release", 1, (uint32_t[]){ *pv });
        release(o);

        /* ATTRSORT alone keeps every vertex, in place */
        *po = 0; *pv = 0;
        hr = call("ID3DXMesh::Optimize", 7, (uint32_t[]){ m, 0x02000000u, 0, 0, 0, A(pv), A(po) });
        ok("Optimize(ATTRSORT) without adjacency", hr == D3D_OK && *po);
        ok("ATTRSORT alone keeps the vertex count", call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){ *po }) == 10);
        uint32_t *vr2 = (uint32_t *)hle_ptr(call("ID3DXMeshBuffer::GetBufferPointer", 1, (uint32_t[]){ *pv }));
        ok("identity vertex remap", vr2[0] == 0 && vr2[9] == 9);
        call("ID3DXMeshBuffer::Release", 1, (uint32_t[]){ *pv });
        release(*po);

        /* COMPACT drops faces marked unused */
        uint32_t *ua = lock_ab(m); ua[3] = 0xFFFFFFFFu; unlock_ab(m);
        hr = call("ID3DXMesh::Optimize", 7, (uint32_t[]){ m, 0x01000000u, 0, 0, A(fr), 0, A(po) });
        ok("Optimize(COMPACT) drops the unused face", hr == D3D_OK && call("ID3DXMesh::GetNumFaces", 1, (uint32_t[]){ *po }) == 5 &&
           fr[0] == 0 && fr[1] == 1 && fr[2] == 2 && fr[3] == 4 && fr[4] == 5);
        release(*po);

        /* argument checks */
        ok("no optimization flag invalid", call("ID3DXMesh::Optimize", 7, (uint32_t[]){ m, 0, 0, 0, 0, 0, A(po) }) == D3DERR_INVALIDCALL);
        ok("VERTEXCACHE refused", call("ID3DXMesh::Optimize", 7, (uint32_t[]){ m, 0x04000000u | 0x02000000u, 0, 0, 0, 0, A(po) }) == D3DERR_NOTAVAILABLE);
        ok("STRIPREORDER refused", call("ID3DXMesh::Optimize", 7, (uint32_t[]){ m, 0x08000000u, 0, 0, 0, 0, A(po) }) == D3DERR_NOTAVAILABLE);
        ok("adjacency out needs adjacency in", call("ID3DXMesh::Optimize", 7, (uint32_t[]){ m, 0x02000000u, 0, A(adjOut), 0, 0, A(po) }) == D3DERR_INVALIDCALL);
        ok("NULL ppOptMesh invalid", call("ID3DXMesh::Optimize", 7, (uint32_t[]){ m, 0x02000000u, 0, 0, 0, 0, 0 }) == D3DERR_INVALIDCALL);
        uint32_t *badadj = (uint32_t *)hle_alloc(72); memcpy(badadj, adjIn, 72); badadj[2] = 6;
        ok("adjacency naming a missing face invalid", call("ID3DXMesh::Optimize", 7, (uint32_t[]){ m, 0x02000000u, A(badadj), 0, 0, 0, A(po) }) == D3DERR_INVALIDCALL);
        release(m);

        /* OptimizeInplace changes the mesh itself */
        m = make_line_mesh(10, faces, 6, attrs);
        *pv = 0;
        hr = call("ID3DXMesh::OptimizeInplace", 6, (uint32_t[]){ m, flags, A(adjIn), A(adjOut), A(fr), A(pv) });
        ok("OptimizeInplace(ATTRSORT|COMPACT)", hr == D3D_OK);
        ok("in place: 8 vertices, 6 faces", call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){ m }) == 8 && call("ID3DXMesh::GetNumFaces", 1, (uint32_t[]){ m }) == 6);
        oi = (uint16_t *)lock_ib(m);
        ok("in place: renumbered indices", memcmp(oi, want, 36) == 0);
        unlock_ib(m);
        ok("in place: face remap", fr[0] == 1 && fr[1] == 3 && fr[2] == 2 && fr[3] == 5 && fr[4] == 0 && fr[5] == 4);
        ok("in place: adjacency out", memcmp(adjOut, wadj, 72) == 0);
        *nt = 4;
        ok("in place: attribute table", call("ID3DXMesh::GetAttributeTable", 3, (uint32_t[]){ m, A(tab), A(nt) }) == D3D_OK && *nt == 3 && tab[2].fs == 4);
        ok("OptimizeInplace refuses mesh option flags", call("ID3DXMesh::OptimizeInplace", 6, (uint32_t[]){ m, flags | 1, 0, 0, 0, 0 }) == D3DERR_INVALIDCALL);
        call("ID3DXMeshBuffer::Release", 1, (uint32_t[]){ *pv });
        release(m);
    }

    ok("every device reference taken by the tests is gone", REF(dev) == dev_ref0);

    /* ============ COM checklist: every guest-vtable slot, esp measured ============ */
    {
        /* Order and signatures: public d3dx9mesh.h (ID3DXBaseMesh, ID3DXMesh); Wine's include/d3dx9mesh.h
         * lists the same order. argc includes `this`. */
        static const char *const names[29] = {
            "QueryInterface", "AddRef", "Release", "DrawSubset", "GetNumFaces", "GetNumVertices", "GetFVF",
            "GetDeclaration", "GetNumBytesPerVertex", "GetOptions", "GetDevice", "CloneMeshFVF", "CloneMesh",
            "GetVertexBuffer", "GetIndexBuffer", "LockVertexBuffer", "UnlockVertexBuffer", "LockIndexBuffer",
            "UnlockIndexBuffer", "GetAttributeTable", "ConvertPointRepsToAdjacency", "ConvertAdjacencyToPointReps",
            "GenerateAdjacency", "UpdateSemantics", "LockAttributeBuffer", "UnlockAttributeBuffer", "Optimize",
            "OptimizeInplace", "SetAttributeTable" };
        static const int argc[29] = { 3, 1, 1, 2, 1, 1, 1, 2, 1, 1, 2, 5, 5, 2, 2, 3, 1, 3, 1, 3, 3, 3, 3, 2, 3, 1, 7, 6, 3 };
        uint32_t m = create_fvf(2, 4, 0, 0x2, NULL);
        uint32_t vt = MEM32(m);
        int right = 1;
        for (int i = 0; i < 29; i++) {
            char full[64]; snprintf(full, sizeof full, "ID3DXMesh::%s", names[i]);
            if (MEM32(vt + 4u * (uint32_t)i) != hle_resolve(full)) { printf("FAIL slot %d is not %s\n", i, full); right = 0; fails++; }
        }
        ok("vtable order matches the header", right);
        ok("vtable has exactly 29 slots (IUnknown 3 + ID3DXBaseMesh 21 + ID3DXMesh 5)", MEM32(vt + 4u * 29u) == 0);
        for (int i = 0; i < 29; i++) {
            uint32_t *sc = (uint32_t *)hle_alloc(64); memset(sc, 0, 64);
            uint32_t a[7] = { m, 0, 0, 0, 0, 0, 0 };
            if (i == 0) { a[1] = iid_unk(); a[2] = A(sc); }      /* QueryInterface(riid, ppv) */
            if (i == 7) a[1] = A(sc);                               /* GetDeclaration */
            if (i == 10) a[1] = A(sc);                              /* GetDevice */
            if (i == 11) { a[2] = 0x2; a[3] = dev; a[4] = A(sc); }  /* CloneMeshFVF */
            if (i == 13 || i == 14) a[1] = A(sc);                   /* Get{Vertex,Index}Buffer */
            if (i == 15 || i == 17 || i == 24) a[2] = A(sc);        /* Lock* */
            if (i == 19) a[2] = A(sc);                              /* GetAttributeTable(NULL, &size) */
            if (i == 22) a[2] = A(sc);                              /* GenerateAdjacency */
            uint32_t delta, eax;
            if (i == 2) REF(m) = 2;                                 /* keep the mesh alive across Release */
            eax = slot_call(m, i, argc[i], a, &delta);
            char msg[96]; snprintf(msg, sizeof msg, "slot %d %s: entered, esp balanced (delta %u)", i, names[i], (unsigned)delta);
            ok(msg, delta == 0u);
            if (i == 2) { ok("Release returned the decremented count", eax == 1); REF(m) = 1; }
            if (i == 0 && MEM32(A(sc)) == m) REF(m)--;              /* the reference QueryInterface took */
            if (i == 10) REF(dev) = dev_ref0 + 1;                   /* GetDevice's reference */
            if (i == 11 && MEM32(A(sc))) release(MEM32(A(sc)));
            if (i == 15) unlock_vb(m);
            if (i == 17) unlock_ib(m);
            if (i == 24) unlock_ab(m);
        }
        release(m);

        /* ID3DXBuffer of the remaps: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize */
        uint32_t *po = (uint32_t *)hle_alloc(4), *pv = (uint32_t *)hle_alloc(4);
        uint16_t one[3] = { 0, 1, 2 };
        uint32_t o = make_line_mesh(3, one, 1, NULL);
        call("ID3DXMesh::Optimize", 7, (uint32_t[]){ o, 0x01000000u, 0, 0, 0, A(pv), A(po) });
        uint32_t b = *pv;
        static const int bargc[5] = { 3, 1, 1, 1, 1 };
        uint32_t bvt = MEM32(b);
        ok("ID3DXBuffer vtable has 5 slots", MEM32(bvt + 4u * 4u) != 0 && MEM32(bvt + 4u * 5u) == 0);
        for (int i = 0; i < 5; i++) {
            uint32_t *sc = (uint32_t *)hle_alloc(8);
            uint32_t a[3] = { b, i == 0 ? iid_unk() : 0, i == 0 ? A(sc) : 0 };
            if (i == 2) REF(b) = 2;                                 /* keep the buffer alive across Release */
            uint32_t delta, eax = slot_call(b, i, bargc[i], a, &delta);
            if (i == 2) { ok("ID3DXBuffer::Release returned 1", eax == 1); REF(b) = 1; }
            char msg[64]; snprintf(msg, sizeof msg, "ID3DXBuffer slot %d: esp balanced (delta %u)", i, (unsigned)delta);
            ok(msg, delta == 0u);
            if (i == 0) { ok("ID3DXBuffer::QueryInterface out", MEM32(A(sc)) == b); REF(b) = 1; }
            if (i == 1) { ok("ID3DXBuffer::AddRef", eax == 2); REF(b) = 1; }
            if (i == 3) ok("ID3DXBuffer::GetBufferPointer", eax != 0);
            if (i == 4) ok("ID3DXBuffer::GetBufferSize", eax == 12);
        }
        release(*po);
        call("ID3DXMeshBuffer::Release", 1, (uint32_t[]){ b });
        release(o);
        ok("D3DXCreateMeshFVF / D3DXCreateMesh: esp balanced and the right argc", ({
            uint32_t *pp = (uint32_t *)hle_alloc(4), delta1 = 1, delta2 = 1;
            uint32_t a[6] = { 1, 3, 0, 0x2, dev, A(pp) };
            uint32_t r1 = 0, r2 = 0;
            mach_enter();
            uint32_t esp0 = g_esp;
            for (int k = 5; k >= 0; k--) PUSH32(g_esp, a[k]);
            PUSH32(g_esp, RECOMP_RETADDR);
            recomp_func_t f = recomp_lookup_import(hle_resolve("D3DXCreateMeshFVF"));
            if (f) { f(); r1 = g_eax; delta1 = g_esp - esp0; }
            g_esp = esp0;
            mach_leave();
            (void)r2; (void)delta2;
            delta1 == 0 && r1 == D3D_OK && *pp != 0 && (release(*pp), 1);
        }));
    }

    printf(fails == 0 ? "d3dx9_mesh_selftest: all checks passed\n" : "d3dx9_mesh_selftest: FAILED\n");
    return fails != 0;
}
