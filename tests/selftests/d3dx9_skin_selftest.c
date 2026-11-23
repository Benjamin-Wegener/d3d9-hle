/*
 * d3dx9_skin_selftest.c - ABI and behaviour tests for the D3DX9 skin-info HLE.
 *
 * Slot order, count and signatures come ONLY from Wine's public header
 * include/d3dx9mesh.h (27 ID3DXSkinInfo methods). Every slot is invoked through
 * the GUEST vtable with a raw call whose resulting ESP proves the shim's single
 * RET popped exactly ret + argc*4 bytes (argc INCLUDES `this`).
 *
 * Functional expectations are hand-computed, independent constants:
 *   - weighted bone matrices: x-scale 2 + translation (10,0,0) and translation
 *     (0,20,0), both weighted 0.5, transform (1,2,3) to (6.5,12,3);
 *   - inverse-transpose normals: a bone transform with x-scale 2 and its
 *     inverse-transpose with x-scale 0.5 turn normal (1,0,0), weighted 0.5 by
 *     two bones (one identity), into (0.75,0,0). Using the bone transform for
 *     normals instead would give (1.5,0,0), so the test distinguishes them.
 * They would go red if the implementation regressed or were replaced by a
 * stub.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_mesh.h"
#include "d3dx9_skin.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

#define D3D_OK_V      0x00000000u
#define INVALID_V     0x8876086Cu
#define NOTIMPL_V     0x80004001u
#define EFAIL_V       0x80004005u

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
static uint32_t g_stack;

static uint32_t va0(uint32_t n) { void *p = hle_alloc(n); memset(p, 0, n); return hle_va(p); }
static int cf(float a, float b) { return fabsf(a - b) < 0.0008f; }
static int v3(uint32_t va, float x, float y, float z) {
    return cf(MEMF(va),x) && cf(MEMF(va+4u),y) && cf(MEMF(va+8u),z);
}
static void put_ident(uint32_t m) {
    memset(hle_ptr(m), 0, 64u);
    for (int i = 0; i < 4; i++) MEMF(m + 20u * (uint32_t)i) = 1.0f;  /* diagonal */
}

/* ---- raw guest-vtable call: nothing restores esp after the callee -------- */
static uint32_t raw_vcall(uint32_t obj, int slot, int argc, const uint32_t *args,
                          uint32_t *eax, double *st0) {
    uint32_t vt = MEM32(obj);
    uint32_t fva = MEM32(vt + 4u * (uint32_t)slot);
    *eax = 0; if (st0) *st0 = 0.0;
    if (!fva) return 0;
    uint32_t zargs[16] = { 0 };
    if (!args) { zargs[0] = obj; args = zargs; }   /* `this` is arg 0 */
    uint32_t sp = g_stack + 0x800u;
    for (int i = argc - 1; i >= 0; i--) { sp -= 4; MEM32(sp) = args[i]; }
    sp -= 4; MEM32(sp) = 0xDEADBEEFu;                 /* fake return address */
    mach_enter();
    g_esp = sp;
    int top0 = g_fp_top;
    recomp_func_t f = recomp_lookup(fva);
    if (!f) f = recomp_lookup_import(fva);
    if (f) f();
    *eax = g_eax;
    if (st0) *st0 = g_st[0];
    uint32_t after = g_esp;
    g_fp_top = top0;
    mach_leave();
    return after;
}
static uint32_t vfun(uint32_t obj, int slot, int argc, const uint32_t *a) {
    uint32_t ea; double st;
    raw_vcall(obj, slot, argc, a, &ea, &st);
    return ea;
}
static double vfloat(uint32_t obj, int slot, int argc, const uint32_t *a) {
    uint32_t ea; double st;
    raw_vcall(obj, slot, argc, a, &ea, &st);
    return st;
}

/* Public order (Wine include/d3dx9mesh.h), argc INCLUDES `this`. */
static const char *const si_names[ID3DXSKININFO_SLOTS] = {
    "QueryInterface","AddRef","Release","SetBoneInfluence","SetBoneVertexInfluence",
    "GetNumBoneInfluences","GetBoneInfluence","GetBoneVertexInfluence",
    "GetMaxVertexInfluences","GetNumBones","FindBoneVertexInfluenceIndex",
    "GetMaxFaceInfluences","SetMinBoneInfluence","GetMinBoneInfluence",
    "SetBoneName","GetBoneName","SetBoneOffsetMatrix","GetBoneOffsetMatrix",
    "Clone","Remap","SetFVF","SetDeclaration","GetFVF","GetDeclaration",
    "UpdateSkinnedMesh","ConvertToBlendedMesh","ConvertToIndexedBlendedMesh"
};
static const int si_argc[ID3DXSKININFO_SLOTS] = {
    3,1,1,5,4,2,4,5,2,1,4,4,2,1,3,2,3,2,2,3,2,2,1,2,5,11,12
};

static void abi_check(uint32_t obj) {
    uint32_t ea; double st;
    char full[160], desc[200];
    uint32_t vt = MEM32(obj);
    for (int i = 0; i < ID3DXSKININFO_SLOTS; i++) {
        snprintf(full, sizeof full, "ID3DXSkinInfo::%s", si_names[i]);
        snprintf(desc, sizeof desc, "slot %d %s -> %s", i, si_names[i], full);
        ok(desc, MEM32(vt + 4u * (uint32_t)i) == hle_resolve(full));
        uint32_t args[16] = { 0 };
        args[0] = obj;                              /* every slot receives `this` */
        uint32_t after = raw_vcall(obj, i, si_argc[i], args, &ea, &st);
        snprintf(desc, sizeof desc, "slot %d %s esp restored (argc %d)", i, si_names[i], si_argc[i]);
        ok(desc, after == g_stack + 0x800u);
    }
    snprintf(desc, sizeof desc, "ID3DXSkinInfo vtable ends after %d slots", ID3DXSKININFO_SLOTS);
    ok(desc, MEM32(vt + 4u * (uint32_t)ID3DXSKININFO_SLOTS) == 0u);
}

/* A minimal stream-0 FLOAT3 POSITION declaration, plus the end marker. */
static uint32_t make_decl_xyz(void) {
    uint32_t d = va0(2u * 8u);
    MEM16(d) = 0; MEM16(d + 2u) = 0; MEM8(d + 4u) = 2; MEM8(d + 5u) = 0;
    MEM8(d + 6u) = 0; MEM8(d + 7u) = 0;              /* POSITION FLOAT3 */
    MEM16(d + 8u) = 0xFF; MEM8(d + 12u) = 17;        /* D3DDECL_END */
    return d;
}

static uint32_t create_xyz(uint32_t nv, uint32_t nb) {
    uint32_t out = va0(4);
    uint32_t a[4] = { nv, make_decl_xyz(), nb, out };
    ok("D3DXCreateSkinInfo(XYZ) succeeds", call("D3DXCreateSkinInfo", 4, a) == D3D_OK_V && MEM32(out) != 0u);
    return MEM32(out);
}
static uint32_t create_fvf(uint32_t nv, uint32_t fvf, uint32_t nb) {
    uint32_t out = va0(4);
    uint32_t a[4] = { nv, fvf, nb, out };
    ok("D3DXCreateSkinInfoFVF succeeds", call("D3DXCreateSkinInfoFVF", 4, a) == D3D_OK_V && MEM32(out) != 0u);
    return MEM32(out);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    recomp_host_init();
    void *stack = hle_alloc(0x1000);
    g_stack = hle_va(stack);
    win32hle_register(win32hle_d3dx9);

    /* ------------------------- factory validation ------------------------- */
    {
        uint32_t a[4] = { 2, make_decl_xyz(), 1, 0 };
        ok("CreateSkinInfo null out -> INVALIDCALL", call("D3DXCreateSkinInfo", 4, a) == INVALID_V);
        a[3] = va0(4);
        a[0] = 0;
        ok("CreateSkinInfo zero vertices -> INVALIDCALL", call("D3DXCreateSkinInfo", 4, a) == INVALID_V);
        a[0] = 2; a[1] = 0;
        ok("CreateSkinInfo null declaration -> INVALIDCALL", call("D3DXCreateSkinInfo", 4, a) == INVALID_V);
        a[1] = make_decl_xyz(); a[2] = 0;
        ok("CreateSkinInfo zero bones -> INVALIDCALL", call("D3DXCreateSkinInfo", 4, a) == INVALID_V);

        uint32_t b[4] = { 2, 0xDEADBEEFu, 1, va0(4) };
        ok("CreateSkinInfoFVF bad FVF -> INVALIDCALL", call("D3DXCreateSkinInfoFVF", 4, b) == INVALID_V);
        b[3] = 0; b[1] = 0x2;
        ok("CreateSkinInfoFVF null out -> INVALIDCALL", call("D3DXCreateSkinInfoFVF", 4, b) == INVALID_V);
    }

    uint32_t si = create_xyz(4, 2);
    uint32_t fv = create_fvf(1, 0x2u | 0x10u, 2);     /* XYZ | NORMAL */
    if (!si || !fv) { printf("FAIL object creation\n"); return 1; }

    /* ---------------------- COM checklist: every slot --------------------- */
    vfun(si, 1, 1, (uint32_t[]){ si });                /* refcount 2 before Release slot */
    abi_check(si);
    vfun(fv, 1, 1, (uint32_t[]){ fv });
    abi_check(fv);
    ok("GetNumBones == 2", vfun(si, 9, 1, (uint32_t[]){ si }) == 2u);
    ok("GetFVF == 0 for declaration-based object", vfun(si, 22, 1, (uint32_t[]){ si }) == 0u);
    ok("GetFVF == XYZ|NORMAL", vfun(fv, 22, 1, (uint32_t[]){ fv }) == (0x2u | 0x10u));

    /* ------------------------- influence storage -------------------------- */
    uint32_t v0 = va0(8), w0 = va0(8), v1 = va0(8), w1 = va0(8);
    MEM32(v0) = 0; MEM32(v0 + 4u) = 1;
    MEMF(w0) = 0.25f; MEMF(w0 + 4u) = 0.75f;
    MEM32(v1) = 1; MEM32(v1 + 4u) = 2;
    MEMF(w1) = 0.5f; MEMF(w1 + 4u) = 0.5f;
    ok("SetBoneInfluence bone0", vfun(si, 3, 5, (uint32_t[]){ si, 0, 2, v0, w0 }) == D3D_OK_V);
    ok("SetBoneInfluence bone1", vfun(si, 3, 5, (uint32_t[]){ si, 1, 2, v1, w1 }) == D3D_OK_V);
    ok("SetBoneInfluence out of range -> INVALIDCALL",
       vfun(si, 3, 5, (uint32_t[]){ si, 2, 2, v0, w0 }) == INVALID_V);
    ok("GetNumBoneInfluences bone0 == 2", vfun(si, 5, 2, (uint32_t[]){ si, 0 }) == 2u);
    ok("GetNumBoneInfluences bone1 == 2", vfun(si, 5, 2, (uint32_t[]){ si, 1 }) == 2u);

    uint32_t rv = va0(8), rw = va0(8);
    ok("GetBoneInfluence bone0", vfun(si, 6, 4, (uint32_t[]){ si, 0, rv, rw }) == D3D_OK_V);
    ok("GetBoneInfluence bone0 vertices {0,1}", MEM32(rv) == 0u && MEM32(rv + 4u) == 1u);
    ok("GetBoneInfluence bone0 weights {0.25,0.75}", cf(MEMF(rw),0.25f) && cf(MEMF(rw+4u),0.75f));

    uint32_t one_w = va0(4), one_v = va0(4);
    ok("GetBoneVertexInfluence bone0 idx1",
       vfun(si, 7, 5, (uint32_t[]){ si, 0, 1, one_w, one_v }) == D3D_OK_V);
    ok("GetBoneVertexInfluence bone0 idx1 -> v=1,w=0.75", MEM32(one_v) == 1u && cf(MEMF(one_w),0.75f));

    uint32_t idx_va = va0(4); MEM32(idx_va) = 0xDEADBEEFu;
    ok("FindBoneVertexInfluenceIndex bone1 vertex2 -> 1",
       vfun(si, 10, 4, (uint32_t[]){ si, 1, 2, idx_va }) == D3D_OK_V && MEM32(idx_va) == 1u);
    MEM32(idx_va) = 0xDEADBEEFu;
    ok("FindBoneVertexInfluenceIndex bone0 vertex2 -> E_FAIL",
       vfun(si, 10, 4, (uint32_t[]){ si, 0, 2, idx_va }) == EFAIL_V);
    uint32_t maxinf_va = va0(4);
    ok("GetMaxVertexInfluences writes 2",
       vfun(si, 8, 2, (uint32_t[]){ si, maxinf_va }) == D3D_OK_V && MEM32(maxinf_va) == 2u);

    /* SetBoneVertexInfluence writes through to the stored weight. */
    float w = 0.4f; uint32_t wbits; memcpy(&wbits, &w, 4);
    ok("SetBoneVertexInfluence bone0 idx0 = 0.4",
       vfun(si, 4, 4, (uint32_t[]){ si, 0, 0, wbits }) == D3D_OK_V);
    ok("GetBoneVertexInfluence bone0 idx0 -> 0.4",
       vfun(si, 7, 5, (uint32_t[]){ si, 0, 0, one_w, one_v }) == D3D_OK_V && cf(MEMF(one_w),0.4f));

    /* ------------------------ scalar / string state ----------------------- */
    float mn = 0.01f; uint32_t mnb; memcpy(&mnb, &mn, 4);
    ok("SetMinBoneInfluence", vfun(si, 12, 2, (uint32_t[]){ si, mnb }) == D3D_OK_V);
    ok("GetMinBoneInfluence == 0.01", fabs(vfloat(si, 13, 1, (uint32_t[]){ si }) - 0.01) < 0.0002);

    uint32_t name = va0(8); memcpy(hle_ptr(name), "root", 5);
    ok("SetBoneName bone0", vfun(si, 14, 3, (uint32_t[]){ si, 0, name }) == D3D_OK_V);
    uint32_t nb_va = vfun(si, 15, 2, (uint32_t[]){ si, 0 });
    ok("GetBoneName bone0 -> \"root\"", nb_va && strcmp((const char *)hle_ptr(nb_va), "root") == 0);

    uint32_t m = va0(64); put_ident(m);
    MEMF(m + 48u) = 3.5f; MEMF(m + 52u) = -1.25f;   /* translation row */
    ok("SetBoneOffsetMatrix bone1", vfun(si, 16, 3, (uint32_t[]){ si, 1, m }) == D3D_OK_V);
    uint32_t om = vfun(si, 17, 2, (uint32_t[]){ si, 1 });
    ok("GetBoneOffsetMatrix bone1 -> translation",
       om && cf(MEMF(om + 48u),3.5f) && cf(MEMF(om + 52u),-1.25f) && cf(MEMF(om),1.0f));

    /* ------------------------------- Clone -------------------------------- */
    uint32_t clone_out = va0(4);
    ok("Clone succeeds", vfun(si, 18, 2, (uint32_t[]){ si, clone_out }) == D3D_OK_V && MEM32(clone_out) != 0u);
    uint32_t cl = MEM32(clone_out);
    ok("Clone: GetNumBones == 2", vfun(cl, 9, 1, (uint32_t[]){ cl }) == 2u);
    ok("Clone: GetNumBoneInfluences bone0 == 2", vfun(cl, 5, 2, (uint32_t[]){ cl, 0 }) == 2u);
    ok("Clone: influence weight copied",
       vfun(cl, 7, 5, (uint32_t[]){ cl, 0, 0, one_w, one_v }) == D3D_OK_V && cf(MEMF(one_w),0.4f));
    uint32_t cl_om = vfun(cl, 17, 2, (uint32_t[]){ cl, 1 });
    ok("Clone: offset matrix copied", cl_om && cf(MEMF(cl_om + 48u),3.5f));
    uint32_t cl_name = vfun(cl, 15, 2, (uint32_t[]){ cl, 0 });
    ok("Clone: bone name copied", cl_name && strcmp((const char *)hle_ptr(cl_name), "root") == 0);

    /* --------------------------- FVF / declaration ------------------------ */
    ok("SetFVF XYZ", vfun(si, 20, 2, (uint32_t[]){ si, 0x2u }) == D3D_OK_V);
    ok("GetFVF == XYZ", vfun(si, 22, 1, (uint32_t[]){ si }) == 0x2u);
    ok("SetDeclaration -> XYZ declaration", vfun(si, 21, 2, (uint32_t[]){ si, make_decl_xyz() }) == D3D_OK_V);
    ok("GetFVF == 0 after SetDeclaration", vfun(si, 22, 1, (uint32_t[]){ si }) == 0u);
    uint32_t got_decl = va0(2u * 8u + 8u);
    memset(hle_ptr(got_decl), 0xCD, 2u * 8u + 8u);
    ok("GetDeclaration succeeds", vfun(si, 23, 2, (uint32_t[]){ si, got_decl }) == D3D_OK_V);
    ok("GetDeclaration POSITION element", MEM16(got_decl) == 0 && MEM8(got_decl + 4u) == 2 && MEM8(got_decl + 6u) == 0);
    ok("GetDeclaration end marker", MEM16(got_decl + 8u) == 0xFF);

    /* --------------------------- UpdateSkinnedMesh ------------------------ */
    {
        /* XYZ|NORMAL, one vertex at (1,2,3), normal (1,0,0). */
        uint32_t pfv = create_fvf(1, 0x2u | 0x10u, 2);
        uint32_t src = va0(24), dst = va0(24);
        MEMF(src) = 1.0f; MEMF(src + 4u) = 2.0f; MEMF(src + 8u) = 3.0f;
        MEMF(src + 12u) = 1.0f; MEMF(src + 16u) = 0.0f; MEMF(src + 20u) = 0.0f;
        memset(hle_ptr(dst), 0x7F, 24u);

        uint32_t bt = va0(128);
        put_ident(bt);                                            /* bone 0: x-scale 2, tx 10 */
        MEMF(bt) = 2.0f; MEMF(bt + 48u) = 10.0f;
        put_ident(bt + 64u);                                      /* bone 1: ty 20 */
        MEMF(bt + 64u + 52u) = 20.0f;
        uint32_t it = va0(128);
        put_ident(it);                                            /* bone 0 inv-transpose: x 0.5 */
        MEMF(it) = 0.5f;
        put_ident(it + 64u);                                      /* bone 1 inv-transpose: identity */

        uint32_t vv = va0(4), ww = va0(4);
        MEM32(vv) = 0; MEMF(ww) = 0.5f;
        ok("Update: SetBoneInfluence bone0", vfun(pfv, 3, 5, (uint32_t[]){ pfv, 0, 1, vv, ww }) == D3D_OK_V);
        ok("Update: SetBoneInfluence bone1", vfun(pfv, 3, 5, (uint32_t[]){ pfv, 1, 1, vv, ww }) == D3D_OK_V);
        ok("UpdateSkinnedMesh succeeds",
           vfun(pfv, 24, 5, (uint32_t[]){ pfv, bt, it, src, dst }) == D3D_OK_V);
        ok("UpdateSkinnedMesh position == (6.5,12,3)", v3(dst, 6.5f, 12.0f, 3.0f));
        ok("UpdateSkinnedMesh normal == (0.75,0,0)", v3(dst + 12u, 0.75f, 0.0f, 0.0f));

        /* With NULL inverse-transpose, normals use the bone transform instead. */
        memset(hle_ptr(dst), 0x7F, 24u);
        ok("UpdateSkinnedMesh with NULL inverse-transpose",
           vfun(pfv, 24, 5, (uint32_t[]){ pfv, bt, 0u, src, dst }) == D3D_OK_V);
        ok("NULL inverse: position unchanged (6.5,12,3)", v3(dst, 6.5f, 12.0f, 3.0f));
        ok("NULL inverse: normal == (1.5,0,0)", v3(dst + 12u, 1.5f, 0.0f, 0.0f));
    }

    printf("d3dx9_skin_selftest: %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
