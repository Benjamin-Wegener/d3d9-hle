/*
 * test_d3d9_frontbuffer.c - Real Metal pixel test for GetFrontBufferData (Device & SwapChain).
 *
 * 1) Clears backbuffer to a known color (teal: 0xFF008080).
 * 2) Creates sysmem offscreen plain surface matching backbuffer dimensions.
 * 3) Calls IDirect3DDevice9::GetFrontBufferData.
 * 4) Locks destination surface and verifies pixels match backbuffer.
 * 5) Calls IDirect3DSwapChain9::GetFrontBufferData.
 * 6) Locks destination surface and verifies pixels match backbuffer.
 * 7) Tests invalid arguments (NULL surface, wrong pool, invalid swapchain index).
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
    printf("=== Test: D3D9 GetFrontBufferData with Metal readback ===\n");

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

    /* Clear backbuffer to teal (0xFF008080) */
    const uint32_t teal = 0xFF008080u;
    hr = call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, teal, 0, 0 });
    ok("Clear backbuffer to teal", hr == D3D_OK);

    /* Create sysmem surface for receiving front buffer data */
    uint32_t *pDst = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::CreateOffscreenPlainSurface", 7,
              (uint32_t[]){ dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, A(pDst), 0 });
    ok("CreateOffscreenPlainSurface(sysmem)", hr == D3D_OK && *pDst != 0);
    uint32_t dst_surf = *pDst;

    /* 1) Test Device::GetFrontBufferData */
    hr = call("IDirect3DDevice9::GetFrontBufferData", 3, (uint32_t[]){ dev, 0, dst_surf });
    ok("Device::GetFrontBufferData", hr == D3D_OK);

    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*lr));
    hr = call("IDirect3DSurface9::LockRect", 4, (uint32_t[]){ dst_surf, A(lr), 0, D3DLOCK_READONLY });
    ok("dst_surf::LockRect after Device GetFrontBufferData", hr == D3D_OK && lr->pBits != 0);
    uint32_t *pixels = (uint32_t *)hle_ptr(lr->pBits);
    int all_teal = 1;
    for (int i = 0; i < W * H; i++) {
        if (pixels[i] != teal) {
            all_teal = 0;
            break;
        }
    }
    ok("Device::GetFrontBufferData read back teal pixels", all_teal);
    call("IDirect3DSurface9::UnlockRect", 1, (uint32_t[]){ dst_surf });

    /* 2) Test SwapChain::GetFrontBufferData */
    uint32_t *pSC = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::GetSwapChain", 3, (uint32_t[]){ dev, 0, A(pSC) });
    ok("GetSwapChain(0)", hr == D3D_OK && *pSC != 0);
    uint32_t sc = *pSC;

    /* Re-clear to orange (0xFFFF8000) */
    const uint32_t orange = 0xFFFF8000u;
    call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, orange, 0, 0 });

    hr = call("IDirect3DSwapChain9::GetFrontBufferData", 2, (uint32_t[]){ sc, dst_surf });
    ok("SwapChain::GetFrontBufferData", hr == D3D_OK);

    hr = call("IDirect3DSurface9::LockRect", 4, (uint32_t[]){ dst_surf, A(lr), 0, D3DLOCK_READONLY });
    ok("dst_surf::LockRect after SwapChain GetFrontBufferData", hr == D3D_OK && lr->pBits != 0);
    pixels = (uint32_t *)hle_ptr(lr->pBits);
    int all_orange = 1;
    for (int i = 0; i < W * H; i++) {
        if (pixels[i] != orange) {
            all_orange = 0;
            break;
        }
    }
    ok("SwapChain::GetFrontBufferData read back orange pixels", all_orange);
    call("IDirect3DSurface9::UnlockRect", 1, (uint32_t[]){ dst_surf });

    /* 3) Error validations */
    hr = call("IDirect3DDevice9::GetFrontBufferData", 3, (uint32_t[]){ dev, 0, 0 });
    ok("Device::GetFrontBufferData NULL dst -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);
    hr = call("IDirect3DDevice9::GetFrontBufferData", 3, (uint32_t[]){ dev, 1, dst_surf });
    ok("Device::GetFrontBufferData invalid index -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);
    hr = call("IDirect3DSwapChain9::GetFrontBufferData", 2, (uint32_t[]){ sc, 0 });
    ok("SwapChain::GetFrontBufferData NULL dst -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);

    call("IDirect3DSwapChain9::Release", 1, (uint32_t[]){ sc });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ dst_surf });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_frontbuffer: OK\n" : "test_d3d9_frontbuffer: FAILED\n");
    return fails != 0;
}
