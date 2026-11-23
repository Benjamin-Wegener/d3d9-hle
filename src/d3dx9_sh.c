/*
 * d3dx9_sh.c - D3DX9 Spherical Harmonics (D3DXSH*).
 *
 * Real SH basis as documented by Microsoft's D3DX9 (D3DXSHEvalDirection):
 * coefficients ordered by l=0..order-1, m=-l..l.  Formulas are the public
 * real spherical harmonics; no Microsoft code or DLLs used.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "d3dx9_sh.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


float *d3dx9_sh_eval_direction(float *out, unsigned order, const d3dx9_vec3 *dir) {
    if (!out || !dir || order < 2 || order > 6) {
        fprintf(stderr, "[d3dx9_sh] EvalDirection: invalid args\n");
        return NULL;
    }
    float x = dir->x, y = dir->y, z = dir->z;
    if (isnan(x) || isnan(y) || isnan(z)) {
        fprintf(stderr, "[d3dx9_sh] EvalDirection: NaN in direction\n");
        return NULL;
    }
    if (x == 0.0f && y == 0.0f && z == 0.0f) {
        fprintf(stderr, "[d3dx9_sh] EvalDirection: zero direction\n");
        return NULL;
    }
    float *p = out;
    /* l=0 */
    *p++ = 0.28209479177387814f; /* 1/(2*sqrt(pi)) */
    if (order == 1) return out;
    /* l=1 */
    *p++ = -0.4886025119029199f * y;  /* -sqrt(3/(4pi))*y  m=-1 */
    *p++ =  0.4886025119029199f * z;  /*  sqrt(3/(4pi))*z  m=0  */
    *p++ = -0.4886025119029199f * x;  /* -sqrt(3/(4pi))*x  m=1  */
    if (order == 2) return out;
    /* l=2 */
    float xx=x*x, yy=y*y, zz=z*z;
    *p++ =  1.0925484305920792f * x*y;                /* sqrt(15/(4pi))*x*y */
    *p++ = -1.0925484305920792f * y*z;                /* -sqrt(15/(4pi))*y*z */
    *p++ =  0.31539156525252005f * (3.0f*zz - 1.0f); /* sqrt(5/(16pi))*(3z^2-1) */
    *p++ = -1.0925484305920792f * x*z;                /* -sqrt(15/(4pi))*x*z */
    *p++ =  0.5462742152960396f * (xx - yy);          /* sqrt(15/(16pi))*(x^2-y^2) */
    if (order == 3) return out;
    /* l=3 */
    *p++ = -0.5900435899266435f * y*(3.0f*xx - yy);
    *p++ =  2.890611442640554f  * x*y*z;
    *p++ = -0.4570457994644658f * y*(5.0f*zz - 1.0f);
    *p++ =  0.3731763325901154f * z*(5.0f*zz - 3.0f);
    *p++ = -0.4570457994644658f * x*(5.0f*zz - 1.0f);
    *p++ =  1.445305721320277f  * z*(xx - yy);
    *p++ = -0.5900435899266435f * x*(xx - 3.0f*yy);
    if (order == 4) return out;
    /* l=4 */
    float z2=zz;
    *p++ =  2.5033429417967046f * x*y*(xx-yy);
    *p++ = -1.7701307697799304f * y*z*(3.0f*xx-yy);
    *p++ =  0.9461746957575601f * x*y*(7.0f*z2-1.0f);
    *p++ = -0.6690465435572892f * y*z*(7.0f*z2-3.0f);
    *p++ =  0.10578554691520431f*(35.0f*z2*z2 - 30.0f*z2 + 3.0f);
    *p++ = -0.6690465435572892f * x*z*(7.0f*z2-3.0f);
    *p++ =  0.47308734787878004f*(xx-yy)*(7.0f*z2-1.0f);
    *p++ = -1.7701307697799304f * x*z*(xx-3.0f*yy);
    *p++ =  0.6258357354491761f *(xx*(xx-3.0f*yy)-yy*(3.0f*xx-yy));
    if (order == 5) return out;
    /* l=5 */
    *p++ = -0.6563820568401701f * y*(5.0f*xx*xx - 10.0f*xx*yy + yy*yy);
    *p++ =  8.302649259524165f  * x*y*z*(xx-yy);
    *p++ = -0.4892382994352504f * y*(3.0f*xx-yy)*(9.0f*z2-1.0f);
    *p++ =  4.793536784973324f  * x*y*z*(3.0f*z2-1.0f);
    *p++ = -0.45294665119569694f* y*(21.0f*z2*z2 - 14.0f*z2 + 1.0f);
    *p++ =  0.1169503224534236f * z*(63.0f*z2*z2 - 70.0f*z2 + 15.0f);
    *p++ = -0.45294665119569694f* x*(21.0f*z2*z2 - 14.0f*z2 + 1.0f);
    *p++ =  2.396768392486662f  * z*(xx-yy)*(3.0f*z2-1.0f);
    *p++ = -0.4892382994352504f * x*(xx-3.0f*yy)*(9.0f*z2-1.0f);
    *p++ =  2.075662314881041f  * z*(xx*xx - 6.0f*xx*yy + yy*yy);
    *p++ = -0.6563820568401701f * x*(xx*xx - 10.0f*xx*yy + 5.0f*yy*yy);
    return out;
}

float *d3dx9_sh_rotate_z(float *out, unsigned order, float angle, const float *in) {
    if (!out || !in || order < 1 || order > 6) {
        fprintf(stderr, "[d3dx9_sh] RotateZ: invalid args\n");
        return NULL;
    }
    float c[6], s[6];
    for (unsigned m=0;m<6;m++){ c[m]=cosf(angle*(float)m); s[m]=sinf(angle*(float)m); }
    for (unsigned l=0; l<order; l++) {
        for (int m = -(int)l; m <= (int)l; m++) {
            int am = m < 0 ? -m : m;
            unsigned idx = l*l + (unsigned)(l+m);
            if (m == 0) out[idx] = in[idx];
            else if (m > 0) {
                unsigned ip = l*l + (unsigned)(l+m);
                unsigned ineg = l*l + (unsigned)(l-m);
                out[idx] = c[am]*in[ip] - s[am]*in[ineg];
            } else {
                unsigned ip = l*l + (unsigned)(l+(-m));
                unsigned ineg = l*l + (unsigned)(l+m);
                out[idx] = s[am]*in[ip] + c[am]*in[ineg];
            }
        }
    }
    return out;
}

float *d3dx9_sh_add(float *out, unsigned order, const float *a, const float *b) {
    if (!out || !a || !b || order < 1 || order > 6) {
        fprintf(stderr, "[d3dx9_sh] Add: invalid args\n");
        return NULL;
    }
    unsigned n = order*order;
    for (unsigned i=0;i<n;i++) out[i]=a[i]+b[i];
    return out;
}

float *d3dx9_sh_scale(float *out, unsigned order, const float *in, float scale) {
    if (!out || !in || order < 1 || order > 6) {
        fprintf(stderr, "[d3dx9_sh] Scale: invalid args\n");
        return NULL;
    }
    unsigned n = order*order;
    for (unsigned i=0;i<n;i++) out[i]=in[i]*scale;
    return out;
}

float d3dx9_sh_dot(unsigned order, const float *a, const float *b) {
    if (!a || !b || order < 1 || order > 6) {
        fprintf(stderr, "[d3dx9_sh] Dot: invalid args\n");
        return 0.0f;
    }
    unsigned n = order*order;
    float s=0.0f;
    for (unsigned i=0;i<n;i++) s+=a[i]*b[i];
    return s;
}



float *d3dx9_sh_eval_directional_light(float *pROut, float *pGOut, float *pBOut,
                                       unsigned order, const d3dx9_vec3 *dir,
                                       float r, float g, float b) {
    if (!pROut || !pGOut || !pBOut || !dir || order < 2 || order > 6) {
        fprintf(stderr, "[d3dx9_sh] EvalDirectionalLight: invalid args\n");
        return NULL;
    }
    float tmp[36];
    if (!d3dx9_sh_eval_direction(tmp, order, dir)) return NULL;
    unsigned n = order * order;
    for (unsigned i = 0; i < n; i++) {
        pROut[i] = tmp[i] * r;
        pGOut[i] = tmp[i] * g;
        pBOut[i] = tmp[i] * b;
    }
    return pROut;
}

float *d3dx9_sh_eval_spherical_light(float *pROut, float *pGOut, float *pBOut,
                                     unsigned order, const d3dx9_vec3 *pos, float radius,
                                     float r, float g, float b) {
    (void)pos; (void)radius; (void)r; (void)g; (void)b;
    fprintf(stderr, "[d3dx9_sh] EvalSphericalLight: not implemented (order=%u)\n", order);
    if (pROut && pGOut && pBOut && order >= 2 && order <= 6) {
        unsigned n = order * order;
        memset(pROut, 0, n * sizeof(float));
        memset(pGOut, 0, n * sizeof(float));
        memset(pBOut, 0, n * sizeof(float));
    }
    return NULL; /* honest error: not implemented */
}

float *d3dx9_sh_eval_cone_light(float *pROut, float *pGOut, float *pBOut,
                                unsigned order, const d3dx9_vec3 *dir, float radius,
                                float r, float g, float b) {
    (void)dir; (void)radius; (void)r; (void)g; (void)b;
    fprintf(stderr, "[d3dx9_sh] EvalConeLight: not implemented (order=%u)\n", order);
    if (pROut && pGOut && pBOut && order >= 2 && order <= 6) {
        unsigned n = order * order;
        memset(pROut, 0, n * sizeof(float));
        memset(pGOut, 0, n * sizeof(float));
        memset(pBOut, 0, n * sizeof(float));
    }
    return NULL;
}

float *d3dx9_sh_eval_hemisphere_light(float *pROut, float *pGOut, float *pBOut,
                                      unsigned order, const d3dx9_vec3 *dir,
                                      float topR, float topG, float topB,
                                      float botR, float botG, float botB) {
    (void)dir; (void)topR; (void)topG; (void)topB; (void)botR; (void)botG; (void)botB;
    fprintf(stderr, "[d3dx9_sh] EvalHemisphereLight: not implemented (order=%u)\n", order);
    if (pROut && pGOut && pBOut && order >= 2 && order <= 6) {
        unsigned n = order * order;
        memset(pROut, 0, n * sizeof(float));
        memset(pGOut, 0, n * sizeof(float));
        memset(pBOut, 0, n * sizeof(float));
    }
    return NULL;
}

float *d3dx9_sh_rotate(float *out, unsigned outOrder, const d3dx9_matrix *mat, const float *in) {
    (void)mat;
    fprintf(stderr, "[d3dx9_sh] Rotate (matrix): not implemented (order=%u)\n", outOrder);
    if (out && in && outOrder >= 1 && outOrder <= 6) {
        unsigned n = outOrder * outOrder;
        memset(out, 0, n * sizeof(float));
    }
    return NULL;
}


float *d3dx9_sh_multiply2(float *out, const float *a, const float *b) {
    if (!out || !a || !b) { fprintf(stderr, "[d3dx9_sh] Multiply2: invalid args\n"); return NULL; }
    fprintf(stderr, "[d3dx9_sh] Multiply2: not implemented\n");
    memset(out, 0, 4 * sizeof(float));
    return NULL;
}
float *d3dx9_sh_multiply3(float *out, const float *a, const float *b) {
    if (!out || !a || !b) { fprintf(stderr, "[d3dx9_sh] Multiply3: invalid args\n"); return NULL; }
    fprintf(stderr, "[d3dx9_sh] Multiply3: not implemented\n");
    memset(out, 0, 9 * sizeof(float));
    return NULL;
}
float *d3dx9_sh_multiply4(float *out, const float *a, const float *b) {
    if (!out || !a || !b) { fprintf(stderr, "[d3dx9_sh] Multiply4: invalid args\n"); return NULL; }
    fprintf(stderr, "[d3dx9_sh] Multiply4: not implemented\n");
    memset(out, 0, 16 * sizeof(float));
    return NULL;
}
float *d3dx9_sh_multiply5(float *out, const float *a, const float *b) {
    if (!out || !a || !b) { fprintf(stderr, "[d3dx9_sh] Multiply5: invalid args\n"); return NULL; }
    fprintf(stderr, "[d3dx9_sh] Multiply5: not implemented\n");
    memset(out, 0, 25 * sizeof(float));
    return NULL;
}
float *d3dx9_sh_multiply6(float *out, const float *a, const float *b) {
    if (!out || !a || !b) { fprintf(stderr, "[d3dx9_sh] Multiply6: invalid args\n"); return NULL; }
    fprintf(stderr, "[d3dx9_sh] Multiply6: not implemented\n");
    memset(out, 0, 36 * sizeof(float));
    return NULL;
}

uint32_t d3dx9_sh_project_cube_map(unsigned order, void *texture, float *red, float *green, float *blue) {
    (void)texture; (void)red; (void)green; (void)blue;
    fprintf(stderr, "[d3dx9_sh] ProjectCubeMap: not implemented (order=%u)\n", order);
    return 0x80004001u; /* E_NOTIMPL */
}
