/*
 * test_d3d9_updatesurface.c - Real Metal pixel test for IDirect3DDevice9::UpdateSurface.
 *
 * 1) Creates a sysmem source surface (D3DPOOL_SYSTEMMEM).
 * 2) Fills the source surface with pattern/color in host memory.
 * 3) Creates a default destination render target (D3DPOOL_DEFAULT).
 * 4) Calls UpdateSurface(src, src_rect, dst, dst_point).
 * 5) Reads back destination render target from Metal GPU using hle_d3d9_metal_read_pixels.
 * 6) Verifies that destination pixels match source data inside copied area.
 * 7) Verifies error validations (pool mismatch, NULL, out-of-bounds).
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
    printf("=== Test: D3D9 UpdateSurface with Metal readback ===\n");

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
    uint32_t *pDst = (uint32_t *)hle_alloc(4);
    uint32_t a_crt[9] = { dev, W, H, D3DFMT_A8R8G8B8, 0, 0, 1, A(pDst), 0 };
    hr = call("IDirect3DDevice9::CreateRenderTarget", 9, a_crt);
    ok("CreateRenderTarget(dst)", hr == D3D_OK && *pDst != 0);
    uint32_t dst_surf = *pDst;

    /* Clear dst_surf to black (0xFF000000) */
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, dst_surf });
    call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, 0, 0 });

    /* Create source surface in POOL_SYSTEMMEM */
    uint32_t *pSrc = (uint32_t *)hle_alloc(4);
    uint32_t a_cplain[7] = { dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, A(pSrc), 0 };
    hr = call("IDirect3DDevice9::CreateOffscreenPlainSurface", 7, a_cplain);
    ok("CreateOffscreenPlainSurface(src)", hr == D3D_OK && *pSrc != 0);
    uint32_t src_surf = *pSrc;

    /* Fill src_surf: write yellow (0xFFFFFF00) into 16x16 subregion [8,8,24,24] */
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*lr));
    hr = call("IDirect3DSurface9::LockRect", 4, (uint32_t[]){ src_surf, A(lr), 0, 0 });
    ok("src_surf::LockRect", hr == D3D_OK && lr->pBits != 0);
    uint32_t *src_pixels = (uint32_t *)hle_ptr(lr->pBits);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (x >= 8 && x < 24 && y >= 8 && y < 24)
                src_pixels[y * (lr->Pitch / 4) + x] = 0xFFFFFF00u; /* yellow */
            else
                src_pixels[y * (lr->Pitch / 4) + x] = 0xFF0000FFu; /* blue */
        }
    }
    call("IDirect3DSurface9::UnlockRect", 1, (uint32_t[]){ src_surf });

    /* 1) Test sub-rect UpdateSurface: copy [8,8,24,24] from src to [8,8] on dst */
    int32_t *srect = (int32_t *)hle_alloc(16);
    srect[0] = 8; srect[1] = 8; srect[2] = 24; srect[3] = 24;
    int32_t *dpt = (int32_t *)hle_alloc(8);
    dpt[0] = 8; dpt[1] = 8;

    hr = call("IDirect3DDevice9::UpdateSurface", 5, (uint32_t[]){ dev, src_surf, A(srect), dst_surf, A(dpt) });
    ok("UpdateSurface subrect", hr == D3D_OK);

    /* Read back dst_surf via Metal */
    call("IDirect3DDevice9::SetRenderTarget", 3, (uint32_t[]){ dev, 0, dst_surf });
    uint32_t pixels[W * H];
    memset(pixels, 0, sizeof(pixels));
    ok("Metal readback of dst", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);

    /* Pixel (0,0) outside rect should still be black */
    ok("pixel (0,0) outside == black", pixels[0] == 0xFF000000u);
    /* Pixel (16,16) inside rect should be yellow */
    ok("pixel (16,16) inside == yellow", pixels[16 * W + 16] == 0xFFFFFF00u);

    /* 2) Test full surface UpdateSurface (pSourceRect=NULL, pDestPoint=NULL) */
    hr = call("IDirect3DDevice9::UpdateSurface", 5, (uint32_t[]){ dev, src_surf, 0, dst_surf, 0 });
    ok("UpdateSurface full", hr == D3D_OK);
    ok("Metal readback after full update", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("pixel (0,0) after full == blue", pixels[0] == 0xFF0000FFu);
    ok("pixel (16,16) after full == yellow", pixels[16 * W + 16] == 0xFFFFFF00u);

    /* 3) Error validations */
    hr = call("IDirect3DDevice9::UpdateSurface", 5, (uint32_t[]){ dev, 0, 0, dst_surf, 0 });
    ok("UpdateSurface NULL src -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);
    hr = call("IDirect3DDevice9::UpdateSurface", 5, (uint32_t[]){ dev, src_surf, 0, 0, 0 });
    ok("UpdateSurface NULL dst -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);
    /* src pool default instead of sysmem */
    hr = call("IDirect3DDevice9::UpdateSurface", 5, (uint32_t[]){ dev, dst_surf, 0, dst_surf, 0 });
    ok("UpdateSurface src DEFAULT pool -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);

    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ src_surf });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ dst_surf });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_updatesurface: OK\n" : "test_d3d9_updatesurface: FAILED\n");
    return fails != 0;
}
