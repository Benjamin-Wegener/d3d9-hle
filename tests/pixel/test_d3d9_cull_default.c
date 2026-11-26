/*
 * test_d3d9_cull_default.c - public D3D9 default D3DRS_CULLMODE = D3DCULL_CCW.
 *
 * A counter-clockwise triangle (in the backend's y-up NDC) must be culled when
 * the application never sets the render state, both after CreateDevice and
 * after Reset (which restores all render states to their defaults). Setting
 * D3DCULL_NONE keeps it; setting D3DCULL_CCW explicitly culls it. All
 * expectations are read back from real Metal pixels and are fixed by hand from
 * the public D3D9 documentation. No game data, no reverse engineering.
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

static int fails;
static void ok(const char *what, int cond) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) fails++;
}
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
#define A(p) hle_va(p)
#define W 64
#define H 64

typedef struct {
    float x, y, z, w, r, g, b, a, u, v;
} Vertex;

static Vertex vert(float px, float py) {
    Vertex v;
    v.x = px / (float)W * 2.0f - 1.0f;
    v.y = 1.0f - py / (float)H * 2.0f;
    v.z = 0.5f; v.w = 1.0f;
    v.r = 0.0f; v.g = 1.0f; v.b = 0.0f; v.a = 1.0f;
    v.u = 0.0f; v.v = 0.0f;
    return v;
}

static void draw(uint32_t dev) {
    float z = 1.0f; uint32_t zr; memcpy(&zr, &z, 4);
    call("IDirect3DDevice9::Clear", 7,
         (uint32_t[]){ dev, 0, 0, D3DCLEAR_TARGET, 0xFF000000u, zr, 0 });
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 1 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
}
static uint32_t center_pixel(void) {
    uint32_t p[W * H];
    hle_d3d9_metal_read_pixels(0, 0, W, H, p, W * 4);
    return p[32 * W + 32];
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0 || !hle_d3d9_metal_is_active()) {
        printf("  [FAIL] Metal backend available\n");
        return 1;
    }
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = W; pp->BackBufferHeight = H;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    ok("CreateDevice", call("IDirect3D9::CreateDevice", 7,
        (uint32_t[]){ d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, 0,
                      D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev) }) == D3D_OK && *pDev);
    uint32_t dev = *pDev;
    if (!dev) return 1;

    uint32_t bytes = (uint32_t)(sizeof(Vertex) * 3);
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    ok("CreateVertexBuffer", call("IDirect3DDevice9::CreateVertexBuffer", 7,
        (uint32_t[]){ dev, bytes, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 }) == D3D_OK && *pVB);
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, bytes, A(pData), 0 });
    Vertex *v = (Vertex *)hle_ptr(*pData);
    v[0] = vert(32.0f, 10.0f);  /* apex */
    v[1] = vert(14.0f, 50.0f);  /* bottom left -> counter-clockwise in y-up NDC */
    v[2] = vert(50.0f, 50.0f);  /* bottom right */
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(Vertex) });
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });

    /* Default after CreateDevice: CCW culls the CCW triangle -> black. */
    draw(dev);
    ok("CreateDevice default culls CCW (0x00000000)", center_pixel() == 0xFF000000u);

    /* Explicit NONE keeps it. */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 22, 1 });
    draw(dev);
    ok("D3DCULL_NONE keeps it (0x00FF00)", center_pixel() == 0xFF00FF00u);

    /* Explicit CCW culls it. */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 22, 3 });
    draw(dev);
    ok("D3DCULL_CCW culls it", center_pixel() == 0xFF000000u);

    /* Reset restores the public default even if NONE was set before. */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 22, 1 });
    ok("Reset", call("IDirect3DDevice9::Reset", 2, (uint32_t[]){ dev, A(pp) }) == D3D_OK);
    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(Vertex) });
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });
    draw(dev);
    ok("Reset default culls CCW (0x00000000)", center_pixel() == 0xFF000000u);

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pVB });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();
    printf(fails ? "test_d3d9_cull_default: FAILED\n" : "test_d3d9_cull_default: OK\n");
    return fails != 0;
}
