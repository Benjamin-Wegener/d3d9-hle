/* Test FVF + Transforms (World, View, Projection) on real Metal backend.
 * Model vertices are around origin in [-0.5, 0.5].
 * Transforms translate and project to cover the screen centre.
 * Clears background red, draws green quad.
 * Without correct transform processing, the geometry would not land on screen centre and pixel stays red. */
#include "test_d3d9_common.h"

typedef struct {
    float x, y, z;
    uint32_t diffuse;
} VtxXYZ;

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    VtxXYZ v[4] = {
        { -0.5f, -0.5f, 0.0f, 0xFF00FF00u },
        {  0.5f, -0.5f, 0.0f, 0xFF00FF00u },
        {  0.5f,  0.5f, 0.0f, 0xFF00FF00u },
        { -0.5f,  0.5f, 0.0f, 0xFF00FF00u }
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

    /* Set identity View and Proj, and Scale World matrix */
    D3DMATRIX world, view, proj;
    memset(&world, 0, sizeof(world));
    memset(&view, 0, sizeof(view));
    memset(&proj, 0, sizeof(proj));

    world.m[0][0] = 1.0f; world.m[1][1] = 1.0f; world.m[2][2] = 1.0f; world.m[3][3] = 1.0f;
    view.m[0][0] = 1.0f; view.m[1][1] = 1.0f; view.m[2][2] = 1.0f; view.m[3][3] = 1.0f;
    proj.m[0][0] = 1.0f; proj.m[1][1] = 1.0f; proj.m[2][2] = 1.0f; proj.m[3][3] = 1.0f;

    call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 256 /* D3DTS_WORLD */, A(G_copy(&world, 64)) });
    call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 2 /* D3DTS_VIEW */, A(G_copy(&view, 64)) });
    call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 3 /* D3DTS_PROJECTION */, A(G_copy(&proj, 64)) });

    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(VtxXYZ) });
    call("IDirect3DDevice9::SetFVF", 2,
         (uint32_t[]){ dev, 0x002u /* D3DFVF_XYZ */ | 0x040u /* D3DFVF_DIFFUSE */ });

    clear(dev, 0xFFFF0000u, 1, 1.0f); /* Clear red */
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    /* DrawPrimitive TRIANGLEFAN (2 triangles quad) */
    hr = call("IDirect3DDevice9::DrawPrimitive", 4,
              (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 2 });
    ok("DrawPrimitive(TRIANGLEFAN, XYZ)", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("centre pixel == green", pixels[16 * 32 + 16] == 0xFF00FF00u);
    if (pixels[16 * 32 + 16] != 0xFF00FF00u)
        printf("    got 0x%08X (expected 0xFF00FF00)\n", pixels[16 * 32 + 16]);

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pVB });
    printf(fails == 0 ? "test_d3d9_ffp_transforms: OK\n" : "test_d3d9_ffp_transforms: FAILED\n");
    return fails != 0;
}
