#ifndef WIN32HLE_D3DX9_FLOAT16_H
#define WIN32HLE_D3DX9_FLOAT16_H

#include <stdint.h>

/* Bit-exact D3DX half-float conversions (D3DXFLOAT16 is a 16-bit .value). */
uint16_t d3dx_f32_to_f16(uint32_t fbits);
uint32_t d3dx_f16_to_f32(uint16_t half);

/* D3DXFloat32To16Array(out, in, n) / D3DXFloat16To32Array(out, in, n),
 * both stdcall with three arguments and both returning `out`. */
void s_D3DXFloat32To16Array(void);
void s_D3DXFloat16To32Array(void);

#endif /* WIN32HLE_D3DX9_FLOAT16_H */
