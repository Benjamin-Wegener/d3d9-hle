/* Gamma ramp on real Metal. The D3D9 ramp belongs to the swap chain: it is
 * applied by a GPU pass at Present into a separate present texture, never to
 * the render target and never to the system display gamma. Public
 * D3DGAMMARAMP is planar (red[256], green[256], blue[256], 16-bit each).
 * Expected colours are chosen independently of the implementation: a mid-gray
 * (0.5) pixel maps through index 128 to the ramp value at that index. */
#include "test_d3d9_common.h"

#define W 32
#define H 32
#define D3DSGR_CALIBRATE 1u

static uint16_t *make_ramp(uint32_t dev, const char *name, int mode) {
    /* mode 0 = identity, 1 = magenta at 128, 2 = green at 128 */
    uint16_t *r = (uint16_t *)hle_alloc(3 * 256 * sizeof(uint16_t));
    for (unsigned i = 0; i < 768; i++) r[i] = (uint16_t)(i % 256u) * 257u;
    if (mode == 1) {
        r[128] = 65535u; r[256 + 128] = 0u; r[512 + 128] = 65535u;
    } else if (mode == 2) {
        r[128] = 0u; r[256 + 128] = 65535u; r[512 + 128] = 0u;
    }
    ok(name, call("IDirect3DDevice9::SetGammaRamp", 4,
                  (uint32_t[]){ dev, 0, 0, A(r) }) == D3D_OK);
    return r;
}

static void present(uint32_t dev) {
    call("IDirect3DDevice9::Present", 5, (uint32_t[]){ dev, 0, 0, 0, 0 });
}

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    /* A full-screen gray triangle: NDC (-2,-2),(2,-2),(0,3). */
    Vtx v[3] = {
        vertex(-16.0f, 48.0f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f),
        vertex( 48.0f, 48.0f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f),
        vertex( 16.0f,-32.0f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f)
    };
    uint32_t vb = make_vb(dev, v, 3);
    if (!vb) return 1;

    uint32_t rt[W * H], pres[W * H];
    const uint32_t GRAY = 0xFF808080u;

    /* Identity ramp: present output equals the render target. */
    make_ramp(dev, "SetGammaRamp(identity) == D3D_OK", 0);
    clear(dev, 0xFF000000u, 1, 1.0f);
    draw_triangle(dev, vb);
    present(dev);
    ok("Metal RT readback (identity)", hle_d3d9_metal_read_pixels(0, 0, W, H, rt, W * 4) == 0);
    ok("RT pixel is mid-gray", rt[16 * W + 16] == GRAY);
    ok("Metal present readback (identity)",
       hle_d3d9_metal_read_present_pixels(0, 0, W, H, pres, W * 4) == 0);
    ok("identity ramp preserves gray", pres[16 * W + 16] == GRAY);
    if (pres[16 * W + 16] != GRAY) printf("    got 0x%08X\n", pres[16 * W + 16]);

    /* Magenta ramp at index 128: only the present output changes. */
    make_ramp(dev, "SetGammaRamp(magenta@128) == D3D_OK", 1);
    present(dev);
    ok("Metal present readback (magenta)",
       hle_d3d9_metal_read_present_pixels(0, 0, W, H, pres, W * 4) == 0);
    ok("ramp maps gray -> magenta", pres[16 * W + 16] == 0xFFFF00FFu);
    if (pres[16 * W + 16] != 0xFFFF00FFu) printf("    got 0x%08X\n", pres[16 * W + 16]);
    ok("Metal RT unchanged after ramp", hle_d3d9_metal_read_pixels(0, 0, W, H, rt, W * 4) == 0);
    ok("gamma does not modify the render target", rt[16 * W + 16] == GRAY);

    /* Green ramp at index 128 with D3DSGR_CALIBRATE still transfers the ramp. */
    uint16_t *g = make_ramp(dev, "SetGammaRamp(green@128) == D3D_OK", 2);
    ok("SetGammaRamp(CALIBRATE) == D3D_OK",
       call("IDirect3DDevice9::SetGammaRamp", 4,
            (uint32_t[]){ dev, 0, D3DSGR_CALIBRATE, A(g) }) == D3D_OK);
    present(dev);
    ok("Metal present readback (green)",
       hle_d3d9_metal_read_present_pixels(0, 0, W, H, pres, W * 4) == 0);
    ok("ramp maps gray -> green", pres[16 * W + 16] == 0xFF00FF00u);
    if (pres[16 * W + 16] != 0xFF00FF00u) printf("    got 0x%08X\n", pres[16 * W + 16]);

    /* Honest errors for unsupported parameters. */
    uint32_t bad = call("IDirect3DDevice9::SetGammaRamp", 4,
                        (uint32_t[]){ dev, 0, 7, A(g) });
    ok("invalid flags rejected", bad != D3D_OK);
    bad = call("IDirect3DDevice9::SetGammaRamp", 4,
               (uint32_t[]){ dev, 1, 0, A(g) });
    ok("non-zero swap chain rejected", bad != D3D_OK);
    bad = call("IDirect3DDevice9::SetGammaRamp", 4,
               (uint32_t[]){ dev, 0, 0, 0 });
    ok("null ramp rejected", bad == D3DERR_INVALIDCALL);

    printf(fails == 0 ? "test_d3d9_gamma_ramp: OK\n" : "test_d3d9_gamma_ramp: FAILED\n");
    return fails != 0;
}
