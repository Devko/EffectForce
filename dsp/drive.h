#pragma once
// Drive: saturation, clipping, folding and a bit crusher (docs/DESIGN.md, "Modules").
//
//   in -> x Drive -> + bias -> 2x up -> shaper -> 2x down -> DC blocker -> tilt -> x Out -> mix with in
//
// Soft, Tube, Hard, Fold and Sine shape at 88.2 kHz, between dsp/halfband.h's stereo interpolator
// and decimator: the harmonics the shaper makes between 24 and 64 kHz are filtered off instead of
// folding back into the audio band; those above 64 kHz still fold. How much that leaves depends
// on the curve and on how hard it's driven: a -12 dBFS 5 kHz sine at drive 24 dB aliases 82 dB
// under the fundamental through Soft (1 dB hotter: 63, 2 dB: 52), 52 through Tube, 39 through Hard;
// near full drive every curve is a clipper and 2x holds it to about 25 dB. Crush is not oversampled
// (its aliasing is the point).
//
// Bias adds an offset before the shaper, o = bias * 0.5 * sqrt(1 + (drive * 0.25)^2): about half
// the shapers' knee at low drive and half the swing of a -12 dBFS sine at high drive, so it
// bends the curve (even harmonics) at every drive. shaper(o) is subtracted after the shaper, so
// silence stays silent and a bias move doesn't thump; the 10 Hz DC blocker removes the DC the
// asymmetric curve makes from the signal itself.
//
// Gain compensation: Drive changes the character, not the level. Per type, a table over drive
// (1 dB steps) and bias (0.1 steps), computed once from the shapers themselves, holds the gain
// that gives a -12 dBFS sine the same RMS (without DC) after the shaper as before it,
// interpolated per chunk (every 4th sample while drive or bias moves: the compensation is about
// -drive up to the knee and flattens after, so ramping the two separately would swell halfway
// through a jump). Material around -12 dBFS keeps its level at any drive, louder material comes
// out compressed and quieter material lifted.
//
// Crush: bit depth 16 - drive / 3 (16 bits at 0 dB, 4 at 36 dB, fractional between: the step
// is 2^(drive / 3 - 15) of full scale) and a sample-and-hold at 44.1 kHz * 2^(-drive / 10.4)
// (one octave per 10.4 dB: 4 kHz at 36 dB), both straight lines in dB of Drive. The converter
// clips at +-1 and rounds to the nearest step, so quiet signals gate; bias shifts its grid by
// up to half a step (at 1 they buzz at +-half a step instead). No compensation: the level stays.
//
// Tone: a tilt around 1 kHz (a one-pole split, TPT so it holds its shape up to 20 kHz): lows
// x 10^(-6 tone / 20), highs x 10^(6 tone / 20); 100 Hz and 8 kHz 11.3 dB apart at the extremes.
//
// Per chunk, drive (in dB), bias, tone, Out and Mix move in a straight line to their new values;
// a type change crossfades the old and the new shaper over one chunk.
//
// Mix is dry + mix * (wet - dry): at 0 the input comes back bit for bit. The oversampled wet is
// 2.7 samples late at low frequencies and 5 at 20 kHz (the halfbands' phase), so between 0 and 1
// the dry goes through a halfband pair of its own (no shaper between): the two line up at every
// frequency and blend without combing. Crush isn't oversampled and blends with the raw input.
// Leaving 0, the dry crossfades from raw to filtered over the chunk (and back on the way to 0).
// At 1, and while the dry is raw, the dry pair rests; starting again, it first re-runs the last
// kHistory input samples, so it picks up as if it had never stopped (the halfbands' ring is 76 dB
// down after 256).
#include "common.h"
#include "halfband.h"

namespace ef {

namespace drv {

// floor() for |x| < 2^31, branch-free (the shapers' callers keep x in range).
inline float floorSmall(float x) {
    const float t = static_cast<float>(static_cast<int32_t>(x));
    return t > x ? t - 1.0f : t;
}

// The shapers, on the driven signal u (input x Drive + bias offset). Each has slope 1 at 0 (a quiet
// signal at 0 dB drive passes unchanged), is continuous, and is finite for any finite u.

constexpr float kSoftEdge = 2.70703125f;   // 11!! / 10!!: where the soft curve reaches 1

// The odd polynomial whose slope is (1 - (u / kSoftEdge)^2)^5: flat at +-1 from |u| = kSoftEdge
// on, its first five derivatives continuous there. tanh-like, but up to about 4.5x past its knee
// its high harmonics are 10 to 30 dB lower than tanh's, so 2x oversampling holds them (the
// 15th..23rd of a sine driven to 4x: 81 dB under the fundamental, tanh's 52; at 5x, 52 and 45).
inline float soft(float u) {
    const float v = clampf(u * (1.0f / kSoftEdge), -1.0f, 1.0f), w = v * v;
    // the integral of (1 - w)^5 term by term: binomial coefficients over 1, 3, 5, ...
    return kSoftEdge * v * (1.0f - w * (5.0f / 3.0f - w * (2.0f - w * (10.0f / 7.0f - w * (5.0f / 9.0f - w * (1.0f / 11.0f))))));
}

// Soft plus a square-law term, like a triode: even harmonics at every level (the 2nd 30 dB under a
// -12 dBFS sine at drive 0 dB), ceilings at +1.25 and -0.75.
constexpr float kTubeEven = 0.25f;
inline float tube(float u) {
    const float s = soft(u);
    return s + kTubeEven * s * s;
}

// Linear to 0.8, then a quadratic knee that meets 1 with zero slope at 1.2.
inline float hard(float u) {
    const float c = clampf(u, -1.2f, 1.2f);
    const float k = std::max(std::fabs(c) - 0.8f, 0.0f);
    return c - std::copysign(1.25f * k * k, c);
}

// A triangle wavefolder: linear to +-1, then down to -+1 at +-3 and back, period 4. Exact (u
// itself) below |u| = 2; past 2^16 it stops folding.
inline float fold(float u) {
    u = clampf(u, -65536.0f, 65536.0f);
    const float w = u - 4.0f * floorSmall(u * 0.25f + 0.5f);   // -2..2
    return 2.0f * clampf(w, -1.0f, 1.0f) - w;
}

// sin(u), a smooth folder: the triangle bends the phase into sinQuarter's quarter wave.
inline float sine(float u) { return sinQuarter(1.570796327f * fold(u * 0.636619772f)); }

// Drive::Type's shaper by index (0..4; Crush is not a shaper).
inline float shape(int type, float u) {
    switch (type) {
    case 0: return soft(u);
    case 1: return tube(u);
    case 2: return hard(u);
    case 3: return fold(u);
    default: return sine(u);
    }
}

// The crusher's converter: clips to +-1, rounds to the nearest multiple of q (invQ = 1 / q).
inline float quantize(float x, float q, float invQ) {
    return q * floorSmall(clampf(x, -1.0f, 1.0f) * invQ + 0.5f);
}

} // namespace drv

class Drive {
public:
    enum Type { Soft, Tube, Hard, Fold, Sine, Crush };
    static constexpr int kNumTypes = 6;

    struct Params {
        int type = Soft;        // Type: 0 Soft, 1 Tube, 2 Hard, 3 Fold, 4 Sine, 5 Crush
        float driveDb = 12.0f;  // 0..36 dB of gain into the shaper (Crush: how far it crushes)
        float tone = 0.0f;      // -1..1 tilt after the shaper: -1 dark, 0 flat, +1 bright
        float bias = 0.0f;      // 0..1 asymmetry (adds even harmonics)
        float outDb = 0.0f;     // -24..+12 dB output trim
        float mix = 1.0f;       // 0..1 dry / wet
    };

    // The DC blocker's step falls 60 dB in 4850 samples (110 ms); the halfband's ring and the
    // crusher's hold add the rest.
    static constexpr int kTail = 5100;

    Drive();
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return mix_.target() > 0.0f ? kTail : 0; }

private:
    struct Channel {
        float dcIn = 0.0f, dcOut = 0.0f;    // DC blocker
        float split = 0.0f;                 // the tilt's one-pole (TPT) state
        float held = 0.0f, phase = 1.0f;    // crusher: the held sample; >= 1 takes the next one
    };
    // One chunk's per-sample controls, shared by both channels.
    struct Controls {
        float drive[kChunk], offset[kChunk], comp[kChunk], rest[kChunk];   // rest: shaper(offset)
        float oldComp[kChunk], oldRest[kChunk], fade[kChunk];              // the old type's, while fading
        float direct[kChunk], low[kChunk], mix[kChunk];                    // tilt and Out: direct x wet + low x low band
        float filtered[kChunk];                                            // the dry: 0 raw, 1 through the halfbands
    };
    static constexpr int kHistory = 256;   // the input the dry halfbands re-run when they start again (a power of 2)

    void processChunk(float* L, float* R, int n);
    void oversampled(const float (*dry)[kChunk], float (*out)[kChunk], const Controls& k, int n, bool outgoing);
    void crush(const float* dry, float* out, Channel& c, int n) const;
    void finish(float* io, Channel& c, const float* dry, const float* wet, const Controls& k, int n);
    void filteredDry(const float (*dry)[kChunk], float (*out)[kChunk], int n);
    void warmUp();
    void remember(const float (*dry)[kChunk], int n);
    void resetChannels();

    const float* table_;    // the compensation table (drive.cpp), shared by every instance
    bool fresh_ = true, fading_ = false;
    int type_ = Soft, oldType_ = Soft;
    Ramp driveDb_, bias_, offset_, rest_, direct_, low_, mix_, filtered_;
    float drive_ = 1.0f, comp_ = 1.0f;          // at the targets: while drive and bias stand still
    float lastComp_ = 1.0f;                     // the compensation the last chunk ended at
    float oldComp_ = 1.0f, oldRest_ = 0.0f;     // the old type's, while fading
    float step_ = 1.0f / 32768.0f, invStep_ = 32768.0f, crushOffset_ = 0.0f, holdRate_ = 1.0f;   // crusher
    StereoInterpolator up_, dryUp_;
    StereoDecimator down_, dryDown_;
    bool dryRunning_ = false;                   // the dry halfbands ran last chunk
    float history_[2][kHistory] = {};           // the input, a ring
    int historyPos_ = 0;
    Channel ch_[2];
};

} // namespace ef
