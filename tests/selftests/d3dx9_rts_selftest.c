/*
 * d3dx9_rts_selftest.c - ID3DXRenderToSurface ABI, state and error paths.
 *
 * The nine slots and their order come from the public Wine header
 * include/d3dx9core.h (ID3DXRenderToSurface, slots 0..8). Every slot is
 * called through the guest vtable with a raw push/ret, so a wrong stdcall
 * argc (including `this`) changes the measured esp delta and fails the test.
 * Functional checks use independently known values: saved render target,
 * depth surface and viewport must be restored after EndScene.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_rts.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }

/* Enter a guest vtable slot DIRECTLY: hle_call_guest() saves/restores esp, so
 * its esp delta would always be zero and prove nothing. */
static uint32_t vslot(uint32_t obj, int slot, int argc, const uint32_t *extra,
                      int *called, uint32_t *delta) {
    uint32_t fva = MEM32(MEM32(obj) + 4u * (uint32_t)slot);
    *called = 0; *delta = 0;
    if (!fva) return 0u;
    uint32_t a[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    a[0] = obj;
    for (int k = 1; k < argc; k++) a[k] = extra ? extra[k - 1] : 0u;
    mach_enter();
    uint32_t esp0 = g_esp;
    for (int k = argc - 1; k >= 0; k--) PUSH32(g_esp, a[k]);
    PUSH32(g_esp, RECOMP_RETADDR);
    uint32_t eax = 0;
    recomp_func_t f = recomp_lookup_import(fva);
    if (f) { *called = 1; f(); eax = g_eax; }
    *delta = g_esp - esp0;
    g_esp = esp0;
    mach_leave();
    return eax;
}
static uint32_t mcall(uint32_t obj, int slot, int argc, const uint32_t *extra) {
    int c; uint32_t d;
    return vslot(obj, slot, argc, extra, &c, &d);
}
enum { RTS_SLOTS = 9 };
static const char *const slot_names[RTS_SLOTS] = {
    "dxr_QueryInterface", "dxr_AddRef", "dxr_Release", "dxr_GetDevice",
    "dxr_GetDesc", "dxr_BeginScene", "dxr_EndScene",
    "dxr_OnLostDevice", "dxr_OnResetDevice"
};
/* argc INCLUDES this; source: public ID3DXRenderToSurface declaration. */
static const int slot_argc[RTS_SLOTS] = { 3, 1, 1, 2, 2, 3, 2, 1, 1 };

static uint32_t get_pointer(uint32_t dev, const char *name, int argc) {
    uint32_t p = hle_va(hle_alloc(4));
    MEM32(p) = 0xDEADBEEFu;
    uint32_t hr = argc == 3
        ? call(name, 3, (uint32_t[]){ dev, 0u, p })
        : call(name, 2, (uint32_t[]){ dev, p });
    ok(name, hr == D3D_OK && MEM32(p) != 0u && MEM32(p) != 0xDEADBEEFu);
    return MEM32(p);
}
static void get_vp(uint32_t dev, D3DVIEWPORT9 *vp) {
    uint32_t p = hle_va(hle_alloc(sizeof *vp));
    ok("GetViewport", call("IDirect3DDevice9::GetViewport", 2,
                           (uint32_t[]){ dev, p }) == D3D_OK);
    memcpy(vp, hle_ptr(p), sizeof *vp);
    hle_free(hle_ptr(p));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);
    win32hle_register(win32hle_d3d9);

    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32u });
    ok("Direct3DCreate9", d3d9 != 0u);
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof *pp);
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = 64; pp->BackBufferHeight = 64;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    pp->Windowed = 1; pp->SwapEffect = 1;
    pp->EnableAutoDepthStencil = 1; pp->AutoDepthStencilFormat = D3DFMT_D24S8;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    ok("CreateDevice", call("IDirect3D9::CreateDevice", 7,
        (uint32_t[]){ d3d9, 0u, D3DDEVTYPE_HAL, 0u,
                      D3DCREATE_SOFTWARE_VERTEXPROCESSING, hle_va(pp), hle_va(pDev) }) == D3D_OK && *pDev);
    uint32_t dev = *pDev;

    /* Our destination surface and the device's original render target. */
    uint32_t *pSurf = (uint32_t *)hle_alloc(4);
    ok("CreateRenderTarget(dest)",
       call("IDirect3DDevice9::CreateRenderTarget", 9,
            (uint32_t[]){ dev, 64u, 64u, D3DFMT_A8R8G8B8, 0u, 0u, 0u,
                          hle_va(pSurf), 0u }) == D3D_OK && *pSurf);
    uint32_t dest = *pSurf;
    uint32_t saved_rt = get_pointer(dev, "IDirect3DDevice9::GetRenderTarget", 3);
    uint32_t saved_ds = get_pointer(dev, "IDirect3DDevice9::GetDepthStencilSurface", 2);
    D3DVIEWPORT9 original_vp;
    get_vp(dev, &original_vp);

    /* ---- Create + ABI ---- */
    ok("D3DXCreateRenderToSurface(NULL out)",
       call("D3DXCreateRenderToSurface", 7,
            (uint32_t[]){ dev, 64u, 64u, D3DFMT_A8R8G8B8, 1u, D3DFMT_D24S8, 0u }) == D3DERR_INVALIDCALL);
    ok("D3DXCreateRenderToSurface(NULL device)",
       call("D3DXCreateRenderToSurface", 7,
            (uint32_t[]){ 0u, 64u, 64u, D3DFMT_A8R8G8B8, 1u, D3DFMT_D24S8, hle_va(pSurf) }) == D3DERR_INVALIDCALL);
    MEM32(pSurf) = 0xDEADBEEFu;
    ok("D3DXCreateRenderToSurface -> D3D_OK",
       call("D3DXCreateRenderToSurface", 7,
            (uint32_t[]){ dev, 64u, 64u, D3DFMT_A8R8G8B8, 1u, D3DFMT_D24S8, hle_va(pSurf) }) == D3D_OK);
    uint32_t rts = MEM32(pSurf);
    ok("object handed back", rts != 0u && rts != 0xDEADBEEFu);
    uint32_t vt = rts ? MEM32(rts) : 0u;
    ok("object has vtable", vt != 0u);
    for (int i = 0; i < RTS_SLOTS; i++)
        ok(slot_names[i], MEM32(vt + 4u * (uint32_t)i) == hle_resolve(slot_names[i]));
    ok("vtable ends after 9 slots", MEM32(vt + 4u * RTS_SLOTS) == 0u);

    /* ---- Every slot through the guest vtable, esp after each ---- */
    for (int i = 0; i < RTS_SLOTS; i++) {
        int c = 0; uint32_t d = 1;
        vslot(rts, i, slot_argc[i], NULL, &c, &d);
        char desc[128];
        snprintf(desc, sizeof desc, "slot %d %s: called + esp restored (argc %d)",
                 i, slot_names[i], slot_argc[i]);
        ok(desc, c && d == 0u);
    }

    /* ---- GetDesc / GetDevice / QueryInterface ---- */
    uint32_t desc_va = hle_va(hle_alloc(40));
    memset(hle_ptr(desc_va), 0, 40);
    ok("GetDesc -> D3D_OK", mcall(rts, 4, 2, (uint32_t[]){ desc_va }) == D3D_OK);
    {
        uint32_t *d = (uint32_t *)hle_ptr(desc_va);
        ok("GetDesc.Width", d[0] == 64u);
        ok("GetDesc.Height", d[1] == 64u);
        ok("GetDesc.Format", d[2] == D3DFMT_A8R8G8B8);
        ok("GetDesc.DepthStencil", d[3] == 1u);
        ok("GetDesc.DepthStencilFormat", d[4] == D3DFMT_D24S8);
    }
    ok("GetDesc(NULL) -> D3DERR_INVALIDCALL",
       mcall(rts, 4, 2, (uint32_t[]){ 0u }) == D3DERR_INVALIDCALL);

    uint32_t dev_out = hle_va(hle_alloc(4));
    MEM32(dev_out) = 0;
    ok("GetDevice -> D3D_OK", mcall(rts, 3, 2, (uint32_t[]){ dev_out }) == D3D_OK);
    ok("GetDevice returned the device", MEM32(dev_out) == dev);
    ok("GetDevice(NULL) -> D3DERR_INVALIDCALL",
       mcall(rts, 3, 2, (uint32_t[]){ 0u }) == D3DERR_INVALIDCALL);

    uint32_t qi_out = hle_va(hle_alloc(4));
    static const uint8_t iid_rts[16] = {
        0x46,0xF3,0x85,0x69, 0x3D,0x2C, 0xB3,0x43,
        0xBE,0x8B,0xDA,0xAE,0x8A,0x03,0xD8,0x94
    };
    uint32_t iid_va = hle_va(hle_alloc(16));
    memcpy(hle_ptr(iid_va), iid_rts, 16);
    MEM32(qi_out) = 0;
    ok("QueryInterface(IID_ID3DXRenderToSurface)",
       mcall(rts, 0, 3, (uint32_t[]){ iid_va, qi_out }) == D3D_OK && MEM32(qi_out) == rts);
    static const uint8_t bogus[16] = { 1,2,3 };
    uint32_t bad_iid = hle_va(hle_alloc(16));
    memcpy(hle_ptr(bad_iid), bogus, 16);
    MEM32(qi_out) = 0xDEADBEEFu;
    ok("QueryInterface(bogus) -> E_NOINTERFACE, out = NULL",
       mcall(rts, 0, 3, (uint32_t[]){ bad_iid, qi_out }) == 0x80004002u && MEM32(qi_out) == 0u);
    ok("QueryInterface(NULL out) -> D3DERR_INVALIDCALL",
       mcall(rts, 0, 3, (uint32_t[]){ iid_va, 0u }) == D3DERR_INVALIDCALL);
    ok("AddRef/Release balanced", mcall(rts, 1, 1, NULL) == 3u && mcall(rts, 2, 1, NULL) == 2u);

    /* ---- BeginScene validation, all with correct RET/esp ---- */
    ok("BeginScene(NULL) -> D3DERR_INVALIDCALL",
       mcall(rts, 5, 3, (uint32_t[]){ 0u, 0u }) == D3DERR_INVALIDCALL);
    uint32_t *pOther = (uint32_t *)hle_alloc(4);
    call("IDirect3DDevice9::CreateOffscreenPlainSurface", 7,
         (uint32_t[]){ dev, 32u, 32u, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, hle_va(pOther), 0u });
    ok("BeginScene(wrong size) -> D3DERR_INVALIDCALL",
       mcall(rts, 5, 3, (uint32_t[]){ *pOther, 0u }) == D3DERR_INVALIDCALL);
    D3DVIEWPORT9 bad_vp = { 0, 0, 65, 64, 0.0f, 1.0f };
    uint32_t bad_vp_va = hle_va(hle_alloc(sizeof bad_vp));
    memcpy(hle_ptr(bad_vp_va), &bad_vp, sizeof bad_vp);
    ok("BeginScene(out-of-bounds viewport) -> D3DERR_INVALIDCALL",
       mcall(rts, 5, 3, (uint32_t[]){ dest, bad_vp_va }) == D3DERR_INVALIDCALL);
    ok("EndScene without BeginScene -> D3DERR_INVALIDCALL",
       mcall(rts, 6, 2, (uint32_t[]){ 1u }) == D3DERR_INVALIDCALL);

    /* ---- State capture / restore with independently known values ---- */
    uint32_t rt_before = get_pointer(dev, "IDirect3DDevice9::GetRenderTarget", 3);
    uint32_t ds_before = get_pointer(dev, "IDirect3DDevice9::GetDepthStencilSurface", 2);
    uint32_t rt_refs_before = MEM32(rt_before + 4);
    uint32_t ds_refs_before = MEM32(ds_before + 4);
    D3DVIEWPORT9 vp_before;
    get_vp(dev, &vp_before);
    D3DVIEWPORT9 vp_scene = { 4, 8, 16, 24, 0.25f, 0.75f };
    uint32_t vp_scene_va = hle_va(hle_alloc(sizeof vp_scene));
    memcpy(hle_ptr(vp_scene_va), &vp_scene, sizeof vp_scene);

    ok("BeginScene(dest, viewport) -> D3D_OK",
       mcall(rts, 5, 3, (uint32_t[]){ dest, vp_scene_va }) == D3D_OK);
    {
        uint32_t now = get_pointer(dev, "IDirect3DDevice9::GetRenderTarget", 3);
        ok("BeginScene bound the destination surface", now == dest);
        D3DVIEWPORT9 now_vp; get_vp(dev, &now_vp);
        ok("BeginScene installed the requested viewport",
           now_vp.X == vp_scene.X && now_vp.Y == vp_scene.Y &&
           now_vp.Width == vp_scene.Width && now_vp.Height == vp_scene.Height &&
           now_vp.MinZ == vp_scene.MinZ && now_vp.MaxZ == vp_scene.MaxZ);
        ok("BeginScene created a fresh depth surface",
           get_pointer(dev, "IDirect3DDevice9::GetDepthStencilSurface", 2) != ds_before);
    }
    ok("second BeginScene while active -> D3DERR_INVALIDCALL",
       mcall(rts, 5, 3, (uint32_t[]){ dest, 0u }) == D3DERR_INVALIDCALL);
    ok("EndScene(mipfilter) -> D3D_OK",
       mcall(rts, 6, 2, (uint32_t[]){ 1u }) == D3D_OK);
    {
        uint32_t now = get_pointer(dev, "IDirect3DDevice9::GetRenderTarget", 3);
        ok("EndScene restored the original render target", now == rt_before);
        ok("EndScene restored the original depth surface",
           get_pointer(dev, "IDirect3DDevice9::GetDepthStencilSurface", 2) == ds_before);
        D3DVIEWPORT9 now_vp; get_vp(dev, &now_vp);
        ok("EndScene restored the original viewport",
           memcmp(&now_vp, &original_vp, sizeof now_vp) == 0);
        ok("saved render target refcount unchanged", MEM32(rt_before + 4) == rt_refs_before + 1u);
        ok("saved depth refcount unchanged", MEM32(ds_before + 4) == ds_refs_before + 1u);
    }

    /* ---- unsupported/error reporting paths keep their slots ---- */
    ok("OnLostDevice -> D3D_OK (no persistent resources)", mcall(rts, 7, 1, NULL) == D3D_OK);
    ok("OnResetDevice -> D3D_OK (no persistent resources)", mcall(rts, 8, 1, NULL) == D3D_OK);
    ok("Release -> 1 (QueryInterface ref)", mcall(rts, 2, 1, NULL) == 1u);
    ok("Release -> 0 (frees object)", mcall(rts, 2, 1, NULL) == 0u);

    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ *pOther });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ dest });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    printf(fails ? "d3dx9_rts_selftest: %d FAILURES\n" : "d3dx9_rts_selftest: OK\n", fails);
    return fails != 0;
}
