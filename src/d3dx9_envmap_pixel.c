/*
 * d3dx9_envmap_pixel.c - REAL METAL per-face pixel test for render-to-cube.
 *
 * Compiled only by d3dx9_envmap_metal_test.sh, which links d3d9_metal.m and
 * the Metal/Foundation frameworks. In the ordinary selftest build this file
 * is an empty translation unit (the macro below is not defined), so it cannot
 * add a second main() or unresolved Metal symbols.
 *
 * Every drawing step goes through D3D9 / D3DX calls; all colour assertions
 * read pixels from real MTLTextures (hle_d3d9_metal_read_pixels for the
 * currently bound render target and GetRenderTargetData for each cube face).
 * No simulated GPU and no CPU writes into a target.
 */
#ifdef D3DX9_ENVMAP_METAL_PIXELTEST
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_metal.h"
#include "d3dx9_hle.h"
#include "d3dx9_surface.h"          /* D3DX_FILTER_NONE */

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

#define W 32u
#define H 32u
#define BACK 0xFF00FFFFu            /* cyan */
/* Six independently chosen, pairwise different colours, one per cube face. */
static const uint32_t face_colors[6] = {
    0xFFFF0000u,    /* +X red     */
    0xFF00FF00u,    /* -X green   */
    0xFF0000FFu,    /* +Y blue    */
    0xFFFFFF00u,    /* -Y yellow  */
    0xFFFF00FFu,    /* +Z magenta */
    0xFFFFFFFFu     /* -Z white   */
};

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
static int read_current(uint32_t color, const char *what) {
    uint32_t *pixels = (uint32_t *)hle_alloc(W * H * 4u);
    int rc = hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4u);
    int good = 0;
    ok(what, rc == 0);
    if (rc == 0) {
        good = all_pixels(pixels, color);
        ok("all 32x32 Metal pixels match", good);
        if (!good) printf("    first pixel 0x%08X, expected 0x%08X\n", pixels[0], color);
    }
    hle_free(pixels);
    return good;
}
static uint32_t get_surface(uint32_t dev, const char *name, int argc) {
    uint32_t p = hle_va(hle_alloc(4));
    MEM32(p) = 0;
    uint32_t hr = argc == 3
        ? call(name, 3, (uint32_t[]){ dev, 0u, p })
        : call(name, 2, (uint32_t[]){ dev, p });
    ok(name, hr == D3D_OK && MEM32(p) != 0u);
    return MEM32(p);
}
static void get_vp(uint32_t dev, D3DVIEWPORT9 *vp) {
    uint32_t p = hle_va(hle_alloc(sizeof *vp));
    ok("GetViewport", call("IDirect3DDevice9::GetViewport", 2,
                           (uint32_t[]){ dev, p }) == D3D_OK);
    memcpy(vp, hle_ptr(p), sizeof *vp);
    hle_free(hle_ptr(p));
}
static int face_pixels(uint32_t dev, uint32_t face_surface, uint32_t sysmem, uint32_t color,
                       const char *what) {
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof *lr);
    ok("GetRenderTargetData(face, sysmem)",
       call("IDirect3DDevice9::GetRenderTargetData", 3,
            (uint32_t[]){ dev, face_surface, sysmem }) == D3D_OK);
    memset(lr, 0, sizeof *lr);
    ok("sysmem LockRect", call("IDirect3DSurface9::LockRect", 4,
        (uint32_t[]){ sysmem, A(lr), 0u, 0u }) == D3D_OK);
    const uint32_t *p = (const uint32_t *)hle_ptr(lr->pBits);
    int good = p && all_pixels(p, color);
    ok(what, good);
    if (!good && p) printf("    first face pixel 0x%08X, expected 0x%08X\n", p[0], color);
    call("IDirect3DSurface9::UnlockRect", 1, (uint32_t[]){ sysmem });
    hle_free(lr);
    return good;
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

    /* A cube whose six level-0 faces are real render targets. */
    uint32_t *pCube = (uint32_t *)hle_alloc(4);
    ok("CreateCubeTexture(32, 1, RENDERTARGET)",
       call("IDirect3DDevice9::CreateCubeTexture", 8,
            (uint32_t[]){ dev, W, 1u, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                          D3DPOOL_DEFAULT, A(pCube), 0u }) == D3D_OK && *pCube);
    uint32_t cube = *pCube;
    uint32_t *pSys = (uint32_t *)hle_alloc(4);
    ok("CreateOffscreenPlainSurface(SYSTEMMEM)",
       call("IDirect3DDevice9::CreateOffscreenPlainSurface", 7,
            (uint32_t[]){ dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, A(pSys), 0u }) == D3D_OK && *pSys);
    uint32_t sysmem = *pSys;
    uint32_t *pEnv = (uint32_t *)hle_alloc(4);
    ok("D3DXCreateRenderToEnvMap",
       call("D3DXCreateRenderToEnvMap", 7,
            (uint32_t[]){ dev, W, 1u, D3DFMT_A8R8G8B8, 1u, D3DFMT_D24S8, A(pEnv) }) == D3D_OK && *pEnv);
    uint32_t env = *pEnv;

    /* Original state, independent of what BeginCube/End touch. */
    D3DVIEWPORT9 vp_orig = { 1u, 2u, 20u, 21u, 0.25f, 0.75f };
    uint32_t vp_va = hle_va(hle_alloc(sizeof vp_orig));
    memcpy(hle_ptr(vp_va), &vp_orig, sizeof vp_orig);
    call("IDirect3DDevice9::SetViewport", 2, (uint32_t[]){ dev, vp_va });
    uint32_t old_rt = get_surface(dev, "IDirect3DDevice9::GetRenderTarget", 3);
    uint32_t old_ds = get_surface(dev, "IDirect3DDevice9::GetDepthStencilSurface", 2);
    D3DVIEWPORT9 original_vp;
    get_vp(dev, &original_vp);
    ok("device has an original render target and depth surface", old_rt && old_ds);

    call("IDirect3DDevice9::Clear", 7,
         (uint32_t[]){ dev, 0u, 0u, D3DCLEAR_TARGET, BACK, 0u, 0u });
    read_current(BACK, "backbuffer cleared cyan through Metal");

    uint32_t faces[6];
    for (uint32_t f = 0; f < 6; f++) {
        printf("face %u:\n", f);
        uint32_t *pFace = (uint32_t *)hle_alloc(4);
        ok("GetCubeMapSurface(face, 0)",
           call("IDirect3DCubeTexture9::GetCubeMapSurface", 4,
                (uint32_t[]){ cube, f, 0u, A(pFace) }) == D3D_OK && *pFace);
        faces[f] = *pFace;

        ok("BeginCube(cube)", call("dxe_BeginCube", 2, (uint32_t[]){ env, cube }) == D3D_OK);
        ok("Face(face, D3DX_FILTER_NONE)",
           call("dxe_Face", 3, (uint32_t[]){ env, f, D3DX_FILTER_NONE }) == D3D_OK);

        uint32_t now_rt = get_surface(dev, "IDirect3DDevice9::GetRenderTarget", 3);
        ok("envmap bound this cube face as render target", now_rt == faces[f]);
        call("IDirect3DDevice9::Clear", 7,
             (uint32_t[]){ dev, 0u, 0u, D3DCLEAR_TARGET, face_colors[f], 0u, 0u });
        char what[96];
        snprintf(what, sizeof what, "Metal cleared face %u to its own colour 0x%08X", f, face_colors[f]);
        read_current(face_colors[f], what);

        ok("End(D3DX_FILTER_NONE)",
           call("dxe_End", 2, (uint32_t[]){ env, D3DX_FILTER_NONE }) == D3D_OK);
        read_current(BACK, "backbuffer restored after End");
        uint32_t after_rt = get_surface(dev, "IDirect3DDevice9::GetRenderTarget", 3);
        uint32_t after_ds = get_surface(dev, "IDirect3DDevice9::GetDepthStencilSurface", 2);
        D3DVIEWPORT9 after_vp; get_vp(dev, &after_vp);
        ok("render target restored after End", after_rt == old_rt);
        ok("depth surface restored after End", after_ds == old_ds);
        ok("viewport restored after End",
           after_vp.X == original_vp.X && after_vp.Y == original_vp.Y &&
           after_vp.Width == original_vp.Width && after_vp.Height == original_vp.Height &&
           after_vp.MinZ == original_vp.MinZ && after_vp.MaxZ == original_vp.MaxZ);
        snprintf(what, sizeof what, "face %u's own GPU pixels kept its colour", f);
        face_pixels(dev, faces[f], sysmem, face_colors[f], what);
    }

    /* Read all six faces again after the whole pass: no face may have been
     * overwritten by a later one (this is what makes it a per-face test). */
    printf("final per-face verification:\n");
    for (uint32_t f = 0; f < 6; f++) {
        char what[96];
        snprintf(what, sizeof what, "face %u still 0x%08X", f, face_colors[f]);
        face_pixels(dev, faces[f], sysmem, face_colors[f], what);
    }

    call("dxe_Release", 1, (uint32_t[]){ env });
    for (uint32_t f = 0; f < 6; f++)
        call("IDirect3DSurface9::Release", 1, (uint32_t[]){ faces[f] });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ sysmem });
    call("IDirect3DCubeTexture9::Release", 1, (uint32_t[]){ cube });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();
    printf(fails ? "d3dx9_envmap_metal_test: FAILED (%d)\n" : "d3dx9_envmap_metal_test: OK\n", fails);
    return fails != 0;
}
#endif /* D3DX9_ENVMAP_METAL_PIXELTEST */
