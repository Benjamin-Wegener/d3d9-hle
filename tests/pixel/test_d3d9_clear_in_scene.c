/*
 * test_d3d9_clear_in_scene.c - D3D9 allows BeginScene -> Clear -> Draw ->
 * EndScene, the usual order of a game: the draws after the Clear must still
 * arrive. Same hand-set pixels as test_d3d9_triangle, only the order differs.
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_metal.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails = 0;
static void ok(const char *name, int cond) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) fails++;
}
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
#define A(p) hle_va(p)

#define W 64
#define H 64

/* Matches the Metal backend's vertex layout: float4 pos, float4 colour,
 * float2 uv. Positions are in Metal NDC (x right, y up). */
typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
} Vertex;

static Vertex ndc_vertex(float px, float py, float r, float g, float b) {
    Vertex v;
    v.x = (px / (float)W) * 2.0f - 1.0f;
    v.y = 1.0f - (py / (float)H) * 2.0f;
    v.z = 0.5f;
    v.w = 1.0f;
    v.r = r; v.g = g; v.b = b; v.a = 1.0f;
    v.u = 0.0f; v.v = 0.0f;
    return v;
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) {
        printf("  [FAIL] Metal backend available\n");
        return 1;
    }
    ok("Metal backend available", hle_d3d9_metal_is_active());
    printf("=== Test: D3D9 triangle with Metal pixel readback ===\n");

    uint32_t a_create[1] = { 32 };
    uint32_t d3d9 = call("Direct3DCreate9", 1, a_create);
    ok("Direct3DCreate9", d3d9 != 0);

    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = W; pp->BackBufferHeight = H;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t a_cdev[7] = { d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, 0,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev) };
    uint32_t hr = call("IDirect3D9::CreateDevice", 7, a_cdev);
    ok("CreateDevice", hr == D3D_OK && *pDev != 0);
    uint32_t dev = *pDev;

    const uint32_t vb_size = sizeof(Vertex) * 3;
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    uint32_t a_cvb[7] = { dev, vb_size, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 };
    hr = call("IDirect3DDevice9::CreateVertexBuffer", 7, a_cvb);
    ok("CreateVertexBuffer", hr == D3D_OK && *pVB != 0);
    uint32_t vb = *pVB;

    uint32_t *pData = (uint32_t *)hle_alloc(4);
    uint32_t a_lockvb[5] = { vb, 0, vb_size, A(pData), 0 };
    hr = call("IDirect3DVertexBuffer9::Lock", 5, a_lockvb);
    ok("VertexBuffer::Lock", hr == D3D_OK);
    Vertex *verts = (Vertex *)hle_ptr(*pData);
    verts[0] = ndc_vertex(32.0f, 10.0f, 0.0f, 1.0f, 0.0f);  /* apex */
    verts[1] = ndc_vertex(50.0f, 50.0f, 0.0f, 1.0f, 0.0f);
    verts[2] = ndc_vertex(14.0f, 50.0f, 0.0f, 1.0f, 0.0f);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ vb });

    uint32_t a_stream[5] = { dev, 0, vb, 0, sizeof(Vertex) };
    hr = call("IDirect3DDevice9::SetStreamSource", 5, a_stream);
    ok("SetStreamSource", hr == D3D_OK);
    uint32_t a_fvf[2] = { dev, 0 };
    call("IDirect3DDevice9::SetFVF", 2, a_fvf);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    float z = 1.0f; uint32_t z_raw; memcpy(&z_raw, &z, 4);
    uint32_t a_clr[7] = { dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, z_raw, 0 };
    hr = call("IDirect3DDevice9::Clear", 7, a_clr);
    ok("Clear(black)", hr == D3D_OK);

    uint32_t a_draw[4] = { dev, D3DPT_TRIANGLELIST, 0, 1 };
    hr = call("IDirect3DDevice9::DrawPrimitive", 4, a_draw);
    ok("DrawPrimitive(D3DPT_TRIANGLELIST)", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[W * H];
    ok("Metal readback", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    uint32_t inside = pixels[36 * W + 32];
    ok("pixel (32,36) inside triangle == green", inside == 0xFF00FF00u);
    if (inside != 0xFF00FF00u) printf("    got 0x%08X\n", inside);
    int outside_ok = 1;
    const int px[4] = { 5, 60, 5, 60 }, py[4] = { 5, 5, 60, 60 };
    for (int i = 0; i < 4; i++) {
        if (pixels[py[i] * W + px[i]] != 0xFF000000u) outside_ok = 0;
    }
    ok("pixels outside triangle remain black", outside_ok);

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ vb });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_clear_in_scene: OK\n" : "test_d3d9_clear_in_scene: FAILED\n");
    return fails != 0;
}
