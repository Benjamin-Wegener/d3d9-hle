/* .fx source -> ID3DXEffect -> real Metal: a changed parameter changes the
 * rendered image. No binary FX container, no application shaders. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_metal.h"
#include "d3dx9_effect_hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what); if (!cond) fails++; }
static int d8(int a, int b) { return a > b ? a - b : b - a; }
static int near_ch(uint32_t px, int r, int g, int b, int tol) {
    return d8((int)((px >> 16) & 0xFFu), r) <= tol &&   /* 0xAARRGGBB */
           d8((int)((px >> 8) & 0xFFu), g) <= tol &&
           d8((int)(px & 0xFFu), b) <= tol;
}
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define A(p) hle_va(p)

static const char *FX =
    "texture SrcTex;\n"
    "float4 Tint;\n"
    "sampler2D S = sampler_state { Texture = (SrcTex); MinFilter = %s; MagFilter = %s; };\n"
    "void VS(float4 pos : POSITION, float2 uv : TEXCOORD0, out float4 oPos : POSITION, out float2 oUv : TEXCOORD0)\n"
    "{ oPos = pos; oUv = uv; }\n"
    "float4 PS(float2 uv : TEXCOORD0) : COLOR { return tex2D(S, uv) * Tint * SCALE; }\n"
    "technique T {\n"
    "  pass P {\n"
    "    VertexShader = compile vs_1_1 VS();\n"
    "    PixelShader = compile ps_2_0 PS();\n"
    "  }\n"
    "}\n";

int main(void)
{
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    win32hle_register(win32hle_d3d9fx);
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 32; pp->BackBufferHeight = 32;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    ok("CreateDevice", call("IDirect3D9::CreateDevice", 7,
        (uint32_t[]){ d3d9, 0, D3DDEVTYPE_HAL, 0, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                      A(pp), A(pDev) }) == D3D_OK && *pDev);
    uint32_t dev = *pDev;
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 7 /* D3DRS_ZENABLE */, 0 });   /* repeated draws overwrite */

    /* A 2x2 texture with four different colours: TL red, TR green, BL blue, BR yellow. */
    uint32_t *pTex = (uint32_t *)hle_alloc(4);
    ok("CreateTexture", call("IDirect3DDevice9::CreateTexture", 9,
        (uint32_t[]){ dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, 1, A(pTex), 0 }) == D3D_OK && *pTex);
    uint32_t tex = *pTex;
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(8);
    call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ tex, 0, A(lr), 0, 0 });
    uint32_t *t = (uint32_t *)hle_ptr(lr->pBits);
    t[0] = 0xFFFF0000u; t[1] = 0xFF00FF00u; t[2] = 0xFF0000FFu; t[3] = 0xFFFFFF00u;
    call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ tex, 0 });

    /* Fullscreen quad in the translated VS layout (16 float4 per vertex). */
    const float pos[6][2] = { { -1,1 }, { 1,1 }, { -1,-1 }, { -1,-1 }, { 1,1 }, { 1,-1 } };
    float *verts = (float *)hle_alloc(sizeof(float) * 6 * 16 * 4);
    memset(verts, 0, sizeof(float) * 6 * 16 * 4);
    for (int i = 0; i < 6; i++) {
        float *q = verts + i * 16 * 4;
        q[0] = pos[i][0]; q[1] = pos[i][1]; q[2] = 0.5f; q[3] = 1.0f;
        q[4] = pos[i][0] > 0 ? 1.0f : 0.0f;            /* uv.x: -1 -> 0, +1 -> 1 */
        q[5] = pos[i][1] > 0 ? 0.0f : 1.0f;            /* uv.y: +1 (top) -> 0, -1 -> 1 */
    }
    uint32_t bytes = (uint32_t)(sizeof(float) * 6 * 16 * 4);
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer", 7, (uint32_t[]){ dev, bytes, 0, 0, 1, A(pVB), 0 });
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, bytes, A(pData), 0 });
    memcpy(hle_ptr(*pData), verts, bytes);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
    call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, *pVB, 0, bytes / 6 });
    void *decl = hle_alloc(24);
    uint8_t *db = (uint8_t *)decl;
    memset(db, 0, 24);
    /* D3DVERTEXELEMENT9: Stream(2) Offset(2) Type Method Usage UsageIndex */
    db[4]=3;                                  /* POSITION: FLOAT4 at offset 0 */
    db[8+2]=16; db[8+4]=1; db[8+6]=5;         /* TEXCOORD0: FLOAT2 at offset 16 */
    db[16]=0xFF; db[16+4]=17;                 /* D3DDECL_END */
    uint32_t *pDecl = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexDeclaration", 3, (uint32_t[]){ dev, A(decl), A(pDecl) });
    call("IDirect3DDevice9::SetVertexDeclaration", 2, (uint32_t[]){ dev, *pDecl });

    /* Build the effect from .fx source text. */
    uint32_t *pFX = (uint32_t *)hle_alloc(4), *pErr = (uint32_t *)hle_alloc(4);
    *pFX = 0; *pErr = 0;
    char fxbuf[1024];
    snprintf(fxbuf, sizeof fxbuf, FX, "Point", "Point");           /* the filter is part of the source */
    uint32_t srclen = (uint32_t)strlen(fxbuf);
    void *src = hle_alloc(srclen + 1);
    memcpy(src, fxbuf, srclen + 1);
    /* D3DXMACRO { Name, Definition }, NULL-terminated: SCALE must reach the
     * HLSL compiler, otherwise PS fails to compile (undeclared identifier). */
    void *mname = hle_alloc(6), *mdef = hle_alloc(5);
    memcpy(mname, "SCALE", 6); memcpy(mdef, "1.0f", 5);
    uint32_t *macros = (uint32_t *)hle_alloc(16);
    macros[0] = A(mname); macros[1] = A(mdef); macros[2] = macros[3] = 0;
    uint32_t hr = call("D3DXCreateEffect", 9,
        (uint32_t[]){ dev, A(src), srclen, A(macros), 0, 0, 0 /* pPool */, A(pFX), A(pErr) });
    if (hr != D3D_OK && *pErr)
        printf("    compiler: %s\n", (const char *)hle_ptr(MEM32(*pErr)));
    ok("D3DXCreateEffect from source", hr == D3D_OK && *pFX);
    if (hr != D3D_OK || !*pFX) return 1;
    uint32_t effect = *pFX;

    uint32_t *pTint = (uint32_t *)hle_alloc(4), *pTexP = (uint32_t *)hle_alloc(4);
    void *n_tint = hle_alloc(8), *n_tex = hle_alloc(8);
    memcpy(n_tint, "Tint", 5); memcpy(n_tex, "SrcTex", 7);
    *pTint = call("ID3DXEffect::GetParameterByName", 3, (uint32_t[]){ effect, 0, A(n_tint) });
    /* The texture is set through its own name (the `texture` declaration), as
     * an application does, not through the sampler. */
    *pTexP = call("ID3DXEffect::GetParameterByName", 3, (uint32_t[]){ effect, 0, A(n_tex) });
    ok("effect parameters found by name", *pTint != 0 && *pTexP != 0);
    ok("SetTexture(SrcTex)", call("ID3DXEffect::SetTexture", 3, (uint32_t[]){ effect, *pTexP, tex }) == D3D_OK);

    uint32_t pixels[32 * 32];
    const uint32_t RED = 0xFFFF0000u, GREEN = 0xFF00FF00u, BLUE = 0xFF0000FFu,
                   YELLOW = 0xFFFFFF00u, BLACK = 0xFF000000u;
    float white[4] = { 1, 1, 1, 1 }, red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };
    void *wg = hle_alloc(16), *rg = hle_alloc(16), *gg = hle_alloc(16);
    memcpy(wg, white, 16); memcpy(rg, red, 16); memcpy(gg, green, 16);
    uint32_t *pn = (uint32_t *)hle_alloc(4); *pn = 0;
#define PX(x, y) pixels[(y) * 32 + (x)]
    /* One frame: Begin (flags in the HLE's current 2-argument form), one pass, one draw. */
    #define DRAW_FRAME(fx) do { \
        call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, 1, BLACK, 0, 0 }); \
        call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev }); \
        call("ID3DXEffect::Begin", 3, (uint32_t[]){ (fx), A(pn), 0 }); \
        call("ID3DXEffect::BeginPass", 2, (uint32_t[]){ (fx), 0 }); \
        call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 }); \
        call("ID3DXEffect::EndPass", 1, (uint32_t[]){ (fx) }); \
        call("ID3DXEffect::End", 1, (uint32_t[]){ (fx) }); \
        call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev }); \
    } while (0)

    /* Tint white, Point filter: every quadrant shows its own texel. */
    call("ID3DXEffect::SetVector", 3, (uint32_t[]){ effect, *pTint, A(wg) });
    ok("CommitChanges == D3D_OK", call("ID3DXEffect::CommitChanges", 1, (uint32_t[]){ effect }) == D3D_OK);
    DRAW_FRAME(effect);
    ok("one pass", *pn == 1);
    ok("Metal readback 1", hle_d3d9_metal_read_pixels(0, 0, 32, 32, pixels, 32 * 4) == 0);
    ok("top-left shows texel (0,0) red", PX(8, 8) == RED);
    uint32_t point_quadrant = PX(8, 8);   /* POINT reference for the LINEAR check below */
    ok("top-right shows texel (1,0) green", PX(24, 8) == GREEN);
    ok("bottom-left shows texel (0,1) blue", PX(8, 24) == BLUE);
    ok("bottom-right shows texel (1,1) yellow", PX(24, 24) == YELLOW);
    ok("Point: just inside the top-left quadrant is still exactly red", PX(15, 15) == RED);
    ok("Point: just inside the bottom-right quadrant is still exactly yellow", PX(16, 16) == YELLOW);

    /* Tint red: red stays red, green and blue are multiplied away, yellow becomes red. */
    call("ID3DXEffect::SetVector", 3, (uint32_t[]){ effect, *pTint, A(rg) });
    DRAW_FRAME(effect);
    ok("Metal readback 2", hle_d3d9_metal_read_pixels(0, 0, 32, 32, pixels, 32 * 4) == 0);
    ok("red tint: TL red, TR black, BL black, BR red",
       PX(8, 8) == RED && PX(24, 8) == BLACK && PX(8, 24) == BLACK && PX(24, 24) == RED);

    /* CommitChanges inside a pass: a parameter set after BeginPass reaches the
     * device only with CommitChanges. Each frame draws twice into the same pixels
     * (the depth test is off): once with the old tint, once after the change. */
    #define TWO_DRAWS(commit) do { \
        call("ID3DXEffect::SetVector", 3, (uint32_t[]){ effect, *pTint, A(rg) }); \
        call("IDirect3DDevice9::Clear", 7, (uint32_t[]){ dev, 0, 0, 1, BLACK, 0, 0 }); \
        call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev }); \
        call("ID3DXEffect::Begin", 3, (uint32_t[]){ effect, A(pn), 0 }); \
        call("ID3DXEffect::BeginPass", 2, (uint32_t[]){ effect, 0 }); \
        call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 }); \
        call("ID3DXEffect::SetVector", 3, (uint32_t[]){ effect, *pTint, A(gg) }); \
        if (commit) ok("CommitChanges inside the pass", call("ID3DXEffect::CommitChanges", 1, (uint32_t[]){ effect }) == D3D_OK); \
        call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 }); \
        call("ID3DXEffect::EndPass", 1, (uint32_t[]){ effect }); \
        call("ID3DXEffect::End", 1, (uint32_t[]){ effect }); \
        call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev }); \
        hle_d3d9_metal_read_pixels(0, 0, 32, 32, pixels, 32 * 4); \
    } while (0)
    TWO_DRAWS(0);
    ok("without CommitChanges the second draw still uses the old (red) tint: TL red, TR black",
       PX(8, 8) == RED && PX(24, 8) == BLACK);
    TWO_DRAWS(1);
    ok("with CommitChanges the second draw uses the new (green) tint: TL black (red x green), TR green",
       PX(8, 8) == BLACK && PX(24, 8) == GREEN);

    /* Sampler states: the effect's sampler_state must reach both the device
     * and the Metal sampler. A second effect with Linear must change what the
     * device reports AND change the sampled image (the sampler is no longer
     * hard-wired to Nearest). */
    uint32_t *pv = (uint32_t *)hle_alloc(4);
    #define SAMPLER_STATE(type) (call("IDirect3DDevice9::GetSamplerState", 4, (uint32_t[]){ dev, 0, (type), A(pv) }), *pv)
    call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 5, 3 });    /* MAGFILTER: anisotropic, a value the effect does not use */
    call("IDirect3DDevice9::SetSamplerState", 4, (uint32_t[]){ dev, 0, 6, 3 });    /* MINFILTER */
    DRAW_FRAME(effect);
    ok("Point effect: MAGFILTER = D3DTEXF_POINT (1) after BeginPass", SAMPLER_STATE(5) == 1u);
    ok("Point effect: MINFILTER = D3DTEXF_POINT (1) after BeginPass", SAMPLER_STATE(6) == 1u);
    snprintf(fxbuf, sizeof fxbuf, FX, "Linear", "Linear");
    uint32_t srclen2 = (uint32_t)strlen(fxbuf);
    void *src2 = hle_alloc(srclen2 + 1);
    memcpy(src2, fxbuf, srclen2 + 1);
    uint32_t *pFX2 = (uint32_t *)hle_alloc(4); *pFX2 = 0;
    ok("D3DXCreateEffect with Linear filter", call("D3DXCreateEffect", 9,
        (uint32_t[]){ dev, A(src2), srclen2, A(macros), 0, 0, 0 /* pPool */, A(pFX2), A(pErr) }) == D3D_OK && *pFX2);
    uint32_t fx2 = *pFX2;
    uint32_t tint2 = call("ID3DXEffect::GetParameterByName", 3, (uint32_t[]){ fx2, 0, A(n_tint) });
    uint32_t tex2 = call("ID3DXEffect::GetParameterByName", 3, (uint32_t[]){ fx2, 0, A(n_tex) });
    call("ID3DXEffect::SetTexture", 3, (uint32_t[]){ fx2, tex2, tex });
    call("ID3DXEffect::SetVector", 3, (uint32_t[]){ fx2, tint2, A(wg) });
    DRAW_FRAME(fx2);
    ok("Linear effect: MAGFILTER = D3DTEXF_LINEAR (2) after BeginPass", SAMPLER_STATE(5) == 2u);
    ok("Linear effect: MINFILTER = D3DTEXF_LINEAR (2) after BeginPass", SAMPLER_STATE(6) == 2u);
    hle_d3d9_metal_read_pixels(0, 0, 32, 32, pixels, 32 * 4);
    /* Pixel centres are at x+0.5, so (8,8) lies 0.03125 texels past the centre
     * of texel (0,0). The analytic bilinear weights over
     * red/green/blue/yellow are 0.93848 / 0.03027 / 0.03027 / 0.00098, giving
     * (239.6, 8.0, 8.0); (24,24) is similarly just past the yellow texel.
     * POINT gave pure red at (8,8), so a real LINEAR sampler must differ. */
    ok("Linear effect: LINEAR actually changes the sampled quadrant",
       PX(8, 8) != point_quadrant);
    ok("Linear effect: (8,8) is the analytic bilinear blend (240,8,8)",
       near_ch(PX(8, 8), 240, 8, 8, 4));
    ok("Linear effect: (24,24) stays yellow (255,255,0)",
       near_ch(PX(24, 24), 255, 255, 0, 4));
    ok("Linear effect: extreme corner still samples the edge texel", PX(0, 0) == RED);

    /* The public factory entry points must resolve to the real .fx-source
     * implementations, not to the old not-available stubs. */
    uint32_t *pEx = (uint32_t *)hle_alloc(4);
    *pEx = 0;
    ok("CreateEffectEx from source", call("D3DXCreateEffectEx", 10,
        (uint32_t[]){ dev, A(src), srclen, A(macros), 0, 0, 0, 0, A(pEx), A(pErr) }) == D3D_OK && *pEx);
    uint32_t *pComp = (uint32_t *)hle_alloc(4);
    *pComp = 0;
    ok("CreateEffectCompiler from source", call("D3DXCreateEffectCompiler", 7,
        (uint32_t[]){ A(src), srclen, A(macros), 0, 0, A(pComp), A(pErr) }) == D3D_OK && *pComp);
    ok("CreateEffectCompiler(NULL) is invalid", call("D3DXCreateEffectCompiler", 7,
        (uint32_t[]){ 0, 0, 0, 0, 0, A(pComp), A(pErr) }) == 0x8876086Cu);

    printf(fails == 0 ? "test_d3d9_fx_effect: OK\n" : "test_d3d9_fx_effect: FAILED\n");
    return fails != 0;
}
