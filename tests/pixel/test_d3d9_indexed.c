/* Index buffer + DrawIndexedPrimitive: a green triangle drawn only through
 * the index buffer. Without indexed drawing the cleared red remains. */
#include "test_d3d9_common.h"

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;
    Vtx v[3] = { vertex(16, 5, 0.5f, 0, 1, 0, 1),
                 vertex(26, 27, 0.5f, 0, 1, 0, 1),
                 vertex(6, 27, 0.5f, 0, 1, 0, 1) };
    make_vb(dev, v, 3);

    uint32_t *pIB = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3DDevice9::CreateIndexBuffer", 7,
                       (uint32_t[]){ dev, 6, 0, 101 /* D3DFMT_INDEX16 */, 1, A(pIB), 0 });
    ok("CreateIndexBuffer", hr == D3D_OK && *pIB);
    uint32_t *pIdx = (uint32_t *)hle_alloc(4);
    call("IDirect3DIndexBuffer9::Lock", 5, (uint32_t[]){ *pIB, 0, 6, A(pIdx), 0 });
    uint16_t indices[3] = { 0, 1, 2 };
    memcpy(hle_ptr(*pIdx), indices, sizeof indices);
    call("IDirect3DIndexBuffer9::Unlock", 1, (uint32_t[]){ *pIB });
    call("IDirect3DDevice9::SetIndices", 2, (uint32_t[]){ dev, *pIB });

    clear(dev, 0xFFFF0000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    hr = call("IDirect3DDevice9::DrawIndexedPrimitive", 7,
              (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 });
    ok("DrawIndexedPrimitive", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("indexed triangle pixel == green", pixels[20 * 32 + 16] == 0xFF00FF00u);
    if (pixels[20 * 32 + 16] != 0xFF00FF00u)
        printf("    got 0x%08X\n", pixels[20 * 32 + 16]);

    call("IDirect3DIndexBuffer9::Release", 1, (uint32_t[]){ *pIB });
    printf(fails == 0 ? "test_d3d9_indexed: OK\n" : "test_d3d9_indexed: FAILED\n");
    return fails != 0;
}
