/* Hand-calculated D3D9 texture-coordinate-generation tests on the real Metal
 * backend.  Sources: Microsoft Learn D3DTEXTURESTAGESTATETYPE (TEXCOORDINDEX
 * CAMERASPACE* modes, D3DTTFF_COUNTn / D3DTTFF_PROJECTED, BUMPENVMAP), Wine
 * dlls/wined3d/glsl_shader.c (public texgen formulas).
 *
 * Every draw goes through D3D9 calls; pixels are read back with the Metal
 * readback helper.  Expected colors are computed by hand from the generated
 * UVs and the known 2x2 / 4x1 texture contents. */
#include "test_d3d9_common.h"

#define W 32
#define H 32

/* Vertex with position + normal + diffuse + one texcoord. */
typedef struct {
    float x, y, z;
    float nx, ny, nz;
    uint32_t diffuse;
    float u, v;
} VtxXYZN;

/* Vertex with transformed position (RHW) + diffuse + one texcoord. */
typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse;
    float u, v;
} VtxRHW;

/* Same but with two texcoords (needed for two active texture stages). */
typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse;
    float u, v;
    float u2, v2;
} VtxRHW2;

static uint32_t make_tex(uint32_t dev, uint32_t w, uint32_t h, const uint32_t *colors) {
    uint32_t *pTex = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3DDevice9::CreateTexture", 9,
        (uint32_t[]){ dev, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 });
    ok("CreateTexture", hr == D3D_OK && *pTex);
    D3DLOCKED_RECT *plr = (D3DLOCKED_RECT *)hle_alloc(sizeof(*plr));
    call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ *pTex, 0, A(plr), 0, 0 });
    uint32_t *px = (uint32_t *)hle_ptr(plr->pBits);
    for (uint32_t i = 0; i < w * h; i++) px[i] = colors[i];
    call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ *pTex, 0 });
    return *pTex;
}

static void set_stage(uint32_t dev, uint32_t stage, uint32_t cop, uint32_t ca1, uint32_t ca2,
                      uint32_t aop, uint32_t aa1, uint32_t aa2) {
    call("IDirect3DDevice9::SetTextureStageState", 4, (uint32_t[]){ dev, stage, 1, cop });
    call("IDirect3DDevice9::SetTextureStageState", 4, (uint32_t[]){ dev, stage, 2, ca1 });
    call("IDirect3DDevice9::SetTextureStageState", 4, (uint32_t[]){ dev, stage, 3, ca2 });
    call("IDirect3DDevice9::SetTextureStageState", 4, (uint32_t[]){ dev, stage, 4, aop });
    call("IDirect3DDevice9::SetTextureStageState", 4, (uint32_t[]){ dev, stage, 5, aa1 });
    call("IDirect3DDevice9::SetTextureStageState", 4, (uint32_t[]){ dev, stage, 6, aa2 });
}

static void draw_rhw_quad(uint32_t dev, uint32_t vb, uint32_t diffuse) {
    VtxRHW v[4] = {
        { 0, 0,0.5f,1, diffuse, 0,0},
        {32, 0,0.5f,1, diffuse, 1,0},
        {32,32,0.5f,1, diffuse, 1,1},
        { 0,32,0.5f,1, diffuse, 0,1}
    };
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ vb, 0, sizeof(v), A(pData), 0 });
    memcpy(hle_ptr(*pData), v, sizeof(v));
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ vb });
    call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, vb, 0, sizeof(VtxRHW) });
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0x004u | 0x040u | (1u << 8) });
    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
}

static uint32_t pixel(uint32_t x, uint32_t y) {
    uint32_t px[W * H];
    read_all(px);
    return px[y * W + x];
}

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    /* 2x2 texture: (0,0)=red (0,1)=green (1,0)=blue (1,1)=yellow */
    uint32_t c2[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u };
    uint32_t tex2 = make_tex(dev, 2, 2, c2);
    /* 4x1 texture: red, green, blue, yellow */
    uint32_t c4[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u };
    uint32_t tex4 = make_tex(dev, 4, 1, c4);

    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer", 7,
         (uint32_t[]){ dev, sizeof(VtxRHW) * 4, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });

    /* ---- CAMERASPACEPOSITION: texcoord = (view*world*pos).xy -------------
     * Quad positions -1..1, all texcoords forced to (0,0).  With identity
     * world/view the generated UV equals position, so the four quadrants
     * sample different texture corners.  Passthrough texcoords would give a
     * uniform red screen.  With identity world/view (device default for zero
     * transforms) the camera-space position equals the object position.
     * At pixel (4,4): pos=(-0.75,0.75) -> clamp U=0, V=0.75 -> texel(0,1)=blue.
     * At pixel (28,4): pos=(0.75,0.75) -> texel(1,1)=yellow. */
    {
        /* Eigener VB in der groesseren XYZN-Stride (VtxRHW ist nur 28 Byte). */
        uint32_t *pPosVB = (uint32_t *)hle_alloc(4);
        call("IDirect3DDevice9::CreateVertexBuffer", 7,
             (uint32_t[]){ dev, sizeof(VtxXYZN) * 4, 0, 0, D3DPOOL_MANAGED, A(pPosVB), 0 });
        uint32_t *pData = (uint32_t *)hle_alloc(4);
        VtxXYZN v[4] = {
            {-1,-1,0.5f, 0,1,0, 0xFFFFFFFFu, 0,0},
            { 1,-1,0.5f, 0,1,0, 0xFFFFFFFFu, 0,0},
            { 1, 1,0.5f, 0,1,0, 0xFFFFFFFFu, 0,0},
            {-1, 1,0.5f, 0,1,0, 0xFFFFFFFFu, 0,0}
        };
        call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pPosVB, 0, sizeof(v), A(pData), 0 });
        memcpy(hle_ptr(*pData), v, sizeof(v));
        call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pPosVB });
        call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, *pPosVB, 0, sizeof(VtxXYZN) });
        call("IDirect3DDevice9::SetFVF", 2,
             (uint32_t[]){ dev, 0x002u | 0x010u | 0x040u | (1u << 8) });
        D3DMATRIX id2; memset(&id2, 0, sizeof(id2));
        id2.m[0][0] = id2.m[1][1] = id2.m[2][2] = id2.m[3][3] = 1.0f;
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 256, A(G_copy(&id2, 64)) });
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 2, A(G_copy(&id2, 64)) });
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 3, A(G_copy(&id2, 64)) });
        call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex2 });
        set_stage(dev, 0, 2, 2, 0, 2, 2, 0); /* SELECTARG1 TEXTURE */
        call("IDirect3DDevice9::SetTextureStageState", 4,
             (uint32_t[]){ dev, 0, 11, 0x20000u }); /* D3DTSS_TCI_CAMERASPACEPOSITION */
        clear(dev, 0xFF000000u, 1, 1.0f);
        call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
        call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 2 });
        call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
        { uint32_t got = pixel(4,4);
          ok("CAMERASPACEPOSITION texel(0,1)=blue", got == 0xFF0000FFu);
          if (got != 0xFF0000FFu) printf("  got 0x%08X\n", got); }
        { uint32_t got = pixel(28,4);
          ok("CAMERASPACEPOSITION texel(1,1)=yellow", got == 0xFFFFFF00u);
          if (got != 0xFFFFFF00u) printf("  got 0x%08X\n", got); }
        call("IDirect3DDevice9::SetTextureStageState", 4, (uint32_t[]){ dev, 0, 11, 0 });
    }

    /* ---- CAMERASPACENORMAL: texcoord = normalize(view*world*normal).xy ---
     * FVF XYZ|NORMAL|DIFFUSE|TEX1, all normals (0,1,0).  Generated UV=(0,1):
     * U=0 -> col 0, V=1 -> row 1 (clamp) -> blue.  Passthrough texcoords=0
     * would give red. */
    {
        VtxXYZN v[4] = {
            {-1,-1,0.5f, 0,1,0, 0xFFFFFFFFu, 0,0},
            { 1,-1,0.5f, 0,1,0, 0xFFFFFFFFu, 0,0},
            { 1, 1,0.5f, 0,1,0, 0xFFFFFFFFu, 0,0},
            {-1, 1,0.5f, 0,1,0, 0xFFFFFFFFu, 0,0}
        };
        uint32_t *pTexVB = (uint32_t *)hle_alloc(4);
        call("IDirect3DDevice9::CreateVertexBuffer", 7,
             (uint32_t[]){ dev, sizeof(v), 0, 0, D3DPOOL_MANAGED, A(pTexVB), 0 });
        uint32_t *pData = (uint32_t *)hle_alloc(4);
        call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pTexVB, 0, sizeof(v), A(pData), 0 });
        memcpy(hle_ptr(*pData), v, sizeof(v));
        call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pTexVB });
        call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, *pTexVB, 0, sizeof(VtxXYZN) });
        call("IDirect3DDevice9::SetFVF", 2,
             (uint32_t[]){ dev, 0x002u | 0x010u | 0x040u | (1u << 8) }); /* XYZ|NORMAL|DIFFUSE|TEX1 */
        /* Identity world/view, projection maps [-1,1] to NDC [-1,1]. */
        D3DMATRIX id; memset(&id, 0, sizeof(id));
        id.m[0][0] = id.m[1][1] = id.m[2][2] = id.m[3][3] = 1.0f;
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 256, A(G_copy(&id, 64)) });
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 2, A(G_copy(&id, 64)) });
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 3, A(G_copy(&id, 64)) });
        call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex2 });
        set_stage(dev, 0, 2, 2, 0, 2, 2, 0);
        call("IDirect3DDevice9::SetTextureStageState", 4,
             (uint32_t[]){ dev, 0, 11, 0x10000u }); /* D3DTSS_TCI_CAMERASPACENORMAL */
        clear(dev, 0xFF000000u, 1, 1.0f);
        call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
        call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 2 });
        call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
        { uint32_t got = pixel(16,16);
          ok("CAMERASPACENORMAL texel(0,1)=blue", got == 0xFF0000FFu);
          if (got != 0xFF0000FFu) printf("  got 0x%08X\n", got); }
        call("IDirect3DDevice9::SetTextureStageState", 4, (uint32_t[]){ dev, 0, 11, 0 });
        call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pTexVB });
    }

    /* ---- TEXTURETRANSFORMFLAGS COUNTn|PROJECTED -------------------------
     * 4x1 texture (red,green,blue,yellow).  Matrix sets y=1, z=2, w=4 with
     * x=u.  At pixel (24,24) u=0.75:
     *   COUNT2|PROJECTED: div=_t.y=1 -> u'=0.75 -> col 3 = yellow
     *   COUNT3|PROJECTED: div=_t.z=2 -> u'=0.375 -> col 1 = green
     *   COUNT4|PROJECTED: div=_t.w=4 -> u'=0.1875 -> col 0 = red
     * This distinguishes which component is used as the projective divisor. */
    {
        D3DMATRIX m; memset(&m, 0, sizeof(m));
        m.m[0][0] = 1.0f; m.m[3][1] = 1.0f; m.m[3][2] = 2.0f; m.m[3][3] = 4.0f;
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 16, A(G_copy(&m, 64)) });
        call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, tex4 });
        set_stage(dev, 0, 2, 2, 0, 2, 2, 0);
        uint32_t counts[3] = { 2, 3, 4 };
        uint32_t expected[3] = { 0xFFFFFF00u, 0xFF00FF00u, 0xFFFF0000u };
        const char *names[3] = { "COUNT2", "COUNT3", "COUNT4" };
        for (uint32_t k = 0; k < 3; k++) {
            call("IDirect3DDevice9::SetTextureStageState", 4,
                 (uint32_t[]){ dev, 0, 24, counts[k] | 0x100u }); /* TTFF_COUNTn|PROJECTED */
            draw_rhw_quad(dev, *pVB, 0xFFFFFFFFu);
            uint32_t got = pixel(24,24);
            char msg[80]; snprintf(msg, sizeof msg, "%s|PROJECTED divisor", names[k]);
            ok(msg, got == expected[k]);
            if (got != expected[k]) printf("  got 0x%08X expected 0x%08X\n", got, expected[k]);
        }
        call("IDirect3DDevice9::SetTextureStageState", 4, (uint32_t[]){ dev, 0, 24, 0 });
    }

    /* ---- BUMPENVMAP: perturb stage-1 UVs by the bump map's R/G channels --
     * Stage0 bump texture is all (R=255,G=0) -> du=+1, dv=-1.  Bumpenv matrix
     * m00=0.25,m01=0,m10=0,m11=0 -> delta=(0.25,0).  Stage1 samples 4x1 tex
     * at uv+(0.25,0).  At pixel (4,4) uv=0.125 -> 0.375 -> col 1 = green.
     * Without bump: uv=0.125 -> col 0 = red. */
    {
        uint32_t bump_col[4] = { 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u };
        uint32_t bump = make_tex(dev, 2, 2, bump_col);
        float b00 = 0.25f, zero = 0.0f;
        uint32_t ub00, uz;
        memcpy(&ub00, &b00, 4); memcpy(&uz, &zero, 4);
        call("IDirect3DDevice9::SetTextureStageState", 4,
             (uint32_t[]){ dev, 0, 7, ub00 }); /* BUMPENVMAT00 */
        call("IDirect3DDevice9::SetTextureStageState", 4,
             (uint32_t[]){ dev, 0, 8, uz });  /* BUMPENVMAT01 */
        call("IDirect3DDevice9::SetTextureStageState", 4,
             (uint32_t[]){ dev, 0, 9, uz });  /* BUMPENVMAT10 */
        call("IDirect3DDevice9::SetTextureStageState", 4,
             (uint32_t[]){ dev, 0, 10, uz }); /* BUMPENVMAT11 */
        /* stage0 BUMPENVMAP (D3DTOP_BUMPENVMAP = 22) */
        set_stage(dev, 0, 22, 0, 0, 2, 0, 0);
        call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, bump });
        /* stage1 SELECTARG1 TEXTURE */
        set_stage(dev, 1, 2, 2, 0, 2, 2, 0);
        call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 1, tex4 });
        /* Two stages require TEX2 in the FVF (stage_count follows texcoords). */
        uint32_t *pVB2 = (uint32_t *)hle_alloc(4);
        call("IDirect3DDevice9::CreateVertexBuffer", 7,
             (uint32_t[]){ dev, sizeof(VtxRHW2) * 4, 0, 0, D3DPOOL_MANAGED, A(pVB2), 0 });
        {
            VtxRHW2 v[4] = {
                { 0, 0,0.5f,1, 0xFFFFFFFFu, 0,0, 0,0},
                {32, 0,0.5f,1, 0xFFFFFFFFu, 1,0, 0,0},
                {32,32,0.5f,1, 0xFFFFFFFFu, 1,1, 0,0},
                { 0,32,0.5f,1, 0xFFFFFFFFu, 0,1, 0,0}
            };
            uint32_t *pData = (uint32_t *)hle_alloc(4);
            call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB2, 0, sizeof(v), A(pData), 0 });
            memcpy(hle_ptr(*pData), v, sizeof(v));
            call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB2 });
            call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, *pVB2, 0, sizeof(VtxRHW2) });
            call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0x004u | 0x040u | (2u << 8) });
        }
        clear(dev, 0xFF000000u, 1, 1.0f);
        call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
        call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 2 });
        call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
        { uint32_t got = pixel(4,4);
          ok("BUMPENVMAP perturbs stage1 UV -> green", got == 0xFF00FF00u);
          if (got != 0xFF00FF00u) printf("  got 0x%08X\n", got); }
        /* reset */
        set_stage(dev, 1, 1, 2, 0, 1, 2, 0);
        set_stage(dev, 0, 2, 2, 0, 2, 2, 0);
        call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 0, 0 });
        call("IDirect3DDevice9::SetTexture", 3, (uint32_t[]){ dev, 1, 0 });
        call("IDirect3DTexture9::Release", 1, (uint32_t[]){ bump });
        call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pVB2 });
    }

    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ tex2 });
    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ tex4 });
    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pVB });
    printf(fails == 0 ? "test_d3d9_ffp_texcoord_gen: OK\n" : "test_d3d9_ffp_texcoord_gen: FAILED\n");
    return fails != 0;
}
