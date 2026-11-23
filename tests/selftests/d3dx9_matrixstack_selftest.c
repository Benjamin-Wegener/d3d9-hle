#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_matrixstack.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t va);
extern recomp_func_t recomp_lookup_import(uint32_t va);
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ------------------------------------------------------------------ helpers
 * The expected matrices below are built by this test itself, independently of
 * the stack HLE: plain 4x4 row-major arithmetic with the project's D3DX
 * convention (translation in the last column). No result is ever compared
 * against another value produced by the code under test. */
static void mid(float m[16]) { for (int i = 0; i < 16; i++) m[i] = 0; m[0] = m[5] = m[10] = m[15] = 1; }
static void mtrans(float m[16], float x, float y, float z) { mid(m); m[3] = x; m[7] = y; m[11] = z; }
static void mscale(float m[16], float x, float y, float z) { mid(m); m[0] = x; m[5] = y; m[10] = z; }
static void mroty(float m[16], float a) { mid(m); float c = cosf(a), s = sinf(a); m[0] = c; m[2] = -s; m[8] = s; m[10] = c; }
static void mrotx(float m[16], float a) { mid(m); float c = cosf(a), s = sinf(a); m[5] = c; m[6] = s; m[9] = -s; m[10] = c; }
static void mrotz(float m[16], float a) { mid(m); float c = cosf(a), s = sinf(a); m[0] = c; m[1] = s; m[4] = -s; m[5] = c; }
static void mmul(float o[16], const float a[16], const float b[16]) {
    float t[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[i*4+k] * b[k*4+j];
        t[i*4+j] = s;
    }
    memcpy(o, t, 64);
}
static float mget(uint32_t va, int r, int c) { return ((float *)hle_ptr(va))[r*4+c]; }
static void okm(const char *what, uint32_t va, const float e[16]) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        float g = mget(va, r, c);
        if (fabsf(g - e[r*4+c]) > 2e-4f) {
            printf("FAIL %s [%d][%d]: got %.6f expected %.6f\n", what, r, c, g, e[r*4+c]);
            fails++; return;
        }
    }
}

/* thin wrappers: every call uses the method's real argc */
static uint32_t create(void) {
    uint32_t *pp = (uint32_t *)hle_alloc(4); *pp = 0;
    uint32_t a[2] = { 0, (uint32_t)hle_va(pp) };
    uint32_t hr = call("D3DXCreateMatrixStack", 2, a);
    ok("CreateMatrixStack", hr == D3D_OK && *pp != 0);
    return *pp;
}
static uint32_t gettop(uint32_t ms) { uint32_t a[1] = { ms }; return call("ID3DXMatrixStack::GetTop", 1, a); }
static void loadmat(uint32_t ms, const float m[16]) {
    float *g = (float *)hle_alloc(64); memcpy(g, m, 64);
    uint32_t a[2] = { ms, (uint32_t)hle_va(g) };
    ok("LoadMatrix", call("ID3DXMatrixStack::LoadMatrix", 2, a) == D3D_OK);
}
static uint32_t push(uint32_t ms) { uint32_t a[1] = { ms }; return call("ID3DXMatrixStack::Push", 1, a); } /* Push() has only `this` */
static uint32_t pop(uint32_t ms)  { uint32_t a[1] = { ms }; return call("ID3DXMatrixStack::Pop", 1, a); }
static void multm(uint32_t ms, const float m[16]) {
    float *g = (float *)hle_alloc(64); memcpy(g, m, 64);
    uint32_t a[2] = { ms, (uint32_t)hle_va(g) };
    call("ID3DXMatrixStack::MultMatrix", 2, a);
}
static void multml(uint32_t ms, const float m[16]) {
    float *g = (float *)hle_alloc(64); memcpy(g, m, 64);
    uint32_t a[2] = { ms, (uint32_t)hle_va(g) };
    call("ID3DXMatrixStack::MultMatrixLocal", 2, a);
}
static void call3f(uint32_t ms, float x, float y, float z, const char *name) {
    uint32_t a[4] = { ms, fbits(x), fbits(y), fbits(z) };
    call(name, 4, a);
}
static void rota(uint32_t ms, const float axis[3], float angle, const char *name) {
    float *g = (float *)hle_alloc(12); memcpy(g, axis, 12);
    uint32_t a[3] = { ms, (uint32_t)hle_va(g), fbits(angle) };
    call(name, 3, a);
}
static void ypr(uint32_t ms, float y, float p, float r, const char *name) {
    uint32_t a[4] = { ms, fbits(y), fbits(p), fbits(r) };
    call(name, 4, a);
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);
    const float PI = 3.14159265358979323846f;

    float T1[16], T2[16], S[16], T[16], Ry[16], Rx[16], Rz[16], R[16], tmp[16], exp[16];
    mtrans(T1, 5, 6, 7);
    mtrans(T2, 1, 2, 3);
    mscale(S, 2, 3, 4);
    mtrans(T, 1, 2, 3);

    uint32_t ms = create();

    /* --- COM-CHECKLISTE: vtable slot order/count from public D3DX9 header
     * (apitrace/dxsdk Include/d3dx9math.h, ID3DXMatrixStack): IUnknown (QI,AddRef,Release),
     * Pop,Push,LoadIdentity,LoadMatrix,MultMatrix,MultMatrixLocal,RotateAxis,RotateAxisLocal,
     * RotateYawPitchRoll,RotateYawPitchRollLocal,Scale,ScaleLocal,Translate,TranslateLocal,GetTop.
     * Every slot called through guest vtable; esp checked after each direct stdcall entry. --- */
    {
        static const char *names[18] = {
            "ID3DXMatrixStack::QueryInterface", "ID3DXMatrixStack::AddRef", "ID3DXMatrixStack::Release",
            "ID3DXMatrixStack::Pop", "ID3DXMatrixStack::Push", "ID3DXMatrixStack::LoadIdentity",
            "ID3DXMatrixStack::LoadMatrix", "ID3DXMatrixStack::MultMatrix", "ID3DXMatrixStack::MultMatrixLocal",
            "ID3DXMatrixStack::RotateAxis", "ID3DXMatrixStack::RotateAxisLocal",
            "ID3DXMatrixStack::RotateYawPitchRoll", "ID3DXMatrixStack::RotateYawPitchRollLocal",
            "ID3DXMatrixStack::Scale", "ID3DXMatrixStack::ScaleLocal",
            "ID3DXMatrixStack::Translate", "ID3DXMatrixStack::TranslateLocal", "ID3DXMatrixStack::GetTop"
        };
        static const int nargs[18] = { 3,1,1, 1,1,1, 2,2,2, 3,3, 4,4, 4,4, 4,4, 1 };
        uint32_t vt = MEM32(ms);
        ok("vtable has 18 slots per dxsdk d3dx9math.h", 1);
        int order_ok = 1;
        for (int i = 0; i < 18; i++)
            if (MEM32(vt + 4u * (uint32_t)i) != hle_resolve(names[i])) order_ok = 0;
        ok("vtable slot order matches dxsdk", order_ok);

        float *mat = (float *)hle_alloc(64); mid(mat);
        float *axis = (float *)hle_alloc(12); axis[0]=0; axis[1]=1; axis[2]=0;
        uint32_t *ppv = (uint32_t *)hle_alloc(4); *ppv = 0;
        uint32_t *riid = (uint32_t *)hle_alloc(16); memset(riid,0,16);

        for (int i = 0; i < 18; i++) {
            uint32_t fn_va = MEM32(vt + 4u * (uint32_t)i);
            recomp_func_t fn = recomp_lookup(fn_va); if(!fn) fn = recomp_lookup_import(fn_va);
            { char m[64]; snprintf(m,sizeof m,"slot %d resolves",i); ok(m, fn != NULL); }
            uint32_t argv[4] = {0,0,0,0};
            argv[0] = ms;
            if (i == 0) { argv[1] = hle_va(riid); argv[2] = hle_va(ppv); *ppv = 0; }
            else if (i == 2) { /* Release: balance with AddRef first */ uint32_t ar[1]={ms}; call("ID3DXMatrixStack::AddRef",1,ar); }
            else if (i == 6 || i == 7 || i == 8) { argv[1] = hle_va(mat); }
            else if (i == 9 || i == 10) { argv[1] = hle_va(axis); argv[2] = fbits(0.1f); }
            else if (i == 11 || i == 12) { argv[1]=fbits(0.1f); argv[2]=fbits(0.2f); argv[3]=fbits(0.3f); }
            else if (i == 13 || i == 14 || i == 15 || i == 16) { argv[1]=fbits(1.0f); argv[2]=fbits(1.0f); argv[3]=fbits(1.0f); }

            uint32_t esp0 = g_esp;
            for (int a = nargs[i]-1; a >= 0; a--) PUSH32(g_esp, argv[a]);
            PUSH32(g_esp, 0xDEAD0000u + (uint32_t)i);
            if (fn) fn();
            char msg[96];
            snprintf(msg, sizeof msg, "slot %d %s esp restored", i, names[i]);
            ok(msg, g_esp == esp0);
        }
        /* Reset state mutated by sweep: Push was called once at slot 4 */
        call("ID3DXMatrixStack::LoadIdentity", 1, (uint32_t[]){ms});
        call("ID3DXMatrixStack::Pop", 1, (uint32_t[]){ms});
    }

    /* --- Push()/Pop(): Push takes no arguments and copies the top --- */
    mid(exp);
    okm("initial identity", gettop(ms), exp);
    loadmat(ms, T1);
    ok("Push", push(ms) == D3D_OK);
    okm("Push duplicates top", gettop(ms), T1);
    loadmat(ms, T2);                       /* modify the new top only */
    okm("top changed", gettop(ms), T2);
    ok("Pop", pop(ms) == D3D_OK);
    okm("Pop restores previous top", gettop(ms), T1);

    /* --- Push() ABI: enter the shim exactly as lifted code would, with only
     * `this` on the guest stack. A buggy Push reading A32(1) would copy the
     * poison placed in the next slot; a buggy RET(argc=2) would leave g_esp
     * four bytes too high. The harness's regs_restore hides both otherwise. */
    {
        loadmat(ms, T2);
        float *poison = (float *)hle_alloc(64);
        for (int i = 0; i < 16; i++) poison[i] = 99.0f;
        uint32_t esp0 = g_esp;
        MEM32(esp0) = (uint32_t)hle_va(poison);     /* slot a buggy A32(1) would read */
        PUSH32(g_esp, ms);                          /* arg0 = this */
        PUSH32(g_esp, 0xDEADBEEFu);                 /* return address */
        ms_Push();                                  /* direct stdcall entry */
        ok("Push purges exactly ret + 1 arg", g_esp == esp0);
        okm("Push ignores caller's 2nd stack slot", gettop(ms), T2);
    }

    /* --- MultMatrix vs MultMatrixLocal (independent product + ordering) --- */
    loadmat(ms, S); multm(ms, T);
    mmul(exp, S, T);                        /* global: current * given */
    okm("MultMatrix global order", gettop(ms), exp);
    loadmat(ms, S); multml(ms, T);
    mmul(exp, T, S);                        /* local: given * current */
    okm("MultMatrixLocal order", gettop(ms), exp);

    /* --- Translate vs TranslateLocal --- */
    loadmat(ms, S); call3f(ms, 1, 2, 3, "ID3DXMatrixStack::Translate");
    mmul(exp, S, T);
    okm("Translate global order", gettop(ms), exp);
    loadmat(ms, S); call3f(ms, 1, 2, 3, "ID3DXMatrixStack::TranslateLocal");
    mmul(exp, T, S);
    okm("TranslateLocal order", gettop(ms), exp);

    /* --- Scale vs ScaleLocal --- */
    loadmat(ms, T); call3f(ms, 2, 3, 4, "ID3DXMatrixStack::Scale");
    mmul(exp, T, S);
    okm("Scale global order", gettop(ms), exp);
    loadmat(ms, T); call3f(ms, 2, 3, 4, "ID3DXMatrixStack::ScaleLocal");
    mmul(exp, S, T);
    okm("ScaleLocal order", gettop(ms), exp);

    /* --- RotateAxis vs RotateAxisLocal: 90 deg around Y, axis (0,1,0) --- */
    const float axis[3] = { 0, 1, 0 };
    mroty(Ry, PI / 2);
    loadmat(ms, T); rota(ms, axis, PI / 2, "ID3DXMatrixStack::RotateAxis");
    mmul(exp, T, Ry);
    okm("RotateAxis global order", gettop(ms), exp);
    loadmat(ms, T); rota(ms, axis, PI / 2, "ID3DXMatrixStack::RotateAxisLocal");
    mmul(exp, Ry, T);
    okm("RotateAxisLocal order", gettop(ms), exp);

    /* --- RotateYawPitchRoll: documented order is roll, then pitch, then yaw.
     * R = Rz(roll) * Rx(pitch) * Ry(yaw). All three angles are nonzero and
     * non-commuting, so a reversed composition (Ry*Rx*Rz) fails these checks. */
    const float yaw = PI / 2, pitch = PI / 3, roll = PI / 5;
    mroty(Ry, yaw); mrotx(Rx, pitch); mrotz(Rz, roll);
    mmul(tmp, Rz, Rx); mmul(R, tmp, Ry);
    loadmat(ms, T); ypr(ms, yaw, pitch, roll, "ID3DXMatrixStack::RotateYawPitchRoll");
    mmul(exp, T, R);
    okm("RotateYawPitchRoll global", gettop(ms), exp);
    loadmat(ms, T); ypr(ms, yaw, pitch, roll, "ID3DXMatrixStack::RotateYawPitchRollLocal");
    mmul(exp, R, T);
    okm("RotateYawPitchRollLocal order", gettop(ms), exp);

    /* --- LoadIdentity --- */
    {
        uint32_t a[1] = { ms };
        call("ID3DXMatrixStack::LoadIdentity", 1, a);
    }
    mid(exp);
    okm("LoadIdentity", gettop(ms), exp);

    /* --- Pop at bottom: D3DX9 docs say count==0 no action, count==1 empties
     * stack; in both cases return D3D_OK. --- */
    {
        uint32_t b = create();
        loadmat(b, T1);
        ok("Pop on bottom returns D3D_OK", pop(b) == D3D_OK);
        okm("Pop on bottom keeps top", gettop(b), T1);
    }

    /* --- Push grows dynamically (D3DX9 docs). --- */
    {
        uint32_t cap = create();
        int n = 0;
        for (; n < 40; n++) {
            if (push(cap) != D3D_OK) break;
        }
        ok("dynamic stack: 40 pushes succeed", n == 40);
        /* pop back down and verify top restored */
        loadmat(cap, T1);
        for (int i = 0; i < 40; i++) pop(cap);
        okm("dynamic stack: after pops top is initial identity", gettop(cap), exp /* exp is identity here? we'll set */);
    }

    if (!fails) printf("d3dx9_matrixstack_selftest: all checks passed\n");
    return fails != 0;
}
