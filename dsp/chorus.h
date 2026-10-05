#pragma once
// Chorus (docs/DESIGN.md, "Modules"): modulated delay lines with cubic interpolation, in three
// characters.
//
//   Chorus     Two voices per side on a sine LFO: a side's two in antiphase, the right pair a quarter
//              cycle from the left's. Up to +-4 ms around the base delay.
//   Ensemble   Three voices per side, 120 degrees apart, each moved by the slow LFO (the rate, up to
//              +-3 ms) plus a fast one at 6x the rate (up to +-0.5 ms, the same pitch swing): a string
//              machine's shimmer.
//   Dimension  Juno-60 style: one voice per side on triangle LFOs in antiphase, each side with 20% of
//              the other's voice. Up to +-2 ms (the Juno's own is about +-1.85 ms).
//
// Each input channel has its own line, so a stereo input stays stereo. Width turns the right side's
// LFO phases from the left's (0: the same, so a mono input gives L == R) to the spread above. The
// wet's low cut (12 dB/oct Butterworth, ahead of the lines) keeps the lows dry and centred.
//
// Levels: a side's voices sum at constant power (1 / sqrt(voices)) and dry / wet cross-fade at
// constant power, so the level holds where the voices are decorrelated (everywhere but the lows).
// At depth 0 the voices coincide: one echo at exactly the base delay, sqrt(voices) high.
// At short base delays the depth is held back so no read comes closer than one sample.
#include "common.h"

#include <cstdint>
#include <vector>

namespace ef {

class Chorus {
public:
    enum Mode { kChorus, kEnsemble, kDimension };

    struct Params {
        int mode = kChorus;        // 0 Chorus, 1 Ensemble, 2 Dimension
        float rateHz = 0.5f;       // 0.03..10
        float depth = 0.5f;        // 0..1
        float delayMs = 12.0f;     // 1..40 base delay
        float lowCutHz = 120.0f;   // 20..1000: high-pass on the wet only; <= 20 = off
        float width = 1.0f;        // 0..1 stereo spread of the voices
        float mix = 0.5f;          // 0..1 dry / wet
    };

    // Modulation at depth 1, +- ms around the base delay.
    static constexpr float kChorusMs = 4.0f, kEnsembleSlowMs = 3.0f, kEnsembleFastMs = 0.5f, kDimensionMs = 2.0f;
    static constexpr float kFastRatio = 6.0f;   // Ensemble: the fast LFO's rate over the rate
    static constexpr float kCross = 0.2f;       // Dimension: the other side's share of a side's wet
    static constexpr int kLine = 2048;          // per channel: 40 ms + 4 ms of modulation, a chunk and the taps

    Chorus();
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const;

private:
    static constexpr int kStride = kLine + 4;   // a line, then its first three samples again

    template <int M>
    void run(float* L, float* R, int n);
    void writeLines(const float* L, const float* R, int n);
    float voiceTargets(f4 (&t)[2][2]) const;

    std::vector<float> buf_;   // the left line, then the right one
    int w_ = 0;

    // The wet's low cut: state per side; its g = tan(pi f / rate) glides per chunk in octaves, the
    // update's coefficients a1..a3 follow in straight lines across the chunk (between filters that
    // close, the straight line stays one); `cut` fades it in and out (<= 20 Hz is off).
    float ic1l_ = 0.0f, ic2l_ = 0.0f, ic1r_ = 0.0f, ic2r_ = 0.0f;
    float gGlide_ = 0.0f;
    float a_[3] = {}, aStep_[3] = {}, aTarget_[3] = {};
    float cut_ = 0.0f, cutTarget_ = 0.0f, cutStep_ = 0.0f;

    // LFOs: sine and cosine of the slow and fast ones (magic circles), the triangle's phase.
    float s_ = 0.0f, c_ = 1.0f, sf_ = 0.0f, cf_ = 1.0f, e_ = 0.0f, ef_ = 0.0f;
    uint32_t acc_ = 0, inc_ = 0;

    // Base delay, depth, width and mix glide per chunk. The base delay and the voices' coefficients
    // (two vectors per group of four voices, see run()) follow in straight lines across it.
    float baseGlide_ = 0.0f, depthGlide_ = 0.0f, widthGlide_ = 0.0f, mixGlide_[2] = {};
    float gate_ = 1.0f;   // the wet's share while a mode change fades it out and back in
    float delay_ = 0.0f, delayStep_ = 0.0f, delayTarget_ = 0.0f;
    f4 coef_[2][2] = {}, coefStep_[2][2] = {}, coefTarget_[2][2] = {};

    // Dry and wet gains (constant power).
    float dry_ = 1.0f, wet_ = 0.0f, dryStep_ = 0.0f, wetStep_ = 0.0f, dryTarget_ = 1.0f, wetTarget_ = 0.0f;

    int left_ = 0;   // samples until the ramps reach their targets
    int tail_ = 0;
    int mode_ = kChorus;
    bool fresh_ = true;
};

} // namespace ef
