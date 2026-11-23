/* Translated shaders on real Metal: constants in F registers are uploaded to
 * the GPU. Changing c0 changes the pixel - PS c0 is the output colour, VS c0
 * shifts the geometry. Without the constant bridge both draws look the same. */
#include "test_d3d9_common.h"

#define W 32
#define H 32

/* D3D9 operand tokens, Microsoft layout: type in bits 28..30 + 11..12. */
#define REG(t, n)  ((((uint32_t)(t) & 7u) << 28u) | ((((uint32_t)(t) >> 3u) & 3u) << 11u) | (uint32_t)(n))
#define DST(t, n)  (0x80000000u | REG(t, n) | (0xFu << 16u))
#define SRC(t, n)  (REG(t, n) | (0xE4u << 16u))   /* .xyzw */
#define VS_IN   SRC(1, 0)                          /* v0.xyzw */
#define VS_POS  DST(4, 0)                          /* oPos (RASTOUT) */
#define PS_COL  DST(8, 0)                          /* oC0 (COLOROUT) */
#define VS_C0   SRC(2, 0)                          /* c0 */
#define END     0x0000FFFFu

static uint32_t make_shader(uint32_t dev, const char *name, int is_ps, uint32_t mov_dst, uint32_t src) {
    uint32_t tokens[8] = {
        is_ps ? 0xFFFF0300u : 0xFFFE0300u,         /* ps_3_0 / vs_3_0 */
        0x02000001u, mov_dst, src,                  /* mov dst, src */
        END
    };
    uint32_t *guest = (uint32_t *)hle_alloc(sizeof tokens);
    memcpy(guest, tokens, sizeof tokens);
    uint32_t *out = (uint32_t *)hle_alloc(4);
    uint32_t hr = call(is_ps ? "IDirect3DDevice9::CreatePixelShader"
                             : "IDirect3DDevice9::CreateVertexShader",
                       3, (uint32_t[]){ dev, A(guest), A(out) });
    ok(name, hr == D3D_OK && *out);
    return *out;
}

static void draw(uint32_t dev) {
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
}

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    uint32_t vs_pos = make_shader(dev, "CreateVertexShader(mov oPos,v0)", 0, VS_POS, VS_IN);
    uint32_t ps_col = make_shader(dev, "CreatePixelShader(mov oC0,c0)", 1, PS_COL, VS_C0);
    call("IDirect3DDevice9::SetVertexShader", 2, (uint32_t[]){ dev, vs_pos });
    call("IDirect3DDevice9::SetPixelShader", 2, (uint32_t[]){ dev, ps_col });

    /* A minimal declaration: POSITION0 float4 at offset 0, terminated by
     * D3DDECL_END (Stream 0xFF, Type UNUSED/17). */
    uint8_t *e = (uint8_t *)hle_alloc(sizeof(D3DVERTEXELEMENT9) * 2);
    memset(e, 0, sizeof(D3DVERTEXELEMENT9) * 2);
    /* element 0: POSITION0 float4 */
    ((uint16_t *)e)[0] = 0;    /* Stream */
    ((uint16_t *)e)[1] = 0;    /* Offset */
    e[4] = 3;                  /* Type = D3DDECLTYPE_FLOAT4 */
    e[5] = 0;                  /* Method */
    e[6] = 0;                  /* Usage = D3DDECLUSAGE_POSITION */
    e[7] = 0;                  /* UsageIndex */
    /* element 1: D3DDECL_END */
    ((uint16_t *)(e + 8))[0] = 0xFF; /* Stream */
    ((uint16_t *)(e + 8))[1] = 0;    /* Offset */
    e[8 + 4] = 17;             /* Type = D3DDECLTYPE_UNUSED */
    e[8 + 5] = 0;
    e[8 + 6] = 0;
    e[8 + 7] = 0;
    uint32_t *pDecl = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexDeclaration", 3, (uint32_t[]){ dev, A(e), A(pDecl) });
    call("IDirect3DDevice9::SetVertexDeclaration", 2, (uint32_t[]){ dev, *pDecl });

    const float quad[6][2] = { { -1, 1 }, { 1, 1 }, { -1, -1 },
                               { -1, -1 }, { 1, 1 }, { 1, -1 } };
    float *verts = (float *)hle_alloc(sizeof(float) * 6 * 16 * 4);
    memset(verts, 0, sizeof(float) * 6 * 16 * 4);
    for (int v = 0; v < 6; v++) {
        float *q = verts + v * 16 * 4;
        q[0] = quad[v][0]; q[1] = quad[v][1]; q[2] = 0.5f; q[3] = 1.0f;
    }
    uint32_t bytes = (uint32_t)(sizeof(float) * 6 * 16 * 4);
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer", 7,
         (uint32_t[]){ dev, bytes, 0, 0, 1, A(pVB), 0 });
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, bytes, A(pData), 0 });
    memcpy(hle_ptr(*pData), verts, bytes);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });
    call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, *pVB, 0, bytes / 6 });
    call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, 0 });

    /* PS constant c0 is the output colour: change it and the pixel changes. */
    float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };
    void *red_g = G_copy(red, sizeof red), *green_g = G_copy(green, sizeof green);
    uint32_t pixels[W * H];
    call("IDirect3DDevice9::SetPixelShaderConstantF", 4, (uint32_t[]){ dev, 0, A(red_g), 1 });
    clear(dev, 0xFF000000u, 1, 1.0f);
    draw(dev);
    ok("Metal readback 1", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("PS c0 = red", pixels[16 * W + 16] == 0xFFFF0000u);
    if (pixels[16 * W + 16] != 0xFFFF0000u) printf("    got 0x%08X\n", pixels[16 * W + 16]);

    call("IDirect3DDevice9::SetPixelShaderConstantF", 4, (uint32_t[]){ dev, 0, A(green_g), 1 });
    clear(dev, 0xFF000000u, 1, 1.0f);
    draw(dev);
    ok("Metal readback 2", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("changed PS c0 = green", pixels[16 * W + 16] == 0xFF00FF00u);
    if (pixels[16 * W + 16] != 0xFF00FF00u) printf("    got 0x%08X\n", pixels[16 * W + 16]);

    /* VS constant c0 shifts the geometry: add oPos, v0, c0. With c0.x = -1.5
     * the quad moves off the centre pixel; with 0 it covers it again. */
    uint32_t add_tokens[7] = { 0xFFFE0300u, 0x03000002u, VS_POS, VS_IN, VS_C0, END };
    uint32_t *add_guest = (uint32_t *)hle_alloc(sizeof add_tokens);
    memcpy(add_guest, add_tokens, sizeof add_tokens);
    uint32_t *pVS2 = (uint32_t *)hle_alloc(4);
    ok("CreateVertexShader(add oPos,v0,c0)",
       call("IDirect3DDevice9::CreateVertexShader", 3, (uint32_t[]){ dev, A(add_guest), A(pVS2) }) == D3D_OK && *pVS2);
    call("IDirect3DDevice9::SetVertexShader", 2, (uint32_t[]){ dev, *pVS2 });
    float shift[4] = { -1.5f, 0, 0, 0 }, origin[4] = { 0, 0, 0, 0 };
    void *shift_g = G_copy(shift, sizeof shift), *origin_g = G_copy(origin, sizeof origin);
    call("IDirect3DDevice9::SetPixelShaderConstantF", 4, (uint32_t[]){ dev, 0, A(green_g), 1 });

    call("IDirect3DDevice9::SetVertexShaderConstantF", 4, (uint32_t[]){ dev, 0, A(shift_g), 1 });
    clear(dev, 0xFF000000u, 1, 1.0f);
    draw(dev);
    ok("Metal readback 3", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("VS c0 shifted quad away from centre", pixels[16 * W + 16] == 0xFF000000u);
    if (pixels[16 * W + 16] != 0xFF000000u) printf("    got 0x%08X\n", pixels[16 * W + 16]);

    call("IDirect3DDevice9::SetVertexShaderConstantF", 4, (uint32_t[]){ dev, 0, A(origin_g), 1 });
    clear(dev, 0xFF000000u, 1, 1.0f);
    draw(dev);
    ok("Metal readback 4", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("changed VS c0 brings quad back", pixels[16 * W + 16] == 0xFF00FF00u);
    if (pixels[16 * W + 16] != 0xFF00FF00u) printf("    got 0x%08X\n", pixels[16 * W + 16]);


    /* PS integer constant i0 moves into the colour: mov oC0, i0. */
    uint32_t psi[5] = { 0xFFFF0300u, 0x02000001u, PS_COL, SRC(7, 0), END };
    uint32_t *psi_g = (uint32_t *)hle_alloc(sizeof psi);
    memcpy(psi_g, psi, sizeof psi);
    uint32_t *pPS2 = (uint32_t *)hle_alloc(4);
    ok("CreatePixelShader(mov oC0,i0)",
       call("IDirect3DDevice9::CreatePixelShader", 3, (uint32_t[]){ dev, A(psi_g), A(pPS2) }) == D3D_OK && *pPS2);
    call("IDirect3DDevice9::SetPixelShader", 2, (uint32_t[]){ dev, *pPS2 });
    int32_t i_red[4] = { 1, 0, 0, 1 }, i_green[4] = { 0, 1, 0, 1 };
    void *i_red_g = G_copy(i_red, sizeof i_red), *i_green_g = G_copy(i_green, sizeof i_green);
    call("IDirect3DDevice9::SetPixelShaderConstantI", 4, (uint32_t[]){ dev, 0, A(i_red_g), 1 });
    clear(dev, 0xFF000000u, 1, 1.0f);
    draw(dev);
    ok("Metal readback 5", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("PS i0 = red", pixels[16 * W + 16] == 0xFFFF0000u);
    call("IDirect3DDevice9::SetPixelShaderConstantI", 4, (uint32_t[]){ dev, 0, A(i_green_g), 1 });
    clear(dev, 0xFF000000u, 1, 1.0f);
    draw(dev);
    ok("Metal readback 6", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("changed PS i0 = green", pixels[16 * W + 16] == 0xFF00FF00u);

    printf(fails == 0 ? "test_d3d9_shader_constants: OK\n" : "test_d3d9_shader_constants: FAILED\n");
    return fails != 0;
}
