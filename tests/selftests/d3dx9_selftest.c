/*
 * d3dx9_selftest.c - D3DX9's math, through the shims as a guest calls them:
 *
 *   - identity, multiply, inverse, transpose, scaling, translation and the
 *     X/Y/Z rotations against known matrices;
 *   - M * M^-1 is the identity, a singular matrix's inverse is NULL;
 *   - pOut may alias an input;
 *   - every shim leaves the guest stack balanced (stdcall: ret + args popped);
 *   - 2D, 3D, and 4D vector normalize, transforms, coord/normal projections, cross.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3dx9_hle.h"

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define A(p) hle_va(p)

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static int nearf(float a, float b) { return fabsf(a - b) < 1e-4f; }

/* the 16 floats of a guest D3DXMATRIX, row by row */
static int mat_eq(uint32_t va, const float want[16]) {
    const float *g = (const float *)hle_ptr(va);
    for (int i = 0; i < 16; i++)
        if (!nearf(g[i], want[i])) return 0;
    return 1;
}
static void mat_set(uint32_t va, const float v[16]) { memcpy(hle_ptr(va), v, 64); }

static const float ID[16] = {
    1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1
};

/* a stdcall shim must pop ret + its args: call it directly and check the
 * stack pointer is exactly where it was before. */
static int balanced(const char *name, int n, const uint32_t *a) {
    uint32_t va = hle_resolve(name);
    mach_enter();
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) PUSH32(g_esp, a[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_lookup_import(va)();
    int b = (g_esp == esp0);
    g_esp = esp0;
    mach_leave();
    return b;
}

/* ---------------------------------------------------------------- vectors */

static int vec2_eq(uint32_t va, float x, float y) {
    const float *v = (const float *)hle_ptr(va);
    return nearf(v[0], x) && nearf(v[1], y);
}

static int vec3_eq(uint32_t va, float x, float y, float z) {
    const float *v = (const float *)hle_ptr(va);
    return nearf(v[0], x) && nearf(v[1], y) && nearf(v[2], z);
}

static int vec4_eq(uint32_t va, float x, float y, float z, float w) {
    const float *v = (const float *)hle_ptr(va);
    return nearf(v[0], x) && nearf(v[1], y) && nearf(v[2], z) && nearf(v[3], w);
}

static void test_vectors(void) {
    void *out = hle_alloc(64), *v2 = hle_alloc(16), *v3 = hle_alloc(16), *v4 = hle_alloc(16);
    void *mat = hle_alloc(64);

    /* Vec2Normalize */
    float v2_raw[2] = { 3.0f, 4.0f };
    memcpy(hle_ptr(A(v2)), v2_raw, 8);
    uint32_t a_v2norm[2] = { A(out), A(v2) };
    ok("Vec2Normalize return", call("D3DXVec2Normalize", 2, a_v2norm) == A(out));
    ok("Vec2Normalize(3,4)", vec2_eq(A(out), 0.6f, 0.8f));
    ok("stack: Vec2Normalize", balanced("D3DXVec2Normalize", 2, a_v2norm));

    /* Vec3Normalize */
    float v3_raw[3] = { 0.0f, 0.0f, 5.0f };
    memcpy(hle_ptr(A(v3)), v3_raw, 12);
    uint32_t a_v3norm[2] = { A(out), A(v3) };
    ok("Vec3Normalize return", call("D3DXVec3Normalize", 2, a_v3norm) == A(out));
    ok("Vec3Normalize(0,0,5)", vec3_eq(A(out), 0.0f, 0.0f, 1.0f));
    ok("stack: Vec3Normalize", balanced("D3DXVec3Normalize", 2, a_v3norm));

    /* Vec4Normalize */
    float v4_raw[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    memcpy(hle_ptr(A(v4)), v4_raw, 16);
    uint32_t a_v4norm[2] = { A(out), A(v4) };
    ok("Vec4Normalize return", call("D3DXVec4Normalize", 2, a_v4norm) == A(out));
    ok("Vec4Normalize(1,1,1,1)", vec4_eq(A(out), 0.5f, 0.5f, 0.5f, 0.5f));
    ok("stack: Vec4Normalize", balanced("D3DXVec4Normalize", 2, a_v4norm));

    /* Translation matrix T(10, 20, 30) in D3DX convention */
    float T[16] = { 1, 0, 0, 10,
                    0, 1, 0, 20,
                    0, 0, 1, 30,
                    0, 0, 0, 1 };
    mat_set(A(mat), T);

    /* Vec3TransformCoord vs Vec3TransformNormal vs Vec3Transform */
    float p[3] = { 1.0f, 2.0f, 3.0f };
    memcpy(hle_ptr(A(v3)), p, 12);
    uint32_t a_v3trans[3] = { A(out), A(v3), A(mat) };
    ok("Vec3TransformCoord", call("D3DXVec3TransformCoord", 3, a_v3trans) == A(out)
       && vec3_eq(A(out), 11.0f, 22.0f, 33.0f));
    ok("Vec3TransformNormal", call("D3DXVec3TransformNormal", 3, a_v3trans) == A(out)
       && vec3_eq(A(out), 1.0f, 2.0f, 3.0f));
    ok("Vec3Transform", call("D3DXVec3Transform", 3, a_v3trans) == A(out)
       && vec4_eq(A(out), 11.0f, 22.0f, 33.0f, 1.0f));
    ok("stack: Vec3TransformCoord", balanced("D3DXVec3TransformCoord", 3, a_v3trans));
    ok("stack: Vec3TransformNormal", balanced("D3DXVec3TransformNormal", 3, a_v3trans));
    ok("stack: Vec3Transform", balanced("D3DXVec3Transform", 3, a_v3trans));

    /* Vec2TransformCoord vs Vec2TransformNormal */
    float p2[2] = { 5.0f, 6.0f };
    memcpy(hle_ptr(A(v2)), p2, 8);
    uint32_t a_v2trans[3] = { A(out), A(v2), A(mat) };
    ok("Vec2TransformCoord", call("D3DXVec2TransformCoord", 3, a_v2trans) == A(out)
       && vec2_eq(A(out), 15.0f, 26.0f));
    ok("Vec2TransformNormal", call("D3DXVec2TransformNormal", 3, a_v2trans) == A(out)
       && vec2_eq(A(out), 5.0f, 6.0f));
    ok("Vec2Transform", call("D3DXVec2Transform", 3, a_v2trans) == A(out)
       && vec4_eq(A(out), 15.0f, 26.0f, 30.0f, 1.0f));
    ok("stack: Vec2TransformCoord", balanced("D3DXVec2TransformCoord", 3, a_v2trans));
    ok("stack: Vec2TransformNormal", balanced("D3DXVec2TransformNormal", 3, a_v2trans));
    ok("stack: Vec2Transform", balanced("D3DXVec2Transform", 3, a_v2trans));

    /* Vec4Cross: orthogonal to 3 standard basis vectors */
    void *v4_a = hle_alloc(16), *v4_b = hle_alloc(16), *v4_c = hle_alloc(16);
    float e1[4] = { 1, 0, 0, 0 }, e2[4] = { 0, 1, 0, 0 }, e3[4] = { 0, 0, 1, 0 };
    memcpy(hle_ptr(A(v4_a)), e1, 16);
    memcpy(hle_ptr(A(v4_b)), e2, 16);
    memcpy(hle_ptr(A(v4_c)), e3, 16);
    uint32_t a_cross[5] = { A(out), A(v4_a), A(v4_b), A(v4_c) };
    ok("Vec4Cross", call("D3DXVec4Cross", 4, a_cross) == A(out)
       && vec4_eq(A(out), 0.0f, 0.0f, 0.0f, -1.0f));
    ok("stack: Vec4Cross", balanced("D3DXVec4Cross", 4, a_cross));
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);

    void *out = hle_alloc(64), *a1 = hle_alloc(64), *a2 = hle_alloc(64);

    /* identity */
    uint32_t args[4] = { A(out), 0, 0, 0 };
    ok("Identity returns pOut", call("D3DXMatrixIdentity", 1, args) == A(out));
    ok("Identity", mat_eq(A(out), ID));

    /* scaling and translation */
    float S[16] = { 2, 0, 0, 0,  0, 3, 0, 0,  0, 0, 4, 0,  0, 0, 0, 1 };
    float T[16] = { 1, 0, 0, 1,  0, 1, 0, 2,  0, 0, 1, 3,  0, 0, 0, 1 };
    args[1] = f2u(2), args[2] = f2u(3), args[3] = f2u(4);
    ok("Scaling returns pOut", call("D3DXMatrixScaling", 4, args) == A(out));
    ok("Scaling(2,3,4)", mat_eq(A(out), S));
    args[1] = f2u(1), args[2] = f2u(2), args[3] = f2u(3);
    ok("Translation(1,2,3)", call("D3DXMatrixTranslation", 4, args) == A(out) && mat_eq(A(out), T));

    /* multiply: S * T */
    float ST[16] = { 2, 0, 0, 2,  0, 3, 0, 6,  0, 0, 4, 12,  0, 0, 0, 1 };
    mat_set(A(a1), S), mat_set(A(a2), T);
    uint32_t m[3] = { A(out), A(a1), A(a2) };
    ok("Multiply returns pOut", call("D3DXMatrixMultiply", 3, m) == A(out));
    ok("S*T", mat_eq(A(out), ST));
    mat_set(A(a1), T), mat_set(A(a2), S);
    ok("T*S differs from S*T", call("D3DXMatrixMultiply", 3, m) == A(out) && !mat_eq(A(out), ST));

    /* transpose */
    float STt[16] = { 2, 0, 0, 0,  0, 3, 0, 0,  0, 0, 4, 0,  2, 6, 12, 1 };
    mat_set(A(a1), ST);
    uint32_t t[2] = { A(out), A(a1) };
    ok("Transpose returns pOut", call("D3DXMatrixTranspose", 2, t) == A(out));
    ok("transpose(S*T)", mat_eq(A(out), STt));
    t[1] = A(out);
    ok("double transpose", call("D3DXMatrixTranspose", 2, t) == A(out) && mat_eq(A(out), ST));

    /* inverse */
    float Ti[16] = { 1, 0, 0, -1,  0, 1, 0, -2,  0, 0, 1, -3,  0, 0, 0, 1 };
    mat_set(A(a1), T);
    uint32_t inv[2] = { A(out), A(a1) };
    ok("Inverse(T) returns pOut", call("D3DXMatrixInverse", 2, inv) == A(out));
    ok("Inverse(Translation(1,2,3))", mat_eq(A(out), Ti));
    float Si[16] = { 0.5f, 0, 0, 0,  0, 1.0f / 3, 0, 0,  0, 0, 0.25f, 0,  0, 0, 0, 1 };
    mat_set(A(a1), S);
    ok("Inverse(Scaling)", call("D3DXMatrixInverse", 2, inv) == A(out) && mat_eq(A(out), Si));
    /* out * in = I (the inverse really inverts) */
    mat_set(A(a1), out); mat_set(A(a2), S);
    m[0] = A(out); m[1] = A(a1); m[2] = A(a2);
    ok("S^-1 * S = I", call("D3DXMatrixMultiply", 3, m) == A(out) && mat_eq(A(out), ID));
    /* singular: two equal rows */
    float sing[16] = { 1, 2, 3, 4,  1, 2, 3, 4,  0, 0, 0, 1,  0, 0, 0, 1 };
    mat_set(A(a1), sing);
    ok("Inverse(singular) is NULL", call("D3DXMatrixInverse", 2, inv) == 0);

    /* the fixed-axis rotations at pi/2 */
    float Rx[16] = { 1, 0, 0, 0,  0, 0, 1, 0,  0, -1, 0, 0,  0, 0, 0, 1 };
    float Ry[16] = { 0, 0, -1, 0,  0, 1, 0, 0,  1, 0, 0, 0,  0, 0, 0, 1 };
    float Rz[16] = { 0, 1, 0, 0,  -1, 0, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    float halfpi = 3.14159265358979323846f / 2;
    uint32_t r[2] = { A(out), f2u(halfpi) };
    ok("RotationX(pi/2)", call("D3DXMatrixRotationX", 2, r) == A(out) && mat_eq(A(out), Rx));
    ok("RotationY(pi/2)", call("D3DXMatrixRotationY", 2, r) == A(out) && mat_eq(A(out), Ry));
    ok("RotationZ(pi/2)", call("D3DXMatrixRotationZ", 2, r) == A(out) && mat_eq(A(out), Rz));
    /* a rotation's inverse is its transpose */
    mat_set(A(a1), Rx);
    ok("Inverse(Rx) = transpose(Rx)", call("D3DXMatrixInverse", 2, inv) == A(out)
        && mat_eq(A(out), (float[]){ 1, 0, 0, 0,  0, 0, -1, 0,  0, 1, 0, 0,  0, 0, 0, 1 }));

    /* aliasing: pOut is pM */
    float asym[16] = { 1, 2, 3, 4,  5, 6, 7, 8,  9, 10, 11, 12,  13, 14, 15, 16 };
    float asymt[16] = { 1, 5, 9, 13,  2, 6, 10, 14,  3, 7, 11, 15,  4, 8, 12, 16 };
    mat_set(A(out), asym);
    uint32_t self[2] = { A(out), A(out) };
    ok("in-place transpose", call("D3DXMatrixTranspose", 2, self) == A(out) && mat_eq(A(out), asymt));
    mat_set(A(out), S);
    ok("in-place multiply S*S", call("D3DXMatrixMultiply", 3, self) == A(out)
        && mat_eq(A(out), (float[]){ 4, 0, 0, 0,  0, 9, 0, 0,  0, 0, 16, 0,  0, 0, 0, 1 }));

    /* stdcall: every shim pops ret + its args */
    ok("stack: Identity",     balanced("D3DXMatrixIdentity", 1, args));
    ok("stack: Multiply",     balanced("D3DXMatrixMultiply", 3, m));
    ok("stack: Inverse",      balanced("D3DXMatrixInverse", 2, inv));
    ok("stack: Transpose",    balanced("D3DXMatrixTranspose", 2, t));
    ok("stack: Scaling",      balanced("D3DXMatrixScaling", 4, args));
    ok("stack: Translation",  balanced("D3DXMatrixTranslation", 4, args));
    ok("stack: RotationX",    balanced("D3DXMatrixRotationX", 2, r));
    ok("stack: RotationY",    balanced("D3DXMatrixRotationY", 2, r));
    ok("stack: RotationZ",    balanced("D3DXMatrixRotationZ", 2, r));

    test_vectors();

    printf(fails ? "d3dx9_selftest: %d failed\n" : "d3dx9_selftest: ok\n", fails);
    return fails != 0;
}
