#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "win32hle.h"
#include "d3dx9_shapes.h"
#include "d3dx9_mesh.h"

#define FVF_XYZ_NORMAL 0x00000012u
#define VSIZE 24u

/* Shape meshes: XYZ|NORMAL vertices, 16-bit indices. */
static uint32_t make_mesh(uint32_t dev, uint32_t nverts, uint32_t nfaces) {
    return d3dx9_mesh_new_fvf(dev, FVF_XYZ_NORMAL, nfaces, nverts);
}

static void set_vert(float *v, uint32_t i, float x, float y, float z, float nx, float ny, float nz) {
    float *p = v + (size_t)i * 6u;
    p[0]=x; p[1]=y; p[2]=z; p[3]=nx; p[4]=ny; p[5]=nz;
}

/* ---- shape generators ---- */

void s_D3DXCreateBox(void) {
    /* D3DXCreateBox(pDevice, Width, Height, Depth, ppMesh, ppAdjacency) */
    uint32_t dev = A32(0);
    float w, h, d; uint32_t wb=A32(1), hb=A32(2), db=A32(3);
    memcpy(&w,&wb,4); memcpy(&h,&hb,4); memcpy(&d,&db,4);
    uint32_t ppMesh = A32(4);
    uint32_t ppAdj = A32(5);
    if (!dev || !ppMesh || !(w >= 0.0f) || !(h >= 0.0f) || !(d >= 0.0f)) { RET(D3DERR_INVALIDCALL, 6); return; }
    uint32_t obj = make_mesh(dev, 24, 12);
    if (!obj) { RET(0x8007000Eu /*E_OUTOFMEMORY*/, 6); return; }
    float *v; uint16_t *idx;
    d3dx9_mesh_data(obj, (void **)&v, (void **)&idx);
    float hw=w*0.5f, hh=h*0.5f, hd=d*0.5f;
    /* 6 faces, 4 verts each, outward normals, CW when viewed from outside (D3D default). */
    set_vert(v,0, -hw,-hh, hd, 0,0,1); set_vert(v,1,  hw,-hh, hd, 0,0,1);
    set_vert(v,2,  hw, hh, hd, 0,0,1); set_vert(v,3, -hw, hh, hd, 0,0,1);
    set_vert(v,4,  hw,-hh,-hd, 0,0,-1); set_vert(v,5, -hw,-hh,-hd, 0,0,-1);
    set_vert(v,6, -hw, hh,-hd, 0,0,-1); set_vert(v,7,  hw, hh,-hd, 0,0,-1);
    set_vert(v,8, -hw, hh, hd, 0,1,0); set_vert(v,9,  hw, hh, hd, 0,1,0);
    set_vert(v,10, hw, hh,-hd, 0,1,0); set_vert(v,11,-hw, hh,-hd, 0,1,0);
    set_vert(v,12,-hw,-hh,-hd, 0,-1,0); set_vert(v,13, hw,-hh,-hd, 0,-1,0);
    set_vert(v,14, hw,-hh, hd, 0,-1,0); set_vert(v,15,-hw,-hh, hd, 0,-1,0);
    set_vert(v,16, hw,-hh, hd, 1,0,0); set_vert(v,17, hw,-hh,-hd, 1,0,0);
    set_vert(v,18, hw, hh,-hd, 1,0,0); set_vert(v,19, hw, hh, hd, 1,0,0);
    set_vert(v,20,-hw,-hh,-hd,-1,0,0); set_vert(v,21,-hw,-hh, hd,-1,0,0);
    set_vert(v,22,-hw, hh, hd,-1,0,0); set_vert(v,23,-hw, hh,-hd,-1,0,0);
    uint32_t ii=0;
    for (uint16_t f=0; f<6; f++) {
        uint16_t bv = (uint16_t)(f*4u);
        idx[ii++]=bv; idx[ii++]=bv+2; idx[ii++]=bv+1;
        idx[ii++]=bv; idx[ii++]=bv+3; idx[ii++]=bv+2;
    }
    d3dx9_mesh_write_adjacency(obj, ppAdj);
    MEM32(ppMesh) = obj;
    RET(D3D_OK, 6);
}

void s_D3DXCreateSphere(void) {
    /* D3DXCreateSphere(pDevice, Radius, Slices, Stacks, ppMesh, ppAdjacency) */
    uint32_t dev=A32(0);
    float radius; uint32_t rb=A32(1); memcpy(&radius,&rb,4);
    uint32_t slices=A32(2), stacks=A32(3);
    uint32_t ppMesh=A32(4); uint32_t ppAdj=A32(5);
    if (!dev || !ppMesh || !(radius >= 0.0f) || slices < 3 || stacks < 3) { RET(D3DERR_INVALIDCALL, 6); return; }
    uint32_t nverts = (stacks+1u)*(slices+1u);
    uint32_t nfaces = stacks*slices*2u;
    if (nverts > 0xFFFFu) { RET(D3DERR_INVALIDCALL, 6); return; }
    uint32_t obj = make_mesh(dev, nverts, nfaces);
    if (!obj) { RET(0x8007000Eu /*E_OUTOFMEMORY*/, 6); return; }
    float *v; uint16_t *idx;
    d3dx9_mesh_data(obj, (void **)&v, (void **)&idx);
    uint32_t vi=0;
    for (uint32_t st=0; st<=stacks; st++) {
        float phi = (float)st / (float)stacks * 3.14159265358979323846f;
        float sp = sinf(phi), cp = cosf(phi);
        for (uint32_t sl=0; sl<=slices; sl++) {
            float theta = (float)sl / (float)slices * 2.0f * 3.14159265358979323846f;
            float ct = cosf(theta), stt = sinf(theta);
            float x = sp*ct, y = cp, z = sp*stt;
            set_vert(v, vi++, x*radius, y*radius, z*radius, x, y, z);
        }
    }
    uint32_t ii=0;
    for (uint32_t st=0; st<stacks; st++) {
        for (uint32_t sl=0; sl<slices; sl++) {
            uint16_t av = (uint16_t)(st*(slices+1u)+sl);
            uint16_t bv = (uint16_t)(st*(slices+1u)+sl+1u);
            uint16_t cv = (uint16_t)((st+1u)*(slices+1u)+sl);
            uint16_t dv = (uint16_t)((st+1u)*(slices+1u)+sl+1u);
            idx[ii++]=av; idx[ii++]=cv; idx[ii++]=bv;
            idx[ii++]=bv; idx[ii++]=cv; idx[ii++]=dv;
        }
    }
    d3dx9_mesh_write_adjacency(obj, ppAdj);
    MEM32(ppMesh)=obj;
    RET(D3D_OK, 6);
}

void s_D3DXCreateCylinder(void) {
    /* D3DXCreateCylinder(pDevice, Radius1, Radius2, Length, Slices, Stacks, ppMesh, ppAdjacency) */
    uint32_t dev=A32(0);
    float r1,r2,len; uint32_t a=A32(1),b=A32(2),c=A32(3);
    memcpy(&r1,&a,4); memcpy(&r2,&b,4); memcpy(&len,&c,4);
    uint32_t slices=A32(4), stacks=A32(5);
    uint32_t ppMesh=A32(6); uint32_t ppAdj=A32(7);
    if (!dev || !ppMesh || !(r1 >= 0.0f) || !(r2 >= 0.0f) || !(len >= 0.0f) || slices < 3 || stacks < 1) {
        RET(D3DERR_INVALIDCALL, 8); return;
    }
    uint32_t body_verts = (stacks+1u)*(slices+1u);
    uint32_t cap_verts = 1u + slices; /* center + ring */
    uint32_t nverts = body_verts + 2u*cap_verts;
    uint32_t nfaces = stacks*slices*2u + 2u*slices;
    if (nverts > 0xFFFFu) { RET(D3DERR_INVALIDCALL, 8); return; }
    uint32_t obj = make_mesh(dev, nverts, nfaces);
    if (!obj) { RET(0x8007000Eu /*E_OUTOFMEMORY*/, 8); return; }
    float *v; uint16_t *idx;
    d3dx9_mesh_data(obj, (void **)&v, (void **)&idx);
    uint32_t vi=0, ii=0;
    float dr = r2-r1;
    float denom = sqrtf(dr*dr + len*len);
    float side_ny = 0.0f, side_nr = 1.0f;
    if (denom > 1e-20f) { side_ny = -dr / denom; side_nr = len / denom; }
    /* body */
    for (uint32_t st=0; st<=stacks; st++) {
        float t = (float)st/(float)stacks;
        float r = r1 + (r2-r1)*t;
        float y = (t-0.5f)*len;
        for (uint32_t sl=0; sl<=slices; sl++) {
            float theta = (float)sl/(float)slices*2.0f*3.14159265358979323846f;
            float ct=cosf(theta), stt=sinf(theta);
            set_vert(v, vi++, ct*r, y, stt*r, ct*side_nr, side_ny, stt*side_nr);
        }
    }
    for (uint32_t st=0; st<stacks; st++) {
        for (uint32_t sl=0; sl<slices; sl++) {
            uint16_t av = (uint16_t)(st*(slices+1u)+sl);
            uint16_t bv = (uint16_t)(st*(slices+1u)+sl+1u);
            uint16_t cv = (uint16_t)((st+1u)*(slices+1u)+sl);
            uint16_t dv = (uint16_t)((st+1u)*(slices+1u)+sl+1u);
            idx[ii++]=av; idx[ii++]=bv; idx[ii++]=cv;
            idx[ii++]=bv; idx[ii++]=dv; idx[ii++]=cv;
        }
    }
    /* bottom cap (y = -len/2, normal -Y) */
    uint32_t center_bottom = vi;
    set_vert(v, vi++, 0.0f, -len*0.5f, 0.0f, 0, -1, 0);
    uint32_t ring_bottom = vi;
    for (uint32_t sl=0; sl<slices; sl++) {
        float theta = (float)sl/(float)slices*2.0f*3.14159265358979323846f;
        set_vert(v, vi++, cosf(theta)*r1, -len*0.5f, sinf(theta)*r1, 0, -1, 0);
    }
    for (uint32_t sl=0; sl<slices; sl++) {
        idx[ii++]=(uint16_t)center_bottom;
        idx[ii++]=(uint16_t)(ring_bottom + ((sl+1u)%slices));
        idx[ii++]=(uint16_t)(ring_bottom + sl);
    }
    /* top cap (y = +len/2, normal +Y) */
    uint32_t center_top = vi;
    set_vert(v, vi++, 0.0f, len*0.5f, 0.0f, 0, 1, 0);
    uint32_t ring_top = vi;
    for (uint32_t sl=0; sl<slices; sl++) {
        float theta = (float)sl/(float)slices*2.0f*3.14159265358979323846f;
        set_vert(v, vi++, cosf(theta)*r2, len*0.5f, sinf(theta)*r2, 0, 1, 0);
    }
    for (uint32_t sl=0; sl<slices; sl++) {
        idx[ii++]=(uint16_t)center_top;
        idx[ii++]=(uint16_t)(ring_top + sl);
        idx[ii++]=(uint16_t)(ring_top + ((sl+1u)%slices));
    }
    d3dx9_mesh_write_adjacency(obj, ppAdj);
    MEM32(ppMesh)=obj;
    RET(D3D_OK, 8);
}

void s_D3DXCreateTorus(void) {
    /* D3DXCreateTorus(pDevice, InnerRadius, OuterRadius, Sides, Rings, ppMesh, ppAdjacency) */
    uint32_t dev=A32(0);
    float ir, orr; uint32_t a=A32(1), b=A32(2);
    memcpy(&ir,&a,4); memcpy(&orr,&b,4);
    uint32_t sides=A32(3), rings=A32(4);
    uint32_t ppMesh=A32(5); uint32_t ppAdj=A32(6);
    if (!dev || !ppMesh || !(ir >= 0.0f) || !(orr >= 0.0f) || sides < 3 || rings < 3) {
        RET(D3DERR_INVALIDCALL, 7); return;
    }
    uint32_t nverts = (rings+1u)*(sides+1u);
    uint32_t nfaces = rings*sides*2u;
    if (nverts > 0xFFFFu) { RET(D3DERR_INVALIDCALL, 7); return; }
    uint32_t obj = make_mesh(dev, nverts, nfaces);
    if (!obj) { RET(0x8007000Eu /*E_OUTOFMEMORY*/, 7); return; }
    float *v; uint16_t *idx;
    d3dx9_mesh_data(obj, (void **)&v, (void **)&idx);
    uint32_t vi=0, ii=0;
    for (uint32_t ri=0; ri<=rings; ri++) {
        float u = (float)ri/(float)rings * 2.0f*3.14159265358979323846f;
        float cu=cosf(u), su=sinf(u);
        for (uint32_t si=0; si<=sides; si++) {
            float v_ang = (float)si/(float)sides * 2.0f*3.14159265358979323846f;
            float cv=cosf(v_ang), sv=sinf(v_ang);
            float x = (orr + ir*cv)*cu;
            float y = ir*sv;
            float z = (orr + ir*cv)*su;
            float nx = cv*cu, ny = sv, nz = cv*su;
            set_vert(v, vi++, x, y, z, nx, ny, nz);
        }
    }
    for (uint32_t ri=0; ri<rings; ri++) {
        for (uint32_t si=0; si<sides; si++) {
            uint16_t av = (uint16_t)(ri*(sides+1u)+si);
            uint16_t bv = (uint16_t)(ri*(sides+1u)+si+1u);
            uint16_t cv2 = (uint16_t)((ri+1u)*(sides+1u)+si);
            uint16_t dv = (uint16_t)((ri+1u)*(sides+1u)+si+1u);
            idx[ii++]=av; idx[ii++]=cv2; idx[ii++]=bv;
            idx[ii++]=bv; idx[ii++]=cv2; idx[ii++]=dv;
        }
    }
    d3dx9_mesh_write_adjacency(obj, ppAdj);
    MEM32(ppMesh)=obj;
    RET(D3D_OK, 7);
}

/* ---- helpers ---- */

void s_D3DXComputeBoundingBox(void) {
    /* D3DXComputeBoundingBox(pFirstPosition, NumVertices, dwStride, pMin, pMax) */
    uint32_t pv = A32(0);
    uint32_t n = A32(1);
    uint32_t stride = A32(2);
    uint32_t pMin = A32(3);
    uint32_t pMax = A32(4);
    if (!pv || !pMin || !pMax || stride < 12u) { RET(D3DERR_INVALIDCALL, 5); return; }
    float *v = (float *)hle_ptr(pv);
    float minx=1e30f,miny=1e30f,minz=1e30f, maxx=-1e30f,maxy=-1e30f,maxz=-1e30f;
    for (uint32_t i=0; i<n; i++) {
        float *p = (float *)((uint8_t *)v + (size_t)i*stride);
        if (p[0]<minx) minx=p[0]; if (p[1]<miny) miny=p[1]; if (p[2]<minz) minz=p[2];
        if (p[0]>maxx) maxx=p[0]; if (p[1]>maxy) maxy=p[1]; if (p[2]>maxz) maxz=p[2];
    }
    float *mn = (float *)hle_ptr(pMin); mn[0]=minx; mn[1]=miny; mn[2]=minz;
    float *mx = (float *)hle_ptr(pMax); mx[0]=maxx; mx[1]=maxy; mx[2]=maxz;
    RET(D3D_OK, 5);
}

void s_D3DXComputeBoundingSphere(void) {
    /* D3DXComputeBoundingSphere(pFirstPosition, NumVertices, dwStride, pCenter, pRadius) */
    uint32_t pv = A32(0);
    uint32_t n = A32(1);
    uint32_t stride = A32(2);
    uint32_t pCenter = A32(3);
    uint32_t pRadius = A32(4);
    if (!pv || !pCenter || !pRadius || stride < 12u || n==0) { RET(D3DERR_INVALIDCALL, 5); return; }
    float *v = (float *)hle_ptr(pv);
    float cx=0,cy=0,cz=0;
    for (uint32_t i=0; i<n; i++) { float *p=(float *)((uint8_t *)v+(size_t)i*stride); cx+=p[0]; cy+=p[1]; cz+=p[2]; }
    cx/=(float)n; cy/=(float)n; cz/=(float)n;
    float r=0;
    for (uint32_t i=0; i<n; i++) {
        float *p=(float *)((uint8_t *)v+(size_t)i*stride);
        float dx=p[0]-cx,dy=p[1]-cy,dz=p[2]-cz;
        float d=sqrtf(dx*dx+dy*dy+dz*dz);
        if (d>r) r=d;
    }
    float *c = (float *)hle_ptr(pCenter); c[0]=cx; c[1]=cy; c[2]=cz;
    *(float *)hle_ptr(pRadius) = r;
    RET(D3D_OK, 5);
}

void s_D3DXGetFVFVertexSize(void) {
    /* D3DXGetFVFVertexSize(DWORD FVF) -> UINT; 0 for an FVF without a layout this module describes. */
    RET(d3dx9_fvf_vertex_size(A32(0)), 1);
}

void s_D3DXDeclaratorFromFVF(void) {
    /* D3DXDeclaratorFromFVF(DWORD FVF, D3DVERTEXELEMENT9 pDeclarator[MAX_FVF_DECL_SIZE]) */
    uint32_t pDecl = A32(1), hr = D3DERR_INVALIDCALL;
    if (pDecl && d3dx9_decl_from_fvf(A32(0), (D3DVERTEXELEMENT9 *)hle_ptr(pDecl))) hr = D3D_OK;
    RET(hr, 2);
}
