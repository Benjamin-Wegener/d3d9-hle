/*
 * d3dx9_math_selftest.c - D3DX9 math through the registered stdcall shims.
 *
 * Every expected value below is worked out here from the public D3DX9 docs,
 * not from the implementation under test. Float arguments are passed by value
 * on the guest stack (their raw bits); float results come back as raw bits.
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "win32hle.h"
#include "host.h"
#include "d3dx9_math.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) {
    if (!cond) { printf("FAIL %s\n", what); fails++; }
}
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
static int call_balanced(const char *name, int n, const uint32_t *a, uint32_t *out) {
    uint32_t esp0 = g_esp;
    uint32_t r = call(name, n, a);
    int okb = (g_esp == esp0);
    if (out) *out = r;
    if (!okb) printf("FAIL %s esp got 0x%x want 0x%x\n", name, g_esp, esp0);
    return okb;
}
#define A(p) hle_va(p)
/* Copy host data into guest memory; all guest arguments must live there. */
static void *G(const void *src, size_t n) {
    void *p = hle_alloc(n);
    if (src) memcpy(p, src, n);
    return p;
}
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float ff(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static int eq(float a, float b) { return fabsf(a - b) < 1e-5f; }
static int v4eq(float *v, float x, float y, float z, float w) {
    return eq(v[0], x) && eq(v[1], y) && eq(v[2], z) && eq(v[3], w);
}
static int mateq(float *m, const float *e) {
    for (int i = 0; i < 16; i++) if (!eq(m[i], e[i])) return 0;
    return 1;
}
static void matee(const char *what, float *m, const float *e) {
    if (!mateq(m, e)) {
        ok(what, 0);
        printf("    got ");
        for (int i = 0; i < 16; i++) printf("%g ", m[i]);
        printf("\n");
    } else ok(what, 1);
}

int main(void) {
    recomp_host_init();

    /* ---- LookAt LH/RH: Eye=(0,0,0), At=(1,0,0), Up=(0,1,0).
     * LH: z=(1,0,0), x=cross(Up,z)=(0,0,-1), y=(0,1,0).
     * RH: z=(-1,0,0), x=(0,0,1),  y=(0,1,0). Axes are the rows. */
    float s_eye[3] = { 0, 0, 0 }, s_at[3] = { 1, 0, 0 }, s_up[3] = { 0, 1, 0 };
    void *eye = G(s_eye, 12), *at = G(s_at, 12), *up = G(s_up, 12);
    float *out = (float *)G(NULL, 64);
    const float lh[16] = { 0, 0, 1, 0,  0, 1, 0, 0,  -1, 0, 0, 0,  0, 0, 0, 1 };
    const float rh[16] = { 0, 0, -1, 0, 0, 1, 0, 0,  1, 0, 0, 0,  0, 0, 0, 1 };
    uint32_t a_look[4] = { A(out), A(eye), A(at), A(up) };
    call("D3DXMatrixLookAtLH", 4, a_look);
    matee("LookAtLH axes", out, lh);
    call("D3DXMatrixLookAtRH", 4, a_look);
    matee("LookAtRH axes", out, rh);
    /* Aliasing: pOut is also pEye; the inputs are copied before computing. */
    memcpy(out, s_eye, 12);
    a_look[1] = A(out);
    call("D3DXMatrixLookAtLH", 4, a_look);
    matee("LookAtLH aliasing (pOut == pEye)", out, lh);

    /* ---- Perspective w=2 h=4 zn=1 zf=3.
     * LH m00=1, m11=0.5, m22=1.5, m23=1, m32=-0.5, m33=0.
     * RH m22=-1.5, m23=-1, m32=-1.5. */
    const float plh[16] = { 1,0,0,0,  0,0.5f,0,0,  0,0,1.5f,1,  0,0,-0.5f,0 };
    const float prh[16] = { 1,0,0,0,  0,0.5f,0,0,  0,0,-1.5f,-1, 0,0,-1.5f,0 };
    uint32_t a_proj[5] = { A(out), fb(2), fb(4), fb(1), fb(3) };
    call("D3DXMatrixPerspectiveLH", 5, a_proj);
    matee("PerspectiveLH", out, plh);
    call("D3DXMatrixPerspectiveRH", 5, a_proj);
    matee("PerspectiveRH", out, prh);

    /* ---- Ortho w=2 h=4 zn=1 zf=3: m00=1, m11=0.5.
     * LH m22=0.5; RH m22=-0.5; both m32=-0.5, m33=1. */
    const float olh[16] = { 1,0,0,0,  0,0.5f,0,0,  0,0,0.5f,0,  0,0,-0.5f,1 };
    const float orh[16] = { 1,0,0,0,  0,0.5f,0,0,  0,0,-0.5f,0, 0,0,-0.5f,1 };
    uint32_t a_ortho[5] = { A(out), fb(2), fb(4), fb(1), fb(3) };
    call("D3DXMatrixOrthoLH", 5, a_ortho);
    matee("OrthoLH", out, olh);
    call("D3DXMatrixOrthoRH", 5, a_ortho);
    matee("OrthoRH", out, orh);
    /* Edge case: zero width/height must not crash (IEEE division by zero). */
    uint32_t a_zero[5] = { A(out), fb(0), fb(0), fb(1), fb(3) };
    call("D3DXMatrixPerspectiveLH", 5, a_zero);
    ok("PerspectiveLH zero extents survives", 1);

    /* ---- Quaternions ---- */
    void *qout = G(NULL, 16);
    call("D3DXQuaternionIdentity", 1, (uint32_t[]){ A(qout) });
    ok("QuaternionIdentity", v4eq((float *)qout, 0, 0, 0, 1));

    float s_qx[4] = { 1, 0, 0, 0 }, s_qy[4] = { 0, 1, 0, 0 };
    void *qx = G(s_qx, 16), *qy = G(s_qy, 16);
    call("D3DXQuaternionMultiply", 3, (uint32_t[]){ A(qout), A(qx), A(qy) });
    /* (180 deg about x) * (180 deg about y) = (0,0,1,0). */
    ok("QuaternionMultiply", v4eq((float *)qout, 0, 0, 1, 0));
    call("D3DXQuaternionConjugate", 2, (uint32_t[]){ A(qout), A(qout) });
    ok("QuaternionConjugate (aliased)", v4eq((float *)qout, 0, 0, -1, 0));

    float s_qbig[4] = { 2, 0, 0, 0 }, s_qzero[4] = { 0, 0, 0, 0 };
    void *qbig = G(s_qbig, 16), *qzero = G(s_qzero, 16);
    call("D3DXQuaternionNormalize", 2, (uint32_t[]){ A(qout), A(qbig) });
    ok("QuaternionNormalize", v4eq((float *)qout, 1, 0, 0, 0));
    call("D3DXQuaternionNormalize", 2, (uint32_t[]){ A(qout), A(qzero) });
    ok("QuaternionNormalize zero stays zero", v4eq((float *)qout, 0, 0, 0, 0));

    float s_zaxis[3] = { 0, 0, 1 };
    void *zaxis = G(s_zaxis, 12);
    call("D3DXQuaternionRotationAxis", 3,
         (uint32_t[]){ A(qout), A(zaxis), fb((float)(M_PI / 2.0)) });
    float s45 = sinf((float)(M_PI / 4.0f)), c45 = cosf((float)(M_PI / 4.0f));
    ok("QuaternionRotationAxis(z, pi/2)", v4eq((float *)qout, 0, 0, s45, c45));

    void *axout = G(NULL, 12), *angout = G(NULL, 4);
    call("D3DXQuaternionToAxisAngle", 3, (uint32_t[]){ A(qout), A(axout), A(angout) });
    ok("ToAxisAngle axis", v4eq((float *)axout, 0, 0, 1, 0));
    ok("ToAxisAngle angle == pi/2", eq(ff(*(uint32_t *)angout), (float)(M_PI / 2.0)));
    float s_qid[4] = { 0, 0, 0, 1 };
    void *qid = G(s_qid, 16);
    call("D3DXQuaternionToAxisAngle", 3, (uint32_t[]){ A(qid), A(axout), A(angout) });
    ok("ToAxisAngle identity axis == (1,0,0)", v4eq((float *)axout, 1, 0, 0, 0));
    ok("ToAxisAngle identity angle == 0", eq(ff(*(uint32_t *)angout), 0));

    float s_qz[4] = { 0, 0, 1, 0 };
    void *qz = G(s_qz, 16);
    call("D3DXQuaternionSlerp", 4,
         (uint32_t[]){ A(qout), A(qid), A(qz), fb(0.5f) });
    ok("QuaternionSlerp halfway", v4eq((float *)qout, 0, 0, s45, c45));

    /* ---- Planes ---- */
    void *pout = G(NULL, 16);
    float s_point[3] = { 1, 2, 3 }, s_pnormal[3] = { 0, 1, 0 };
    void *point = G(s_point, 12), *pnormal = G(s_pnormal, 12);
    call("D3DXPlaneFromPointNormal", 3, (uint32_t[]){ A(pout), A(point), A(pnormal) });
    ok("PlaneFromPointNormal (0,1,0,-2)", v4eq((float *)pout, 0, 1, 0, -2));
    float s_pv[3] = { 4, 5, 6 };
    void *pv = G(s_pv, 12);
    uint32_t dotbits = call("D3DXPlaneDotCoord", 2, (uint32_t[]){ A(pout), A(pv) });
    ok("PlaneDotCoord == 3", eq(ff(dotbits), 3));
    dotbits = call("D3DXPlaneDotNormal", 2, (uint32_t[]){ A(pout), A(pv) });
    ok("PlaneDotNormal == 5", eq(ff(dotbits), 5));

    float s_p1[3] = { 0, 0, 0 }, s_p2[3] = { 1, 0, 0 }, s_p3[3] = { 0, 1, 0 };
    void *p1 = G(s_p1, 12), *p2 = G(s_p2, 12), *p3 = G(s_p3, 12);
    call("D3DXPlaneFromPoints", 4, (uint32_t[]){ A(pout), A(p1), A(p2), A(p3) });
    ok("PlaneFromPoints XY plane", v4eq((float *)pout, 0, 0, 1, 0));
    float s_pbig[4] = { 0, 2, 0, -4 }, s_pz[4] = { 0, 0, 0, 0 };
    void *pbig = G(s_pbig, 16), *pz = G(s_pz, 16);
    call("D3DXPlaneNormalize", 2, (uint32_t[]){ A(pout), A(pbig) });
    ok("PlaneNormalize", v4eq((float *)pout, 0, 1, 0, -2));
    call("D3DXPlaneNormalize", 2, (uint32_t[]){ A(pout), A(pz) });
    ok("PlaneNormalize zero stays zero", v4eq((float *)pout, 0, 0, 0, 0));

    /* ---- Colours ---- */
    void *cout = G(NULL, 16);
    float s_ca[4] = { 0.25f, 0.5f, 0.75f, 1.0f }, s_cb[4] = { 0.5f, 0.25f, 0.75f, 0.5f };
    void *ca = G(s_ca, 16), *cb = G(s_cb, 16);
    call("D3DXColorAdd", 3, (uint32_t[]){ A(cout), A(ca), A(cb) });
    ok("ColorAdd", v4eq((float *)cout, 0.75f, 0.75f, 1.5f, 1.5f));
    call("D3DXColorSubtract", 3, (uint32_t[]){ A(cout), A(ca), A(cb) });
    ok("ColorSubtract", v4eq((float *)cout, -0.25f, 0.25f, 0, 0.5f));
    call("D3DXColorModulate", 3, (uint32_t[]){ A(cout), A(ca), A(cb) });
    ok("ColorModulate", v4eq((float *)cout, 0.125f, 0.125f, 0.5625f, 0.5f));
    call("D3DXColorScale", 3, (uint32_t[]){ A(cout), A(ca), fb(2) });
    ok("ColorScale", v4eq((float *)cout, 0.5f, 1, 1.5f, 2));
    call("D3DXColorNegative", 2, (uint32_t[]){ A(cout), A(ca) });
    ok("ColorNegative", v4eq((float *)cout, 0.75f, 0.5f, 0.25f, 0));
    float s_c0[4] = { 0, 0, 0, 0 }, s_c1[4] = { 1, 1, 1, 1 };
    void *c0 = G(s_c0, 16), *c1 = G(s_c1, 16);
    call("D3DXColorLerp", 4, (uint32_t[]){ A(cout), A(c0), A(c1), fb(0.25f) });
    ok("ColorLerp", v4eq((float *)cout, 0.25f, 0.25f, 0.25f, 0.25f));
    float s_cred[4] = { 1, 0, 0.5f, 0.3f }, s_cred2[4] = { 1, 0, 0, 0.5f };
    void *cred = G(s_cred, 16), *cred2 = G(s_cred2, 16);
    call("D3DXColorAdjustContrast", 3, (uint32_t[]){ A(cout), A(cred), fb(0) });
    ok("ColorAdjustContrast(s=0) == 50% grey, alpha kept",
       v4eq((float *)cout, 0.5f, 0.5f, 0.5f, 0.3f));
    call("D3DXColorAdjustContrast", 3, (uint32_t[]){ A(cout), A(cred), fb(1) });
    ok("ColorAdjustContrast(s=1) unchanged", v4eq((float *)cout, 1, 0, 0.5f, 0.3f));
    call("D3DXColorAdjustSaturation", 3, (uint32_t[]){ A(cout), A(cred2), fb(0) });
    ok("ColorAdjustSaturation(s=0) == BT.709 grey, alpha kept",
       v4eq((float *)cout, 0.2125f, 0.2125f, 0.2125f, 0.5f));
    call("D3DXColorAdjustSaturation", 3, (uint32_t[]){ A(cout), A(cred2), fb(1) });
    ok("ColorAdjustSaturation(s=1) unchanged", v4eq((float *)cout, 1, 0, 0, 0.5f));

    /* ---- New matrix extras (D3DX row-vector layout, v' = v*M) ---- */
    {
        float *m = (float*)G(NULL,64);
        uint32_t r;
        ok("MatrixDeterminant esp", call_balanced("D3DXMatrixDeterminant",1,(uint32_t[]){A(out)},&r));
        call("D3DXMatrixIdentity",1,(uint32_t[]){A(m)});
        r = call("D3DXMatrixDeterminant",1,(uint32_t[]){A(m)});
        ok("MatrixDeterminant(identity)==1", eq(ff(r),1.0f));

        /* RotationAxis: z axis, pi/2. D3DX row-vector Rz: [[c,s,0,0],[-s,c,0,0],[0,0,1,0],[0,0,0,1]] */
        float ax[3]={0,0,1}; void *gax=G(ax,12);
        call("D3DXMatrixRotationAxis",3,(uint32_t[]){A(m),A(gax),fb((float)(M_PI/2.0))});
        const float rz[16]={0,1,0,0, -1,0,0,0, 0,0,1,0, 0,0,0,1};
        matee("MatrixRotationAxis z pi/2 (row-vector)", (float*)m, rz);

        /* RotationYawPitchRoll: yaw=pi/2,pitch=0,roll=0 => Ry(pi/2) row-vector: [[0,0,-1,0],[0,1,0,0],[1,0,0,0],[0,0,0,1]] */
        call("D3DXMatrixRotationYawPitchRoll",4,(uint32_t[]){A(m),fb((float)(M_PI/2.0)),fb(0),fb(0)});
        const float ry[16]={0,0,-1,0, 0,1,0,0, 1,0,0,0, 0,0,0,1};
        matee("MatrixRotationYawPitchRoll yaw pi/2 (row-vector)", (float*)m, ry);

        /* RotationQuaternion identity */
        float qi[4]={0,0,0,1}; void *gqi=G(qi,16);
        call("D3DXMatrixRotationQuaternion",2,(uint32_t[]){A(m),A(gqi)});
        const float I[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        matee("MatrixRotationQuaternion identity", (float*)m, I);
        /* RotationQuaternion: 90deg around z => same as Rz(pi/2) */
        float qz90[4]={0,0,sinf((float)M_PI/4),cosf((float)M_PI/4)}; void *gqz90=G(qz90,16);
        call("D3DXMatrixRotationQuaternion",2,(uint32_t[]){A(m),A(gqz90)});
        matee("MatrixRotationQuaternion z pi/2", (float*)m, rz);

        /* PerspectiveFovLH fovy=pi/2 aspect=1 zn=1 zf=2 => m00=1,m11=1,m22=2,m23=1,m32=-2 */
        call("D3DXMatrixPerspectiveFovLH",5,(uint32_t[]){A(m),fb((float)(M_PI/2.0)),fb(1),fb(1),fb(2)});
        const float pf[16]={1,0,0,0, 0,1,0,0, 0,0,2,1, 0,0,-2,0};
        matee("PerspectiveFovLH", (float*)m, pf);
        /* PerspectiveFovRH */
        call("D3DXMatrixPerspectiveFovRH",5,(uint32_t[]){A(m),fb((float)(M_PI/2.0)),fb(1),fb(1),fb(2)});
        const float pfrh[16]={1,0,0,0, 0,1,0,0, 0,0,-2,-1, 0,0,-2,0};
        matee("PerspectiveFovRH", (float*)m, pfrh);
        /* OrthoOffCenterLH l=0 r=2 b=0 t=2 zn=0 zf=1 */
        call("D3DXMatrixOrthoOffCenterLH",7,(uint32_t[]){A(m),fb(0),fb(2),fb(0),fb(2),fb(0),fb(1)});
        const float ooc[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, -1,-1,0,1};
        matee("OrthoOffCenterLH", (float*)m, ooc);
        /* OrthoOffCenterRH */
        call("D3DXMatrixOrthoOffCenterRH",7,(uint32_t[]){A(m),fb(0),fb(2),fb(0),fb(2),fb(0),fb(1)});
        const float oocrh[16]={1,0,0,0, 0,1,0,0, 0,0,-1,0, -1,-1,0,1};
        matee("OrthoOffCenterRH", (float*)m, oocrh);
        /* PerspectiveOffCenterLH l=-1 r=1 b=-1 t=1 zn=1 zf=2 */
        call("D3DXMatrixPerspectiveOffCenterLH",7,(uint32_t[]){A(m),fb(-1),fb(1),fb(-1),fb(1),fb(1),fb(2)});
        const float poclh[16]={1,0,0,0, 0,1,0,0, 0,0,2,1, 0,0,-2,0};
        matee("PerspectiveOffCenterLH", (float*)m, poclh);
        /* PerspectiveOffCenterRH */
        call("D3DXMatrixPerspectiveOffCenterRH",7,(uint32_t[]){A(m),fb(-1),fb(1),fb(-1),fb(1),fb(1),fb(2)});
        const float pocrh[16]={1,0,0,0, 0,1,0,0, 0,0,-2,-1, 0,0,-2,0};
        matee("PerspectiveOffCenterRH", (float*)m, pocrh);

        /* Reflect plane z=0 => (0,0,1,0) row-vector */
        float pln[4]={0,0,1,0}; void *gpl=G(pln,16);
        call("D3DXMatrixReflect",2,(uint32_t[]){A(m),A(gpl)});
        const float refl[16]={1,0,0,0, 0,1,0,0, 0,0,-1,0, 0,0,0,1};
        matee("MatrixReflect z=0", (float*)m, refl);

        /* Shadow: plane z=0, directional light (0,0,-1,0) => projects onto z=0 */
        float light[4]={0,0,-1,0}, plane[4]={0,0,1,0};
        void *gl=G(light,16), *gpl2=G(plane,16);
        call("D3DXMatrixShadow",3,(uint32_t[]){A(m),A(gl),A(gpl2)});
        /* D3DXMatrixShadow formula: dot=-1 for L=(0,0,-1,0), P=(0,0,1,0) */
        const float shadow[16]={-1,0,0,0, 0,-1,0,0, 0,0,0,0, 0,0,0,-1};
        matee("MatrixShadow dir light z=0", (float*)m, shadow);
    }

    /* ---- New vector extras ---- */
    {
        float a2[2]={3,4}, b2[2]={1,2}; void *ga2=G(a2,8), *gb2=G(b2,8), *go2=G(NULL,8);
        uint32_t r;
        ok("Vec2Length esp", call_balanced("D3DXVec2Length",1,(uint32_t[]){A(ga2)},&r));
        ok("Vec2Length(3,4)==5", eq(ff(r),5.0f));
        r=call("D3DXVec2Dot",2,(uint32_t[]){A(ga2),A(gb2)});
        ok("Vec2Dot==11", eq(ff(r),11.0f));
        call("D3DXVec2Add",3,(uint32_t[]){A(go2),A(ga2),A(gb2)});
        ok("Vec2Add", eq(((float*)go2)[0],4) && eq(((float*)go2)[1],6));
        call("D3DXVec2Subtract",3,(uint32_t[]){A(go2),A(ga2),A(gb2)});
        ok("Vec2Subtract", eq(((float*)go2)[0],2) && eq(((float*)go2)[1],2));
        call("D3DXVec2Minimize",3,(uint32_t[]){A(go2),A(ga2),A(gb2)});
        ok("Vec2Minimize", eq(((float*)go2)[0],1) && eq(((float*)go2)[1],2));
        call("D3DXVec2Maximize",3,(uint32_t[]){A(go2),A(ga2),A(gb2)});
        ok("Vec2Maximize", eq(((float*)go2)[0],3) && eq(((float*)go2)[1],4));
        call("D3DXVec2Lerp",4,(uint32_t[]){A(go2),A(ga2),A(gb2),fb(0.5f)});
        ok("Vec2Lerp", eq(((float*)go2)[0],2) && eq(((float*)go2)[1],3));

        float a3[3]={1,0,0}, b3[3]={0,1,0}; void *ga3=G(a3,12), *gb3=G(b3,12), *go3=G(NULL,12);
        r=call("D3DXVec3Length",1,(uint32_t[]){A(ga3)});
        ok("Vec3Length==1", eq(ff(r),1.0f));
        r=call("D3DXVec3Dot",2,(uint32_t[]){A(ga3),A(gb3)});
        ok("Vec3Dot==0", eq(ff(r),0.0f));
        call("D3DXVec3Cross",3,(uint32_t[]){A(go3),A(ga3),A(gb3)});
        ok("Vec3Cross x,y=z", v4eq((float*)go3,0,0,1,0));
        call("D3DXVec3Subtract",3,(uint32_t[]){A(go3),A(ga3),A(gb3)});
        ok("Vec3Subtract", v4eq((float*)go3,1,-1,0,0));
        call("D3DXVec3Minimize",3,(uint32_t[]){A(go3),A(ga3),A(gb3)});
        ok("Vec3Minimize", v4eq((float*)go3,0,0,0,0));
        call("D3DXVec3Maximize",3,(uint32_t[]){A(go3),A(ga3),A(gb3)});
        ok("Vec3Maximize", v4eq((float*)go3,1,1,0,0));

        float a4[4]={1,2,3,4}, b4[4]={2,0,0,0}; void *ga4=G(a4,16), *gb4=G(b4,16), *go4=G(NULL,16);
        r=call("D3DXVec4Dot",2,(uint32_t[]){A(ga4),A(gb4)});
        ok("Vec4Dot==2", eq(ff(r),2.0f));
        call("D3DXVec4Subtract",3,(uint32_t[]){A(go4),A(ga4),A(gb4)});
        ok("Vec4Subtract", v4eq((float*)go4,-1,2,3,4));
        call("D3DXVec4Minimize",3,(uint32_t[]){A(go4),A(ga4),A(gb4)});
        ok("Vec4Minimize", v4eq((float*)go4,1,0,0,0));
        call("D3DXVec4Maximize",3,(uint32_t[]){A(go4),A(ga4),A(gb4)});
        ok("Vec4Maximize", v4eq((float*)go4,2,2,3,4));
        call("D3DXVec4Scale",3,(uint32_t[]){A(go4),A(ga4),fb(2)});
        ok("Vec4Scale", v4eq((float*)go4,2,4,6,8));
    }

    /* ---- Array variants ---- */
    {
        float in2[4]={1,0, 0,1}; float *out2=(float*)G(NULL,16); void *gin2=G(in2,16);
        float *m=(float*)G(NULL,64); call("D3DXMatrixIdentity",1,(uint32_t[]){A(m)});
        /* scale by 2 in x: use scaling matrix */
        call("D3DXMatrixScaling",4,(uint32_t[]){A(m),fb(2),fb(1),fb(1)});
        call("D3DXVec2TransformCoordArray",6,(uint32_t[]){A(out2),8,A(gin2),8,A(m),2});
        ok("Vec2TransformCoordArray[0]", eq(out2[0],2) && eq(out2[1],0));
        ok("Vec2TransformCoordArray[1]", eq(out2[2],0) && eq(out2[3],1));

        float in3[6]={1,0,0, 0,2,0}; float *out3=(float*)G(NULL,24); void *gin3=G(in3,24);
        call("D3DXMatrixIdentity",1,(uint32_t[]){A(m)});
        call("D3DXMatrixTranslation",4,(uint32_t[]){A(m),fb(1),fb(0),fb(0)});
        call("D3DXVec3TransformCoordArray",6,(uint32_t[]){A(out3),12,A(gin3),12,A(m),2});
        ok("Vec3TransformCoordArray[0]", eq(out3[0],2)&&eq(out3[1],0)&&eq(out3[2],0));
        ok("Vec3TransformCoordArray[1]", eq(out3[3],1)&&eq(out3[4],2)&&eq(out3[5],0));

        float nin3[6]={1,0,0, 0,1,0}; float *nout3=(float*)G(NULL,24); void *gnin3=G(nin3,24);
        call("D3DXMatrixScaling",4,(uint32_t[]){A(m),fb(2),fb(1),fb(1)});
        call("D3DXVec3TransformNormalArray",6,(uint32_t[]){A(nout3),12,A(gnin3),12,A(m),2});
        ok("Vec3TransformNormalArray[0]", eq(nout3[0],2)&&eq(nout3[1],0)&&eq(nout3[2],0));
        ok("Vec3TransformNormalArray[1]", eq(nout3[3],0)&&eq(nout3[4],1)&&eq(nout3[5],0));

        float nin2[4]={1,0, 0,1}; float *nout2=(float*)G(NULL,16); void *gnin2=G(nin2,16);
        call("D3DXVec2TransformNormalArray",6,(uint32_t[]){A(nout2),8,A(gnin2),8,A(m),2});
        ok("Vec2TransformNormalArray[0]", eq(nout2[0],2)&&eq(nout2[1],0));
        ok("Vec2TransformNormalArray[1]", eq(nout2[2],0)&&eq(nout2[3],1));

        float in4[8]={1,0,0,1, 0,1,0,1}; float *out4v=(float*)G(NULL,32); void *gin4=G(in4,32);
        call("D3DXMatrixIdentity",1,(uint32_t[]){A(m)});
        call("D3DXMatrixTranslation",4,(uint32_t[]){A(m),fb(1),fb(2),fb(3)});
        call("D3DXVec4TransformArray",6,(uint32_t[]){A(out4v),16,A(gin4),16,A(m),2});
        ok("Vec4TransformArray[0]", v4eq(out4v,2,2,3,1));
        ok("Vec4TransformArray[1]", v4eq(out4v+4,1,3,3,1));
    }

    /* ---- New quat/plane extras ---- */
    {
        float q[4]={0,0,0,1}; void *gq=G(q,16), *go=G(NULL,16);
        uint32_t r;
        r=call("D3DXQuaternionLength",1,(uint32_t[]){A(gq)});
        ok("QuaternionLength identity==1", eq(ff(r),1.0f));
        float q2[4]={0,0,sinf((float)M_PI/4),cosf((float)M_PI/4)}; void *gq2=G(q2,16);
        r=call("D3DXQuaternionDot",2,(uint32_t[]){A(gq),A(gq2)});
        ok("QuaternionDot", eq(ff(r),cosf((float)M_PI/4)));
        call("D3DXQuaternionInverse",2,(uint32_t[]){A(go),A(gq2)});
        float inv[4]={0,0,-sinf((float)M_PI/4),cosf((float)M_PI/4)};
        ok("QuaternionInverse", v4eq((float*)go,inv[0],inv[1],inv[2],inv[3]));

        /* QuaternionLn/Exp: Ln(identity) ~ (0,0,0,0), Exp(0) ~ identity */
        float lnq[4]; void *gln=G(lnq,16);
        call("D3DXQuaternionLn",2,(uint32_t[]){A(gln),A(gq)});
        ok("QuaternionLn(identity)", v4eq((float*)gln,0,0,0,0));
        float qexp_in[4]={0,0,0,0}; void *gqexp_in=G(qexp_in,16), *gqexp=G(NULL,16);
        call("D3DXQuaternionExp",2,(uint32_t[]){A(gqexp),A(gqexp_in)});
        ok("QuaternionExp(0)==identity", v4eq((float*)gqexp,0,0,0,1));

        /* QuaternionRotationMatrix from Rz(pi/2) */
        float *m=(float*)G(NULL,64);
        const float rz[16]={0,1,0,0,-1,0,0,0,0,0,1,0,0,0,0,1};
        memcpy(m,rz,64);
        call("D3DXQuaternionRotationMatrix",2,(uint32_t[]){A(go),A(m)});
        ok("QuaternionRotationMatrix Rz(pi/2)", v4eq((float*)go,0,0,sinf((float)M_PI/4),cosf((float)M_PI/4)));

        /* QuaternionRotationYawPitchRoll yaw=pi/2 => same as z rotation? D3DX: yaw around Y */
        call("D3DXQuaternionRotationYawPitchRoll",4,(uint32_t[]){A(go),fb((float)(M_PI/2.0)),fb(0),fb(0)});
        ok("QuaternionRotationYawPitchRoll yaw pi/2", v4eq((float*)go,0,sinf((float)M_PI/4),0,cosf((float)M_PI/4)));

        float pl[4]={0,2,0,2}, v4p[4]={1,1,1,1}; void *gpl=G(pl,16), *gv4=G(v4p,16);
        r=call("D3DXPlaneDot",2,(uint32_t[]){A(gpl),A(gv4)});
        ok("PlaneDot==4", eq(ff(r),4.0f));
        /* PlaneScale */
        float ps[4]={1,2,3,4}; void *gps=G(ps,16), *gpo=G(NULL,16);
        call("D3DXPlaneScale",3,(uint32_t[]){A(gpo),A(gps),fb(2)});
        ok("PlaneScale", v4eq((float*)gpo,2,4,6,8));
        /* PlaneTransform: identity */
        float pm[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; void *gpm=G(pm,64);
        float pl2[4]={0,0,1,0}; void *gpl2=G(pl2,16);
        call("D3DXPlaneTransform",3,(uint32_t[]){A(gpo),A(gpl2),A(gpm)});
        ok("PlaneTransform identity", v4eq((float*)gpo,0,0,1,0));
    }

    printf(fails == 0 ? "d3dx9_math_selftest: all checks passed\n"
                      : "d3dx9_math_selftest: FAILED\n");
    return fails != 0;
}
