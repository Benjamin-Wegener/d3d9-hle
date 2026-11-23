/*
 * d3dx9_effect_selftest.c - ID3DXEffectPool: create the guest COM object, walk
 * its vtable, set/get every value type with the stdcall ABI, check the stack
 * balance of every method, and check the two-pass error path.
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3dx9_effect_hle.h"

#define ENTRY_VA 0x00401000u
#define D3DERR_INVALIDCALL 0x8876086Cu

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }

static uint32_t esp0;
#define XCALL(va, n, ...)                                                    \
    do {                                                                     \
        uint32_t _xa[8] = { __VA_ARGS__ };                                   \
        esp0 = esp;                                                          \
        for (int _xi = (int)(n) - 1; _xi >= 0; _xi--) PUSH32(esp, _xa[_xi]); \
        RECOMP_ICALL(va);                                                    \
        ok(#va " stack", esp == esp0);                                       \
    } while (0)

static uint32_t V_create;
static uint32_t g_out, g_out2, g_name_time, g_name_count, g_name_flag, g_name_light;
static uint32_t g_name_world, g_name_tex, g_name_missing, g_name_base, g_name_lighting;
static uint32_t g_name_tech0, g_name_tech1;
static uint32_t g_val, g_vec, g_mat;

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float as_float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t method(uint32_t obj, int i) { return MEM32(MEM32(obj) + 4u * (uint32_t)i); }
static uint32_t guest_str(const char *s) { return hle_guest_strdup(s); }

/* Technique/pass state machine on a synthetic effect description. */
static void effect_tests(uint32_t pool) {
    uint32_t _cf = 0; uint32_t _flag_k = FK_NONE; uint32_t _flag_a = 0, _flag_b = 0;
    uint32_t effect = hle_d3d9fx_effect_new(pool), em[17], tech = 0, pass = 0, h = 0, stats, commits = 0;
    const char *passes[] = { "Ambient", "Lighting" };
    ok("effect object", effect != 0);
    if (!effect) return;
    ok("add technique", hle_d3d9fx_effect_add_tech(effect, "Base", passes, 2) == 1);
    /* slots of the public 79-slot ID3DXEffect layout (d3dx9effect.h) */
    static const int EMAP[17] = { 0, 1, 2, 63 /*Begin*/, 67 /*End*/, 64 /*BeginPass*/, 66 /*EndPass*/,
                                  65 /*CommitChanges*/, 13 /*GetTechniqueByName*/, 15 /*GetPassByName*/,
                                  58 /*SetTechnique*/, 59 /*GetCurrentTechnique*/, 9 /*GetParameterByName*/,
                                  30 /*SetFloat*/, 31 /*GetFloat*/, 52 /*SetTexture*/, 53 /*GetTexture*/ };
    for (int i = 0; i < 17; i++) em[i] = method(effect, EMAP[i]);
    ok("all effect methods present", em[0] && em[3] && em[7] && em[11] && em[12] && em[16]);

    XCALL(em[8], 2, effect, g_name_base);
    ok("GetTechniqueByName", eax != 0u);
    tech = eax;
    XCALL(em[11], 1, effect);
    ok("no current technique", eax == 0u);
    XCALL(em[10], 2, effect, tech);
    ok("SetTechnique", eax == 0u);
    XCALL(em[11], 1, effect);
    ok("GetCurrentTechnique", eax == tech);
    XCALL(em[9], 3, effect, tech, g_name_lighting);
    ok("GetPassByName", eax != 0u);
    pass = eax;

    XCALL(em[3], 3, effect, g_val, 0u);
    ok("Begin", eax == 0u);
    ok("Begin pass count", MEM32(g_val) == 2u);
    XCALL(em[5], 2, effect, 1u);
    ok("BeginPass", eax == 0u);
    XCALL(em[6], 1, effect);
    ok("EndPass", eax == 0u);
    XCALL(em[7], 1, effect);
    ok("CommitChanges", eax == 0u);
    XCALL(em[4], 1, effect);
    ok("End", eax == 0u);
    XCALL(em[5], 2, effect, 0u);
    ok("BeginPass outside Begin fails", eax == D3DERR_INVALIDCALL);
    XCALL(em[8], 2, effect, g_name_missing);
    ok("unknown technique returns 0", eax == 0u);

    XCALL(em[12], 3, effect, 0u, g_name_time);   /* (this, parent handle, name) */
    ok("GetParameterByName via effect", eax != 0u);
    h = eax;
    MEM32(g_val) = 0;
    XCALL(em[14], 3, effect, h, g_val);
    ok("effect GetFloat", eax == 0u && as_float(MEM32(g_val)) == 1.5f);
    XCALL(em[13], 3, effect, h, fbits(2.5f));
    ok("effect SetFloat", eax == 0u);
    XCALL(em[15], 3, effect, h, 0x99u);
    XCALL(em[16], 3, effect, h, g_val);
    ok("effect texture roundtrip", eax == 0u && MEM32(g_val) == 0x99u);

    stats = hle_d3d9fx_effect_stats(effect, &commits);
    ok("effect stats", (stats >> 16) == 1u && commits == 1u && (stats & 1u) == 0u);
    XCALL(em[2], 1, effect);
    ok("effect final Release", eax == 0u);
}


/* ---------------------------------------------------------------------------
 * Device shader objects: made once per effect and pass, freed with the effect.
 * The device is a fake COM object that counts what the effect creates; the
 * expected counts are worked out by hand (2 passes x (VS + PS) = 4 objects).
 * ------------------------------------------------------------------------- */
static uint32_t g_created, g_live, g_cur_vs, g_cur_ps, g_dev_ref = 1, g_shader_vt;

static void m_dev_addref(void) { g_dev_ref++; RET(g_dev_ref, 1); }
static void m_dev_release(void) { g_dev_ref--; RET(g_dev_ref, 1); }
static void m_other(void) { RET(0u, 1); }
static void m_sh_addref(void) { uint32_t o = A32(0); MEM32(o + 4)++; RET(MEM32(o + 4), 1); }
static void m_sh_release(void) {
    uint32_t o = A32(0), n = --MEM32(o + 4);
    if (!n) { g_live--; hle_com_free(o); }
    RET(n, 1);
}
static void m_create_shader(void) {                /* (this, pFunction, ppShader) */
    uint32_t o = hle_com_new(g_shader_vt, 16);
    MEM32(A32(2)) = o;
    g_created++; g_live++;
    RET(0u, 3);
}
static void m_set_vs(void) { g_cur_vs = A32(1); RET(0u, 2); }
static void m_set_ps(void) { g_cur_ps = A32(1); RET(0u, 2); }
static void m_get_vs(void) { if (g_cur_vs) MEM32(g_cur_vs + 4)++; MEM32(A32(1)) = g_cur_vs; RET(0u, 2); }
static void m_get_ps(void) { if (g_cur_ps) MEM32(g_cur_ps + 4)++; MEM32(A32(1)) = g_cur_ps; RET(0u, 2); }
static const win32hle_shim fake_dev_shims[] = {
    { "FxFake::AddRef", m_dev_addref }, { "FxFake::Release", m_dev_release }, { "FxFake::Other", m_other },
    { "FxFake::ShAddRef", m_sh_addref }, { "FxFake::ShRelease", m_sh_release },
    { "FxFake::CreateShader", m_create_shader },
    { "FxFake::SetVS", m_set_vs }, { "FxFake::SetPS", m_set_ps },
    { "FxFake::GetVS", m_get_vs }, { "FxFake::GetPS", m_get_ps }, { 0, 0 }
};

static void shader_lifetime_tests(void) {
    win32hle_register(fake_dev_shims);
    uint32_t *sv = (uint32_t *)hle_alloc(16);
    sv[0] = sv[3] = hle_resolve("FxFake::Other"); sv[1] = hle_resolve("FxFake::ShAddRef"); sv[2] = hle_resolve("FxFake::ShRelease");
    g_shader_vt = hle_va(sv);
    uint32_t *vt = (uint32_t *)hle_alloc(4u * 119u);
    for (int i = 0; i < 119; i++) vt[i] = hle_resolve("FxFake::Other");
    vt[1] = hle_resolve("FxFake::AddRef"); vt[2] = hle_resolve("FxFake::Release");
    vt[91] = vt[106] = hle_resolve("FxFake::CreateShader");     /* CreateVertexShader, CreatePixelShader */
    vt[92] = hle_resolve("FxFake::SetVS"); vt[93] = hle_resolve("FxFake::GetVS");
    vt[107] = hle_resolve("FxFake::SetPS"); vt[108] = hle_resolve("FxFake::GetPS");
    uint32_t dev = hle_com_new(hle_va(vt), 16);

    uint32_t effect = hle_d3d9fx_effect_new(hle_d3d9fx_pool_new());
    hle_d3d9fx_effect_set_device(effect, dev);
    ok("the effect holds a reference to its device", g_dev_ref == 2u);
    const char *passes[] = { "A", "B" };
    uint32_t vs[2] = { 0x1000u, 0x1100u }, ps[2] = { 0x2000u, 0x2100u };
    ok("technique with 2 passes", hle_d3d9fx_effect_add_tech_ct(effect, "T", passes, 2, vs, ps, NULL, NULL) == 1);
    uint32_t n = hle_va(hle_alloc(4));
    uint32_t created1 = 0, live1 = 0;
    for (int i = 0; i < 1000; i++) {
        hle_call_guest(method(effect, 63), 3, (uint32_t[]){ effect, n, 0 });              /* Begin */
        for (uint32_t p = 0; p < 2; p++) {
            hle_call_guest(method(effect, 64), 2, (uint32_t[]){ effect, p });             /* BeginPass */
            hle_call_guest(method(effect, 66), 1, (uint32_t[]){ effect });                /* EndPass */
        }
        hle_call_guest(method(effect, 67), 1, (uint32_t[]){ effect });                    /* End */
        if (i == 0) { created1 = g_created; live1 = g_live; }
    }
    ok("the first frame makes 4 shader objects (2 passes x VS+PS)", created1 == 4u && live1 == 4u);
    ok("1000 frames of 2 passes still 4 created, 4 live", g_created == 4u && g_live == 4u);
    uint32_t live = 0;
    ok("effect statistics agree", hle_d3d9fx_effect_shader_stats(effect, &live) == 4u && live == 4u);
    ok("the last pass left its shaders bound", g_cur_vs != 0 && g_cur_ps != 0);

    /* device lost: shaders are dropped and unbound, made again by the next BeginPass */
    hle_d3d9fx_effect_on_lost(effect);
    ok("lost device: all shader objects freed", g_live == 0u && g_created == 4u);
    ok("lost device: nothing of ours stays bound", g_cur_vs == 0u && g_cur_ps == 0u);
    hle_call_guest(method(effect, 63), 3, (uint32_t[]){ effect, n, 0 });
    hle_call_guest(method(effect, 64), 2, (uint32_t[]){ effect, 0u });
    ok("after the reset BeginPass makes the pass's shaders again", g_created == 6u && g_live == 2u);
    hle_call_guest(method(effect, 66), 1, (uint32_t[]){ effect });
    hle_call_guest(method(effect, 67), 1, (uint32_t[]){ effect });

    /* a reference the application took on a shader keeps it alive past the effect */
    uint32_t held = g_cur_ps;
    hle_call_guest(MEM32(MEM32(held) + 4u), 1, (uint32_t[]){ held });                    /* application AddRef */
    uint32_t r = hle_call_guest(method(effect, 2), 1, (uint32_t[]){ effect });           /* last Release */
    ok("effect Release returns 0", r == 0u);
    ok("Release freed the shaders nobody else holds, kept the shared one", g_live == 1u);
    ok("Release unbound the effect's shaders from the device", g_cur_vs == 0u && g_cur_ps == 0u);
    ok("Release let go of the device", g_dev_ref == 1u);
    hle_call_guest(MEM32(MEM32(held) + 8u), 1, (uint32_t[]){ held });                    /* application Release */
    ok("after the application's Release every shader is free", g_live == 0u);
}

/* Minimal fake device for CloneEffect: only AddRef/Release are called by the HLE. */
static void m_clone_qi(void) { RET(0u, 3); }
static void m_clone_addref(void) { RET(1u, 1); }
static void m_clone_release(void) { RET(0u, 1); }
static const win32hle_shim clone_dev_shims[] = {
    { "CloneFake::QueryInterface", m_clone_qi },
    { "CloneFake::AddRef", m_clone_addref },
    { "CloneFake::Release", m_clone_release },
    { 0, 0 }
};

/* The slots added for the effect-pool task. All expected values below were
 * written down before running the code: handle identity, 7.25f restored by a
 * parameter block, S_FALSE after the last technique, and so on. */
static void effect_pool_slot_tests(uint32_t pool) {
    uint32_t _cf = 0; uint32_t _flag_k = FK_NONE; uint32_t _flag_a = 0, _flag_b = 0;
    uint32_t a = hle_d3d9fx_effect_new(pool), b = hle_d3d9fx_effect_new(pool);
    uint32_t ha = 0, hb = 0, t0 = 0, t1 = 0, pb = 0, clone = 0, dev;
    const char *passes[] = { "P0", "P1" };
    ok("two effects for one pool", a != 0 && b != 0);
    if (!a || !b) return;
    win32hle_register(clone_dev_shims);
    dev = hle_com_new(hle_com_vtable(clone_dev_shims), 8);
    ok("fake device for CloneEffect", dev != 0);

    /* A shared pool parameter must be the very same handle in both effects. */
    XCALL(method(a, 9), 3, a, 0u, g_name_time);
    ha = eax;
    XCALL(method(b, 9), 3, b, 0u, g_name_time);
    hb = eax;
    ok("both effects resolve the shared parameter", ha != 0 && ha == hb);
    XCALL(method(a, 30), 3, a, ha, fbits(3.5f));
    ok("shared parameter set through effect A", eax == 0u);
    MEM32(g_val) = 0;
    XCALL(method(b, 31), 3, b, hb, g_val);
    ok("effect B reads 3.5f from the shared pool", eax == 0u && as_float(MEM32(g_val)) == 3.5f);

    /* Two techniques, then their handles/validation/iteration. */
    ok("technique T0 added", hle_d3d9fx_effect_add_tech(a, "T0", passes, 2) == 1);
    ok("technique T1 added", hle_d3d9fx_effect_add_tech(a, "T1", passes, 2) == 2);
    XCALL(method(a, 13), 2, a, g_name_tech0);
    t0 = eax;
    XCALL(method(a, 13), 2, a, g_name_tech1);
    t1 = eax;
    ok("technique handles are distinct", t0 != 0 && t1 != 0 && t0 != t1);
    XCALL(method(a, 60), 2, a, t0);
    ok("ValidateTechnique accepts a real handle", eax == 0u);
    XCALL(method(a, 60), 2, a, 0xDEADBEEFu);
    ok("ValidateTechnique rejects a bogus handle", eax == D3DERR_INVALIDCALL);
    MEM32(g_out2) = 0xAAAAAAAAu;
    XCALL(method(a, 61), 3, a, 0u, g_out2);
    ok("FindNextValidTechnique(NULL) returns T0", eax == 0u && MEM32(g_out2) == t0);
    XCALL(method(a, 61), 3, a, t0, g_out2);
    ok("FindNextValidTechnique(T0) returns T1", eax == 0u && MEM32(g_out2) == t1);
    MEM32(g_out2) = 0xAAAAAAAAu;
    XCALL(method(a, 61), 3, a, t1, g_out2);
    ok("no further technique gives S_FALSE and NULL", eax == 1u && MEM32(g_out2) == 0u);
    XCALL(method(a, 61), 3, a, 0xDEADBEEFu, g_out2);
    ok("FindNextValidTechnique rejects a bogus handle", eax == D3DERR_INVALIDCALL);

    /* Begin/set/End records 7.25f; after changing it to 9.5f, Apply must
     * restore 7.25f in both effects because the pool is shared. */
    XCALL(method(a, 73), 1, a);
    ok("BeginParameterBlock", eax == 0u);
    XCALL(method(a, 73), 1, a);
    ok("a second BeginParameterBlock fails", eax == D3DERR_INVALIDCALL);
    XCALL(method(a, 30), 3, a, ha, fbits(7.25f));
    ok("recorded SetFloat", eax == 0u);
    XCALL(method(a, 74), 1, a);
    pb = eax;
    ok("EndParameterBlock returns a handle", pb != 0u);
    XCALL(method(a, 74), 1, a);
    ok("EndParameterBlock without Begin returns 0", eax == 0u);
    XCALL(method(a, 30), 3, a, ha, fbits(9.5f));
    ok("change after End", eax == 0u);
    XCALL(method(a, 75), 2, a, pb);
    ok("ApplyParameterBlock", eax == 0u);
    MEM32(g_val) = 0;
    XCALL(method(b, 31), 3, b, hb, g_val);
    ok("effect B sees the restored 7.25f", eax == 0u && as_float(MEM32(g_val)) == 7.25f);
    XCALL(method(a, 75), 2, a, 0xDEADBEEFu);
    ok("ApplyParameterBlock rejects a bogus handle", eax == D3DERR_INVALIDCALL);
    XCALL(method(a, 76), 2, a, pb);
    ok("DeleteParameterBlock", eax == 0u);
    XCALL(method(a, 75), 2, a, pb);
    ok("ApplyParameterBlock after Delete fails", eax == D3DERR_INVALIDCALL);

    /* An empty block is not applicable; Begin/End must still balance. */
    XCALL(method(a, 73), 1, a);
    ok("BeginParameterBlock for the empty block", eax == 0u);
    XCALL(method(a, 74), 1, a);
    pb = eax;
    ok("empty block has a handle", pb != 0u);
    XCALL(method(a, 75), 2, a, pb);
    ok("empty block cannot be applied", eax == D3DERR_INVALIDCALL);
    XCALL(method(a, 76), 2, a, pb);
    ok("empty block deleted", eax == 0u);

    /* CloneEffect writes an out handle, returns an HRESULT and keeps the pool. */
    MEM32(g_out2) = 0;
    XCALL(method(a, 77), 3, a, 0u, g_out2);
    ok("CloneEffect with no device fails", eax == D3DERR_INVALIDCALL && MEM32(g_out2) == 0u);
    XCALL(method(a, 77), 3, a, dev, 0u);
    ok("CloneEffect with no out pointer fails", eax == D3DERR_INVALIDCALL);
    MEM32(g_out2) = 0;
    XCALL(method(a, 77), 3, a, dev, g_out2);
    ok("CloneEffect returns D3D_OK", eax == 0u);
    clone = MEM32(g_out2);
    ok("clone is a different object", clone != 0 && clone != a);
    XCALL(method(clone, 9), 3, clone, 0u, g_name_time);
    ok("clone resolves the same pool parameter", eax == ha);
    XCALL(method(clone, 13), 2, clone, g_name_tech0);
    ok("clone kept the technique", eax != 0u);
    MEM32(g_out2) = 0;
    XCALL(method(clone, 68), 2, clone, g_out2);
    ok("clone reports the requested device", eax == 0u && MEM32(g_out2) == dev);

    /* SetRawValue(this, param, data, byte_offset, bytes). */
    MEM32(g_val) = fbits(4.75f);
    XCALL(method(a, 78), 5, a, ha, g_val, 0u, 4u);
    ok("SetRawValue writes 4.75f", eax == 0u);
    MEM32(g_val) = 0;
    XCALL(method(a, 31), 3, a, ha, g_val);
    ok("GetFloat sees 4.75f", eax == 0u && as_float(MEM32(g_val)) == 4.75f);
    MEM32(g_val) = fbits(4.75f);
    XCALL(method(a, 78), 5, a, ha, g_val, 4u, 4u);
    ok("SetRawValue rejects an out-of-range offset", eax == D3DERR_INVALIDCALL);
    XCALL(method(a, 78), 5, a, ha, g_val, 0u, 8u);
    ok("SetRawValue rejects too many bytes", eax == D3DERR_INVALIDCALL);
    XCALL(method(a, 78), 5, a, 0xDEADBEEFu, g_val, 0u, 4u);
    ok("SetRawValue rejects a bogus handle", eax == D3DERR_INVALIDCALL);
    /* The vector parameter was set to 1,2,3,4 by effect_tests. */
    XCALL(method(a, 9), 3, a, 0u, g_name_light);
    ha = eax;
    MEM32(g_val) = fbits(6.5f);
    XCALL(method(a, 78), 5, a, ha, g_val, 8u, 4u);
    ok("SetRawValue writes vector component 2", eax == 0u);
    memset(hle_ptr(g_vec), 0, 16);
    hle_call_guest(hle_resolve("d3dx9fx_host::PoolGetVector"), 3,
                   (uint32_t[]){ pool, g_name_light, g_vec });
    ok("vector is 1,2,6.5,4", as_float(MEM32(g_vec)) == 1.0f && as_float(MEM32(g_vec + 4)) == 2.0f &&
        as_float(MEM32(g_vec + 8)) == 6.5f && as_float(MEM32(g_vec + 12)) == 4.0f);

    /* The remaining effect slots requested by the task, and the honest
     * not-found answer of the annotation slot. */
    XCALL(method(a, 69), 1, a);
    ok("OnLostDevice via the guest vtable", eax == 0u);
    XCALL(method(a, 70), 1, a);
    ok("OnResetDevice via the guest vtable", eax == 0u);
    XCALL(method(a, 18), 3, a, 0u, 0u);   /* GetAnnotation: base slot 18 */
    ok("GetAnnotation reports not found", eax == 0u);

    XCALL(method(clone, 2), 1, clone);
    ok("clone Release", eax == 0u);
    XCALL(method(b, 2), 1, b);
    ok("effect B Release", eax == 0u);
    XCALL(method(a, 2), 1, a);
    ok("effect A Release", eax == 0u);
}

static void guest(void) {
    uint32_t _cf = 0; uint32_t _flag_k = FK_NONE; uint32_t _flag_a = 0, _flag_b = 0;
    uint32_t pool, m[15];
    RECOMP_ENTER(ENTRY_VA);

    XCALL(V_create, 1, g_out);
    ok("D3DXCreateEffectPool return", eax == 0u);
    pool = MEM32(g_out);
    ok("pool object", pool != 0u);
    if (!pool) return;
    ok("empty pool", hle_d3d9fx_pool_values(pool) == 0u);

    /* the guest-visible pool is IUnknown only (d3dx9effect.h); its parameter store
     * is driven through the host hooks */
    for (int i = 0; i < 3; i++) m[i] = method(pool, i);
    ok("IUnknown methods", m[0] && m[1] && m[2]);
    static const char *const HOOKS[12] = { "PoolSetFloat", "PoolGetFloat", "PoolSetInt", "PoolGetInt",
        "PoolSetBool", "PoolGetBool", "PoolSetVector", "PoolGetVector", "PoolSetMatrix", "PoolGetMatrix",
        "PoolSetTexture", "PoolGetTexture" };
    for (int i = 0; i < 12; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "d3dx9fx_host::%s", HOOKS[i]);
        m[3 + i] = hle_resolve(nm);
        ok(nm, m[3 + i] != 0);
    }

    {   /* IID_IUnknown */
        uint8_t *iid = (uint8_t *)hle_alloc(16);
        memset(iid, 0, 16); iid[8] = 0xC0; iid[15] = 0x46;
        MEM32(g_out) = 0;
        XCALL(m[0], 3, pool, hle_va(iid), g_out);
        ok("QueryInterface return", eax == 0u);
    }
    ok("QueryInterface object", MEM32(g_out) == pool);
    /* QueryInterface already took a reference: 1 -> 2. */
    XCALL(m[1], 1, pool);
    ok("AddRef return", eax == 3u);
    XCALL(m[2], 1, pool);
    ok("Release return", eax == 2u);

    XCALL(m[3], 3, pool, g_name_time, fbits(1.5f));
    ok("SetFloat return", eax == 0u);
    MEM32(g_val) = 0;
    XCALL(m[4], 3, pool, g_name_time, g_val);
    ok("GetFloat return", eax == 0u);
    ok("GetFloat value", as_float(MEM32(g_val)) == 1.5f);

    XCALL(m[5], 3, pool, g_name_count, 7u);
    ok("SetInt return", eax == 0u);
    MEM32(g_val) = 0;
    XCALL(m[6], 3, pool, g_name_count, g_val);
    ok("GetInt return", eax == 0u);
    ok("GetInt value", MEM32(g_val) == 7u);

    XCALL(m[7], 3, pool, g_name_flag, 1u);
    ok("SetBool return", eax == 0u);
    MEM32(g_val) = 0;
    XCALL(m[8], 3, pool, g_name_flag, g_val);
    ok("GetBool return", eax == 0u);
    ok("GetBool value", as_float(MEM32(g_val)) == 1.0f);

    MEM32(g_vec) = fbits(1.0f); MEM32(g_vec + 4) = fbits(2.0f);
    MEM32(g_vec + 8) = fbits(3.0f); MEM32(g_vec + 12) = fbits(4.0f);
    XCALL(m[9], 3, pool, g_name_light, g_vec);
    ok("SetVector return", eax == 0u);
    memset(hle_ptr(g_vec), 0, 16);
    XCALL(m[10], 3, pool, g_name_light, g_vec);
    ok("GetVector return", eax == 0u);
    ok("GetVector value", as_float(MEM32(g_vec)) == 1.0f && as_float(MEM32(g_vec + 12)) == 4.0f);

    for (int i = 0; i < 16; i++) MEM32(g_mat + 4 * i) = 0;
    MEM32(g_mat) = MEM32(g_mat + 20) = MEM32(g_mat + 40) = MEM32(g_mat + 60) = fbits(1.0f);
    XCALL(m[11], 3, pool, g_name_world, g_mat);
    ok("SetMatrix return", eax == 0u);
    memset(hle_ptr(g_mat), 0, 64);
    XCALL(m[12], 3, pool, g_name_world, g_mat);
    ok("GetMatrix return", eax == 0u);
    ok("GetMatrix value", as_float(MEM32(g_mat)) == 1.0f && as_float(MEM32(g_mat + 60)) == 1.0f);

    XCALL(m[13], 3, pool, g_name_tex, 0x1234u);
    ok("SetTexture return", eax == 0u);
    MEM32(g_val) = 0;
    XCALL(m[14], 3, pool, g_name_tex, g_val);
    ok("GetTexture return", eax == 0u);
    ok("GetTexture value", MEM32(g_val) == 0x1234u);

    XCALL(m[4], 3, pool, g_name_missing, g_val);
    ok("missing parameter errors", eax == D3DERR_INVALIDCALL);
    ok("six parameters stored", hle_d3d9fx_pool_values(pool) == 6u);

    effect_tests(pool);
    effect_pool_slot_tests(pool);

    XCALL(m[2], 1, pool);                               /* 2 -> 1 */
    ok("Release to one", eax == 1u);
    XCALL(m[2], 1, pool);                               /* 1 -> 0: last release frees */
    ok("final Release", eax == 0u);
}

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { ENTRY_VA, guest } };
const uint32_t recomp_dispatch_count = 1;

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9fx);
    V_create = hle_resolve("D3DXCreateEffectPool");
    ok("D3DXCreateEffectPool resolves", V_create != 0);

    g_out = hle_va(hle_alloc(4));
    g_out2 = hle_va(hle_alloc(4));
    g_name_time = guest_str("Time");
    g_name_count = guest_str("Count");
    g_name_flag = guest_str("Flag");
    g_name_light = guest_str("LightDir");
    g_name_world = guest_str("World");
    g_name_tex = guest_str("Diffuse");
    g_name_missing = guest_str("Missing");
    g_name_base = guest_str("Base");
    g_name_lighting = guest_str("Lighting");
    g_name_tech0 = guest_str("T0");
    g_name_tech1 = guest_str("T1");
    g_val = hle_va(hle_alloc(4));
    g_vec = hle_va(hle_alloc(16));
    g_mat = hle_va(hle_alloc(64));

    mach_enter();
    guest();
    shader_lifetime_tests();
    mach_leave();

    if (!fails) printf("d3dx9_effect_selftest: all checks passed\n");
    return fails != 0;
}
