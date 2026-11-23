/* Test FVF + Alpha Test on real Metal backend.
 * Clears background green (0xFF00FF00).
 * Draws full-screen quad with red color (0xFFFF0000) and alpha = 64 (0x40).
 * Sets Alpha Test: func = GREATER, ref = 128 (0x80).
 * Since quad alpha (64) is NOT > 128, all fragments of the red quad are discarded!
 * Green background survives. Without alpha test, pixel would turn red! */
#include "test_d3d9_common.h"

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse;
} VtxAlpha;

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    VtxAlpha v[4] = {
        { -1.0f, -1.0f, 0.5f, 1.0f, 0x40FF0000u }, /* Red with alpha 64 */
        {  1.0f, -1.0f, 0.5f, 1.0f, 0x40FF0000u },
        {  1.0f,  1.0f, 0.5f, 1.0f, 0x40FF0000u },
        { -1.0f,  1.0f, 0.5f, 1.0f, 0x40FF0000u }
    };

    uint32_t vb_size = sizeof(v);
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3DDevice9::CreateVertexBuffer", 7,
                       (uint32_t[]){ dev, vb_size, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });
    ok("CreateVertexBuffer", hr == D3D_OK && *pVB);
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, vb_size, A(pData), 0 });
    memcpy(hle_ptr(*pData), v, vb_size);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });

    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(VtxAlpha) });
    call("IDirect3DDevice9::SetFVF", 2,
         (uint32_t[]){ dev, 0x004u /* D3DFVF_XYZRHW */ | 0x040u /* D3DFVF_DIFFUSE */ });

    /* Enable Alpha Test: GREATER than 128 */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 15 /* D3DRS_ALPHATESTENABLE */, 1 });
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 25 /* D3DRS_ALPHAFUNC */, 5 /* D3DCMP_GREATER */ });
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 24 /* D3DRS_ALPHAREF */, 128 });

    clear(dev, 0xFF00FF00u, 1, 1.0f); /* Clear green */
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    hr = call("IDirect3DDevice9::DrawPrimitive", 4,
              (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 2 });
    ok("DrawPrimitive(TRIANGLEFAN, AlphaTest)", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("alpha-tested discarded pixel leaves green", pixels[16 * 32 + 16] == 0xFF00FF00u);
    if (pixels[16 * 32 + 16] != 0xFF00FF00u)
        printf("    got 0x%08X (expected 0xFF00FF00)\n", pixels[16 * 32 + 16]);

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pVB });
    printf(fails == 0 ? "test_d3d9_ffp_alpha_test: OK\n" : "test_d3d9_ffp_alpha_test: FAILED\n");
    return fails != 0;
}
