/*
 * test_d3d9_getrendertargetdata.c - Real Metal pixel test for GetRenderTargetData.
 *
 * 1) Creates an offscreen render target surface (rt) in POOL_DEFAULT.
 * 2) Creates an offscreen plain surface (dst_surf) in POOL_SYSTEMMEM.
 * 3) Clears rt to a distinctive color: violet (0xFF800080).
 * 4) Calls GetRenderTargetData(rt, dst_surf) to read GPU pixels into dst_surf.
 * 5) Locks dst_surf with LockRect and directly inspects host memory pixels.
 *    If GetRenderTargetData did not perform a real readback from the Metal GPU,
 *    dst_surf would contain 0 or garbage and the test fails.
 * 6) All rendering is performed by the real Metal backend.
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
    printf("=== Test: D3D9 GetRenderTargetData with Metal pixel readback ===\n");

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

    /* Create Render Target in POOL_DEFAULT */
    uint32_t *pRT = (uint32_t *)hle_alloc(4);
    uint32_t a_crt[9] = { dev, W, H, D3DFMT_A8R8G8B8, 0, 0, 1, A(pRT), 0 };
    hr = call("IDirect3DDevice9::CreateRenderTarget", 9, a_crt);
    ok("CreateRenderTarget", hr == D3D_OK && *pRT != 0);
    uint32_t rt = *pRT;

    /* Create Offscreen Plain Surface in POOL_SYSTEMMEM */
    uint32_t *pDest = (uint32_t *)hle_alloc(4);
    uint32_t a_cplain[7] = { dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, A(pDest), 0 };
    hr = call("IDirect3DDevice9::CreateOffscreenPlainSurface", 7, a_cplain);
    ok("CreateOffscreenPlainSurface(SYSTEMMEM)", hr == D3D_OK && *pDest != 0);
    uint32_t dst_surf = *pDest;

    /* Bind rt and clear to Violet (0xFF800080) */
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, rt });
    const uint32_t violet = 0xFF800080u;
    uint32_t a_clr[7] = { dev, 0, 0, D3DCLEAR_TARGET, violet, 0, 0 };
    hr = call("IDirect3DDevice9::Clear", 7, a_clr);
    ok("Clear(rt, violet)", hr == D3D_OK);

    /* Call GetRenderTargetData to copy from GPU render target to system memory surface */
    hr = call("IDirect3DDevice9::GetRenderTargetData", 3, (uint32_t[]){ dev, rt, dst_surf });
    ok("GetRenderTargetData(rt, dst_surf)", hr == D3D_OK);

    /* Lock dst_surf and verify all pixels are violet */
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*lr));
    hr = call("IDirect3DSurface9::LockRect", 4, (uint32_t[]){ dst_surf, A(lr), 0, D3DLOCK_READONLY });
    ok("dst_surf::LockRect", hr == D3D_OK && lr->pBits != 0);

    uint32_t *pixels = (uint32_t *)hle_ptr(lr->pBits);
    int all_match = 1;
    for (int i = 0; i < W * H; i++) {
        if (pixels[i] != violet) {
            all_match = 0;
            break;
        }
    }
    ok("all dst_surf pixels read from GPU == violet", all_match);
    if (!all_match) {
        printf("    got pixel 0 = 0x%08X (expected 0x%08X)\n", pixels[0], violet);
    }
    call("IDirect3DSurface9::UnlockRect", 1, (uint32_t[]){ dst_surf });

    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ dst_surf });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ rt });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_getrendertargetdata: OK\n" : "test_d3d9_getrendertargetdata: FAILED\n");
    return fails != 0;
}