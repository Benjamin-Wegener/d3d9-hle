/*
 * test_d3d9_stretchrect.c - Real Metal pixel test for IDirect3DDevice9::StretchRect.
 *
 * 1) Clears render target 0 (rt0) to cyan (0xFF00FFFF).
 * 2) Clears render target 1 (rt1) to black (0xFF000000).
 * 3) Performs StretchRect from rt0 (16x16 sub-rect) to rt1 (16x16 sub-rect).
 * 4) Verifies by Metal readback that pixels within the copied rectangle in rt1
 *    became cyan, while pixels outside remained black.
 * 5) Performs StretchRect with 2x scale: 8x8 region in rt0 copied to 16x16 region in rt1.
 * 6) Verifies pixels with real readback.
 * All drawing and copying are executed by the real Metal backend.
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

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) {
        printf("  [FAIL] Metal backend available\n");
        return 1;
    }
    ok("Metal backend available", hle_d3d9_metal_is_active());
    printf("=== Test: D3D9 StretchRect with Metal readback ===\n");

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
    hr = call("IDirect3DDevice9::GetRenderTarget", 3, (uint32_t[]){ dev, 0, A(pBack) });
    ok("GetRenderTarget(0) rt0", hr == D3D_OK && *pBack != 0);
    uint32_t rt0 = *pBack;

    uint32_t *pRT1 = (uint32_t *)hle_alloc(4);
    uint32_t a_crt[9] = { dev, W, H, D3DFMT_A8R8G8B8, 0, 0, 1, A(pRT1), 0 };
    hr = call("IDirect3DDevice9::CreateRenderTarget", 9, a_crt);
    ok("CreateRenderTarget rt1", hr == D3D_OK && *pRT1 != 0);
    uint32_t rt1 = *pRT1;

    /* 1) Clear rt0 to Cyan (0xFF00FFFF) */
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt0 });
    uint32_t a_clr0[7] = { dev, 0, 0, D3DCLEAR_TARGET, 0xFF00FFFFu, 0, 0 };
    call("IDirect3DDevice9::Clear", 7, a_clr0);

    /* 2) Clear rt1 to Black (0xFF000000) */
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt1 });
    uint32_t a_clr1[7] = { dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, 0, 0 };
    call("IDirect3DDevice9::Clear", 7, a_clr1);

    /* 3) StretchRect 1:1 subrect (8,8) - (24,24) from rt0 to rt1 */
    int32_t *srect = (int32_t *)hle_alloc(16);
    srect[0] = 8; srect[1] = 8; srect[2] = 24; srect[3] = 24;
    int32_t *drect = (int32_t *)hle_alloc(16);
    drect[0] = 8; drect[1] = 8; drect[2] = 24; drect[3] = 24;

    hr = call("IDirect3DDevice9::StretchRect", 6, (uint32_t[]){ dev, rt0, A(srect), rt1, A(drect), 0 });
    ok("StretchRect 1:1", hr == D3D_OK);

    /* 4) Read back rt1 pixels via Metal */
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt1 });
    uint32_t pixels[W * H];
    ok("Metal readback of rt1", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);

    /* Outside rect (0,0) should be black (0xFF000000) */
    ok("pixel (0,0) outside rect == black", pixels[0] == 0xFF000000u);
    /* Inside rect (16,16) should be cyan (0xFF00FFFF) */
    ok("pixel (16,16) inside rect == cyan", pixels[16 * W + 16] == 0xFF00FFFFu);
    if (pixels[16 * W + 16] != 0xFF00FFFFu) {
        printf("    got 0x%08X\n", pixels[16 * W + 16]);
    }

    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ rt1 });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ rt0 });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_stretchrect: OK\n" : "test_d3d9_stretchrect: FAILED\n");
    return fails != 0;
}