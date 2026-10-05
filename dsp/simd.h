#pragma once
// Four floats at a time for the per-sample hot spots (SubForce's dsp/simd.h, unchanged but for the
// namespace; there: the ladder, the cutoff coefficients; here: the reverb's delay network). NEON on
// the device, GCC's generic vectors elsewhere, so the x86 tests run the same arithmetic lane by
// lane.
//
// Reciprocals: ARMv7 has no vector divide, and VFP's divider isn't pipelined (about 15 cycles a
// division, one at a time; the engine used to queue 14 of them per sample behind each other).
// NEON's estimate (8 bits) refined by Newton-Raphson steps runs in the pipelined vector unit: one
// step is good to about 2e-5, two to about 2e-7. Elsewhere they are plain divisions.
#include <cstdint>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define EF_NEON 1
#else
#define EF_NEON 0
#endif

// For the few per-sample helpers GCC would otherwise call out of line.
#define EF_INLINE inline __attribute__((always_inline))

namespace ef {

#if EF_NEON
using f4 = float32x4_t;
using i4 = int32x4_t;
#else
typedef float f4 __attribute__((vector_size(16)));
typedef int32_t i4 __attribute__((vector_size(16)));
#endif

inline f4 splat(float x) { return f4{x, x, x, x}; }

inline f4 load4(const float* p) {
#if EF_NEON
    return vld1q_f32(p);
#else
    return f4{p[0], p[1], p[2], p[3]};
#endif
}

inline void store4(float* p, f4 v) {
#if EF_NEON
    vst1q_f32(p, v);
#else
    p[0] = v[0];
    p[1] = v[1];
    p[2] = v[2];
    p[3] = v[3];
#endif
}

// a's top 4 - N lanes, then b's bottom N: ext<N>(a, b) = (a[N], .., a[3], b[0], .., b[N - 1]).
template <int N>
inline f4 ext(f4 a, f4 b) {
#if EF_NEON
    return vextq_f32(a, b, N);
#else
    f4 r;
    for (int i = 0; i < 4; ++i) r[i] = i + N < 4 ? a[i + N] : b[i + N - 4];
    return r;
#endif
}

// Lane 3 in every lane.
inline f4 lane3(f4 a) {
#if EF_NEON
    return vdupq_lane_f32(vget_high_f32(a), 1);
#else
    return splat(a[3]);
#endif
}

inline f4 min4(f4 a, f4 b) {
#if EF_NEON
    return vminq_f32(a, b);
#else
    return a < b ? a : b;
#endif
}

inline f4 max4(f4 a, f4 b) {
#if EF_NEON
    return vmaxq_f32(a, b);
#else
    return a > b ? a : b;
#endif
}

// 1 / d for d > 0, Steps Newton-Raphson steps on NEON's estimate.
template <int Steps>
inline f4 recip4(f4 d) {
#if EF_NEON
    f4 e = vrecpeq_f32(d);
    for (int i = 0; i < Steps; ++i) e = vmulq_f32(e, vrecpsq_f32(d, e));
    return e;
#else
    return splat(1.0f) / d;
#endif
}

// The same for one value, without VFP's divider.
template <int Steps>
inline float recip1(float d) {
#if EF_NEON
    const float32x2_t v = vdup_n_f32(d);
    float32x2_t e = vrecpe_f32(v);
    for (int i = 0; i < Steps; ++i) e = vmul_f32(e, vrecps_f32(v, e));
    return vget_lane_f32(e, 0);
#else
    return 1.0f / d;
#endif
}

// tanh(x) / x: the Pade approximant of fastmath.h's tanhXdX, four at a time. Its denominator is
// at least 945, so one refining step leaves 2e-5 -- far inside the approximant's own error.
inline f4 tanhXdX4(f4 x) {
    const f4 a = x * x;
    const f4 num = (a + splat(105.0f)) * a + splat(945.0f);
    const f4 den = (splat(15.0f) * a + splat(420.0f)) * a + splat(945.0f);
    return num * recip4<1>(den);
}

// 2^x, fastmath.h's exp2Fast four at a time (the same polynomial, the exponent by bits).
inline f4 exp2Fast4(f4 x) {
    x = min4(max4(x, splat(-126.0f)), splat(126.0f));
#if EF_NEON
    const uint32x4_t neg = vcltq_f32(x, vdupq_n_f32(0.0f));
    const f4 xt = vbslq_f32(neg, vsubq_f32(x, vdupq_n_f32(1.0f)), x);
    const i4 i = vcvtq_s32_f32(xt);   // toward zero: floor, or one below
    const f4 f = vsubq_f32(x, vcvtq_f32_s32(i));
#else
    const f4 xt = x < splat(0.0f) ? x - splat(1.0f) : x;
    const i4 i = __builtin_convertvector(xt, i4);
    const f4 f = x - __builtin_convertvector(i, f4);
#endif
    f4 p = splat(2.120030258e-4f);
    p = p * f + splat(1.258059288e-3f);
    p = p * f + splat(9.664113633e-3f);
    p = p * f + splat(5.549038202e-2f);
    p = p * f + splat(2.402283251e-1f);
    p = p * f + splat(6.931471229e-1f);
    p = p * f + splat(1.0f);
#if EF_NEON
    const f4 scale = vreinterpretq_f32_s32(vshlq_n_s32(vaddq_s32(i, vdupq_n_s32(127)), 23));
#else
    const i4 bits = (i + 127) << 23;
    f4 scale;
    __builtin_memcpy(&scale, &bits, sizeof scale);
#endif
    return p * scale;
}

// tan(w) for 0 <= w < pi/2: fastmath.h's tanFast four at a time.
inline f4 tanFast4(f4 w) {
    const f4 quarter = splat(0.785398163f), half = splat(1.570796327f);
#if EF_NEON
    const uint32x4_t upper = vcgtq_f32(w, quarter);
    const f4 x = vbslq_f32(upper, vsubq_f32(half, w), w);
#else
    const auto upper = w > quarter;
    const f4 x = upper ? half - w : w;
#endif
    const f4 t = x * x;
    f4 p = splat(8.657055907e-3f);
    p = p * t + splat(4.348253831e-3f);
    p = p * t + splat(2.366562374e-2f);
    p = p * t + splat(5.362507701e-2f);
    p = p * t + splat(1.333622932e-1f);
    p = p * t + splat(3.333325386e-1f);
    p = p * t + splat(1.0f);
    const f4 r = p * x;
    const f4 inv = recip4<2>(max4(r, splat(1e-12f)));
#if EF_NEON
    return vbslq_f32(upper, inv, r);
#else
    return upper ? inv : r;
#endif
}

} // namespace ef
