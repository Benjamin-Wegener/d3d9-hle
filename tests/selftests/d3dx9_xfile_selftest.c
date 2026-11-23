/*
 * d3dx9_xfile_selftest.c - D3DXLoadMeshFromXA/W/InMemory.
 *
 * Uses only self-written .x text data. Expected values are computed by hand
 * from the input data (independent of implementation).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define A(p) hle_va(p)

static int feq(float a, float b) { return fabsf(a-b) < 1e-5f; }

/* 1 triangle, 3 vertices, normals same as position indices, 2 texcoords, 1 material. */
static const char x_simple[] =
"xof 0302txt 0064\n"
"Mesh {\n"
" 3;\n"
" 0.0;0.0;0.0;,\n"
" 1.0;0.0;0.0;,\n"
" 0.0;1.0;0.0;;\n"
" 1;\n"
" 3;0,1,2;;\n"
" MeshNormals {\n"
"  3;\n"
"  0.0;0.0;1.0;,\n"
"  0.0;0.0;1.0;,\n"
"  0.0;0.0;1.0;;\n"
"  1;\n"
"  3;0,1,2;;\n"
" }\n"
" MeshTextureCoords {\n"
"  3;\n"
"  0.0;0.0;,\n"
"  1.0;0.0;,\n"
"  0.0;1.0;;\n"
" }\n"
" MeshMaterialList {\n"
"  1;\n"
"  1;\n"
"  0;\n"
"  Material {\n"
"   1.0;0.5;0.25;1.0;;\n"
"   2.0;\n"
"   0.1;0.2;0.3;;\n"
"   0.0;0.0;0.0;;\n"
"   TextureFilename { \"tex.png\"; }\n"
"  }\n"
" }\n"
"}\n";

/* Broken inputs */
static const char x_bad_header[] = "notx 0302txt 0064\nMesh { 0; 0; }\n";
static const char x_binary[]    = "xof 0302bin 0064\n";
static const char x_no_mesh[]    = "xof 0302txt 0064\nFrame { }\n";
static const char x_trunc[]      = "xof 0302txt 0064\nMesh { 3; 0;0;0;, 1;0;0;,";

/* ---- AllocateHierarchy mock for hierarchy tests ---- */
static int g_cf, g_cmc, g_df, g_dmc;
static uint32_t g_last_frame, g_last_mc;
static char g_cf_names[8][64];
static int g_cf_n;

static void ah_CreateFrame(void) {
    /* (this, name, ppNewFrame) */
    uint32_t name = A32(1), pp = A32(2);
    g_cf++;
    if (g_cf_n < 8) {
        const char *n = name ? (const char*)hle_ptr(name) : "";
        strncpy(g_cf_names[g_cf_n], n, 63); g_cf_names[g_cf_n][63]=0;
        g_cf_n++;
    }
    uint32_t f = hle_va(hle_alloc(80));
    memset(hle_ptr(f), 0, 80);
    /* identity */
    float *m = (float*)hle_ptr(f+4);
    m[0]=m[5]=m[10]=m[15]=1.0f;
    MEM32(f) = name; /* keep name ptr as given */
    MEM32(pp) = f;
    g_last_frame = f;
    RET(D3D_OK, 3);
}
static void ah_CreateMeshContainer(void) {
    /* (this, name, meshData, materials, effects, numMaterials, adjacency, skinInfo, ppNewMeshContainer) */
    uint32_t meshData = A32(2), materials = A32(3), numMat = A32(5), pp = A32(8);
    g_cmc++;
    uint32_t mc = hle_va(hle_alloc(36));
    memset(hle_ptr(mc), 0, 36);
    if (meshData) {
        MEM32(mc+4) = MEM32(meshData+0); /* Type */
        MEM32(mc+8) = MEM32(meshData+4); /* pMesh */
    }
    MEM32(mc+12) = materials;
    MEM32(mc+20) = numMat;
    MEM32(pp) = mc;
    g_last_mc = mc;
    RET(D3D_OK, 9);
}
static void ah_DestroyFrame(void) {
    g_df++;
    RET(D3D_OK, 2);
}
static void ah_DestroyMeshContainer(void) {
    g_dmc++;
    /* release mesh if present */
    uint32_t mc = A32(1);
    if (mc) {
        uint32_t mesh = MEM32(mc+8);
        if (mesh) { uint32_t vt=MEM32(mesh); hle_call_guest(MEM32(vt+8),1,(uint32_t[]){mesh}); }
    }
    RET(D3D_OK, 2);
}

static uint32_t make_alloc_hierarchy(void) {
    static int reg = 0;
    if (!reg) {
        static const win32hle_shim sh[] = {
            { "AH::CreateFrame", ah_CreateFrame },
            { "AH::CreateMeshContainer", ah_CreateMeshContainer },
            { "AH::DestroyFrame", ah_DestroyFrame },
            { "AH::DestroyMeshContainer", ah_DestroyMeshContainer },
            { 0, 0 }
        };
        win32hle_register(sh);
        reg = 1;
    }
    uint32_t *vt = (uint32_t *)hle_alloc(4*4);
    vt[0] = hle_resolve("AH::CreateFrame");
    vt[1] = hle_resolve("AH::CreateMeshContainer");
    vt[2] = hle_resolve("AH::DestroyFrame");
    vt[3] = hle_resolve("AH::DestroyMeshContainer");
    return hle_com_new(A(vt), 16);
}

static uint32_t make_device(void) {
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32u });
    ok("Direct3DCreate9", d3d9 != 0);
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof *pp);
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = 64; pp->BackBufferHeight = 64;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    pp->Windowed = 1; pp->SwapEffect = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3D9::CreateDevice", 7, (uint32_t[]){
        d3d9, 0u, D3DDEVTYPE_HAL, 0u, D3DCREATE_SOFTWARE_VERTEXPROCESSING, A(pp), A(pDev) });
    ok("CreateDevice", hr == D3D_OK && *pDev != 0);
    return *pDev;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);
    win32hle_register(win32hle_d3d9);

    uint32_t dev = make_device();

    /* ---- InMemory success ---- */
    void *xd = hle_alloc(sizeof(x_simple));
    memcpy(xd, x_simple, sizeof(x_simple));
    uint32_t *ppAdj=(uint32_t*)hle_alloc(4), *ppMat=(uint32_t*)hle_alloc(4), *ppEff=(uint32_t*)hle_alloc(4), *pNum=(uint32_t*)hle_alloc(4), *ppMesh=(uint32_t*)hle_alloc(4);
    *ppAdj=*ppMat=*ppEff=*ppMesh=0xDEAD; *pNum=0xDEAD;
    uint32_t hr = call("D3DXLoadMeshFromXInMemory", 9, (uint32_t[]){
        A(xd), (uint32_t)(sizeof(x_simple)-1), 0u, dev, A(ppAdj), A(ppMat), A(ppEff), A(pNum), A(ppMesh) });
    ok("LoadMeshFromXInMemory hr", hr == D3D_OK);
    ok("ppMesh set", *ppMesh != 0 && *ppMesh != 0xDEADu);
    ok("pNumMaterials == 1", *pNum == 1u);
    ok("ppMaterials set", *ppMat != 0 && *ppMat != 0xDEADu);
    ok("ppEffectInstances NULL", *ppEff == 0u);
    ok("ppAdjacency set", *ppAdj != 0 && *ppAdj != 0xDEADu);

    uint32_t mesh = *ppMesh;
    uint32_t nf = call("ID3DXMesh::GetNumFaces", 1, (uint32_t[]){ mesh });
    uint32_t nv = call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){ mesh });
    uint32_t fvf = call("ID3DXMesh::GetFVF", 1, (uint32_t[]){ mesh });
    ok("NumFaces == 1", nf == 1u);
    ok("NumVertices == 3 (normals match indices)", nv == 3u);
    ok("FVF has XYZ|NORMAL|TEX1", fvf == (0x002u|0x010u|0x100u));

    /* verify VB contents */
    uint32_t *ppV=(uint32_t*)hle_alloc(4); *ppV=0;
    hr = call("ID3DXMesh::LockVertexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppV) });
    ok("LockVertexBuffer", hr == D3D_OK && *ppV);
    float *v = (float *)hle_ptr(*ppV);
    /* vertex 0: pos(0,0,0), normal(0,0,1), uv(0,0) */
    ok("v0 pos", feq(v[0],0)&&feq(v[1],0)&&feq(v[2],0));
    ok("v0 normal", feq(v[3],0)&&feq(v[4],0)&&feq(v[5],1));
    ok("v0 uv", feq(v[6],0)&&feq(v[7],0));
    /* vertex 1: pos(1,0,0), normal(0,0,1), uv(1,0) */
    ok("v1 pos", feq(v[8],1)&&feq(v[9],0)&&feq(v[10],0));
    ok("v1 uv", feq(v[14],1)&&feq(v[15],0));
    /* vertex 2: pos(0,1,0), uv(0,1) */
    ok("v2 pos", feq(v[16],0)&&feq(v[17],1)&&feq(v[18],0));
    ok("v2 uv", feq(v[22],0)&&feq(v[23],1));
    call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){ mesh });

    /* verify IB */
    uint32_t *ppI=(uint32_t*)hle_alloc(4); *ppI=0;
    hr = call("ID3DXMesh::LockIndexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppI) });
    ok("LockIndexBuffer", hr == D3D_OK && *ppI);
    uint16_t *ib = (uint16_t *)hle_ptr(*ppI);
    ok("indices 0,1,2", ib[0]==0 && ib[1]==1 && ib[2]==2);
    call("ID3DXMesh::UnlockIndexBuffer", 1, (uint32_t[]){ mesh });

    /* material buffer */
    uint32_t matPtr = call("dxb_GetBufferPointer", 1, (uint32_t[]){ *ppMat });
    uint32_t matSz = call("dxb_GetBufferSize", 1, (uint32_t[]){ *ppMat });
    ok("mat buffer size >= 72", matSz >= 72u);
    float *mat = (float *)hle_ptr(matPtr);
    ok("mat diffuse", feq(mat[0],1.0f)&&feq(mat[1],0.5f)&&feq(mat[2],0.25f)&&feq(mat[3],1.0f));
    ok("mat power", feq(mat[16],2.0f));
    ok("mat specular", feq(mat[8],0.1f)&&feq(mat[9],0.2f)&&feq(mat[10],0.3f));
    uint32_t texname = MEM32(matPtr + 68u);
    ok("mat texname ptr set", texname != 0);
    if (texname) ok("mat texname", strcmp((char*)hle_ptr(texname), "tex.png") == 0);

    /* ---- broken inputs ---- */
    uint32_t *ppM2=(uint32_t*)hle_alloc(4);
    void *bad; uint32_t hrb;
    bad = hle_alloc(sizeof(x_bad_header)); memcpy(bad,x_bad_header,sizeof(x_bad_header));
    *ppM2=0xDEAD; hrb = call("D3DXLoadMeshFromXInMemory", 9, (uint32_t[]){ A(bad),(uint32_t)(sizeof(x_bad_header)-1),0,dev,0,0,0,0,A(ppM2) });
    ok("bad header fails", hrb != D3D_OK && *ppM2 == 0);
    bad = hle_alloc(sizeof(x_binary)); memcpy(bad,x_binary,sizeof(x_binary));
    *ppM2=0xDEAD; hrb = call("D3DXLoadMeshFromXInMemory", 9, (uint32_t[]){ A(bad),(uint32_t)(sizeof(x_binary)-1),0,dev,0,0,0,0,A(ppM2) });
    ok("binary format fails", hrb != D3D_OK && *ppM2 == 0);
    bad = hle_alloc(sizeof(x_no_mesh)); memcpy(bad,x_no_mesh,sizeof(x_no_mesh));
    *ppM2=0xDEAD; hrb = call("D3DXLoadMeshFromXInMemory", 9, (uint32_t[]){ A(bad),(uint32_t)(sizeof(x_no_mesh)-1),0,dev,0,0,0,0,A(ppM2) });
    ok("no mesh fails", hrb != D3D_OK && *ppM2 == 0);
    bad = hle_alloc(sizeof(x_trunc)); memcpy(bad,x_trunc,sizeof(x_trunc));
    *ppM2=0xDEAD; hrb = call("D3DXLoadMeshFromXInMemory", 9, (uint32_t[]){ A(bad),(uint32_t)(sizeof(x_trunc)-1),0,dev,0,0,0,0,A(ppM2) });
    ok("truncated fails", hrb != D3D_OK && *ppM2 == 0);
    *ppM2=0xDEAD; hrb = call("D3DXLoadMeshFromXInMemory", 9, (uint32_t[]){ 0,0,0,dev,0,0,0,0,A(ppM2) });
    ok("NULL args fail", hrb != D3D_OK);

    /* ---- buffers with NO trailing NUL: strtol/strtod must stay in bounds ---- */
    {
        static const char *nonul[] = {
            "xof 0302txt 0064\nMesh { 3; 0.12345",       /* ends in digits */
            "xof 0302txt 0064\nMesh { 123456",           /* ends in digits */
            /* the known-good file above, with its trailing NUL removed */
            x_simple
        };
        for (int i = 0; i < 3; i++) {
            size_t n = (i == 2) ? sizeof(x_simple) - 1u : strlen(nonul[i]);
            void *b = hle_alloc((uint32_t)n);            /* exact size, deliberately no NUL */
            memcpy(b, nonul[i], n);
            uint32_t *out = (uint32_t *)hle_alloc(4); *out = 0xDEAD;
            uint32_t r = call("D3DXLoadMeshFromXInMemory", 9,
                (uint32_t[]){ hle_va(b), (uint32_t)n, 0, dev, 0, 0, 0, 0, hle_va(out) });
            if (i < 2) ok("non-NUL truncated input rejected", r != D3D_OK && *out == 0);
            else ok("non-NUL complete input accepted", r == D3D_OK && *out != 0);
            hle_free(b);
        }
    }

    /* ---- file A ---- */
    const char *tmpa = "/tmp/d3dx9_xfile_test.x";
    FILE *f = fopen(tmpa, "wb"); fwrite(x_simple, 1, sizeof(x_simple)-1, f); fclose(f);
    uint32_t *ppM3=(uint32_t*)hle_alloc(4); *ppM3=0;
    void *pa = hle_alloc(strlen(tmpa)+1); memcpy(pa, tmpa, strlen(tmpa)+1);
    hr = call("D3DXLoadMeshFromXA", 8, (uint32_t[]){ A(pa), 0, dev, 0,0,0,0, A(ppM3) });
    ok("LoadMeshFromXA hr", hr == D3D_OK && *ppM3 != 0);

    /* ---- file W ---- */
    uint32_t *ppM4=(uint32_t*)hle_alloc(4); *ppM4=0;
    uint16_t *pw = (uint16_t *)hle_alloc((strlen(tmpa)+1)*2);
    for (size_t i=0;i<=strlen(tmpa);i++) pw[i]=(uint16_t)(unsigned char)tmpa[i];
    hr = call("D3DXLoadMeshFromXW", 8, (uint32_t[]){ A(pw), 0, dev, 0,0,0,0, A(ppM4) });
    ok("LoadMeshFromXW hr", hr == D3D_OK && *ppM4 != 0);

    /* ---- read_int ERANGE / range check ---- */
    {
        /* oversized integer must be rejected, not silently truncated */
        static const char x_bigint[] =
            "xof 0302txt 0064\nMesh {\n 99999999999999999999;\n 0.0;0.0;0.0;;\n 0;\n}\n";
        uint32_t *po = (uint32_t *)hle_alloc(4); *po = 0xDEAD;
        void *b = hle_alloc(sizeof(x_bigint)-1); memcpy(b, x_bigint, sizeof(x_bigint)-1);
        uint32_t r = call("D3DXLoadMeshFromXInMemory", 9,
            (uint32_t[]){ A(b), (uint32_t)(sizeof(x_bigint)-1), 0, dev, 0,0,0,0, A(po) });
        ok("oversized int rejected", r != D3D_OK && *po == 0);
        /* INT_MIN-1 style: very negative */
        static const char x_neg[] =
            "xof 0302txt 0064\nMesh {\n -99999999999999999999;\n 0.0;0.0;0.0;;\n 0;\n}\n";
        *po = 0xDEAD;
        b = hle_alloc(sizeof(x_neg)-1); memcpy(b, x_neg, sizeof(x_neg)-1);
        r = call("D3DXLoadMeshFromXInMemory", 9,
            (uint32_t[]){ A(b), (uint32_t)(sizeof(x_neg)-1), 0, dev, 0,0,0,0, A(po) });
        ok("oversized negative int rejected", r != D3D_OK && *po == 0);
    }

    /* ---- LoadMeshHierarchyFromXInMemory / FrameDestroy ---- */
    {
        static const char x_hier[] =
            "xof 0302txt 0064\n"
            "Frame Root {\n"
            "  FrameTransformMatrix {\n"
            "    1.0,0.0,0.0,0.0,\n"
            "    0.0,1.0,0.0,0.0,\n"
            "    0.0,0.0,1.0,0.0,\n"
            "    1.0,2.0,3.0,1.0;;\n"
            "  }\n"
            "  Frame Child {\n"
            "    FrameTransformMatrix {\n"
            "      2.0,0.0,0.0,0.0,\n"
            "      0.0,2.0,0.0,0.0,\n"
            "      0.0,0.0,2.0,0.0,\n"
            "      0.0,0.0,0.0,1.0;;\n"
            "    }\n"
            "    Mesh {\n"
            "      3;\n"
            "      0.0;0.0;0.0;,\n"
            "      1.0;0.0;0.0;,\n"
            "      0.0;1.0;0.0;;\n"
            "      1;\n"
            "      3;0,1,2;;\n"
            "    }\n"
            "  }\n"
            "}\n";
        g_cf=g_cmc=g_df=g_dmc=g_cf_n=0;
        uint32_t alloc = make_alloc_hierarchy();
        uint32_t *ppF = (uint32_t *)hle_alloc(4); *ppF = 0xDEAD;
        uint32_t *ppA = (uint32_t *)hle_alloc(4); *ppA = 0xDEAD;
        void *b = hle_alloc(sizeof(x_hier)-1); memcpy(b, x_hier, sizeof(x_hier)-1);
        uint32_t hr = call("D3DXLoadMeshHierarchyFromXInMemory", 8,
            (uint32_t[]){ A(b), (uint32_t)(sizeof(x_hier)-1), 0, dev, alloc, 0, A(ppF), A(ppA) });
        ok("LoadMeshHierarchyFromXInMemory hr", hr == D3D_OK);
        ok("hierarchy root returned", *ppF != 0 && *ppF != 0xDEAD);
        ok("anim controller NULL", *ppA == 0);
        ok("CreateFrame called >=2", g_cf >= 2);
        ok("CreateMeshContainer called >=1", g_cmc >= 1);
        /* check names recorded */
        int saw_root = 0, saw_child = 0;
        for (int i=0;i<g_cf_n;i++) { if (strcmp(g_cf_names[i],"Root")==0) saw_root=1; if (strcmp(g_cf_names[i],"Child")==0) saw_child=1; }
        ok("CreateFrame name Root", saw_root);
        ok("CreateFrame name Child", saw_child);

        /* verify root frame matrix and child link */
        uint32_t root = *ppF;
        if (root) {
            float *m = (float*)hle_ptr(root+4);
            ok("root matrix [12]=1", feq(m[12], 1.0f));
            ok("root matrix [13]=2", feq(m[13], 2.0f));
            ok("root matrix [14]=3", feq(m[14], 3.0f));
            ok("root matrix [15]=1", feq(m[15], 1.0f));
            ok("root matrix [0]=1", feq(m[0], 1.0f));
            uint32_t child = MEM32(root + 76);
            ok("root has child", child != 0);
            if (child) {
                float *cm = (float*)hle_ptr(child+4);
                ok("child matrix [0]=2", feq(cm[0], 2.0f));
                ok("child matrix [5]=2", feq(cm[5], 2.0f));
                ok("child matrix [10]=2", feq(cm[10], 2.0f));
                uint32_t mc = MEM32(child + 68);
                ok("child has mesh container", mc != 0);
                if (mc) {
                    ok("mesh container type MESH", MEM32(mc+4) == 1);
                    ok("mesh container has mesh", MEM32(mc+8) != 0);
                }
            }
        }

        /* FrameDestroy */
        hr = call("D3DXFrameDestroy", 2, (uint32_t[]){ root, alloc });
        ok("FrameDestroy hr", hr == D3D_OK);
        ok("DestroyFrame called", g_df >= 2);
        ok("DestroyMeshContainer called", g_dmc >= 1);
    }

    /* ---- LoadMeshHierarchyFromXA/W ---- */
    {
        static const char x_hier2[] =
            "xof 0302txt 0064\n"
            "Frame A {\n"
            "  FrameTransformMatrix { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1;; }\n"
            "}\n";
        const char *tmp = "/tmp/d3dx9_xhier_test.x";
        FILE *f = fopen(tmp, "wb"); fwrite(x_hier2, 1, sizeof(x_hier2)-1, f); fclose(f);
        uint32_t alloc = make_alloc_hierarchy();
        uint32_t *ppF = (uint32_t *)hle_alloc(4); *ppF = 0;
        void *pa = hle_alloc(strlen(tmp)+1); memcpy(pa, tmp, strlen(tmp)+1);
        uint32_t hr = call("D3DXLoadMeshHierarchyFromXA", 7, (uint32_t[]){ A(pa), 0, dev, alloc, 0, A(ppF), 0 });
        ok("LoadMeshHierarchyFromXA hr", hr == D3D_OK && *ppF != 0);
        if (*ppF) call("D3DXFrameDestroy", 2, (uint32_t[]){ *ppF, alloc });

        /* W */
        uint16_t *pw = (uint16_t *)hle_alloc((strlen(tmp)+1)*2);
        for (size_t i=0;i<=strlen(tmp);i++) pw[i]=(uint16_t)(unsigned char)tmp[i];
        alloc = make_alloc_hierarchy();
        *ppF = 0;
        hr = call("D3DXLoadMeshHierarchyFromXW", 7, (uint32_t[]){ A(pw), 0, dev, alloc, 0, A(ppF), 0 });
        ok("LoadMeshHierarchyFromXW hr", hr == D3D_OK && *ppF != 0);
        if (*ppF) call("D3DXFrameDestroy", 2, (uint32_t[]){ *ppF, alloc });
        remove(tmp);
    }

    /* ---- FrameDestroy NULL is OK, NULL alloc is invalid ---- */
    {
        uint32_t hr = call("D3DXFrameDestroy", 2, (uint32_t[]){ 0, 0 });
        ok("FrameDestroy(NULL,0) OK", hr == D3D_OK);
        uint32_t dummy = hle_va(hle_alloc(80)); memset(hle_ptr(dummy), 0, 80);
        hr = call("D3DXFrameDestroy", 2, (uint32_t[]){ dummy, 0 });
        ok("FrameDestroy(frame,NULL) INVALIDCALL", hr == D3DERR_INVALIDCALL);
    }

    printf(fails == 0 ? "d3dx9_xfile_selftest: all checks passed\\n" : "d3dx9_xfile_selftest: FAILED\\n");
    return fails != 0;
}
