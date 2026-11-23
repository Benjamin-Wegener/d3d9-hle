/*
 * test_d3d9_mipmaps.c - Real Metal pixel test for 2D texture mipmaps.
 *
 * Requirements covered: 2D texture with mip chain, Levels=0 full chain,
 * LockRect/UnlockRect per level (correct pitch, DXT/odd sizes basic),
 * invalid level INVALIDCALL, UpdateTexture, MIPFILTER sampler, D3DX
 * CreateTexture/FilterTexture, pixel test where each level has different
 * color and a scaled draw picks the correct mip.
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

typedef struct { float x,y,z,w; float r,g,b,a; float u,v; } Vtx;
static Vtx vtx_ndc(float px, float py, float u, float v) {
    Vtx t;
    t.x = (px / (float)W) * 2.0f - 1.0f;
    t.y = 1.0f - (py / (float)H) * 2.0f;
    t.z = 0.5f; t.w = 1.0f;
    t.r = 1; t.g = 1; t.b = 1; t.a = 1;
    t.u = u; t.v = v;
    return t;
}
static int all_pixels(uint32_t *pixels, uint32_t color) {
    for (int i = 0; i < W * H; i++) if (pixels[i] != color) return 0;
    return 1;
}

static void fill_level(uint32_t tex, uint32_t level, uint32_t w, uint32_t h, uint32_t argb) {
    uint32_t *pLR = (uint32_t *)hle_alloc(8);
    uint32_t hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ tex, level, A(pLR), 0, 0 });
    char msg[64];
    snprintf(msg, sizeof msg, "LockRect level %u", level);
    ok(msg, hr == D3D_OK);
    if (hr != D3D_OK) return;
    uint32_t pitch = pLR[0];
    uint8_t *bits = (uint8_t *)hle_ptr(pLR[1]);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(bits + y * pitch);
        for (uint32_t x = 0; x < w; x++) row[x] = argb;
    }
    hr = call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ tex, level });
    snprintf(msg, sizeof msg, "UnlockRect level %u", level);
    ok(msg, hr == D3D_OK);
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) { printf("  [FAIL] Metal backend available\n"); return 1; }
    ok("Metal backend available", hle_d3d9_metal_is_active());
    printf("=== Test: D3D9 2D mipmaps (Metal) ===\n");

    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    ok("Direct3DCreate9", d3d9 != 0);
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = W; pp->BackBufferHeight = H;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3D9::CreateDevice", 7,
        (uint32_t[]){ d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, 0,
                      D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev) });
    ok("CreateDevice", hr == D3D_OK && *pDev != 0);
    uint32_t dev = *pDev;

    /* Levels=0 on 4x4 gives full chain: 4x4,2x2,1x1 = 3 levels. */
    uint32_t *pTex = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::CreateTexture", 9,
        (uint32_t[]){ dev, 4, 4, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 });
    ok("CreateTexture 4x4 levels=0", hr == D3D_OK && *pTex != 0);
    uint32_t tex = *pTex;
    uint32_t levels = call("IDirect3DTexture9::GetLevelCount", 1, (uint32_t[]){ tex });
    ok("Levels=0 gives 3 for 4x4", levels == 3);

    /* Invalid level INVALIDCALL. */
    uint32_t *pLR = (uint32_t *)hle_alloc(8);
    hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ tex, 3, A(pLR), 0, 0 });
    ok("LockRect invalid level INVALIDCALL", hr == D3DERR_INVALIDCALL);
    hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ tex, 99, A(pLR), 0, 0 });
    ok("LockRect level 99 INVALIDCALL", hr == D3DERR_INVALIDCALL);

    /* Upload each level with different color. Level 0 red, 1 green, 2 blue. */
    fill_level(tex, 0, 4, 4, 0xFFFF0000u);
    fill_level(tex, 1, 2, 2, 0xFF00FF00u);
    fill_level(tex, 2, 1, 1, 0xFF0000FFu);

    /* Bind texture and set sampler to POINT/POINT/POINT so mip selection is clear. */
    call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex });
    call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 5, 1 }); /* MAG POINT */
    call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 6, 1 }); /* MIN POINT */
    call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 7, 1 }); /* MIP POINT */
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });

    /* Draw a fullscreen quad sampling the texture - should pick level 0 (large). */
    Vtx quad[6] = {
        vtx_ndc(0,0,0,0), vtx_ndc(32,0,1,0), vtx_ndc(0,32,0,1),
        vtx_ndc(0,32,0,1), vtx_ndc(32,0,1,0), vtx_ndc(32,32,1,1)
    };
    uint32_t vb_bytes = sizeof(quad);
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer", 7, (uint32_t[]){ dev, vb_bytes, 0, (0x400u|0x040u|0x100u), D3DPOOL_MANAGED, A(pVB), 0 });
    uint32_t *pVBuf = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, vb_bytes, A(pVBuf), 0 });
    memcpy(hle_ptr(*pVBuf), quad, vb_bytes);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
    call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, *pVB, 0, sizeof(Vtx) });

    float z = 1.0f; uint32_t zraw; memcpy(&zraw, &z, 4);
    uint32_t pixels[W*H];
    call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, zraw, 0 });
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    ok("Metal readback level0", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    /* fullscreen samples level 0: red */
    ok("fullscreen draw samples level 0 (red)", all_pixels(pixels, 0xFFFF0000u));

    /* Draw a 1x1 pixel quad - should pick smallest mip (level 2, blue). */
    Vtx tiny[6] = {
        vtx_ndc(16,16,0,0), vtx_ndc(17,16,1,0), vtx_ndc(16,17,0,1),
        vtx_ndc(16,17,0,1), vtx_ndc(17,16,1,0), vtx_ndc(17,17,1,1)
    };
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, vb_bytes, A(pVBuf), 0 });
    memcpy(hle_ptr(*pVBuf), tiny, sizeof(tiny));
    memset((uint8_t*)hle_ptr(*pVBuf) + sizeof(tiny), 0, vb_bytes - sizeof(tiny));
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
    call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, zraw, 0 });
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    ok("Metal readback tiny", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("1x1 draw samples mip (blue or green)", pixels[16*W+16] == 0xFF0000FFu || pixels[16*W+16] == 0xFF00FF00u);

    /* UpdateTexture: copy from src texture to dst texture, all mip levels. */
    uint32_t *pSrc = (uint32_t *)hle_alloc(4), *pDst = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::CreateTexture", 9, (uint32_t[]){ dev,4,4,0,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,A(pSrc),0 });
    ok("CreateTexture src SYSTEMMEM", hr == D3D_OK);
    hr = call("IDirect3DDevice9::CreateTexture", 9, (uint32_t[]){ dev,4,4,0,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,A(pDst),0 });
    ok("CreateTexture dst DEFAULT", hr == D3D_OK);
    fill_level(*pSrc, 0, 4, 4, 0xFFFF00FFu);
    fill_level(*pSrc, 1, 2, 2, 0xFFFF00FFu);
    fill_level(*pSrc, 2, 1, 1, 0xFFFF00FFu);
    hr = call("IDirect3DDevice9::UpdateTexture", 3, (uint32_t[]){ dev, *pSrc, *pDst });
    ok("UpdateTexture OK", hr == D3D_OK);
    call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, *pDst });
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, vb_bytes, A(pVBuf), 0 });
    memcpy(hle_ptr(*pVBuf), quad, sizeof(quad));
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
    call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, zraw, 0 });
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    ok("Metal readback UpdateTexture", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("UpdateTexture uploaded magenta", all_pixels(pixels, 0xFFFF00FFu));

    /* D3DXCreateTexture with MipLevels=0 gives full chain. */
    uint32_t *pDx = (uint32_t *)hle_alloc(4);
    hr = call("D3DXCreateTexture", 8, (uint32_t[]){ dev, 4, 4, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pDx) });
    ok("D3DXCreateTexture", hr == D3D_OK && *pDx != 0);
    if (*pDx) {
        uint32_t lc = call("IDirect3DTexture9::GetLevelCount", 1, (uint32_t[]){ *pDx });
        ok("D3DXCreateTexture levels=0 full chain", lc == 3);
    }

    /* D3DXFilterTexture generates lower mips from level 0. */
    {
        uint32_t *pFt = (uint32_t *)hle_alloc(4);
        hr = call("IDirect3DDevice9::CreateTexture", 9, (uint32_t[]){ dev,4,4,0,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(pFt),0 });
        ok("FilterTexture tex", hr == D3D_OK && *pFt);
        fill_level(*pFt, 0, 4, 4, 0xFFFFFFFFu);
        /* zero level 1 */
        fill_level(*pFt, 1, 2, 2, 0xFF000000u);
        hr = call("D3DXFilterTexture", 4, (uint32_t[]){ *pFt, 0, 0, 0xFFFFFFFFu /*D3DX_DEFAULT*/ });
        ok("D3DXFilterTexture hr", hr == D3D_OK);
        /* lock level 1 and verify it got non-black data */
        uint32_t *lr = (uint32_t *)hle_alloc(8);
        hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ *pFt, 1, A(lr), 0, 0 });
        ok("FilterTexture LockRect level1", hr == D3D_OK);
        if (hr == D3D_OK) {
            uint8_t *bits = (uint8_t *)hle_ptr(lr[1]);
            uint32_t pix = *(uint32_t *)bits;
            ok("FilterTexture generated level1", pix != 0xFF000000u);
            call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ *pFt, 1 });
        }
    }

    /* DXT1 mip chain: 4x4,2x2,1x1 - pitches 8,8,8. Upload and sample. */
    {
        uint32_t *pDx1 = (uint32_t *)hle_alloc(4);
        hr = call("IDirect3DDevice9::CreateTexture", 9, (uint32_t[]){ dev,4,4,0,0,D3DFMT_DXT1,D3DPOOL_MANAGED,A(pDx1),0 });
        ok("DXT1 CreateTexture 4x4 levels=0", hr == D3D_OK && *pDx1);
        if (*pDx1) {
            uint32_t lc = call("IDirect3DTexture9::GetLevelCount", 1, (uint32_t[]){ *pDx1 });
            ok("DXT1 levels=0 gives 3", lc == 3);
            /* DXT1 solid red: color0=0xF800(565 red), color1=0xF800, indices=0 */
            uint8_t block_red[8] = { 0x00,0xF8, 0x00,0xF8, 0,0,0,0 };
            uint8_t block_green[8] = { 0xE0,0x07, 0xE0,0x07, 0,0,0,0 };
            uint8_t block_blue[8] = { 0x1F,0x00, 0x1F,0x00, 0,0,0,0 };
            uint32_t *lr = (uint32_t *)hle_alloc(8);
            hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ *pDx1, 0, A(lr), 0, 0 });
            ok("DXT1 LockRect level0", hr == D3D_OK && lr[0] == 8);
            if (hr == D3D_OK) { memcpy(hle_ptr(lr[1]), block_red, 8); call("IDirect3DTexture9::UnlockRect",2,(uint32_t[]){*pDx1,0}); }
            hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ *pDx1, 1, A(lr), 0, 0 });
            ok("DXT1 LockRect level1 pitch 8", hr == D3D_OK && lr[0] == 8);
            if (hr == D3D_OK) { memcpy(hle_ptr(lr[1]), block_green, 8); call("IDirect3DTexture9::UnlockRect",2,(uint32_t[]){*pDx1,1}); }
            hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ *pDx1, 2, A(lr), 0, 0 });
            ok("DXT1 LockRect level2 pitch 8", hr == D3D_OK && lr[0] == 8);
            if (hr == D3D_OK) { memcpy(hle_ptr(lr[1]), block_blue, 8); call("IDirect3DTexture9::UnlockRect",2,(uint32_t[]){*pDx1,2}); }
            call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, *pDx1 });
            call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 5, 1 });
            call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 6, 1 });
            call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 7, 1 });
            call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });
            call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, zraw, 0 });
            call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
            call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
            call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
            ok("DXT1 readback", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
            /* red level0 sampled at fullscreen; DXT1 red 565 -> 0xF8 in R */
            ok("DXT1 fullscreen samples red", (pixels[16*W+16] & 0x00FF0000u) >= 0x00F00000u && (pixels[16*W+16] & 0x0000FFFFu) == 0);
        }
    }

    /* Odd size 3x3 levels=0 -> 2 levels (3x3,1x1). */
    {
        uint32_t *pOdd = (uint32_t *)hle_alloc(4);
        hr = call("IDirect3DDevice9::CreateTexture", 9, (uint32_t[]){ dev,3,3,0,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(pOdd),0 });
        ok("Odd 3x3 CreateTexture", hr == D3D_OK && *pOdd);
        if (*pOdd) {
            uint32_t lc = call("IDirect3DTexture9::GetLevelCount", 1, (uint32_t[]){ *pOdd });
            ok("Odd 3x3 levels=0 gives 2", lc == 2);
            uint32_t *lr = (uint32_t *)hle_alloc(8);
            hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ *pOdd, 1, A(lr), 0, 0 });
            ok("Odd 3x3 level1 lock ok", hr == D3D_OK);
            if (hr == D3D_OK) {
                ok("Odd 3x3 level1 pitch >= 4", lr[0] >= 4);
                call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ *pOdd, 1 });
            }
            hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ *pOdd, 2, A(lr), 0, 0 });
            ok("Odd 3x3 level2 INVALIDCALL", hr == D3DERR_INVALIDCALL);
        }
    }

    /* MIPFILTER LINEAR: no AGX/crash, samples from mip chain. */
    call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex });
    call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 5, 2 }); /* MAG LINEAR */
    call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 6, 2 }); /* MIN LINEAR */
    call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 7, 2 }); /* MIP LINEAR */
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });
    call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, zraw, 0 });
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    ok("MIPFILTER LINEAR readback", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("MIPFILTER LINEAR sampled non-black", (pixels[16*W+16] & 0x00FFFFFFu) != 0);

    /* Cube texture: Levels=0 full chain, LockRect/UnlockRect OK. */
    {
        uint32_t *pCube = (uint32_t *)hle_alloc(4);
        hr = call("IDirect3DDevice9::CreateCubeTexture", 8, (uint32_t[]){ dev, 4, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pCube), 0 });
        ok("CreateCubeTexture 4 levels=0", hr == D3D_OK && *pCube);
        if (*pCube) {
            uint32_t lc = call("IDirect3DCubeTexture9::GetLevelCount", 1, (uint32_t[]){ *pCube });
            ok("Cube levels=0 gives 3", lc == 3);
            uint32_t *lr = (uint32_t *)hle_alloc(8);
            hr = call("IDirect3DCubeTexture9::LockRect", 6, (uint32_t[]){ *pCube, 0, 1, A(lr), 0, 0 });
            ok("Cube LockRect level1 OK", hr == D3D_OK);
            if (hr == D3D_OK) {
                uint32_t pitch = lr[0];
                ok("Cube level1 pitch > 0", pitch >= 8);
                uint8_t *bits = (uint8_t *)hle_ptr(lr[1]);
                for (uint32_t i=0; i<2*2; i++) ((uint32_t*)(bits))[i] = 0xFFFFFF00u;
                hr = call("IDirect3DCubeTexture9::UnlockRect", 3, (uint32_t[]){ *pCube, 0, 1 });
                ok("Cube UnlockRect level1 OK", hr == D3D_OK);
            }
            hr = call("IDirect3DCubeTexture9::LockRect", 6, (uint32_t[]){ *pCube, 0, 3, A(lr), 0, 0 });
            ok("Cube invalid level INVALIDCALL", hr == D3DERR_INVALIDCALL);
        }
    }

    if (fails) { printf("test_d3d9_mipmaps: %d failed\n", fails); return 1; }
    printf("test_d3d9_mipmaps: ok\n");
    return 0;
}
