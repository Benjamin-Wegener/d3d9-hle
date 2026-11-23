/*
 * test_d3d9_clear.c - Real Metal pixel test for IDirect3DDevice9::Clear.
 *
 * Drawing happens only through D3D9 calls. The framebuffer is read back with
 * hle_d3d9_metal_read_pixels(), which reads the Metal render target texture.
 * All drawing is done by the real Metal backend: no simulated device, no
 * software rasteriser, no direct writes to the target.
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

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);

    if (hle_d3d9_metal_enable() != 0) {
        printf("  [FAIL] Metal backend available\n");
        return 1;
    }
    ok("Metal backend available", hle_d3d9_metal_is_active());

    printf("=== Test: D3D9 Clear with Metal pixel readback ===\n");

    uint32_t a_create[1] = { 32 };           /* D3D_SDK_VERSION */
    uint32_t d3d9 = call("Direct3DCreate9", 1, a_create);
    ok("Direct3DCreate9", d3d9 != 0);

    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = W;
    pp->BackBufferHeight = H;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8;
    pp->BackBufferCount = 1;

    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t a_cdev[7] = {
        d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, 0,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev)
    };
    uint32_t hr = call("IDirect3D9::CreateDevice", 7, a_cdev);
    ok("CreateDevice", hr == D3D_OK && *pDev != 0);
    uint32_t dev = *pDev;

    float z = 1.0f;
    uint32_t z_raw;
    memcpy(&z_raw, &z, 4);
    uint32_t pixels[W * H];

    /* Clear the backbuffer to red, then verify every pixel came from Metal. */
    uint32_t red = 0xFFFF0000u;
    uint32_t a_clr[7] = { dev, 0, 0, D3DCLEAR_TARGET, red, z_raw, 0 };
    hr = call("IDirect3DDevice9::Clear", 7, a_clr);
    ok("Clear(0xFFFF0000)", hr == D3D_OK);
    ok("Metal readback", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    int all_red = 1;
    for (int i = 0; i < W * H; i++) if (pixels[i] != red) { all_red = 0; break; }
    ok("all 4096 Metal pixels == 0xFFFF0000", all_red);
    if (!all_red) printf("    first mismatch: 0x%08X\n", pixels[0]);

    uint32_t blue = 0xFF0000FFu;
    a_clr[4] = blue;
    hr = call("IDirect3DDevice9::Clear", 7, a_clr);
    ok("Clear(0xFF0000FF)", hr == D3D_OK);
    ok("Metal readback", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    int all_blue = 1;
    for (int i = 0; i < W * H; i++) if (pixels[i] != blue) { all_blue = 0; break; }
    ok("all 4096 Metal pixels == 0xFF0000FF", all_blue);

    uint32_t a_rel[1] = { dev };
    call("IDirect3DDevice9::Release", 1, a_rel);
    a_rel[0] = d3d9;
    call("IDirect3D9::Release", 1, a_rel);
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_clear: OK\n" : "test_d3d9_clear: FAILED\n");
    return fails != 0;
}
