#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_line.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

typedef struct { float x,y,z,rhw; uint32_t color; } vert_t;
static struct {
    int calls;
    uint32_t dev;
    uint32_t primType;
    uint32_t primCount;
    uint32_t pData;
    uint32_t stride;
    vert_t verts[16];
    uint32_t vertCount;
} g_dpu;

static void mock_DrawPrimitiveUP(void) {
    /* (this, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride) */
    g_dpu.calls++;
    g_dpu.dev = A32(0);
    g_dpu.primType = A32(1);
    g_dpu.primCount = A32(2);
    g_dpu.pData = A32(3);
    g_dpu.stride = A32(4);
    g_dpu.vertCount = 0;
    if (g_dpu.pData && g_dpu.stride == sizeof(vert_t)) {
        uint32_t n = g_dpu.primCount + 1; /* LINESTRIP */
        if (n > 16) n = 16;
        const vert_t *src = (const vert_t *)hle_ptr(g_dpu.pData);
        for (uint32_t i = 0; i < n; i++) g_dpu.verts[g_dpu.vertCount++] = src[i];
    }
    RET(D3D_OK, 5);
}
static const win32hle_shim mock_shims[] = {
    { "IDirect3DDevice9::DrawPrimitiveUP", mock_DrawPrimitiveUP },
    { 0, 0 }
};

static uint32_t create_device(void) {
    uint32_t a1[1] = { 32u };
    uint32_t d3d9 = call("Direct3DCreate9", 1, a1);
    if (!d3d9) return 0;
    void *pp_h = hle_alloc(sizeof(D3DPRESENT_PARAMETERS));
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)pp_h;
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = 64;
    pp->BackBufferHeight = 64;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->Windowed = 1;
    void *out_h = hle_alloc(4);
    uint32_t a[7] = { d3d9, 0, D3DDEVTYPE_HAL, 0, 0x44u, hle_va(pp_h), hle_va(out_h) };
    if (call("IDirect3D9::CreateDevice", 7, a) != D3D_OK) return 0;
    uint32_t dev;
    memcpy(&dev, out_h, 4);
    return dev;
}

int main(void) {
    recomp_host_init();
    /* Register mock first so hle_resolve finds it before d3d9's real one. */
    win32hle_register(mock_shims);
    win32hle_register(win32hle_d3dx9);
    win32hle_register(win32hle_d3d9);

    uint32_t dev = create_device();
    ok("create device", dev != 0);

    /* ---- CreateLine ABI (2 args) ---- */
    uint32_t *ppl = (uint32_t *)hle_alloc(4); *ppl = 0;
    uint32_t hr = call("D3DXCreateLine", 2, (uint32_t[]){dev, hle_va(ppl)});
    ok("D3DXCreateLine hr", hr == D3D_OK);
    ok("D3DXCreateLine out", *ppl != 0);
    uint32_t line = *ppl;

    /* ---- GetDevice ---- */
    uint32_t *pdev_out = (uint32_t *)hle_alloc(4); *pdev_out = 0;
    ok("GetDevice hr", call("ID3DXLine::GetDevice", 2, (uint32_t[]){line, hle_va(pdev_out)}) == D3D_OK);
    ok("GetDevice value", *pdev_out == dev);

    /* ---- width/pattern/patternscale/antialias/gllines ---- */
    ok("default width", u2f(call("ID3DXLine::GetWidth", 1, (uint32_t[]){line})) == 1.0f);
    ok("SetWidth", call("ID3DXLine::SetWidth", 2, (uint32_t[]){line, f2u(3.5f)}) == D3D_OK);
    ok("GetWidth", u2f(call("ID3DXLine::GetWidth", 1, (uint32_t[]){line})) == 3.5f);
    ok("default pattern", call("ID3DXLine::GetPattern", 1, (uint32_t[]){line}) == 0xFFFFFFFFu);
    ok("SetPattern", call("ID3DXLine::SetPattern", 2, (uint32_t[]){line, 0xAABBCCDDu}) == D3D_OK);
    ok("GetPattern", call("ID3DXLine::GetPattern", 1, (uint32_t[]){line}) == 0xAABBCCDDu);
    ok("SetPatternScale", call("ID3DXLine::SetPatternScale", 2, (uint32_t[]){line, f2u(2.0f)}) == D3D_OK);
    ok("GetPatternScale", u2f(call("ID3DXLine::GetPatternScale", 1, (uint32_t[]){line})) == 2.0f);
    ok("SetAntialias", call("ID3DXLine::SetAntialias", 2, (uint32_t[]){line, 1}) == D3D_OK);
    ok("GetAntialias", call("ID3DXLine::GetAntialias", 1, (uint32_t[]){line}) == 1);
    ok("SetGLLines", call("ID3DXLine::SetGLLines", 2, (uint32_t[]){line, 1}) == D3D_OK);
    ok("GetGLLines", call("ID3DXLine::GetGLLines", 1, (uint32_t[]){line}) == 1);

    /* ---- Begin/End ---- */
    ok("Begin", call("ID3DXLine::Begin", 1, (uint32_t[]){line}) == D3D_OK);
    ok("End", call("ID3DXLine::End", 1, (uint32_t[]){line}) == D3D_OK);

    /* ---- Draw: LINESTRIP, color, stride, vertex count ---- */
    memset(&g_dpu, 0, sizeof g_dpu);
    D3DXVECTOR2 *v2 = (D3DXVECTOR2 *)hle_alloc(3 * sizeof(D3DXVECTOR2));
    v2[0].x=10; v2[0].y=20; v2[1].x=30; v2[1].y=40; v2[2].x=50; v2[2].y=60;
    hr = call("ID3DXLine::Draw", 4, (uint32_t[]){line, hle_va(v2), 3, 0xFFFF1122u});
    ok("Draw hr", hr == D3D_OK);
    ok("Draw called DPU", g_dpu.calls >= 1);
    ok("Draw LINESTRIP", g_dpu.primType == 3u); /* D3DPT_LINESTRIP */
    ok("Draw primCount count-1", g_dpu.primCount == 2u);
    ok("Draw stride 20", g_dpu.stride == 20u);
    ok("Draw vertCount 3", g_dpu.vertCount == 3u);
    ok("Draw v0.x", fabsf(g_dpu.verts[0].x - 10.0f) < 1e-4f);
    ok("Draw v0.y", fabsf(g_dpu.verts[0].y - 20.0f) < 1e-4f);
    ok("Draw v1.x", fabsf(g_dpu.verts[1].x - 30.0f) < 1e-4f);
    ok("Draw v2.x", fabsf(g_dpu.verts[2].x - 50.0f) < 1e-4f);
    ok("Draw color", g_dpu.verts[0].color == 0xFFFF1122u && g_dpu.verts[1].color == 0xFFFF1122u);
    ok("Draw rhw 1", fabsf(g_dpu.verts[0].rhw - 1.0f) < 1e-4f);

    /* ---- DrawTransform: row-vector matrix, non-identity ---- */
    memset(&g_dpu, 0, sizeof g_dpu);
    D3DXVECTOR3 *v3 = (D3DXVECTOR3 *)hle_alloc(2 * sizeof(D3DXVECTOR3));
    v3[0].x=1; v3[0].y=2; v3[0].z=3;
    v3[1].x=4; v3[1].y=5; v3[1].z=6;
    D3DXMATRIX *m = (D3DXMATRIX *)hle_alloc(sizeof(D3DXMATRIX));
    memset(m, 0, sizeof(*m));
    /* Non-identity matrix: scale + translate + projection-ish w */
    m->_11=2; m->_12=0; m->_13=0; m->_14=0;
    m->_21=0; m->_22=3; m->_23=0; m->_24=0;
    m->_31=0; m->_32=0; m->_33=1; m->_34=0;
    m->_41=10; m->_42=20; m->_43=0; m->_44=2; /* w' = 2 for any point */
    hr = call("ID3DXLine::DrawTransform", 5, (uint32_t[]){line, hle_va(v3), 2, hle_va(m), 0xFF00FF00u});
    ok("DrawTransform hr", hr == D3D_OK);
    ok("DT called DPU", g_dpu.calls >= 1);
    ok("DT LINESTRIP", g_dpu.primType == 3u);
    ok("DT primCount", g_dpu.primCount == 1u);
    /* v0: x'=1*2+10=12, y'=2*3+20=26, w'=2 -> 6,13 */
    ok("DT v0.x", fabsf(g_dpu.verts[0].x - 6.0f) < 1e-3f);
    ok("DT v0.y", fabsf(g_dpu.verts[0].y - 13.0f) < 1e-3f);
    /* v1: x'=4*2+10=18, y'=5*3+20=35, w'=2 -> 9,17.5 */
    ok("DT v1.x", fabsf(g_dpu.verts[1].x - 9.0f) < 1e-3f);
    ok("DT v1.y", fabsf(g_dpu.verts[1].y - 17.5f) < 1e-3f);
    ok("DT color", g_dpu.verts[0].color == 0xFF00FF00u);

    /* ---- OnLostDevice/OnResetDevice ---- */
    ok("OnLostDevice", call("ID3DXLine::OnLostDevice", 1, (uint32_t[]){line}) == D3D_OK);
    ok("OnResetDevice", call("ID3DXLine::OnResetDevice", 1, (uint32_t[]){line}) == D3D_OK);

    /* ---- Draw with count < 2 is no-op ---- */
    memset(&g_dpu, 0, sizeof g_dpu);
    ok("Draw count 1", call("ID3DXLine::Draw", 4, (uint32_t[]){line, hle_va(v2), 1, 0xFFFFFFFFu}) == D3D_OK);
    ok("Draw count 1 no DPU", g_dpu.calls == 0);

    /* ---- COM checklist: walk EVERY guest-vtable slot, check esp, count slots ----
     * Order and signatures come from the public d3dx9core.h ID3DXLine
     * declaration (Wine/ReactOS sdk/include/dxsdk/d3dx9core.h). Calling by
     * name through hle_call_guest would hide a stack imbalance because it
     * saves/restores esp, so each slot is entered directly instead. */
    {
        static const char *const names[20] = {
            "QueryInterface", "AddRef", "Release", "GetDevice", "Begin",
            "Draw", "DrawTransform", "SetPattern", "GetPattern",
            "SetPatternScale", "GetPatternScale", "SetWidth", "GetWidth",
            "SetAntialias", "GetAntialias", "SetGLLines", "GetGLLines",
            "End", "OnLostDevice", "OnResetDevice"
        };
        /* stdcall argc INCLUDING `this`, from the public header */
        static const int argc[20] = {
            3, 1, 1, 2, 1, 4, 5, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 1, 1, 1
        };
        uint32_t vt = MEM32(line);
        int nonzero = 0;
        for (int i = 0; i < 20; i++) if (MEM32(vt + 4u * (uint32_t)i)) nonzero++;
        ok("vtable has all 20 ID3DXLine slots", nonzero == 20);
        ok("vtable ends after 20 slots", MEM32(vt + 4u * 20u) == 0u);

        for (int i = 0; i < 20; i++) {
            uint32_t a[6] = { 0, 0, 0, 0, 0, 0 };
            a[0] = line;                                   /* this */
            uint32_t *scratch = (uint32_t *)hle_alloc(4);   /* guest out-pointer */
            if (i == 0 || i == 3) a[i == 0 ? 2 : 1] = hle_va(scratch);
            else if (i == 5 || i == 6) { a[1] = 0; a[2] = 2; a[3] = 0xFFFF0000u; }
            else if (i == 7) a[1] = 0xAABBCCDDu;
            else if (i == 9 || i == 11) a[1] = f2u(2.0f);
            else if (i == 13 || i == 15) a[1] = 1;

            uint32_t fva = MEM32(vt + 4u * (uint32_t)i);
            int called = 0;
            uint32_t delta = 0;
            if (fva) {
                mach_enter();
                uint32_t esp0 = g_esp;
                for (int k = argc[i] - 1; k >= 0; k--) PUSH32(g_esp, a[k]);
                PUSH32(g_esp, RECOMP_RETADDR);
                recomp_func_t f = recomp_lookup_import(fva);
                if (f) { called = 1; f(); }
                delta = g_esp - esp0;
                g_esp = esp0;
                mach_leave();
            }
            char desc[96];
            snprintf(desc, sizeof desc, "slot %d %s: called + esp restored (argc %d)",
                     i, names[i], argc[i]);
            ok(desc, called && delta == 0u);
        }
    }

    if (!fails) printf("d3dx9_line_selftest: all checks passed\n");
    return fails != 0;
}
