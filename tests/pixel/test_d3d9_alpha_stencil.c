/* Alpha test / stencil / depth bias / blend op / separate alpha blend /
 * color write masks: public-doc validation (invalid enum -> D3DERR_INVALIDCALL),
 * honest draw behavior (the Metal backend cannot apply the combination ->
 * D3DERR_NOTAVAILABLE) and a pixel check. Alpha test is supported by the
 * HLE's fixed-function pipeline. */
#include "test_d3d9_common.h"

#define W 32
#define H 32

static uint32_t get_rs(uint32_t dev, uint32_t rs) {
    uint32_t *v = (uint32_t *)hle_alloc(4);   /* guest-visible buffer (based memory) */
    call("IDirect3DDevice9::GetRenderState", 3, (uint32_t[]){ dev, rs, A(v) });
    return *v;
}
static int set_rs(uint32_t dev, uint32_t rs, uint32_t val) {
    return call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, rs, val }) == D3D_OK;
}
static uint32_t draw(uint32_t dev) {
    return call("IDirect3DDevice9::DrawPrimitive", 4,
                (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 1 });
}
static int all_px(const uint32_t *p, uint32_t expected) {
    for (int i = 0; i < W * H; i++) {
        if (p[i] != expected) {
            printf("    first mismatch at %d: 0x%08X, expected 0x%08X\n",
                   i, p[i], expected);
            return 0;
        }
    }
    return 1;
}

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    uint32_t rt[W * H];

    /* Boundary: invalid enum values -> D3DERR_INVALIDCALL per public docs. */
    ok("SetRenderState(SRCBLEND,99) == D3DERR_INVALIDCALL",
       call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, D3DRS_SRCBLEND, 99 }) == D3DERR_INVALIDCALL);
    ok("SetRenderState(DESTBLEND,99) == D3DERR_INVALIDCALL",
       call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, D3DRS_DESTBLEND, 99 }) == D3DERR_INVALIDCALL);
    ok("SetRenderState(ZFUNC,0) == D3DERR_INVALIDCALL",
       call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, D3DRS_ZFUNC, 0 }) == D3DERR_INVALIDCALL);
    ok("SetRenderState(ALPHAFUNC,0) == D3DERR_INVALIDCALL",
       call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, D3DRS_ALPHAFUNC, 0 }) == D3DERR_INVALIDCALL);
    ok("SetRenderState(STENCILPASS,0) == D3DERR_INVALIDCALL",
       call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, D3DRS_STENCILPASS, 0 }) == D3DERR_INVALIDCALL);
    ok("SetRenderState(BLENDOP,42) == D3DERR_INVALIDCALL",
       call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, D3DRS_BLENDOP, 42 }) == D3DERR_INVALIDCALL);
    ok("SetRenderState(COLORWRITEENABLE,0x10) == D3DERR_INVALIDCALL",
       call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, D3DRS_COLORWRITEENABLE, 0x10 }) == D3DERR_INVALIDCALL);

    /* Valid roundtrips of the new states through the public API. */
    ok("ALPHAFUNC roundtrip", set_rs(dev, D3DRS_ALPHAFUNC, D3DCMP_LESS) && get_rs(dev, D3DRS_ALPHAFUNC) == D3DCMP_LESS);
    ok("ALPHAREF roundtrip", set_rs(dev, D3DRS_ALPHAREF, 128) && get_rs(dev, D3DRS_ALPHAREF) == 128);
    ok("STENCILFAIL roundtrip", set_rs(dev, D3DRS_STENCILFAIL, D3DSTENCILOP_REPLACE) && get_rs(dev, D3DRS_STENCILFAIL) == D3DSTENCILOP_REPLACE);
    ok("STENCILPASS roundtrip", set_rs(dev, D3DRS_STENCILPASS, D3DSTENCILOP_DECR) && get_rs(dev, D3DRS_STENCILPASS) == D3DSTENCILOP_DECR);
    ok("STENCILFUNC roundtrip", set_rs(dev, D3DRS_STENCILFUNC, D3DCMP_EQUAL) && get_rs(dev, D3DRS_STENCILFUNC) == D3DCMP_EQUAL);
    ok("STENCILREF roundtrip", set_rs(dev, D3DRS_STENCILREF, 0xAB) && get_rs(dev, D3DRS_STENCILREF) == 0xAB);
    ok("STENCILMASK roundtrip", set_rs(dev, D3DRS_STENCILMASK, 0x3C) && get_rs(dev, D3DRS_STENCILMASK) == 0x3C);
    ok("STENCILWRITEMASK roundtrip", set_rs(dev, D3DRS_STENCILWRITEMASK, 0x7F) && get_rs(dev, D3DRS_STENCILWRITEMASK) == 0x7F);
    ok("BLENDOP roundtrip", set_rs(dev, D3DRS_BLENDOP, D3DBLENDOP_MAX) && get_rs(dev, D3DRS_BLENDOP) == D3DBLENDOP_MAX);
    ok("SEPARATEALPHABLENDENABLE roundtrip", set_rs(dev, D3DRS_SEPARATEALPHABLENDENABLE, 1) && get_rs(dev, D3DRS_SEPARATEALPHABLENDENABLE) == 1);
    ok("SRCBLENDALPHA roundtrip", set_rs(dev, D3DRS_SRCBLENDALPHA, D3DBLEND_INVSRCALPHA) && get_rs(dev, D3DRS_SRCBLENDALPHA) == D3DBLEND_INVSRCALPHA);
    ok("DESTBLENDALPHA roundtrip", set_rs(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_ONE) && get_rs(dev, D3DRS_DESTBLENDALPHA) == D3DBLEND_ONE);
    float fv = 2.5f, sv = -0.5f;
    uint32_t fb, sb;
    memcpy(&fb, &fv, 4); memcpy(&sb, &sv, 4);
    ok("DEPTHBIAS roundtrip (float bits)", set_rs(dev, D3DRS_DEPTHBIAS, fb) && get_rs(dev, D3DRS_DEPTHBIAS) == fb);
    ok("SLOPESCALEDDEPTHBIAS roundtrip (float bits)", set_rs(dev, D3DRS_SLOPESCALEDEPTHBIAS, sb) && get_rs(dev, D3DRS_SLOPESCALEDEPTHBIAS) == sb);
    set_rs(dev, D3DRS_SEPARATEALPHABLENDENABLE, 0);
    set_rs(dev, D3DRS_DEPTHBIAS, 0);
    set_rs(dev, D3DRS_SLOPESCALEDEPTHBIAS, 0);
    set_rs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);

    /* Honest draws: unsupported combinations -> D3DERR_NOTAVAILABLE. */
    ok("SetRenderState(STENCILENABLE,1)", set_rs(dev, D3DRS_STENCILENABLE, 1));
    ok("DrawPrimitive with stencil == D3DERR_NOTAVAILABLE", draw(dev) == D3DERR_NOTAVAILABLE);
    ok("SetRenderState(STENCILENABLE,0)", set_rs(dev, D3DRS_STENCILENABLE, 0));
    ok("SetRenderState(SEPARATEALPHABLENDENABLE,0)", set_rs(dev, D3DRS_SEPARATEALPHABLENDENABLE, 0));
    ok("DrawPrimitive after stencil off == D3D_OK", draw(dev) == D3D_OK);

    ok("SetRenderState(DEPTHBIAS,float)", set_rs(dev, D3DRS_DEPTHBIAS, fb));
    ok("DrawPrimitive with DEPTHBIAS != 0 == D3DERR_NOTAVAILABLE", draw(dev) == D3DERR_NOTAVAILABLE);
    ok("SetRenderState(DEPTHBIAS,0)", set_rs(dev, D3DRS_DEPTHBIAS, 0));
    ok("SetRenderState(SLOPESCALEDDEPTHBIAS,float)", set_rs(dev, D3DRS_SLOPESCALEDEPTHBIAS, sb));
    ok("DrawPrimitive with SLOPESCALEDDEPTHBIAS != 0 == D3DERR_NOTAVAILABLE", draw(dev) == D3DERR_NOTAVAILABLE);
    ok("SetRenderState(SLOPESCALEDDEPTHBIAS,0)", set_rs(dev, D3DRS_SLOPESCALEDEPTHBIAS, 0));

    ok("SetRenderState(COLORWRITEENABLE,0)", set_rs(dev, D3DRS_COLORWRITEENABLE, 0));
    ok("DrawPrimitive with COLORWRITEENABLE == 0 == D3DERR_NOTAVAILABLE", draw(dev) == D3DERR_NOTAVAILABLE);
    ok("SetRenderState(COLORWRITEENABLE,0xF)", set_rs(dev, D3DRS_COLORWRITEENABLE, 0xF));
    ok("SetRenderState(COLORWRITEENABLE2,0x4)", set_rs(dev, D3DRS_COLORWRITEENABLE2, 0x4));
    ok("DrawPrimitive with COLORWRITEENABLE2 == 0x4 == D3DERR_NOTAVAILABLE", draw(dev) == D3DERR_NOTAVAILABLE);
    ok("SetRenderState(COLORWRITEENABLE2,0xF)", set_rs(dev, D3DRS_COLORWRITEENABLE2, 0xF));
    ok("SetRenderState(BLENDOP,SUBTRACT)", set_rs(dev, D3DRS_BLENDOP, D3DBLENDOP_SUBTRACT));
    ok("DrawPrimitive with BLENDOP != ADD == D3DERR_NOTAVAILABLE", draw(dev) == D3DERR_NOTAVAILABLE);
    ok("SetRenderState(BLENDOP,ADD)", set_rs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD));

    ok("SetRenderState(SEPARATEALPHABLENDENABLE,1)", set_rs(dev, D3DRS_SEPARATEALPHABLENDENABLE, 1));
    ok("SetRenderState(SRCBLENDALPHA,INVSRCALPHA)", set_rs(dev, D3DRS_SRCBLENDALPHA, D3DBLEND_INVSRCALPHA));
    ok("DrawPrimitive with separate alpha != rgb == D3DERR_NOTAVAILABLE", draw(dev) == D3DERR_NOTAVAILABLE);
    ok("SetRenderState(SRCBLENDALPHA,SRCALPHA)", set_rs(dev, D3DRS_SRCBLENDALPHA, D3DBLEND_SRCALPHA));
    ok("SetRenderState(DESTBLENDALPHA,INVSRCALPHA)", set_rs(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA));
    ok("DrawPrimitive with separate alpha == rgb == D3D_OK", draw(dev) == D3D_OK);
    ok("SetRenderState(SEPARATEALPHABLENDENABLE,0)", set_rs(dev, D3DRS_SEPARATEALPHABLENDENABLE, 0));

    /* Alpha test is supported by the HLE's fixed-function pipeline. */
    ok("SetRenderState(ALPHATESTENABLE,1)", set_rs(dev, D3DRS_ALPHATESTENABLE, 1));
    ok("SetRenderState(ALPHAFUNC,LESS)", set_rs(dev, D3DRS_ALPHAFUNC, D3DCMP_LESS));
    ok("SetRenderState(ALPHAREF,128)", set_rs(dev, D3DRS_ALPHAREF, 128));
    ok("DrawPrimitive with alpha test == D3D_OK", draw(dev) == D3D_OK);
    ok("SetRenderState(ALPHATESTENABLE,0)", set_rs(dev, D3DRS_ALPHATESTENABLE, 0));

    /* Pixel check: Clear lands on the Metal target. */
    clear(dev, 0xFF123456u, D3DCLEAR_TARGET, 1.0f);
    ok("Metal RT readback == 0xFF123456",
       hle_d3d9_metal_read_pixels(0, 0, W, H, rt, W * 4) == 0);
    ok("all pixels == 0xFF123456", all_px(rt, 0xFF123456u));

    printf(fails == 0 ? "test_d3d9_alpha_stencil: OK\n" : "test_d3d9_alpha_stencil: FAILED\n");
    return fails != 0;
}
