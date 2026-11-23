/*
 * d3dx9.c - D3DX9 (d3dx9_24.dll .. d3dx9_43.dll), the helper library a
 * DirectX 9 program links beside d3d9: its math, texture creation, the effect
 * framework and the sprite/font helpers. All versions export the same names,
 * so they are registered without a DLL prefix.
 *
 * Every D3DX function is stdcall: the arguments are read with A32 (a float
 * arrives as a 4-byte slot), and a function that hands back its pOut gives it
 * in eax. The math lives in d3dx9_math.c, computed in float, row-major, for
 * row vectors (v' = v * M), as D3DX does.
 */
#include <string.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3dx9_math.h"
#include "d3dx9_sh.h"
#include "d3dx9_consttable.h"
#include "d3dx9_texture.h"
#include "d3dx9_shapes.h"
#include "d3dx9_xfile.h"
#include "d3dx9_mesh.h"
#include "d3dx9_meshtools.h"
#include "d3dx9_line.h"
#include "d3dx9_matrixstack.h"
#include "d3dx9_font.h"
#include "d3dx9_anim.h"
#include "d3dx9_skin.h"
#include "d3dx9_rts.h"
#include "d3dx9_envmap.h"
#include "d3dx9_buffer.h"
#include "d3dx9_float16.h"
#include "d3dx9_asm.h"
#include "d3dx9_shtools.h"
#include "d3dx9_misc.h"

/* Die Surface-Helfer leben in d3dx9_surface.c und werden hier registriert. */
extern void s_D3DXLoadSurfaceFromMemory(void);
extern void s_D3DXLoadSurfaceFromFileA(void);
extern void s_D3DXLoadSurfaceFromFileInMemory(void);
extern void s_D3DXLoadVolumeFromMemory(void);
extern void s_D3DXLoadSurfaceFromSurface(void);
extern void s_D3DXFilterTexture(void);
extern void s_D3DXSaveSurfaceToFileA(void);

/* D3DXCreateSprite (ID3DXSprite), implemented in d3dx9_sprite.c. */
void s_D3DXCreateSprite(void);

/* a float argument: 4 bytes by value on the stack */
static float farg(int i) { float f; uint32_t u = A32(i); memcpy(&f, &u, 4); return f; }

/* ---------------------------------------------------------------- matrices */

static void s_D3DXMatrixIdentity(void) {                  /* (pOut) */
    d3dx9_matrix m;
    d3dx9_matrix_identity(&m);
    if (APTR(0)) memcpy(APTR(0), &m, 64);
    RET(A32(0), 1);
}

static void s_D3DXMatrixMultiply(void) {                  /* (pOut, pA, pB) = pA * pB */
    d3dx9_matrix a, b;
    memcpy(&a, APTR(1), 64);
    memcpy(&b, APTR(2), 64);
    d3dx9_matrix_multiply(&a, &a, &b);
    if (APTR(0)) memcpy(APTR(0), &a, 64);
    RET(A32(0), 3);
}

static void s_D3DXMatrixInverse(void) {                   /* (pOut, pM): NULL when singular */
    d3dx9_matrix m;
    uint32_t out = A32(0);
    memcpy(&m, APTR(1), 64);
    if (!d3dx9_matrix_inverse(&m, &m)) RET(0, 2);
    if (out) memcpy(hle_ptr(out), &m, 64);
    RET(out, 2);
}

static void s_D3DXMatrixTranspose(void) {                 /* (pOut, pM) */
    d3dx9_matrix m;
    memcpy(&m, APTR(1), 64);
    d3dx9_matrix_transpose(&m, &m);
    if (APTR(0)) memcpy(APTR(0), &m, 64);
    RET(A32(0), 2);
}

static void s_D3DXMatrixScaling(void) {                    /* (pOut, x, y, z) */
    d3dx9_matrix m;
    d3dx9_matrix_scaling(&m, farg(1), farg(2), farg(3));
    if (APTR(0)) memcpy(APTR(0), &m, 64);
    RET(A32(0), 4);
}

static void s_D3DXMatrixTranslation(void) {                /* (pOut, x, y, z) */
    d3dx9_matrix m;
    d3dx9_matrix_translation(&m, farg(1), farg(2), farg(3));
    if (APTR(0)) memcpy(APTR(0), &m, 64);
    RET(A32(0), 4);
}

static void s_D3DXMatrixRotationX(void) {                  /* (pOut, angle) */
    d3dx9_matrix m;
    d3dx9_matrix_rotation_x(&m, farg(1));
    if (APTR(0)) memcpy(APTR(0), &m, 64);
    RET(A32(0), 2);
}

static void s_D3DXMatrixRotationY(void) {                  /* (pOut, angle) */
    d3dx9_matrix m;
    d3dx9_matrix_rotation_y(&m, farg(1));
    if (APTR(0)) memcpy(APTR(0), &m, 64);
    RET(A32(0), 2);
}

static void s_D3DXMatrixRotationZ(void) {                  /* (pOut, angle) */
    d3dx9_matrix m;
    d3dx9_matrix_rotation_z(&m, farg(1));
    if (APTR(0)) memcpy(APTR(0), &m, 64);
    RET(A32(0), 2);
}

/* ---------------------------------------------------------------- vectors */

static void s_D3DXVec2Normalize(void) {                   /* (pOut, pV) */
    d3dx9_vec2 v;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    d3dx9_vec2_normalize(&v, &v);
    if (APTR(0)) memcpy(APTR(0), &v, sizeof v);
    RET(A32(0), 2);
}

static void s_D3DXVec2Transform(void) {                   /* (pOut, pV, pM) */
    d3dx9_vec2 v;
    d3dx9_vec4 out;
    d3dx9_matrix m;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    if (APTR(2)) memcpy(&m, APTR(2), sizeof m);
    d3dx9_vec2_transform(&out, &v, &m);
    if (APTR(0)) memcpy(APTR(0), &out, sizeof out);
    RET(A32(0), 3);
}

static void s_D3DXVec2TransformCoord(void) {              /* (pOut, pV, pM) */
    d3dx9_vec2 v;
    d3dx9_matrix m;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    if (APTR(2)) memcpy(&m, APTR(2), sizeof m);
    d3dx9_vec2_transform_coord(&v, &v, &m);
    if (APTR(0)) memcpy(APTR(0), &v, sizeof v);
    RET(A32(0), 3);
}

static void s_D3DXVec2TransformNormal(void) {             /* (pOut, pV, pM) */
    d3dx9_vec2 v;
    d3dx9_matrix m;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    if (APTR(2)) memcpy(&m, APTR(2), sizeof m);
    d3dx9_vec2_transform_normal(&v, &v, &m);
    if (APTR(0)) memcpy(APTR(0), &v, sizeof v);
    RET(A32(0), 3);
}

static void s_D3DXVec3Normalize(void) {                   /* (pOut, pV) */
    d3dx9_vec3 v;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    d3dx9_vec3_normalize(&v, &v);
    if (APTR(0)) memcpy(APTR(0), &v, sizeof v);
    RET(A32(0), 2);
}

static void s_D3DXVec3Transform(void) {                   /* (pOut, pV, pM) */
    d3dx9_vec3 v;
    d3dx9_vec4 out;
    d3dx9_matrix m;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    if (APTR(2)) memcpy(&m, APTR(2), sizeof m);
    d3dx9_vec3_transform(&out, &v, &m);
    if (APTR(0)) memcpy(APTR(0), &out, sizeof out);
    RET(A32(0), 3);
}

static void s_D3DXVec3TransformCoord(void) {              /* (pOut, pV, pM) */
    d3dx9_vec3 v;
    d3dx9_matrix m;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    if (APTR(2)) memcpy(&m, APTR(2), sizeof m);
    d3dx9_vec3_transform_coord(&v, &v, &m);
    if (APTR(0)) memcpy(APTR(0), &v, sizeof v);
    RET(A32(0), 3);
}

static void s_D3DXVec3TransformNormal(void) {             /* (pOut, pV, pM) */
    d3dx9_vec3 v;
    d3dx9_matrix m;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    if (APTR(2)) memcpy(&m, APTR(2), sizeof m);
    d3dx9_vec3_transform_normal(&v, &v, &m);
    if (APTR(0)) memcpy(APTR(0), &v, sizeof v);
    RET(A32(0), 3);
}

static void s_D3DXVec4Normalize(void) {                   /* (pOut, pV) */
    d3dx9_vec4 v;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    d3dx9_vec4_normalize(&v, &v);
    if (APTR(0)) memcpy(APTR(0), &v, sizeof v);
    RET(A32(0), 2);
}

static void s_D3DXVec4Transform(void) {                   /* (pOut, pV, pM) */
    d3dx9_vec4 v;
    d3dx9_matrix m;
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    if (APTR(2)) memcpy(&m, APTR(2), sizeof m);
    d3dx9_vec4_transform(&v, &v, &m);
    if (APTR(0)) memcpy(APTR(0), &v, sizeof v);
    RET(A32(0), 3);
}

static void s_D3DXVec4Cross(void) {                       /* (pOut, pV1, pV2, pV3) */
    d3dx9_vec4 v1, v2, v3, out;
    if (APTR(1)) memcpy(&v1, APTR(1), sizeof v1);
    if (APTR(2)) memcpy(&v2, APTR(2), sizeof v2);
    if (APTR(3)) memcpy(&v3, APTR(3), sizeof v3);
    d3dx9_vec4_cross(&out, &v1, &v2, &v3);
    if (APTR(0)) memcpy(APTR(0), &out, sizeof out);
    RET(A32(0), 4);
}

/* --------------------------------------------------------- view/projection */

#define MATRIX_SHIM(name, fn, argc) \
    static void name(void) { d3dx9_matrix m; fn(&m, farg(1), farg(2), farg(3), farg(4)); \
        if (APTR(0)) memcpy(APTR(0), &m, sizeof m); RET(A32(0), argc); }
MATRIX_SHIM(s_D3DXMatrixPerspectiveLH, d3dx9_matrix_perspective_lh, 5)
MATRIX_SHIM(s_D3DXMatrixPerspectiveRH, d3dx9_matrix_perspective_rh, 5)
MATRIX_SHIM(s_D3DXMatrixOrthoLH, d3dx9_matrix_ortho_lh, 5)
MATRIX_SHIM(s_D3DXMatrixOrthoRH, d3dx9_matrix_ortho_rh, 5)

#define LOOKAT_SHIM(name, fn) \
    static void name(void) { d3dx9_vec3 eye, at, up; \
        if (APTR(1)) memcpy(&eye, APTR(1), sizeof eye); \
        if (APTR(2)) memcpy(&at, APTR(2), sizeof at); \
        if (APTR(3)) memcpy(&up, APTR(3), sizeof up); \
        d3dx9_matrix m; fn(&m, &eye, &at, &up); \
        if (APTR(0)) memcpy(APTR(0), &m, sizeof m); RET(A32(0), 4); }
LOOKAT_SHIM(s_D3DXMatrixLookAtLH, d3dx9_matrix_look_at_lh)
LOOKAT_SHIM(s_D3DXMatrixLookAtRH, d3dx9_matrix_look_at_rh)

/* --------------------------------------------------------------- quaternions */

static void s_D3DXQuaternionIdentity(void) {
    d3dx9_quat q; d3dx9_quat_identity(&q);
    if (APTR(0)) memcpy(APTR(0), &q, sizeof q);
    RET(A32(0), 1);
}
static void s_D3DXQuaternionMultiply(void) {           /* (pOut, pQ1, pQ2) */
    d3dx9_quat a, b;
    if (APTR(1)) memcpy(&a, APTR(1), sizeof a);
    if (APTR(2)) memcpy(&b, APTR(2), sizeof b);
    d3dx9_quat_multiply(&a, &a, &b);
    if (APTR(0)) memcpy(APTR(0), &a, sizeof a);
    RET(A32(0), 3);
}
static void s_D3DXQuaternionNormalize(void) {          /* (pOut, pQ) */
    d3dx9_quat q;
    if (APTR(1)) memcpy(&q, APTR(1), sizeof q);
    d3dx9_quat_normalize(&q, &q);
    if (APTR(0)) memcpy(APTR(0), &q, sizeof q);
    RET(A32(0), 2);
}
static void s_D3DXQuaternionConjugate(void) {
    d3dx9_quat q;
    if (APTR(1)) memcpy(&q, APTR(1), sizeof q);
    d3dx9_quat_conjugate(&q, &q);
    if (APTR(0)) memcpy(APTR(0), &q, sizeof q);
    RET(A32(0), 2);
}
static void s_D3DXQuaternionRotationAxis(void) {       /* (pOut, pAxis, Angle) */
    d3dx9_vec3 axis = { 0, 0, 0 };
    if (APTR(1)) memcpy(&axis, APTR(1), sizeof axis);
    d3dx9_quat q; d3dx9_quat_rotation_axis(&q, &axis, farg(2));
    if (APTR(0)) memcpy(APTR(0), &q, sizeof q);
    RET(A32(0), 3);
}
static void s_D3DXQuaternionToAxisAngle(void) {        /* (pQ, pAxis, pAngle) */
    d3dx9_quat q;
    if (APTR(0)) memcpy(&q, APTR(0), sizeof q);
    d3dx9_vec3 axis; float angle;
    d3dx9_quat_to_axis_angle(&q, &axis, &angle);
    if (APTR(1)) memcpy(APTR(1), &axis, sizeof axis);
    if (APTR(2)) memcpy(APTR(2), &angle, sizeof angle);
    RET(A32(0), 3);
}
static void s_D3DXQuaternionSlerp(void) {              /* (pOut, pQ1, pQ2, t) */
    d3dx9_quat a, b;
    if (APTR(1)) memcpy(&a, APTR(1), sizeof a);
    if (APTR(2)) memcpy(&b, APTR(2), sizeof b);
    d3dx9_quat q; d3dx9_quat_slerp(&q, &a, &b, farg(3));
    if (APTR(0)) memcpy(APTR(0), &q, sizeof q);
    RET(A32(0), 4);
}

/* -------------------------------------------------------------------- planes */

static void s_D3DXPlaneFromPointNormal(void) {
    d3dx9_vec3 point, normal;
    if (APTR(1)) memcpy(&point, APTR(1), sizeof point);
    if (APTR(2)) memcpy(&normal, APTR(2), sizeof normal);
    d3dx9_plane p; d3dx9_plane_from_point_normal(&p, &point, &normal);
    if (APTR(0)) memcpy(APTR(0), &p, sizeof p);
    RET(A32(0), 3);
}
static void s_D3DXPlaneFromPoints(void) {
    d3dx9_vec3 v1, v2, v3;
    if (APTR(1)) memcpy(&v1, APTR(1), sizeof v1);
    if (APTR(2)) memcpy(&v2, APTR(2), sizeof v2);
    if (APTR(3)) memcpy(&v3, APTR(3), sizeof v3);
    d3dx9_plane p; d3dx9_plane_from_points(&p, &v1, &v2, &v3);
    if (APTR(0)) memcpy(APTR(0), &p, sizeof p);
    RET(A32(0), 4);
}
static void s_D3DXPlaneNormalize(void) {
    d3dx9_plane p;
    if (APTR(1)) memcpy(&p, APTR(1), sizeof p);
    d3dx9_plane_normalize(&p, &p);
    if (APTR(0)) memcpy(APTR(0), &p, sizeof p);
    RET(A32(0), 2);
}
/* D3DX returns a FLOAT; on this HLE its bits are handed back in EAX. */
static void s_D3DXPlaneDotCoord(void) {
    d3dx9_plane p; d3dx9_vec3 v;
    if (APTR(0)) memcpy(&p, APTR(0), sizeof p);
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    float f = d3dx9_plane_dot_coord(&p, &v);
    uint32_t bits; memcpy(&bits, &f, 4); RET(bits, 2);
}
static void s_D3DXPlaneDotNormal(void) {
    d3dx9_plane p; d3dx9_vec3 v;
    if (APTR(0)) memcpy(&p, APTR(0), sizeof p);
    if (APTR(1)) memcpy(&v, APTR(1), sizeof v);
    float f = d3dx9_plane_dot_normal(&p, &v);
    uint32_t bits; memcpy(&bits, &f, 4); RET(bits, 2);
}

/* -------------------------------------------------------------------- colors */

#define COLOR2_SHIM(name, fn) \
    static void name(void) { d3dx9_color x, y; \
        if (APTR(1)) memcpy(&x, APTR(1), sizeof x); \
        if (APTR(2)) memcpy(&y, APTR(2), sizeof y); \
        d3dx9_color t; fn(&t, &x, &y); \
        if (APTR(0)) memcpy(APTR(0), &t, sizeof t); RET(A32(0), 3); }
#define COLOR1F_SHIM(name, fn) \
    static void name(void) { d3dx9_color c; \
        if (APTR(1)) memcpy(&c, APTR(1), sizeof c); \
        d3dx9_color t; fn(&t, &c, farg(2)); \
        if (APTR(0)) memcpy(APTR(0), &t, sizeof t); RET(A32(0), 3); }
COLOR2_SHIM(s_D3DXColorAdd, d3dx9_color_add)
COLOR2_SHIM(s_D3DXColorSubtract, d3dx9_color_subtract)
COLOR2_SHIM(s_D3DXColorModulate, d3dx9_color_modulate)
static void s_D3DXColorLerp(void) {              /* (pOut, pC1, pC2, s) */
    d3dx9_color x, y;
    if (APTR(1)) memcpy(&x, APTR(1), sizeof x);
    if (APTR(2)) memcpy(&y, APTR(2), sizeof y);
    d3dx9_color t; d3dx9_color_lerp(&t, &x, &y, farg(3));
    if (APTR(0)) memcpy(APTR(0), &t, sizeof t);
    RET(A32(0), 4);
}
COLOR1F_SHIM(s_D3DXColorScale, d3dx9_color_scale)
COLOR1F_SHIM(s_D3DXColorAdjustContrast, d3dx9_color_adjust_contrast)
COLOR1F_SHIM(s_D3DXColorAdjustSaturation, d3dx9_color_adjust_saturation)
static void s_D3DXColorNegative(void) {
    d3dx9_color c;
    if (APTR(1)) memcpy(&c, APTR(1), sizeof c);
    d3dx9_color t; d3dx9_color_negative(&t, &c);
    if (APTR(0)) memcpy(APTR(0), &t, sizeof t);
    RET(A32(0), 2);
}

/* -------------------------------------------------------- math extras (SHIMs) */

static void s_D3DXMatrixDeterminant(void) { /* (pM) -> float */
    d3dx9_matrix m; if (APTR(0)) memcpy(&m, APTR(0), sizeof m);
    float f = d3dx9_matrix_determinant(&m);
    uint32_t bits; memcpy(&bits,&f,4); RET(bits,1);
}
static void s_D3DXMatrixRotationAxis(void) { /* (pOut,pV,Angle) */
    d3dx9_vec3 axis={0,0,0}; if (APTR(1)) memcpy(&axis, APTR(1), sizeof axis);
    d3dx9_matrix m; d3dx9_matrix_rotation_axis(&m,&axis,farg(2));
    if (APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),3);
}
static void s_D3DXMatrixRotationYawPitchRoll(void) { /* (pOut,Yaw,Pitch,Roll) */
    d3dx9_matrix m; d3dx9_matrix_rotation_yaw_pitch_roll(&m,farg(1),farg(2),farg(3));
    if (APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),4);
}
static void s_D3DXMatrixRotationQuaternion(void) { /* (pOut,pQ) */
    d3dx9_quat q; if (APTR(1)) memcpy(&q, APTR(1), sizeof q);
    d3dx9_matrix m; d3dx9_matrix_rotation_quaternion(&m,&q);
    if (APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),2);
}
static void s_D3DXMatrixReflect(void) { /* (pOut,pPlane) */
    d3dx9_plane p; if (APTR(1)) memcpy(&p, APTR(1), sizeof p);
    d3dx9_matrix m; d3dx9_matrix_reflect(&m,&p);
    if (APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),2);
}
static void s_D3DXMatrixShadow(void) { /* (pOut,pLight,pPlane) */
    d3dx9_vec4 l; d3dx9_plane p;
    if (APTR(1)) memcpy(&l, APTR(1), sizeof l);
    if (APTR(2)) memcpy(&p, APTR(2), sizeof p);
    d3dx9_matrix m; d3dx9_matrix_shadow(&m,&l,&p);
    if (APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),3);
}
static void s_D3DXMatrixPerspectiveFovLH(void){ d3dx9_matrix m; d3dx9_matrix_perspective_fov_lh(&m,farg(1),farg(2),farg(3),farg(4)); if(APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),5); }
static void s_D3DXMatrixPerspectiveFovRH(void){ d3dx9_matrix m; d3dx9_matrix_perspective_fov_rh(&m,farg(1),farg(2),farg(3),farg(4)); if(APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),5); }
static void s_D3DXMatrixPerspectiveOffCenterLH(void){ d3dx9_matrix m; d3dx9_matrix_perspective_off_center_lh(&m,farg(1),farg(2),farg(3),farg(4),farg(5),farg(6)); if(APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),7); }
static void s_D3DXMatrixPerspectiveOffCenterRH(void){ d3dx9_matrix m; d3dx9_matrix_perspective_off_center_rh(&m,farg(1),farg(2),farg(3),farg(4),farg(5),farg(6)); if(APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),7); }
static void s_D3DXMatrixOrthoOffCenterLH(void){ d3dx9_matrix m; d3dx9_matrix_ortho_off_center_lh(&m,farg(1),farg(2),farg(3),farg(4),farg(5),farg(6)); if(APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),7); }
static void s_D3DXMatrixOrthoOffCenterRH(void){ d3dx9_matrix m; d3dx9_matrix_ortho_off_center_rh(&m,farg(1),farg(2),farg(3),farg(4),farg(5),farg(6)); if(APTR(0)) memcpy(APTR(0),&m,sizeof m); RET(A32(0),7); }

#define VEC2_BIN_SHIM(name,fn) static void name(void){ d3dx9_vec2 a,b; if(APTR(1)) memcpy(&a,APTR(1),sizeof a); if(APTR(2)) memcpy(&b,APTR(2),sizeof b); d3dx9_vec2 o; fn(&o,&a,&b); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),3); }
#define VEC3_BIN_SHIM(name,fn) static void name(void){ d3dx9_vec3 a,b; if(APTR(1)) memcpy(&a,APTR(1),sizeof a); if(APTR(2)) memcpy(&b,APTR(2),sizeof b); d3dx9_vec3 o; fn(&o,&a,&b); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),3); }
#define VEC4_BIN_SHIM(name,fn) static void name(void){ d3dx9_vec4 a,b; if(APTR(1)) memcpy(&a,APTR(1),sizeof a); if(APTR(2)) memcpy(&b,APTR(2),sizeof b); d3dx9_vec4 o; fn(&o,&a,&b); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),3); }
VEC2_BIN_SHIM(s_D3DXVec2Add,d3dx9_vec2_add)
VEC2_BIN_SHIM(s_D3DXVec2Subtract,d3dx9_vec2_subtract)
VEC2_BIN_SHIM(s_D3DXVec2Minimize,d3dx9_vec2_minimize)
VEC2_BIN_SHIM(s_D3DXVec2Maximize,d3dx9_vec2_maximize)
VEC3_BIN_SHIM(s_D3DXVec3Add,d3dx9_vec3_add)
VEC3_BIN_SHIM(s_D3DXVec3Subtract,d3dx9_vec3_subtract)
VEC3_BIN_SHIM(s_D3DXVec3Minimize,d3dx9_vec3_minimize)
VEC3_BIN_SHIM(s_D3DXVec3Maximize,d3dx9_vec3_maximize)
VEC4_BIN_SHIM(s_D3DXVec4Add,d3dx9_vec4_add)
VEC4_BIN_SHIM(s_D3DXVec4Subtract,d3dx9_vec4_subtract)
VEC4_BIN_SHIM(s_D3DXVec4Minimize,d3dx9_vec4_minimize)
VEC4_BIN_SHIM(s_D3DXVec4Maximize,d3dx9_vec4_maximize)
static void s_D3DXVec2Scale(void){ d3dx9_vec2 v; if(APTR(1)) memcpy(&v,APTR(1),sizeof v); d3dx9_vec2 o; d3dx9_vec2_scale(&o,&v,farg(2)); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),3); }
static void s_D3DXVec3Scale(void){ d3dx9_vec3 v; if(APTR(1)) memcpy(&v,APTR(1),sizeof v); d3dx9_vec3 o; d3dx9_vec3_scale(&o,&v,farg(2)); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),3); }
static void s_D3DXVec4Scale(void){ d3dx9_vec4 v; if(APTR(1)) memcpy(&v,APTR(1),sizeof v); d3dx9_vec4 o; d3dx9_vec4_scale(&o,&v,farg(2)); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),3); }
static void s_D3DXVec2Lerp(void){ d3dx9_vec2 a,b; if(APTR(1)) memcpy(&a,APTR(1),sizeof a); if(APTR(2)) memcpy(&b,APTR(2),sizeof b); d3dx9_vec2 o; d3dx9_vec2_lerp(&o,&a,&b,farg(3)); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),4); }
static void s_D3DXVec3Lerp(void){ d3dx9_vec3 a,b; if(APTR(1)) memcpy(&a,APTR(1),sizeof a); if(APTR(2)) memcpy(&b,APTR(2),sizeof b); d3dx9_vec3 o; d3dx9_vec3_lerp(&o,&a,&b,farg(3)); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),4); }
static void s_D3DXVec4Lerp(void){ d3dx9_vec4 a,b; if(APTR(1)) memcpy(&a,APTR(1),sizeof a); if(APTR(2)) memcpy(&b,APTR(2),sizeof b); d3dx9_vec4 o; d3dx9_vec4_lerp(&o,&a,&b,farg(3)); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),4); }
static void s_D3DXVec2Length(void){ d3dx9_vec2 v; if(APTR(0)) memcpy(&v,APTR(0),sizeof v); float f=d3dx9_vec2_length(&v); uint32_t b; memcpy(&b,&f,4); RET(b,1); }
static void s_D3DXVec2LengthSq(void){ d3dx9_vec2 v; if(APTR(0)) memcpy(&v,APTR(0),sizeof v); float f=d3dx9_vec2_length_sq(&v); uint32_t b; memcpy(&b,&f,4); RET(b,1); }
static void s_D3DXVec2Dot(void){ d3dx9_vec2 a,b; if(APTR(0)) memcpy(&a,APTR(0),sizeof a); if(APTR(1)) memcpy(&b,APTR(1),sizeof b); float f=d3dx9_vec2_dot(&a,&b); uint32_t u; memcpy(&u,&f,4); RET(u,2); }
static void s_D3DXVec2CCW(void){ d3dx9_vec2 a,b; if(APTR(0)) memcpy(&a,APTR(0),sizeof a); if(APTR(1)) memcpy(&b,APTR(1),sizeof b); float f=d3dx9_vec2_ccw(&a,&b); uint32_t u; memcpy(&u,&f,4); RET(u,2); }
static void s_D3DXVec3Length(void){ d3dx9_vec3 v; if(APTR(0)) memcpy(&v,APTR(0),sizeof v); float f=d3dx9_vec3_length(&v); uint32_t b; memcpy(&b,&f,4); RET(b,1); }
static void s_D3DXVec3LengthSq(void){ d3dx9_vec3 v; if(APTR(0)) memcpy(&v,APTR(0),sizeof v); float f=d3dx9_vec3_length_sq(&v); uint32_t b; memcpy(&b,&f,4); RET(b,1); }
static void s_D3DXVec3Dot(void){ d3dx9_vec3 a,b; if(APTR(0)) memcpy(&a,APTR(0),sizeof a); if(APTR(1)) memcpy(&b,APTR(1),sizeof b); float f=d3dx9_vec3_dot(&a,&b); uint32_t u; memcpy(&u,&f,4); RET(u,2); }
static void s_D3DXVec3Cross(void){ d3dx9_vec3 a,b,o; if(APTR(1)) memcpy(&a,APTR(1),sizeof a); if(APTR(2)) memcpy(&b,APTR(2),sizeof b); d3dx9_vec3_cross(&o,&a,&b); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),3); }
static void s_D3DXVec4Length(void){ d3dx9_vec4 v; if(APTR(0)) memcpy(&v,APTR(0),sizeof v); float f=d3dx9_vec4_length(&v); uint32_t b; memcpy(&b,&f,4); RET(b,1); }
static void s_D3DXVec4LengthSq(void){ d3dx9_vec4 v; if(APTR(0)) memcpy(&v,APTR(0),sizeof v); float f=d3dx9_vec4_length_sq(&v); uint32_t b; memcpy(&b,&f,4); RET(b,1); }
static void s_D3DXVec4Dot(void){ d3dx9_vec4 a,b; if(APTR(0)) memcpy(&a,APTR(0),sizeof a); if(APTR(1)) memcpy(&b,APTR(1),sizeof b); float f=d3dx9_vec4_dot(&a,&b); uint32_t u; memcpy(&u,&f,4); RET(u,2); }

static void s_D3DXVec2TransformCoordArray(void){ /* (pOut,OutStride,pV,Stride,pM,n) */
    if(!APTR(0)||!APTR(2)||!APTR(4)){ fprintf(stderr,"[d3dx9_math] Vec2TransformCoordArray: NULL\n"); RET(0,6); }
    d3dx9_matrix m; memcpy(&m,APTR(4),sizeof m);
    d3dx9_vec2_transform_coord_array((d3dx9_vec2*)APTR(0),A32(1),(const d3dx9_vec2*)APTR(2),A32(3),&m,A32(5));
    RET(A32(0),6);
}
static void s_D3DXVec2TransformNormalArray(void){
    if(!APTR(0)||!APTR(2)||!APTR(4)){ fprintf(stderr,"[d3dx9_math] Vec2TransformNormalArray: NULL\n"); RET(0,6); }
    d3dx9_matrix m; memcpy(&m,APTR(4),sizeof m);
    d3dx9_vec2_transform_normal_array((d3dx9_vec2*)APTR(0),A32(1),(const d3dx9_vec2*)APTR(2),A32(3),&m,A32(5));
    RET(A32(0),6);
}
static void s_D3DXVec3TransformCoordArray(void){
    if(!APTR(0)||!APTR(2)||!APTR(4)){ fprintf(stderr,"[d3dx9_math] Vec3TransformCoordArray: NULL\n"); RET(0,6); }
    d3dx9_matrix m; memcpy(&m,APTR(4),sizeof m);
    d3dx9_vec3_transform_coord_array((d3dx9_vec3*)APTR(0),A32(1),(const d3dx9_vec3*)APTR(2),A32(3),&m,A32(5));
    RET(A32(0),6);
}
static void s_D3DXVec3TransformNormalArray(void){
    if(!APTR(0)||!APTR(2)||!APTR(4)){ fprintf(stderr,"[d3dx9_math] Vec3TransformNormalArray: NULL\n"); RET(0,6); }
    d3dx9_matrix m; memcpy(&m,APTR(4),sizeof m);
    d3dx9_vec3_transform_normal_array((d3dx9_vec3*)APTR(0),A32(1),(const d3dx9_vec3*)APTR(2),A32(3),&m,A32(5));
    RET(A32(0),6);
}
static void s_D3DXVec4TransformArray(void){
    if(!APTR(0)||!APTR(2)||!APTR(4)){ fprintf(stderr,"[d3dx9_math] Vec4TransformArray: NULL\n"); RET(0,6); }
    d3dx9_matrix m; memcpy(&m,APTR(4),sizeof m);
    d3dx9_vec4_transform_array((d3dx9_vec4*)APTR(0),A32(1),(const d3dx9_vec4*)APTR(2),A32(3),&m,A32(5));
    RET(A32(0),6);
}

static void s_D3DXQuaternionInverse(void){ d3dx9_quat q; if(APTR(1)) memcpy(&q,APTR(1),sizeof q); d3dx9_quat o; d3dx9_quat *r=d3dx9_quat_inverse(&o,&q); if(!r){ RET(0,2); } if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),2); }
static void s_D3DXQuaternionLength(void){ d3dx9_quat q; if(APTR(0)) memcpy(&q,APTR(0),sizeof q); float f=d3dx9_quat_length(&q); uint32_t b; memcpy(&b,&f,4); RET(b,1); }
static void s_D3DXQuaternionLengthSq(void){ d3dx9_quat q; if(APTR(0)) memcpy(&q,APTR(0),sizeof q); float f=d3dx9_quat_length_sq(&q); uint32_t b; memcpy(&b,&f,4); RET(b,1); }
static void s_D3DXQuaternionDot(void){ d3dx9_quat a,b; if(APTR(0)) memcpy(&a,APTR(0),sizeof a); if(APTR(1)) memcpy(&b,APTR(1),sizeof b); float f=d3dx9_quat_dot(&a,&b); uint32_t u; memcpy(&u,&f,4); RET(u,2); }
static void s_D3DXQuaternionRotationMatrix(void){ d3dx9_matrix m; if(APTR(1)) memcpy(&m,APTR(1),sizeof m); d3dx9_quat q; d3dx9_quat_rotation_matrix(&q,&m); if(APTR(0)) memcpy(APTR(0),&q,sizeof q); RET(A32(0),2); }
static void s_D3DXQuaternionRotationYawPitchRoll(void){ d3dx9_quat q; d3dx9_quat_rotation_yaw_pitch_roll(&q,farg(1),farg(2),farg(3)); if(APTR(0)) memcpy(APTR(0),&q,sizeof q); RET(A32(0),4); }
static void s_D3DXQuaternionLn(void){ d3dx9_quat q; if(APTR(1)) memcpy(&q,APTR(1),sizeof q); d3dx9_quat o; d3dx9_quat_ln(&o,&q); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),2); }
static void s_D3DXQuaternionExp(void){ d3dx9_quat q; if(APTR(1)) memcpy(&q,APTR(1),sizeof q); d3dx9_quat o; d3dx9_quat_exp(&o,&q); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),2); }

static void s_D3DXPlaneDot(void){ d3dx9_plane p; d3dx9_vec4 v; if(APTR(0)) memcpy(&p,APTR(0),sizeof p); if(APTR(1)) memcpy(&v,APTR(1),sizeof v); float f=d3dx9_plane_dot(&p,&v); uint32_t b; memcpy(&b,&f,4); RET(b,2); }
static void s_D3DXPlaneScale(void){ d3dx9_plane p; if(APTR(1)) memcpy(&p,APTR(1),sizeof p); d3dx9_plane o; d3dx9_plane_scale(&o,&p,farg(2)); if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),3); }

static void s_D3DXPlaneTransform(void){ d3dx9_plane p; d3dx9_matrix m; if(APTR(1)) memcpy(&p,APTR(1),sizeof p); if(APTR(2)) memcpy(&m,APTR(2),sizeof m); d3dx9_plane o; d3dx9_plane *r=d3dx9_plane_transform(&o,&p,&m); if(!r){ RET(0,3); } if(APTR(0)) memcpy(APTR(0),&o,sizeof o); RET(A32(0),3); }

/* ----------------------------------------------------- Spherical Harmonics */

static void s_D3DXSHEvalDirection(void){ /* (pOut,Order,pDir) */
    uint32_t pOut=A32(0), order=A32(1), pDir=A32(2);
    d3dx9_vec3 dir={0,0,0};
    if(!pOut || !pDir || order<2 || order>6){ fprintf(stderr,"[d3dx9_sh] SHEvalDirection: invalid args\n"); RET(0,3); }
    memcpy(&dir,hle_ptr(pDir),sizeof dir);
    float *r = d3dx9_sh_eval_direction((float*)hle_ptr(pOut),order,&dir);
    RET(r? pOut:0,3);
}
static void s_D3DXSHRotateZ(void){ /* (pOut,Order,Angle,pIn) */
    uint32_t pOut=A32(0), order=A32(1); float angle=farg(2); uint32_t pIn=A32(3);
    if(!pOut || !pIn || order<1 || order>6){ fprintf(stderr,"[d3dx9_sh] SHRotateZ: invalid args\n"); RET(0,4); }
    float *r = d3dx9_sh_rotate_z((float*)hle_ptr(pOut),order,angle,(const float*)hle_ptr(pIn));
    RET(r? pOut:0,4);
}
static void s_D3DXSHAdd(void){ /* (pOut,Order,pA,pB) */
    uint32_t pOut=A32(0), order=A32(1), pA=A32(2), pB=A32(3);
    if(!pOut || !pA || !pB || order<1 || order>6){ fprintf(stderr,"[d3dx9_sh] SHAdd: invalid args\n"); RET(0,4); }
    float *r = d3dx9_sh_add((float*)hle_ptr(pOut),order,(const float*)hle_ptr(pA),(const float*)hle_ptr(pB));
    RET(r? pOut:0,4);
}
static void s_D3DXSHScale(void){ /* (pOut,Order,pIn,Scale) */
    uint32_t pOut=A32(0), order=A32(1), pIn=A32(2); float scale=farg(3);
    if(!pOut || !pIn || order<1 || order>6){ fprintf(stderr,"[d3dx9_sh] SHScale: invalid args\n"); RET(0,4); }
    float *r = d3dx9_sh_scale((float*)hle_ptr(pOut),order,(const float*)hle_ptr(pIn),scale);
    RET(r? pOut:0,4);
}
static void s_D3DXSHDot(void){ /* (Order,pA,pB) -> float */
    uint32_t order=A32(0), pA=A32(1), pB=A32(2);
    if(!pA || !pB || order<1 || order>6){ fprintf(stderr,"[d3dx9_sh] SHDot: invalid args\n"); uint32_t b=0; RET(b,3); }
    float f = d3dx9_sh_dot(order,(const float*)hle_ptr(pA),(const float*)hle_ptr(pB));
    uint32_t b; memcpy(&b,&f,4); RET(b,3);
}

/* ---------------------------------------------------------------- textures */

static void s_D3DXCreateTexture(void) {
    /* (pDevice, Width, Height, MipLevels, Usage, Format, Pool, ppTexture) */
    uint32_t tex = 0;
    uint32_t hr = d3dx9_texture_create(A32(0), A32(1), A32(2), A32(3), A32(4),
                                       A32(5), A32(6), &tex);
    uint32_t pp = A32(7);
    if (pp) MEM32(pp) = tex;
    RET(hr, 8);
}
static void s_D3DXCreateTextureFromFileA(void) {
    /* (pDevice, pSrcFile, ppTexture) */
    uint32_t tex = 0;
    uint32_t hr = d3dx9_texture_create_from_file_a(A32(0), ASTR(1), &tex, NULL);
    uint32_t pp = A32(2);
    if (pp) MEM32(pp) = tex;
    RET(hr, 3);
}
static void s_D3DXCreateTextureFromFileInMemory(void) {
    /* (pDevice, pSrcData, SrcDataSize, ppTexture) */
    uint32_t tex = 0;
    uint32_t hr = d3dx9_texture_create_from_file_memory(A32(0), APTR(1), A32(2),
                                                       D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT,
                                                       0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
                                                       &tex, NULL);
    uint32_t pp = A32(3);
    if (pp) MEM32(pp) = tex;
    RET(hr, 4);
}
static void s_D3DXCreateTextureFromFileInMemoryEx(void) {
    /* (pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage,
     *  Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture)
     * Filter/MipFilter are hint parameters: the HLE does not resample, so they
     * have no effect on the output (documented and tested as no-ops).
     * ColorKey != 0 requests color-keying, which is not implemented -> error. */
    if (A32(11) != 0) {
        uint32_t pp = A32(14);
        if (pp) MEM32(pp) = 0;
        RET(D3DERR_NOTAVAILABLE, 15);
    }
    uint32_t tex = 0;
    uint32_t hr = d3dx9_texture_create_from_file_memory(A32(0), APTR(1), A32(2),
                                                       A32(3), A32(4), A32(5), A32(6),
                                                       A32(7), A32(8), &tex, hle_ptr(A32(12)));
    uint32_t pp = A32(14);
    if (pp) MEM32(pp) = tex;
    RET(hr, 15);
}
static void s_D3DXCreateTextureFromFileExA(void) {
    /* (pDevice, pSrcFile, Width, Height, MipLevels, Usage, Format, Pool,
     *  Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture) */
    uint32_t tex = 0;
    uint32_t hr = d3dx9_texture_create_from_file_ex_a(A32(0), ASTR(1), A32(2), A32(3), A32(4),
                                                      A32(5), A32(6), A32(7), A32(8), A32(9),
                                                      A32(10), hle_ptr(A32(11)), hle_ptr(A32(12)), &tex);
    uint32_t pp = A32(13);
    if (pp) MEM32(pp) = tex;
    RET(hr, 14);
}
static void s_D3DXCreateTextureFromFileExW(void) {
    /* (pDevice, pSrcFile, Width, Height, MipLevels, Usage, Format, Pool,
     *  Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture) */
    uint32_t tex = 0;
    uint32_t hr = d3dx9_texture_create_from_file_ex_w(A32(0), (const uint16_t *)APTR(1),
                                                      A32(2), A32(3), A32(4), A32(5), A32(6), A32(7),
                                                      A32(8), A32(9), A32(10),
                                                      hle_ptr(A32(11)), hle_ptr(A32(12)), &tex);
    uint32_t pp = A32(13);
    if (pp) MEM32(pp) = tex;
    RET(hr, 14);
}
static void s_D3DXCreateCubeTextureFromFileA(void) {
    /* (pDevice, pSrcFile, ppCubeTexture) */
    uint32_t tex = 0;
    uint32_t hr = d3dx9_create_cube_texture_from_file_a(A32(0), ASTR(1), &tex);
    uint32_t pp = A32(2);
    if (pp) MEM32(pp) = tex;
    RET(hr, 3);
}
static void s_D3DXCreateCubeTextureFromFileW(void) {
    /* (pDevice, pSrcFile, ppCubeTexture) */
    uint32_t tex = 0;
    uint32_t hr = d3dx9_create_cube_texture_from_file_w(A32(0), (const uint16_t *)APTR(1), &tex);
    uint32_t pp = A32(2);
    if (pp) MEM32(pp) = tex;
    RET(hr, 3);
}
static void s_D3DXSaveTextureToFileA(void) {
    /* (pDestFile, DestFormat, pSrcTexture, pSrcPalette) */
    RET(d3dx9_save_texture_to_file_a(ASTR(0), A32(1), A32(2), hle_ptr(A32(3))), 4);
}
static void s_D3DXGetImageInfoFromFileInMemory(void) {
    RET(d3dx9_texture_get_image_info(APTR(0), A32(1), hle_ptr(A32(2))), 3);
}
static void s_D3DXGetImageInfoFromFileA(void) {
    /* (pSrcFile, pSrcInfo) */
    RET(d3dx9_texture_get_image_info_from_file_a(ASTR(0), hle_ptr(A32(1))), 2);
}
static void s_D3DXGetImageInfoFromFileW(void) {
    /* (pSrcFile, pSrcInfo) */
    RET(d3dx9_texture_get_image_info_from_file_w((const uint16_t *)APTR(0), hle_ptr(A32(1))), 2);
}
static void s_D3DXCheckTextureRequirements(void) {
    RET(d3dx9_texture_check_requirements(A32(0), hle_ptr(A32(1)), hle_ptr(A32(2)),
                                         hle_ptr(A32(3)), A32(4), hle_ptr(A32(5)),
                                         A32(6)), 7);
}
static void s_D3DXCreateCubeTexture(void) {
    /* (pDevice, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture) */
    uint32_t tex = 0;
    uint32_t hr = d3dx9_create_cube_texture(A32(0), A32(1), A32(2), A32(3), A32(4), A32(5), &tex);
    uint32_t pp = A32(6); if (pp) MEM32(pp) = tex; else if (!hr) hr = D3DERR_INVALIDCALL;
    RET(hr, 7);
}
static void s_D3DXCreateVolumeTexture(void) {
    /* (pDevice, Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture) */
    uint32_t tex = 0;
    uint32_t hr = d3dx9_create_volume_texture(A32(0),A32(1),A32(2),A32(3),A32(4),A32(5),A32(6),A32(7),&tex);
    uint32_t pp = A32(8); if (pp) MEM32(pp) = tex; else if (!hr) hr = D3DERR_INVALIDCALL;
    RET(hr, 9);
}
static void s_D3DXFillTexture(void) {
    /* (pTexture, pFunction, pData) */
    uint32_t hr = d3dx9_fill_texture(A32(0), A32(1), A32(2));
    RET(hr, 3);
}
static void s_D3DXComputeNormalMap(void) {
    /* (pTexture, pSrcTexture, pSrcPalette, Flags, Channel, Amplitude) */
    uint32_t hr = d3dx9_compute_normal_map(A32(0), A32(1), A32(3), A32(4), farg(5));
    RET(hr, 6);
}

const win32hle_shim win32hle_d3dx9[] = {
    { "D3DXMatrixIdentity",  s_D3DXMatrixIdentity },
    { "D3DXMatrixMultiply",  s_D3DXMatrixMultiply },
    { "D3DXMatrixInverse",   s_D3DXMatrixInverse },
    { "D3DXMatrixTranspose", s_D3DXMatrixTranspose },
    { "D3DXMatrixScaling",   s_D3DXMatrixScaling },
    { "D3DXMatrixTranslation", s_D3DXMatrixTranslation },
    { "D3DXMatrixRotationX", s_D3DXMatrixRotationX },
    { "D3DXMatrixRotationY", s_D3DXMatrixRotationY },
    { "D3DXMatrixRotationZ", s_D3DXMatrixRotationZ },
    { "D3DXVec2Normalize",       s_D3DXVec2Normalize },
    { "D3DXVec2Transform",       s_D3DXVec2Transform },
    { "D3DXVec2TransformCoord",  s_D3DXVec2TransformCoord },
    { "D3DXVec2TransformNormal", s_D3DXVec2TransformNormal },
    { "D3DXVec3Normalize",       s_D3DXVec3Normalize },
    { "D3DXVec3Transform",       s_D3DXVec3Transform },
    { "D3DXVec3TransformCoord",  s_D3DXVec3TransformCoord },
    { "D3DXVec3TransformNormal", s_D3DXVec3TransformNormal },
    { "D3DXVec4Normalize",       s_D3DXVec4Normalize },
    { "D3DXVec4Transform",       s_D3DXVec4Transform },
    { "D3DXVec4Cross",           s_D3DXVec4Cross },
    { "D3DXMatrixLookAtLH",  s_D3DXMatrixLookAtLH },
    { "D3DXMatrixLookAtRH",  s_D3DXMatrixLookAtRH },
    { "D3DXMatrixPerspectiveLH", s_D3DXMatrixPerspectiveLH },
    { "D3DXMatrixPerspectiveRH", s_D3DXMatrixPerspectiveRH },
    { "D3DXMatrixOrthoLH",   s_D3DXMatrixOrthoLH },
    { "D3DXMatrixOrthoRH",   s_D3DXMatrixOrthoRH },
    { "D3DXQuaternionIdentity",     s_D3DXQuaternionIdentity },
    { "D3DXQuaternionMultiply",     s_D3DXQuaternionMultiply },
    { "D3DXQuaternionNormalize",    s_D3DXQuaternionNormalize },
    { "D3DXQuaternionConjugate",    s_D3DXQuaternionConjugate },
    { "D3DXQuaternionRotationAxis", s_D3DXQuaternionRotationAxis },
    { "D3DXQuaternionToAxisAngle",  s_D3DXQuaternionToAxisAngle },
    { "D3DXQuaternionSlerp",        s_D3DXQuaternionSlerp },
    { "D3DXPlaneFromPointNormal", s_D3DXPlaneFromPointNormal },
    { "D3DXPlaneFromPoints",      s_D3DXPlaneFromPoints },
    { "D3DXPlaneNormalize",       s_D3DXPlaneNormalize },
    { "D3DXPlaneDotCoord",        s_D3DXPlaneDotCoord },
    { "D3DXPlaneDotNormal",       s_D3DXPlaneDotNormal },
    { "D3DXColorAdd",              s_D3DXColorAdd },
    { "D3DXColorSubtract",         s_D3DXColorSubtract },
    { "D3DXColorModulate",         s_D3DXColorModulate },
    { "D3DXColorScale",            s_D3DXColorScale },
    { "D3DXColorNegative",         s_D3DXColorNegative },
    { "D3DXColorLerp",             s_D3DXColorLerp },
    { "D3DXColorAdjustContrast",   s_D3DXColorAdjustContrast },
    { "D3DXColorAdjustSaturation", s_D3DXColorAdjustSaturation },
    { "D3DXMatrixDeterminant", s_D3DXMatrixDeterminant },
    { "D3DXMatrixRotationAxis", s_D3DXMatrixRotationAxis },
    { "D3DXMatrixRotationYawPitchRoll", s_D3DXMatrixRotationYawPitchRoll },
    { "D3DXMatrixRotationQuaternion", s_D3DXMatrixRotationQuaternion },
    { "D3DXMatrixReflect", s_D3DXMatrixReflect },
    { "D3DXMatrixShadow", s_D3DXMatrixShadow },
    { "D3DXMatrixPerspectiveFovLH", s_D3DXMatrixPerspectiveFovLH },
    { "D3DXMatrixPerspectiveFovRH", s_D3DXMatrixPerspectiveFovRH },
    { "D3DXMatrixPerspectiveOffCenterLH", s_D3DXMatrixPerspectiveOffCenterLH },
    { "D3DXMatrixPerspectiveOffCenterRH", s_D3DXMatrixPerspectiveOffCenterRH },
    { "D3DXMatrixOrthoOffCenterLH", s_D3DXMatrixOrthoOffCenterLH },
    { "D3DXMatrixOrthoOffCenterRH", s_D3DXMatrixOrthoOffCenterRH },
    { "D3DXVec2Length", s_D3DXVec2Length },
    { "D3DXVec2LengthSq", s_D3DXVec2LengthSq },
    { "D3DXVec2Dot", s_D3DXVec2Dot },
    { "D3DXVec2CCW", s_D3DXVec2CCW },
    { "D3DXVec2Add", s_D3DXVec2Add },
    { "D3DXVec2Subtract", s_D3DXVec2Subtract },
    { "D3DXVec2Minimize", s_D3DXVec2Minimize },
    { "D3DXVec2Maximize", s_D3DXVec2Maximize },
    { "D3DXVec2Scale", s_D3DXVec2Scale },
    { "D3DXVec2Lerp", s_D3DXVec2Lerp },
    { "D3DXVec2TransformCoordArray", s_D3DXVec2TransformCoordArray },
    { "D3DXVec2TransformNormalArray", s_D3DXVec2TransformNormalArray },
    { "D3DXVec3Length", s_D3DXVec3Length },
    { "D3DXVec3LengthSq", s_D3DXVec3LengthSq },
    { "D3DXVec3Dot", s_D3DXVec3Dot },
    { "D3DXVec3Cross", s_D3DXVec3Cross },
    { "D3DXVec3Add", s_D3DXVec3Add },
    { "D3DXVec3Subtract", s_D3DXVec3Subtract },
    { "D3DXVec3Minimize", s_D3DXVec3Minimize },
    { "D3DXVec3Maximize", s_D3DXVec3Maximize },
    { "D3DXVec3Scale", s_D3DXVec3Scale },
    { "D3DXVec3Lerp", s_D3DXVec3Lerp },
    { "D3DXVec3TransformCoordArray", s_D3DXVec3TransformCoordArray },
    { "D3DXVec3TransformNormalArray", s_D3DXVec3TransformNormalArray },
    { "D3DXVec4Length", s_D3DXVec4Length },
    { "D3DXVec4LengthSq", s_D3DXVec4LengthSq },
    { "D3DXVec4Dot", s_D3DXVec4Dot },
    { "D3DXVec4Add", s_D3DXVec4Add },
    { "D3DXVec4Subtract", s_D3DXVec4Subtract },
    { "D3DXVec4Minimize", s_D3DXVec4Minimize },
    { "D3DXVec4Maximize", s_D3DXVec4Maximize },
    { "D3DXVec4Scale", s_D3DXVec4Scale },
    { "D3DXVec4Lerp", s_D3DXVec4Lerp },
    { "D3DXVec4TransformArray", s_D3DXVec4TransformArray },
    { "D3DXQuaternionLength", s_D3DXQuaternionLength },
    { "D3DXQuaternionLengthSq", s_D3DXQuaternionLengthSq },
    { "D3DXQuaternionDot", s_D3DXQuaternionDot },
    { "D3DXQuaternionInverse", s_D3DXQuaternionInverse },
    { "D3DXQuaternionRotationMatrix", s_D3DXQuaternionRotationMatrix },
    { "D3DXQuaternionRotationYawPitchRoll", s_D3DXQuaternionRotationYawPitchRoll },
    { "D3DXQuaternionLn", s_D3DXQuaternionLn },
    { "D3DXQuaternionExp", s_D3DXQuaternionExp },
    { "D3DXPlaneDot", s_D3DXPlaneDot },
    { "D3DXPlaneScale", s_D3DXPlaneScale },
    { "D3DXPlaneTransform", s_D3DXPlaneTransform },
    { "D3DXSHEvalDirection", s_D3DXSHEvalDirection },
    { "D3DXSHRotateZ", s_D3DXSHRotateZ },
    { "D3DXSHAdd", s_D3DXSHAdd },
    { "D3DXSHScale", s_D3DXSHScale },
    { "D3DXSHDot", s_D3DXSHDot },
    { "D3DXLoadSurfaceFromMemory", s_D3DXLoadSurfaceFromMemory },
    { "D3DXLoadSurfaceFromFileA",  s_D3DXLoadSurfaceFromFileA },
    { "D3DXLoadSurfaceFromFileInMemory", s_D3DXLoadSurfaceFromFileInMemory },
    { "D3DXLoadVolumeFromMemory",  s_D3DXLoadVolumeFromMemory },
    { "D3DXLoadSurfaceFromSurface", s_D3DXLoadSurfaceFromSurface },
    { "D3DXFilterTexture",        s_D3DXFilterTexture },
    { "D3DXSaveSurfaceToFileA",   s_D3DXSaveSurfaceToFileA },
    { "D3DXCreateSprite",         s_D3DXCreateSprite },
    { "D3DXAssembleShader",        s_D3DXAssembleShader },
    { "D3DXDisassembleShader",      s_D3DXDisassembleShader },
    { "D3DXGetVertexShaderProfile", s_D3DXGetVertexShaderProfile },
    { "D3DXGetPixelShaderProfile",  s_D3DXGetPixelShaderProfile },
    { "D3DXAssembleShaderFromFileA", s_D3DXAssembleShaderFromFileA },
    { "D3DXAssembleShaderFromFileW", s_D3DXAssembleShaderFromFileW },
    { "D3DXPreprocessShaderFromFileA", s_D3DXPreprocessShaderFromFileA },
    { "ID3DXBuffer::QueryInterface",  d3d_QueryInterface },
    { "ID3DXBuffer::AddRef",          hle_com_AddRef },
    { "ID3DXBuffer::Release",         db_Release },
    { "ID3DXBuffer::GetBufferPointer", db_GetBufferPointer },
    { "ID3DXBuffer::GetBufferSize",   db_GetBufferSize },
    { "D3DXGetShaderVersion",      s_D3DXGetShaderVersion },
    { "D3DXGetShaderSize",         s_D3DXGetShaderSize },
    { "D3DXFindShaderComment",     s_D3DXFindShaderComment },
    { "D3DXGetShaderConstantTable",s_D3DXGetShaderConstantTable },
    { "D3DXCreateMeshFVF",        s_D3DXCreateMeshFVF },
    { "D3DXCreateMesh",           s_D3DXCreateMesh },
    { "D3DXLoadMeshFromXA",       s_D3DXLoadMeshFromXA },
    { "D3DXLoadMeshFromXW",       s_D3DXLoadMeshFromXW },
    { "D3DXLoadMeshFromXInMemory", s_D3DXLoadMeshFromXInMemory },
    { "D3DXLoadMeshHierarchyFromXA",        s_D3DXLoadMeshHierarchyFromXA },
    { "D3DXLoadMeshHierarchyFromXW",        s_D3DXLoadMeshHierarchyFromXW },
    { "D3DXLoadMeshHierarchyFromXInMemory", s_D3DXLoadMeshHierarchyFromXInMemory },
    { "D3DXFrameDestroy",                   s_D3DXFrameDestroy },
    { "D3DXComputeNormals",       s_D3DXComputeNormals },
    { "D3DXWeldVertices",         s_D3DXWeldVertices },
    { "D3DXOptimizeFaces",        s_D3DXOptimizeFaces },
    { "D3DXOptimizeVertices",     s_D3DXOptimizeVertices },
    { "D3DXIntersect",            s_D3DXIntersect },
    { "D3DXIntersectTri",         s_D3DXIntersectTri },
    { "D3DXSphereBoundProbe",     s_D3DXSphereBoundProbe },
    { "D3DXBoxBoundProbe",        s_D3DXBoxBoundProbe },
    { "D3DXCreatePolygon",        s_D3DXCreatePolygon },
    { "D3DXCreateBox",            s_D3DXCreateBox },
    { "D3DXCreateSphere",         s_D3DXCreateSphere },
    { "D3DXCreateCylinder",       s_D3DXCreateCylinder },
    { "D3DXCreateTorus",          s_D3DXCreateTorus },
    { "D3DXComputeBoundingBox",   s_D3DXComputeBoundingBox },
    { "D3DXComputeBoundingSphere", s_D3DXComputeBoundingSphere },
    { "D3DXGetFVFVertexSize",     s_D3DXGetFVFVertexSize },
    { "D3DXDeclaratorFromFVF",    s_D3DXDeclaratorFromFVF },
    { "ID3DXMesh::QueryInterface", d3d_QueryInterface },
    { "ID3DXMesh::AddRef",         mesh_AddRef },
    { "ID3DXMesh::Release",        mesh_Release },
    { "ID3DXMesh::DrawSubset",     mesh_DrawSubset },
    { "ID3DXMesh::GetNumFaces",    mesh_GetNumFaces },
    { "ID3DXMesh::GetNumVertices", mesh_GetNumVertices },
    { "ID3DXMesh::GetFVF",         mesh_GetFVF },
    { "ID3DXMesh::GetDeclaration", mesh_GetDeclaration },
    { "ID3DXMesh::GetNumBytesPerVertex", mesh_GetNumBytesPerVertex },
    { "ID3DXMesh::GetOptions",     mesh_GetOptions },
    { "ID3DXMesh::GetDevice",      mesh_GetDevice },
    { "ID3DXMesh::CloneMeshFVF",   mesh_CloneMeshFVF },
    { "ID3DXMesh::CloneMesh",      mesh_CloneMesh },
    { "ID3DXMesh::GetVertexBuffer", mesh_GetVertexBuffer },
    { "ID3DXMesh::GetIndexBuffer",  mesh_GetIndexBuffer },
    { "ID3DXMesh::LockVertexBuffer", mesh_LockVertexBuffer },
    { "ID3DXMesh::UnlockVertexBuffer", mesh_UnlockVertexBuffer },
    { "ID3DXMesh::LockIndexBuffer", mesh_LockIndexBuffer },
    { "ID3DXMesh::UnlockIndexBuffer", mesh_UnlockIndexBuffer },
    { "ID3DXMesh::GetAttributeTable", mesh_GetAttributeTable },
    { "ID3DXMesh::ConvertPointRepsToAdjacency", mesh_ConvertPointRepsToAdjacency },
    { "ID3DXMesh::ConvertAdjacencyToPointReps", mesh_ConvertAdjacencyToPointReps },
    { "ID3DXMesh::GenerateAdjacency", mesh_GenerateAdjacency },
    { "ID3DXMesh::UpdateSemantics", mesh_UpdateSemantics },
    { "ID3DXMesh::LockAttributeBuffer", mesh_LockAttributeBuffer },
    { "ID3DXMesh::UnlockAttributeBuffer", mesh_UnlockAttributeBuffer },
    { "ID3DXMesh::Optimize",       mesh_Optimize },
    { "ID3DXMesh::OptimizeInplace", mesh_OptimizeInplace },
    { "ID3DXMesh::SetAttributeTable", mesh_SetAttributeTable },
    { "D3DXCreateLine",           s_D3DXCreateLine },
    { "ID3DXLine::QueryInterface", d3d_QueryInterface },
    { "ID3DXLine::AddRef",         l_AddRef },
    { "ID3DXLine::Release",        l_Release },
    { "ID3DXLine::GetDevice",      l_GetDevice },
    { "ID3DXLine::Begin",          l_Begin },
    { "ID3DXLine::Draw",           l_Draw },
    { "ID3DXLine::DrawTransform",  l_DrawTransform },
    { "ID3DXLine::SetPattern",     l_SetPattern },
    { "ID3DXLine::GetPattern",     l_GetPattern },
    { "ID3DXLine::SetPatternScale", l_SetPatternScale },
    { "ID3DXLine::GetPatternScale", l_GetPatternScale },
    { "ID3DXLine::SetWidth",       l_SetWidth },
    { "ID3DXLine::GetWidth",       l_GetWidth },
    { "ID3DXLine::SetAntialias",   l_SetAntialias },
    { "ID3DXLine::GetAntialias",   l_GetAntialias },
    { "ID3DXLine::SetGLLines",     l_SetGLLines },
    { "ID3DXLine::GetGLLines",     l_GetGLLines },
    { "ID3DXLine::End",            l_End },
    { "ID3DXLine::OnLostDevice",   l_OnLostDevice },
    { "ID3DXLine::OnResetDevice",  l_OnResetDevice },
    { "D3DXCreateMatrixStack",    s_D3DXCreateMatrixStack },
    { "ID3DXMatrixStack::QueryInterface", d3d_QueryInterface },
    { "ID3DXMatrixStack::AddRef", ms_AddRef },
    { "ID3DXMatrixStack::Release", ms_Release },
    { "ID3DXMatrixStack::Push", ms_Push },
    { "ID3DXMatrixStack::Pop", ms_Pop },
    { "ID3DXMatrixStack::GetTop", ms_GetTop },
    { "ID3DXMatrixStack::LoadIdentity", ms_LoadIdentity },
    { "ID3DXMatrixStack::LoadMatrix", ms_LoadMatrix },
    { "ID3DXMatrixStack::MultMatrix", ms_MultMatrix },
    { "ID3DXMatrixStack::MultMatrixLocal", ms_MultMatrixLocal },
    { "ID3DXMatrixStack::RotateAxis", ms_RotateAxis },
    { "ID3DXMatrixStack::RotateAxisLocal", ms_RotateAxisLocal },
    { "ID3DXMatrixStack::RotateYawPitchRoll", ms_RotateYawPitchRoll },
    { "ID3DXMatrixStack::RotateYawPitchRollLocal", ms_RotateYawPitchRollLocal },
    { "ID3DXMatrixStack::Scale", ms_Scale },
    { "ID3DXMatrixStack::ScaleLocal", ms_ScaleLocal },
    { "ID3DXMatrixStack::Translate", ms_Translate },
    { "ID3DXMatrixStack::TranslateLocal", ms_TranslateLocal },
    { "D3DXCreateFontA",          s_D3DXCreateFontA },
    { "D3DXCreateFontW",          s_D3DXCreateFontW },
    { "D3DXCreateFontIndirectA",  s_D3DXCreateFontIndirectA },
    { "D3DXCreateFontIndirectW",  s_D3DXCreateFontIndirectW },
    { "ID3DXFont::QueryInterface",    d3d_QueryInterface },
    { "ID3DXFont::AddRef",            f_AddRef },
    { "ID3DXFont::Release",           f_Release },
    { "ID3DXFont::GetDevice",         f_GetDevice },
    { "ID3DXFont::GetDescA",          f_GetDescA },
    { "ID3DXFont::GetDescW",          f_GetDescW },
    { "ID3DXFont::GetTextMetricsA",   f_GetTextMetricsA },
    { "ID3DXFont::GetTextMetricsW",   f_GetTextMetricsW },
    { "ID3DXFont::GetDC",             f_GetDC },
    { "ID3DXFont::GetGlyphData",      f_GetGlyphData },
    { "ID3DXFont::PreloadCharacters", f_PreloadCharacters },
    { "ID3DXFont::PreloadGlyphs",     f_PreloadGlyphs },
    { "ID3DXFont::PreloadTextA",      f_PreloadTextA },
    { "ID3DXFont::PreloadTextW",      f_PreloadTextW },
    { "ID3DXFont::DrawTextA",         f_DrawTextA },
    { "ID3DXFont::DrawTextW",         f_DrawTextW },
    { "ID3DXFont::OnLostDevice",      f_OnLostDevice },
    { "ID3DXFont::OnResetDevice",     f_OnResetDevice },
    { "D3DXCreateTexture",        s_D3DXCreateTexture },
    { "D3DXCreateCubeTexture",    s_D3DXCreateCubeTexture },
    { "D3DXCreateVolumeTexture",  s_D3DXCreateVolumeTexture },
    { "D3DXCreateTextureFromFileA", s_D3DXCreateTextureFromFileA },
    { "D3DXCreateTextureFromFileExA", s_D3DXCreateTextureFromFileExA },
    { "D3DXCreateTextureFromFileExW", s_D3DXCreateTextureFromFileExW },
    { "D3DXCreateTextureFromFileInMemory", s_D3DXCreateTextureFromFileInMemory },
    { "D3DXCreateTextureFromFileInMemoryEx", s_D3DXCreateTextureFromFileInMemoryEx },
    { "D3DXCreateCubeTextureFromFileA", s_D3DXCreateCubeTextureFromFileA },
    { "D3DXCreateCubeTextureFromFileW", s_D3DXCreateCubeTextureFromFileW },
    { "D3DXGetImageInfoFromFileInMemory", s_D3DXGetImageInfoFromFileInMemory },
    { "D3DXGetImageInfoFromFileA", s_D3DXGetImageInfoFromFileA },
    { "D3DXGetImageInfoFromFileW", s_D3DXGetImageInfoFromFileW },
    { "D3DXSaveTextureToFileA", s_D3DXSaveTextureToFileA },
    { "D3DXCheckTextureRequirements", s_D3DXCheckTextureRequirements },
    { "D3DXCreateRenderToSurface", s_D3DXCreateRenderToSurface },
    /* D3DXCreateRenderToEnvMap: only the factory is public. The 13
     * ID3DXRenderToEnvMap method shims are registered lazily by
     * hle_com_vtable() on first object creation (see animation/skin note). */
    { "D3DXCreateRenderToEnvMap", s_D3DXCreateRenderToEnvMap },
    { "dxr_QueryInterface",       d3d_QueryInterface },
    { "dxr_AddRef",               dxr_AddRef },
    { "dxr_Release",              dxr_Release },
    { "dxr_GetDevice",            dxr_GetDevice },
    { "dxr_GetDesc",              dxr_GetDesc },
    { "dxr_BeginScene",           dxr_BeginScene },
    { "dxr_EndScene",             dxr_EndScene },
    { "dxr_OnLostDevice",         dxr_OnLostDevice },
    { "dxr_OnResetDevice",        dxr_OnResetDevice },
    { "D3DXCreateBuffer",             s_D3DXCreateBuffer },
    { "dxb_QueryInterface",           d3d_QueryInterface },
    { "dxb_AddRef",                   db_AddRef },
    { "dxb_Release",                  db_Release },
    { "dxb_GetBufferPointer",         db_GetBufferPointer },
    { "dxb_GetBufferSize",            db_GetBufferSize },
    { "D3DXFloat32To16Array",         s_D3DXFloat32To16Array },
    { "D3DXFloat16To32Array",         s_D3DXFloat16To32Array },
    { "D3DXFillTexture",          s_D3DXFillTexture },
    { "D3DXComputeNormalMap",     s_D3DXComputeNormalMap },

    /* D3DX9 animation factories. The ID3DXKeyframedAnimationSet (35) and
     * ID3DXAnimationController (42) method shims are registered by
     * hle_com_vtable() when the first object is created. They must NOT also
     * be listed here: recomp_host_init() registers this table and several
     * selftests register it again, and duplicating 77 shims pushes the fixed
     * MAX_SHIMS registry (2048) over its limit. */
    { "D3DXCreateKeyframedAnimationSet", s_D3DXCreateKeyframedAnimationSet },
    { "D3DXCreateAnimationController", s_D3DXCreateAnimationController },

    /* D3DXCreateSkinInfo/CreateSkinInfoFVF. The 27 ID3DXSkinInfo method
     * shims are registered lazily by hle_com_vtable() when the first object
     * is created, so they are not duplicated here (see the animation note). */
    { "D3DXCreateSkinInfo",   s_D3DXCreateSkinInfo },
    { "D3DXCreateSkinInfoFVF", s_D3DXCreateSkinInfoFVF },

    { "D3DXCheckVersion",              s_D3DXCheckVersion },
    { "D3DXDebugMute",                 s_D3DXDebugMute },
    { "D3DXCreateEffectFromFileExA",   s_D3DXCreateEffectFromFileExA },
    { "D3DXCreateEffectFromFileExW",   s_D3DXCreateEffectFromFileExW },
    { "D3DXCreateEffectFromFileW",     s_D3DXCreateEffectFromFileW },
    { 0, 0 }
};
