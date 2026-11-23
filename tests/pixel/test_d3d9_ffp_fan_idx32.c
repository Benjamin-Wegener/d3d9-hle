/* Test FVF + TRIANGLEFAN + 32-bit index buffer on real Metal backend.
 * Clears background red, draws green quad using TRIANGLEFAN with 32-bit indices.
 * Without feature, background remains red. */
#include "test_d3d9_common.h"

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse;
} VtxFVF;

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    /* A quad covering pixels 8..24 of the 32x32 target using TRIANGLEFAN:
     * v0 (center/start), v1, v2, v3 */
    VtxFVF v[4] = {
        { 8.0f,  8.0f, 0.5f, 1.0f, 0xFF00FF00u },
        { 24.0f, 8.0f, 0.5f, 1.0f, 0xFF00FF00u },
        { 24.0f, 24.0f, 0.5f, 1.0f, 0xFF00FF00u },
        { 8.0f,  24.0f, 0.5f, 1.0f, 0xFF00FF00u }
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

    /* Create 32-bit Index Buffer */
    uint32_t *pIB = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::CreateIndexBuffer", 7,
              (uint32_t[]){ dev, 4 * sizeof(uint32_t), 0, 102 /* D3DFMT_INDEX32 */, 1, A(pIB), 0 });
    ok("CreateIndexBuffer(INDEX32)", hr == D3D_OK && *pIB);
    uint32_t *pIdx = (uint32_t *)hle_alloc(4);
    call("IDirect3DIndexBuffer9::Lock", 5, (uint32_t[]){ *pIB, 0, 4 * sizeof(uint32_t), A(pIdx), 0 });
    uint32_t indices[4] = { 0, 1, 2, 3 };
    memcpy(hle_ptr(*pIdx), indices, sizeof(indices));
    call("IDirect3DIndexBuffer9::Unlock", 1, (uint32_t[]){ *pIB });

    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(VtxFVF) });
    call("IDirect3DDevice9::SetIndices", 2, (uint32_t[]){ dev, *pIB });
    call("IDirect3DDevice9::SetFVF", 2,
         (uint32_t[]){ dev, 0x004u /* D3DFVF_XYZRHW */ | 0x040u /* D3DFVF_DIFFUSE */ });

    clear(dev, 0xFFFF0000u, 1, 1.0f); /* Clear red */
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    /* DrawIndexedPrimitive with D3DPT_TRIANGLEFAN: 2 primitives (quad) */
    hr = call("IDirect3DDevice9::DrawIndexedPrimitive", 7,
              (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 0, 4, 0, 2 });
    ok("DrawIndexedPrimitive(TRIANGLEFAN, INDEX32)", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("inside fan pixel == green", pixels[16 * 32 + 16] == 0xFF00FF00u);
    if (pixels[16 * 32 + 16] != 0xFF00FF00u)
        printf("    got 0x%08X (expected 0xFF00FF00)\n", pixels[16 * 32 + 16]);

    ok("outside fan pixel == red", pixels[2 * 32 + 2] == 0xFFFF0000u);

    call("IDirect3DIndexBuffer9::Release", 1, (uint32_t[]){ *pIB });
    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pVB });
    printf(fails == 0 ? "test_d3d9_ffp_fan_idx32: OK\n" : "test_d3d9_ffp_fan_idx32: FAILED\n");
    return fails != 0;
}
