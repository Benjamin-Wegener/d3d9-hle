/*
 * d3dx9_float16.c - D3DXFLOAT16 array conversions.
 *
 * Public references (semantics/ABI only; no Microsoft binary or foreign code):
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3dx9math.h
 *       D3DXFLOAT16 *WINAPI D3DXFloat32To16Array(D3DXFLOAT16 *pout,
 *                                                const FLOAT *pin, UINT n);
 *       FLOAT *WINAPI D3DXFloat16To32Array(FLOAT *pout,
 *                                          const D3DXFLOAT16 *pin, UINT n);
 *       (both return pout, are stdcall with 3 args, and D3DXFLOAT16 is a
 *        single 16-bit .value)
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/math.c
 *       the array entry points loop over n and return pout.
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/d3dx_helpers.c
 *       half conversion: Inf/NaN -> sign|0x7fff, signed zero, round-to-
 *       nearest-even including the subnormal path; the 16->32 direction
 *       treats the half exponent-31 (Inf/NaN) field as an ordinary biased
 *       exponent, so 0x7fff becomes 131008.0f and 0xffff becomes -131008.0f.
 *
 * The procedures below are written from those documented rules; every shim
 * has exactly one RET with the correct stdcall argc (3 for both APIs).
 */
#include <math.h>
#include <string.h>
#include "win32hle.h"
#include "d3dx9_float16.h"

uint16_t d3dx_f32_to_f16(uint32_t bits) {
    float in;
    memcpy(&in, &bits, sizeof in);

    const uint32_t sign = bits >> 31;               /* bit 15 of the half */
    const uint32_t expf = (bits >> 23) & 0xffu;
    const uint32_t manf = bits & 0x7fffffu;

    /* D3DX collapses both Inf and NaN into the half infinity pattern. */
    if (expf == 0xffu) return (uint16_t)((sign << 15) | 0x7fffu);
    /* Signed zero is preserved. */
    if (expf == 0u && manf == 0u) return (uint16_t)(sign << 15);

    /* Scale |in| into [1024, 2048) so bit 10 is the (implicit) leading one. */
    float tmp = fabsf(in);
    int e = 0;
    while (tmp < 1024.0f)   { tmp *= 2.0f; --e; }
    while (tmp >= 2048.0f)  { tmp /= 2.0f; ++e; }

    e += 25;                                        /* +10 normalise, +15 bias */
    const int origexp = e;

    uint32_t m = (uint32_t)tmp;                     /* 1024..2047 */
    const float frac = tmp - (float)m;
    if ((frac == 0.5f && (m & 1u)) || frac > 0.5f) ++m;  /* nearest, ties even */
    if (m == 2048u) { m = 1024u; ++e; }             /* mantissa carry */

    uint16_t ret;
    if (e > 31) {
        ret = 0x7fffu;                              /* overflow -> Inf */
    } else if (e <= 0) {
        /* Subnormal half: keep 13 extra bits, denormalise, round to even. */
        if (e < -11) return (uint16_t)(sign << 15); /* underflow -> signed 0 */
        uint32_t r = (uint32_t)(tmp * 8192.0f);     /* 1 << 13 */
        r >>= 1 - e;
        r -= ~(r >> 13) & 1u;
        r >>= 12;
        const uint32_t round = r & 1u;
        r >>= 1;
        ret = (uint16_t)(r + round);
    } else {
        ret = (uint16_t)((e << 10) | (m & 0x3ffu));
    }
    return (uint16_t)(ret | (uint16_t)(sign << 15));
}

uint32_t d3dx_f16_to_f32(uint16_t half) {
    const uint32_t s = half & 0x8000u;
    const uint32_t e = (half & 0x7c00u) >> 10;
    const uint32_t m = half & 0x03ffu;
    const float sgn = s ? -1.0f : 1.0f;

    float v;
    if (e == 0u) {
        v = (m == 0u) ? 0.0f : powf(2.0f, -14.0f) * ((float)m / 1024.0f);
    } else {
        /* No special case for e == 31: D3DX maps half Inf/NaN to large
         * finite floats (0x7fff -> 131008.0f). */
        v = powf(2.0f, (float)e - 15.0f) * (1.0f + (float)m / 1024.0f);
    }
    const float out = sgn * v;                      /* keeps -0.0f negative */
    uint32_t bits;
    memcpy(&bits, &out, sizeof bits);
    return bits;
}

void s_D3DXFloat32To16Array(void) {                 /* (pout, pin, n) */
    uint32_t out = A32(0);
    uint32_t in = A32(1);
    uint32_t n = A32(2);
    for (uint32_t i = 0; i < n; ++i)
        MEM16(out + 2u * i) = d3dx_f32_to_f16(MEM32(in + 4u * i));
    RET(out, 3);
}

void s_D3DXFloat16To32Array(void) {                 /* (pout, pin, n) */
    uint32_t out = A32(0);
    uint32_t in = A32(1);
    uint32_t n = A32(2);
    for (uint32_t i = 0; i < n; ++i)
        MEM32(out + 4u * i) = d3dx_f16_to_f32((uint16_t)MEM16(in + 2u * i));
    RET(out, 3);
}
