/*
 * test_d3d9_processvertices_z.c - ProcessVertices POSITIONT -> POSITION depth.
 *
 * Public D3D9: POSITIONT/XYZRHW carries viewport depth in [MinZ, MaxZ] and
 * rhw = 1/w. When ProcessVertices writes a POSITION element from a POSITIONT
 * source, the clip-space z must be z_viewport * w (D3D9 clips z in [0, 1]).
 * The old OpenGL-style 2*z-1 remap made z < 0.5 negative and therefore
 * clipped away by the Metal backend. Six source vertices at z = 0.25 must
 * still produce a visible quad. All expectations are fixed by hand; no game
 * data, no reverse engineering.
 */
#include "test_d3d9_common.h"

#define FVF_XYZW   0x004u | 0x040u
#define FVF_XYZC   0x002u | 0x040u
#define STRIDE_SRC 20u
#define STRIDE_DST 16u

typedef struct { float x, y, z, rhw; uint32_t c; } Src;
typedef struct { float x, y, z; uint32_t c; } Dst;

static uint32_t make_vb_fvf(uint32_t dev, uint32_t fvf, uint32_t bytes) {
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    ok("CreateVertexBuffer", call("IDirect3DDevice9::CreateVertexBuffer", 7,
        (uint32_t[]){ dev, bytes, 0, fvf, D3DPOOL_MANAGED, A(pVB), 0 }) == D3D_OK && *pVB);
    return *pVB;
}

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;
    uint32_t W = 32, H = 32;
    D3DVIEWPORT9 vp;
    memset(&vp, 0, sizeof vp);
    vp.Width = W; vp.Height = H; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    ok("SetViewport", call("IDirect3DDevice9::SetViewport", 2,
        (uint32_t[]){ dev, hle_va(G_copy(&vp, sizeof vp)) }) == D3D_OK);

    uint32_t src = make_vb_fvf(dev, FVF_XYZW, sizeof(Src) * 6);
    uint32_t dst = make_vb_fvf(dev, FVF_XYZC, sizeof(Dst) * 6);
    if (!src || !dst) return 1;

    Src s[6];
    for (int i = 0; i < 6; i++) { s[i].x = 0; s[i].y = 0; s[i].z = 0.25f; s[i].rhw = 1.0f; s[i].c = 0xFF00FF00u; }
    s[0].x = 0;  s[0].y = 0;
    s[1].x = 32; s[1].y = 0;
    s[2].x = 32; s[2].y = 32;
    s[3].x = 0;  s[3].y = 0;
    s[4].x = 32; s[4].y = 32;
    s[5].x = 0;  s[5].y = 32;
    uint32_t *p = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ src, 0, sizeof s, A(p), 0 });
    memcpy(hle_ptr(*p), s, sizeof s);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ src });

    ok("SetFVF(XYZRHW|DIFFUSE)", call("IDirect3DDevice9::SetFVF", 2,
        (uint32_t[]){ dev, FVF_XYZW }) == D3D_OK);
    ok("SetStreamSource(src)", call("IDirect3DDevice9::SetStreamSource", 5,
        (uint32_t[]){ dev, 0, src, 0, STRIDE_SRC }) == D3D_OK);
    uint32_t hr = call("IDirect3DDevice9::ProcessVertices", 7,
        (uint32_t[]){ dev, 0, 0, 6, dst, 0, 0 });
    ok("ProcessVertices", hr == D3D_OK);

    /* Draw the processed output as regular XYZ|DIFFUSE vertices. */
    ok("SetFVF(XYZ|DIFFUSE)", call("IDirect3DDevice9::SetFVF", 2,
        (uint32_t[]){ dev, FVF_XYZC }) == D3D_OK);
    ok("SetStreamSource(dst)", call("IDirect3DDevice9::SetStreamSource", 5,
        (uint32_t[]){ dev, 0, dst, 0, STRIDE_DST }) == D3D_OK);

    clear(dev, 0xFF000000u, D3DCLEAR_TARGET, 1.0f);
    draw_triangle(dev, dst);
    uint32_t px[32 * 32];
    hle_d3d9_metal_read_pixels(0, 0, W, H, px, W * 4);
    ok("z=0.25 quad visible after ProcessVertices (0x00FF00)",
       px[16 * 32 + 16] == 0xFF00FF00u);

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ src });
    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ dst });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    hle_d3d9_metal_shutdown_device();
    printf(fails ? "test_d3d9_processvertices_z: FAILED\n" : "test_d3d9_processvertices_z: OK\n");
    return fails != 0;
}
