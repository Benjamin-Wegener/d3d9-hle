/*
 * d3dx9_envmap_selftest.c - ID3DXRenderToEnvMap ABI, state and error paths.
 *
 * The thirteen slots and their order come from the public Wine header
 * include/d3dx9core.h (ID3DXRenderToEnvMap, slots 0..12). Every slot is
 * called through the guest vtable with a raw push/ret, so a wrong stdcall
 * argc (including `this`) changes the measured esp delta and fails the test.
 * Functional checks use independently known values: the exact D3DXRTE_DESC
 * handed to the factory, the cube face surfaces returned by the device, and
 * the device's original render target / depth surface / viewport, which must
 * be restored after End.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_envmap.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

#define E_NOTIMPL_   0x80004001u
#define E_NOINTERFACE_ 0x80004002u

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
enum { ENVMAP_SLOTS = 13 };
static const char *const slot_names[ENVMAP_SLOTS] = {
    "dxe_QueryInterface", "dxe_AddRef", "dxe_Release", "dxe_GetDevice",
    "dxe_GetDesc", "dxe_BeginCube", "dxe_BeginSphere", "dxe_BeginHemisphere",
    "dxe_BeginParabolic", "dxe_Face", "dxe_End",
    "dxe_OnLostDevice", "dxe_OnResetDevice"
};
/* argc INCLUDES this; source: public ID3DXRenderToEnvMap declaration. */
static const int slot_argc[ENVMAP_SLOTS] = { 3, 1, 1, 2, 2, 2, 2, 3, 3, 3, 2, 1, 1 };

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
static uint32_t make_cube(uint32_t dev, uint32_t edge) {
    uint32_t p = hle_va(hle_alloc(4));
    MEM32(p) = 0;
    ok("CreateCubeTexture", call("IDirect3DDevice9::CreateCubeTexture", 8,
        (uint32_t[]){ dev, edge, 1u, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                      D3DPOOL_DEFAULT, p, 0u }) == D3D_OK && MEM32(p));
    return MEM32(p);
}
static uint32_t cube_face_surface(uint32_t cube, uint32_t face) {
    uint32_t p = hle_va(hle_alloc(4));
    MEM32(p) = 0;
    ok("GetCubeMapSurface", call("IDirect3DCubeTexture9::GetCubeMapSurface", 4,
        (uint32_t[]){ cube, face, 0u, p }) == D3D_OK && MEM32(p));
    return MEM32(p);
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

    uint32_t cube32 = make_cube(dev, 32u);
    uint32_t cube16 = make_cube(dev, 16u);

    /* ---- Create + ABI ---- */
    ok("D3DXCreateRenderToEnvMap(NULL out)",
       call("D3DXCreateRenderToEnvMap", 7,
            (uint32_t[]){ dev, 32u, 1u, D3DFMT_A8R8G8B8, 1u, D3DFMT_D24S8, 0u }) == D3DERR_INVALIDCALL);
    ok("D3DXCreateRenderToEnvMap(NULL device)",
       call("D3DXCreateRenderToEnvMap", 7,
            (uint32_t[]){ 0u, 32u, 1u, D3DFMT_A8R8G8B8, 1u, D3DFMT_D24S8, hle_va(pDev) }) == D3DERR_INVALIDCALL);
    MEM32(pDev) = 0xDEADBEEFu;
    ok("D3DXCreateRenderToEnvMap -> D3D_OK",
       call("D3DXCreateRenderToEnvMap", 7,
            (uint32_t[]){ dev, 32u, 1u, D3DFMT_A8R8G8B8, 1u, D3DFMT_D24S8, hle_va(pDev) }) == D3D_OK);
    uint32_t env = MEM32(pDev);
    ok("object handed back", env != 0u && env != 0xDEADBEEFu);
    uint32_t vt = env ? MEM32(env) : 0u;
    ok("object has vtable", vt != 0u);
    for (int i = 0; i < ENVMAP_SLOTS; i++)
        ok(slot_names[i], MEM32(vt + 4u * (uint32_t)i) == hle_resolve(slot_names[i]));
    ok("vtable ends after 13 slots", MEM32(vt + 4u * ENVMAP_SLOTS) == 0u);

    /* ---- Every slot through the guest vtable, esp after each ---- */
    for (int i = 0; i < ENVMAP_SLOTS; i++) {
        int c = 0; uint32_t d = 1;
        vslot(env, i, slot_argc[i], NULL, &c, &d);
        char desc[128];
        snprintf(desc, sizeof desc, "slot %d %s: called + esp restored (argc %d)",
                 i, slot_names[i], slot_argc[i]);
        ok(desc, c && d == 0u);
    }

    /* ---- GetDesc / GetDevice / QueryInterface ---- */
    uint32_t desc_va = hle_va(hle_alloc(40));
    memset(hle_ptr(desc_va), 0, 40);
    ok("GetDesc -> D3D_OK", mcall(env, 4, 2, (uint32_t[]){ desc_va }) == D3D_OK);
    {
        uint32_t *d = (uint32_t *)hle_ptr(desc_va);
        ok("GetDesc.Size", d[0] == 32u);
        ok("GetDesc.MipLevels", d[1] == 1u);
        ok("GetDesc.Format", d[2] == D3DFMT_A8R8G8B8);
        ok("GetDesc.DepthStencil", d[3] == 1u);
        ok("GetDesc.DepthStencilFormat", d[4] == D3DFMT_D24S8);
    }
    ok("GetDesc(NULL) -> D3DERR_INVALIDCALL",
       mcall(env, 4, 2, (uint32_t[]){ 0u }) == D3DERR_INVALIDCALL);

    uint32_t dev_out = hle_va(hle_alloc(4));
    MEM32(dev_out) = 0;
    ok("GetDevice -> D3D_OK", mcall(env, 3, 2, (uint32_t[]){ dev_out }) == D3D_OK);
    ok("GetDevice returned the device", MEM32(dev_out) == dev);
    ok("GetDevice(NULL) -> D3DERR_INVALIDCALL",
       mcall(env, 3, 2, (uint32_t[]){ 0u }) == D3DERR_INVALIDCALL);

    uint32_t qi_out = hle_va(hle_alloc(4));
    static const uint8_t iid_envmap[16] = {
        0x4b,0x1b,0x3f,0x31, 0xb0,0xc7, 0xa2,0x4f,
        0x9d,0x9d,0x8d,0x38,0x0b,0x64,0x38,0x5e
    };
    uint32_t iid_va = hle_va(hle_alloc(16));
    memcpy(hle_ptr(iid_va), iid_envmap, 16);
    MEM32(qi_out) = 0;
    ok("QueryInterface(IID_ID3DXRenderToEnvMap)",
       mcall(env, 0, 3, (uint32_t[]){ iid_va, qi_out }) == D3D_OK && MEM32(qi_out) == env);
    static const uint8_t bogus[16] = { 1,2,3 };
    uint32_t bad_iid = hle_va(hle_alloc(16));
    memcpy(hle_ptr(bad_iid), bogus, 16);
    MEM32(qi_out) = 0xDEADBEEFu;
    ok("QueryInterface(bogus) -> E_NOINTERFACE, out = NULL",
       mcall(env, 0, 3, (uint32_t[]){ bad_iid, qi_out }) == E_NOINTERFACE_ && MEM32(qi_out) == 0u);
    ok("QueryInterface(NULL out) -> D3DERR_INVALIDCALL",
       mcall(env, 0, 3, (uint32_t[]){ iid_va, 0u }) == D3DERR_INVALIDCALL);
    ok("AddRef/Release balanced", mcall(env, 1, 1, NULL) == 3u && mcall(env, 2, 1, NULL) == 2u);

    /* ---- unsupported maps keep their slots and report an honest error ---- */
    ok("BeginSphere -> E_NOTIMPL",
       mcall(env, 6, 2, (uint32_t[]){ cube32 }) == E_NOTIMPL_);
    ok("BeginHemisphere -> E_NOTIMPL",
       mcall(env, 7, 3, (uint32_t[]){ cube32, cube32 }) == E_NOTIMPL_);
    ok("BeginParabolic -> E_NOTIMPL",
       mcall(env, 8, 3, (uint32_t[]){ cube32, cube32 }) == E_NOTIMPL_);

    /* ---- validation before any cube is active ---- */
    ok("End without BeginCube -> D3DERR_INVALIDCALL",
       mcall(env, 10, 2, (uint32_t[]){ 1u }) == D3DERR_INVALIDCALL);
    ok("Face without BeginCube -> D3DERR_INVALIDCALL",
       mcall(env, 9, 3, (uint32_t[]){ 0u, 1u }) == D3DERR_INVALIDCALL);
    ok("BeginCube(NULL) -> D3DERR_INVALIDCALL",
       mcall(env, 5, 2, (uint32_t[]){ 0u }) == D3DERR_INVALIDCALL);
    ok("BeginCube(wrong size) -> D3DERR_INVALIDCALL",
       mcall(env, 5, 2, (uint32_t[]){ cube16 }) == D3DERR_INVALIDCALL);

    /* ---- state capture / restore with independently known values ---- */
    uint32_t saved_rt = get_pointer(dev, "IDirect3DDevice9::GetRenderTarget", 3);
    uint32_t saved_ds = get_pointer(dev, "IDirect3DDevice9::GetDepthStencilSurface", 2);
    uint32_t rt_refs_before = MEM32(saved_rt + 4);
    uint32_t ds_refs_before = MEM32(saved_ds + 4);
    D3DVIEWPORT9 vp_orig = { 1u, 2u, 20u, 21u, 0.25f, 0.75f };
    uint32_t vp_va = hle_va(hle_alloc(sizeof vp_orig));
    memcpy(hle_ptr(vp_va), &vp_orig, sizeof vp_orig);
    ok("SetViewport(original)", call("IDirect3DDevice9::SetViewport", 2,
        (uint32_t[]){ dev, vp_va }) == D3D_OK);
    D3DVIEWPORT9 original_vp;
    get_vp(dev, &original_vp);

    uint32_t cube_refs_before = MEM32(cube32 + 4);
    ok("BeginCube(cube32) -> D3D_OK",
       mcall(env, 5, 2, (uint32_t[]){ cube32 }) == D3D_OK);
    ok("BeginCube AddRef'd the cube", MEM32(cube32 + 4) == cube_refs_before + 1u);
    ok("second BeginCube while active -> D3DERR_INVALIDCALL",
       mcall(env, 5, 2, (uint32_t[]){ cube32 }) == D3DERR_INVALIDCALL);

    uint32_t f0 = cube_face_surface(cube32, 0u);
    uint32_t f3 = cube_face_surface(cube32, 3u);
    ok("Face(POSITIVE_X, filter) -> D3D_OK",
       mcall(env, 9, 3, (uint32_t[]){ 0u, 1u }) == D3D_OK);
    {
        uint32_t now = get_pointer(dev, "IDirect3DDevice9::GetRenderTarget", 3);
        ok("Face bound cube face 0", now == f0);
        ok("Face created a fresh depth surface",
           get_pointer(dev, "IDirect3DDevice9::GetDepthStencilSurface", 2) != saved_ds);
    }
    ok("Face(NEGATIVE_Z, filter) -> D3D_OK (face switch)",
       mcall(env, 9, 3, (uint32_t[]){ 3u, 1u }) == D3D_OK);
    {
        uint32_t now = get_pointer(dev, "IDirect3DDevice9::GetRenderTarget", 3);
        ok("Face switched to cube face 3", now == f3);
    }
    ok("Face(6) -> D3DERR_INVALIDCALL",
       mcall(env, 9, 3, (uint32_t[]){ 6u, 1u }) == D3DERR_INVALIDCALL);
    ok("End(filter) -> D3D_OK",
       mcall(env, 10, 2, (uint32_t[]){ 1u }) == D3D_OK);
    {
        uint32_t now_rt = get_pointer(dev, "IDirect3DDevice9::GetRenderTarget", 3);
        ok("End restored the original render target", now_rt == saved_rt);
        ok("End restored the original depth surface",
           get_pointer(dev, "IDirect3DDevice9::GetDepthStencilSurface", 2) == saved_ds);
        D3DVIEWPORT9 now_vp; get_vp(dev, &now_vp);
        ok("End restored the original viewport",
           memcmp(&now_vp, &original_vp, sizeof now_vp) == 0);
        ok("render target refcount unchanged", MEM32(saved_rt + 4) == rt_refs_before + 1u);
        ok("depth refcount unchanged", MEM32(saved_ds + 4) == ds_refs_before + 1u);
        ok("End released the cube reference", MEM32(cube32 + 4) == cube_refs_before);
    }

    ok("OnLostDevice -> D3D_OK", mcall(env, 11, 1, NULL) == D3D_OK);
    ok("OnResetDevice -> D3D_OK", mcall(env, 12, 1, NULL) == D3D_OK);
    ok("Release -> 1 (QueryInterface ref)", mcall(env, 2, 1, NULL) == 1u);
    ok("Release -> 0 (frees object)", mcall(env, 2, 1, NULL) == 0u);

    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ f3 });
    call("IDirect3DSurface9::Release", 1, (uint32_t[]){ f0 });
    call("IDirect3DCubeTexture9::Release", 1, (uint32_t[]){ cube16 });
    call("IDirect3DCubeTexture9::Release", 1, (uint32_t[]){ cube32 });
    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    printf(fails ? "d3dx9_envmap_selftest: %d FAILURES\n" : "d3dx9_envmap_selftest: OK\n", fails);
    return fails != 0;
}
