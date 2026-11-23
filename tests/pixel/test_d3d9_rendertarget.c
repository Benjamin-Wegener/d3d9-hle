/*
 * test_d3d9_rendertarget.c - Real Metal pixel test for SetRenderTarget.
 *
 * Two render targets are cleared to different colours through D3D9 calls.
 * After each switch the pixels are read from the Metal texture that is
 * currently bound. If SetRenderTarget did not switch the Metal target, the
 * second clear would overwrite the first target and the test would fail.
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

#define W 32
#define H 32

/* The backend's vertex layout: float4 position, float4 colour, float2 uv. */
typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
} Vtx;

static Vtx vtx_ndc(float px, float py, float r, float g, float b) {
    Vtx v;
    v.x = (px / (float)W) * 2.0f - 1.0f;
    v.y = 1.0f - (py / (float)H) * 2.0f;
    v.z = 0.5f; v.w = 1.0f;
    v.r = r; v.g = g; v.b = b; v.a = 1.0f;
    v.u = 0.0f; v.v = 0.0f;
    return v;
}

static int all_pixels(uint32_t *pixels, uint32_t color) {
    for (int i = 0; i < W * H; i++) if (pixels[i] != color) return 0;
    return 1;
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) {
        printf("  [FAIL] Metal backend available\n");
        return 1;
    }
    ok("Metal backend available", hle_d3d9_metal_is_active());
    printf("=== Test: D3D9 render targets with Metal pixel readback ===\n");

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

    uint32_t *pBack = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::GetBackBuffer", 5, (uint32_t[]){ dev, 0, 0, 0, A(pBack) });
    ok("GetBackBuffer", hr == D3D_OK && *pBack != 0);
    uint32_t rt0 = *pBack;

    uint32_t *pRT1 = (uint32_t *)hle_alloc(4);
    uint32_t a_crt[9] = { dev, W, H, D3DFMT_A8R8G8B8, 0, 0, 1, A(pRT1), 0 };
    hr = call("IDirect3DDevice9::CreateRenderTarget", 9, a_crt);
    ok("CreateRenderTarget", hr == D3D_OK && *pRT1 != 0);
    uint32_t rt1 = *pRT1;

    float z = 1.0f; uint32_t z_raw; memcpy(&z_raw, &z, 4);
    uint32_t pixels[W * H];
    const uint32_t cyan = 0xFF00FFFFu, yellow = 0xFFFFFF00u;

    /* Backbuffer: clear to cyan and verify from Metal. */
    uint32_t a_clr[7] = { dev, 0, 0, D3DCLEAR_TARGET, cyan, z_raw, 0 };
    hr = call("IDirect3DDevice9::Clear", 7, a_clr);
    ok("Clear(backbuffer, cyan)", hr == D3D_OK);
    ok("Metal readback of backbuffer", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("backbuffer pixels == cyan", all_pixels(pixels, cyan));

    /* Switch to rt1: GetRenderTarget and Metal must both follow. */
    hr = call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt1 });
    ok("SetRenderTarget(rt1)", hr == D3D_OK);
    uint32_t *pCur = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::GetRenderTarget", 3, (uint32_t[]){ dev, 0, A(pCur) });
    ok("GetRenderTarget == rt1", *pCur == rt1);

    a_clr[4] = yellow;
    hr = call("IDirect3DDevice9::Clear", 7, a_clr);
    ok("Clear(rt1, yellow)", hr == D3D_OK);
    ok("Metal readback of rt1", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("rt1 pixels == yellow", all_pixels(pixels, yellow));
    if (!all_pixels(pixels, yellow)) printf("    rt1 first pixel: 0x%08X\n", pixels[0]);

    /* Switch back: the first target must still hold cyan. */
    hr = call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt0 });
    ok("SetRenderTarget(backbuffer)", hr == D3D_OK);
    ok("Metal readback after switch back", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("backbuffer still cyan", all_pixels(pixels, cyan));
    if (!all_pixels(pixels, cyan)) printf("    backbuffer first pixel: 0x%08X\n", pixels[0]);

    /* SetRenderTarget with an encoder already open from BeginScene: a Metal
     * pass cannot retarget, so the backend must close and reopen it. The red
     * triangle has to land in rt1, never in the cyan backbuffer. */
    const uint32_t vb_bytes = sizeof(Vtx) * 3;
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::CreateVertexBuffer", 7,
              (uint32_t[]){ dev, vb_bytes, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });
    ok("CreateVertexBuffer", hr == D3D_OK && *pVB != 0);
    uint32_t vb = *pVB;
    uint32_t *pVBuf = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ vb, 0, vb_bytes, A(pVBuf), 0 });
    ok("VertexBuffer::Lock", hr == D3D_OK);
    Vtx *vs = (Vtx *)hle_ptr(*pVBuf);
    vs[0] = vtx_ndc(16.0f, 5.0f, 1.0f, 0.0f, 0.0f);   /* apex */
    vs[1] = vtx_ndc(26.0f, 27.0f, 1.0f, 0.0f, 0.0f);
    vs[2] = vtx_ndc(6.0f, 27.0f, 1.0f, 0.0f, 0.0f);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ vb });
    call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, vb, 0, sizeof(Vtx) });
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });

    /* rt0 is bound and cyan; make rt1 black so the red triangle is visible. */
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt1 });
    a_clr[4] = 0xFF000000u;
    call("IDirect3DDevice9::Clear", 7, a_clr);
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt0 });
    a_clr[4] = cyan;
    call("IDirect3DDevice9::Clear", 7, a_clr);

    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });  /* encoder on rt0 */
    hr = call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt1 });
    ok("SetRenderTarget during scene", hr == D3D_OK);
    hr = call("IDirect3DDevice9::DrawPrimitive", 4,
              (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 1 });
    ok("DrawPrimitive after RT switch", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    ok("Metal readback of rt1 after draw", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("rt1 triangle pixel == red", pixels[16 * W + 16] == 0xFFFF0000u);
    if (pixels[16 * W + 16] != 0xFFFF0000u) printf("    rt1 got 0x%08X\n", pixels[16 * W + 16]);
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt0 });
    ok("Metal readback of backbuffer after draw", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("backbuffer unchanged (no triangle)", all_pixels(pixels, cyan));
    if (!all_pixels(pixels, cyan)) printf("    backbuffer got 0x%08X\n", pixels[16 * W + 16]);

    /* Reset to a new backbuffer size: init_device resizes its own texture but
     * must not free the borrowed surface texture. Releasing rt0 below would
     * double-free it otherwise, and the readback would hit freed memory. */
    D3DPRESENT_PARAMETERS *pp2 = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp2));
    memset(pp2, 0, sizeof(*pp2));
    pp2->BackBufferWidth = 64; pp2->BackBufferHeight = 48;
    pp2->BackBufferFormat = D3DFMT_A8R8G8B8; pp2->BackBufferCount = 1;
    hr = call("IDirect3DDevice9::Reset", 2, (uint32_t[]){ dev, A(pp2) });
    ok("Reset to new backbuffer size", hr == D3D_OK);
    ok("Metal readback after Reset", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("backbuffer surface still valid after Reset", all_pixels(pixels, cyan));

    /* Test SetRenderTarget and GetRenderTarget validations */
    hr = call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 1, rt0 });
    ok("SetRenderTarget(index >= 1) -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);
    hr = call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, 0 });
    ok("SetRenderTarget(index 0, NULL) -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);

    hr = call("IDirect3DDevice9::GetRenderTarget", 3, (uint32_t[]){ dev, 0, 0 });
    ok("GetRenderTarget(ppSurface == NULL) -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);
    hr = call("IDirect3DDevice9::GetRenderTarget", 3, (uint32_t[]){ dev, 1, A(pBack) });
    ok("GetRenderTarget(index >= 1) -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ vb });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ rt1 });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ rt0 });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_rendertarget: OK\n" : "test_d3d9_rendertarget: FAILED\n");
    return fails != 0;
}
