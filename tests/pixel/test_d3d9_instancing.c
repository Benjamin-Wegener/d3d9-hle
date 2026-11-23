/* Indexed instancing with SetStreamSourceFreq on real Metal. Public docs
 * (IDirect3DDevice9::SetStreamSourceFreq + "Efficiently Drawing Multiple
 * Instances of Geometry"): stream 0 carries INDEXEDDATA|instanceCount, a
 * stream > 0 carries INSTANCEDATA|divisor, and DrawIndexedPrimitive draws all
 * instances in one call. This test uses a vs_3_0 shader that adds the
 * per-instance attribute v1 to the geometry position v0. Independent expected
 * colours: the base triangle covers the left sample, the +0.5-NDC-shifted
 * copy covers the right sample; both must be red after a single draw. Without
 * instancing only one copy exists and one check fails. */
#include "test_d3d9_common.h"

#define W 32
#define H 32

#define D3DSTREAMSOURCE_INDEXEDDATA  0x40000000u
#define D3DSTREAMSOURCE_INSTANCEDATA 0x80000000u

/* D3D9 operand tokens (Microsoft layout, as used in the other shader test). */
#define REG(t, n)  ((((uint32_t)(t) & 7u) << 28u) | ((((uint32_t)(t) >> 3u) & 3u) << 11u) | (uint32_t)(n))
#define DST(t, n)  (0x80000000u | REG(t, n) | (0xFu << 16u))
#define SRC(t, n)  (REG(t, n) | (0xE4u << 16u))
#define VS_POS  DST(4, 0)   /* oPos */
#define PS_COL  DST(8, 0)   /* oC0 */
#define V0      SRC(1, 0)   /* v0 = geometry position (stream 0) */
#define V1      SRC(1, 1)   /* v1 = per-instance offset (stream 1) */
#define C0      SRC(2, 0)   /* c0 = constant colour */
#define END     0x0000FFFFu

static uint32_t make_shader(uint32_t dev, int is_ps, const uint32_t *tokens, unsigned n) {
    uint32_t *guest = (uint32_t *)hle_alloc(n * 4);
    memcpy(guest, tokens, n * 4);
    uint32_t *out = (uint32_t *)hle_alloc(4);
    uint32_t hr = call(is_ps ? "IDirect3DDevice9::CreatePixelShader"
                             : "IDirect3DDevice9::CreateVertexShader",
                       3, (uint32_t[]){ dev, A(guest), A(out) });
    ok(is_ps ? "CreatePixelShader" : "CreateVertexShader", hr == D3D_OK && *out);
    return *out;
}

static void set_decl(uint32_t dev) {
    /* D3DVERTEXELEMENT9 is 8 bytes: Stream(u16) Offset(u16) Type(u8)
     * Method(u8) Usage(u8) UsageIndex(u8). Declaration order maps to vN. */
    uint8_t *e = (uint8_t *)hle_alloc(sizeof(D3DVERTEXELEMENT9) * 3);
    memset(e, 0, sizeof(D3DVERTEXELEMENT9) * 3);
    /* element 0: POSITION0, FLOAT4, stream 0, offset 0 -> attribute 0 (v0) */
    e[4] = 3; e[6] = 0;
    /* element 1: TEXCOORD0, FLOAT4, stream 1, offset 0 -> attribute 1 (v1) */
    uint8_t *e1 = e + 8;
    ((uint16_t *)e1)[0] = 1; ((uint16_t *)e1)[1] = 0;
    e1[4] = 3; e1[6] = 5;
    /* element 2: D3DDECL_END */
    uint8_t *e2 = e + 16;
    ((uint16_t *)e2)[0] = 0xFF; e2[4] = 17;
    uint32_t *pDecl = (uint32_t *)hle_alloc(4);
    ok("CreateVertexDeclaration",
       call("IDirect3DDevice9::CreateVertexDeclaration", 3,
            (uint32_t[]){ dev, A(e), A(pDecl) }) == D3D_OK && *pDecl);
    call("IDirect3DDevice9::SetVertexDeclaration", 2, (uint32_t[]){ dev, *pDecl });
}

static uint32_t make_buffer(uint32_t dev, const void *data, uint32_t bytes, const char *name) {
    uint32_t *pB = (uint32_t *)hle_alloc(4);
    ok(name, call("IDirect3DDevice9::CreateVertexBuffer", 7,
                  (uint32_t[]){ dev, bytes, 0, 0, D3DPOOL_MANAGED, A(pB), 0 }) == D3D_OK && *pB);
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pB, 0, bytes, A(pData), 0 });
    memcpy(hle_ptr(*pData), data, bytes);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pB });
    return *pB;
}

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    /* add oPos, v0, v1 : instance offset v1 shifts geometry v0. */
    uint32_t vs_tok[6] = { 0xFFFE0300u, 0x03000002u, VS_POS, V0, V1, END };
    uint32_t ps_tok[5] = { 0xFFFF0300u, 0x02000001u, PS_COL, C0, END };
    uint32_t vs = make_shader(dev, 0, vs_tok, 6);
    uint32_t ps = make_shader(dev, 1, ps_tok, 5);
    call("IDirect3DDevice9::SetVertexShader", 2, (uint32_t[]){ dev, vs });
    call("IDirect3DDevice9::SetPixelShader", 2, (uint32_t[]){ dev, ps });
    set_decl(dev);

    float red[4] = { 1, 0, 0, 1 };
    void *red_g = G_copy(red, sizeof red);
    call("IDirect3DDevice9::SetPixelShaderConstantF", 4,
         (uint32_t[]){ dev, 0, A(red_g), 1 });

    /* Geometry: a triangle with base (6,6)-(18,6) and apex (12,18) in pixels.
     * At py=12 it spans px 9..15; the +8px copy spans px 17..23. */
    /* Translated-shader ABI: 16 float4 per vertex = 256-byte stride. */
    float geom[3][64] = { { { 0 } } };
    Vtx g0 = vertex(6, 6, 0.5f, 0, 0, 0, 1);
    Vtx g1 = vertex(18, 6, 0.5f, 0, 0, 0, 1);
    Vtx g2 = vertex(12, 18, 0.5f, 0, 0, 0, 1);
    geom[0][0] = g0.x; geom[0][1] = g0.y; geom[0][2] = g0.z; geom[0][3] = 1;
    geom[1][0] = g1.x; geom[1][1] = g1.y; geom[1][2] = g1.z; geom[1][3] = 1;
    geom[2][0] = g2.x; geom[2][1] = g2.y; geom[2][2] = g2.z; geom[2][3] = 1;
    uint32_t geom_vb = make_buffer(dev, geom, sizeof geom, "CreateVertexBuffer(geometry)");

    /* Two instances: no shift, and +0.5 NDC (8 pixels) to the right. */
    float inst[2][4] = { { 0, 0, 0, 0 }, { 0.5f, 0, 0, 0 } };
    uint32_t inst_vb = make_buffer(dev, inst, sizeof inst, "CreateVertexBuffer(instances)");

    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, geom_vb, 0, 256 });
    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 1, inst_vb, 0, 16 });

    uint32_t *pIB = (uint32_t *)hle_alloc(4);
    ok("CreateIndexBuffer",
       call("IDirect3DDevice9::CreateIndexBuffer", 7,
            (uint32_t[]){ dev, 6, 0, 101 /* D3DFMT_INDEX16 */, 1, A(pIB), 0 }) == D3D_OK && *pIB);
    uint32_t *pIdx = (uint32_t *)hle_alloc(4);
    call("IDirect3DIndexBuffer9::Lock", 5, (uint32_t[]){ *pIB, 0, 6, A(pIdx), 0 });
    uint16_t indices[3] = { 0, 1, 2 };
    memcpy(hle_ptr(*pIdx), indices, sizeof indices);
    call("IDirect3DIndexBuffer9::Unlock", 1, (uint32_t[]){ *pIB });
    call("IDirect3DDevice9::SetIndices", 2, (uint32_t[]){ dev, *pIB });

    /* Public frequency constants: INDEXEDDATA|2 on stream 0, INSTANCEDATA|1
     * on stream 1. */
    ok("SetStreamSourceFreq(0, INDEXEDDATA|2)",
       call("IDirect3DDevice9::SetStreamSourceFreq", 3,
            (uint32_t[]){ dev, 0, D3DSTREAMSOURCE_INDEXEDDATA | 2u }) == D3D_OK);
    ok("SetStreamSourceFreq(1, INSTANCEDATA|1)",
       call("IDirect3DDevice9::SetStreamSourceFreq", 3,
            (uint32_t[]){ dev, 1, D3DSTREAMSOURCE_INSTANCEDATA | 1u }) == D3D_OK);

    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    ok("DrawIndexedPrimitive(instanced)",
       call("IDirect3DDevice9::DrawIndexedPrimitive", 7,
            (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 }) == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[W * H];
    ok("Metal readback", hle_d3d9_metal_read_pixels(0, 0, W, H, pixels, W * 4) == 0);
    ok("instance 0 covers left sample",  pixels[12 * W + 12] == 0xFFFF0000u);
    ok("instance 1 covers right sample", pixels[12 * W + 20] == 0xFFFF0000u);
    if (pixels[12 * W + 12] != 0xFFFF0000u || pixels[12 * W + 20] != 0xFFFF0000u)
        printf("    got left=0x%08X right=0x%08X\n",
               pixels[12 * W + 12], pixels[12 * W + 20]);

    /* Unsupported cases must report an error, never a false OK. */
    ok("SetStreamSourceFreq(1, INDEXEDDATA) rejected",
       call("IDirect3DDevice9::SetStreamSourceFreq", 3,
            (uint32_t[]){ dev, 1, D3DSTREAMSOURCE_INDEXEDDATA | 2u }) != D3D_OK);
    ok("SetStreamSourceFreq(0, INSTANCEDATA) rejected",
       call("IDirect3DDevice9::SetStreamSourceFreq", 3,
            (uint32_t[]){ dev, 0, D3DSTREAMSOURCE_INSTANCEDATA | 1u }) != D3D_OK);
    ok("SetStreamSourceFreq(2, reserved bits) rejected",
       call("IDirect3DDevice9::SetStreamSourceFreq", 3,
            (uint32_t[]){ dev, 2, 0xC0000001u }) != D3D_OK);

    /* Non-indexed instancing is unsupported: must not report D3D_OK. */
    ok("non-indexed DrawPrimitive with instancing rejected",
       call("IDirect3DDevice9::DrawPrimitive", 4,
            (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 1 }) != D3D_OK);

    /* TRIANGLEFAN is unsupported in the indexed instancing path. */
    ok("TRIANGLEFAN instancing rejected",
       call("IDirect3DDevice9::DrawIndexedPrimitive", 7,
            (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 0, 3, 0, 1 }) != D3D_OK);

    printf(fails == 0 ? "test_d3d9_instancing: OK\n" : "test_d3d9_instancing: FAILED\n");
    return fails != 0;
}
