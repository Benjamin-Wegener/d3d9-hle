/*
 * test_d3d9_texture.c - Real Metal pixel test for texture upload and sampling.
 *
 * Texture data goes up through IDirect3DTexture9::LockRect/UnlockRect, it is
 * bound with SetTexture, a quad is drawn via DrawPrimitive, and the sampled
 * pixels are read back from the Metal render target. All drawing is done by
 * the real Metal backend, with no simulated device and no rasteriser.
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

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
} Vertex;

static Vertex quad_vertex(float x, float y, float u, float v) {
    Vertex t;
    t.x = x; t.y = y; t.z = 0.5f; t.w = 1.0f;
    t.r = 1.0f; t.g = 1.0f; t.b = 1.0f; t.a = 1.0f;
    t.u = u; t.v = v;
    return t;
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) {
        printf("  [FAIL] Metal backend available\n");
        return 1;
    }
    ok("Metal backend available", hle_d3d9_metal_is_active());
    printf("=== Test: D3D9 texture with Metal sampling and readback ===\n");

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

    uint32_t *pTex = (uint32_t *)hle_alloc(4);
    uint32_t a_ctex[9] = { dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 };
    hr = call("IDirect3DDevice9::CreateTexture", 9, a_ctex);
    ok("CreateTexture(2x2)", hr == D3D_OK && *pTex != 0);
    uint32_t tex = *pTex;

    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*lr));
    uint32_t a_lock[5] = { tex, 0, A(lr), 0, 0 };
    hr = call("IDirect3DTexture9::LockRect", 5, a_lock);
    ok("Texture::LockRect", hr == D3D_OK && lr->pBits != 0);
    uint32_t *texels = (uint32_t *)hle_ptr(lr->pBits);
    texels[0] = 0xFFFF0000u;    /* top-left: red */
    texels[1] = 0xFF00FF00u;    /* top-right: green */
    texels[2] = 0xFF0000FFu;    /* bottom-left: blue */
    texels[3] = 0xFFFFFF00u;    /* bottom-right: yellow */
    hr = call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ tex, 0 });
    ok("Texture::UnlockRect uploads to Metal", hr == D3D_OK);

    uint32_t a_stex[3] = { dev, 0, tex };
    hr = call("IDirect3DDevice9::SetTexture", 3, a_stex);
    ok("SetTexture(0)", hr == D3D_OK);

    const uint32_t vb_size = sizeof(Vertex) * 4;
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    uint32_t a_cvb[7] = { dev, vb_size, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 };
    hr = call("IDirect3DDevice9::CreateVertexBuffer", 7, a_cvb);
    ok("CreateVertexBuffer", hr == D3D_OK && *pVB != 0);
    uint32_t vb = *pVB;

    uint32_t *pData = (uint32_t *)hle_alloc(4);
    uint32_t a_lockvb[5] = { vb, 0, vb_size, A(pData), 0 };
    hr = call("IDirect3DVertexBuffer9::Lock", 5, a_lockvb);
    ok("VertexBuffer::Lock", hr == D3D_OK);
    Vertex *verts = (Vertex *)hle_ptr(*pData);
    verts[0] = quad_vertex(-1.0f,  1.0f, 0.0f, 0.0f);
    verts[1] = quad_vertex( 1.0f,  1.0f, 1.0f, 0.0f);
    verts[2] = quad_vertex(-1.0f, -1.0f, 0.0f, 1.0f);
    verts[3] = quad_vertex( 1.0f, -1.0f, 1.0f, 1.0f);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ vb });

    uint32_t a_stream[5] = { dev, 0, vb, 0, sizeof(Vertex) };
    call("IDirect3DDevice9::SetStreamSource", 5, a_stream);
    uint32_t a_fvf[2] = { dev, 0 };
    call("IDirect3DDevice9::SetFVF", 2, a_fvf);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    uint32_t a_draw[4] = { dev, D3DPT_TRIANGLESTRIP, 0, 2 };
    hr = call("IDirect3DDevice9::DrawPrimitive", 4, a_draw);
    ok("DrawPrimitive(textured quad)", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[W * H];
    ok("Metal readback", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("top-left quadrant == red",   pixels[16 * W + 16] == 0xFFFF0000u);
    ok("top-right quadrant == green",pixels[16 * W + 48] == 0xFF00FF00u);
    ok("bottom-left quadrant == blue",pixels[48 * W + 16] == 0xFF0000FFu);
    ok("bottom-right quadrant == yellow", pixels[48 * W + 48] == 0xFFFFFF00u);
    printf("    sampled: 0x%08X 0x%08X / 0x%08X 0x%08X\n",
           pixels[16 * W + 16], pixels[16 * W + 48],
           pixels[48 * W + 16], pixels[48 * W + 48]);

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ vb });
    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ tex });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_texture: OK\n" : "test_d3d9_texture: FAILED\n");
    return fails != 0;
}
