#pragma once
// 2x up and down: the decimator (SubForce's dsp/halfband.h; there the whole voice runs at
// 88.2 kHz) folds a 2x oversampled signal back to MPC's 44.1 kHz; the interpolator, its mirror
// image, lifts 44.1 kHz to 88.2 kHz (the drive shapes there). Both are the same polyphase IIR
// halfband (two chains of first-order allpasses at the low rate, Laurent de Soras's HIIR
// structure): 8 multiplies per low-rate sample each.
//
// Design: 8 coefficients, transition band 0.0232 of the high rate (passband to 20.0 kHz,
// stopband from 24.1 kHz): passband ripple < 1e-7 dB, stopband >= 85 dB. Coefficients from
// HIIR's PolyphaseIir2Designer formulas (SubForce's tools/halfband_design.py prints them); the
// decimator's response is measured in SubForce's test/engine_test.cpp, the interpolator's (and
// the pair's) in test/drive_test.cpp. Nonlinear phase (an IIR), like any analog filter: up and
// down together delay low frequencies by 2.7 samples, 20 kHz by 5.
//
// The stereo versions at the end run both channels' four chains in the lanes of one f4
// (dsp/simd.h): the same arithmetic, a quarter of the instructions.
#include "simd.h"

namespace ef {

// The allpass coefficients: even indices form one chain, odd indices the other.
inline constexpr int kHalfbandCoefs = 8;
inline constexpr float kHalfband[kHalfbandCoefs] = {0.0536154666f, 0.1934367242f, 0.3723159713f, 0.5466893949f,
                                                    0.6930302670f, 0.8067034095f, 0.8940130551f, 0.9658743028f};

class Decimator {
public:
    static constexpr int kCoefs = kHalfbandCoefs;

    // Two high-rate samples, `early` first, become one low-rate sample.
    float process(float early, float late) {
        float a = late, b = early;   // a: the even chain (coefs 0, 2, ...), b: the odd chain (1, 3, ...)
        for (int i = 0; i < kCoefs; i += 2) {
            const float ta = (a - y_[i]) * kHalfband[i] + x_[i];
            const float tb = (b - y_[i + 1]) * kHalfband[i + 1] + x_[i + 1];
            x_[i] = a;
            x_[i + 1] = b;
            y_[i] = a = ta;
            y_[i + 1] = b = tb;
        }
        return 0.5f * (a + b);
    }

    void reset() {
        for (int i = 0; i < kCoefs; ++i) x_[i] = y_[i] = 0.0f;
    }

private:
    float x_[kCoefs] = {}, y_[kCoefs] = {};
};

// HIIR's Upsampler2x: one low-rate sample feeds both chains; the even chain's output is the early
// high-rate sample, the odd chain's the late one. Zero-stuffing would halve the level and the
// halfband's sum of the chains would double it again, so neither appears: passband gain is 1.
// The image of an input at f sits at 44.1 kHz - f: for 0..20 kHz in the stopband, >= 85 dB down.
class Interpolator {
public:
    static constexpr int kCoefs = kHalfbandCoefs;

    // One low-rate sample becomes two high-rate samples, `early` first.
    void process(float in, float& early, float& late) {
        float a = in, b = in;   // a: the even chain (coefs 0, 2, ...), b: the odd chain (1, 3, ...)
        for (int i = 0; i < kCoefs; i += 2) {
            const float ta = (a - y_[i]) * kHalfband[i] + x_[i];
            const float tb = (b - y_[i + 1]) * kHalfband[i + 1] + x_[i + 1];
            x_[i] = a;
            x_[i + 1] = b;
            y_[i] = a = ta;
            y_[i + 1] = b = tb;
        }
        early = a;
        late = b;
    }

    void reset() {
        for (int i = 0; i < kCoefs; ++i) x_[i] = y_[i] = 0.0f;
    }

private:
    float x_[kCoefs] = {}, y_[kCoefs] = {};
};

// The allpass chains of two channels side by side, lanes (left even, left odd, right even,
// right odd): each lane steps exactly as the scalar classes' chains do.
class StereoHalfband {
public:
    void reset() {
        for (int s = 0; s < kStages; ++s) x_[s] = y_[s] = splat(0.0f);
    }

protected:
    static constexpr int kStages = kHalfbandCoefs / 2;

    f4 run(f4 a) {
        for (int s = 0; s < kStages; ++s) {
            const f4 c = f4{kHalfband[2 * s], kHalfband[2 * s + 1], kHalfband[2 * s], kHalfband[2 * s + 1]};
            const f4 t = (a - y_[s]) * c + x_[s];
            x_[s] = a;
            y_[s] = a = t;
        }
        return a;
    }

private:
    f4 x_[kStages] = {}, y_[kStages] = {};
};

class StereoInterpolator : public StereoHalfband {
public:
    // One low-rate sample per channel becomes (left early, left late, right early, right late).
    f4 process(float left, float right) {
#if EF_NEON
        return run(vcombine_f32(vdup_n_f32(left), vdup_n_f32(right)));
#else
        return run(f4{left, left, right, right});
#endif
    }
};

class StereoDecimator : public StereoHalfband {
public:
#if EF_NEON
    // Two frames (L R)(L R), early first, become one (L R): process()'s arithmetic, the lanes
    // arranged in registers instead of assembled from scalars.
    float32x2_t processPair(float32x4_t frames) {
        const float32x2x2_t t = vtrn_f32(vget_low_f32(frames), vget_high_f32(frames));   // (Le Ll), (Re Rl)
        const f4 a = run(vrev64q_f32(vcombine_f32(t.val[0], t.val[1])));
        return vmul_f32(vpadd_f32(vget_low_f32(a), vget_high_f32(a)), vdup_n_f32(0.5f));
    }
#endif
    // (left early, left late, right early, right late) become one low-rate sample per channel.
    void process(f4 high, float& left, float& right) {
#if EF_NEON
        const f4 a = run(vrev64q_f32(high));   // the even chains take the late samples
        const float32x2_t sum = vpadd_f32(vget_low_f32(a), vget_high_f32(a));
        left = 0.5f * vget_lane_f32(sum, 0);
        right = 0.5f * vget_lane_f32(sum, 1);
#else
        const f4 a = run(f4{high[1], high[0], high[3], high[2]});
        left = 0.5f * (a[0] + a[1]);
        right = 0.5f * (a[2] + a[3]);
#endif
    }
};

} // namespace ef
