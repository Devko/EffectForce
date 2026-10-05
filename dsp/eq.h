#pragma once
// EQ (docs/DESIGN.md, "Modules"): low cut, low shelf, bell, high shelf, high cut, in that order,
// each one dsp/svf.h filter with Andrew Simper's mixes ("SvfLinearTrapOptimised2"), A = 10^(dB / 40):
//
//   low cut     12 dB / octave Butterworth high-pass (Q 0.707): 1, -k, -1
//   low shelf   g / sqrt(A), Q 0.707 (no overshoot):  1, k (A - 1), A^2 - 1
//   bell        k = 1 / (Q A):                         1, k (A^2 - 1), 0
//   high shelf  g * sqrt(A), Q 0.707:                  A^2, k (1 - A) A, 1 - A^2
//   high cut    12 dB / octave Butterworth low-pass:  0, 0, 1
//
// The shelves are at half their gain (in dB) at the corner, the bell at its full gain at the
// centre, all exactly: the bilinear transform is prewarped there.
//
// A band at exactly 0 dB, or a cut switched off, is a pass-through (1, 0, 0); once its glide
// there has landed it isn't processed at all, so a flat EQ passes the input through bit for bit
// and costs nothing. Woken again, it starts from a silent state with its mix gliding out of the
// pass-through: no click either way.
//
// Tail: how long the slowest pole of any band that isn't a pass-through rings once the input
// stops. A boosted narrow bell rings longest (its poles' Q is Q A: 22.6 at +18 dB and Q 8, about
// 0.65 s at 100 Hz), a wide deep cut's slow real pole next (0.13 s at 100 Hz, Q 0.3); shelves and
// cuts ring for milliseconds. Flat: 0.
#include "svf.h"

namespace ef {

class Eq {
public:
    struct Params {
        float lowCutHz = 20.0f;                                  // 20..1000; 20 (or less) = off
        float lowFreq = 100.0f, lowGainDb = 0.0f;                // low shelf: 30..500 Hz, -18..+18 dB
        float midFreq = 1000.0f, midGainDb = 0.0f, midQ = 1.0f;  // bell: 100..10000 Hz, -18..+18 dB, Q 0.3..8
        float highFreq = 6000.0f, highGainDb = 0.0f;             // high shelf: 1000..16000 Hz, -18..+18 dB
        float highCutHz = 20000.0f;                              // 1000..20000; 20000 (or more) = off
    };

    Eq();
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return tail_; }

private:
    enum : int { LOW_CUT, LOW_SHELF, BELL, HIGH_SHELF, HIGH_CUT, kBands };
    enum : int { G, K, M, kValues = M + 3 };   // per band: g (gliding in octaves), k, the mix

    struct Band {
        Glide<kValues, 1> gl;
        SvfUpdate a = {};    // at the glide's target
        SvfState s;
        bool on = false;     // processed: not a pass-through that has landed
    };

    static bool passes(const f2* v) { return v[M][0] == 1.0f && v[M + 1][0] == 0.0f && v[M + 2][0] == 0.0f; }

    template <bool Sanitize>
    static void run(Band& b, float* L, float* R, int n);

    Band band_[kBands];
    Params last_;
    bool fresh_ = true;
    int tail_ = 0;
};

} // namespace ef
