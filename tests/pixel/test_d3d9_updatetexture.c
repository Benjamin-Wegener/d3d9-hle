/*
 * test_d3d9_updatetexture.c - Real Metal pixel test for IDirect3DDevice9::UpdateTexture.
 *
 * 1) Creates a SYSTEMMEM texture and locks it to write pixel colours (quadrant pattern).
 * 2) Creates a DEFAULT texture (which has a Metal GPU texture backing).
 * 3) Calls IDirect3DDevice9::UpdateTexture(sysmem_tex, default_tex) to copy pixels
 *    and upload them to the Metal GPU texture.
 * 4) Binds default_tex with SetTexture(0) and draws a textured quad onto the backbuffer.
 * 5) Reads back the Metal render target to verify that the sampled pixels
 *    match the updated texture colours.
 * All drawing and uploading are executed by the real Metal backend.
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
    printf("=== Test: D3D9 UpdateTexture with Metal sampling and readback ===\n");

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

    /* Create Source Texture in SYSTEMMEM */
    uint32_t *pSrcTex = (uint32_t *)hle_alloc(4);
    uint32_t a_csrc[9] = { dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, A(pSrcTex), 0 };
    hr = call("IDirect3DDevice9::CreateTexture", 9, a_csrc);
    ok("CreateTexture(Source SYSTEMMEM)", hr == D3D_OK && *pSrcTex != 0);
    uint32_t src_tex = *pSrcTex;

    /* Create Destination Texture in DEFAULT (or MANAGED) pool */
    uint32_t *pDstTex = (uint32_t *)hle_alloc(4);
    uint32_t a_cdst[9] = { dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, A(pDstTex), 0 };
    hr = call("IDirect3DDevice9::CreateTexture", 9, a_cdst);
    ok("CreateTexture(Dest DEFAULT)", hr == D3D_OK && *pDstTex != 0);
    uint32_t dst_tex = *pDstTex;

    /* Fill Source Texture with known colours */
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*lr));
    hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ src_tex, 0, A(lr), 0, 0 });
    ok("Source Texture::LockRect", hr == D3D_OK && lr->pBits != 0);
    uint32_t *texels = (uint32_t *)hle_ptr(lr->pBits);
    texels[0] = 0xFFFF00FFu;    /* top-left: magenta */
    texels[1] = 0xFF00FFFFu;    /* top-right: cyan */
    texels[2] = 0xFFFFFF00u;    /* bottom-left: yellow */
    texels[3] = 0xFFFFFFFFu;    /* bottom-right: white */
    call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ src_tex, 0 });

    /* Call UpdateTexture to copy from sysmem to dest texture and upload to Metal */
    hr = call("IDirect3DDevice9::UpdateTexture", 3, (uint32_t[]){ dev, src_tex, dst_tex });
    ok("UpdateTexture(src, dst)", hr == D3D_OK);

    /* Bind destination texture and draw quad */
    hr = call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, dst_tex });
    ok("SetTexture(dest_tex)", hr == D3D_OK);

    const uint32_t vb_size = sizeof(Vertex) * 4;
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    uint32_t a_cvb[7] = { dev, vb_size, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 };
    hr = call("IDirect3DDevice9::CreateVertexBuffer", 7, a_cvb);
    ok("CreateVertexBuffer", hr == D3D_OK && *pVB != 0);
    uint32_t vb = *pVB;

    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ vb, 0, vb_size, A(pData), 0 });
    Vertex *verts = (Vertex *)hle_ptr(*pData);
    verts[0] = quad_vertex(-1.0f,  1.0f, 0.0f, 0.0f);
    verts[1] = quad_vertex( 1.0f,  1.0f, 1.0f, 0.0f);
    verts[2] = quad_vertex(-1.0f, -1.0f, 0.0f, 1.0f);
    verts[3] = quad_vertex( 1.0f, -1.0f, 1.0f, 1.0f);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ vb });

    call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, vb, 0, sizeof(Vertex) });
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    hr = call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLESTRIP, 0, 2 });
    ok("DrawPrimitive(textured quad)", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    /* Readback Metal backbuffer and check pixels */
    uint32_t pixels[W * H];
    ok("Metal readback", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("top-left quadrant == magenta",     pixels[16 * W + 16] == 0xFFFF00FFu);
    ok("top-right quadrant == cyan",       pixels[16 * W + 48] == 0xFF00FFFFu);
    ok("bottom-left quadrant == yellow",   pixels[48 * W + 16] == 0xFFFFFF00u);
    ok("bottom-right quadrant == white",   pixels[48 * W + 48] == 0xFFFFFFFFu);
    if (pixels[16 * W + 16] != 0xFFFF00FFu) {
        printf("    got 0x%08X (expected 0xFFFF00FF)\n", pixels[16 * W + 16]);
    }

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ vb });
    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ dst_tex });
    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ src_tex });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_updatetexture: OK\n" : "test_d3d9_updatetexture: FAILED\n");
    return fails != 0;
}