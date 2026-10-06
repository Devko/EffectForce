#pragma once
// Small, branch-light math for the audio thread: libm's exp2f / tanf cost ~10x as much and the
// modules call these per sample. SubForce's dsp/fastmath.h, unchanged but for the namespace; its
// error bounds are checked in SubForce's test/engine_test.cpp, the ones EffectForce relies on in
// test/dsp_test.cpp.
#include "simd.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ef {

constexpr float kPi = 3.14159265f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 2^x, relative error < 1.1e-7 (a degree-6 polynomial on the fraction, the exponent by bits).
inline float exp2Fast(float x) {
    x = clampf(x, -126.0f, 126.0f);
    const int i = static_cast<int>(x < 0.0f ? x - 1.0f : x);   // floor, or one below: f stays in [0, 1]
    const float f = x - static_cast<float>(i);
    float p = 2.120030258e-4f;
    p = p * f + 1.258059288e-3f;
    p = p * f + 9.664113633e-3f;
    p = p * f + 5.549038202e-2f;
    p = p * f + 2.402283251e-1f;
    p = p * f + 6.931471229e-1f;
    p = p * f + 1.0f;
    const uint32_t bits = static_cast<uint32_t>(i + 127) << 23;
    float scale;
    std::memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

// log2(x) for normal x > 0: the exponent by bits, a degree-6 polynomial on the mantissa (error
// < 2.3e-6, continuous from one octave to the next).
inline float log2Fast(float x) {
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    const float e = static_cast<float>(static_cast<int>(bits >> 23) - 127);
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &bits, sizeof m);
    const float u = m - 1.0f;
    float p = -2.584141108e-2f;
    p = p * u + 1.217977931e-1f;
    p = p * u - 2.779052116e-1f;
    p = p * u + 4.575491284e-1f;
    p = p * u - 7.181452413e-1f;
    p = p * u + 1.442544942e+0f;
    return e + u * p;
}

// tan(w) for 0 <= w < pi/2 (filter coefficients): an odd polynomial on [0, pi/4], and
// tan(w) = 1 / tan(pi/2 - w) above. Relative error < 5e-7 up to 0.4 pi (the ladder's range);
// it grows toward the pole, where pi/2 - w loses float precision.
inline float tanFast(float w) {
    constexpr float kQuarter = 0.785398163f, kHalf = 1.570796327f;
    const bool upper = w > kQuarter;
    const float x = upper ? kHalf - w : w, t = x * x;
    float p = 8.657055907e-3f;
    p = p * t + 4.348253831e-3f;
    p = p * t + 2.366562374e-2f;
    p = p * t + 5.362507701e-2f;
    p = p * t + 1.333622932e-1f;
    p = p * t + 3.333325386e-1f;
    p = p * t + 1.0f;
    const float r = p * x;
    return upper ? 1.0f / (r > 1e-12f ? r : 1e-12f) : r;
}

// sin(x) for |x| <= pi/2 as a short odd series, error < 4e-6.
inline float sinQuarter(float x) {
    const float x2 = x * x;
    return x * (1.0f + x2 * (-1.666666667e-1f + x2 * (8.333333333e-3f + x2 * (-1.984126984e-4f + x2 * 2.755731922e-6f))));
}

// sin(2 pi x) for 0 <= x < 1, folded onto a quarter wave.
inline float sinCycle(float x) {
    const float y = x < 0.25f ? x : (x < 0.75f ? 0.5f - x : x - 1.0f);
    return sinQuarter(6.283185307f * y);
}

// floor() without the library call (ARMv7 has no rounding instruction): through an int32, the
// library only for |x| >= 2^31 or NaN (a host's wild song position).
inline double floorFast(double x) {
    if (!(x > -2147483648.0 && x < 2147483648.0)) return std::floor(x);
    const double t = static_cast<double>(static_cast<int32_t>(x));
    return t > x ? t - 1.0 : t;
}
inline float floorFast(float x) {
    if (!(x > -2147483648.0f && x < 2147483648.0f)) return std::floor(x);
    const float t = static_cast<float>(static_cast<int32_t>(x));
    return t > x ? t - 1.0f : t;
}

// MIDI note (semitones, fractional) <-> Hz.
inline float noteHz(float note) { return 440.0f * exp2Fast((note - 69.0f) * (1.0f / 12.0f)); }

// tanh-like saturator, exactly +-1 from |x| = 3 on (a Pade fit: 1st and 3rd order match tanh).
// A division: in a serial chain VFP's divider is quicker than a reciprocal through NEON.
inline float softclip(float x) {
    x = clampf(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// tanh(x) / x, a Pade approximant: within 1% of the real ratio up to |x| = 5 and 19% at 12
// (x times it then reads 1.19 for tanh's 1.0); it falls toward 1/15 instead of 0, so far past
// any level the engine reaches a stage keeps growing slowly. (SubForce's, for its ladder's per-stage
// gains.)
inline float tanhXdX(float x) {
    const float a = x * x;
    return ((a + 105.0f) * a + 945.0f) / ((15.0f * a + 420.0f) * a + 945.0f);
}

// xorshift32: the engine's random numbers (noise, S&H, drift). Never returns 0 from a nonzero state.
inline uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
inline float randBipolar(uint32_t& s) { return static_cast<float>(static_cast<int32_t>(xorshift(s))) * (1.0f / 2147483648.0f); }

} // namespace ef
