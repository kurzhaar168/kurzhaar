/* SPDX-License-Identifier: GPL-3.0-only */
/* Bass engine maths: no libm on the FM-1, so the few functions the engine needs, in float, the same on the Mac
 * and on the chip (bit-identical results: no host libm involved). */
#ifndef BE_MATH_H
#define BE_MATH_H
#include <stdint.h>

#define BE_INL static inline __attribute__((always_inline))   /* the hot path must not call (it may run from RAM) */

#define BE_PI 3.14159265358979f

BE_INL float be_fabs(float x) { return x < 0.0f ? -x : x; }
BE_INL float be_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* 2^x, relative error < 2e-7 for |x| < 126 */
BE_INL float be_exp2(float x)
{
    union { float f; uint32_t u; } r;
    int32_t i;
    float f, p;
    if (x < -126.0f) return 0.0f;
    if (x > 126.0f) x = 126.0f;
    i = (int32_t)x;
    if ((float)i > x) i--;                       /* floor */
    f = x - (float)i;                            /* 0..1 */
    p = 1.0f + f * (0.693147182f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f
        + f * 0.00133335581f))));
    r.u = (uint32_t)(i + 127) << 23;
    return r.f * p;
}
BE_INL float be_exp(float x) { return be_exp2(x * 1.44269504f); }

/* tanh(x)/x: smooth, never divides by zero (1 at 0). Lambert's continued fraction, error < 1e-6 to |x| = 4.97 */
BE_INL float be_tanh_ratio(float x)
{
    float x2 = x * x;
    if (x2 > 24.7f) return 1.0f / be_fabs(x);    /* |x| > 4.97: tanh = +-1 */
    return (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)))
         / (135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f)));
}
BE_INL float be_tanh(float x) { return x * be_tanh_ratio(x); }

/* the same ratio as numerator and denominator (n / d = tanh(x)/x), for callers that share one division between
 * several; scaled so d stays within ~1..30 (a product of five stays far from float overflow) */
BE_INL void be_tanh_nd(float x, float *n, float *d)
{
    float x2 = x * x;
    if (x2 > 24.7f) { *n = 1.0f; *d = be_fabs(x); return; }
    *n = 1.0f + x2 * (0.128205128f + x2 * (0.00279720280f + x2 * 7.40000740e-6f));      /* / 135135 */
    *d = 1.0f + x2 * (0.461538462f + x2 * (0.0233100233f + x2 * 2.07200207e-4f));
}

/* tan(x) for 0 <= x < 1.2 (filter prewarping), Pade 5/4 */
BE_INL float be_tan(float x)
{
    float x2 = x * x;
    return x * (945.0f + x2 * (-105.0f + x2)) / (945.0f + x2 * (-420.0f + x2 * 15.0f));
}

/* sin(2 pi p) for a phase p in [0, 1): odd polynomial, error < 1e-6 */
BE_INL float be_sin2pi(float p)
{
    float x, x2;
    p -= 0.5f;                                   /* -0.5..0.5, sin(2pi p) = -sin(2pi (p-0.5)) */
    if (p > 0.25f) p = 0.5f - p;                 /* fold into -0.25..0.25 */
    else if (p < -0.25f) p = -0.5f - p;
    x = p * 2.0f * BE_PI;
    x2 = x * x;
    return -x * (1.0f + x2 * (-0.166666667f + x2 * (0.00833333333f + x2 * (-0.000198412698f
        + x2 * 2.75573192e-6f))));
}

/* white noise, -1..1 */
BE_INL float be_noise(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}
#endif
