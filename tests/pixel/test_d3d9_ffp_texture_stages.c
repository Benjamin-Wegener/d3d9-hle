/* Test FVF + Texture stages (MODULATE / SELECTARG1) on real Metal backend.
 * Quad vertex diffuse is yellow (0xFFFFFF00).
 * Texture stage 0 is blue (0xFF0000FF).
 * MODULATE produces green (0xFF00FF00) (yellow.rgb * blue.rgb = (1,1,0)*(0,0,1)? Wait: yellow is red+green (1,1,0), blue is (0,0,1) -> modulate = 0 black!).
 * Let's use:
 * diffuse = cyan (0xFF00FFFF = green + blue)
 * texture = yellow (0xFFFFFF00 = red + green)
 * modulate: (0, 1, 1) * (1, 1, 0) = (0, 1, 0) = pure green (0xFF00FF00)!
 * If texture stage was disabled or not modulated, cyan or red would appear instead of green. */
#include "test_d3d9_common.h"

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse;
    float u, v;
} VtxTex;

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    /* Create 2x2 yellow texture */
    uint32_t *pTex = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3DDevice9::CreateTexture", 9,
                       (uint32_t[]){ dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 });
    ok("CreateTexture", hr == D3D_OK && *pTex);

    D3DLOCKED_RECT *plr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*plr));
    call("IDirect3DTexture9::LockRect", 5,
         (uint32_t[]){ *pTex, 0, A(plr), 0, 0 });
    /* Fill with yellow (ARGB = 0xFFFFFF00) */
    uint32_t *tex_pixels = (uint32_t *)hle_ptr(plr->pBits);
    for (int i = 0; i < 4; i++) tex_pixels[i] = 0xFFFFFF00u;
    call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ *pTex, 0 });

    /* Quad covering the full 32x32 target with cyan diffuse */
    VtxTex v[4] = {
        {  0.0f,  0.0f, 0.5f, 1.0f, 0xFF00FFFFu, 0.0f, 0.0f },
        { 32.0f,  0.0f, 0.5f, 1.0f, 0xFF00FFFFu, 1.0f, 0.0f },
        { 32.0f, 32.0f, 0.5f, 1.0f, 0xFF00FFFFu, 1.0f, 1.0f },
        {  0.0f, 32.0f, 0.5f, 1.0f, 0xFF00FFFFu, 0.0f, 1.0f }
    };

    uint32_t vb_size = sizeof(v);
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::CreateVertexBuffer", 7,
              (uint32_t[]){ dev, vb_size, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });
    ok("CreateVertexBuffer", hr == D3D_OK && *pVB);
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, vb_size, A(pData), 0 });
    memcpy(hle_ptr(*pData), v, vb_size);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });

    call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, *pTex });
    /* Set TextureStageState 0 to MODULATE */
    call("IDirect3DDevice9::SetTextureStageState", 4,
         (uint32_t[]){ dev, 0, 1 /* D3DTSS_COLOROP */, 4 /* D3DTOP_MODULATE */ });
    call("IDirect3DDevice9::SetTextureStageState", 4,
         (uint32_t[]){ dev, 0, 2 /* D3DTSS_COLORARG1 */, 2 /* D3DTA_TEXTURE */ });
    call("IDirect3DDevice9::SetTextureStageState", 4,
         (uint32_t[]){ dev, 0, 3 /* D3DTSS_COLORARG2 */, 0 /* D3DTA_DIFFUSE */ });

    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(VtxTex) });
    call("IDirect3DDevice9::SetFVF", 2,
         (uint32_t[]){ dev, 0x004u /* D3DFVF_XYZRHW */ | 0x040u /* D3DFVF_DIFFUSE */ | (1u << 8) });

    clear(dev, 0xFFFF0000u, 1, 1.0f); /* Clear red */
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    hr = call("IDirect3DDevice9::DrawPrimitive", 4,
              (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 2 });
    ok("DrawPrimitive(TRIANGLEFAN, TexStage)", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("modulated pixel == green", pixels[16 * 32 + 16] == 0xFF00FF00u);
    if (pixels[16 * 32 + 16] != 0xFF00FF00u)
        printf("    got 0x%08X (expected 0xFF00FF00)\n", pixels[16 * 32 + 16]);

    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ *pTex });
    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pVB });
    printf(fails == 0 ? "test_d3d9_ffp_texture_stages: OK\n" : "test_d3d9_ffp_texture_stages: FAILED\n");
    return fails != 0;
}
