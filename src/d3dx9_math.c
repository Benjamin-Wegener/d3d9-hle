/*
 * d3dx9_math.c - D3DX9's math: matrices, vectors, quaternions, planes.
 *
 * D3DX is single-precision throughout: a D3DXMATRIX is float m[4][4] applied
 * to row vectors (v' = v * M). Every function that writes an out pointer also
 * accepts one of its inputs as that pointer: the result is built locally
 * first, then copied back.
 *
 * The behaviour is the documented D3DX one (the d3dx9math.h contracts and the
 * matrices the SDK header prints), implemented here directly.
 */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "d3dx9_math.h"

/* ---------------------------------------------------------------- matrices */

void d3dx9_matrix_identity(d3dx9_matrix *m)
{
    static const d3dx9_matrix I = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 },
                                      { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } };
    *m = I;
}

/* pOut = pA * pB (row vectors: v * (A * B) = (v * A) * B). */
void d3dx9_matrix_multiply(d3dx9_matrix *out, const d3dx9_matrix *a, const d3dx9_matrix *b)
{
    d3dx9_matrix t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t.m[i][j] = a->m[i][0] * b->m[0][j]
                      + a->m[i][1] * b->m[1][j]
                      + a->m[i][2] * b->m[2][j]
                      + a->m[i][3] * b->m[3][j];
    *out = t;
}

/* The inverse by Gauss-Jordan elimination with partial pivoting: 0 when the
 * matrix is singular (a zero pivot), as D3DX answers NULL. */
int d3dx9_matrix_inverse(d3dx9_matrix *out, const d3dx9_matrix *m)
{
    /* General case: Gaussian elimination */
    float a[4][8];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) a[i][j] = m->m[i][j];
        for (int j = 0; j < 4; j++) a[i][4 + j] = (i == j);
    }
    for (int col = 0; col < 4; col++) {
        int p = col;
        for (int i = col + 1; i < 4; i++)
            if (fabsf(a[i][col]) > fabsf(a[p][col])) p = i;
        if (a[p][col] == 0.0f) return 0;  /* singular */
        if (p != col)
            for (int j = 0; j < 8; j++) { float t = a[col][j]; a[col][j] = a[p][j]; a[p][j] = t; }
        float d = a[col][col];
        for (int j = 0; j < 8; j++) a[col][j] /= d;
        for (int i = 0; i < 4; i++) {
            if (i == col || a[i][col] == 0.0f) continue;
            float factor = a[i][col];
            for (int j = 0; j < 8; j++) a[i][j] -= factor * a[col][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out->m[i][j] = a[i][4 + j];
    return 1;
}

void d3dx9_matrix_transpose(d3dx9_matrix *out, const d3dx9_matrix *m)
{
    d3dx9_matrix t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t.m[i][j] = m->m[j][i];
    *out = t;
}

void d3dx9_matrix_scaling(d3dx9_matrix *m, float x, float y, float z)
{
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    t.m[0][0] = x, t.m[1][1] = y, t.m[2][2] = z;
    *m = t;
}

void d3dx9_matrix_translation(d3dx9_matrix *m, float x, float y, float z)
{
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    t.m[0][3] = x, t.m[1][3] = y, t.m[2][3] = z;
    *m = t;
}

/* The rotations about the fixed axes, as the SDK header prints them: the
 * angle is in radians, right-hand rule, row vectors. */
void d3dx9_matrix_rotation_x(d3dx9_matrix *m, float angle)
{
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    float c = cosf(angle), s = sinf(angle);
    t.m[1][1] = c,  t.m[1][2] = s;
    t.m[2][1] = -s, t.m[2][2] = c;
    *m = t;
}

void d3dx9_matrix_rotation_y(d3dx9_matrix *m, float angle)
{
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    float c = cosf(angle), s = sinf(angle);
    t.m[0][0] = c,  t.m[0][2] = -s;
    t.m[2][0] = s,  t.m[2][2] = c;
    *m = t;
}

void d3dx9_matrix_rotation_z(d3dx9_matrix *m, float angle)
{
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    float c = cosf(angle), s = sinf(angle);
    t.m[0][0] = c,  t.m[0][1] = s;
    t.m[1][0] = -s, t.m[1][1] = c;
    *m = t;
}

/* ---------------------------------------------------------------- vectors */

void d3dx9_vec2_normalize(d3dx9_vec2 *out, const d3dx9_vec2 *v)
{
    float len = sqrtf(v->x * v->x + v->y * v->y);
    if (len > 0.0f) {
        float inv = 1.0f / len;
        out->x = v->x * inv;
        out->y = v->y * inv;
    } else {
        out->x = 0.0f;
        out->y = 0.0f;
    }
}

void d3dx9_vec2_transform(d3dx9_vec4 *out, const d3dx9_vec2 *v, const d3dx9_matrix *m)
{
    d3dx9_vec4 t;
    t.x = v->x * m->m[0][0] + v->y * m->m[0][1] + m->m[0][3];
    t.y = v->x * m->m[1][0] + v->y * m->m[1][1] + m->m[1][3];
    t.z = v->x * m->m[2][0] + v->y * m->m[2][1] + m->m[2][3];
    t.w = v->x * m->m[3][0] + v->y * m->m[3][1] + m->m[3][3];
    *out = t;
}

void d3dx9_vec2_transform_coord(d3dx9_vec2 *out, const d3dx9_vec2 *v, const d3dx9_matrix *m)
{
    d3dx9_vec4 t;
    d3dx9_vec2_transform(&t, v, m);
    float inv_w = (t.w != 0.0f) ? (1.0f / t.w) : 1.0f;
    out->x = t.x * inv_w;
    out->y = t.y * inv_w;
}

void d3dx9_vec2_transform_normal(d3dx9_vec2 *out, const d3dx9_vec2 *v, const d3dx9_matrix *m)
{
    d3dx9_vec2 t;
    t.x = v->x * m->m[0][0] + v->y * m->m[0][1];
    t.y = v->x * m->m[1][0] + v->y * m->m[1][1];
    *out = t;
}

void d3dx9_vec3_normalize(d3dx9_vec3 *out, const d3dx9_vec3 *v)
{
    float len = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
    if (len > 0.0f) {
        float inv = 1.0f / len;
        out->x = v->x * inv;
        out->y = v->y * inv;
        out->z = v->z * inv;
    } else {
        out->x = 0.0f;
        out->y = 0.0f;
        out->z = 0.0f;
    }
}

void d3dx9_vec3_transform(d3dx9_vec4 *out, const d3dx9_vec3 *v, const d3dx9_matrix *m)
{
    d3dx9_vec4 t;
    t.x = v->x * m->m[0][0] + v->y * m->m[0][1] + v->z * m->m[0][2] + m->m[0][3];
    t.y = v->x * m->m[1][0] + v->y * m->m[1][1] + v->z * m->m[1][2] + m->m[1][3];
    t.z = v->x * m->m[2][0] + v->y * m->m[2][1] + v->z * m->m[2][2] + m->m[2][3];
    t.w = v->x * m->m[3][0] + v->y * m->m[3][1] + v->z * m->m[3][2] + m->m[3][3];
    *out = t;
}

void d3dx9_vec3_transform_coord(d3dx9_vec3 *out, const d3dx9_vec3 *v, const d3dx9_matrix *m)
{
    d3dx9_vec4 t;
    d3dx9_vec3_transform(&t, v, m);
    float inv_w = (t.w != 0.0f) ? (1.0f / t.w) : 1.0f;
    out->x = t.x * inv_w;
    out->y = t.y * inv_w;
    out->z = t.z * inv_w;
}

void d3dx9_vec3_transform_normal(d3dx9_vec3 *out, const d3dx9_vec3 *v, const d3dx9_matrix *m)
{
    d3dx9_vec3 t;
    t.x = v->x * m->m[0][0] + v->y * m->m[0][1] + v->z * m->m[0][2];
    t.y = v->x * m->m[1][0] + v->y * m->m[1][1] + v->z * m->m[1][2];
    t.z = v->x * m->m[2][0] + v->y * m->m[2][1] + v->z * m->m[2][2];
    *out = t;
}

void d3dx9_vec4_normalize(d3dx9_vec4 *out, const d3dx9_vec4 *v)
{
    float len = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z + v->w * v->w);
    if (len > 0.0f) {
        float inv = 1.0f / len;
        out->x = v->x * inv;
        out->y = v->y * inv;
        out->z = v->z * inv;
        out->w = v->w * inv;
    } else {
        out->x = 0.0f;
        out->y = 0.0f;
        out->z = 0.0f;
        out->w = 0.0f;
    }
}

void d3dx9_vec4_transform(d3dx9_vec4 *out, const d3dx9_vec4 *v, const d3dx9_matrix *m)
{
    d3dx9_vec4 t;
    t.x = v->x * m->m[0][0] + v->y * m->m[0][1] + v->z * m->m[0][2] + v->w * m->m[0][3];
    t.y = v->x * m->m[1][0] + v->y * m->m[1][1] + v->z * m->m[1][2] + v->w * m->m[1][3];
    t.z = v->x * m->m[2][0] + v->y * m->m[2][1] + v->z * m->m[2][2] + v->w * m->m[2][3];
    t.w = v->x * m->m[3][0] + v->y * m->m[3][1] + v->z * m->m[3][2] + v->w * m->m[3][3];
    *out = t;
}

void d3dx9_vec4_cross(d3dx9_vec4 *out, const d3dx9_vec4 *v1, const d3dx9_vec4 *v2, const d3dx9_vec4 *v3)
{
    d3dx9_vec4 t;
    t.x = v1->y * (v2->z * v3->w - v3->z * v2->w) - v1->z * (v2->y * v3->w - v3->y * v2->w) + v1->w * (v2->y * v3->z - v2->z * v3->y);
    t.y = -(v1->x * (v2->z * v3->w - v3->z * v2->w) - v1->z * (v2->x * v3->w - v3->x * v2->w) + v1->w * (v2->x * v3->z - v3->x * v2->z));
    t.z = v1->x * (v2->y * v3->w - v3->y * v2->w) - v1->y * (v2->x * v3->w - v3->x * v2->w) + v1->w * (v2->x * v3->y - v3->x * v2->y);
    t.w = -(v1->x * (v2->y * v3->z - v3->y * v2->z) - v1->y * (v2->x * v3->z - v3->x * v2->z) + v1->z * (v2->x * v3->y - v3->x * v2->y));
    *out = t;
}

/* ------------------------------------------------- view/projection matrices */

static d3dx9_vec3 v3_sub(const d3dx9_vec3 *a, const d3dx9_vec3 *b) {
    d3dx9_vec3 r = { a->x - b->x, a->y - b->y, a->z - b->z };
    return r;
}
static d3dx9_vec3 v3_cross(const d3dx9_vec3 *a, const d3dx9_vec3 *b) {
    d3dx9_vec3 r = { a->y * b->z - a->z * b->y,
                     a->z * b->x - a->x * b->z,
                     a->x * b->y - a->y * b->x };
    return r;
}
static float v3_dot(const d3dx9_vec3 *a, const d3dx9_vec3 *b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}
static d3dx9_vec3 v3_unit(const d3dx9_vec3 *v) {
    float len = sqrtf(v3_dot(v, v));
    d3dx9_vec3 r = { 0, 0, 0 };
    if (len > 0.0f) {
        float inv = 1.0f / len;
        r.x = v->x * inv; r.y = v->y * inv; r.z = v->z * inv;
    }
    return r;
}

/* Public D3DX docs: z = normal(At-Eye) (LH) / normal(Eye-At) (RH),
 * x = normal(cross(Up,z)), y = cross(z,x); the axes are the rows and the
 * translation is -dot(axis, Eye) in the last column. */
static void look_at(d3dx9_matrix *m, const d3dx9_vec3 *eye, const d3dx9_vec3 *at,
                    const d3dx9_vec3 *up, int rh) {
    d3dx9_vec3 dir = rh ? v3_sub(eye, at) : v3_sub(at, eye);
    d3dx9_vec3 z = v3_unit(&dir);
    d3dx9_vec3 cross_up_z = v3_cross(up, &z);
    d3dx9_vec3 x = v3_unit(&cross_up_z);
    d3dx9_vec3 y = v3_cross(&z, &x);
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    t.m[0][0] = x.x; t.m[0][1] = y.x; t.m[0][2] = z.x;
    t.m[1][0] = x.y; t.m[1][1] = y.y; t.m[1][2] = z.y;
    t.m[2][0] = x.z; t.m[2][1] = y.z; t.m[2][2] = z.z;
    t.m[3][0] = -v3_dot(&x, eye);
    t.m[3][1] = -v3_dot(&y, eye);
    t.m[3][2] = -v3_dot(&z, eye);
    *m = t;
}
void d3dx9_matrix_look_at_lh(d3dx9_matrix *m, const d3dx9_vec3 *e, const d3dx9_vec3 *a, const d3dx9_vec3 *u) {
    look_at(m, e, a, u, 0);
}
void d3dx9_matrix_look_at_rh(d3dx9_matrix *m, const d3dx9_vec3 *e, const d3dx9_vec3 *a, const d3dx9_vec3 *u) {
    look_at(m, e, a, u, 1);
}

void d3dx9_matrix_perspective_lh(d3dx9_matrix *m, float w, float h, float zn, float zf) {
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0] = 2.0f * zn / w;
    t.m[1][1] = 2.0f * zn / h;
    t.m[2][2] = zf / (zf - zn);
    t.m[2][3] = 1.0f;
    t.m[3][2] = zn * zn / (zn - zf);
    *m = t;
}
void d3dx9_matrix_perspective_rh(d3dx9_matrix *m, float w, float h, float zn, float zf) {
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0] = 2.0f * zn / w;
    t.m[1][1] = 2.0f * zn / h;
    t.m[2][2] = zf / (zn - zf);
    t.m[2][3] = -1.0f;
    t.m[3][2] = zn * zf / (zn - zf);
    *m = t;
}
void d3dx9_matrix_ortho_lh(d3dx9_matrix *m, float w, float h, float zn, float zf) {
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0] = 2.0f / w;
    t.m[1][1] = 2.0f / h;
    t.m[2][2] = 1.0f / (zf - zn);
    t.m[3][2] = zn / (zn - zf);
    t.m[3][3] = 1.0f;
    *m = t;
}
void d3dx9_matrix_ortho_rh(d3dx9_matrix *m, float w, float h, float zn, float zf) {
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0] = 2.0f / w;
    t.m[1][1] = 2.0f / h;
    t.m[2][2] = 1.0f / (zn - zf);
    t.m[3][2] = zn / (zn - zf);
    t.m[3][3] = 1.0f;
    *m = t;
}

/* --------------------------------------------------------------- quaternions */

void d3dx9_quat_identity(d3dx9_quat *q) {
    d3dx9_quat t = { 0, 0, 0, 1 };
    *q = t;
}
void d3dx9_quat_multiply(d3dx9_quat *out, const d3dx9_quat *a, const d3dx9_quat *b) {
    d3dx9_quat t;
    t.x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
    t.y = a->w * b->y - a->x * b->z + a->y * b->w + a->z * b->x;
    t.z = a->w * b->z + a->x * b->y - a->y * b->x + a->z * b->w;
    t.w = a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z;
    *out = t;
}
void d3dx9_quat_normalize(d3dx9_quat *out, const d3dx9_quat *q) {
    float n2 = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    if (n2 > 0.0f) {
        float inv = 1.0f / sqrtf(n2);
        d3dx9_quat t = { q->x * inv, q->y * inv, q->z * inv, q->w * inv };
        *out = t;
    } else {
        *out = *q;
    }
}
void d3dx9_quat_conjugate(d3dx9_quat *out, const d3dx9_quat *q) {
    d3dx9_quat t = { -q->x, -q->y, -q->z, q->w };
    *out = t;
}
void d3dx9_quat_rotation_axis(d3dx9_quat *out, const d3dx9_vec3 *axis, float angle) {
    d3dx9_vec3 a = v3_unit(axis);
    float half = angle * 0.5f;
    d3dx9_quat t = { a.x * sinf(half), a.y * sinf(half), a.z * sinf(half), cosf(half) };
    *out = t;
}
void d3dx9_quat_to_axis_angle(const d3dx9_quat *q, d3dx9_vec3 *axis, float *angle) {
    float w = q->w;
    if (w > 1.0f) w = 1.0f;
    if (w < -1.0f) w = -1.0f;
    *angle = 2.0f * acosf(w);
    float s = sqrtf(1.0f - w * w);
    d3dx9_vec3 a = { 1.0f, 0.0f, 0.0f };   /* public docs: identity axis */
    if (s > 1e-6f) {
        a.x = q->x / s; a.y = q->y / s; a.z = q->z / s;
    }
    *axis = a;
}
void d3dx9_quat_slerp(d3dx9_quat *out, const d3dx9_quat *q1, const d3dx9_quat *q2, float t) {
    d3dx9_quat a = *q1, b = *q2;
    float cosom = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cosom < 0.0f) {
        b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
        cosom = -cosom;
    }
    float s0, s1;
    if (cosom > 1.0f - 1e-6f) {
        s0 = 1.0f - t; s1 = t;
    } else {
        float omega = acosf(cosom), sinom = sinf(omega);
        s0 = sinf((1.0f - t) * omega) / sinom;
        s1 = sinf(t * omega) / sinom;
    }
    d3dx9_quat r = { s0 * a.x + s1 * b.x, s0 * a.y + s1 * b.y,
                     s0 * a.z + s1 * b.z, s0 * a.w + s1 * b.w };
    *out = r;
}

/* -------------------------------------------------------------------- planes */

void d3dx9_plane_from_point_normal(d3dx9_plane *p, const d3dx9_vec3 *point, const d3dx9_vec3 *normal) {
    d3dx9_plane t = { normal->x, normal->y, normal->z,
                      -(normal->x * point->x + normal->y * point->y + normal->z * point->z) };
    *p = t;
}
void d3dx9_plane_from_points(d3dx9_plane *p, const d3dx9_vec3 *v1, const d3dx9_vec3 *v2, const d3dx9_vec3 *v3) {
    d3dx9_vec3 e1 = v3_sub(v2, v1), e2 = v3_sub(v3, v1);
    d3dx9_vec3 crossed = v3_cross(&e1, &e2);
    d3dx9_vec3 n = v3_unit(&crossed);
    d3dx9_plane t = { n.x, n.y, n.z, -v3_dot(&n, v1) };
    *p = t;
}
void d3dx9_plane_normalize(d3dx9_plane *out, const d3dx9_plane *p) {
    float n2 = p->a * p->a + p->b * p->b + p->c * p->c;
    if (n2 > 0.0f) {
        float inv = 1.0f / sqrtf(n2);
        d3dx9_plane t = { p->a * inv, p->b * inv, p->c * inv, p->d * inv };
        *out = t;
    } else {
        *out = *p;
    }
}
float d3dx9_plane_dot_coord(const d3dx9_plane *p, const d3dx9_vec3 *v) {
    return p->a * v->x + p->b * v->y + p->c * v->z + p->d;
}
float d3dx9_plane_dot_normal(const d3dx9_plane *p, const d3dx9_vec3 *v) {
    return p->a * v->x + p->b * v->y + p->c * v->z;
}

/* -------------------------------------------------------------------- colours */

void d3dx9_color_add(d3dx9_color *out, const d3dx9_color *x, const d3dx9_color *y) {
    d3dx9_color t = { x->r + y->r, x->g + y->g, x->b + y->b, x->a + y->a };
    *out = t;
}
void d3dx9_color_subtract(d3dx9_color *out, const d3dx9_color *x, const d3dx9_color *y) {
    d3dx9_color t = { x->r - y->r, x->g - y->g, x->b - y->b, x->a - y->a };
    *out = t;
}
void d3dx9_color_modulate(d3dx9_color *out, const d3dx9_color *x, const d3dx9_color *y) {
    d3dx9_color t = { x->r * y->r, x->g * y->g, x->b * y->b, x->a * y->a };
    *out = t;
}
void d3dx9_color_scale(d3dx9_color *out, const d3dx9_color *c, float s) {
    d3dx9_color t = { c->r * s, c->g * s, c->b * s, c->a * s };
    *out = t;
}
void d3dx9_color_negative(d3dx9_color *out, const d3dx9_color *c) {
    d3dx9_color t = { 1.0f - c->r, 1.0f - c->g, 1.0f - c->b, 1.0f - c->a };
    *out = t;
}
void d3dx9_color_lerp(d3dx9_color *out, const d3dx9_color *x, const d3dx9_color *y, float s) {
    d3dx9_color t = { x->r + s * (y->r - x->r), x->g + s * (y->g - x->g),
                      x->b + s * (y->b - x->b), x->a + s * (y->a - x->a) };
    *out = t;
}
void d3dx9_color_adjust_contrast(d3dx9_color *out, const d3dx9_color *c, float s) {
    d3dx9_color t = { 0.5f + s * (c->r - 0.5f), 0.5f + s * (c->g - 0.5f),
                      0.5f + s * (c->b - 0.5f), c->a };
    *out = t;
}
void d3dx9_color_adjust_saturation(d3dx9_color *out, const d3dx9_color *c, float s) {
    float grey = c->r * 0.2125f + c->g * 0.7154f + c->b * 0.0721f;
    d3dx9_color t = { grey + s * (c->r - grey), grey + s * (c->g - grey),
                      grey + s * (c->b - grey), c->a };
    *out = t;
}

/* ----------------------------------------------------------- matrix extras */

float d3dx9_matrix_determinant(const d3dx9_matrix *m) {
    const float (*a)[4] = m->m;
    float det;
    float m00=a[0][0],m01=a[0][1],m02=a[0][2],m03=a[0][3];
    float m10=a[1][0],m11=a[1][1],m12=a[1][2],m13=a[1][3];
    float m20=a[2][0],m21=a[2][1],m22=a[2][2],m23=a[2][3];
    float m30=a[3][0],m31=a[3][1],m32=a[3][2],m33=a[3][3];
    det = m00*(m11*(m22*m33-m23*m32)-m12*(m21*m33-m23*m31)+m13*(m21*m32-m22*m31))
        - m01*(m10*(m22*m33-m23*m32)-m12*(m20*m33-m23*m30)+m13*(m20*m32-m22*m30))
        + m02*(m10*(m21*m33-m23*m31)-m11*(m20*m33-m23*m30)+m13*(m20*m31-m21*m30))
        - m03*(m10*(m21*m32-m22*m31)-m11*(m20*m32-m22*m30)+m12*(m20*m31-m21*m30));
    return det;
}

void d3dx9_matrix_rotation_axis(d3dx9_matrix *out, const d3dx9_vec3 *axis, float angle) {
    d3dx9_vec3 a = v3_unit(axis);
    float c = cosf(angle), s = sinf(angle), cc = 1.0f - c;
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    /* D3DX row-vector convention (v' = v*M), per public D3DX docs. */
    t.m[0][0] = cc*a.x*a.x + c;       t.m[0][1] = cc*a.x*a.y + s*a.z; t.m[0][2] = cc*a.x*a.z - s*a.y;
    t.m[1][0] = cc*a.x*a.y - s*a.z;   t.m[1][1] = cc*a.y*a.y + c;     t.m[1][2] = cc*a.y*a.z + s*a.x;
    t.m[2][0] = cc*a.x*a.z + s*a.y;   t.m[2][1] = cc*a.y*a.z - s*a.x; t.m[2][2] = cc*a.z*a.z + c;
    *out = t;
}

void d3dx9_matrix_rotation_yaw_pitch_roll(d3dx9_matrix *out, float yaw, float pitch, float roll) {
    /* D3DX row-vector convention (v' = v*M): M = Rz(roll)*Rx(pitch)*Ry(yaw). */
    float sy=sinf(yaw), cy=cosf(yaw);
    float sp=sinf(pitch), cp=cosf(pitch);
    float sr=sinf(roll), cr=cosf(roll);
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    t.m[0][0] =  sr*sp*sy + cr*cy; t.m[0][1] = sr*cp; t.m[0][2] = sr*sp*cy - cr*sy;
    t.m[1][0] =  cr*sp*sy - sr*cy; t.m[1][1] = cr*cp; t.m[1][2] = cr*sp*cy + sr*sy;
    t.m[2][0] =  cp*sy;            t.m[2][1] = -sp;   t.m[2][2] = cp*cy;
    *out = t;
}

void d3dx9_matrix_rotation_quaternion(d3dx9_matrix *out, const d3dx9_quat *q) {
    float xx=q->x*q->x, yy=q->y*q->y, zz=q->z*q->z;
    float xy=q->x*q->y, xz=q->x*q->z, yz=q->y*q->z;
    float wx=q->w*q->x, wy=q->w*q->y, wz=q->w*q->z;
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    /* D3DX row-vector convention (v' = v*M), per public D3DX docs. */
    t.m[0][0]=1.0f-2.0f*(yy+zz); t.m[0][1]=2.0f*(xy+wz);     t.m[0][2]=2.0f*(xz-wy);
    t.m[1][0]=2.0f*(xy-wz);      t.m[1][1]=1.0f-2.0f*(xx+zz); t.m[1][2]=2.0f*(yz+wx);
    t.m[2][0]=2.0f*(xz+wy);      t.m[2][1]=2.0f*(yz-wx);     t.m[2][2]=1.0f-2.0f*(xx+yy);
    *out = t;
}

void d3dx9_matrix_reflect(d3dx9_matrix *out, const d3dx9_plane *p) {
    d3dx9_plane n = *p;
    d3dx9_matrix t;
    d3dx9_matrix_identity(&t);
    /* D3DX row-vector convention (v' = v*M), per public D3DX docs. */
    t.m[0][0] = -2.0f*n.a*n.a+1.0f; t.m[0][1] = -2.0f*n.b*n.a;      t.m[0][2] = -2.0f*n.c*n.a;      t.m[0][3] = 0;
    t.m[1][0] = -2.0f*n.a*n.b;      t.m[1][1] = -2.0f*n.b*n.b+1.0f; t.m[1][2] = -2.0f*n.c*n.b;      t.m[1][3] = 0;
    t.m[2][0] = -2.0f*n.a*n.c;      t.m[2][1] = -2.0f*n.b*n.c;      t.m[2][2] = -2.0f*n.c*n.c+1.0f; t.m[2][3] = 0;
    t.m[3][0] = -2.0f*n.a*n.d;      t.m[3][1] = -2.0f*n.b*n.d;      t.m[3][2] = -2.0f*n.c*n.d;      t.m[3][3] = 1;
    *out = t;
}

void d3dx9_matrix_shadow(d3dx9_matrix *out, const d3dx9_vec4 *light, const d3dx9_plane *plane) {
    d3dx9_plane n = *plane;
    float d = n.a*light->x + n.b*light->y + n.c*light->z + n.d*light->w;
    d3dx9_matrix t;
    /* D3DX row-vector convention (v' = v*M), per public D3DX docs. */
    t.m[0][0]=d-n.a*light->x; t.m[0][1]=-n.b*light->x;  t.m[0][2]=-n.c*light->x;  t.m[0][3]=-n.d*light->x;
    t.m[1][0]=-n.a*light->y;  t.m[1][1]=d-n.b*light->y; t.m[1][2]=-n.c*light->y;  t.m[1][3]=-n.d*light->y;
    t.m[2][0]=-n.a*light->z;  t.m[2][1]=-n.b*light->z;  t.m[2][2]=d-n.c*light->z; t.m[2][3]=-n.d*light->z;
    t.m[3][0]=-n.a*light->w;  t.m[3][1]=-n.b*light->w;  t.m[3][2]=-n.c*light->w;  t.m[3][3]=d-n.d*light->w;
    *out = t;
}

void d3dx9_matrix_perspective_fov_lh(d3dx9_matrix *m, float fovy, float aspect, float zn, float zf) {
    float y = 1.0f / tanf(fovy * 0.5f);
    float x = y / aspect;
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0]=x; t.m[1][1]=y;
    t.m[2][2]=zf/(zf-zn); t.m[2][3]=1.0f;
    t.m[3][2]=-zn*zf/(zf-zn);
    *m = t;
}
void d3dx9_matrix_perspective_fov_rh(d3dx9_matrix *m, float fovy, float aspect, float zn, float zf) {
    float y = 1.0f / tanf(fovy * 0.5f);
    float x = y / aspect;
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0]=x; t.m[1][1]=y;
    t.m[2][2]=zf/(zn-zf); t.m[2][3]=-1.0f;
    t.m[3][2]=zn*zf/(zn-zf);
    *m = t;
}
void d3dx9_matrix_perspective_off_center_lh(d3dx9_matrix *m, float l, float r, float b, float t_, float zn, float zf) {
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0]=2.0f*zn/(r-l);
    t.m[1][1]=2.0f*zn/(t_-b);
    t.m[2][0]=(l+r)/(l-r);
    t.m[2][1]=(t_+b)/(b-t_);
    t.m[2][2]=zf/(zf-zn);
    t.m[2][3]=1.0f;
    t.m[3][2]=zn*zf/(zn-zf);
    *m = t;
}
void d3dx9_matrix_perspective_off_center_rh(d3dx9_matrix *m, float l, float r, float b, float t_, float zn, float zf) {
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0]=2.0f*zn/(r-l);
    t.m[1][1]=2.0f*zn/(t_-b);
    t.m[2][0]=(l+r)/(r-l);
    t.m[2][1]=(t_+b)/(t_-b);
    t.m[2][2]=zf/(zn-zf);
    t.m[2][3]=-1.0f;
    t.m[3][2]=zn*zf/(zn-zf);
    *m = t;
}
void d3dx9_matrix_ortho_off_center_lh(d3dx9_matrix *m, float l, float r, float b, float t_, float zn, float zf) {
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0]=2.0f/(r-l);
    t.m[1][1]=2.0f/(t_-b);
    t.m[2][2]=1.0f/(zf-zn);
    t.m[3][0]=(l+r)/(l-r);
    t.m[3][1]=(t_+b)/(b-t_);
    t.m[3][2]=zn/(zn-zf);
    t.m[3][3]=1.0f;
    *m = t;
}
void d3dx9_matrix_ortho_off_center_rh(d3dx9_matrix *m, float l, float r, float b, float t_, float zn, float zf) {
    d3dx9_matrix t = { { { 0 } } };
    t.m[0][0]=2.0f/(r-l);
    t.m[1][1]=2.0f/(t_-b);
    t.m[2][2]=1.0f/(zn-zf);
    t.m[3][0]=(l+r)/(l-r);
    t.m[3][1]=(t_+b)/(b-t_);
    t.m[3][2]=zn/(zn-zf);
    t.m[3][3]=1.0f;
    *m = t;
}

/* ----------------------------------------------------------- vector extras */

float d3dx9_vec2_length(const d3dx9_vec2 *v){ return sqrtf(v->x*v->x+v->y*v->y); }
float d3dx9_vec2_length_sq(const d3dx9_vec2 *v){ return v->x*v->x+v->y*v->y; }
float d3dx9_vec2_dot(const d3dx9_vec2 *a,const d3dx9_vec2 *b){ return a->x*b->x+a->y*b->y; }
float d3dx9_vec2_ccw(const d3dx9_vec2 *a,const d3dx9_vec2 *b){ return a->x*b->y-a->y*b->x; }
void d3dx9_vec2_add(d3dx9_vec2 *o,const d3dx9_vec2 *a,const d3dx9_vec2 *b){ d3dx9_vec2 t={a->x+b->x,a->y+b->y}; *o=t; }
void d3dx9_vec2_subtract(d3dx9_vec2 *o,const d3dx9_vec2 *a,const d3dx9_vec2 *b){ d3dx9_vec2 t={a->x-b->x,a->y-b->y}; *o=t; }
void d3dx9_vec2_scale(d3dx9_vec2 *o,const d3dx9_vec2 *v,float s){ d3dx9_vec2 t={v->x*s,v->y*s}; *o=t; }
void d3dx9_vec2_lerp(d3dx9_vec2 *o,const d3dx9_vec2 *a,const d3dx9_vec2 *b,float s){ d3dx9_vec2 t={a->x+s*(b->x-a->x),a->y+s*(b->y-a->y)}; *o=t; }
void d3dx9_vec2_minimize(d3dx9_vec2 *o,const d3dx9_vec2 *a,const d3dx9_vec2 *b){ d3dx9_vec2 t={a->x<b->x?a->x:b->x,a->y<b->y?a->y:b->y}; *o=t; }
void d3dx9_vec2_maximize(d3dx9_vec2 *o,const d3dx9_vec2 *a,const d3dx9_vec2 *b){ d3dx9_vec2 t={a->x>b->x?a->x:b->x,a->y>b->y?a->y:b->y}; *o=t; }

float d3dx9_vec3_length(const d3dx9_vec3 *v){ return sqrtf(v->x*v->x+v->y*v->y+v->z*v->z); }
float d3dx9_vec3_length_sq(const d3dx9_vec3 *v){ return v->x*v->x+v->y*v->y+v->z*v->z; }
float d3dx9_vec3_dot(const d3dx9_vec3 *a,const d3dx9_vec3 *b){ return a->x*b->x+a->y*b->y+a->z*b->z; }
void d3dx9_vec3_cross(d3dx9_vec3 *o,const d3dx9_vec3 *a,const d3dx9_vec3 *b){
    d3dx9_vec3 t={a->y*b->z-a->z*b->y, a->z*b->x-a->x*b->z, a->x*b->y-a->y*b->x}; *o=t;
}
void d3dx9_vec3_add(d3dx9_vec3 *o,const d3dx9_vec3 *a,const d3dx9_vec3 *b){ d3dx9_vec3 t={a->x+b->x,a->y+b->y,a->z+b->z}; *o=t; }
void d3dx9_vec3_subtract(d3dx9_vec3 *o,const d3dx9_vec3 *a,const d3dx9_vec3 *b){ d3dx9_vec3 t={a->x-b->x,a->y-b->y,a->z-b->z}; *o=t; }
void d3dx9_vec3_scale(d3dx9_vec3 *o,const d3dx9_vec3 *v,float s){ d3dx9_vec3 t={v->x*s,v->y*s,v->z*s}; *o=t; }
void d3dx9_vec3_lerp(d3dx9_vec3 *o,const d3dx9_vec3 *a,const d3dx9_vec3 *b,float s){ d3dx9_vec3 t={a->x+s*(b->x-a->x),a->y+s*(b->y-a->y),a->z+s*(b->z-a->z)}; *o=t; }
void d3dx9_vec3_minimize(d3dx9_vec3 *o,const d3dx9_vec3 *a,const d3dx9_vec3 *b){ d3dx9_vec3 t={a->x<b->x?a->x:b->x,a->y<b->y?a->y:b->y,a->z<b->z?a->z:b->z}; *o=t; }
void d3dx9_vec3_maximize(d3dx9_vec3 *o,const d3dx9_vec3 *a,const d3dx9_vec3 *b){ d3dx9_vec3 t={a->x>b->x?a->x:b->x,a->y>b->y?a->y:b->y,a->z>b->z?a->z:b->z}; *o=t; }

float d3dx9_vec4_length(const d3dx9_vec4 *v){ return sqrtf(v->x*v->x+v->y*v->y+v->z*v->z+v->w*v->w); }
float d3dx9_vec4_length_sq(const d3dx9_vec4 *v){ return v->x*v->x+v->y*v->y+v->z*v->z+v->w*v->w; }
float d3dx9_vec4_dot(const d3dx9_vec4 *a,const d3dx9_vec4 *b){ return a->x*b->x+a->y*b->y+a->z*b->z+a->w*b->w; }
void d3dx9_vec4_add(d3dx9_vec4 *o,const d3dx9_vec4 *a,const d3dx9_vec4 *b){ d3dx9_vec4 t={a->x+b->x,a->y+b->y,a->z+b->z,a->w+b->w}; *o=t; }
void d3dx9_vec4_subtract(d3dx9_vec4 *o,const d3dx9_vec4 *a,const d3dx9_vec4 *b){ d3dx9_vec4 t={a->x-b->x,a->y-b->y,a->z-b->z,a->w-b->w}; *o=t; }
void d3dx9_vec4_scale(d3dx9_vec4 *o,const d3dx9_vec4 *v,float s){ d3dx9_vec4 t={v->x*s,v->y*s,v->z*s,v->w*s}; *o=t; }
void d3dx9_vec4_lerp(d3dx9_vec4 *o,const d3dx9_vec4 *a,const d3dx9_vec4 *b,float s){ d3dx9_vec4 t={a->x+s*(b->x-a->x),a->y+s*(b->y-a->y),a->z+s*(b->z-a->z),a->w+s*(b->w-a->w)}; *o=t; }
void d3dx9_vec4_minimize(d3dx9_vec4 *o,const d3dx9_vec4 *a,const d3dx9_vec4 *b){ d3dx9_vec4 t={a->x<b->x?a->x:b->x,a->y<b->y?a->y:b->y,a->z<b->z?a->z:b->z,a->w<b->w?a->w:b->w}; *o=t; }
void d3dx9_vec4_maximize(d3dx9_vec4 *o,const d3dx9_vec4 *a,const d3dx9_vec4 *b){ d3dx9_vec4 t={a->x>b->x?a->x:b->x,a->y>b->y?a->y:b->y,a->z>b->z?a->z:b->z,a->w>b->w?a->w:b->w}; *o=t; }

/* array variants: stride in bytes, as D3DX takes. */
void d3dx9_vec2_transform_coord_array(d3dx9_vec2 *out,unsigned os,const d3dx9_vec2 *in,unsigned is,const d3dx9_matrix *m,unsigned n){
    if(!out||!in||!m){ fprintf(stderr,"[d3dx9_math] Vec2TransformCoordArray: NULL\n"); return; }
    if(!os) os=sizeof(d3dx9_vec2); if(!is) is=sizeof(d3dx9_vec2);
    for(unsigned i=0;i<n;i++){ d3dx9_vec2 tmp; d3dx9_vec2_transform_coord(&tmp,(const d3dx9_vec2*)((const char*)in+i*is),m); *(d3dx9_vec2*)((char*)out+i*os)=tmp; }
}
void d3dx9_vec2_transform_normal_array(d3dx9_vec2 *out,unsigned os,const d3dx9_vec2 *in,unsigned is,const d3dx9_matrix *m,unsigned n){
    if(!out||!in||!m){ fprintf(stderr,"[d3dx9_math] Vec2TransformNormalArray: NULL\n"); return; }
    if(!os) os=sizeof(d3dx9_vec2); if(!is) is=sizeof(d3dx9_vec2);
    for(unsigned i=0;i<n;i++){ d3dx9_vec2 tmp; d3dx9_vec2_transform_normal(&tmp,(const d3dx9_vec2*)((const char*)in+i*is),m); *(d3dx9_vec2*)((char*)out+i*os)=tmp; }
}
void d3dx9_vec3_transform_coord_array(d3dx9_vec3 *out,unsigned os,const d3dx9_vec3 *in,unsigned is,const d3dx9_matrix *m,unsigned n){
    if(!out||!in||!m){ fprintf(stderr,"[d3dx9_math] Vec3TransformCoordArray: NULL\n"); return; }
    if(!os) os=sizeof(d3dx9_vec3); if(!is) is=sizeof(d3dx9_vec3);
    for(unsigned i=0;i<n;i++){ d3dx9_vec3 tmp; d3dx9_vec3_transform_coord(&tmp,(const d3dx9_vec3*)((const char*)in+i*is),m); *(d3dx9_vec3*)((char*)out+i*os)=tmp; }
}
void d3dx9_vec3_transform_normal_array(d3dx9_vec3 *out,unsigned os,const d3dx9_vec3 *in,unsigned is,const d3dx9_matrix *m,unsigned n){
    if(!out||!in||!m){ fprintf(stderr,"[d3dx9_math] Vec3TransformNormalArray: NULL\n"); return; }
    if(!os) os=sizeof(d3dx9_vec3); if(!is) is=sizeof(d3dx9_vec3);
    for(unsigned i=0;i<n;i++){ d3dx9_vec3 tmp; d3dx9_vec3_transform_normal(&tmp,(const d3dx9_vec3*)((const char*)in+i*is),m); *(d3dx9_vec3*)((char*)out+i*os)=tmp; }
}
void d3dx9_vec4_transform_array(d3dx9_vec4 *out,unsigned os,const d3dx9_vec4 *in,unsigned is,const d3dx9_matrix *m,unsigned n){
    if(!out||!in||!m){ fprintf(stderr,"[d3dx9_math] Vec4TransformArray: NULL\n"); return; }
    if(!os) os=sizeof(d3dx9_vec4); if(!is) is=sizeof(d3dx9_vec4);
    for(unsigned i=0;i<n;i++){ d3dx9_vec4 tmp; d3dx9_vec4_transform(&tmp,(const d3dx9_vec4*)((const char*)in+i*is),m); *(d3dx9_vec4*)((char*)out+i*os)=tmp; }
}

/* ---------------------------------------------------------- quaternion extras */

float d3dx9_quat_length(const d3dx9_quat *q){ return sqrtf(q->x*q->x+q->y*q->y+q->z*q->z+q->w*q->w); }
float d3dx9_quat_length_sq(const d3dx9_quat *q){ return q->x*q->x+q->y*q->y+q->z*q->z+q->w*q->w; }
float d3dx9_quat_dot(const d3dx9_quat *a,const d3dx9_quat *b){ return a->x*b->x+a->y*b->y+a->z*b->z+a->w*b->w; }
d3dx9_quat *d3dx9_quat_inverse(d3dx9_quat *out,const d3dx9_quat *q){
    float n2=q->x*q->x+q->y*q->y+q->z*q->z+q->w*q->w;
    if(n2<=0.0f){ fprintf(stderr,"[d3dx9_math] QuaternionInverse: singular\n"); return NULL; }
    float inv=1.0f/n2;
    d3dx9_quat t={-q->x*inv,-q->y*inv,-q->z*inv,q->w*inv};
    *out=t; return out;
}
void d3dx9_quat_rotation_matrix(d3dx9_quat *out,const d3dx9_matrix *m){
    /* Shoemake/standard D3DXMatrix -> quaternion. */
    float tr=m->m[0][0]+m->m[1][1]+m->m[2][2];
    d3dx9_quat q;
    if(tr>0.0f){
        float s=sqrtf(tr+1.0f)*2.0f;
        q.w=0.25f*s;
        q.x=(m->m[1][2]-m->m[2][1])/s;
        q.y=(m->m[2][0]-m->m[0][2])/s;
        q.z=(m->m[0][1]-m->m[1][0])/s;
    } else if(m->m[0][0]>m->m[1][1] && m->m[0][0]>m->m[2][2]){
        float s=sqrtf(1.0f+m->m[0][0]-m->m[1][1]-m->m[2][2])*2.0f;
        q.w=(m->m[1][2]-m->m[2][1])/s;
        q.x=0.25f*s;
        q.y=(m->m[1][0]+m->m[0][1])/s;
        q.z=(m->m[2][0]+m->m[0][2])/s;
    } else if(m->m[1][1]>m->m[2][2]){
        float s=sqrtf(1.0f+m->m[1][1]-m->m[0][0]-m->m[2][2])*2.0f;
        q.w=(m->m[2][0]-m->m[0][2])/s;
        q.x=(m->m[1][0]+m->m[0][1])/s;
        q.y=0.25f*s;
        q.z=(m->m[2][1]+m->m[1][2])/s;
    } else {
        float s=sqrtf(1.0f+m->m[2][2]-m->m[0][0]-m->m[1][1])*2.0f;
        q.w=(m->m[0][1]-m->m[1][0])/s;
        q.x=(m->m[2][0]+m->m[0][2])/s;
        q.y=(m->m[2][1]+m->m[1][2])/s;
        q.z=0.25f*s;
    }
    *out=q;
}
void d3dx9_quat_rotation_yaw_pitch_roll(d3dx9_quat *out,float yaw,float pitch,float roll){
    float sy=sinf(yaw*0.5f), cy=cosf(yaw*0.5f);
    float sp=sinf(pitch*0.5f), cp=cosf(pitch*0.5f);
    float sr=sinf(roll*0.5f), cr=cosf(roll*0.5f);
    d3dx9_quat q;
    q.x = sp*cy*cr + cp*sy*sr;
    q.y = cp*sy*cr - sp*cy*sr;
    q.z = cp*cy*sr - sp*sy*cr;
    q.w = cp*cy*cr + sp*sy*sr;
    *out=q;
}
void d3dx9_quat_ln(d3dx9_quat *out,const d3dx9_quat *q){
    float vlen=sqrtf(q->x*q->x+q->y*q->y+q->z*q->z);
    d3dx9_quat r={0,0,0,0};
    if(vlen>1e-6f){
        float t=atan2f(vlen,q->w)/vlen;
        r.x=q->x*t; r.y=q->y*t; r.z=q->z*t; r.w=0.0f;
    }
    *out=r;
}
void d3dx9_quat_exp(d3dx9_quat *out,const d3dx9_quat *q){
    float vlen=sqrtf(q->x*q->x+q->y*q->y+q->z*q->z);
    d3dx9_quat r;
    if(vlen>1e-6f){
        float s=sinf(vlen)/vlen;
        r.x=q->x*s; r.y=q->y*s; r.z=q->z*s; r.w=cosf(vlen);
    } else { r.x=0; r.y=0; r.z=0; r.w=1; }
    *out=r;
}

/* -------------------------------------------------------------- plane extras */

float d3dx9_plane_dot(const d3dx9_plane *p,const d3dx9_vec4 *v){ return p->a*v->x+p->b*v->y+p->c*v->z+p->d*v->w; }
void d3dx9_plane_scale(d3dx9_plane *out,const d3dx9_plane *p,float s){ d3dx9_plane t={p->a*s,p->b*s,p->c*s,p->d*s}; *out=t; }
d3dx9_plane *d3dx9_plane_transform(d3dx9_plane *out,const d3dx9_plane *p,const d3dx9_matrix *m){
    /* D3DXPlaneTransform: out = p * inverse(transpose(M)), row-vector convention. */
    d3dx9_matrix inv, it;
    if(!d3dx9_matrix_inverse(&inv,m)){
        fprintf(stderr,"[d3dx9_math] PlaneTransform: singular matrix\n");
        return NULL;
    }
    d3dx9_matrix_transpose(&it,&inv);
    d3dx9_plane t;
    t.a=p->a*it.m[0][0]+p->b*it.m[1][0]+p->c*it.m[2][0]+p->d*it.m[3][0];
    t.b=p->a*it.m[0][1]+p->b*it.m[1][1]+p->c*it.m[2][1]+p->d*it.m[3][1];
    t.c=p->a*it.m[0][2]+p->b*it.m[1][2]+p->c*it.m[2][2]+p->d*it.m[3][2];
    t.d=p->a*it.m[0][3]+p->b*it.m[1][3]+p->c*it.m[2][3]+p->d*it.m[3][3];
    *out=t; return out;
}
