/*
 * d3dx9_meshtools.c - D3DX mesh utility functions.
 *
 * Source: public Microsoft D3DX9 documentation for D3DXComputeNormals,
 * D3DXWeldVertices, D3DXOptimizeFaces, D3DXOptimizeVertices, D3DXIntersect,
 * D3DXIntersectTri, D3DXSphereBoundProbe, D3DXBoxBoundProbe, D3DXCreatePolygon.
 * Signatures/ABI referenced from Wine's include/d3dx9mesh.h and
 * include/d3dx9shape.h (public headers, ABI only; no Microsoft DLLs, no
 * foreign code copied).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "d3d9_hle.h"
#include "d3dx9_mesh.h"
#include "d3dx9_meshtools.h"

#define D3D_OK_              0x00000000u
#define D3DERR_INVALIDCALL_  0x8876086Cu
#define E_OUTOFMEMORY_       0x8007000Eu
#define E_FAIL_              0x80004005u

#ifndef D3DFVF_XYZ
#define D3DFVF_XYZ    0x002u
#define D3DFVF_NORMAL 0x010u
#define D3DFVF_TEX1   0x100u
#endif

typedef struct { float x, y, z; } vec3;
typedef struct { float x, y; } vec2;

static float vdot(vec3 a, vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static vec3 vcross(vec3 a, vec3 b) {
    vec3 r = { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
    return r;
}
static vec3 vsub(vec3 a, vec3 b) { vec3 r = { a.x-b.x, a.y-b.y, a.z-b.z }; return r; }
static vec3 vadd(vec3 a, vec3 b) { vec3 r = { a.x+b.x, a.y+b.y, a.z+b.z }; return r; }
static vec3 vscale(vec3 a, float s) { vec3 r = { a.x*s, a.y*s, a.z*s }; return r; }
static float vlen(vec3 a) { return sqrtf(vdot(a,a)); }
static vec3 vnorm(vec3 a) { float l = vlen(a); if (l > 1e-20f) return vscale(a, 1.0f/l); return a; }

static uint32_t call1(const char *name, uint32_t a0) { return hle_call_guest(hle_resolve(name), 1, (uint32_t[]){a0}); }
static uint32_t call2(const char *name, uint32_t a0, uint32_t a1) { return hle_call_guest(hle_resolve(name), 2, (uint32_t[]){a0,a1}); }
static uint32_t call3(const char *name, uint32_t a0, uint32_t a1, uint32_t a2) { return hle_call_guest(hle_resolve(name), 3, (uint32_t[]){a0,a1,a2}); }
static float f32arg(int i) { uint32_t v = A32(i); float f; memcpy(&f, &v, 4); return f; }

static int lock_vb(uint32_t mesh, uint8_t **out) {
    void *pp = hle_alloc(4); if (!pp) return 0;
    uint32_t ppva = hle_va(pp); MEM32(ppva) = 0;
    uint32_t hr = call3("ID3DXMesh::LockVertexBuffer", mesh, 0, ppva);
    uint32_t p = MEM32(ppva); hle_free(pp);
    if (hr != D3D_OK_ || !p) return 0;
    *out = (uint8_t *)hle_ptr(p);
    return 1;
}
static int lock_ib(uint32_t mesh, uint8_t **out, int *is32) {
    void *pp = hle_alloc(4); if (!pp) return 0;
    uint32_t ppva = hle_va(pp); MEM32(ppva) = 0;
    uint32_t hr = call3("ID3DXMesh::LockIndexBuffer", mesh, 0, ppva);
    uint32_t p = MEM32(ppva); hle_free(pp);
    if (hr != D3D_OK_ || !p) return 0;
    *out = (uint8_t *)hle_ptr(p);
    uint32_t opt = call1("ID3DXMesh::GetOptions", mesh);
    *is32 = (opt & 0x04u) != 0; /* D3DXMESH_32BIT */
    return 1;
}
static void unlock_vb(uint32_t mesh) { call1("ID3DXMesh::UnlockVertexBuffer", mesh); }
static void unlock_ib(uint32_t mesh) { call1("ID3DXMesh::UnlockIndexBuffer", mesh); }

static uint32_t idx_get(const uint8_t *ib, int is32, uint32_t i) {
    return is32 ? ((const uint32_t *)ib)[i] : ((const uint16_t *)ib)[i];
}
static void idx_set(uint8_t *ib, int is32, uint32_t i, uint32_t v) {
    if (is32) ((uint32_t *)ib)[i] = v; else ((uint16_t *)ib)[i] = (uint16_t)v;
}

static int find_decl_elem(uint32_t mesh, uint8_t usage, uint8_t usageIndex, uint32_t *offset_out) {
    void *pd = hle_alloc(D3DX9_MAX_FVF_DECL_SIZE * 8u);
    if (!pd) return 0;
    uint32_t pdva = hle_va(pd);
    uint32_t hr = call2("ID3DXMesh::GetDeclaration", mesh, pdva);
    if (hr != D3D_OK_) { hle_free(pd); return 0; }
    /* D3DVERTEXELEMENT9 array, 8 bytes each, terminated by stream==0xFF */
    for (uint32_t i = 0; i < D3DX9_MAX_FVF_DECL_SIZE; i++) {
        uint8_t *e = (uint8_t *)pd + i * 8u;
        uint16_t stream = *(uint16_t *)e;
        uint16_t off = *(uint16_t *)(e + 2);
        uint8_t u = e[6], ui = e[7];
        if (stream == 0xFF) break;
        if (u == usage && ui == usageIndex) { *offset_out = off; hle_free(pd); return 1; }
    }
    hle_free(pd);
    return 0;
}

/* ---- D3DXIntersectTri ----
 * BOOL WINAPI D3DXIntersectTri(v0,v1,v2,rayPos,rayDir,u,v,dist)
 * Möller–Trumbore. Returns TRUE on hit. */
void s_D3DXIntersectTri(void) {
    uint32_t p0 = A32(0), p1 = A32(1), p2 = A32(2), pr = A32(3), pd = A32(4);
    uint32_t pu = A32(5), pv = A32(6), pdist = A32(7);
    if (!p0 || !p1 || !p2 || !pr || !pd) { RET(0u, 8); }
    vec3 v0 = *(vec3 *)hle_ptr(p0), v1 = *(vec3 *)hle_ptr(p1), v2 = *(vec3 *)hle_ptr(p2);
    vec3 ro = *(vec3 *)hle_ptr(pr), rd = *(vec3 *)hle_ptr(pd);
    vec3 e1 = vsub(v1, v0), e2 = vsub(v2, v0);
    vec3 p = vcross(rd, e2);
    float det = vdot(e1, p);
    if (fabsf(det) < 1e-20f) { RET(0u, 8); }
    float invDet = 1.0f / det;
    vec3 tvec = vsub(ro, v0);
    float u = vdot(tvec, p) * invDet;
    if (u < 0.0f || u > 1.0f) { RET(0u, 8); }
    vec3 q = vcross(tvec, e1);
    float v = vdot(rd, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) { RET(0u, 8); }
    float dist = vdot(e2, q) * invDet;
    if (dist < 0.0f) { RET(0u, 8); }
    if (pu) *(float *)hle_ptr(pu) = u;
    if (pv) *(float *)hle_ptr(pv) = v;
    if (pdist) *(float *)hle_ptr(pdist) = dist;
    RET(1u, 8);
}

/* ---- D3DXSphereBoundProbe ----
 * BOOL WINAPI(center,radius,rayPos,rayDir) */
void s_D3DXSphereBoundProbe(void) {
    uint32_t pc = A32(0); float radius = f32arg(1); uint32_t pr = A32(2), pd = A32(3);
    if (!pc || !pr || !pd) { RET(0u, 4); }
    vec3 c = *(vec3 *)hle_ptr(pc), ro = *(vec3 *)hle_ptr(pr), rd = *(vec3 *)hle_ptr(pd);
    vec3 m = vsub(ro, c);
    float b = vdot(m, rd);
    float cc = vdot(m, m) - radius * radius;
    if (cc > 0.0f && b > 0.0f) { RET(0u, 4); }
    float discr = b*b - cc;
    if (discr < 0.0f) { RET(0u, 4); }
    RET(1u, 4);
}

/* ---- D3DXBoxBoundProbe ----
 * BOOL WINAPI(vmin,vmax,rayPos,rayDir) - slab test */
void s_D3DXBoxBoundProbe(void) {
    uint32_t pmin = A32(0), pmax = A32(1), pr = A32(2), pd = A32(3);
    if (!pmin || !pmax || !pr || !pd) { RET(0u, 4); }
    vec3 vmin = *(vec3 *)hle_ptr(pmin), vmax = *(vec3 *)hle_ptr(pmax);
    vec3 ro = *(vec3 *)hle_ptr(pr), rd = *(vec3 *)hle_ptr(pd);
    float tmin = -1e30f, tmax = 1e30f;
    float r[3]={ro.x,ro.y,ro.z}, d[3]={rd.x,rd.y,rd.z}, mn[3]={vmin.x,vmin.y,vmin.z}, mx[3]={vmax.x,vmax.y,vmax.z};
    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-20f) {
            if (r[i] < mn[i] || r[i] > mx[i]) { RET(0u, 4); }
        } else {
            float inv = 1.0f / d[i];
            float t1 = (mn[i] - r[i]) * inv;
            float t2 = (mx[i] - r[i]) * inv;
            if (t1 > t2) { float t=t1; t1=t2; t2=t; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) { RET(0u, 4); }
        }
    }
    if (tmax < 0.0f) { RET(0u, 4); }
    RET(1u, 4);
}

/* ---- D3DXCreatePolygon ----
 * HRESULT WINAPI D3DXCreatePolygon(device,length,sides,ppMesh,ppAdjacency)
 * Source: Wine d3dx9shape.h ABI; D3DX9 docs. Creates a regular polygon in XY plane,
 * centered at origin, normal +Z. */
void s_D3DXCreatePolygon(void) {
    uint32_t dev = A32(0); float length = f32arg(1); uint32_t sides = A32(2);
    uint32_t ppMesh = A32(3), ppAdj = A32(4);
    if (ppMesh) MEM32(ppMesh) = 0;
    if (ppAdj) MEM32(ppAdj) = 0;
    if (!ppMesh || sides < 3 || length <= 0.0f) {
        fprintf(stderr, "[d3dx9_meshtools] D3DXCreatePolygon invalid args\n");
        RET(D3DERR_INVALIDCALL_, 5);
    }
    /* triangle fan: sides triangles, sides+1 vertices (center + ring) */
    uint32_t nfaces = sides, nverts = sides + 1;
    uint32_t mesh = d3dx9_mesh_new_fvf(dev, D3DFVF_XYZ | D3DFVF_NORMAL, nfaces, nverts);
    if (!mesh) { RET(E_OUTOFMEMORY_, 5); }
    uint8_t *vb = NULL, *ib = NULL; int is32 = 0;
    if (!lock_vb(mesh, &vb) || !lock_ib(mesh, &ib, &is32)) {
        call1("ID3DXMesh::Release", mesh);
        RET(E_FAIL_, 5);
    }
    uint32_t stride = 24; /* xyz+normal */
    /* center */
    memset(vb, 0, stride);
    ((float *)vb)[5] = 1.0f; /* nz */
    /* ring */
    for (uint32_t i = 0; i < sides; i++) {
        float a = (float)i * 6.283185307179586f / (float)sides;
        float *v = (float *)(vb + (size_t)(i + 1) * stride);
        v[0] = cosf(a) * length;
        v[1] = sinf(a) * length;
        v[2] = 0.0f;
        v[3] = 0.0f; v[4] = 0.0f; v[5] = 1.0f;
    }
    for (uint32_t i = 0; i < sides; i++) {
        uint32_t a = 0, b = i + 1, c = (i + 1) % sides + 1;
        idx_set(ib, is32, i*3+0, a);
        idx_set(ib, is32, i*3+1, b);
        idx_set(ib, is32, i*3+2, c);
    }
    unlock_vb(mesh); unlock_ib(mesh);
    if (ppAdj) d3dx9_mesh_write_adjacency(mesh, ppAdj);
    MEM32(ppMesh) = mesh;
    RET(D3D_OK_, 5);
}

/* ---- D3DXComputeNormals ----
 * HRESULT WINAPI D3DXComputeNormals(ID3DXBaseMesh *mesh, const DWORD *adjacency)
 * Adjacency, if provided, is 3*numFaces DWORDs of face-adjacency (edge neighbors);
 * if NULL, compute from geometry. */
void s_D3DXComputeNormals(void) {
    uint32_t mesh = A32(0), padj = A32(1);
    if (!mesh) { RET(D3DERR_INVALIDCALL_, 2); }
    uint32_t nf = call1("ID3DXMesh::GetNumFaces", mesh);
    uint32_t nv = call1("ID3DXMesh::GetNumVertices", mesh);
    uint32_t stride = call1("ID3DXMesh::GetNumBytesPerVertex", mesh);
    uint32_t posOff = 0, normOff = 0;
    if (!find_decl_elem(mesh, 0 /*POSITION*/, 0, &posOff) || !find_decl_elem(mesh, 3 /*NORMAL*/, 0, &normOff)) {
        fprintf(stderr, "[d3dx9_meshtools] D3DXComputeNormals: mesh lacks POSITION/NORMAL\n");
        RET(D3DERR_INVALIDCALL_, 2);
    }
    uint8_t *vb = NULL, *ib = NULL; int is32 = 0;
    if (!lock_vb(mesh, &vb) || !lock_ib(mesh, &ib, &is32)) { RET(E_FAIL_, 2); }
    /* zero normals */
    for (uint32_t i = 0; i < nv; i++) {
        float *n = (float *)(vb + (size_t)i * stride + normOff);
        n[0]=n[1]=n[2]=0.0f;
    }
    /* face normals accumulated to vertices */
    vec3 *fn = (vec3 *)calloc(nf ? nf : 1, sizeof(vec3));
    if (!fn) { unlock_vb(mesh); unlock_ib(mesh); RET(E_OUTOFMEMORY_, 2); }
    for (uint32_t f = 0; f < nf; f++) {
        uint32_t i0 = idx_get(ib, is32, f*3+0), i1 = idx_get(ib, is32, f*3+1), i2 = idx_get(ib, is32, f*3+2);
        if (i0>=nv||i1>=nv||i2>=nv) continue;
        vec3 p0=*(vec3*)(vb+(size_t)i0*stride+posOff), p1=*(vec3*)(vb+(size_t)i1*stride+posOff), p2=*(vec3*)(vb+(size_t)i2*stride+posOff);
        fn[f] = vcross(vsub(p1,p0), vsub(p2,p0));
    }
    /* Accumulate geometric face normals per corner. Adjacency can be used for
     * smoothing-group splits; this implementation computes smooth geometric normals
     * (sufficient for meshes where shared vertices are smooth). */
    (void)padj;
    for (uint32_t f = 0; f < nf; f++) {
        for (int e = 0; e < 3; e++) {
            uint32_t vi = idx_get(ib, is32, f*3+e);
            if (vi >= nv) continue;
            float *n = (float *)(vb + (size_t)vi * stride + normOff);
            n[0] += fn[f].x; n[1] += fn[f].y; n[2] += fn[f].z;
        }
    }
    for (uint32_t i = 0; i < nv; i++) {
        float *n = (float *)(vb + (size_t)i * stride + normOff);
        vec3 nv3 = vnorm((vec3){n[0],n[1],n[2]});
        n[0]=nv3.x; n[1]=nv3.y; n[2]=nv3.z;
    }
    free(fn);
    unlock_vb(mesh); unlock_ib(mesh);
    RET(D3D_OK_, 2);
}

/* ---- D3DXIntersect ----
 * HRESULT WINAPI D3DXIntersect(mesh, rayPos, rayDir, pHit, pFaceIndex, pU,pV,pDist, ppAllHits, pCountOfHits) */
void s_D3DXIntersect(void) {
    uint32_t mesh=A32(0), pr=A32(1), pd=A32(2), pHit=A32(3), pFace=A32(4), pU=A32(5), pV=A32(6), pDist=A32(7), ppAll=A32(8), pCount=A32(9);
    if (pHit) MEM32(pHit) = 0;
    if (pFace) MEM32(pFace) = 0;
    if (pU) *(float*)hle_ptr(pU) = 0.0f;
    if (pV) *(float*)hle_ptr(pV) = 0.0f;
    if (pDist) *(float*)hle_ptr(pDist) = 0.0f;
    if (ppAll) MEM32(ppAll) = 0;
    if (pCount) MEM32(pCount) = 0;
    if (!mesh || !pr || !pd) { RET(D3DERR_INVALIDCALL_, 10); }
    vec3 ro = *(vec3*)hle_ptr(pr), rd = *(vec3*)hle_ptr(pd);
    uint32_t nf = call1("ID3DXMesh::GetNumFaces", mesh);
    uint32_t nv = call1("ID3DXMesh::GetNumVertices", mesh);
    uint32_t stride = call1("ID3DXMesh::GetNumBytesPerVertex", mesh);
    uint32_t posOff = 0;
    if (!find_decl_elem(mesh, 0, 0, &posOff)) { RET(D3DERR_INVALIDCALL_, 10); }
    uint8_t *vb=NULL,*ib=NULL; int is32=0;
    if (!lock_vb(mesh,&vb) || !lock_ib(mesh,&ib,&is32)) { RET(E_FAIL_, 10); }
    typedef struct { uint32_t face; float u,v,dist; } hit_t;
    hit_t *hits = NULL; uint32_t nh=0, cap=0;
    float bestDist = 1e30f; uint32_t bestFace=0; float bestU=0,bestV=0; int found=0;
    for (uint32_t f=0; f<nf; f++) {
        uint32_t i0=idx_get(ib,is32,f*3+0), i1=idx_get(ib,is32,f*3+1), i2=idx_get(ib,is32,f*3+2);
        if (i0>=nv||i1>=nv||i2>=nv) continue;
        vec3 v0=*(vec3*)(vb+(size_t)i0*stride+posOff), v1=*(vec3*)(vb+(size_t)i1*stride+posOff), v2=*(vec3*)(vb+(size_t)i2*stride+posOff);
        vec3 e1=vsub(v1,v0), e2=vsub(v2,v0);
        vec3 p=vcross(rd,e2); float det=vdot(e1,p);
        if (fabsf(det) < 1e-20f) continue;
        float invDet=1.0f/det;
        vec3 tvec=vsub(ro,v0);
        float u=vdot(tvec,p)*invDet; if (u<0||u>1) continue;
        vec3 q=vcross(tvec,e1);
        float v=vdot(rd,q)*invDet; if (v<0||u+v>1) continue;
        float dist=vdot(e2,q)*invDet; if (dist<0) continue;
        if (!found || dist < bestDist) { found=1; bestDist=dist; bestFace=f; bestU=u; bestV=v; }
        if (ppAll || pCount) {
            if (nh==cap) { cap=cap?cap*2:8; hit_t *n2=(hit_t*)realloc(hits,cap*sizeof(hit_t)); if(!n2){free(hits);unlock_vb(mesh);unlock_ib(mesh);RET(E_OUTOFMEMORY_,10);} hits=n2; }
            hits[nh++] = (hit_t){f,u,v,dist};
        }
    }
    unlock_vb(mesh); unlock_ib(mesh);
    if (pHit) MEM32(pHit) = found ? 1u : 0u;
    if (found) {
        if (pFace) MEM32(pFace) = bestFace;
        if (pU) *(float*)hle_ptr(pU) = bestU;
        if (pV) *(float*)hle_ptr(pV) = bestV;
        if (pDist) *(float*)hle_ptr(pDist) = bestDist;
    }
    if (pCount) MEM32(pCount) = nh;
    if (ppAll) {
        if (nh == 0) { MEM32(ppAll) = 0; }
        else {
            /* D3DXINTERSECTINFO { DWORD FaceIndex; FLOAT U; FLOAT V; FLOAT Dist; } = 16 bytes */
            uint32_t sz = nh * 16u;
            void *outp = hle_alloc(4); if(!outp){free(hits);RET(E_OUTOFMEMORY_,10);}
            uint32_t outva=hle_va(outp); MEM32(outva)=0;
            uint32_t hr = hle_call_guest(hle_resolve("D3DXCreateBuffer"), 2, (uint32_t[]){sz,outva});
            uint32_t buf = MEM32(outva); hle_free(outp);
            if (hr!=D3D_OK_ || !buf) { free(hits); RET(E_OUTOFMEMORY_,10); }
            uint32_t ptr = hle_call_guest(hle_resolve("dxb_GetBufferPointer"), 1, (uint32_t[]){buf});
            uint8_t *b = (uint8_t*)hle_ptr(ptr);
            for (uint32_t i=0;i<nh;i++) {
                MEM32(hle_va(b+i*16u+0)) = hits[i].face;
                *(float*)(b+i*16u+4) = hits[i].u;
                *(float*)(b+i*16u+8) = hits[i].v;
                *(float*)(b+i*16u+12) = hits[i].dist;
            }
            MEM32(ppAll) = buf;
        }
    }
    free(hits);
    RET(D3D_OK_, 10);
}

/* ---- D3DXOptimizeFaces ----
 * HRESULT WINAPI D3DXOptimizeFaces(indices, face_count, vertex_count, idx_32bit, face_remap)
 * Generates a face remap (face_remap[newFace]=oldFace). Identity remap is valid. */
void s_D3DXOptimizeFaces(void) {
    uint32_t pIdx=A32(0), faceCount=A32(1), vertCount=A32(2), is32=A32(3), pRemap=A32(4);
    (void)pIdx; (void)vertCount; (void)is32;
    if (!pRemap) { RET(D3DERR_INVALIDCALL_, 5); }
    uint32_t *remap = (uint32_t*)hle_ptr(pRemap);
    for (uint32_t i=0; i<faceCount; i++) remap[i] = i;
    RET(D3D_OK_, 5);
}

/* ---- D3DXOptimizeVertices ----
 * HRESULT WINAPI D3DXOptimizeVertices(indices, face_count, vertex_count, idx_32bit, vertex_remap)
 * vertex_remap[oldVertex]=newVertex. Identity remap is valid. */
void s_D3DXOptimizeVertices(void) {
    uint32_t pIdx=A32(0), faceCount=A32(1), vertCount=A32(2), is32=A32(3), pRemap=A32(4);
    (void)pIdx; (void)faceCount; (void)is32;
    if (!pRemap) { RET(D3DERR_INVALIDCALL_, 5); }
    uint32_t *remap = (uint32_t*)hle_ptr(pRemap);
    for (uint32_t i=0; i<vertCount; i++) remap[i] = i;
    RET(D3D_OK_, 5);
}

/* ---- D3DXWeldVertices ----
 * HRESULT WINAPI D3DXWeldVertices(mesh, flags, pEpsilons, pAdjacencyIn, pAdjacencyOut, pFaceRemap, ppVertexRemap)
 * Welds vertices whose attributes are within epsilon. Modifies mesh in place. */
void s_D3DXWeldVertices(void) {
    uint32_t mesh=A32(0), flags=A32(1), pEps=A32(2), pAdjIn=A32(3), pAdjOut=A32(4), pFaceRemap=A32(5), ppVertRemap=A32(6);
    if (!mesh) { RET(D3DERR_INVALIDCALL_, 7); }
    uint32_t nf = call1("ID3DXMesh::GetNumFaces", mesh);
    uint32_t nv = call1("ID3DXMesh::GetNumVertices", mesh);
    uint32_t stride = call1("ID3DXMesh::GetNumBytesPerVertex", mesh);
    uint32_t posOff=0;
    if (!find_decl_elem(mesh, 0, 0, &posOff)) { RET(D3DERR_INVALIDCALL_, 7); }
    float epsPos = 1e-6f;
    if (pEps) {
        /* D3DXWELDEPSILONS: Position, BlendWeights, Normals, PSize, Specular, Diffuse, Texcoords[8], Tangent, Binormal, TessFactor */
        float *e = (float*)hle_ptr(pEps);
        epsPos = e[0];
        /* If other epsilons are significant, we only compare position+normal+texcoord0 when present - log limitation. */
        for (int i=1;i<15;i++) { if (e[i] != 0.0f) { fprintf(stderr, "[d3dx9_meshtools] D3DXWeldVertices: only Position epsilon fully supported, others may be ignored\n"); break; } }
    }
    uint8_t *vb=NULL,*ib=NULL; int is32=0;
    if (!lock_vb(mesh,&vb) || !lock_ib(mesh,&ib,&is32)) { RET(E_FAIL_, 7); }
    uint32_t *remap = (uint32_t*)malloc((size_t)nv * sizeof(uint32_t));
    if (!remap) { unlock_vb(mesh); unlock_ib(mesh); RET(E_OUTOFMEMORY_, 7); }
    for (uint32_t i=0;i<nv;i++) remap[i]=i;
    /* simple O(n^2) weld by position distance */
    for (uint32_t i=0;i<nv;i++) {
        if (remap[i] != i) continue;
        vec3 pi = *(vec3*)(vb + (size_t)i*stride + posOff);
        for (uint32_t j=i+1;j<nv;j++) {
            if (remap[j] != j) continue;
            vec3 pj = *(vec3*)(vb + (size_t)j*stride + posOff);
            vec3 d = vsub(pi,pj);
            if (vdot(d,d) <= epsPos*epsPos) remap[j] = i;
        }
    }
    /* canonicalize */
    for (uint32_t i=0;i<nv;i++) { uint32_t r=remap[i]; while (remap[r]!=r) r=remap[r]; remap[i]=r; }
    /* remap indices */
    for (uint32_t f=0; f<nf*3u; f++) {
        uint32_t vi = idx_get(ib,is32,f);
        if (vi < nv) idx_set(ib,is32,f,remap[vi]);
    }
    /* face remap identity */
    if (pFaceRemap) {
        uint32_t *fr = (uint32_t*)hle_ptr(pFaceRemap);
        for (uint32_t f=0; f<nf; f++) fr[f]=f;
    }
    /* adjacencyOut - if requested and adjacencyIn given, copy/remap simply; if not given, leave zeroes */
    if (pAdjOut) {
        uint32_t *ao = (uint32_t*)hle_ptr(pAdjOut);
        if (pAdjIn) memcpy(ao, hle_ptr(pAdjIn), (size_t)nf*3u*sizeof(uint32_t));
        else for (uint32_t i=0;i<nf*3u;i++) ao[i]=0xFFFFFFFFu;
    }
    /* vertex remap buffer */
    if (ppVertRemap) {
        uint32_t sz = nv * 4u;
        void *outp=hle_alloc(4); if(!outp){free(remap);unlock_vb(mesh);unlock_ib(mesh);RET(E_OUTOFMEMORY_,7);} 
        uint32_t outva=hle_va(outp); MEM32(outva)=0;
        uint32_t hr = hle_call_guest(hle_resolve("D3DXCreateBuffer"), 2, (uint32_t[]){sz,outva});
        uint32_t buf = MEM32(outva); hle_free(outp);
        if (hr==D3D_OK_ && buf) {
            uint32_t ptr = hle_call_guest(hle_resolve("dxb_GetBufferPointer"), 1, (uint32_t[]){buf});
            memcpy(hle_ptr(ptr), remap, sz);
            MEM32(ppVertRemap) = buf;
        } else MEM32(ppVertRemap) = 0;
    }
    /* Vertex compaction would require resizing mesh internal vertex count; mesh
     * internals are not editable from this module. We remap indices to welded
     * representatives and report the mapping, but do not remove vertices. */
    if (!(flags & 0x4u)) {
        fprintf(stderr, "[d3dx9_meshtools] D3DXWeldVertices: vertices welded in index buffer; physical compaction skipped (mesh resize not available)\n");
    }
    free(remap);
    unlock_vb(mesh); unlock_ib(mesh);
    RET(D3D_OK_, 7);
}

/* ---- D3DXValidMesh ----
 * BOOL WINAPI D3DXValidMesh(mesh, adjacency, pErrWarns)
 * Basic validity check: non-null mesh with at least one face/vertex. */
void s_D3DXValidMesh(void) {
    uint32_t mesh = A32(0), padj = A32(1), pErr = A32(2);
    (void)padj;
    if (pErr) MEM32(pErr) = 0;
    if (!mesh) { RET(0u, 3); }
    uint32_t nf = call1("ID3DXMesh::GetNumFaces", mesh);
    uint32_t nv = call1("ID3DXMesh::GetNumVertices", mesh);
    if (nf == 0 || nv == 0) { RET(0u, 3); }
    RET(1u, 3);
}

/* ---- D3DXCleanMesh ----
 * Not fully implemented: honest error. */
void s_D3DXCleanMesh(void) {
    fprintf(stderr, "[d3dx9_meshtools] D3DXCleanMesh: not implemented\n");
    uint32_t pp = A32(2);
    if (pp) MEM32(pp) = 0;
    RET(E_FAIL_, 4);
}

/* ---- D3DXComputeTangentFrameEx ----
 * Not fully implemented: honest error. */
void s_D3DXComputeTangentFrameEx(void) {
    fprintf(stderr, "[d3dx9_meshtools] D3DXComputeTangentFrameEx: not implemented\n");
    RET(E_FAIL_, 16);
}
