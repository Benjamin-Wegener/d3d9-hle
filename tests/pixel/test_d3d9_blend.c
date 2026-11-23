/* Alpha blending: a red quad with alpha 0 over green. With SRCALPHA /
 * INVSRCALPHA blending the pixel stays green; without blending it turns red. */
#include "test_d3d9_common.h"

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;
    Vtx v[6] = {
        vertex(0, 0, 0.5f, 1, 0, 0, 0), vertex(32, 0, 0.5f, 1, 0, 0, 0),
        vertex(0, 32, 0.5f, 1, 0, 0, 0),
        vertex(0, 32, 0.5f, 1, 0, 0, 0), vertex(32, 0, 0.5f, 1, 0, 0, 0),
        vertex(32, 32, 0.5f, 1, 0, 0, 0)
    };
    make_vb(dev, v, 6);

    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 27, 1 });  /* ALPHABLENDENABLE */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 19, 5 });  /* SRCBLEND = SRCALPHA */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 20, 6 });  /* DESTBLEND = INVSRCALPHA */

    clear(dev, 0xFF00FF00u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("alpha-blended pixel == green", pixels[16 * 32 + 16] == 0xFF00FF00u);
    if (pixels[16 * 32 + 16] != 0xFF00FF00u)
        printf("    got 0x%08X\n", pixels[16 * 32 + 16]);

    printf(fails == 0 ? "test_d3d9_blend: OK\n" : "test_d3d9_blend: FAILED\n");
    return fails != 0;
}
