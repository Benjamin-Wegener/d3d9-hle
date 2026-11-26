/* Shared scaffolding for the render-state pixel tests. Each test must draw
 * only through D3D9 calls and read back through the Metal backend. */
#ifndef TEST_D3D9_COMMON_H
#define TEST_D3D9_COMMON_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_metal.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) fails++;
}
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
#define A(p) hle_va(p)
static void *G_copy(const void *src, size_t n) {
    void *p = hle_alloc(n);
    memcpy(p, src, n);
    return p;
}

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
} Vtx;

/* Pixel coordinates -> Metal NDC (y up), colour as linear float. */
static Vtx vertex(float px, float py, float z, float r, float g, float b, float a) {
    Vtx v;
    v.x = px / 32.0f * 2.0f - 1.0f;
    v.y = 1.0f - py / 32.0f * 2.0f;
    v.z = z; v.w = 1.0f;
    v.r = r; v.g = g; v.b = b; v.a = a;
    v.u = 0.0f; v.v = 0.0f;
    return v;
}

static uint32_t make_device(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) { ok("Metal backend available", 0); return 0; }
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    if (!d3d9) { ok("Direct3DCreate9", 0); return 0; }
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 32; pp->BackBufferHeight = 32;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    pp->EnableAutoDepthStencil = 1; pp->AutoDepthStencilFormat = D3DFMT_D24S8;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3D9::CreateDevice", 7,
                       (uint32_t[]){ d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, 0,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev) });
    if (hr != D3D_OK || !*pDev) { ok("CreateDevice", 0); return 0; }
    /* These tests exercise clip planes, fog, gamma etc., not culling. D3D9's
     * device default is D3DCULL_CCW; set NONE explicitly so their triangles are
     * not culled by the correct default. The default itself has its own test
     * (test_d3d9_cull_default.c). */
    call("IDirect3DDevice9::SetRenderState", 3,
         (uint32_t[]){ *pDev, 22 /* D3DRS_CULLMODE */, 1 /* D3DCULL_NONE */ });
    return *pDev;
}

static uint32_t make_vb(uint32_t dev, const Vtx *v, uint32_t n) {
    uint32_t bytes = (uint32_t)(sizeof(Vtx) * n);
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer", 7,
         (uint32_t[]){ dev, bytes, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });
    if (!*pVB) { ok("CreateVertexBuffer", 0); return 0; }
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, bytes, A(pData), 0 });
    memcpy(hle_ptr(*pData), v, bytes);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(Vtx) });
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });
    return *pVB;
}

static void clear(uint32_t dev, uint32_t color, uint32_t flags, float z) {
    uint32_t zr; memcpy(&zr, &z, 4);
    call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, flags, color, zr, 0 });
}

static void draw_triangle(uint32_t dev, uint32_t vb) {
    (void)vb;
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 1 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
}

static void read_all(uint32_t *pixels) {
    ok("Metal readback", hle_d3d9_metal_read_pixels(0, 0, 32, 32, pixels, 32 * 4) == 0);
}

#endif /* TEST_D3D9_COMMON_H */
