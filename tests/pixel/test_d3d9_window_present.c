/*
 * test_d3d9_window_present.c - Present really shows the backbuffer in a
 * CAMetalLayer drawable (framebufferOnly = NO so getBytes can check it).
 *
 * Three vertical bands (red / green / blue) are drawn through the public
 * D3D9 API into the 32x32 backbuffer. After Present the drawable is read back
 * and compared with independently chosen colours. A second Present with
 * source/destination RECTs checks the scaling path of the display blit.
 *
 * No game special case: the window/layer is set through the general host
 * interface hle_d3d9_metal_set_layer(). Without a layer, Present keeps the
 * offscreen-only behaviour (checked at the start of this test).
 */
#include "test_d3d9_common.h"
#include <SDL.h>

#define W 32
#define H 32
#define RED   0xFFFF0000u
#define GREEN 0xFF00FF00u
#define BLUE  0xFF0000FFu
#define BLACK 0xFF000000u

/* Native layout used by the backend: float4 pos, float4 color, float2 uv. */
typedef struct { float x, y, z, w, r, g, b, a, u, v; } NV;

static uint32_t vb_band(uint32_t x0, uint32_t x1, uint32_t color) {
    float l = (float)x0 / (float)W * 2.0f - 1.0f;
    float r = (float)x1 / (float)W * 2.0f - 1.0f;
    float cr = (float)((color >> 16) & 0xFFu) / 255.0f;
    float cg = (float)((color >> 8) & 0xFFu) / 255.0f;
    float cb = (float)(color & 0xFFu) / 255.0f;
    NV v[4];
    for (int i = 0; i < 4; i++) {
        v[i].x = (i & 1) ? r : l;
        v[i].y = (i & 2) ? -1.0f : 1.0f;   /* row 0 is the top of the target */
        v[i].z = 0.5f; v[i].w = 1.0f;
        v[i].r = cr; v[i].g = cg; v[i].b = cb; v[i].a = 1.0f;
        v[i].u = 0.0f; v[i].v = 0.0f;
    }
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer", 7,
         (uint32_t[]){ 0, sizeof v, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, sizeof v, A(pData), 0 });
    memcpy(hle_ptr(*pData), v, sizeof v);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
    return *pVB;
}

static void render(uint32_t dev, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t zr; float z = 1.0f; memcpy(&zr, &z, 4);
    call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, 1, BLACK, zr, 0 });
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    uint32_t bands[3] = { a, b, c };
    uint32_t vb[3];
    vb[0] = vb_band(0, 10, RED);
    vb[1] = vb_band(10, 20, GREEN);
    vb[2] = vb_band(20, W, BLUE);
    for (int i = 0; i < 3; i++) {
        call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, vb[i], 0, sizeof(NV) });
        call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });
        call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, 5 /*strip*/, 0, 2 });
        call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ vb[i] });
    }
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
}

static int same_rgb(uint32_t got, uint32_t want) {
    return (got & 0xFFFFFFu) == (want & 0xFFFFFFu);
}

/* Present-Rechtecke liegen im Gast (A() verlangt eine Gastadresse). */
static uint32_t present_rects(uint32_t dev, const int32_t *src, const int32_t *dst) {
    uint32_t a = 0, b = 0;
    if (src) { int32_t *g = (int32_t *)hle_alloc(16); memcpy(g, src, 16); a = A(g); }
    if (dst) { int32_t *g = (int32_t *)hle_alloc(16); memcpy(g, dst, 16); b = A(g); }
    return call("IDirect3DDevice9::Present", 5, (uint32_t[]){ dev, a, b, 0, 0 });
}

static int read_drawable(uint32_t *px) {
    memset(px, 0, W * H * sizeof(uint32_t));
    return hle_d3d9_metal_read_drawable_pixels(0, 0, W, H, px, W * 4) == 0;
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    ok("Metal backend", hle_d3d9_metal_enable() == 0);

    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    ok("Direct3DCreate9", d3d9 != 0);
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = W; pp->BackBufferHeight = H;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD; pp->Windowed = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4); *pDev = 0;
    ok("CreateDevice", call("IDirect3D9::CreateDevice", 7,
        (uint32_t[]){ d3d9, 0, D3DDEVTYPE_HAL, 0, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                      A(pp), A(pDev) }) == D3D_OK && *pDev);
    uint32_t dev = *pDev;
    uint32_t px[W * H];

    /* Ohne gesetztes Fenster/Layer: Drawable-Lesen muss scheitern, RT-Lesen
     * muss weiterhin funktionieren (altes Offscreen-Verhalten). */
    render(dev, RED, GREEN, BLUE);
    ok("offscreen read still works", hle_d3d9_metal_read_pixels(0, 0, W, H, px, W * 4) == 0);
    ok("offscreen red", same_rgb(px[16 * W + 4], RED));
    ok("offscreen green", same_rgb(px[16 * W + 14], GREEN));
    ok("offscreen blue", same_rgb(px[16 * W + 26], BLUE));
    ok("no drawable without layer", hle_d3d9_metal_read_drawable_pixels(0, 0, 1, 1, px, 4) != 0);
    ok("Present(offscreen)", call("IDirect3DDevice9::Present", 5,
        (uint32_t[]){ dev, 0, 0, 0, 0 }) == D3D_OK);

    /* Auch ein explizit gesetztes SDL-Fenster darf unter dem Dummy-Treiber
     * (Testmodus) keine Anzeige aktivieren. */
    ok("SDL init", SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_Window *win = SDL_CreateWindow("window present test", 0, 0, W, H, SDL_WINDOW_HIDDEN);
    ok("SDL window", win != 0);
    hle_d3d9_metal_set_window(win);
    ok("dummy/hidden window stays offscreen",
       hle_d3d9_metal_read_drawable_pixels(0, 0, 1, 1, px, 4) != 0);
    SDL_DestroyWindow(win);

    /* Host setzt ein CAMetalLayer (framebufferOnly = NO). */
    void *layer = hle_d3d9_metal_create_layer();
    ok("CAMetalLayer erzeugt", layer != 0);
    hle_d3d9_metal_set_layer(layer);
    hle_d3d9_metal_set_present_params(0, 0, 0);   // keine RECTs = ganzes Ziel

    render(dev, RED, GREEN, BLUE);
    ok("Present(windowed)", call("IDirect3DDevice9::Present", 5,
        (uint32_t[]){ dev, 0, 0, 0, 0 }) == D3D_OK);
    ok("drawable readback", read_drawable(px));
    ok("drawable red", same_rgb(px[16 * W + 4], RED));
    ok("drawable green", same_rgb(px[16 * W + 14], GREEN));
    ok("drawable blue", same_rgb(px[16 * W + 26], BLUE));
    if (!same_rgb(px[16 * W + 4], RED) || !same_rgb(px[16 * W + 14], GREEN) ||
        !same_rgb(px[16 * W + 26], BLUE))
        printf("    full: R=%08X G=%08X B=%08X\n",
               px[16 * W + 4], px[16 * W + 14], px[16 * W + 26]);

    /* Skalierung: ganzes Backbuffer in ein 16x16-Ziel im Drawable. */
    int32_t src[4] = { 0, 0, W, H }, dst[4] = { 0, 0, 16, 16 };
    render(dev, RED, GREEN, BLUE);
    ok("Present(scaled)", present_rects(dev, src, dst) == D3D_OK);
    ok("drawable readback scaled", read_drawable(px));
    /* Ziel (8,8) -> Quelle (16,16) = gruen; Ziel (12,12) -> Quelle (24,24) = blau. */
    ok("scaled green at (8,8)", same_rgb(px[8 * W + 8], GREEN));
    ok("scaled blue at (12,12)", same_rgb(px[12 * W + 12], BLUE));
    if (!same_rgb(px[8 * W + 8], GREEN) || !same_rgb(px[12 * W + 12], BLUE))
        printf("    scaled: (8,8)=%08X (12,12)=%08X\n", px[8 * W + 8], px[12 * W + 12]);

    /* Hochskalieren: nur das gruene Band auf das ganze Drawable. */
    int32_t src2[4] = { 10, 0, 20, H };
    render(dev, RED, GREEN, BLUE);
    ok("Present(green band)", present_rects(dev, src2, 0) == D3D_OK);
    ok("drawable readback green band", read_drawable(px));
    ok("green band at left", same_rgb(px[16 * W + 4], GREEN));
    ok("green band at middle", same_rgb(px[16 * W + 16], GREEN));
    ok("green band at right", same_rgb(px[16 * W + 28], GREEN));
    if (!same_rgb(px[16 * W + 4], GREEN) || !same_rgb(px[16 * W + 16], GREEN) ||
        !same_rgb(px[16 * W + 28], GREEN))
        printf("    band: L=%08X M=%08X R=%08X\n",
               px[16 * W + 4], px[16 * W + 16], px[16 * W + 28]);

    hle_d3d9_metal_set_layer(0);
    hle_d3d9_metal_release_layer(layer);
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_window_present: OK\n" : "test_d3d9_window_present: FAILED\n");
    return fails != 0;
}
