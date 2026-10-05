#pragma once
// The modulation sources that need DSP: the two LFOs and the envelope follower. The matrix that
// routes them (and the macros) to parameters is plugin/engine.cpp: it works on parameter values.
#include "common.h"
#include "simd.h"

#include <cfloat>

namespace ef {

enum LfoWave : int { LW_SINE, LW_TRIANGLE, LW_SAW_UP, LW_SAW_DOWN, LW_SQUARE, LW_SH, LW_SMOOTH, LW_COUNT };
enum ModSource : int { MS_OFF, MS_MACRO1, MS_MACRO2, MS_MACRO3, MS_MACRO4, MS_LFO1, MS_LFO2, MS_ENV, MS_COUNT };

struct LfoParams {
    int wave = LW_SINE;
    bool sync = false;
    float rateHz = 1.0f;      // free
    double divBeats = 4.0;    // synced: the period in quarter-note beats
    float phase = 0.0f;       // 0..1 cycles: where the cycle starts (synced: against the bar)
};

// -1..1, one value per control chunk. Synced while MPC plays, the phase comes from the song position, so
// the LFO stays on the beat through loops and jumps; stopped, it runs on at the tempo's rate.
class Lfo {
public:
    void reset(uint32_t seed) {
        rng_ = seed ? seed : 1u;
        cycle_ = lastWhole_ = 0.0;
        held_ = randBipolar(rng_);
        from_ = held_;
        to_ = randBipolar(rng_);
    }

    // The value for the chunk of n samples starting now, then advances past it.
    float next(const LfoParams& p, const Transport& t, int n) {
        const double hz = p.sync ? t.bpm / 60.0 / std::max(p.divBeats, 1e-3) : std::clamp(p.rateHz, 0.001f, 100.0f);
        double c = cycle_;
        if (p.sync && t.playing && t.valid) c = t.beats / std::max(p.divBeats, 1e-3);   // whole cycles count too
        cycle_ = c + hz * n / kRate;
        // The phase shifts the whole cycle, its random values with it: S&H and Smooth change where the
        // shifted cycle starts, so Smooth glides end to end at any phase.
        const double shifted = c + p.phase;
        const double whole = std::floor(shifted);
        if (whole != lastWhole_) {   // a new cycle (or a jump): new random values
            lastWhole_ = whole;
            held_ = randBipolar(rng_);
            from_ = to_;
            to_ = randBipolar(rng_);
        }
        return shape(p.wave, static_cast<float>(shifted - whole));
    }

private:
    float shape(int wave, float ph) const {
        switch (wave) {
            case LW_TRIANGLE: return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
            case LW_SAW_UP: return 2.0f * ph - 1.0f;
            case LW_SAW_DOWN: return 1.0f - 2.0f * ph;
            case LW_SQUARE: return ph < 0.5f ? 1.0f : -1.0f;
            case LW_SH: return held_;
            case LW_SMOOTH: {   // a cosine glide from one random value to the next, over a cycle
                const float s = 0.5f - 0.5f * sinCycle(ph * 0.5f + 0.25f);
                return from_ + (to_ - from_) * s;
            }
            default: return sinCycle(ph);
        }
    }

    double cycle_ = 0.0;      // cycles run so far (free) or the song position in cycles (synced)
    double lastWhole_ = 0.0;  // the cycle the random values belong to
    uint32_t rng_ = 1u;
    float held_ = 0.0f, from_ = 0.0f, to_ = 0.0f;
};

// The input's level as 0..1: peak, attack and release one-pole, times the gain, clipped at 1.
class EnvFollower {
public:
    void reset() { env_ = 0.0f; }
    void set(float attackS, float releaseS, float gainDb) {
        att_ = smoothCoef(std::max(attackS, 1e-4f));
        rel_ = smoothCoef(std::max(releaseS, 1e-3f));
        gain_ = dbToGain(gainDb);
    }
    // Follows a chunk; returns the level at its end. On NEON in a vector's lanes: VFP has no min or
    // max, and its compares stall on the flags.
    float process(const float* L, const float* R, int n) {
#if EF_NEON
        float32x2_t e = vdup_n_f32(env_);
        const float32x2_t att = vdup_n_f32(att_), rel = vdup_n_f32(rel_), big = vdup_n_f32(FLT_MAX);
        for (int i = 0; i < n; ++i) {
            float32x2_t v = vld1_lane_f32(R + i, vld1_dup_f32(L + i), 1);
            v = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(v), vcale_f32(v, big)));   // sanitize()
            v = vabs_f32(v);
            const float32x2_t x = vpmax_f32(v, v);
            e = vfma_f32(e, vsub_f32(x, e), vbsl_f32(vcgt_f32(x, e), att, rel));   // fused, as GCC builds the VFP version
        }
        const float last = vget_lane_f32(e, 0);
#else
        float last = env_;
        for (int i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(sanitize(L[i])), std::fabs(sanitize(R[i])));
            last += (x - last) * (x > last ? att_ : rel_);
        }
#endif
        env_ = last < 1e-9f ? 0.0f : last;   // no denormals while it decays
        return std::min(env_ * gain_, 1.0f);
    }

private:
    float env_ = 0.0f, att_ = 1.0f, rel_ = 1.0f, gain_ = 1.0f;
};

} // namespace ef
