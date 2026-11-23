/*
 * d3dx9_consttable_selftest.c - the four D3DX shader helpers and the
 * ID3DXConstantTable object, checked against a self-built shader carrying a
 * 'CTAB' comment in the public d3dx9shader.h layout: a 7-DWORD
 * D3DXSHADER_CONSTANTTABLE, D3DXSHADER_CONSTANTINFO records at ConstantInfo,
 * D3DXSHADER_TYPEINFO records at each record's TypeInfo, and all names,
 * creator, target and defaults as BYTE OFFSETS from the table start.
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3dx9_hle.h"
#include "d3d9_hle.h"
#include "d3dx9_consttable.h"

#define ENTRY_VA 0x00401000u
#define TAB_OFF  12u                 /* version, comment token, fourcc */
#define TAB_BYTES 160u

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}

static uint32_t esp0;
#define XCALL(va, n, ...)                                                    \
    do {                                                                     \
        uint32_t _xa[8] = { __VA_ARGS__ };                                   \
        esp0 = esp;                                                          \
        for (int _xi = (int)(n) - 1; _xi >= 0; _xi--) PUSH32(esp, _xa[_xi]); \
        RECOMP_ICALL(va);                                                    \
        ok(#va " stack", esp == esp0);                                       \
    } while (0)

static void put16(uint8_t *b, uint32_t o, uint16_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t o, uint32_t v) {
    put16(b, o, (uint16_t)v); put16(b, o + 2u, (uint16_t)(v >> 16));
}

static uint32_t V_ver, V_size, V_find, V_ct;

/* Genuine compiler output: self-written ps_2_0 shader from
 * a small hand-written HLSL pixel shader compiled to ps_2_0 with vkd3d-shader, as a
 * token array. This is NOT a hand-built table - it checks the real layout. */
static const uint32_t s_real_ps20[30] = {
    0xFFFF0200u, 0x0018FFFEu, 0x42415443u, 0x0000001Cu, 0x00000048u,
    0xFFFF0200u, 0x00000001u, 0x0000001Cu, 0x00000000u, 0x00000000u,
    0x00000030u, 0x00000002u, 0x00000001u, 0x00000038u, 0x00000000u,
    0x746E6974u, 0xABABAB00u, 0x00030001u, 0x00040001u, 0x00000001u,
    0x00000001u, 0x33646B76u, 0x68732D64u, 0x72656461u, 0x312E3220u,
    0xABABAB00u, 0x02000001u, 0x800F0800u, 0xA0E40000u, 0x0000FFFFu
};
static uint32_t g_real;

static uint32_t g_dev, g_shader, g_shader_size, g_out, g_pdata, g_psize, g_desc, g_cnt;

/* Table-relative offsets, all verified against the public structures. */
#define OFF_CREATOR 100u
#define OFF_TARGET  109u
#define OFF_NAME0   116u
#define OFF_NAME1   130u
#define OFF_DEF1    140u
#define OFF_CINFO0   28u
#define OFF_CINFO1   48u
#define OFF_TINFO0   68u
#define OFF_TINFO1   84u

static uint32_t build_shader(uint32_t *size_out) {
    uint32_t va = hle_va(hle_alloc(2048));
    uint8_t *b = (uint8_t *)hle_ptr(va);
    uint8_t *t = b + TAB_OFF;
    uint32_t total = TAB_OFF + TAB_BYTES + 4u;        /* + END token */
    uint32_t comment_dwords = 1u + TAB_BYTES / 4u;    /* fourcc + payload */

    memset(b, 0, 2048);
    put32(b, 0, D3DXSHADER_VERSION(2, 0));
    put32(b, 4, ((uint32_t)comment_dwords << 16) | D3DSIO_COMMENT);
    put32(b, 8, CTAB_FOURCC);

    /* D3DXSHADER_CONSTANTTABLE (28 bytes). */
    put32(t, 0, sizeof(D3DXSHADER_CONSTANTTABLE));
    put32(t, 4, OFF_CREATOR);
    put32(t, 8, D3DXSHADER_VERSION(2, 0));
    put32(t, 12, 2u);                                /* Constants */
    put32(t, 16, OFF_CINFO0);                        /* ConstantInfo */
    put32(t, 20, 0u);                                /* Flags */
    put32(t, 24, OFF_TARGET);

    /* D3DXSHADER_CONSTANTINFO[0]: WorldViewProj, matrix c0, 4 registers. */
    put32(t, OFF_CINFO0 + 0, OFF_NAME0);
    put16(t, OFF_CINFO0 + 4, 0u);                    /* RegisterSet */
    put16(t, OFF_CINFO0 + 6, 0u);                    /* RegisterIndex */
    put16(t, OFF_CINFO0 + 8, 4u);                    /* RegisterCount */
    put16(t, OFF_CINFO0 + 10, 0u);                   /* Reserved */
    put32(t, OFF_CINFO0 + 12, OFF_TINFO0);
    put32(t, OFF_CINFO0 + 16, 0u);                   /* DefaultValue: absent */

    /* D3DXSHADER_CONSTANTINFO[1]: BaseColor, vector c17, one register. */
    put32(t, OFF_CINFO1 + 0, OFF_NAME1);
    put16(t, OFF_CINFO1 + 4, 0u);
    put16(t, OFF_CINFO1 + 6, 17u);
    put16(t, OFF_CINFO1 + 8, 1u);
    put16(t, OFF_CINFO1 + 10, 0u);
    put32(t, OFF_CINFO1 + 12, OFF_TINFO1);
    put32(t, OFF_CINFO1 + 16, OFF_DEF1);             /* default value present */

    /* D3DXSHADER_TYPEINFO[0]: matrix, 4x4. */
    put16(t, OFF_TINFO0 + 0, 2u);                    /* Class: matrix columns */
    put16(t, OFF_TINFO0 + 2, 3u);                    /* Type: D3DXPT_FLOAT */
    put16(t, OFF_TINFO0 + 4, 4u);                    /* Rows */
    put16(t, OFF_TINFO0 + 6, 4u);                    /* Columns */
    put16(t, OFF_TINFO0 + 8, 1u);                    /* Elements */
    put16(t, OFF_TINFO0 + 10, 0u);                   /* StructMembers */
    put32(t, OFF_TINFO0 + 12, 0u);

    /* D3DXSHADER_TYPEINFO[1]: vector, 1x4. */
    put16(t, OFF_TINFO1 + 0, 0u);                    /* scalar class */
    put16(t, OFF_TINFO1 + 2, 3u);                    /* Type: D3DXPT_FLOAT */
    put16(t, OFF_TINFO1 + 4, 1u);
    put16(t, OFF_TINFO1 + 6, 4u);
    put16(t, OFF_TINFO1 + 8, 1u);
    put16(t, OFF_TINFO1 + 10, 0u);
    put32(t, OFF_TINFO1 + 12, 0u);

    /* Strings and the default value, all as offsets from the table start. */
    memcpy(t + OFF_CREATOR, "Selftest", 9);
    memcpy(t + OFF_TARGET, "vs_2_0", 7);
    memcpy(t + OFF_NAME0, "WorldViewProj", 14);
    memcpy(t + OFF_NAME1, "BaseColor", 10);
    memset(t + OFF_DEF1, 0x3F, 16);                  /* a float4 default */

    put32(b, TAB_OFF + TAB_BYTES, D3DSIO_END);
    *size_out = total;
    return va;
}

static void guest(void) {
    uint32_t _cf = 0; uint32_t _flag_k = FK_NONE; uint32_t _flag_a = 0, _flag_b = 0;
    uint32_t data, dsize, obj, vtbl, tabva;
    uint32_t desc[12], h0, h1, sidx;
    RECOMP_ENTER(ENTRY_VA);

    XCALL(V_ver, 1, g_shader);
    ok("GetShaderVersion", eax == D3DXSHADER_VERSION(2, 0));

    XCALL(V_size, 1, g_shader);
    ok("GetShaderSize", eax == g_shader_size);

    XCALL(V_find, 4, g_shader, CTAB_FOURCC, g_pdata, g_psize);
    ok("FindShaderComment CTAB", eax == D3D_OK);
    data = MEM32(g_pdata);
    dsize = MEM32(g_psize);
    tabva = g_shader + TAB_OFF;
    ok("FindShaderComment data is the table start", data == tabva);
    ok("FindShaderComment payload size", dsize == TAB_BYTES);

    XCALL(V_find, 4, g_shader, 0x11223344u, g_pdata, g_psize);
    ok("FindShaderComment miss", eax == E_FAIL);

    XCALL(V_ct, 2, g_shader, g_out);
    ok("GetShaderConstantTable", eax == D3D_OK);
    obj = MEM32(g_out);
    ok("constant table object", obj != 0u);
    if (!obj) return;
    vtbl = MEM32(obj);

    /* GetDesc (slot 5). The documented Creator field is an offset in the
     * table; the returned DESC carries a guest VA. */
    XCALL(MEM32(vtbl + 5 * 4), 2, obj, g_desc);
    ok("GetDesc", eax == D3D_OK);
    ok("GetDesc.Creator == table+OFF_CREATOR", MEM32(g_desc) == tabva + OFF_CREATOR);
    ok("GetDesc.Version", MEM32(g_desc + 4) == D3DXSHADER_VERSION(2, 0));
    ok("GetDesc.Constants", MEM32(g_desc + 8) == 2u);

    XCALL(MEM32(vtbl + 9 * 4), 3, obj, 0u, tabva + OFF_NAME1);
    h1 = eax;
    ok("GetConstantByName BaseColor", h1 == 2u);

    XCALL(MEM32(vtbl + 8 * 4), 3, obj, 0u, 0u);
    h0 = eax;
    ok("GetConstant index 0", h0 == 1u);

    XCALL(MEM32(vtbl + 10 * 4), 3, obj, 1u, 1u);
    ok("GetConstantElement index 1", eax == 2u);

    /* GetConstantDesc (slot 6), D3DXCONSTANT_DESC = 12 DWORDs. */
    XCALL(MEM32(vtbl + 6 * 4), 4, obj, 2u, g_desc, g_cnt);
    ok("GetConstantDesc BaseColor", eax == D3D_OK && MEM32(g_cnt) == 1u);
    memcpy(desc, hle_ptr(g_desc), sizeof desc);
    ok("GetConstantDesc.Name offset resolved", desc[0] == tabva + OFF_NAME1);
    ok("GetConstantDesc.RegisterSet", desc[1] == 0u);
    ok("GetConstantDesc.RegisterIndex (from cinfo)", desc[2] == 17u);
    ok("GetConstantDesc.RegisterCount", desc[3] == 1u);
    ok("GetConstantDesc.Class (from tinfo)", desc[4] == 0u);
    ok("GetConstantDesc.Type (from tinfo)", desc[5] == 3u);
    ok("GetConstantDesc.Rows (from tinfo)", desc[6] == 1u);
    ok("GetConstantDesc.Columns (from tinfo)", desc[7] == 4u);
    ok("GetConstantDesc.Elements", desc[8] == 1u);
    ok("GetConstantDesc.StructMembers", desc[9] == 0u);
    ok("GetConstantDesc.Bytes (1x4 float)", desc[10] == 16u);
    ok("GetConstantDesc.DefaultValue offset resolved", desc[11] == tabva + OFF_DEF1);

    /* Handle 1 reads the 4x4 matrix TYPEINFO at a different offset. */
    XCALL(MEM32(vtbl + 6 * 4), 4, obj, 1u, g_desc, g_cnt);
    memcpy(desc, hle_ptr(g_desc), sizeof desc);
    ok("matrix Name offset resolved", desc[0] == tabva + OFF_NAME0);
    ok("matrix Class from its own TYPEINFO", desc[4] == 2u);
    ok("matrix Rows/Columns", desc[6] == 4u && desc[7] == 4u);
    ok("matrix Bytes == 64", desc[10] == 64u);
    ok("matrix has no DefaultValue", desc[11] == 0u);

    XCALL(MEM32(vtbl + 7 * 4), 2, obj, 1u);
    sidx = eax;
    ok("GetSamplerIndex", sidx == 0u);

    /* SetDefaults must actually write the CTAB default (0x3F3F3F3F at
     * OFF_DEF1) into the device's F register at BaseColor's index c17. */
    uint32_t zero_c = hle_va(hle_alloc(16));
    memset(hle_ptr(zero_c), 0, 16);
    XCALL(hle_resolve("IDirect3DDevice9::SetVertexShaderConstantF"), 4,
          g_dev, 17u, zero_c, 1u);
    XCALL(MEM32(vtbl + 11 * 4), 2, obj, g_dev);
    ok("SetDefaults", eax == D3D_OK);
    uint32_t got_def = hle_va(hle_alloc(16));
    XCALL(hle_resolve("IDirect3DDevice9::GetVertexShaderConstantF"), 4,
          g_dev, 17u, got_def, 1u);
    ok("SetDefaults wrote the CTAB default to the device",
        MEM32(got_def) == 0x3F3F3F3Fu);
    /* WorldViewProj has no default; its register c0 must stay untouched. */
    uint32_t quarter_c = hle_va(hle_alloc(16));
    memset(hle_ptr(quarter_c), 0, 16);
    float q = 0.25f;
    memcpy(hle_ptr(quarter_c), &q, sizeof q);
    XCALL(hle_resolve("IDirect3DDevice9::SetVertexShaderConstantF"), 4,
          g_dev, 0u, quarter_c, 1u);
    XCALL(MEM32(vtbl + 11 * 4), 2, obj, g_dev);
    ok("SetDefaults again", eax == D3D_OK);
    XCALL(hle_resolve("IDirect3DDevice9::GetVertexShaderConstantF"), 4,
          g_dev, 0u, got_def, 1u);
    {
        float keep;
        memcpy(&keep, hle_ptr(got_def), sizeof keep);
        ok("SetDefaults leaves constants without a default alone", keep == 0.25f);
    }

    /* SetMatrixTransposeArray (slot 25): a 5-argument stdcall method. XCALL
     * checks esp afterwards, so a wrong RET(argc) cannot pass unnoticed. */
    float mats[32] = { 0.0f };
    mats[0] = 1.0f; mats[1] = 2.0f; mats[4] = 3.0f; mats[5] = 4.0f;
    uint32_t mats_va = hle_va(hle_alloc(sizeof mats));
    memcpy(hle_ptr(mats_va), mats, sizeof mats);
    XCALL(MEM32(vtbl + 25 * 4), 5, obj, g_dev, 1u, mats_va, 2u);
    ok("SetMatrixTransposeArray", eax == D3D_OK);
    XCALL(hle_resolve("IDirect3DDevice9::GetVertexShaderConstantF"), 4,
          g_dev, 0u, got_def, 1u);
    {
        float gotm[4];
        memcpy(gotm, hle_ptr(got_def), sizeof gotm);
        ok("SetMatrixTransposeArray transposed the matrix",
            gotm[0] == 1.0f && gotm[1] == 3.0f);
    }
    XCALL(MEM32(vtbl + 25 * 4), 5, obj, g_dev, 1u, 0u, 0u);
    ok("SetMatrixTransposeArray rejects n=0", eax == D3DERR_INVALIDCALL);

    /* ID3DXBuffer methods (slots 3/4): a copy of the CTAB payload. */
    XCALL(MEM32(vtbl + 3 * 4), 1, obj);
    uint32_t buf = eax;
    ok("GetBufferPointer is a copy", buf != 0u && buf != g_shader && buf != tabva);
    XCALL(MEM32(vtbl + 4 * 4), 1, obj);
    ok("GetBufferSize == CTAB payload", eax == TAB_BYTES);
    ok("CTAB copy has the public header size", MEM32(buf) == sizeof(D3DXSHADER_CONSTANTTABLE));

    /* SetFloat (slot 17) forwards to the real device: read c17 back. */
    float fv = 0.75f;
    uint32_t fbits;
    memcpy(&fbits, &fv, sizeof fbits);
    XCALL(MEM32(vtbl + 17 * 4), 4, obj, g_dev, h1, fbits);
    ok("SetFloat", eax == D3D_OK);
    uint32_t got = hle_va(hle_alloc(16));
    XCALL(hle_resolve("IDirect3DDevice9::GetVertexShaderConstantF"), 4, g_dev, 17u, got, 1u);
    float rf;
    memcpy(&rf, hle_ptr(got), sizeof rf);
    ok("SetFloat reached the device constant", rf == 0.75f);

    /* SetBool (slot 13) forwards to the BOOL register file (h0 = c0). */
    XCALL(MEM32(vtbl + 13 * 4), 4, obj, g_dev, h0, 1u);
    ok("SetBool", eax == D3D_OK);
    uint32_t gotb = hle_va(hle_alloc(16));
    XCALL(hle_resolve("IDirect3DDevice9::GetVertexShaderConstantB"), 4, g_dev, 0u, gotb, 1u);
    ok("SetBool reached the device constant", MEM32(gotb) == 1u);


    /* Genuine vkd3d output: the comment is at DWORD 1, no size DWORD. */
    XCALL(V_size, 1, g_real);
    ok("real shader size == 120 bytes", eax == sizeof(s_real_ps20));
    XCALL(V_find, 4, g_real, CTAB_FOURCC, g_pdata, g_psize);
    ok("real shader CTAB found", eax == D3D_OK);
    ok("real shader data = shader+12", MEM32(g_pdata) == g_real + 12u);
    ok("real shader payload size == 92", MEM32(g_psize) == 92u);
    {
        uint32_t out2 = hle_va(hle_alloc(4)), obj2, vt2;
        XCALL(V_ct, 2, g_real, out2);
        obj2 = MEM32(out2);
        ok("real shader constant table", obj2 != 0u);
        if (obj2) {
            vt2 = MEM32(obj2);
            XCALL(MEM32(vt2 + 5 * 4), 2, obj2, g_desc);
            ok("real GetDesc version", MEM32(g_desc + 4) == 0xFFFF0200u);
            ok("real GetDesc constants == 1", MEM32(g_desc + 8) == 1u);
            XCALL(MEM32(vt2 + 6 * 4), 4, obj2, 1u, g_desc, g_cnt);
            memcpy(desc, hle_ptr(g_desc), sizeof desc);
            ok("real constant name offset resolved", desc[0] == g_real + 12u + 0x30u);
        }
    }
    XCALL(MEM32(vtbl + 2 * 4), 1, obj);
    ok("Release", eax == 0u);
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);
    win32hle_register(win32hle_d3d9);
    /* Device method shims are registered by Direct3DCreate9. */
    extern void init_d3d9_vtables(void);
    init_d3d9_vtables();
    V_ver = hle_resolve("D3DXGetShaderVersion");
    V_size = hle_resolve("D3DXGetShaderSize");
    V_find = hle_resolve("D3DXFindShaderComment");
    V_ct = hle_resolve("D3DXGetShaderConstantTable");
    ok("shims resolve", V_ver && V_size && V_find && V_ct);
    ok("device shims resolve", hle_resolve("IDirect3DDevice9::SetVertexShaderConstantF") != 0 && hle_resolve("IDirect3DDevice9::GetVertexShaderConstantF") != 0);

    /* A real device for the Set* forwarding checks. */
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    ok("Direct3DCreate9", d3d9 != 0);
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 32; pp->BackBufferHeight = 32;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t hrd = call("IDirect3D9::CreateDevice", 7, (uint32_t[]){
        d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, 0,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, hle_va(pp), hle_va(pDev) });
    ok("CreateDevice", hrd == D3D_OK && *pDev != 0);
    g_dev = *pDev;
    g_real = hle_va(hle_alloc(sizeof s_real_ps20));
    memcpy(hle_ptr(g_real), s_real_ps20, sizeof s_real_ps20);
    g_shader = build_shader(&g_shader_size);
    g_out = hle_va(hle_alloc(4));
    g_pdata = hle_va(hle_alloc(4));
    g_psize = hle_va(hle_alloc(4));
    g_desc = hle_va(hle_alloc(sizeof(uint32_t) * 16));
    g_cnt = hle_va(hle_alloc(4));

    mach_enter();
    guest();
    mach_leave();

    if (!fails) printf("d3dx9_consttable_selftest: all checks passed\n");
    return fails != 0;
}
