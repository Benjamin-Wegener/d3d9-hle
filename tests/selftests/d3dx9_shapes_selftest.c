#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_shapes.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static uint32_t g_dev;

static int fails;
typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t va);
extern recomp_func_t recomp_lookup_import(uint32_t va);
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static float dot3(const float *a, const float *b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void cross3(const float *a, const float *b, float *o) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}

static void check_mesh(const char *name, uint32_t mesh, uint32_t expectVerts, uint32_t expectFaces) {
    uint32_t nv = call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){mesh});
    uint32_t nf = call("ID3DXMesh::GetNumFaces", 1, (uint32_t[]){mesh});
    char buf[128];
    snprintf(buf, sizeof buf, "%s NumVertices", name); ok(buf, nv == expectVerts);
    snprintf(buf, sizeof buf, "%s NumFaces", name); ok(buf, nf == expectFaces);
    ok("GetFVF XYZ|NORMAL", call("ID3DXMesh::GetFVF", 1, (uint32_t[]){mesh}) == 0x00000012u);
    ok("GetNumBytesPerVertex 24", call("ID3DXMesh::GetNumBytesPerVertex", 1, (uint32_t[]){mesh}) == 24u);
    /* GetDeclaration */
    {
        uint8_t *decl = (uint8_t *)hle_alloc(64); memset(decl, 0, 64);
        uint32_t da[2] = { mesh, (uint32_t)hle_va(decl) };
        ok("GetDeclaration hr", call("ID3DXMesh::GetDeclaration", 2, da) == D3D_OK);
        uint16_t s0,o0,s1,o1,se; uint8_t t0,u0,t1,u1,te;
        memcpy(&s0,decl+0,2); memcpy(&o0,decl+2,2); t0=decl[4]; u0=decl[6];
        memcpy(&s1,decl+8,2); memcpy(&o1,decl+10,2); t1=decl[12]; u1=decl[14];
        memcpy(&se,decl+16,2); te=decl[20];
        ok("GetDeclaration pos", s0==0 && o0==0 && t0==2 && u0==0);
        ok("GetDeclaration normal", s1==0 && o1==12 && t1==2 && u1==3);
        ok("GetDeclaration end", se==0xFF && te==17);
    }
    /* DrawSubset ABI (dev may be 0 in selftest host) */
    ok("DrawSubset hr", call("ID3DXMesh::DrawSubset", 2, (uint32_t[]){mesh, 0}) == D3D_OK);
}

static void check_normals_winding(const char *name, uint32_t mesh) {
    uint32_t nv = call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){mesh});
    uint32_t nf = call("ID3DXMesh::GetNumFaces", 1, (uint32_t[]){mesh});
    uint32_t *pvb = (uint32_t *)hle_alloc(4);
    uint32_t *pib = (uint32_t *)hle_alloc(4);
    ok("LockVB", call("ID3DXMesh::LockVertexBuffer", 3, (uint32_t[]){mesh,0,(uint32_t)hle_va(pvb)}) == D3D_OK);
    ok("LockIB", call("ID3DXMesh::LockIndexBuffer", 3, (uint32_t[]){mesh,0,(uint32_t)hle_va(pib)}) == D3D_OK);
    const float *vb = (const float *)hle_ptr(*pvb);
    const uint16_t *ib = (const uint16_t *)hle_ptr(*pib);
    char buf[128];
    /* Normals should be unit length */
    int normals_ok = 1;
    for (uint32_t i = 0; i < nv; i++) {
        const float *n = vb + i*6 + 3;
        float l = sqrtf(dot3(n,n));
        if (fabsf(l - 1.0f) > 1e-3f) { normals_ok = 0; break; }
    }
    snprintf(buf, sizeof buf, "%s normals unit", name); ok(buf, normals_ok);
    /* Winding: D3D9 front faces are CW as seen from outside.
     * For a CW front face in D3D9's left-handed convention, the geometric
     * cross (v1-v0)x(v2-v0) points opposite to the outward face normal. */
    int winding_ok = 1;
    int winding_consistent = 1;
    float sign = 0.0f;
    for (uint32_t f = 0; f < nf; f++) {
        uint16_t i0=ib[f*3+0], i1=ib[f*3+1], i2=ib[f*3+2];
        if (i0>=nv||i1>=nv||i2>=nv) { winding_ok = 0; break; }
        const float *v0=vb+i0*6, *v1p=vb+i1*6, *v2p=vb+i2*6;
        float e1[3]={v1p[0]-v0[0],v1p[1]-v0[1],v1p[2]-v0[2]};
        float e2[3]={v2p[0]-v0[0],v2p[1]-v0[1],v2p[2]-v0[2]};
        float cr[3]; cross3(e1,e2,cr);
        const float *n0 = v0+3;
        float d = dot3(cr,n0);
        if (fabsf(d) < 1e-4f) continue; /* skip degenerate */
        float s = d < 0 ? -1.0f : 1.0f;
        if (sign == 0.0f) sign = s;
        else if (s != sign) { winding_consistent = 0; }
        /* D3DX CW front face: cross opposite to outward normal => dot < 0 */
        if (d >= 0) winding_ok = 0;
    }
    snprintf(buf, sizeof buf, "%s winding consistent", name); ok(buf, winding_consistent);
    snprintf(buf, sizeof buf, "%s winding CW/front (cross·normal<0)", name); ok(buf, winding_ok);
    call("ID3DXMesh::UnlockIndexBuffer", 1, (uint32_t[]){mesh});
    call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){mesh});
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    win32hle_register(win32hle_d3dx9);
    g_dev = call("Direct3DCreate9", 1, (uint32_t[]){32});
    if (g_dev) {
        uint32_t *pDev = (uint32_t *)hle_alloc(4); *pDev = 0;
        uint32_t pp = (uint32_t)hle_alloc(56); memset(hle_ptr(pp), 0, 56);
        /* D3DPRESENT_PARAMETERS: BackBufferWidth/Height/Format/Count/SwapEffect/hDeviceWindow/Windowed */
        MEM32(pp+0)=64; MEM32(pp+4)=64; MEM32(pp+8)=21; MEM32(pp+12)=1; MEM32(pp+16)=1; MEM32(pp+24)=1;
        uint32_t a[7] = { g_dev, 0, 1, 0, 0x00000040u, pp, (uint32_t)hle_va(pDev) }; /* HAL, HWVP */
        call("IDirect3D9::CreateDevice", 7, a);
        g_dev = *pDev;
    }

    /* ---- D3DXCreateBox ---- */
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[6] = { g_dev, f2u(2.0f), f2u(4.0f), f2u(6.0f), (uint32_t)hle_va(pp), 0 };
        uint32_t hr = call("D3DXCreateBox", 6, a);
        ok("CreateBox hr", hr == D3D_OK);
        ok("CreateBox ppMesh", *pp != 0);
        uint32_t mesh = *pp;
        check_mesh("Box", mesh, 24, 12);
        check_normals_winding("Box", mesh);
        /* Box bounds: width 2 (x -1..1), height 4 (y -2..2), depth 6 (z -3..3) */
        uint32_t *pvb = (uint32_t *)hle_alloc(4);
        call("ID3DXMesh::LockVertexBuffer", 3, (uint32_t[]){mesh,0,(uint32_t)hle_va(pvb)});
        uint32_t minv = hle_va(hle_alloc(12)), maxv = hle_va(hle_alloc(12));
        uint32_t abb[5] = { *pvb, 24, 24, minv, maxv };
        ok("ComputeBoundingBox hr", call("D3DXComputeBoundingBox", 5, abb) == D3D_OK);
        float *mn=(float*)hle_ptr(minv), *mx=(float*)hle_ptr(maxv);
        ok("Box min.x", fabsf(mn[0]+1.0f)<1e-4f);
        ok("Box min.y", fabsf(mn[1]+2.0f)<1e-4f);
        ok("Box min.z", fabsf(mn[2]+3.0f)<1e-4f);
        ok("Box max.x", fabsf(mx[0]-1.0f)<1e-4f);
        ok("Box max.y", fabsf(mx[1]-2.0f)<1e-4f);
        ok("Box max.z", fabsf(mx[2]-3.0f)<1e-4f);
        uint32_t cen = hle_va(hle_alloc(12)), rad = hle_va(hle_alloc(4));
        uint32_t abs2[5] = { *pvb, 24, 24, cen, rad };
        ok("ComputeBoundingSphere hr", call("D3DXComputeBoundingSphere", 5, abs2) == D3D_OK);
        float *c=(float*)hle_ptr(cen);
        ok("Box sphere center ~0", fabsf(c[0])<1e-4f && fabsf(c[1])<1e-4f && fabsf(c[2])<1e-4f);
        ok("Box sphere radius > 0", *(float*)hle_ptr(rad) > 3.0f);
        call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){mesh});
    }

    /* ---- D3DXCreateSphere ---- */
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[6] = { g_dev, f2u(2.0f), 8, 6, (uint32_t)hle_va(pp), 0 };
        uint32_t hr = call("D3DXCreateSphere", 6, a);
        ok("CreateSphere hr", hr == D3D_OK);
        ok("CreateSphere ppMesh", *pp != 0);
        uint32_t mesh = *pp;
        /* (stacks+1)*(slices+1)=7*9=63 verts, stacks*slices*2=96 faces */
        check_mesh("Sphere", mesh, 63, 96);
        check_normals_winding("Sphere", mesh);
        /* normals should be radial */
        uint32_t *pvb=(uint32_t*)hle_alloc(4);
        call("ID3DXMesh::LockVertexBuffer",3,(uint32_t[]){mesh,0,(uint32_t)hle_va(pvb)});
        const float *vb=(const float*)hle_ptr(*pvb);
        int radial_ok=1;
        for (uint32_t i=0;i<63;i++) {
            const float *p=vb+i*6, *n=p+3;
            float plen=sqrtf(dot3(p,p));
            if (plen < 1e-4f) continue;
            /* normal should be close to position/radius */
            if (fabsf(n[0]-p[0]/2.0f)>1e-3f || fabsf(n[1]-p[1]/2.0f)>1e-3f || fabsf(n[2]-p[2]/2.0f)>1e-3f) { radial_ok=0; break; }
        }
        ok("Sphere normals radial", radial_ok);
        call("ID3DXMesh::UnlockVertexBuffer",1,(uint32_t[]){mesh});
    }

    /* ---- D3DXCreateCylinder ---- */
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[8] = { g_dev, f2u(1.0f), f2u(0.5f), f2u(2.0f), 8, 2, (uint32_t)hle_va(pp), 0 };
        uint32_t hr = call("D3DXCreateCylinder", 8, a);
        ok("CreateCylinder hr", hr == D3D_OK);
        ok("CreateCylinder ppMesh", *pp != 0);
        uint32_t mesh = *pp;
        /* body (stacks+1)*(slices+1)=3*9=27, 2 caps * (1+slices)=18, total 45 verts
         * body stacks*slices*2=32, 2 caps * slices=16, total 48 faces */
        check_mesh("Cylinder", mesh, 45, 48);
        check_normals_winding("Cylinder", mesh);
    }

    /* ---- D3DXCreateTorus ---- */
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[7] = { g_dev, f2u(0.5f), f2u(2.0f), 8, 12, (uint32_t)hle_va(pp), 0 };
        uint32_t hr = call("D3DXCreateTorus", 7, a);
        ok("CreateTorus hr", hr == D3D_OK);
        ok("CreateTorus ppMesh", *pp != 0);
        uint32_t mesh = *pp;
        /* (rings+1)*(sides+1)=13*9=117 verts, rings*sides*2=192 faces */
        check_mesh("Torus", mesh, 117, 192);
        check_normals_winding("Torus", mesh);
    }

    /* ---- D3DXGetFVFVertexSize ---- */
    ok("FVF XYZ|NORMAL size 24", call("D3DXGetFVFVertexSize", 1, (uint32_t[]){0x00000012u}) == 24u);
    ok("FVF XYZ size 12", call("D3DXGetFVFVertexSize", 1, (uint32_t[]){0x00000002u}) == 12u);
    ok("FVF XYZRHW|DIFFUSE size 20", call("D3DXGetFVFVertexSize", 1, (uint32_t[]){0x00000044u}) == 20u);

    /* ---- D3DXDeclaratorFromFVF ---- */
    {
        uint8_t *decl = (uint8_t *)hle_alloc(64);
        memset(decl, 0xAA, 64);
        uint32_t a[2] = { 0x00000012u, (uint32_t)hle_va(decl) };
        ok("DeclaratorFromFVF hr", call("D3DXDeclaratorFromFVF", 2, a) == D3D_OK);
        /* element 0: POSITION FLOAT3 */
        uint16_t stream0, off0; uint8_t type0, method0, usage0, ui0;
        memcpy(&stream0, decl+0,2); memcpy(&off0, decl+2,2);
        type0=decl[4]; method0=decl[5]; usage0=decl[6]; ui0=decl[7];
        ok("Decl[0] pos", stream0==0 && off0==0 && type0==2 && method0==0 && usage0==0 && ui0==0);
        /* element 1: NORMAL FLOAT3 */
        uint16_t stream1, off1; uint8_t type1, method1, usage1, ui1;
        memcpy(&stream1, decl+8,2); memcpy(&off1, decl+10,2);
        type1=decl[12]; method1=decl[13]; usage1=decl[14]; ui1=decl[15];
        ok("Decl[1] normal", stream1==0 && off1==12 && type1==2 && method1==0 && usage1==3 && ui1==0);
        /* element 2: END */
        uint16_t stream2, off2; uint8_t type2;
        memcpy(&stream2, decl+16,2); memcpy(&off2, decl+18,2); type2=decl[20];
        ok("Decl[2] end", stream2==0xFF && off2==0 && type2==17);
    }

    /* ---- Null/invalid ABI checks ---- */
    {
        uint32_t a[6] = { g_dev, f2u(1), f2u(1), f2u(1), 0, 0 };
        ok("CreateBox null ppMesh", call("D3DXCreateBox", 6, a) == D3DERR_INVALIDCALL);
    }
    {
        uint32_t a[6] = { g_dev, f2u(1), 8, 8, 0, 0 };
        ok("CreateSphere null ppMesh", call("D3DXCreateSphere", 6, a) == D3DERR_INVALIDCALL);
    }
    {
        uint32_t a[8] = { g_dev, f2u(1), f2u(1), f2u(1), 8, 1, 0, 0 };
        ok("CreateCylinder null ppMesh", call("D3DXCreateCylinder", 8, a) == D3DERR_INVALIDCALL);
    }
    {
        uint32_t a[7] = { g_dev, f2u(1), f2u(2), 8, 8, 0, 0 };
        ok("CreateTorus null ppMesh", call("D3DXCreateTorus", 7, a) == D3DERR_INVALIDCALL);
    }

    /* ---- Rejection-regression: invalid params must be D3DERR_INVALIDCALL ---- */
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[6] = { 0, f2u(1), f2u(1), f2u(1), (uint32_t)hle_va(pp), 0 };
        ok("CreateBox dev==0", call("D3DXCreateBox", 6, a) == D3DERR_INVALIDCALL);
    }
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[6] = { g_dev, f2u(-1), f2u(1), f2u(1), (uint32_t)hle_va(pp), 0 };
        ok("CreateBox negative width", call("D3DXCreateBox", 6, a) == D3DERR_INVALIDCALL);
    }
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[6] = { 0, f2u(1), 8, 8, (uint32_t)hle_va(pp), 0 };
        ok("CreateSphere dev==0", call("D3DXCreateSphere", 6, a) == D3DERR_INVALIDCALL);
        a[0]=g_dev; a[1]=f2u(-1.0f);
        ok("CreateSphere negative radius", call("D3DXCreateSphere", 6, a) == D3DERR_INVALIDCALL);
        a[1]=f2u(1.0f); a[2]=2; a[3]=8;
        ok("CreateSphere slices<3", call("D3DXCreateSphere", 6, a) == D3DERR_INVALIDCALL);
        a[2]=8; a[3]=2;
        ok("CreateSphere stacks<3", call("D3DXCreateSphere", 6, a) == D3DERR_INVALIDCALL);
    }
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[8] = { 0, f2u(1), f2u(1), f2u(1), 8, 1, (uint32_t)hle_va(pp), 0 };
        ok("CreateCylinder dev==0", call("D3DXCreateCylinder", 8, a) == D3DERR_INVALIDCALL);
        a[0]=g_dev; a[1]=f2u(-1);
        ok("CreateCylinder negative radius1", call("D3DXCreateCylinder", 8, a) == D3DERR_INVALIDCALL);
        a[1]=f2u(1); a[4]=2;
        ok("CreateCylinder slices<3", call("D3DXCreateCylinder", 8, a) == D3DERR_INVALIDCALL);
    }
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[7] = { 0, f2u(1), f2u(2), 8, 8, (uint32_t)hle_va(pp), 0 };
        ok("CreateTorus dev==0", call("D3DXCreateTorus", 7, a) == D3DERR_INVALIDCALL);
        a[0]=g_dev; a[1]=f2u(-1);
        ok("CreateTorus negative innerRadius", call("D3DXCreateTorus", 7, a) == D3DERR_INVALIDCALL);
        a[1]=f2u(1); a[3]=2;
        ok("CreateTorus sides<3", call("D3DXCreateTorus", 7, a) == D3DERR_INVALIDCALL);
    }

    /* ---- Cylinder len==0 is valid and must not produce NaN normals ---- */
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t a[8] = { g_dev, f2u(1.0f), f2u(1.0f), f2u(0.0f), 8, 1, (uint32_t)hle_va(pp), 0 };
        uint32_t hr = call("D3DXCreateCylinder", 8, a);
        ok("CreateCylinder len==0 hr", hr == D3D_OK && *pp != 0);
        if (hr == D3D_OK && *pp) {
            uint32_t *pvb=(uint32_t*)hle_alloc(4);
            call("ID3DXMesh::LockVertexBuffer", 3, (uint32_t[]){*pp,0,(uint32_t)hle_va(pvb)});
            uint32_t nv = call("ID3DXMesh::GetNumVertices", 1, (uint32_t[]){*pp});
            const float *vb=(const float*)hle_ptr(*pvb);
            int no_nan = 1;
            for (uint32_t i=0;i<nv*6u;i++) { if (vb[i]!=vb[i]) { no_nan=0; break; } }
            ok("Cylinder len==0 no NaN normals/verts", no_nan);
            call("ID3DXMesh::UnlockVertexBuffer", 1, (uint32_t[]){*pp});
        }
    }

    /* ---- ppAdjacency: Box writes 36 DWORDs ---- */
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t *ppAdj = (uint32_t *)hle_alloc(4); *ppAdj = 0;
        uint32_t a[6] = { g_dev, f2u(1), f2u(1), f2u(1), (uint32_t)hle_va(pp), (uint32_t)hle_va(ppAdj) };
        uint32_t hr = call("D3DXCreateBox", 6, a);
        ok("CreateBox with adjacency hr", hr == D3D_OK);
        ok("CreateBox adjacency is an ID3DXBuffer", *ppAdj != 0);
        if (*ppAdj) {
            /* A box is closed: its 12 triangles have 36 edges and each edge has exactly one neighbour
             * (the quad's other triangle, or a triangle of the next side: the vertices of the sides
             * are duplicated but lie at the same points). */
            uint32_t buf = *ppAdj, vt = MEM32(buf);
            ok("adjacency buffer size 36 DWORDs", hle_call_guest(MEM32(vt + 4u * 4u), 1, (uint32_t[]){buf}) == 144u);
            const uint32_t *adj = (const uint32_t *)hle_ptr(hle_call_guest(MEM32(vt + 4u * 3u), 1, (uint32_t[]){buf}));
            int all_valid = 1, mutual = 1;
            for (int i = 0; i < 36; i++) {
                if (adj[i] >= 12u) { all_valid = 0; continue; }
                int back = 0;
                for (int e = 0; e < 3; e++) if (adj[adj[i] * 3u + (uint32_t)e] == (uint32_t)(i / 3)) back = 1;
                if (!back) mutual = 0;
            }
            ok("every edge of the closed box has a neighbour", all_valid);
            ok("neighbours are mutual", mutual);
            ok("the two triangles of side 0 are neighbours across the diagonal", adj[0 * 3 + 2] == 1u || adj[0 * 3 + 0] == 1u || adj[0 * 3 + 1] == 1u);
            hle_call_guest(MEM32(vt + 4u * 2u), 1, (uint32_t[]){buf});
        }
    }

    /* ---- COM-CHECKLISTE: every ID3DXMesh slot via guest vtable, esp checked.
     * Order/source: public D3DX9 header d3dx9mesh.h (apitrace/dxsdk, Wine/ReactOS).
     * ID3DXBaseMesh slots 0-23, ID3DXMesh slots 24-28. Each slot is entered
     * directly (not via hle_call_guest, which restores esp) with the true
     * header argc so a wrong stdcall RET is actually detected. */
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
        uint32_t ca[6] = { g_dev, f2u(1), f2u(1), f2u(1), (uint32_t)hle_va(pp), 0 };
        call("D3DXCreateBox", 6, ca);
        uint32_t mesh = *pp;
        ok("mesh for slot sweep", mesh != 0);
        static const char *names[29] = {
            "ID3DXMesh::QueryInterface","ID3DXMesh::AddRef","ID3DXMesh::Release",
            "ID3DXMesh::DrawSubset","ID3DXMesh::GetNumFaces","ID3DXMesh::GetNumVertices",
            "ID3DXMesh::GetFVF","ID3DXMesh::GetDeclaration","ID3DXMesh::GetNumBytesPerVertex",
            "ID3DXMesh::GetOptions","ID3DXMesh::GetDevice","ID3DXMesh::CloneMeshFVF",
            "ID3DXMesh::CloneMesh","ID3DXMesh::GetVertexBuffer","ID3DXMesh::GetIndexBuffer",
            "ID3DXMesh::LockVertexBuffer","ID3DXMesh::UnlockVertexBuffer","ID3DXMesh::LockIndexBuffer",
            "ID3DXMesh::UnlockIndexBuffer","ID3DXMesh::GetAttributeTable","ID3DXMesh::ConvertPointRepsToAdjacency",
            "ID3DXMesh::ConvertAdjacencyToPointReps","ID3DXMesh::GenerateAdjacency","ID3DXMesh::UpdateSemantics",
            "ID3DXMesh::LockAttributeBuffer","ID3DXMesh::UnlockAttributeBuffer","ID3DXMesh::Optimize",
            "ID3DXMesh::OptimizeInplace","ID3DXMesh::SetAttributeTable"
        };
        /* stdcall argc INCLUDING `this`, from the public d3dx9mesh.h
         * declaration (Wine/ReactOS sdk/include/dxsdk/d3dx9mesh.h). */
        static const int nargs[29] = { 3,1,1, 2,1,1,1,2,1, 1,2,5,5,2,2,3,1,3,1,3,3,3,3,2, 3,1,7,6,3 };
        uint32_t vt = MEM32(mesh);
        int order_ok = 1;
        for (int i=0;i<29;i++) if (MEM32(vt + 4u*(uint32_t)i) != hle_resolve(names[i])) order_ok = 0;
        ok("ID3DXMesh vtable order/count matches d3dx9mesh.h (29 slots)", order_ok);
        /* the vtable must end after the 29 ID3DXMesh slots */
        ok("ID3DXMesh vtable ends after 29 slots", MEM32(vt + 4u*29u) == 0u);

        uint32_t *outp = (uint32_t *)hle_alloc(64); memset(outp,0,64);
        uint32_t *decl = (uint32_t *)hle_alloc(256); memset(decl,0,256);
        for (int i=0;i<29;i++) {
            uint32_t fn_va = MEM32(vt + 4u*(uint32_t)i);
            recomp_func_t fn = recomp_lookup(fn_va); if(!fn) fn = recomp_lookup_import(fn_va);
            char m[80]; snprintf(m,sizeof m,"mesh slot %d resolves",i); ok(m, fn!=NULL);
            uint32_t argv[8] = {0}; argv[0]=mesh;
            /* provide safe output pointers for methods that write through pointers */
            if (i==0) { argv[1]=0; argv[2]=(uint32_t)hle_va(outp); *outp=0; } /* QI */
            else if (i==2) { /* Release: balance with AddRef */ uint32_t ar[1]={mesh}; call("ID3DXMesh::AddRef",1,ar); }
            else if (i==7) { argv[1]=(uint32_t)hle_va(decl); } /* GetDeclaration */
            else if (i==10) { argv[1]=(uint32_t)hle_va(outp); *outp=0; } /* GetDevice */
            else if (i==13 || i==14) { argv[1]=(uint32_t)hle_va(outp); *outp=0; }
            else if (i==15 || i==17) { argv[1]=0; argv[2]=(uint32_t)hle_va(outp); *outp=0; } /* LockVB/IB */
            else if (i==19) { argv[1]=(uint32_t)hle_va(outp); argv[2]=(uint32_t)hle_va(outp+1); }
            else if (i==20 || i==21) { argv[1]=(uint32_t)hle_va(outp); argv[2]=0; argv[3]=(uint32_t)hle_va(outp+1); }
            else if (i==22) { argv[1]=0; argv[2]=(uint32_t)hle_va(outp); }
            else if (i==23) { argv[1]=(uint32_t)hle_va(decl); }
            else if (i==24) { argv[1]=0; argv[2]=(uint32_t)hle_va(outp); }
            else if (i==26 || i==27) { argv[1]=0; argv[2]=(uint32_t)hle_va(outp); argv[3]=0; argv[4]=(uint32_t)hle_va(outp+1); }
            else if (i==28) { argv[1]=(uint32_t)hle_va(outp); argv[2]=0; }
            /* hle_call_guest saves/restores all registers (regs_save/load),
             * including g_esp, so it can never observe a bad RET. Enter the
             * lifted shim directly with the guest stack set up by hand, as in
             * the accepted d3dx9_line_selftest.c slot sweep. */
            mach_enter();
            uint32_t esp0 = g_esp;
            for (int k = nargs[i]-1; k >= 0; k--) PUSH32(g_esp, argv[k]);
            PUSH32(g_esp, RECOMP_RETADDR);
            fn();
            uint32_t delta = g_esp - esp0;
            g_esp = esp0;
            mach_leave();
            snprintf(m,sizeof m,"mesh slot %d %s esp restored (argc %d, delta %u)",
                     i, names[i], nargs[i], delta);
            ok(m, delta == 0u);
        }
    }

    if (!fails) printf("d3dx9_shapes_selftest: all checks passed\n");
    return fails != 0;
}
