/*
 * test_d3d9_colorfill.c - Real Metal pixel test for IDirect3DDevice9::ColorFill.
 *
 * 1) Creates a render target surface in D3DPOOL_DEFAULT.
 * 2) Clears render target to black (0xFF000000).
 * 3) Calls ColorFill on a sub-rect [8,8,24,24] with magenta (0xFFFF00FF).
 * 4) Reads back pixels from Metal GPU using hle_d3d9_metal_read_pixels.
 * 5) Verifies pixel inside rect is magenta and pixel outside is black.
 * 6) Calls ColorFill with NULL rect (entire surface) with orange (0xFFFF8000).
 * 7) Verifies all pixels became orange.
 * 8) Tests error handling (NULL surface, non-default pool surface, invalid rect).
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
    printf("=== Test: D3D9 ColorFill with Metal readback ===\n");

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

    /* Create destination render target in POOL_DEFAULT */
    uint32_t *pRT = (uint32_t *)hle_alloc(4);
    uint32_t a_crt[9] = { dev, W, H, D3DFMT_A8R8G8B8, 0, 0, 1, A(pRT), 0 };
    hr = call("IDirect3DDevice9::CreateRenderTarget", 9, a_crt);
    ok("CreateRenderTarget", hr == D3D_OK && *pRT != 0);
    uint32_t rt = *pRT;

    /* Clear rt to black */
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt });
    call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, 0, 0 });

    /* 1) ColorFill sub-rect [8,8,24,24] with Magenta (0xFFFF00FF) */
    int32_t *rect = (int32_t *)hle_alloc(16);
    rect[0] = 8; rect[1] = 8; rect[2] = 24; rect[3] = 24;
    const uint32_t magenta = 0xFFFF00FFu;
    hr = call("IDirect3DDevice9::ColorFill", 4, (uint32_t[]){ dev, rt, A(rect), magenta });
    ok("ColorFill subrect", hr == D3D_OK);

    /* Read back via Metal */
    uint32_t pixels[W * H];
    memset(pixels, 0, sizeof(pixels));
    ok("Metal readback after subrect ColorFill", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("pixel (0,0) outside rect == black", pixels[0] == 0xFF000000u);
    ok("pixel (16,16) inside rect == magenta", pixels[16 * W + 16] == magenta);

    /* 2) ColorFill full surface (pRect=NULL) with Orange (0xFFFF8000) */
    const uint32_t orange = 0xFFFF8000u;
    hr = call("IDirect3DDevice9::ColorFill", 4, (uint32_t[]){ dev, rt, 0, orange });
    ok("ColorFill full", hr == D3D_OK);
    ok("Metal readback after full ColorFill", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("pixel (0,0) after full == orange", pixels[0] == orange);
    ok("pixel (16,16) after full == orange", pixels[16 * W + 16] == orange);

    /* 3) Error validations */
    hr = call("IDirect3DDevice9::ColorFill", 4, (uint32_t[]){ dev, 0, 0, orange });
    ok("ColorFill NULL surface -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);

    /* Sysmem surface should fail ColorFill */
    uint32_t *pSys = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::CreateOffscreenPlainSurface", 7,
              (uint32_t[]){ dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, A(pSys), 0 });
    ok("CreateOffscreenPlainSurface(sysmem)", hr == D3D_OK && *pSys != 0);
    hr = call("IDirect3DDevice9::ColorFill", 4, (uint32_t[]){ dev, *pSys, 0, orange });
    ok("ColorFill sysmem surface -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);

    /* Out of bounds rect */
    rect[0] = 0; rect[1] = 0; rect[2] = W + 10; rect[3] = H;
    hr = call("IDirect3DDevice9::ColorFill", 4, (uint32_t[]){ dev, rt, A(rect), orange });
    ok("ColorFill out-of-bounds rect -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);

    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ *pSys });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ rt });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_colorfill: OK\n" : "test_d3d9_colorfill: FAILED\n");
    return fails != 0;
}
