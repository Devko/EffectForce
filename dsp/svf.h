#pragma once
// Andrew Simper's linear trapezoidal state-variable filter (Cytomic, "Solving the continuous SVF
// equations using trapezoidal integration and equivalent currents"), the core of Filter and EQ.
// Two trapezoidal integrators solved together, without a unit delay: the response is the analog
// SVF's under the bilinear transform, prewarped so the cutoff lands exactly where asked.
//
// One step takes the input v0 to the band-pass v1 (gain 1 / k at the cutoff) and the low-pass v2;
// every response is a mix m0 v0 + m1 v1 + m2 v2 (high-pass: 1, -k, -1; notch: 1, -k, 0). So a
// filter is g = tan(pi f / rate), k = 1 / Q and the mix; the update a1..a3 follows from g and k.
// Shelves and bells are only other mixes (and g scaled by the gain).
//
// Modulation: g glides exponentially across a chunk (the cutoff evenly in octaves, but for tan's
// warp near Nyquist), k and the mix in straight lines, and a1..a3 are recomputed every sample
// from g and k, so every sample is a real filter between the two settings. That can't
// destabilize it: the state update is ic' = M ic + (2 a2, 2 a3) v0 with
// M = [2 a1 - 1, -2 a2; 2 a2, 1 - 2 a3], and for any g, k >= 0
//     I - M^T M = 4 g k / (1 + g k + g^2)^2 * (1, -g)^T (1, -g)  >=  0,
// so every sample's M is a contraction, whatever sequence of g and k they come from. (The mix is
// outside the loop: any value is stable.) Gliding a1..a3 themselves would save the reciprocal
// and stay stable too (a blend of contractions is one), but halfway between two distant filters
// they are no filter at all.
//
// L and R run side by side in a two-lane vector: NEON's 64-bit registers on the device.
#include "common.h"
#include "simd.h"

#include <cmath>

namespace ef {

#if EF_NEON
using f2 = float32x2_t;
#else
typedef float f2 __attribute__((vector_size(8)));
#endif

inline f2 splat2(float x) { return f2{x, x}; }
inline bool same2(f2 a, f2 b) { return a[0] == b[0] && a[1] == b[1]; }

// A sample of L and R into one vector and back.
EF_INLINE f2 load2(const float* l, const float* r) {
#if EF_NEON
    return vld1_lane_f32(r, vld1_dup_f32(l), 1);
#else
    return f2{*l, *r};
#endif
}
EF_INLINE void store2(float* l, float* r, f2 x) {
#if EF_NEON
    vst1_lane_f32(l, x, 0);
    vst1_lane_f32(r, x, 1);
#else
    *l = x[0];
    *r = x[1];
#endif
}

inline f2 min2(f2 a, f2 b) {
#if EF_NEON
    return vmin_f32(a, b);
#else
    return a < b ? a : b;
#endif
}
inline f2 max2(f2 a, f2 b) {
#if EF_NEON
    return vmax_f32(a, b);
#else
    return a > b ? a : b;
#endif
}

// 1 / d for d > 0: NEON's estimate and two Newton-Raphson steps (about 2e-7), no VFP divider.
EF_INLINE f2 recip2(f2 d) {
#if EF_NEON
    f2 e = vrecpe_f32(d);
    e = vmul_f32(e, vrecps_f32(d, e));
    return vmul_f32(e, vrecps_f32(d, e));
#else
    return splat2(1.0f) / d;
#endif
}

// fastmath.h's softclip for both lanes.
EF_INLINE f2 softclip2(f2 x) {
    x = min2(max2(x, splat2(-3.0f)), splat2(3.0f));
    const f2 x2 = x * x;
    return x * (splat2(27.0f) + x2) * recip2(splat2(27.0f) + splat2(9.0f) * x2);
}

// What a filter lets into its state (sanitize() for both lanes, without a branch): NaN, the
// infinities and anything past 1e8 (160 dB over full scale, no signal) become 0, so no gain
// inside can overflow a float.
EF_INLINE f2 safeIn(f2 x) {
#if EF_NEON
    return vbsl_f32(vcale_f32(x, vdup_n_f32(1e8f)), x, vdup_n_f32(0.0f));   // |x| <= 1e8: false for NaN
#else
    return (x >= splat2(-1e8f) && x <= splat2(1e8f)) ? x : splat2(0.0f);
#endif
}

// A parameter clamped to its range; NaN (which no comparison catches) becomes `nan`.
inline float clampParam(float x, float lo, float hi, float nan) {
    return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan);
}

// The prewarped g for a cutoff in Hz; libm's tan, so per chunk only.
inline float svfG(float hz) { return std::tan(kPi * hz / kRate); }

// The state update for g and k.
struct SvfUpdate {
    f2 a1, a2, a3;

    // Exact (a division): per chunk.
    static SvfUpdate of(f2 g, f2 k) {
        const f2 a1 = splat2(1.0f) / (splat2(1.0f) + g * (g + k));
        return {a1, g * a1, g * g * a1};
    }
    // Per sample, while g and k glide.
    static EF_INLINE SvfUpdate fast(f2 g, f2 k) {
        const f2 a1 = recip2(splat2(1.0f) + g * (g + k));
        const f2 a2 = g * a1;
        return {a1, a2, g * a2};
    }
};

// One SVF's state for L and R.
struct SvfState {
    f2 ic1 = {}, ic2 = {};

    // One sample: the update a, the output mix m[0..2].
    EF_INLINE f2 tick(f2 v0, const SvfUpdate& a, const f2* m) {
        const f2 v3 = v0 - ic2;
        const f2 v1 = a.a1 * ic1 + a.a2 * v3;
        const f2 v2 = ic2 + a.a2 * ic1 + a.a3 * v3;
        ic1 = v1 + v1 - ic1;
        ic2 = v2 + v2 - ic2;
        return m[0] * v0 + m[1] * v1 + m[2] * v2;
    }

    void clear() { ic1 = ic2 = f2{}; }

    // A decaying state passes through the denormals on its way to zero, and x86 without
    // flush-to-zero crawls through them; nothing under 1e-20 (-400 dB) is worth keeping.
    void flushTiny() {
        for (int i = 0; i < 2; ++i) {
            if (std::fabs(ic1[i]) < 1e-20f) ic1[i] = 0.0f;
            if (std::fabs(ic2[i]) < 1e-20f) ic2[i] = 0.0f;
        }
    }
};

// N stereo values that move to new targets over one chunk, all together: the first E (positive:
// cutoffs) evenly in octaves, the others in straight lines. set() calls to(), process() calls
// next() before each sample; jump() for the first set().
template <int N, int E>
class Glide {
public:
    void jump(const f2* t) {
        for (int i = 0; i < N; ++i) cur_[i] = tgt_[i] = t[i];
        left_ = 0;
    }
    // From where the values are now; no glide at all if they are there already.
    void to(const f2* t) {
        bool there = true;
        for (int i = 0; i < N; ++i) {
            tgt_[i] = t[i];
            there = there && same2(cur_[i], t[i]);
        }
        if (there) {
            left_ = 0;
            return;
        }
        for (int i = 0; i < E; ++i) {
            for (int j = 0; j < 2; ++j) {
                const float r = tgt_[i][j] / cur_[i][j];
                step_[i][j] = r > 0.0f && r < 1e30f ? std::pow(r, 1.0f / kChunk) : 1.0f;   // else it lands at the end
            }
        }
        for (int i = E; i < N; ++i) step_[i] = (tgt_[i] - cur_[i]) * splat2(1.0f / kChunk);
        left_ = kChunk;
    }
    EF_INLINE void next() {
        if (left_ == 0) return;
        if (--left_ == 0) {
            for (int i = 0; i < N; ++i) cur_[i] = tgt_[i];   // lands exactly
        } else {
            for (int i = 0; i < E; ++i) cur_[i] *= step_[i];
            for (int i = E; i < N; ++i) cur_[i] += step_[i];
        }
    }
    bool aimsAt(const f2* t) const {
        for (int i = 0; i < N; ++i)
            if (!same2(tgt_[i], t[i])) return false;
        return true;
    }
    bool moving() const { return left_ > 0; }
    const f2* cur() const { return cur_; }
    const f2* target() const { return tgt_; }

private:
    f2 cur_[N] = {}, step_[N] = {}, tgt_[N] = {};
    int left_ = 0;
};

} // namespace ef
