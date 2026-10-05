#pragma once
// Filter (docs/DESIGN.md, "Modules"): a resonant stereo filter on dsp/svf.h, soft drive before it,
// the cutoffs of L and R spread apart, a dry / wet mix.
//
// Types: one SVF for LP 12, HP 12, BP and Notch; two in series for LP 24 and HP 24. The second
// stage always runs (a plain pass-through for the 12 dB types) so a switch to 24 dB finds its
// state ready; with the outputs gliding, a type change is a crossfade over one chunk.
//
// Resonance: res 0..1 sets Q = 0.707 * 22.6^res, 0.707 (Butterworth, no peak) to 16 (+24 dB at the
// cutoff), exponential so the knob's travel is even to the ear. The 24 dB types keep a
// Butterworth first stage (Q 0.541) and give the second Q / 0.541 (1.307 at res 0): the two
// stages' gains at the cutoff multiply to Q, so a res setting peaks as high in either slope.
// BP is normalized (0 dB at its peak, narrower with res), Notch narrows with res.
//
// Drive: up to +24 dB into fastmath.h's softclip, the output down by half as many dB (a -12 dBFS
// peak comes out about as loud as it went in). Under 1/8 of the range it blends in from clean, so
// drive 0 is exactly linear (and skipped).
#include "svf.h"

namespace ef {

class Filter {
public:
    enum Type : int { LP12, LP24, HP12, HP24, BP, NOTCH, kTypes };

    struct Params {
        int type = LP12;
        float cutoffHz = 1000.0f;   // 20..20000 (each side's cutoff stays under 0.45 of the rate)
        float res = 0.2f;           // 0..1: Butterworth .. +24 dB peak
        float drive = 0.0f;         // 0..1: 0 = linear, 1 = +24 dB into the saturator
        float spread = 0.0f;        // -1..1 octaves: L's cutoff x 2^(-spread / 2), R's x 2^(spread / 2)
        float mix = 1.0f;           // 0..1 dry / wet
    };

    Filter();
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return tail_; }

private:
    // What glides: g (L and R, in octaves), both stages' k and output mix, the drive's gains, the mix.
    enum : int { G, K1, K2, M1, M2 = M1 + 3, DRV_IN = M2 + 3, DRV_OUT, DRV_BLEND, MIX, kValues };

    template <bool Drive>
    void run(float* L, float* R, int n);

    Glide<kValues, 1> gl_;
    SvfUpdate a1_ = {}, a2_ = {};   // the stages' updates at the glide's targets
    SvfState s1_, s2_;
    Params last_;
    bool fresh_ = true;
    int tail_ = 0;
};

} // namespace ef
