/* User clip planes on real Metal. A clip plane (a,b,c,d) keeps fragments for
 * which a*x+b*y+c*z+d*w >= 0 (public IDirect3DDevice9::SetClipPlane docs).
 * Enabling one plane must actually discard the other half of a full-screen
 * triangle; without the backend path both halves stay visible and the test
 * is red. Independent expected colours: white = visible, black = clipped. */
#include "test_d3d9_common.h"

#define W 32
#define H 32
#define D3DRS_CLIPPLANEENABLE 152u

static uint32_t make_white_triangle(uint32_t dev) {
    /* NDC (-2,-2), (2,-2), (0,3): an isoceles triangle covering both
     * sample points (NDC x = -0.5 and +0.5 at y = 0). */
    Vtx v[3] = {
        vertex(-16.0f, 48.0f, 0.5f, 1, 1, 1, 1),
        vertex( 48.0f, 48.0f, 0.5f, 1, 1, 1, 1),
        vertex( 16.0f,-32.0f, 0.5f, 1, 1, 1, 1)
    };
    return make_vb(dev, v, 3);
}

static void set_clip(uint32_t dev, const float plane[4]) {
    void *g = G_copy(plane, sizeof(float) * 4);
    ok("SetClipPlane == D3D_OK",
       call("IDirect3DDevice9::SetClipPlane", 3, (uint32_t[]){ dev, 0, A(g) }) == D3D_OK);
}

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;
    uint32_t vb = make_white_triangle(dev);
    if (!vb) return 1;

    uint32_t pixels[W * H];
    const uint32_t WHITE = 0xFFFFFFFFu, BLACK = 0xFF000000u;

    /* Plane x >= 0: right half (px=24) survives, left half (px=8) clipped. */
    const float keep_right[4] = { 1.0f, 0, 0, 0 };
    set_clip(dev, keep_right);
    ok("SetRenderState(CLIPPLANEENABLE,1) == D3D_OK",
       call("IDirect3DDevice9::SetRenderState", 3,
            (uint32_t[]){ dev, D3DRS_CLIPPLANEENABLE, 1 }) == D3D_OK);
    clear(dev, BLACK, 1, 1.0f);
    draw_triangle(dev, vb);
    ok("Metal readback right", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("clip x>=0: right pixel white", pixels[16 * W + 24] == WHITE);
    ok("clip x>=0: left pixel black",  pixels[16 * W + 8]  == BLACK);
    if (pixels[16 * W + 24] != WHITE || pixels[16 * W + 8] != BLACK)
        printf("    got right=0x%08X left=0x%08X\n", pixels[16 * W + 24], pixels[16 * W + 8]);

    /* Flip the plane to x <= 0: the halves swap. */
    const float keep_left[4] = { -1.0f, 0, 0, 0 };
    set_clip(dev, keep_left);
    clear(dev, BLACK, 1, 1.0f);
    draw_triangle(dev, vb);
    ok("Metal readback left", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("clip x<=0: left pixel white",  pixels[16 * W + 8]  == WHITE);
    ok("clip x<=0: right pixel black", pixels[16 * W + 24] == BLACK);
    if (pixels[16 * W + 8] != WHITE || pixels[16 * W + 24] != BLACK)
        printf("    got left=0x%08X right=0x%08X\n", pixels[16 * W + 8], pixels[16 * W + 24]);

    /* Disable clipping: the whole triangle must be visible again. */
    ok("SetRenderState(CLIPPLANEENABLE,0) == D3D_OK",
       call("IDirect3DDevice9::SetRenderState", 3,
            (uint32_t[]){ dev, D3DRS_CLIPPLANEENABLE, 0 }) == D3D_OK);
    clear(dev, BLACK, 1, 1.0f);
    draw_triangle(dev, vb);
    ok("Metal readback disabled", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("clip disabled: left pixel white",  pixels[16 * W + 8]  == WHITE);
    ok("clip disabled: right pixel white", pixels[16 * W + 24] == WHITE);

    printf(fails == 0 ? "test_d3d9_clip_planes: OK\n" : "test_d3d9_clip_planes: FAILED\n");
    return fails != 0;
}
