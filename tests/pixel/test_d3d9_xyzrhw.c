/* test_d3d9_xyzrhw.c — XYZRHW (D3DFVF_XYZRHW / POSITIONT) tests.
 *
 * Verifies that pre-transformed vertices are treated as screen-pixel
 * coordinates (not clip-space), that World/View/Projection are ignored,
 * that textures with rhw work, and that fog uses the z depth.
 *
 * Public D3D9 docs (D3DFVF_XYZRHW, D3DDECLUSAGE_POSITIONT):
 *   x,y = screen pixels (viewport offset), z = depth, rhw = 1/w.
 *   World/View/Projection are NOT applied.  FFP lighting is off.
 */
#include "test_d3d9_common.h"

/* XYZRHW + DIFFUSE: x, y (pixels), z (depth), rhw (1/w), colour. */
typedef struct { float x, y, z, w; uint32_t c; } XYZW;
#define FVF_XYZW (0x004u | 0x040u)

/* XYZRHW + DIFFUSE + TEX1: adds u, v. */
typedef struct { float x, y, z, w; uint32_t c; float u, v; } XYZW_T;
#define FVF_XYZW_T (0x004u | 0x040u | 0x100u)

/* Render-state / transform constants (public D3D9 values). */
#define RS_ZENABLE       7u
#define RS_FOGENABLE     28u
#define RS_FOGCOLOR      34u
#define RS_FOGTABLEMODE  35u
#define RS_FOGSTART      36u
#define RS_FOGEND        37u
#define RS_FOGVERTEXMODE 140u
#define TS_WORLD         256
#define TS_VIEW          2
#define TS_PROJECTION    3

static uint32_t make_device64(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) { ok("Metal backend available", 0); return 0; }
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    if (!d3d9) { ok("Direct3DCreate9", 0); return 0; }
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 64; pp->BackBufferHeight = 64;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    pp->EnableAutoDepthStencil = 1; pp->AutoDepthStencilFormat = D3DFMT_D24S8;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3D9::CreateDevice", 7,
                       (uint32_t[]){ d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, 0,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev) });
    if (hr != D3D_OK || !*pDev) { ok("CreateDevice(64x64)", 0); return 0; }
    /* Disable depth test — we only care about colour and fog. */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ *pDev, RS_ZENABLE, 0 });
    return *pDev;
}

static void clear64(uint32_t dev, uint32_t colour) {
    uint32_t zr; float z = 1.0f; memcpy(&zr, &z, 4);
    call("IDirect3DDevice9::Clear", 7,
         (uint32_t[]){ dev, 0, 0, 0x3u /* TARGET | ZBUFFER */, colour, zr, 0 });
}

static void read64(uint32_t *px) {
    hle_d3d9_metal_read_pixels(0, 0, 64, 64, px, 64 * 4);
}

static uint32_t upload_xyzw(uint32_t dev, const XYZW *v, uint32_t n, uint32_t fvf) {
    uint32_t bytes = sizeof(XYZW) * n;
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer", 7,
         (uint32_t[]){ dev, bytes, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });
    if (!*pVB) { ok("CreateVertexBuffer(XYZW)", 0); return 0; }
    uint32_t *pd = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, bytes, A(pd), 0 });
    if (*pd) memcpy(hle_ptr(*pd), v, bytes);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(XYZW) });
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, fvf });
    return *pVB;
}

static void draw_trilist(uint32_t dev) {
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
}

int main(void) {
    uint32_t dev = make_device64();
    if (!dev) return 1;
    uint32_t px[64 * 64];

    /* ── Test 1: Pixel-coordinate quad at 64×64 viewport ──────────────── */
    {
        clear64(dev, 0xFF000000u);
        XYZW v[6] = {
            { 0, 0,0.5f,1, 0xFFFF0000u}, {64, 0,0.5f,1, 0xFFFF0000u}, {64,64,0.5f,1, 0xFFFF0000u},
            { 0, 0,0.5f,1, 0xFFFF0000u}, {64,64,0.5f,1, 0xFFFF0000u}, { 0,64,0.5f,1, 0xFFFF0000u},
        };
        upload_xyzw(dev, v, 6, FVF_XYZW);
        draw_trilist(dev);
        read64(px);
        ok("Pixel-Quad 64x64: center red", px[32 * 64 + 32] == 0xFFFF0000u);
        ok("Pixel-Quad 64x64: corner (0,0) red", px[0] == 0xFFFF0000u);
        /* A sub-pixel quad: only a few pixels should be covered. */
        clear64(dev, 0xFF000000u);
        XYZW sv[6] = {
            {30,30,0.5f,1, 0xFF00FF00u}, {34,30,0.5f,1, 0xFF00FF00u}, {34,34,0.5f,1, 0xFF00FF00u},
            {30,30,0.5f,1, 0xFF00FF00u}, {34,34,0.5f,1, 0xFF00FF00u}, {30,34,0.5f,1, 0xFF00FF00u},
        };
        upload_xyzw(dev, sv, 6, FVF_XYZW);
        draw_trilist(dev);
        read64(px);
        ok("Pixel-Quad small: center green", px[32 * 64 + 32] == 0xFF00FF00u);
        ok("Pixel-Quad small: outside black", px[10 * 64 + 10] == 0xFF000000u);
    }

    /* ── Test 2: Offset viewport ──────────────────────────────────────── */
    {
        D3DVIEWPORT9 *vp = (D3DVIEWPORT9 *)hle_alloc(sizeof(*vp));
        vp->X = 16; vp->Y = 16; vp->Width = 32; vp->Height = 32;
        vp->MinZ = 0.0f; vp->MaxZ = 1.0f;
        call("IDirect3DDevice9::SetViewport", 2, (uint32_t[]){ dev, A(vp) });

        clear64(dev, 0xFF000000u);
        /* Quad covering the full offset viewport: (16,16)–(48,48). */
        XYZW v[6] = {
            {16,16,0.5f,1, 0xFF0000FFu}, {48,16,0.5f,1, 0xFF0000FFu}, {48,48,0.5f,1, 0xFF0000FFu},
            {16,16,0.5f,1, 0xFF0000FFu}, {48,48,0.5f,1, 0xFF0000FFu}, {16,48,0.5f,1, 0xFF0000FFu},
        };
        upload_xyzw(dev, v, 6, FVF_XYZW);
        draw_trilist(dev);
        read64(px);
        ok("Offset-Viewport: center blue", px[32 * 64 + 32] == 0xFF0000FFu);
        ok("Offset-Viewport: (16,16) blue (corner)", px[16 * 64 + 16] == 0xFF0000FFu);

        /* Reset viewport. */
        vp->X = 0; vp->Y = 0; vp->Width = 64; vp->Height = 64;
        call("IDirect3DDevice9::SetViewport", 2, (uint32_t[]){ dev, A(vp) });
    }

    /* ── Test 3: WVP independence ─────────────────────────────────────── */
    {
        /* Set wildly non-identity WVP matrices — must have no effect. */
        D3DMATRIX *m = (D3DMATRIX *)hle_alloc(sizeof(*m));
        memset(m, 0, sizeof(*m));
        m->m[0][0] = 3.0f; m->m[1][1] = 3.0f; m->m[2][2] = 3.0f; m->m[3][3] = 1.0f;
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, TS_WORLD,     A(m) });
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, TS_VIEW,      A(m) });
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, TS_PROJECTION, A(m) });

        clear64(dev, 0xFF000000u);
        XYZW v[6] = {
            { 0, 0,0.5f,1, 0xFFFF0000u}, {64, 0,0.5f,1, 0xFFFF0000u}, {64,64,0.5f,1, 0xFFFF0000u},
            { 0, 0,0.5f,1, 0xFFFF0000u}, {64,64,0.5f,1, 0xFFFF0000u}, { 0,64,0.5f,1, 0xFFFF0000u},
        };
        upload_xyzw(dev, v, 6, FVF_XYZW);
        draw_trilist(dev);
        read64(px);
        ok("WVP-unabhaengig: center red (ignores 3x scale)", px[32 * 64 + 32] == 0xFFFF0000u);
        ok("WVP-unabhaengig: corner red (inside quad)", px[0] == 0xFFFF0000u);

        /* Reset transforms to identity. */
        D3DMATRIX *id = (D3DMATRIX *)hle_alloc(sizeof(*id));
        memset(id, 0, sizeof(*id));
        id->m[0][0] = id->m[1][1] = id->m[2][2] = id->m[3][3] = 1.0f;
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, TS_WORLD,     A(id) });
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, TS_VIEW,      A(id) });
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, TS_PROJECTION, A(id) });
    }

    /* ── Test 4: Texture with rhw ─────────────────────────────────────── */
    {
        uint32_t *pTex = (uint32_t *)hle_alloc(4);
        call("IDirect3DDevice9::CreateTexture", 9,
             (uint32_t[]){ dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 });
        uint32_t tex = *pTex;
        ok("CreateTexture(2x2)", tex != 0);
        if (tex) {
            D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*lr));
            call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ tex, 0, A(lr), 0, 0 });
            uint32_t *bits = (uint32_t *)hle_ptr(lr->pBits);
            if (bits) {
                bits[0] = 0xFFFF0000u; bits[1] = 0xFF00FF00u;
                bits[2] = 0xFFFF0000u; bits[3] = 0xFF00FF00u;
            }
            call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ tex, 0 });

            call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex });
            call("IDirect3DDevice9::SetTextureStageState", 4,
                 (uint32_t[]){ dev, 0, 1, 2 }); /* COLOROP = SELECTARG1 */
            call("IDirect3DDevice9::SetTextureStageState", 4,
                 (uint32_t[]){ dev, 0, 2, 2 }); /* COLORARG1 = TEXTURE */

            clear64(dev, 0xFF000000u);
            /* XYZRHW + TEX1 quad: left u=0, right u=1. */
            XYZW_T v[6] = {
                { 0, 0,0.5f,1, 0xFFFFFFFFu, 0,0}, {64, 0,0.5f,1, 0xFFFFFFFFu, 1,0}, {64,64,0.5f,1, 0xFFFFFFFFu, 1,1},
                { 0, 0,0.5f,1, 0xFFFFFFFFu, 0,0}, {64,64,0.5f,1, 0xFFFFFFFFu, 1,1}, { 0,64,0.5f,1, 0xFFFFFFFFu, 0,1},
            };
            uint32_t *pVB = (uint32_t *)hle_alloc(4);
            call("IDirect3DDevice9::CreateVertexBuffer", 7,
                 (uint32_t[]){ dev, sizeof(v), 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });
            uint32_t *pd = (uint32_t *)hle_alloc(4);
            call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, sizeof(v), A(pd), 0 });
            if (*pd) memcpy(hle_ptr(*pd), v, sizeof(v));
            call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
            call("IDirect3DDevice9::SetStreamSource", 5,
                 (uint32_t[]){ dev, 0, *pVB, 0, sizeof(XYZW_T) });
            call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, FVF_XYZW_T });
            draw_trilist(dev);
            read64(px);
            ok("Textur-rhw: left half red", px[32 * 64 + 16] == 0xFFFF0000u);
            ok("Textur-rhw: right half green", px[32 * 64 + 48] == 0xFF00FF00u);

            call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, 0 });
            call("IDirect3DTexture9::Release", 1, (uint32_t[]){ tex });
        }
    }

    /* ── Test 5: Fog for XYZRHW (fog factor from z) ──────────────────── */
    {
        /* Reset texture stage from test 4: use diffuse, no texture. */
        call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, 0 });
        call("IDirect3DDevice9::SetTextureStageState", 4,
             (uint32_t[]){ dev, 0, 1, 1 }); /* COLOROP = DISABLE */
        call("IDirect3DDevice9::SetTextureStageState", 4,
             (uint32_t[]){ dev, 0, 2, 0 }); /* COLORARG1 = DIFFUSE */
        call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, RS_ZENABLE, 0 });
        call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 23u /* D3DRS_ZFUNC */, 8u /* D3DCMP_ALWAYS */ });

        call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, RS_FOGENABLE, 1 });
        call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, RS_FOGCOLOR, 0xFF0000FFu }); /* blue */
        call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, RS_FOGVERTEXMODE, 3u /* LINEAR */ });
        float fstart = 0.5f, fend = 0.9f;
        uint32_t fs, fe; memcpy(&fs, &fstart, 4); memcpy(&fe, &fend, 4);
        call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, RS_FOGSTART, fs });
        call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, RS_FOGEND, fe });

        /* z = 0.5 → fogFactor = 1.0 → no fog → red. */
        clear64(dev, 0xFF000000u);
        XYZW v_near[6] = {
            {0,0,0.5f,1, 0xFFFF0000u}, {64,0,0.5f,1, 0xFFFF0000u}, {64,64,0.5f,1, 0xFFFF0000u},
            {0,0,0.5f,1, 0xFFFF0000u}, {64,64,0.5f,1, 0xFFFF0000u}, {0,64,0.5f,1, 0xFFFF0000u},
        };
        upload_xyzw(dev, v_near, 6, FVF_XYZW);
        draw_trilist(dev);
        read64(px);
        ok("Fog-rhw: z=0.5 no fog (red)", px[32 * 64 + 32] == 0xFFFF0000u);

        /* z = 0.9 → fogFactor = 0.0 → full fog → blue. */
        clear64(dev, 0xFF000000u);
        XYZW v_far[6] = {
            {0,0,0.9f,1, 0xFFFF0000u}, {64,0,0.9f,1, 0xFFFF0000u}, {64,64,0.9f,1, 0xFFFF0000u},
            {0,0,0.9f,1, 0xFFFF0000u}, {64,64,0.9f,1, 0xFFFF0000u}, {0,64,0.9f,1, 0xFFFF0000u},
        };
        upload_xyzw(dev, v_far, 6, FVF_XYZW);
        draw_trilist(dev);
        read64(px);
        ok("Fog-rhw: z=0.9 full fog (blue)", px[32 * 64 + 32] == 0xFF0000FFu);

        /* z = 0.7 → fogFactor = 0.5 → 50% mix: red(127,0,0) + blue(0,0,127). */
        clear64(dev, 0xFF000000u);
        XYZW v_mid[6] = {
            {0,0,0.7f,1, 0xFFFF0000u}, {64,0,0.7f,1, 0xFFFF0000u}, {64,64,0.7f,1, 0xFFFF0000u},
            {0,0,0.7f,1, 0xFFFF0000u}, {64,64,0.7f,1, 0xFFFF0000u}, {0,64,0.7f,1, 0xFFFF0000u},
        };
        upload_xyzw(dev, v_mid, 6, FVF_XYZW);
        draw_trilist(dev);
        read64(px);
        /* 50% fog: R=127, G=0, B=127 → 0xFF7F007F.  Accept ±2 for rounding. */
        {
            uint32_t got = px[32 * 64 + 32];
            int r = (int)((got >> 16) & 0xFF);
            int b = (int)(got & 0xFF);
            ok("Fog-rhw: z=0.7 50%% fog (R~127 B~127)",
               r >= 125 && r <= 129 && b >= 125 && b <= 129);
        }

        call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, RS_FOGENABLE, 0 });
    }

    /* ── Test 6: DrawPrimitiveUP with XYZRHW ─────────────────────────── */
    {
        clear64(dev, 0xFF000000u);
        XYZW v[6] = {
            { 0, 0,0.5f,1, 0xFFFFFF00u}, {64, 0,0.5f,1, 0xFFFFFF00u}, {64,64,0.5f,1, 0xFFFFFF00u},
            { 0, 0,0.5f,1, 0xFFFFFF00u}, {64,64,0.5f,1, 0xFFFFFF00u}, { 0,64,0.5f,1, 0xFFFFFF00u},
        };
        /* Copy to HLE memory for the UP call. */
        uint32_t *pv = (uint32_t *)hle_alloc(sizeof(v));
        memcpy(pv, v, sizeof(v));
        call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, FVF_XYZW });
        call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
        call("IDirect3DDevice9::DrawPrimitiveUP", 5,
             (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 2, A(pv), sizeof(XYZW) });
        call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
        read64(px);
        ok("DrawPrimitiveUP-rhw: center yellow", px[32 * 64 + 32] == 0xFFFFFF00u);
        ok("DrawPrimitiveUP-rhw: corner yellow (inside quad)", px[0] == 0xFFFFFF00u);
    }

    /* ── Test 7: Clear + Scissor mit XYZRHW ────────────────────────────── */
    {
        /* Der Scissor begrenzt den XYZRHW-Draw; Clear bleibt unbeeinflusst. */
        int32_t r[4] = { 16, 16, 48, 48 };
        call("IDirect3DDevice9::SetScissorRect", 2,
             (uint32_t[]){ dev, A(G_copy(r, sizeof r)) });
        call("IDirect3DDevice9::SetRenderState", 3,
             (uint32_t[]){ dev, 174u /* D3DRS_SCISSORTESTENABLE */, 1 });
        clear64(dev, 0xFF000000u);
        XYZW v[6] = {
            { 0, 0,0.5f,1, 0xFFFFFF00u}, {64, 0,0.5f,1, 0xFFFFFF00u}, {64,64,0.5f,1, 0xFFFFFF00u},
            { 0, 0,0.5f,1, 0xFFFFFF00u}, {64,64,0.5f,1, 0xFFFFFF00u}, { 0,64,0.5f,1, 0xFFFFFF00u},
        };
        upload_xyzw(dev, v, 6, FVF_XYZW);
        draw_trilist(dev);
        read64(px);
        ok("Scissor+XYZRHW: innen gelb", px[32 * 64 + 32] == 0xFFFFFF00u);
        ok("Scissor+XYZRHW: aussen schwarz", px[8 * 64 + 8] == 0xFF000000u);
        call("IDirect3DDevice9::SetRenderState", 3,
             (uint32_t[]){ dev, 174u, 0 });
    }

    /* ── Test 8: z over the whole D3D range 0..1 is visible ─────────── */
    {
        /* Pre-transformed z is already a depth value in 0..1 (0 = near, the
         * usual z of 2D drawing); none of it may be clipped away. */
        static const float zs[3] = { 0.0f, 0.25f, 1.0f };
        static const char *const what[3] = { "XYZRHW z=0 visible", "XYZRHW z=0.25 visible", "XYZRHW z=1 visible" };
        for (int k = 0; k < 3; k++) {
            clear64(dev, 0xFF000000u);
            XYZW v[6] = {
                {0,0,zs[k],1, 0xFFFF00FFu}, {64,0,zs[k],1, 0xFFFF00FFu}, {64,64,zs[k],1, 0xFFFF00FFu},
                {0,0,zs[k],1, 0xFFFF00FFu}, {64,64,zs[k],1, 0xFFFF00FFu}, {0,64,zs[k],1, 0xFFFF00FFu},
            };
            upload_xyzw(dev, v, 6, FVF_XYZW);
            draw_trilist(dev);
            read64(px);
            ok(what[k], px[32 * 64 + 32] == 0xFFFF00FFu);
        }
    }

    printf(fails == 0 ? "test_d3d9_xyzrhw: OK\n" : "test_d3d9_xyzrhw: FAILED\n");
    return fails ? 1 : 0;
}