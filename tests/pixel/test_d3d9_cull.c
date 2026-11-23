/* Culling: set CULLMODE so the test triangle's winding is culled. The green
 * clear stays; without culling the red triangle would cover the centre. */
#include "test_d3d9_common.h"

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;
    Vtx v[3] = { vertex(16, 5, 0.5f, 1, 0, 0, 1),
                 vertex(26, 27, 0.5f, 1, 0, 0, 1),
                 vertex(6, 27, 0.5f, 1, 0, 0, 1) };
    make_vb(dev, v, 3);

    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 22, 1 });  /* D3DCULL_CW */
    clear(dev, 0xFF00FF00u, 1, 1.0f);
    draw_triangle(dev, 0);

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("culled triangle leaves green", pixels[20 * 32 + 16] == 0xFF00FF00u);
    if (pixels[20 * 32 + 16] != 0xFF00FF00u)
        printf("    got 0x%08X\n", pixels[20 * 32 + 16]);

    printf(fails == 0 ? "test_d3d9_cull: OK\n" : "test_d3d9_cull: FAILED\n");
    return fails != 0;
}
