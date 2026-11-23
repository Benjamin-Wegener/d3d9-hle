/*
 * d3dx9_anim_selftest.c - ABI and behaviour tests for the D3DX9 animation HLE.
 *
 * Slot order/count come from Wine's public include/d3dx9anim.h. Every slot of
 * both interfaces is invoked through the GUEST vtable with a raw call whose
 * resulting esp proves the shim's single RET popped exactly ret + argc*4 bytes
 * (argc includes `this`). Functional expectations are hand-computed constants
 * (linear scale/translation interpolation, quaternion slerp, weighted blend):
 * they are independent of the implementation and would be red if it regressed.
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
#include "d3dx9_math.h"
#include "d3dx9_anim.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

#define E_NOTIMPL_V 0x80004001u
#define WALK_PERIOD 10.0

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
static uint32_t g_stack;

static uint32_t va0(uint32_t n) { void *p = hle_alloc(n); memset(p, 0, n); return hle_va(p); }
static uint32_t str_va(const char *s) {
    uint32_t v = va0(strlen(s) + 1); memcpy(hle_ptr(v), s, strlen(s) + 1); return v;
}
static void put_vkey(uint32_t va, float t, float x, float y, float z) {
    MEMF(va) = t; MEMF(va+4) = x; MEMF(va+8) = y; MEMF(va+12) = z;
}
static void put_qkey(uint32_t va, float t, float x, float y, float z, float w) {
    MEMF(va) = t; MEMF(va+4) = x; MEMF(va+8) = y; MEMF(va+12) = z; MEMF(va+16) = w;
}
static int cf(float a, float b) { return fabsf(a - b) < 0.0008f; }
static int cd(double a, double b) { return fabs(a - b) < 0.0002; }
static int v3(uint32_t va, float x, float y, float z) {
    return cf(MEMF(va),x) && cf(MEMF(va+4),y) && cf(MEMF(va+8),z);
}
static int q4(uint32_t va, float x, float y, float z, float w) {
    return cf(MEMF(va),x) && cf(MEMF(va+4),y) && cf(MEMF(va+8),z) && cf(MEMF(va+12),w);
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
    uint32_t eax; double st;
    raw_vcall(obj, slot, argc, a, &eax, &st);
    return eax;
}
static double vdouble(uint32_t obj, int slot, int argc, const uint32_t *a) {
    uint32_t eax; double st;
    raw_vcall(obj, slot, argc, a, &eax, &st);
    return st;
}

/* Public order (Wine include/d3dx9anim.h). */
static const char *const kf_names[ID3DXKEYFRAMEDANIMATIONSET_SLOTS] = {
    "QueryInterface","AddRef","Release","GetName","GetPeriod","GetPeriodicPosition",
    "GetNumAnimations","GetAnimationNameByIndex","GetAnimationIndexByName","GetSRT",
    "GetCallback","GetPlaybackType","GetSourceTicksPerSecond","GetNumScaleKeys",
    "GetScaleKeys","GetScaleKey","SetScaleKey","GetNumRotationKeys","GetRotationKeys",
    "GetRotationKey","SetRotationKey","GetNumTranslationKeys","GetTranslationKeys",
    "GetTranslationKey","SetTranslationKey","GetNumCallbackKeys","GetCallbackKeys",
    "GetCallbackKey","SetCallbackKey","UnregisterScaleKey","UnregisterRotationKey",
    "UnregisterTranslationKey","RegisterAnimationSRTKeys","Compress","UnregisterAnimation"
};
static const int kf_argc[ID3DXKEYFRAMEDANIMATIONSET_SLOTS] = {
    3,1,1,1,1,3,1,3,3,7,6,1,1,2,3,4,4,2,3,4,4,2,3,4,4,1,2,3,3,3,3,3,9,5,2
};
static const char *const ac_names[ID3DXANIMATIONCONTROLLER_SLOTS] = {
    "QueryInterface","AddRef","Release","GetMaxNumAnimationOutputs",
    "GetMaxNumAnimationSets","GetMaxNumTracks","GetMaxNumEvents",
    "RegisterAnimationOutput","RegisterAnimationSet","UnregisterAnimationSet",
    "GetNumAnimationSets","GetAnimationSet","GetAnimationSetByName","AdvanceTime",
    "ResetTime","GetTime","SetTrackAnimationSet","GetTrackAnimationSet",
    "SetTrackPriority","SetTrackSpeed","SetTrackWeight","SetTrackPosition",
    "SetTrackEnable","SetTrackDesc","GetTrackDesc","SetPriorityBlend","GetPriorityBlend",
    "KeyTrackSpeed","KeyTrackWeight","KeyTrackPosition","KeyTrackEnable",
    "KeyPriorityBlend","UnkeyEvent","UnkeyAllTrackEvents","UnkeyAllPriorityBlends",
    "GetCurrentTrackEvent","GetCurrentPriorityBlend","GetUpcomingTrackEvent",
    "GetUpcomingPriorityBlend","ValidateEvent","GetEventDesc","CloneAnimationController"
};
static const int ac_argc[ID3DXANIMATIONCONTROLLER_SLOTS] = {
    3,1,1,1,1,1,1,6,2,2,1,3,3,4,1,1,3,3,3,3,3,4,3,3,3,2,1,8,8,6,5,7,
    2,2,1,3,1,3,2,2,3,6
};

static void abi_check(uint32_t obj, int slots, const char *const *names,
                      const int *argc, const char *prefix) {
    uint32_t ea; double st;
    char full[160], desc[200];
    uint32_t vt = MEM32(obj);
    for (int i = 0; i < slots; i++) {
        snprintf(full, sizeof full, "%s::%s", prefix, names[i]);
        snprintf(desc, sizeof desc, "slot %d %s -> %s", i, names[i], full);
        ok(desc, MEM32(vt + 4u * (uint32_t)i) == hle_resolve(full));
        uint32_t args[16] = { 0 };
        args[0] = obj;                              /* every slot receives `this` */
        uint32_t after = raw_vcall(obj, i, argc[i], args, &ea, &st);
        snprintf(desc, sizeof desc, "slot %d %s esp restored (argc %d)", i, names[i], argc[i]);
        ok(desc, after == g_stack + 0x800u);
    }
    snprintf(desc, sizeof desc, "%s vtable ends after %d slots", prefix, slots);
    ok(desc, MEM32(vt + 4u * (uint32_t)slots) == 0u);
}

static uint32_t make_set(const char *name, double tps, int playback,
                         uint32_t cbva, unsigned ncb) {
    uint32_t out = va0(4);
    uint32_t a[8] = { str_va(name), 0u, 0u, (uint32_t)playback, 0u, ncb, cbva, out };
    uint64_t bits; memcpy(&bits, &tps, 8);
    a[1] = (uint32_t)bits; a[2] = (uint32_t)(bits >> 32);
    ok(name, call("D3DXCreateKeyframedAnimationSet", 8, a) == D3D_OK && MEM32(out) != 0u);
    return MEM32(out);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    recomp_host_init();
    void *stack = hle_alloc(0x1000);
    g_stack = hle_va(stack);
    win32hle_register(win32hle_d3dx9);

    /* ---------------------------------------------------------------- set A */
    uint32_t cb = va0(16);
    MEMF(cb) = 2.0f; MEM32(cb + 4u) = 0x1111u;
    MEMF(cb + 8u) = 7.0f; MEM32(cb + 12u) = 0x2222u;
    uint32_t A = make_set("walk", 1.0, 0, cb, 2);
    ok("GetName", strcmp((const char *)hle_ptr(vfun(A,3,1,(uint32_t[]){A})), "walk") == 0);
    ok("GetSourceTicksPerSecond = 1", cd(vdouble(A,12,1,(uint32_t[]){A}), 1.0));
    ok("GetPlaybackType = LOOP", vfun(A,11,1,(uint32_t[]){A}) == 0u);
    ok("GetNumCallbackKeys = 2", vfun(A,25,1,(uint32_t[]){A}) == 2u);
    uint32_t cbo = va0(16);
    ok("GetCallbackKeys",
       vfun(A,26,2,(uint32_t[]){ A, cbo }) == D3D_OK &&
       cf(MEMF(cbo),2.0f) && MEM32(cbo+4u)==0x1111u && cf(MEMF(cbo+8u),7.0f));
    uint32_t cbo1 = va0(8);
    ok("GetCallbackKey(1)",
       vfun(A,27,3,(uint32_t[]){ A, 1u, cbo1 }) == D3D_OK &&
       cf(MEMF(cbo1),7.0f) && MEM32(cbo1+4u)==0x2222u);
    ok("GetCallbackKey(9) -> invalid",
       vfun(A,27,3,(uint32_t[]){ A, 9u, cbo1 }) == D3DERR_INVALIDCALL);

    /* SRT keys for animation "move": scale (1,2,3)->(3,4,5), 0..10;
     * rotation identity -> +90deg about Z; translation (10,20,30)->(50,60,70). */
    uint32_t sk = va0(2*ANIM_VKEY_SIZE), tk = va0(2*ANIM_VKEY_SIZE), rk = va0(2*ANIM_QKEY_SIZE);
    put_vkey(sk,  0.0f, 1,2,3); put_vkey(sk + ANIM_VKEY_SIZE, 10.0f, 3,4,5);
    put_vkey(tk,  0.0f, 10,20,30); put_vkey(tk + ANIM_VKEY_SIZE, 10.0f, 50,60,70);
    const float s45 = 0.707106781f, c45 = 0.707106781f;
    put_qkey(rk, 0.0f, 0,0,0,1); put_qkey(rk + ANIM_QKEY_SIZE, 10.0f, 0,0,s45,c45);
    uint32_t idx = va0(4);
    ok("RegisterAnimationSRTKeys",
       vfun(A,32,9,(uint32_t[]){ A, str_va("move"), 2u,2u,2u, sk,rk,tk, idx }) == D3D_OK &&
       MEM32(idx)==0u);
    ok("GetNumAnimations = 1", vfun(A,6,1,(uint32_t[]){ A }) == 1u);
    ok("GetNumScaleKeys = 2", vfun(A,13,2,(uint32_t[]){ A,0u }) == 2u);
    ok("GetNumRotationKeys = 2", vfun(A,17,2,(uint32_t[]){ A,0u }) == 2u);
    ok("GetNumTranslationKeys = 2", vfun(A,21,2,(uint32_t[]){ A,0u }) == 2u);
    uint32_t name_out = va0(4);
    ok("GetAnimationNameByIndex(0)",
       vfun(A,7,3,(uint32_t[]){ A,0u,name_out }) == D3D_OK &&
       strcmp((const char *)hle_ptr(MEM32(name_out)),"move")==0);
    uint32_t idx_out = va0(4);
    ok("GetAnimationIndexByName(move)",
       vfun(A,8,3,(uint32_t[]){ A,str_va("move"),idx_out }) == D3D_OK && MEM32(idx_out)==0u);
    ok("GetAnimationIndexByName(unknown) -> invalid",
       vfun(A,8,3,(uint32_t[]){ A,str_va("nope"),idx_out }) == D3DERR_INVALIDCALL);
    ok("GetPeriod = 10", cd(vdouble(A,4,1,(uint32_t[]){ A }), WALK_PERIOD));
    uint64_t p25; double d25 = 25.0; memcpy(&p25,&d25,8);
    ok("GetPeriodicPosition(25) = 5",
       cd(vdouble(A,5,3,(uint32_t[]){ A,(uint32_t)p25,(uint32_t)(p25>>32) }), 5.0));

    uint32_t vs = va0(12), vq = va0(16), vt = va0(12);
    uint64_t p5; double d5 = 5.0; memcpy(&p5,&d5,8);
    uint32_t srt_args[7] = { A,(uint32_t)p5,(uint32_t)(p5>>32),0u,vs,vq,vt };
    ok("GetSRT(5) -> D3D_OK", vfun(A,9,7,srt_args) == D3D_OK);
    ok("GetSRT scale = (2,3,4)", v3(vs,2,3,4));
    /* independent half-angle values: sin(22.5), cos(22.5) */
    ok("GetSRT rotation = 45deg about Z", q4(vq,0,0,0.382683432f,0.923879533f));
    ok("GetSRT translation = (30,40,50)", v3(vt,30,40,50));

    /* key get/set: change scale key 0 from (1,2,3) to (7,8,9) and observe SRT. */
    uint32_t one = va0(ANIM_VKEY_SIZE);
    ok("GetScaleKey(0,0)",
       vfun(A,15,4,(uint32_t[]){ A,0u,0u,one }) == D3D_OK &&
       cf(MEMF(one),0.0f) && v3(one+4,1,2,3));
    put_vkey(one, 0.0f, 7,8,9);
    ok("SetScaleKey(0,0)", vfun(A,16,4,(uint32_t[]){ A,0u,0u,one }) == D3D_OK);
    vfun(A,9,7,srt_args);
    ok("GetSRT reflects SetScaleKey: scale = (5,6,7)", v3(vs,5,6,7));
    put_vkey(one, 0.0f, 1,2,3);
    vfun(A,16,4,(uint32_t[]){ A,0u,0u,one });       /* restore */

    /* ---------------------------------------------------------------- set B */
    uint32_t sk2 = va0(ANIM_VKEY_SIZE), rk2 = va0(ANIM_QKEY_SIZE), tk2 = va0(ANIM_VKEY_SIZE);
    put_vkey(sk2, 0.0f, 10,10,10); put_qkey(rk2,0.0f,0,0,0,1); put_vkey(tk2,0.0f,100,100,100);
    uint32_t B = make_set("idle", 1.0, 0, 0u, 0);
    ok("RegisterAnimationSRTKeys(B)",
       vfun(B,32,9,(uint32_t[]){ B,str_va("move"),1u,1u,1u, sk2,rk2,tk2,idx }) == D3D_OK);

    /* ------------------------------------------------------------ controller */
    uint32_t out = va0(4);
    ok("D3DXCreateAnimationController",
       call("D3DXCreateAnimationController",5,(uint32_t[]){2u,2u,2u,0u,out})==D3D_OK && MEM32(out));
    uint32_t C = MEM32(out);
    ok("GetMaxNumAnimationOutputs", vfun(C,3,1,(uint32_t[]){C})==2u);
    ok("GetMaxNumAnimationSets", vfun(C,4,1,(uint32_t[]){C})==2u);
    ok("GetMaxNumTracks", vfun(C,5,1,(uint32_t[]){C})==2u);
    ok("GetMaxNumEvents", vfun(C,6,1,(uint32_t[]){C})==0u);
    uint32_t os = va0(12), oq = va0(16), ot = va0(12);
    ok("RegisterAnimationOutput",
       vfun(C,7,6,(uint32_t[]){C,str_va("move"),0u,os,oq,ot})==D3D_OK);
    ok("RegisterAnimationSet(A)", vfun(C,8,2,(uint32_t[]){C,A})==D3D_OK);
    ok("RegisterAnimationSet(B)", vfun(C,8,2,(uint32_t[]){C,B})==D3D_OK);
    ok("GetNumAnimationSets = 2", vfun(C,10,1,(uint32_t[]){C})==2u);
    uint32_t sout = va0(4);
    ok("GetAnimationSet(0) == A",
       vfun(C,11,3,(uint32_t[]){C,0u,sout})==D3D_OK && MEM32(sout)==A);
    ok("GetAnimationSetByName(idle) == B",
       vfun(C,12,3,(uint32_t[]){C,str_va("idle"),sout})==D3D_OK && MEM32(sout)==B);
    ok("SetTrackAnimationSet(0,A)", vfun(C,16,3,(uint32_t[]){C,0u,A})==D3D_OK);
    ok("SetTrackAnimationSet(1,B)", vfun(C,16,3,(uint32_t[]){C,1u,B})==D3D_OK);
    ok("SetTrackPriority(0,HIGH)", vfun(C,18,3,(uint32_t[]){C,0u,1u})==D3D_OK);
    ok("SetTrackWeight(0,1)",
       vfun(C,20,3,(uint32_t[]){C,0u,0x3F800000u})==D3D_OK);
    ok("SetTrackWeight(1,1)",
       vfun(C,20,3,(uint32_t[]){C,1u,0x3F800000u})==D3D_OK);
    ok("SetTrackSpeed(0,1)",
       vfun(C,19,3,(uint32_t[]){C,0u,0x3F800000u})==D3D_OK);
    ok("SetTrackSpeed(1,1)",
       vfun(C,19,3,(uint32_t[]){C,1u,0x3F800000u})==D3D_OK);
    ok("SetTrackEnable(0,1)", vfun(C,22,3,(uint32_t[]){C,0u,1u})==D3D_OK);
    ok("SetTrackEnable(1,1)", vfun(C,22,3,(uint32_t[]){C,1u,1u})==D3D_OK);
    ok("SetPriorityBlend(0.25)",
       vfun(C,25,2,(uint32_t[]){C,0x3E800000u})==D3D_OK);
    ok("GetPriorityBlend = 0.25",
       cf((float)vdouble(C,26,1,(uint32_t[]){C}),0.25f));

    uint64_t adv; double dd = 5.0; memcpy(&adv,&dd,8);
    ok("AdvanceTime(5)",
       vfun(C,13,4,(uint32_t[]){C,(uint32_t)adv,(uint32_t)(adv>>32),0u})==D3D_OK);
    ok("GetTime = 5", cd(vdouble(C,15,1,(uint32_t[]){C}),5.0));
    /* HIGH = set A at t=5: scale (2,3,4), trans (30,40,50), rot 45deg Z.
     * LOW  = set B: scale (10,10,10), trans (100,100,100), identity.
     * Blend 0.25 -> scale (8,8.25,8.5), trans (82.5,85,87.5), rot 11.25deg Z. */
    ok("mixed output scale = (8,8.25,8.5)", v3(os,8,8.25f,8.5f));
    ok("mixed output translation = (82.5,85,87.5)", v3(ot,82.5f,85,87.5f));
    /* independent half-angle constants for 11.25deg */
    ok("mixed output rot = 11.25deg about Z",
       q4(oq,0,0,0.098017140f,0.995184727f));

    uint32_t desc = va0(ANIM_TRACKDESC_SIZE);
    ok("GetTrackDesc(0)", vfun(C,24,3,(uint32_t[]){C,0u,desc})==D3D_OK);
    ok("GetTrackDesc priority = HIGH", MEM32(desc)==1u);
    ok("GetTrackDesc position = 5", cd(*(double *)hle_ptr(desc+16u),5.0));
    ok("GetTrackDesc enable = 1", MEM32(desc+24u)==1u);
    MEM32(desc)=0u; MEMF(desc+4u)=0.5f; MEMF(desc+8u)=0.0f;
    *(double *)hle_ptr(desc+16u)=0.0; MEM32(desc+24u)=1u;
    ok("SetTrackDesc(0)", vfun(C,23,3,(uint32_t[]){C,0u,desc})==D3D_OK);
    ok("ResetTime", vfun(C,14,1,(uint32_t[]){C})==D3D_OK);
    ok("GetTime after reset = 0", cd(vdouble(C,15,1,(uint32_t[]){C}),0.0));

    /* ====================================================================
     * Defaults, reference counts, time base (ticks vs seconds), blending.
     * Expected values are worked out by hand from the documented semantics.
     * ==================================================================== */
    {
#define REFC(o) MEM32((o) + 4u)
        uint64_t b;
        uint32_t o2 = va0(4);
        ok("controller D", call("D3DXCreateAnimationController",5,(uint32_t[]){2u,3u,3u,0u,o2})==D3D_OK);
        uint32_t D = MEM32(o2);
        uint32_t desc = va0(ANIM_TRACKDESC_SIZE);
        ok("fresh track: GetTrackDesc", vfun(D,24,3,(uint32_t[]){D,2u,desc})==D3D_OK);
        ok("fresh track: LOW priority, weight 1, speed 1, position 0, disabled",
           MEM32(desc)==0u && cf(MEMF(desc+4u),1.0f) && cf(MEMF(desc+8u),1.0f) &&
           cd(*(double *)hle_ptr(desc+16u),0.0) && MEM32(desc+24u)==0u);

        /* set S: 30 ticks/second, keys at tick 0 and 60 => period 2 s.
         * scale 0 -> 6, translation 0 -> (60,0,0), rotation identity -> 90deg about Z. */
        uint32_t S = make_set("run", 30.0, 0, 0u, 0);
        uint32_t sk3 = va0(2*ANIM_VKEY_SIZE), tk3 = va0(2*ANIM_VKEY_SIZE), rk3 = va0(2*ANIM_QKEY_SIZE);
        put_vkey(sk3, 0.0f, 0,0,0); put_vkey(sk3+ANIM_VKEY_SIZE, 60.0f, 6,6,6);
        put_vkey(tk3, 0.0f, 0,0,0); put_vkey(tk3+ANIM_VKEY_SIZE, 60.0f, 60,0,0);
        put_qkey(rk3, 0.0f, 0,0,0,1); put_qkey(rk3+ANIM_QKEY_SIZE, 60.0f, 0,0,s45,c45);
        uint32_t ix = va0(4);
        ok("S keys", vfun(S,32,9,(uint32_t[]){ S, str_va("bone"), 2u,2u,2u, sk3,rk3,tk3, ix })==D3D_OK);
        ok("GetSourceTicksPerSecond = 30", cd(vdouble(S,12,1,(uint32_t[]){S}), 30.0));
        ok("GetPeriod = 60 ticks / 30 = 2 s", cd(vdouble(S,4,1,(uint32_t[]){S}), 2.0));
        double d5s = 5.0; memcpy(&b,&d5s,8);
        ok("GetPeriodicPosition(5 s) = 1 s", cd(vdouble(S,5,3,(uint32_t[]){S,(uint32_t)b,(uint32_t)(b>>32)}), 1.0));
        uint32_t gs = va0(12), gq = va0(16), gt = va0(12);
        double d1s = 1.0; memcpy(&b,&d1s,8);
        ok("GetSRT(1 s) = tick 30", vfun(S,9,7,(uint32_t[]){S,(uint32_t)b,(uint32_t)(b>>32),0u,gs,gq,gt})==D3D_OK);
        ok("GetSRT(1 s): scale 3, translation (30,0,0), rotation 45deg Z",
           v3(gs,3,3,3) && v3(gt,30,0,0) && q4(gq,0,0,0.382683432f,0.923879533f));
        ok("tps 0 is invalid", ({ uint32_t out0 = va0(4);
           uint32_t a[8] = { str_va("x"), 0u, 0u, 0u, 0u, 0u, 0u, out0 };
           call("D3DXCreateKeyframedAnimationSet", 8, a) == D3DERR_INVALIDCALL && MEM32(out0)==0u; }));

        /* reference counts */
        ok("a new set has 1 reference", REFC(S)==1u);
        vfun(D,8,2,(uint32_t[]){D,S});
        ok("RegisterAnimationSet takes a reference", REFC(S)==2u);
        vfun(D,16,3,(uint32_t[]){D,0u,S});
        ok("SetTrackAnimationSet takes a reference", REFC(S)==3u);
        uint32_t ps = va0(4);
        ok("GetTrackAnimationSet returns S", vfun(D,17,3,(uint32_t[]){D,0u,ps})==D3D_OK && MEM32(ps)==S);
        ok("GetTrackAnimationSet adds a reference", REFC(S)==4u);
        vfun(S,2,1,(uint32_t[]){S});
        ok("GetAnimationSet returns S", vfun(D,11,3,(uint32_t[]){D,0u,ps})==D3D_OK && MEM32(ps)==S && REFC(S)==4u);
        vfun(S,2,1,(uint32_t[]){S});
        ok("GetAnimationSetByName returns S", vfun(D,12,3,(uint32_t[]){D,str_va("run"),ps})==D3D_OK && MEM32(ps)==S && REFC(S)==4u);
        vfun(S,2,1,(uint32_t[]){S});
        ok("references are back to registration + track", REFC(S)==3u);
        vfun(D,16,3,(uint32_t[]){D,0u,S});
        ok("setting the same set again changes nothing", REFC(S)==3u);
        vfun(D,16,3,(uint32_t[]){D,0u,0u});
        ok("clearing the track releases its reference", REFC(S)==2u);
        ok("GetTrackAnimationSet of an empty track: NULL", vfun(D,17,3,(uint32_t[]){D,0u,ps})==D3D_OK && MEM32(ps)==0u);
        vfun(D,16,3,(uint32_t[]){D,1u,S});
        ok("UnregisterAnimationSet", vfun(D,9,2,(uint32_t[]){D,S})==D3D_OK);
        ok("Unregister releases the registration and the tracks playing the set", REFC(S)==1u);
        vfun(D,17,3,(uint32_t[]){D,1u,ps});
        ok("the track that played it is empty", MEM32(ps)==0u);
        ok("an unknown set cannot be unregistered", vfun(D,9,2,(uint32_t[]){D,S})==D3DERR_INVALIDCALL);

        /* default track: only the set and Enable are set; time is in seconds */
        uint32_t os3 = va0(12), oq3 = va0(16), ot3 = va0(12), om3 = va0(64);
        ok("RegisterAnimationOutput(bone)", vfun(D,7,6,(uint32_t[]){D,str_va("bone"),om3,os3,oq3,ot3})==D3D_OK);
        uint32_t os4 = va0(12);
        MEMF(os4) = 77.0f; MEMF(os4+4) = 77.0f; MEMF(os4+8) = 77.0f;
        ok("RegisterAnimationOutput(other)", vfun(D,7,6,(uint32_t[]){D,str_va("other"),0u,os4,0u,0u})==D3D_OK);
        vfun(D,8,2,(uint32_t[]){D,S});
        vfun(D,16,3,(uint32_t[]){D,0u,S});
        vfun(D,22,3,(uint32_t[]){D,0u,1u});
        double dh = 0.5; memcpy(&b,&dh,8);
        ok("AdvanceTime(0.5) with default weight/speed", vfun(D,13,4,(uint32_t[]){D,(uint32_t)b,(uint32_t)(b>>32),0u})==D3D_OK);
        /* 0.5 s = tick 15: scale 1.5, translation (15,0,0), rotation 22.5deg */
        ok("default track animates: scale 1.5", v3(os3,1.5f,1.5f,1.5f));
        ok("default track animates: translation and rotation", v3(ot3,15,0,0) && q4(oq3,0,0,0.195090322f,0.980785280f));
        vfun(D,24,3,(uint32_t[]){D,0u,desc});
        ok("track position advanced by dt*speed = 0.5 s", cd(*(double *)hle_ptr(desc+16u),0.5));
        ok("an output without a matching animation is left alone", v3(os4,77,77,77));
        vfun(D,19,3,(uint32_t[]){D,0u,0x40000000u});                   /* speed 2 */
        dh = 0.25; memcpy(&b,&dh,8);
        vfun(D,13,4,(uint32_t[]){D,(uint32_t)b,(uint32_t)(b>>32),0u});
        ok("speed 2: 0.25 s advances the track by 0.5 s to 1 s => scale 3", v3(os3,3,3,3));
        ok("matrix: rotation 45deg Z, scale 3, translation (30,0,0)",
           cf(MEMF(om3),2.1213203f) && cf(MEMF(om3+4),2.1213203f) && cf(MEMF(om3+16),-2.1213203f) &&
           cf(MEMF(om3+20),2.1213203f) && cf(MEMF(om3+40),3.0f) && cf(MEMF(om3+48),30.0f) && cf(MEMF(om3+60),1.0f));
        vfun(D,19,3,(uint32_t[]){D,0u,0x3F800000u});                   /* speed 1 */
        dh = 1.5; memcpy(&b,&dh,8);
        vfun(D,13,4,(uint32_t[]){D,(uint32_t)b,(uint32_t)(b>>32),0u});
        ok("the set loops: position 2.5 s = 0.5 s of the period => scale 1.5", v3(os3,1.5f,1.5f,1.5f));
        vfun(D,22,3,(uint32_t[]){D,0u,0u});
        MEMF(os3) = 55.0f;
        dh = 1.0; memcpy(&b,&dh,8);
        vfun(D,13,4,(uint32_t[]){D,(uint32_t)b,(uint32_t)(b>>32),0u});
        ok("a disabled track neither advances nor writes", MEMF(os3)==55.0f);

        /* blending: P = scale 2, translation (10,0,0), rotation 90deg Z; Q = scale 6, translation (30,0,0), identity */
        uint32_t o3 = va0(4);
        call("D3DXCreateAnimationController",5,(uint32_t[]){1u,2u,2u,0u,o3});
        uint32_t E = MEM32(o3);
        uint32_t P = make_set("p", 1.0, 0, 0u, 0), Q = make_set("q", 1.0, 0, 0u, 0);
        uint32_t k1 = va0(ANIM_VKEY_SIZE), k2 = va0(ANIM_QKEY_SIZE), k3 = va0(ANIM_VKEY_SIZE);
        put_vkey(k1,0,2,2,2); put_qkey(k2,0,0,0,s45,c45); put_vkey(k3,0,10,0,0);
        vfun(P,32,9,(uint32_t[]){P,str_va("bone"),1u,1u,1u,k1,k2,k3,ix});
        put_vkey(k1,0,6,6,6); put_qkey(k2,0,0,0,0,1); put_vkey(k3,0,30,0,0);
        vfun(Q,32,9,(uint32_t[]){Q,str_va("bone"),1u,1u,1u,k1,k2,k3,ix});
        uint32_t es = va0(12), eq = va0(16), et = va0(12);
        vfun(E,7,6,(uint32_t[]){E,str_va("bone"),0u,es,eq,et});
        vfun(E,8,2,(uint32_t[]){E,P}); vfun(E,8,2,(uint32_t[]){E,Q});
        vfun(E,16,3,(uint32_t[]){E,0u,P}); vfun(E,16,3,(uint32_t[]){E,1u,Q});
        vfun(E,22,3,(uint32_t[]){E,0u,1u}); vfun(E,22,3,(uint32_t[]){E,1u,1u});
        vfun(E,20,3,(uint32_t[]){E,0u,0x3F000000u}); vfun(E,20,3,(uint32_t[]){E,1u,0x3F000000u});   /* 0.5 / 0.5 */
        dh = 0.0; memcpy(&b,&dh,8);
        vfun(E,13,4,(uint32_t[]){E,(uint32_t)b,(uint32_t)(b>>32),0u});
        ok("same priority, weights 0.5/0.5: scale 4, translation (20,0,0), rotation 45deg Z",
           v3(es,4,4,4) && v3(et,20,0,0) && q4(eq,0,0,0.382683432f,0.923879533f));
        vfun(E,20,3,(uint32_t[]){E,0u,0x3F800000u}); vfun(E,20,3,(uint32_t[]){E,1u,0x3F800000u});   /* 1 / 1 */
        vfun(E,13,4,(uint32_t[]){E,(uint32_t)b,(uint32_t)(b>>32),0u});
        ok("weights are not normalized: 1/1 gives scale 8, translation (40,0,0)", v3(es,8,8,8) && v3(et,40,0,0));
        vfun(E,18,3,(uint32_t[]){E,0u,1u});                       /* track 0 (P) HIGH */
        vfun(E,25,2,(uint32_t[]){E,0x3F000000u});                 /* blend 0.5 */
        vfun(E,13,4,(uint32_t[]){E,(uint32_t)b,(uint32_t)(b>>32),0u});
        ok("LOW Q + HIGH P, blend 0.5: scale 4, translation (20,0,0), rotation 45deg Z",
           v3(es,4,4,4) && v3(et,20,0,0) && q4(eq,0,0,0.382683432f,0.923879533f));
        vfun(E,25,2,(uint32_t[]){E,0x3F800000u});
        vfun(E,13,4,(uint32_t[]){E,(uint32_t)b,(uint32_t)(b>>32),0u});
        ok("blend 1: HIGH only (P)", v3(es,2,2,2) && v3(et,10,0,0));
        vfun(E,25,2,(uint32_t[]){E,0u});
        vfun(E,13,4,(uint32_t[]){E,(uint32_t)b,(uint32_t)(b>>32),0u});
        ok("blend 0: LOW only (Q)", v3(es,6,6,6) && v3(et,30,0,0) && q4(eq,0,0,0,1));

        /* callbacks are not delivered: honest error, state still advances */
        uint32_t cbk = va0(8); MEMF(cbk) = 1.0f; MEM32(cbk+4u) = 0x5u;
        uint32_t R = make_set("r", 1.0, 0, cbk, 1);
        vfun(R,32,9,(uint32_t[]){R,str_va("bone"),1u,1u,1u,k1,k2,k3,ix});
        ok("the controller holds 2 sets at most", vfun(E,8,2,(uint32_t[]){E,R})==D3DERR_INVALIDCALL);
        vfun(E,9,2,(uint32_t[]){E,Q});
        ok("RegisterAnimationSet(R) after making room", vfun(E,8,2,(uint32_t[]){E,R})==D3D_OK);
        vfun(E,16,3,(uint32_t[]){E,0u,R});
        ok("AdvanceTime with a handler and callback keys: not supported",
           vfun(E,13,4,(uint32_t[]){E,(uint32_t)b,(uint32_t)(b>>32),0x1234u})==D3DERR_NOTAVAILABLE);
        ok("AdvanceTime without a handler is fine",
           vfun(E,13,4,(uint32_t[]){E,(uint32_t)b,(uint32_t)(b>>32),0u})==D3D_OK);

        /* releasing the controller releases what it held */
        vfun(E,2,1,(uint32_t[]){E});
        ok("releasing the controller drops its references (P, Q, R back to their creation reference)",
           REFC(P)==1u && REFC(Q)==1u && REFC(R)==1u);
        vfun(D,2,1,(uint32_t[]){D});
        ok("the first controller released S", REFC(S)==1u);
    }

    /* ---------------------------------------- unsupported slots: honest errors */
    ok("GetCallback -> E_NOTIMPL",
       vfun(A,10,6,(uint32_t[]){A,0u,0u,0u,0u,0u,0u})==E_NOTIMPL_V);
    ok("Compress -> E_NOTIMPL",
       vfun(A,33,5,(uint32_t[]){A,0u,0u,0u,0u,0u})==E_NOTIMPL_V);
    ok("CloneAnimationController -> E_NOTIMPL",
       vfun(C,41,6,(uint32_t[]){C,0u,0u,0u,0u,0u})==E_NOTIMPL_V);
    ok("UnkeyEvent -> E_NOTIMPL", vfun(C,32,2,(uint32_t[]){C,0u,0u})==E_NOTIMPL_V);
    ok("GetCurrentTrackEvent -> 0", vfun(C,35,3,(uint32_t[]){C,0u,0u,0u})==0u);

    /* ---------------------------------- every-slot ABI check on dedicated objects */
    uint32_t A2 = make_set("abi", 1.0, 0, 0u, 0);
    vfun(A2,1,1,(uint32_t[]){A2});                    /* refcount 2 before Release slot */
    abi_check(A2, ID3DXKEYFRAMEDANIMATIONSET_SLOTS, kf_names, kf_argc, "ID3DXKeyframedAnimationSet");
    uint32_t out2 = va0(4);
    call("D3DXCreateAnimationController",5,(uint32_t[]){2u,2u,2u,0u,out2});
    uint32_t C2 = MEM32(out2);
    vfun(C2,1,1,(uint32_t[]){C2});
    abi_check(C2, ID3DXANIMATIONCONTROLLER_SLOTS, ac_names, ac_argc, "ID3DXAnimationController");

    vfun(C,2,1,(uint32_t[]){C}); vfun(A,2,1,(uint32_t[]){A}); vfun(B,2,1,(uint32_t[]){B});
    printf(fails ? "d3dx9_anim_selftest: %d FAILURES\n" : "d3dx9_anim_selftest: OK\n", fails);
    return fails != 0;
}
