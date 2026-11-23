/* d3d9_format integration: an L8 texture is locked with raw luminance bytes,
 * converted to BGRA8 in the UnlockRect upload path, then sampled by Metal.
 * Without the conversion the four quadrants would not be grey. */
#include "test_d3d9_common.h"

#define W 64
#define H 64

static Vtx quad(float x, float y, float u, float v) {
    Vtx q = { 0, 0, 0, 1, 1, 1, 1, 1, 0, 0 };
    q.x = x; q.y = y; q.u = u; q.v = v;
    return q;
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) { ok("Metal available", 0); return 1; }
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = W; pp->BackBufferHeight = H;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    call("IDirect3D9::CreateDevice", 7,
         (uint32_t[]){ d3d9, 0, D3DDEVTYPE_HAL, 0,
                       D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev) });
    uint32_t dev = *pDev;
    ok("CreateDevice", dev != 0);

    uint32_t *pTex = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3DDevice9::CreateTexture", 9,
                       (uint32_t[]){ dev, 2, 2, 1, 0, 50 /* D3DFMT_L8 */, 1, A(pTex), 0 });
    ok("CreateTexture(L8)", hr == D3D_OK && *pTex);
    uint32_t tex = *pTex;

    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*lr));
    call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ tex, 0, A(lr), 0, 0 });
    uint8_t *l = (uint8_t *)hle_ptr(lr->pBits);
    l[0] = 0x00; l[1] = 0x40; l[2] = 0x80; l[3] = 0xFF;
    call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ tex, 0 });
    call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex });

    Vtx v[4] = { quad(-1, 1, 0, 0), quad(1, 1, 1, 0),
                 quad(-1, -1, 0, 1), quad(1, -1, 1, 1) };
    uint32_t vb = make_vb(dev, v, 4);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLESTRIP, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[W * H];
    ok("Metal readback", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("L8 0x00 -> black",   pixels[16 * W + 16] == 0xFF000000u);
    ok("L8 0x40 -> 0x404040", pixels[16 * W + 48] == 0xFF404040u);
    ok("L8 0x80 -> 0x808080", pixels[48 * W + 16] == 0xFF808080u);
    ok("L8 0xFF -> white",   pixels[48 * W + 48] == 0xFFFFFFFFu);
    printf("    sampled: 0x%08X 0x%08X / 0x%08X 0x%08X\n",
           pixels[16 * W + 16], pixels[16 * W + 48],
           pixels[48 * W + 16], pixels[48 * W + 48]);
    (void)vb;

    printf(fails == 0 ? "test_d3d9_format: OK\n" : "test_d3d9_format: FAILED\n");
    return fails != 0;
}
