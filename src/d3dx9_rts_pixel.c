/*
 * d3dx9_rts_pixel.c - REAL METAL pixel test for ID3DXRenderToSurface.
 *
 * Compiled only by d3dx9_rts_metal_test.sh, which links d3d9_metal.m and the
 * Metal/Foundation frameworks. In the ordinary selftest build this file is an
 * empty translation unit (the macro below is not defined), so it cannot add a
 * second main() or unresolved Metal symbols.
 *
 * Every rendering step goes through D3D9 / D3DX calls; all colour assertions
 * read pixels from real MTLTextures (hle_d3d9_metal_read_pixels and
 * GetRenderTargetData). No simulated GPU and no CPU writes into a target.
 */
#ifdef D3DX9_RTS_METAL_PIXELTEST
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_metal.h"
#include "d3dx9_hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

#define W 32u
#define H 32u
#define CYAN 0xFF00FFFFu
#define RED  0xFFFF0000u

static int fails;
static void ok(const char *name, int cond) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) fails++;
}
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
#define A(p) hle_va(p)

static int all_pixels(const uint32_t *p, uint32_t color) {
    for (uint32_t i = 0; i < W * H; ++i) if (p[i] != color) return 0;
    return 1;
}
static int read_current(uint32_t *pixels, uint32_t color, const char *what) {
    int rc = hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4u);
    ok(what, rc == 0);
    if (rc != 0) return 0;
    if (!all_pixels(pixels, color)) {
        printf("    first pixel 0x%08X, expected 0x%08X\n", pixels[0], color);
        ok("pixel values", 0);
        return 0;
    }
    ok("all 32x32 Metal pixels match", 1);
    return 1;
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    win32hle_register(win32hle_d3dx9);
    ok("real Metal backend enabled", hle_d3d9_metal_enable() == 0 && hle_d3d9_metal_is_active());

    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32u });
    ok("Direct3DCreate9", d3d9 != 0u);
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof *pp);
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = W; pp->BackBufferHeight = H;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    pp->Windowed = 1; pp->SwapEffect = 1;
    pp->EnableAutoDepthStencil = 1; pp->AutoDepthStencilFormat = D3DFMT_D24S8;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    ok("CreateDevice", call("IDirect3D9::CreateDevice", 7,
        (uint32_t[]){ d3d9, 0u, D3DDEVTYPE_HAL, 0u,
                      D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev) }) == D3D_OK && *pDev);
    uint32_t dev = *pDev;

    uint32_t *pDest = (uint32_t *)hle_alloc(4);
    ok("CreateRenderTarget(dest 32x32)",
       call("IDirect3DDevice9::CreateRenderTarget", 9,
            (uint32_t[]){ dev, W, H, D3DFMT_A8R8G8B8, 0u, 0u, 0u, A(pDest), 0u }) == D3D_OK && *pDest);
    uint32_t dest = *pDest;
    uint32_t *pSys = (uint32_t *)hle_alloc(4);
    ok("CreateOffscreenPlainSurface(SYSTEMMEM)",
       call("IDirect3DDevice9::CreateOffscreenPlainSurface", 7,
            (uint32_t[]){ dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, A(pSys), 0u }) == D3D_OK && *pSys);
    uint32_t sysmem = *pSys;

    uint32_t *pRts = (uint32_t *)hle_alloc(4);
    ok("D3DXCreateRenderToSurface",
       call("D3DXCreateRenderToSurface", 7,
            (uint32_t[]){ dev, W, H, D3DFMT_A8R8G8B8, 1u, D3DFMT_D24S8, A(pRts) }) == D3D_OK && *pRts);
    uint32_t rts = *pRts;

    /* Original state, set to values independent of what BeginScene installs. */
    D3DVIEWPORT9 vp_orig = { 1, 2, 20, 21, 0.25f, 0.75f };
    uint32_t vp_va = hle_va(hle_alloc(sizeof vp_orig));
    memcpy(hle_ptr(vp_va), &vp_orig, sizeof vp_orig);
    call("IDirect3DDevice9::SetViewport", 2, (uint32_t[]){ dev, vp_va });
    uint32_t *pOldRT = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::GetRenderTarget", 3, (uint32_t[]){ dev, 0u, A(pOldRT) });
    uint32_t old_rt = *pOldRT;
    uint32_t *pOldDS = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::GetDepthStencilSurface", 2, (uint32_t[]){ dev, A(pOldDS) });
    uint32_t old_ds = *pOldDS;
    ok("device has an original render target and depth surface", old_rt && old_ds);

    uint32_t *pixels = (uint32_t *)hle_alloc(W * H * 4u);
    call("IDirect3DDevice9::Clear", 7,
         (uint32_t[]){ dev, 0u, 0u, D3DCLEAR_TARGET, CYAN, 0u, 0u });
    read_current(pixels, CYAN, "backbuffer cleared cyan through Metal");

    /* A viewport different from the original, to prove it is restored. */
    D3DVIEWPORT9 vp_scene = { 4, 6, 8, 10, 0.5f, 0.9f };
    uint32_t vp_scene_va = hle_va(hle_alloc(sizeof vp_scene));
    memcpy(hle_ptr(vp_scene_va), &vp_scene, sizeof vp_scene);
    ok("BeginScene(dest, scene viewport)",
       call("dxr_BeginScene", 3, (uint32_t[]){ rts, dest, vp_scene_va }) == D3D_OK);

    uint32_t *pNowRT = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::GetRenderTarget", 3, (uint32_t[]){ dev, 0u, A(pNowRT) });
    ok("RTS bound the destination as device render target", *pNowRT == dest);
    call("IDirect3DDevice9::Clear", 7,
         (uint32_t[]){ dev, 0u, 0u, D3DCLEAR_TARGET, RED, 0u, 0u });
    read_current(pixels, RED, "Metal cleared the RTS target red (real RT switch)");

    ok("EndScene(D3DX_FILTER_NONE)",
       call("dxr_EndScene", 2, (uint32_t[]){ rts, 1u }) == D3D_OK);
    read_current(pixels, CYAN, "Metal target is cyan again (render target restored)");

    uint32_t *pAfterRT = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::GetRenderTarget", 3, (uint32_t[]){ dev, 0u, A(pAfterRT) });
    ok("device render target restored", *pAfterRT == old_rt);
    uint32_t *pAfterDS = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::GetDepthStencilSurface", 2, (uint32_t[]){ dev, A(pAfterDS) });
    ok("device depth surface restored", *pAfterDS == old_ds);
    uint32_t vp_after_va = hle_va(hle_alloc(sizeof vp_orig));
    call("IDirect3DDevice9::GetViewport", 2, (uint32_t[]){ dev, vp_after_va });
    D3DVIEWPORT9 vp_after;
    memcpy(&vp_after, hle_ptr(vp_after_va), sizeof vp_after);
    ok("device viewport restored",
       vp_after.X == vp_orig.X && vp_after.Y == vp_orig.Y &&
       vp_after.Width == vp_orig.Width && vp_after.Height == vp_orig.Height &&
       vp_after.MinZ == vp_orig.MinZ && vp_after.MaxZ == vp_orig.MaxZ);

    /* Read the destination's own GPU texture: proves the red clear landed in
     * the RTS surface, not merely in whichever target was bound. */
    ok("GetRenderTargetData(dest, sysmem)",
       call("IDirect3DDevice9::GetRenderTargetData", 3,
            (uint32_t[]){ dev, dest, sysmem }) == D3D_OK);
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof *lr);
    ok("sysmem LockRect",
       call("IDirect3DSurface9::LockRect", 4,
            (uint32_t[]){ sysmem, A(lr), 0u, 0u }) == D3D_OK);
    const uint32_t *readback = (const uint32_t *)hle_ptr(lr->pBits);
    ok("all pixels read from the RTS destination are red", all_pixels(readback, RED));
    if (!all_pixels(readback, RED)) printf("    first dest pixel 0x%08X\n", readback[0]);
    call("IDirect3DSurface9::UnlockRect", 1, (uint32_t[]){ sysmem });

    call("dxr_Release", 1, (uint32_t[]){ rts });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ sysmem });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ dest });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();
    printf(fails ? "d3dx9_rts_metal_test: FAILED (%d)\n" : "d3dx9_rts_metal_test: OK\n", fails);
    return fails != 0;
}
#endif /* D3DX9_RTS_METAL_PIXELTEST */
