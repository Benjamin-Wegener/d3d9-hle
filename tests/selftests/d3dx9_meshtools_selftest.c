/*
 * d3dx9_meshtools_selftest.c - tests for D3DX mesh utility functions.
 * Expected values are computed by hand/independent math, not read from impl.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *w, int c) { if (!c) { printf("FAIL %s\n", w); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define A(p) hle_va(p)
static int feq(float a,float b){ return fabsf(a-b) < 1e-4f; }

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

    /* IntersectTri: ray along +Z hitting triangle at z=0 */
    {
        float *v0=(float*)hle_alloc(12), *v1=(float*)hle_alloc(12), *v2=(float*)hle_alloc(12);
        float *ro=(float*)hle_alloc(12), *rd=(float*)hle_alloc(12);
        v0[0]=0;v0[1]=0;v0[2]=0; v1[0]=1;v1[1]=0;v1[2]=0; v2[0]=0;v2[1]=1;v2[2]=0;
        ro[0]=0.25f;ro[1]=0.25f;ro[2]=-1; rd[0]=0;rd[1]=0;rd[2]=1;
        float *u=(float*)hle_alloc(4), *v=(float*)hle_alloc(4), *d=(float*)hle_alloc(4);
        uint32_t hit = call("D3DXIntersectTri", 8, (uint32_t[]){ A(v0),A(v1),A(v2),A(ro),A(rd),A(u),A(v),A(d) });
        ok("IntersectTri hit", hit == 1);
        ok("IntersectTri u", feq(*u, 0.25f));
        ok("IntersectTri v", feq(*v, 0.25f));
        ok("IntersectTri dist", feq(*d, 1.0f));
        /* miss */
        float *ro2=(float*)hle_alloc(12); ro2[0]=2;ro2[1]=2;ro2[2]=-1;
        hit = call("D3DXIntersectTri", 8, (uint32_t[]){ A(v0),A(v1),A(v2),A(ro2),A(rd),A(u),A(v),A(d) });
        ok("IntersectTri miss", hit == 0);
    }

    /* SphereBoundProbe */
    {
        float *c=(float*)hle_alloc(12), *ro3=(float*)hle_alloc(12), *rd3=(float*)hle_alloc(12);
        c[0]=0;c[1]=0;c[2]=5; ro3[0]=0;ro3[1]=0;ro3[2]=0; rd3[0]=0;rd3[1]=0;rd3[2]=1;
        uint32_t hit = call("D3DXSphereBoundProbe", 4, (uint32_t[]){ A(c), 0x3f800000u /*1.0f*/, A(ro3), A(rd3) });
        ok("SphereBoundProbe hit", hit == 1);
        float *rd4=(float*)hle_alloc(12); rd4[0]=1;rd4[1]=0;rd4[2]=0;
        hit = call("D3DXSphereBoundProbe", 4, (uint32_t[]){ A(c), 0x3f800000u, A(ro3), A(rd4) });
        ok("SphereBoundProbe miss", hit == 0);
    }

    /* BoxBoundProbe */
    {
        float *mn=(float*)hle_alloc(12), *mx=(float*)hle_alloc(12), *ro4=(float*)hle_alloc(12), *rd5=(float*)hle_alloc(12);
        mn[0]=-1;mn[1]=-1;mn[2]=-1; mx[0]=1;mx[1]=1;mx[2]=1; ro4[0]=0;ro4[1]=0;ro4[2]=-5; rd5[0]=0;rd5[1]=0;rd5[2]=1;
        uint32_t hit = call("D3DXBoxBoundProbe", 4, (uint32_t[]){ A(mn),A(mx),A(ro4),A(rd5) });
        ok("BoxBoundProbe hit", hit == 1);
        float *rd6=(float*)hle_alloc(12); rd6[0]=0;rd6[1]=1;rd6[2]=0;
        hit = call("D3DXBoxBoundProbe", 4, (uint32_t[]){ A(mn),A(mx),A(ro4),A(rd6) });
        ok("BoxBoundProbe miss", hit == 0);
    }

    /* CreatePolygon: 4 sides, length 2 */
    {
        uint32_t *ppMesh=(uint32_t*)hle_alloc(4), *ppAdj=(uint32_t*)hle_alloc(4); *ppMesh=0; *ppAdj=0;
        uint32_t hr = call("D3DXCreatePolygon", 5, (uint32_t[]){ dev, 0x40000000u/*2.0f*/, 4u, A(ppMesh), A(ppAdj) });
        ok("CreatePolygon hr", hr == D3D_OK);
        ok("CreatePolygon mesh", *ppMesh != 0);
        uint32_t nf = call("ID3DXMesh::GetNumFaces", 1, (uint32_t[]){ *ppMesh });
        uint32_t nv = call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){ *ppMesh });
        uint32_t fvf = call("ID3DXMesh::GetFVF", 1, (uint32_t[]){ *ppMesh });
        ok("CreatePolygon faces==4", nf == 4);
        ok("CreatePolygon verts==5", nv == 5);
        ok("CreatePolygon fvf", fvf == (0x002u|0x010u));
        uint32_t *ppV=(uint32_t*)hle_alloc(4); *ppV=0;
        call("ID3DXMesh::LockVertexBuffer", 3, (uint32_t[]){ *ppMesh, 0, A(ppV) });
        float *vtx = (float*)hle_ptr(*ppV);
        /* center at origin, normal +Z */
        ok("poly center", feq(vtx[0],0)&&feq(vtx[1],0)&&feq(vtx[2],0)&&feq(vtx[5],1));
        /* first ring vertex at angle 0: (2,0,0) */
        ok("poly v1", feq(vtx[6],2)&&feq(vtx[7],0)&&feq(vtx[8],0));
        call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){ *ppMesh });
    }

    /* OptimizeFaces/Vertices identity */
    {
        uint32_t *remap = (uint32_t*)hle_alloc(4*3); remap[0]=9;remap[1]=9;remap[2]=9;
        uint32_t hr = call("D3DXOptimizeFaces", 5, (uint32_t[]){ 0, 3, 4, 0, A(remap) });
        ok("OptimizeFaces hr", hr == D3D_OK);
        ok("OptimizeFaces identity", remap[0]==0 && remap[1]==1 && remap[2]==2);
        uint32_t *vr = (uint32_t*)hle_alloc(4*4); vr[0]=9;vr[1]=9;vr[2]=9;vr[3]=9;
        hr = call("D3DXOptimizeVertices", 5, (uint32_t[]){ 0, 2, 4, 0, A(vr) });
        ok("OptimizeVertices hr", hr == D3D_OK);
        ok("OptimizeVertices identity", vr[0]==0&&vr[1]==1&&vr[2]==2&&vr[3]==3);
    }

    /* ComputeNormals on a simple mesh */
    {
        uint32_t *ppMesh=(uint32_t*)hle_alloc(4); *ppMesh=0;
        /* create mesh: 1 face, 3 verts, XYZ|NORMAL */
        uint32_t hr = call("D3DXCreateMeshFVF", 6, (uint32_t[]){ 1,3,0, (0x002u|0x010u), dev, A(ppMesh) });
        ok("CreateMeshFVF for normals", hr==D3D_OK && *ppMesh);
        uint32_t mesh=*ppMesh;
        uint32_t *ppV=(uint32_t*)hle_alloc(4), *ppI=(uint32_t*)hle_alloc(4); *ppV=*ppI=0;
        call("ID3DXMesh::LockVertexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppV) });
        call("ID3DXMesh::LockIndexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppI) });
        float *vtx=(float*)hle_ptr(*ppV); uint16_t *ib=(uint16_t*)hle_ptr(*ppI);
        /* v0=(0,0,0), v1=(1,0,0), v2=(0,1,0), normals zero */
        memset(vtx,0,3*6*4);
        vtx[0]=0;vtx[1]=0;vtx[2]=0; vtx[6]=1;vtx[7]=0;vtx[8]=0; vtx[12]=0;vtx[13]=1;vtx[14]=0;
        ib[0]=0;ib[1]=1;ib[2]=2;
        call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){ mesh });
        call("ID3DXMesh::UnlockIndexBuffer", 1, (uint32_t[]){ mesh });
        hr = call("D3DXComputeNormals", 2, (uint32_t[]){ mesh, 0 });
        ok("ComputeNormals hr", hr==D3D_OK);
        *ppV=0; call("ID3DXMesh::LockVertexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppV) });
        vtx=(float*)hle_ptr(*ppV);
        /* normal should be (0,0,1) for all verts */
        ok("ComputeNormals n0", feq(vtx[3],0)&&feq(vtx[4],0)&&feq(vtx[5],1));
        ok("ComputeNormals n1", feq(vtx[9],0)&&feq(vtx[10],0)&&feq(vtx[11],1));
        ok("ComputeNormals n2", feq(vtx[15],0)&&feq(vtx[16],0)&&feq(vtx[17],1));
        call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){ mesh });
    }

    /* Intersect on mesh */
    {
        uint32_t *ppMesh=(uint32_t*)hle_alloc(4); *ppMesh=0;
        call("D3DXCreateMeshFVF", 6, (uint32_t[]){ 1,3,0, 0x002u, dev, A(ppMesh) });
        uint32_t mesh=*ppMesh;
        uint32_t *ppV=(uint32_t*)hle_alloc(4), *ppI=(uint32_t*)hle_alloc(4); *ppV=*ppI=0;
        call("ID3DXMesh::LockVertexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppV) });
        call("ID3DXMesh::LockIndexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppI) });
        float *vtx=(float*)hle_ptr(*ppV); uint16_t *ib=(uint16_t*)hle_ptr(*ppI);
        memset(vtx,0,3*3*4); vtx[0]=0;vtx[1]=0;vtx[2]=0; vtx[3]=1;vtx[4]=0;vtx[5]=0; vtx[6]=0;vtx[7]=1;vtx[8]=0;
        ib[0]=0;ib[1]=1;ib[2]=2;
        call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){ mesh });
        call("ID3DXMesh::UnlockIndexBuffer", 1, (uint32_t[]){ mesh });
        float *ro5=(float*)hle_alloc(12), *rd7=(float*)hle_alloc(12);
        ro5[0]=0.25f;ro5[1]=0.25f;ro5[2]=-1; rd7[0]=0;rd7[1]=0;rd7[2]=1;
        uint32_t *pHit=(uint32_t*)hle_alloc(4), *pFace=(uint32_t*)hle_alloc(4);
        float *pu=(float*)hle_alloc(4),*pv=(float*)hle_alloc(4),*pd=(float*)hle_alloc(4);
        *pHit=0;*pFace=9;*pu=*pv=*pd=0;
        uint32_t hr = call("D3DXIntersect", 10, (uint32_t[]){ mesh,A(ro5),A(rd7),A(pHit),A(pFace),A(pu),A(pv),A(pd),0,0 });
        ok("Intersect hr", hr==D3D_OK);
        ok("Intersect hit", *pHit==1);
        ok("Intersect face", *pFace==0);
        ok("Intersect u", feq(*pu,0.25f));
        ok("Intersect v", feq(*pv,0.25f));
        ok("Intersect dist", feq(*pd,1.0f));
    }

    /* WeldVertices basic */
    {
        uint32_t *ppMesh=(uint32_t*)hle_alloc(4); *ppMesh=0;
        call("D3DXCreateMeshFVF", 6, (uint32_t[]){ 2,4,0, 0x002u, dev, A(ppMesh) });
        uint32_t mesh=*ppMesh;
        uint32_t *ppV=(uint32_t*)hle_alloc(4), *ppI=(uint32_t*)hle_alloc(4); *ppV=*ppI=0;
        call("ID3DXMesh::LockVertexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppV) });
        call("ID3DXMesh::LockIndexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppI) });
        float *vtx=(float*)hle_ptr(*ppV); uint16_t *ib=(uint16_t*)hle_ptr(*ppI);
        memset(vtx,0,4*3*4);
        /* two triangles sharing edge v1-v2 but duplicated as v1/v3 */
        vtx[0]=0;vtx[1]=0;vtx[2]=0; vtx[3]=1;vtx[4]=0;vtx[5]=0; vtx[6]=0;vtx[7]=1;vtx[8]=0; vtx[9]=1;vtx[10]=0;vtx[11]=0; /* v3==v1 */
        ib[0]=0;ib[1]=1;ib[2]=2; ib[3]=0;ib[4]=3;ib[5]=2;
        call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){ mesh });
        call("ID3DXMesh::UnlockIndexBuffer", 1, (uint32_t[]){ mesh });
        uint32_t *ppRemap=(uint32_t*)hle_alloc(4); *ppRemap=0;
        uint32_t hr = call("D3DXWeldVertices", 7, (uint32_t[]){ mesh, 0, 0, 0, 0, 0, A(ppRemap) });
        ok("WeldVertices hr", hr==D3D_OK);
        ok("WeldVertices remap buffer", *ppRemap != 0);
        if (*ppRemap) {
            uint32_t ptr = call("dxb_GetBufferPointer", 1, (uint32_t[]){ *ppRemap });
            uint32_t *rm = (uint32_t*)hle_ptr(ptr);
            ok("WeldVertices remap v1==v3", rm[1] == rm[3]);
            ok("WeldVertices remap v0", rm[0]==0);
        }
        *ppI=0; call("ID3DXMesh::LockIndexBuffer", 3, (uint32_t[]){ mesh, 0, A(ppI) });
        ib=(uint16_t*)hle_ptr(*ppI);
        ok("WeldVertices ib welded", ib[4] == ib[1]);
        call("ID3DXMesh::UnlockIndexBuffer", 1, (uint32_t[]){ mesh });
    }

    printf(fails == 0 ? "d3dx9_meshtools_selftest: all checks passed\n" : "d3dx9_meshtools_selftest: FAILED\n");
    return fails != 0;
}
