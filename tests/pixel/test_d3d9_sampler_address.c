/*
 * test_d3d9_sampler_address.c - Real Metal pixel test for D3D9 sampler
 * address modes (D3DTADDRESS_WRAP/MIRROR/CLAMP/BORDER/MIRRORONCE) with
 * texture coordinates outside [0,1] and BORDER mit Border-Farbe.
 *
 * Eine 2x2-Textur (R/G/B/Y) wird hochgeladen, ein Quad deckt den Bildschirm
 * ab, alle Spitzen haben dieselbe (ausserhalb [0,1] liegende) UV. Mit
 * POINT-Filter wird exakt ein Texel abgeprobt. Die Mitte des Quads wird
 * gegen handgerechnete Sollwerte geprueft.
 *
 * 2x2-Textur:
 *   (0,0)=R 0xFFFF0000  (1,0)=G 0xFF00FF00
 *   (0,1)=B 0xFF0000FF  (1,1)=Y 0xFFFFFF00
 *
 * u=-0.25, v=0.25 (Zeile 0):
 *   WRAP:     -0.25 -> 0.75 -> col 1 -> G
 *   MIRROR:   -0.25 -> 0.25 -> col 0 -> R
 *   CLAMP:    -0.25 -> 0.0  -> col 0 -> R
 *   BORDER:   transparent Schwarz (Metal kann keine arbitrary Custom-
 *             Border-Farbe; sd.borderColor wird gesetzt, ClampToZero
 *             liefert 0x00000000, Dokumentation im Code)
 *   MIRRORONCE: -0.25 -> 0.25 -> col 0 -> R
 *
 * u=1.25, v=0.25 (Zeile 0):
 *   WRAP:     1.25 -> 0.25 -> col 0 -> R
 *   MIRROR:   1.25 -> 0.75 -> col 1 -> G
 *   CLAMP:    1.25 -> 1.0  -> col 1 -> G
 *   BORDER:   transparent Schwarz (siehe oben)
 *   MIRRORONCE: 1.25 -> 0.75 -> col 1 -> G
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

/* Draw a fullscreen quad with constant UV (u,v), POINT filtering, given
 * address mode. Returns the centre pixel. */
static uint32_t draw_sample(uint32_t dev, uint32_t tex, uint32_t addr_mode,
                            uint32_t border_color, float u, float v) {
    /* Set sampler state: MIN/MAG/MIP = POINT, address mode */
    call("IDirect3DDevice9::SetSamplerState", 4,
         (uint32_t[]){ dev, 0, 7, 0 });           /* MIPFILTER NONE */
    call("IDirect3DDevice9::SetSamplerState", 4,
         (uint32_t[]){ dev, 0, 6, 1 });           /* MINFILTER POINT */
    call("IDirect3DDevice9::SetSamplerState", 4,
         (uint32_t[]){ dev, 0, 5, 1 });           /* MAGFILTER POINT */
    call("IDirect3DDevice9::SetSamplerState", 4,
         (uint32_t[]){ dev, 0, 1, addr_mode });   /* ADDRESSU */
    call("IDirect3DDevice9::SetSamplerState", 4,
         (uint32_t[]){ dev, 0, 2, addr_mode });   /* ADDRESSV */
    if (border_color != 0xFFFFFFFFu) {
        call("IDirect3DDevice9::SetSamplerState", 4,
             (uint32_t[]){ dev, 0, 4, border_color }); /* BORDERCOLOR */
    }

    const uint32_t vb_size = sizeof(Vertex) * 4;
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer", 7,
         (uint32_t[]){ dev, vb_size, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });
    uint32_t vb = *pVB;
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5,
         (uint32_t[]){ vb, 0, vb_size, A(pData), 0 });
    Vertex *verts = (Vertex *)hle_ptr(*pData);
    verts[0] = quad_vertex(-1.0f,  1.0f, u, v);
    verts[1] = quad_vertex( 1.0f,  1.0f, u, v);
    verts[2] = quad_vertex(-1.0f, -1.0f, u, v);
    verts[3] = quad_vertex( 1.0f, -1.0f, u, v);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ vb });

    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, vb, 0, sizeof(Vertex) });
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLESTRIP, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[W * H];
    hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4);
    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ vb });
    return pixels[16 * W + 16];
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) {
        printf("  [FAIL] Metal backend available\n");
        return 1;
    }
    ok("Metal backend available", hle_d3d9_metal_is_active());
    printf("=== Test: D3D9 sampler address modes with Metal pixel readback ===\n");

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

    /* 2x2-Textur hochladen */
    uint32_t *pTex = (uint32_t *)hle_alloc(4);
    uint32_t a_ctex[9] = { dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 };
    hr = call("IDirect3DDevice9::CreateTexture", 9, a_ctex);
    ok("CreateTexture(2x2)", hr == D3D_OK && *pTex != 0);
    uint32_t tex = *pTex;

    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*lr));
    call("IDirect3DTexture9::LockRect", 5,
         (uint32_t[]){ tex, 0, A(lr), 0, 0 });
    uint32_t *texels = (uint32_t *)hle_ptr(lr->pBits);
    texels[0] = 0xFFFF0000u;    /* (0,0) red */
    texels[1] = 0xFF00FF00u;    /* (1,0) green */
    texels[2] = 0xFF0000FFu;    /* (0,1) blue */
    texels[3] = 0xFFFFFF00u;    /* (1,1) yellow */
    call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ tex, 0 });

    call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex });

    /* D3DSAMP_ADRESSU/V/W = 1 WRAP, 2 MIRROR, 3 CLAMP, 4 BORDER, 5 MIRRORONCE */
    /* u=-0.25, v=0.25 */
    uint32_t px = draw_sample(dev, tex, 1, 0xFFFFFFFFu, -0.25f, 0.25f);
    ok("WRAP u=-0.25 -> green", px == 0xFF00FF00u);
    if (px != 0xFF00FF00u) printf("    got 0x%08X\n", px);

    px = draw_sample(dev, tex, 2, 0xFFFFFFFFu, -0.25f, 0.25f);
    ok("MIRROR u=-0.25 -> red", px == 0xFFFF0000u);
    if (px != 0xFFFF0000u) printf("    got 0x%08X\n", px);

    px = draw_sample(dev, tex, 3, 0xFFFFFFFFu, -0.25f, 0.25f);
    ok("CLAMP u=-0.25 -> red", px == 0xFFFF0000u);
    if (px != 0xFFFF0000u) printf("    got 0x%08X\n", px);

    px = draw_sample(dev, tex, 4, 0xFFFF00FFu, -0.25f, 0.25f);
    ok("BORDER u=-0.25 -> transparent Schwarz (Metal kein custom border)", px == 0x00000000u);
    if (px != 0x00000000u) printf("    got 0x%08X\n", px);

    px = draw_sample(dev, tex, 5, 0xFFFFFFFFu, -0.25f, 0.25f);
    ok("MIRRORONCE u=-0.25 -> red", px == 0xFFFF0000u);
    if (px != 0xFFFF0000u) printf("    got 0x%08X\n", px);

    /* u=1.25, v=0.25 */
    px = draw_sample(dev, tex, 1, 0xFFFFFFFFu, 1.25f, 0.25f);
    ok("WRAP u=1.25 -> red", px == 0xFFFF0000u);
    if (px != 0xFFFF0000u) printf("    got 0x%08X\n", px);

    px = draw_sample(dev, tex, 2, 0xFFFFFFFFu, 1.25f, 0.25f);
    ok("MIRROR u=1.25 -> green", px == 0xFF00FF00u);
    if (px != 0xFF00FF00u) printf("    got 0x%08X\n", px);

    px = draw_sample(dev, tex, 3, 0xFFFFFFFFu, 1.25f, 0.25f);
    ok("CLAMP u=1.25 -> green", px == 0xFF00FF00u);
    if (px != 0xFF00FF00u) printf("    got 0x%08X\n", px);

    px = draw_sample(dev, tex, 4, 0xFFFF00FFu, 1.25f, 0.25f);
    ok("BORDER u=1.25 -> transparent Schwarz (Metal kein custom border)", px == 0x00000000u);
    if (px != 0x00000000u) printf("    got 0x%08X\n", px);

    px = draw_sample(dev, tex, 5, 0xFFFFFFFFu, 1.25f, 0.25f);
    ok("MIRRORONCE u=1.25 -> green", px == 0xFF00FF00u);
    if (px != 0xFF00FF00u) printf("    got 0x%08X\n", px);

    /* Kleiner Textur-Test: 1x1, WRAP, Koordinaten weit ausserhalb [0,1] */
    uint32_t *pTex1 = (uint32_t *)hle_alloc(4);
    uint32_t a_ctex1[9] = { dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex1), 0 };
    hr = call("IDirect3DDevice9::CreateTexture", 9, a_ctex1);
    ok("CreateTexture(1x1)", hr == D3D_OK && *pTex1 != 0);
    uint32_t tex1 = *pTex1;
    call("IDirect3DTexture9::LockRect", 5,
         (uint32_t[]){ tex1, 0, A(lr), 0, 0 });
    *((uint32_t *)hle_ptr(lr->pBits)) = 0xFFFF0000u; /* red */
    call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ tex1, 0 });

    call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex1 });
    px = draw_sample(dev, tex1, 1, 0xFFFFFFFFu, 2.5f, 2.5f);
    ok("1x1 WRAP u=2.5,v=2.5 -> red (only texel)", px == 0xFFFF0000u);
    if (px != 0xFFFF0000u) printf("    got 0x%08X\n", px);

    /* Zurueck auf 2x2-Textur */
    call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex });

    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ tex1 });
    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ tex });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_sampler_address: OK\n" : "test_d3d9_sampler_address: FAILED\n");
    return fails != 0;
}
