/* Depth test: a near green quad is drawn first, then a far red quad over it.
 * With ZENABLE + LESSEQUAL the red quad fails the depth test and green stays;
 * without depth the red quad wins. */
#include "test_d3d9_common.h"

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;
    Vtx near[6] = {
        vertex(0, 0, 0.2f, 0, 1, 0, 1), vertex(32, 0, 0.2f, 0, 1, 0, 1),
        vertex(0, 32, 0.2f, 0, 1, 0, 1),
        vertex(0, 32, 0.2f, 0, 1, 0, 1), vertex(32, 0, 0.2f, 0, 1, 0, 1),
        vertex(32, 32, 0.2f, 0, 1, 0, 1)
    };
    Vtx far[6] = {
        vertex(0, 0, 0.8f, 1, 0, 0, 1), vertex(32, 0, 0.8f, 1, 0, 0, 1),
        vertex(0, 32, 0.8f, 1, 0, 0, 1),
        vertex(0, 32, 0.8f, 1, 0, 0, 1), vertex(32, 0, 0.8f, 1, 0, 0, 1),
        vertex(32, 32, 0.8f, 1, 0, 0, 1)
    };
    uint32_t vb_near = make_vb(dev, near, 6);
    clear(dev, 0xFF000000u, 1 | 2, 1.0f);  /* TARGET | ZBUFFER */

    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 7, 1 });   /* ZENABLE */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 14, 1 });  /* ZWRITEENABLE */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 23, 4 });  /* ZFUNC = LESSEQUAL */

    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    uint32_t vb_far = make_vb(dev, far, 6);   /* rebinds stream 0 */
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    (void)vb_near; (void)vb_far;

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("near green survived far red (depth)", pixels[16 * 32 + 16] == 0xFF00FF00u);
    if (pixels[16 * 32 + 16] != 0xFF00FF00u)
        printf("    got 0x%08X\n", pixels[16 * 32 + 16]);

    printf(fails == 0 ? "test_d3d9_depth: OK\n" : "test_d3d9_depth: FAILED\n");
    return fails != 0;
}
