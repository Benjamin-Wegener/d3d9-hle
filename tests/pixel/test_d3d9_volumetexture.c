/*
 * test_d3d9_volumetexture.c - Real Metal pixel test for CreateVolumeTexture, LockBox, Metal GPU upload/readback.
 *
 * 1) Calls IDirect3DDevice9::CreateVolumeTexture(8, 8, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, ...).
 * 2) Locks box on volume texture level 0.
 * 3) Writes distinct colors into each depth slice:
 *    Slice 0: Red (0xFFFF0000)
 *    Slice 1: Green (0xFF00FF00)
 *    Slice 2: Blue (0xFF0000FF)
 *    Slice 3: White (0xFFFFFFFF)
 * 4) Unlocks box, which flushes CPU data into the real Metal 3D texture.
 * 5) Reads back from Metal 3D texture using hle_d3d9_metal_read_volume_texture.
 * 6) Verifies that each slice contains the expected pixel values.
 * 7) Tests volume interfaces: GetLevelCount, GetLevelDesc, GetVolumeLevel, Volume::GetDesc, Volume::LockBox/UnlockBox.
 * 8) Tests error handling (NULL pointer arguments).
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_internal.h"
#include "d3d9_metal.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails = 0;
static void ok(const char *name, int cond) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) fails++;
}
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
#define A(p) hle_va(p)

#define W 8
#define H 8
#define D 4

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable() != 0) {
        printf("  [FAIL] Metal backend available\n");
        return 1;
    }
    ok("Metal backend available", hle_d3d9_metal_is_active());
    printf("=== Test: D3D9 CreateVolumeTexture with Metal readback ===\n");

    uint32_t a_create[1] = { 32 };
    uint32_t d3d9 = call("Direct3DCreate9", 1, a_create);
    ok("Direct3DCreate9", d3d9 != 0);

    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = W; pp->BackBufferHeight = H;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t a_cdev[7] = { d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, 0,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev) };
    uint32_t hr = call("IDirect3D9::CreateDevice", 7, a_cdev);
    ok("CreateDevice", hr == D3D_OK && *pDev != 0);
    uint32_t dev = *pDev;

    /* 1) CreateVolumeTexture */
    uint32_t *pVolTex = (uint32_t *)hle_alloc(4);
    uint32_t a_cvtex[9] = { dev, W, H, D, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, A(pVolTex) };
    hr = call("IDirect3DDevice9::CreateVolumeTexture", 9, a_cvtex);
    ok("CreateVolumeTexture", hr == D3D_OK && *pVolTex != 0);
    uint32_t vol_tex = *pVolTex;

    /* Check LevelCount */
    uint32_t levels = call("IDirect3DVolumeTexture9::GetLevelCount", 1, (uint32_t[]){ vol_tex });
    ok("GetLevelCount == 1", levels == 1);

    /* Check LevelDesc */
    D3DVOLUME_DESC *vdesc = (D3DVOLUME_DESC *)hle_alloc(sizeof(*vdesc));
    hr = call("IDirect3DVolumeTexture9::GetLevelDesc", 3, (uint32_t[]){ vol_tex, 0, A(vdesc) });
    ok("GetLevelDesc", hr == D3D_OK && vdesc->Width == W && vdesc->Height == H && vdesc->Depth == D);

    /* 2) LockBox on VolumeTexture */
    D3DLOCKED_BOX *lbox = (D3DLOCKED_BOX *)hle_alloc(sizeof(*lbox));
    hr = call("IDirect3DVolumeTexture9::LockBox", 5, (uint32_t[]){ vol_tex, 0, A(lbox), 0, 0 });
    ok("LockBox", hr == D3D_OK && lbox->pBits != 0);

    const uint32_t slice_colors[4] = {
        0xFFFF0000u, /* Red */
        0xFF00FF00u, /* Green */
        0xFF0000FFu, /* Blue */
        0xFFFFFFFFu  /* White */
    };

    uint8_t *base = (uint8_t *)hle_ptr(lbox->pBits);
    for (int z = 0; z < D; z++) {
        uint8_t *slice_base = base + z * lbox->SlicePitch;
        for (int y = 0; y < H; y++) {
            uint32_t *row = (uint32_t *)(slice_base + y * lbox->RowPitch);
            for (int x = 0; x < W; x++) {
                row[x] = slice_colors[z];
            }
        }
    }

    /* 3) UnlockBox -> uploads to Metal 3D texture */
    hr = call("IDirect3DVolumeTexture9::UnlockBox", 2, (uint32_t[]){ vol_tex, 0 });
    ok("UnlockBox", hr == D3D_OK);

    /* 4) Read back from Metal GPU volume texture directly and verify */
    d3d9_volumetexture_ctx_t *ctx = (d3d9_volumetexture_ctx_t *)OBJ_CTX(vol_tex);
    ok("vol_tex has gpu_tex", ctx && ctx->gpu_tex != 0);

    uint32_t readback[W * H * D];
    memset(readback, 0, sizeof(readback));
    int rb_res = hle_d3d9_metal_read_volume_texture(ctx->gpu_tex, 0,
                                                   readback, W * 4, W * H * 4);
    ok("Metal readback of volume texture", rb_res == 0);

    int slice_match[4] = { 1, 1, 1, 1 };
    for (int z = 0; z < D; z++) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                uint32_t px = readback[z * (W * H) + y * W + x];
                if (px != slice_colors[z]) {
                    slice_match[z] = 0;
                }
            }
        }
    }
    ok("Slice 0 is Red", slice_match[0]);
    ok("Slice 1 is Green", slice_match[1]);
    ok("Slice 2 is Blue", slice_match[2]);
    ok("Slice 3 is White", slice_match[3]);

    /* 5) Test IDirect3DVolume9 interface */
    uint32_t *pVol = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DVolumeTexture9::GetVolumeLevel", 3, (uint32_t[]){ vol_tex, 0, A(pVol) });
    ok("GetVolumeLevel", hr == D3D_OK && *pVol != 0);
    uint32_t vol = *pVol;

    D3DVOLUME_DESC *vdesc2 = (D3DVOLUME_DESC *)hle_alloc(sizeof(*vdesc2));
    hr = call("IDirect3DVolume9::GetDesc", 2, (uint32_t[]){ vol, A(vdesc2) });
    ok("Volume::GetDesc", hr == D3D_OK && vdesc2->Width == W && vdesc2->Depth == D);

    /* LockBox through Volume interface */
    hr = call("IDirect3DVolume9::LockBox", 4, (uint32_t[]){ vol, A(lbox), 0, D3DLOCK_READONLY });
    ok("Volume::LockBox", hr == D3D_OK && lbox->pBits != 0);
    uint8_t *vbase = (uint8_t *)hle_ptr(lbox->pBits);
    uint32_t *vrow0 = (uint32_t *)vbase;
    ok("Volume LockBox read slice 0 == Red", vrow0[0] == 0xFFFF0000u);
    hr = call("IDirect3DVolume9::UnlockBox", 1, (uint32_t[]){ vol });
    ok("Volume::UnlockBox", hr == D3D_OK);

    /* Error handling */
    hr = call("IDirect3DDevice9::CreateVolumeTexture", 9, (uint32_t[]){ dev, W, H, D, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, 0 });
    ok("CreateVolumeTexture NULL out ptr -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);

    /* Test volumetex9_LockBox with pLocked == 0 returns D3DERR_INVALIDCALL */
    hr = call("IDirect3DVolumeTexture9::LockBox", 5, (uint32_t[]){ vol_tex, 0, 0, 0, 0 });
    ok("LockBox pLocked=0 -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);

    /* Test COM methods: GetType */
    uint32_t type = call("IDirect3DVolumeTexture9::GetType", 1, (uint32_t[]){ vol_tex });
    ok("VolumeTexture::GetType == D3DRTYPE_VOLUMETEXTURE", type == D3DRTYPE_VOLUMETEXTURE);

    /* PrivateData: an unknown GUID is D3DERR_NOTFOUND, a NULL GUID D3DERR_INVALIDCALL */
    uint32_t *pguid = (uint32_t *)hle_alloc(16); memset(pguid, 0x5A, 16);
    uint32_t *psz = (uint32_t *)hle_alloc(4); *psz = 4;
    hr = call("IDirect3DVolumeTexture9::GetPrivateData", 4, (uint32_t[]){ vol_tex, 0, 0, 0 });
    ok("VolumeTexture::GetPrivateData(NULL guid) -> D3DERR_INVALIDCALL", hr == D3DERR_INVALIDCALL);
    hr = call("IDirect3DVolumeTexture9::GetPrivateData", 4, (uint32_t[]){ vol_tex, A(pguid), 0, A(psz) });
    ok("VolumeTexture::GetPrivateData -> D3DERR_NOTFOUND", hr == D3DERR_NOTFOUND);
    hr = call("IDirect3DVolumeTexture9::FreePrivateData", 2, (uint32_t[]){ vol_tex, A(pguid) });
    ok("VolumeTexture::FreePrivateData -> D3DERR_NOTFOUND", hr == D3DERR_NOTFOUND);

    call("IDirect3DVolume9::Release", 1, (uint32_t[]){ vol });
    call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ vol_tex });

    /* 6) Test CreateVolumeTexture with Levels > 1 (e.g. Levels = 2: 8x8x4 and 4x4x2) */
    uint32_t *pVolTex2 = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::CreateVolumeTexture", 9, (uint32_t[]){ dev, W, H, D, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, A(pVolTex2) });
    ok("CreateVolumeTexture Levels=2", hr == D3D_OK && *pVolTex2 != 0);
    uint32_t vol_tex2 = *pVolTex2;

    uint32_t levels2 = call("IDirect3DVolumeTexture9::GetLevelCount", 1, (uint32_t[]){ vol_tex2 });
    ok("GetLevelCount == 2", levels2 == 2);

    /* Level 0: 8x8x4, fill with Yellow (0xFFFFFF00) */
    hr = call("IDirect3DVolumeTexture9::LockBox", 5, (uint32_t[]){ vol_tex2, 0, A(lbox), 0, 0 });
    ok("LockBox level 0", hr == D3D_OK && lbox->pBits != 0);
    uint8_t *b0 = (uint8_t *)hle_ptr(lbox->pBits);
    for (int z = 0; z < D; z++) {
        uint8_t *sz = b0 + z * lbox->SlicePitch;
        for (int y = 0; y < H; y++) {
            uint32_t *row = (uint32_t *)(sz + y * lbox->RowPitch);
            for (int x = 0; x < W; x++) {
                row[x] = 0xFFFFFF00u;
            }
        }
    }
    hr = call("IDirect3DVolumeTexture9::UnlockBox", 2, (uint32_t[]){ vol_tex2, 0 });
    ok("UnlockBox level 0", hr == D3D_OK);

    /* Level 1: 4x4x2, fill with Cyan (0xFF00FFFF) */
    hr = call("IDirect3DVolumeTexture9::LockBox", 5, (uint32_t[]){ vol_tex2, 1, A(lbox), 0, 0 });
    ok("LockBox level 1", hr == D3D_OK && lbox->pBits != 0);
    uint8_t *b1 = (uint8_t *)hle_ptr(lbox->pBits);
    for (int z = 0; z < D / 2; z++) {
        uint8_t *sz = b1 + z * lbox->SlicePitch;
        for (int y = 0; y < H / 2; y++) {
            uint32_t *row = (uint32_t *)(sz + y * lbox->RowPitch);
            for (int x = 0; x < W / 2; x++) {
                row[x] = 0xFF00FFFFu;
            }
        }
    }
    hr = call("IDirect3DVolumeTexture9::UnlockBox", 2, (uint32_t[]){ vol_tex2, 1 });
    ok("UnlockBox level 1", hr == D3D_OK);

    /* Read back both levels from Metal GPU volume texture */
    d3d9_volumetexture_ctx_t *ctx2 = (d3d9_volumetexture_ctx_t *)OBJ_CTX(vol_tex2);
    ok("vol_tex2 has gpu_tex", ctx2 && ctx2->gpu_tex != 0);

    uint32_t readback0[W * H * D];
    memset(readback0, 0, sizeof(readback0));
    rb_res = hle_d3d9_metal_read_volume_texture(ctx2->gpu_tex, 0, readback0, W * 4, W * H * 4);
    ok("Metal readback of level 0", rb_res == 0);
    int match0 = 1;
    for (int i = 0; i < W * H * D; i++) {
        if (readback0[i] != 0xFFFFFF00u) { match0 = 0; break; }
    }
    ok("Level 0 Metal content verified", match0);

    uint32_t readback1[(W / 2) * (H / 2) * (D / 2)];
    memset(readback1, 0, sizeof(readback1));
    rb_res = hle_d3d9_metal_read_volume_texture(ctx2->gpu_tex, 1, readback1, (W / 2) * 4, (W / 2) * (H / 2) * 4);
    ok("Metal readback of level 1", rb_res == 0);
    int match1 = 1;
    for (int i = 0; i < (W / 2) * (H / 2) * (D / 2); i++) {
        if (readback1[i] != 0xFF00FFFFu) { match1 = 0; break; }
    }
    ok("Level 1 Metal content verified", match1);

    call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ vol_tex2 });

    /* =========================================================================
     * COM-CHECKLISTE Punkt 4: Vtable audit and raw stdcall esp checks for
     * IDirect3DVolumeTexture9 (22 slots) and IDirect3DVolume9 (11 slots).
     * ========================================================================= */
    printf("=== VTable slot audit and esp balance checks ===\n");
    void *stack = hle_alloc(4096);
    uint32_t g_stack = hle_va(stack);

    /* Helper lambda/macros for raw vcall */
    #define RAW_VCALL(obj_vt, slot, argc, args_arr, out_eax_ptr) ({ \
        uint32_t va = MEM32(obj_vt + 4u * (uint32_t)(slot)); \
        uint32_t sp = g_stack + 0x800u; \
        for (int _i = (argc) - 1; _i >= 0; _i--) { sp -= 4; MEM32(sp) = (args_arr)[_i]; } \
        sp -= 4; MEM32(sp) = 0xDEADBEEFu; \
        mach_enter(); \
        g_esp = sp; \
        recomp_func_t _f = recomp_lookup(va); \
        if (!_f) _f = recomp_lookup_import(va); \
        if (_f) _f(); \
        *(out_eax_ptr) = g_eax; \
        uint32_t _after = g_esp; \
        mach_leave(); \
        _after; \
    })

    /* 1. Audit IDirect3DVolumeTexture9 (22 slots) */
    enum { PUBLIC_VOLUMETEX9_SLOTS = 22 };
    static const char *volumetex9_slot_names[PUBLIC_VOLUMETEX9_SLOTS] = {
        "IDirect3DVolumeTexture9::QueryInterface",      /* slot 0 */
        "IDirect3DVolumeTexture9::AddRef",              /* slot 1 */
        "IDirect3DVolumeTexture9::Release",             /* slot 2 */
        "IDirect3DVolumeTexture9::GetDevice",           /* slot 3 */
        "IDirect3DVolumeTexture9::SetPrivateData",      /* slot 4 */
        "IDirect3DVolumeTexture9::GetPrivateData",      /* slot 5 */
        "IDirect3DVolumeTexture9::FreePrivateData",     /* slot 6 */
        "IDirect3DVolumeTexture9::SetPriority",         /* slot 7 */
        "IDirect3DVolumeTexture9::GetPriority",         /* slot 8 */
        "IDirect3DVolumeTexture9::PreLoad",             /* slot 9 */
        "IDirect3DVolumeTexture9::GetType",             /* slot 10 */
        "IDirect3DVolumeTexture9::SetLOD",              /* slot 11 */
        "IDirect3DVolumeTexture9::GetLOD",              /* slot 12 */
        "IDirect3DVolumeTexture9::GetLevelCount",       /* slot 13 */
        "IDirect3DVolumeTexture9::SetAutoGenFilterType", /* slot 14 */
        "IDirect3DVolumeTexture9::GetAutoGenFilterType", /* slot 15 */
        "IDirect3DVolumeTexture9::GenerateMipSubLevels", /* slot 16 */
        "IDirect3DVolumeTexture9::GetLevelDesc",         /* slot 17 */
        "IDirect3DVolumeTexture9::GetVolumeLevel",       /* slot 18 */
        "IDirect3DVolumeTexture9::LockBox",             /* slot 19 */
        "IDirect3DVolumeTexture9::UnlockBox",           /* slot 20 */
        "IDirect3DVolumeTexture9::AddDirtyBox"          /* slot 21 */
    };

    uint32_t *pVtTest = (uint32_t *)hle_alloc(4);
    hr = call("IDirect3DDevice9::CreateVolumeTexture", 9, (uint32_t[]){ dev, 4, 4, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, A(pVtTest) });
    ok("CreateVolumeTexture for VTable audit", hr == D3D_OK && *pVtTest != 0);
    uint32_t vt_tex = *pVtTest;
    uint32_t vt_tex_vt = MEM32(vt_tex);
    ok("volumetex has vtable", vt_tex_vt != 0);

    for (int i = 0; i < PUBLIC_VOLUMETEX9_SLOTS; i++) {
        uint32_t va = MEM32(vt_tex_vt + 4u * i);
        ok(volumetex9_slot_names[i], va != 0 && va == hle_resolve(volumetex9_slot_names[i]));
    }
    ok("volumetex has no slot 22", MEM32(vt_tex_vt + 4u * PUBLIC_VOLUMETEX9_SLOTS) == 0);

    /* Test raw stdcall stack balance on each of the 22 slots of IDirect3DVolumeTexture9 */
    uint32_t eax_val = 0;
    uint32_t pp_out = hle_va(hle_alloc(16));
    D3DVOLUME_DESC *vdesc_test = (D3DVOLUME_DESC *)hle_alloc(sizeof(*vdesc_test));
    D3DLOCKED_BOX *lbox_test = (D3DLOCKED_BOX *)hle_alloc(sizeof(*lbox_test));

    /* slot 0: QI (this, riid, ppv) -> argc 3 */
    ok("slot 0 QueryInterface esp", RAW_VCALL(vt_tex_vt, 0, 3, ((uint32_t[]){ vt_tex, 0, pp_out }), &eax_val) == g_stack + 0x800u);
    /* slot 1: AddRef (this) -> argc 1 */
    ok("slot 1 AddRef esp", RAW_VCALL(vt_tex_vt, 1, 1, ((uint32_t[]){ vt_tex }), &eax_val) == g_stack + 0x800u);
    /* slot 2: Release (this) -> argc 1 */
    ok("slot 2 Release esp", RAW_VCALL(vt_tex_vt, 2, 1, ((uint32_t[]){ vt_tex }), &eax_val) == g_stack + 0x800u);
    /* slot 3: GetDevice (this, ppDevice) -> argc 2 */
    ok("slot 3 GetDevice esp", RAW_VCALL(vt_tex_vt, 3, 2, ((uint32_t[]){ vt_tex, pp_out }), &eax_val) == g_stack + 0x800u);
    /* slot 4: SetPrivateData (this, refguid, pData, SizeOfData, Flags) -> argc 5 */
    ok("slot 4 SetPrivateData esp", RAW_VCALL(vt_tex_vt, 4, 5, ((uint32_t[]){ vt_tex, 0, 0, 0, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 5: GetPrivateData (this, refguid, pData, pSizeOfData) -> argc 4 */
    ok("slot 5 GetPrivateData esp", RAW_VCALL(vt_tex_vt, 5, 4, ((uint32_t[]){ vt_tex, 0, 0, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 6: FreePrivateData (this, refguid) -> argc 2 */
    ok("slot 6 FreePrivateData esp", RAW_VCALL(vt_tex_vt, 6, 2, ((uint32_t[]){ vt_tex, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 7: SetPriority (this, PriorityNew) -> argc 2 */
    ok("slot 7 SetPriority esp", RAW_VCALL(vt_tex_vt, 7, 2, ((uint32_t[]){ vt_tex, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 8: GetPriority (this) -> argc 1 */
    ok("slot 8 GetPriority esp", RAW_VCALL(vt_tex_vt, 8, 1, ((uint32_t[]){ vt_tex }), &eax_val) == g_stack + 0x800u);
    /* slot 9: PreLoad (this) -> argc 1 */
    ok("slot 9 PreLoad esp", RAW_VCALL(vt_tex_vt, 9, 1, ((uint32_t[]){ vt_tex }), &eax_val) == g_stack + 0x800u);
    /* slot 10: GetType (this) -> argc 1 */
    ok("slot 10 GetType esp", RAW_VCALL(vt_tex_vt, 10, 1, ((uint32_t[]){ vt_tex }), &eax_val) == g_stack + 0x800u);
    ok("slot 10 GetType == D3DRTYPE_VOLUMETEXTURE", eax_val == D3DRTYPE_VOLUMETEXTURE);
    /* slot 11: SetLOD (this, LODNew) -> argc 2 */
    ok("slot 11 SetLOD esp", RAW_VCALL(vt_tex_vt, 11, 2, ((uint32_t[]){ vt_tex, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 12: GetLOD (this) -> argc 1 */
    ok("slot 12 GetLOD esp", RAW_VCALL(vt_tex_vt, 12, 1, ((uint32_t[]){ vt_tex }), &eax_val) == g_stack + 0x800u);
    /* slot 13: GetLevelCount (this) -> argc 1 */
    ok("slot 13 GetLevelCount esp", RAW_VCALL(vt_tex_vt, 13, 1, ((uint32_t[]){ vt_tex }), &eax_val) == g_stack + 0x800u);
    ok("slot 13 GetLevelCount == 1", eax_val == 1);
    /* slot 14: SetAutoGenFilterType (this, FilterType) -> argc 2 */
    ok("slot 14 SetAutoGenFilterType esp", RAW_VCALL(vt_tex_vt, 14, 2, ((uint32_t[]){ vt_tex, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 15: GetAutoGenFilterType (this) -> argc 1 */
    ok("slot 15 GetAutoGenFilterType esp", RAW_VCALL(vt_tex_vt, 15, 1, ((uint32_t[]){ vt_tex }), &eax_val) == g_stack + 0x800u);
    /* slot 16: GenerateMipSubLevels (this) -> argc 1 */
    ok("slot 16 GenerateMipSubLevels esp", RAW_VCALL(vt_tex_vt, 16, 1, ((uint32_t[]){ vt_tex }), &eax_val) == g_stack + 0x800u);
    /* slot 17: GetLevelDesc (this, Level, pDesc) -> argc 3 */
    ok("slot 17 GetLevelDesc esp", RAW_VCALL(vt_tex_vt, 17, 3, ((uint32_t[]){ vt_tex, 0, A(vdesc_test) }), &eax_val) == g_stack + 0x800u);
    /* slot 18: GetVolumeLevel (this, Level, ppVolumeLevel) -> argc 3 */
    ok("slot 18 GetVolumeLevel esp", RAW_VCALL(vt_tex_vt, 18, 3, ((uint32_t[]){ vt_tex, 0, pp_out }), &eax_val) == g_stack + 0x800u);
    uint32_t vt_vol = MEM32(pp_out);
    /* slot 19: LockBox (this, Level, pLockedVolume, pBox, Flags) -> argc 5 */
    ok("slot 19 LockBox esp", RAW_VCALL(vt_tex_vt, 19, 5, ((uint32_t[]){ vt_tex, 0, A(lbox_test), 0, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 20: UnlockBox (this, Level) -> argc 2 */
    ok("slot 20 UnlockBox esp", RAW_VCALL(vt_tex_vt, 20, 2, ((uint32_t[]){ vt_tex, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 21: AddDirtyBox (this, pDirtyBox) -> argc 2 */
    ok("slot 21 AddDirtyBox esp", RAW_VCALL(vt_tex_vt, 21, 2, ((uint32_t[]){ vt_tex, 0 }), &eax_val) == g_stack + 0x800u);

    /* 2. Audit IDirect3DVolume9 (11 slots) */
    enum { PUBLIC_VOLUME9_SLOTS = 11 };
    static const char *volume9_slot_names[PUBLIC_VOLUME9_SLOTS] = {
        "IDirect3DVolume9::QueryInterface",      /* slot 0 */
        "IDirect3DVolume9::AddRef",              /* slot 1 */
        "IDirect3DVolume9::Release",             /* slot 2 */
        "IDirect3DVolume9::GetDevice",           /* slot 3 */
        "IDirect3DVolume9::SetPrivateData",      /* slot 4 */
        "IDirect3DVolume9::GetPrivateData",      /* slot 5 */
        "IDirect3DVolume9::FreePrivateData",     /* slot 6 */
        "IDirect3DVolume9::GetContainer",        /* slot 7 */
        "IDirect3DVolume9::GetDesc",             /* slot 8 */
        "IDirect3DVolume9::LockBox",             /* slot 9 */
        "IDirect3DVolume9::UnlockBox"            /* slot 10 */
    };

    ok("vt_vol exists", vt_vol != 0);
    uint32_t vt_vol_vt = MEM32(vt_vol);
    ok("volume has vtable", vt_vol_vt != 0);

    for (int i = 0; i < PUBLIC_VOLUME9_SLOTS; i++) {
        uint32_t va = MEM32(vt_vol_vt + 4u * i);
        ok(volume9_slot_names[i], va != 0 && va == hle_resolve(volume9_slot_names[i]));
    }
    ok("volume has no slot 11", MEM32(vt_vol_vt + 4u * PUBLIC_VOLUME9_SLOTS) == 0);

    /* Test raw stdcall stack balance on each of the 11 slots of IDirect3DVolume9 */
    /* slot 0: QI (this, riid, ppv) -> argc 3 */
    ok("volume slot 0 QueryInterface esp", RAW_VCALL(vt_vol_vt, 0, 3, ((uint32_t[]){ vt_vol, 0, pp_out }), &eax_val) == g_stack + 0x800u);
    /* slot 1: AddRef (this) -> argc 1 */
    ok("volume slot 1 AddRef esp", RAW_VCALL(vt_vol_vt, 1, 1, ((uint32_t[]){ vt_vol }), &eax_val) == g_stack + 0x800u);
    /* slot 2: Release (this) -> argc 1 */
    ok("volume slot 2 Release esp", RAW_VCALL(vt_vol_vt, 2, 1, ((uint32_t[]){ vt_vol }), &eax_val) == g_stack + 0x800u);
    /* slot 3: GetDevice (this, ppDevice) -> argc 2 */
    ok("volume slot 3 GetDevice esp", RAW_VCALL(vt_vol_vt, 3, 2, ((uint32_t[]){ vt_vol, pp_out }), &eax_val) == g_stack + 0x800u);
    /* slot 4: SetPrivateData (this, refguid, pData, SizeOfData, Flags) -> argc 5 */
    ok("volume slot 4 SetPrivateData esp", RAW_VCALL(vt_vol_vt, 4, 5, ((uint32_t[]){ vt_vol, 0, 0, 0, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 5: GetPrivateData (this, refguid, pData, pSizeOfData) -> argc 4 */
    ok("volume slot 5 GetPrivateData esp", RAW_VCALL(vt_vol_vt, 5, 4, ((uint32_t[]){ vt_vol, 0, 0, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 6: FreePrivateData (this, refguid) -> argc 2 */
    ok("volume slot 6 FreePrivateData esp", RAW_VCALL(vt_vol_vt, 6, 2, ((uint32_t[]){ vt_vol, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 7: GetContainer (this, riid, ppContainer) -> argc 3 */
    ok("volume slot 7 GetContainer esp", RAW_VCALL(vt_vol_vt, 7, 3, ((uint32_t[]){ vt_vol, 0, pp_out }), &eax_val) == g_stack + 0x800u);
    ok("volume slot 7 GetContainer returns parent texture", eax_val == D3D_OK && MEM32(pp_out) == vt_tex);
    /* slot 8: GetDesc (this, pDesc) -> argc 2 */
    ok("volume slot 8 GetDesc esp", RAW_VCALL(vt_vol_vt, 8, 2, ((uint32_t[]){ vt_vol, A(vdesc_test) }), &eax_val) == g_stack + 0x800u);
    /* slot 9: LockBox (this, pLockedVolume, pBox, Flags) -> argc 4 */
    ok("volume slot 9 LockBox esp", RAW_VCALL(vt_vol_vt, 9, 4, ((uint32_t[]){ vt_vol, A(lbox_test), 0, 0 }), &eax_val) == g_stack + 0x800u);
    /* slot 10: UnlockBox (this) -> argc 1 */
    ok("volume slot 10 UnlockBox esp", RAW_VCALL(vt_vol_vt, 10, 1, ((uint32_t[]){ vt_vol }), &eax_val) == g_stack + 0x800u);

    call("IDirect3DVolume9::Release", 1, (uint32_t[]){ vt_vol });
    call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ vt_tex });

    call("IDirect3DDevice9::Release", 1, (uint32_t[]){ dev });
    call("IDirect3D9::Release", 1, (uint32_t[]){ d3d9 });
    hle_d3d9_metal_shutdown_device();

    printf(fails == 0 ? "test_d3d9_volumetexture: OK\n" : "test_d3d9_volumetexture: FAILED\n");
    return fails != 0;
}
