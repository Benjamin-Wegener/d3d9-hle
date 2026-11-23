/* Test FVF + Fog on real Metal backend.
 * Clears background red (0xFFFF0000).
 * Fog is enabled with fog color green (0xFF00FF00).
 * Draws quad with XYZ (depth = 1.0f).
 * Fog start = 0.0f, fog end = 1.0f. At depth 1.0f, fogFactor = 0.0f -> pure fogColor (green).
 * Diffuse is blue (0xFF0000FF).
 * With fog active, output is green. Without fog, output would be blue or red. */
#include "test_d3d9_common.h"

typedef struct {
    float x, y, z;
    uint32_t diffuse;
} VtxFog;

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    VtxFog v[4] = {
        { -1.0f, -1.0f, 1.0f, 0xFF0000FFu },
        {  1.0f, -1.0f, 1.0f, 0xFF0000FFu },
        {  1.0f,  1.0f, 1.0f, 0xFF0000FFu },
        { -1.0f,  1.0f, 1.0f, 0xFF0000FFu }
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
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(VtxFog) });
    call("IDirect3DDevice9::SetFVF", 2,
         (uint32_t[]){ dev, 0x002u /* D3DFVF_XYZ */ | 0x040u /* D3DFVF_DIFFUSE */ });

    /* Enable Fog */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 28 /* D3DRS_FOGENABLE */, 1 });
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 35 /* D3DRS_FOGTABLEMODE */, 3 /* D3DFOG_LINEAR */ });
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 34 /* D3DRS_FOGCOLOR */, 0xFF00FF00u }); /* Green fog */
    float fstart = 0.0f, fend = 1.0f;
    uint32_t ustart, uend;
    memcpy(&ustart, &fstart, 4);
    memcpy(&uend, &fend, 4);
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 36 /* D3DRS_FOGSTART */, ustart });
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 37 /* D3DRS_FOGEND */, uend });

    clear(dev, 0xFFFF0000u, 1, 1.0f); /* Clear red */
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    hr = call("IDirect3DDevice9::DrawPrimitive", 4,
              (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 2 });
    ok("DrawPrimitive(TRIANGLEFAN, Fog)", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("fogged pixel == green", pixels[16 * 32 + 16] == 0xFF00FF00u);
    if (pixels[16 * 32 + 16] != 0xFF00FF00u)
        printf("    got 0x%08X (expected 0xFF00FF00)\n", pixels[16 * 32 + 16]);

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pVB });
    printf(fails == 0 ? "test_d3d9_ffp_fog: OK\n" : "test_d3d9_ffp_fog: FAILED\n");
    return fails != 0;
}
