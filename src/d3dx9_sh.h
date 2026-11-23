#ifndef WIN32HLE_D3DX9_SH_H
#define WIN32HLE_D3DX9_SH_H
#include <stdint.h>
#include "d3dx9_math.h"

/* Spherical Harmonics (D3DXSH*), public D3DX9 API. Coeff arrays are float*. */
float *d3dx9_sh_eval_direction(float *out, unsigned order, const d3dx9_vec3 *dir);
float *d3dx9_sh_rotate_z(float *out, unsigned order, float angle, const float *in);
float *d3dx9_sh_add(float *out, unsigned order, const float *a, const float *b);
float *d3dx9_sh_scale(float *out, unsigned order, const float *in, float scale);
float  d3dx9_sh_dot(unsigned order, const float *a, const float *b);

float *d3dx9_sh_eval_directional_light(float *pROut, float *pGOut, float *pBOut,
                                       unsigned order, const d3dx9_vec3 *dir,
                                       float r, float g, float b);
float *d3dx9_sh_eval_spherical_light(float *pROut, float *pGOut, float *pBOut,
                                     unsigned order, const d3dx9_vec3 *pos, float radius,
                                     float r, float g, float b);
float *d3dx9_sh_eval_cone_light(float *pROut, float *pGOut, float *pBOut,
                                unsigned order, const d3dx9_vec3 *dir, float radius,
                                float r, float g, float b);
float *d3dx9_sh_eval_hemisphere_light(float *pROut, float *pGOut, float *pBOut,
                                      unsigned order, const d3dx9_vec3 *dir,
                                      float topR, float topG, float topB,
                                      float botR, float botG, float botB);
float *d3dx9_sh_rotate(float *out, unsigned outOrder, const d3dx9_matrix *mat, const float *in);
float *d3dx9_sh_multiply2(float *out, const float *a, const float *b);
float *d3dx9_sh_multiply3(float *out, const float *a, const float *b);
float *d3dx9_sh_multiply4(float *out, const float *a, const float *b);
float *d3dx9_sh_multiply5(float *out, const float *a, const float *b);
float *d3dx9_sh_multiply6(float *out, const float *a, const float *b);

uint32_t d3dx9_sh_project_cube_map(unsigned order, void *texture, float *red, float *green, float *blue);

#endif
