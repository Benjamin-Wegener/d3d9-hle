/* Scissor test: a red fullscreen quad drawn with an 8x8 scissor. Inside the
 * rect must be red, outside must stay green; without scissor everything is
 * red. */
#include "test_d3d9_common.h"

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;
    Vtx v[6] = {
        vertex(0, 0, 0.5f, 1, 0, 0, 1), vertex(32, 0, 0.5f, 1, 0, 0, 1),
        vertex(0, 32, 0.5f, 1, 0, 0, 1),
        vertex(0, 32, 0.5f, 1, 0, 0, 1), vertex(32, 0, 0.5f, 1, 0, 0, 1),
        vertex(32, 32, 0.5f, 1, 0, 0, 1)
    };
    make_vb(dev, v, 6);

    int32_t rect[4] = { 8, 8, 16, 16 };
    void *grect = G_copy(rect, sizeof rect);
    call("IDirect3DDevice9::SetScissorRect", 2, (uint32_t[]){ dev, A(grect) });
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 174, 1 }); /* SCISSORTESTENABLE */

    clear(dev, 0xFF00FF00u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("scissor inside == red", pixels[12 * 32 + 12] == 0xFFFF0000u);
    ok("scissor outside == green", pixels[24 * 32 + 24] == 0xFF00FF00u);
    if (pixels[24 * 32 + 24] != 0xFF00FF00u)
        printf("    outside got 0x%08X\n", pixels[24 * 32 + 24]);

    printf(fails == 0 ? "test_d3d9_scissor: OK\n" : "test_d3d9_scissor: FAILED\n");
    return fails != 0;
}
