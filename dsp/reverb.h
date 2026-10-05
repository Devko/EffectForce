#pragma once
// Reverb (docs/DESIGN.md, "Modules"): the flagship, so it spends its CPU on a smooth tail.
//
//   predelay -> low cut -> 4 series allpasses per side -> into 8 lines (L the even, R the odd)
//   each line: modulated read (8-point Lagrange) -> allpass -> loop gain and damping
//   -> 8x8 Hadamard -> rotated by one line -> written back with the input
//   out: two Hadamard rows (orthogonal sign patterns over all 8 lines) -> width (mid / side) -> mix
//
// Modes choose the lines' base lengths (primes spread 1:3 geometrically), the diffusion and the
// modulation: Room 10..30 ms lines, dense and quick; Hall 30..92 ms, smooth, a little modulation;
// Plate 9..28 ms with the strongest input diffusion, the fastest build-up, bright; Space
// 75..250 ms with the most modulation. A mode change fades the wet out (12 ms), starts the
// network afresh in the new mode and fades it in: a program change, never a click.
//
// Size scales the lines 0.5x..1.5x of the mode's (linearly; 0.5 = the mode's own lengths). The
// diffusers belong to the mode and don't scale. A size change glides (80 ms, and no line's
// length moves faster than 0.1 sample per sample), so a tail in flight bends in pitch a little.
// The input's level follows the network's length, so a tail of one decay time is equally loud
// in every mode and size.
//
// Decay (Jot): each line's gain comes from its loop delay (line + allpass), so every path through
// the network loses the same dB per second: RT60 = decayS at low and mid frequencies. Damping: a
// one-pole low-pass per line, also set from its delay, so the decay rate grows as
// 1 + (f / dampHz)^2 at low and mid frequencies (the decay time halves at dampHz and keeps
// shortening above, the way air absorbs highs), flattening toward Nyquist.
//
// Modulation: one sine per line (rates 0.67..1.46x of the mode's, never in step) moves the read
// position by up to the mode's depth times `mod`. The Lagrange interpolator never boosts
// (|H| <= 1 at every fraction) and loses under 0.02 dB a pass below 8 kHz.
//
// Freeze: the loop gains glide to 1 (to float precision: 1 - 2e-8), the damping out and the input
// fades (46 ms); leaving it glides back to the normal decay.
//
// Shimmer: a pitch shifter (dsp/pitch.h) on one direction of the feedback, the Hadamard's row 3:
// that component c goes back as cos(a) c + sin(a) (P(c) - b c), a = shimmer x 90 degrees, P the
// shifter, b c the part of P(c) correlated with c (measured over 50 ms) taken out, so the two
// terms add in power, never more. Every pass an eighth of the tail's energy moves up (or down) by
// the interval, so the tail blooms. The shifter never adds energy and the rest of the loop is as
// before, so the network stays as bounded as without it, frozen too (there, what climbs past the
// shimmer path's low-pass, or under its high-pass, leaves: a frozen sound slowly thins). The path:
// a high-pass at 80 Hz and a 24 dB/oct low-pass under rate / 3 / ratio (9 kHz at most), so +19
// doesn't fold. The angle glides (20 ms); an interval change fades the path out (20 ms), switches
// and fades it back in. Shimmer 0 (settled) doesn't run the shifter: the reverb is bit for bit the
// one without it.
#include "common.h"
#include "pitch.h"
#include "simd.h"

#include <cstdint>
#include <vector>

namespace ef {

class Reverb {
public:
    enum Mode : int { ROOM, HALL, PLATE, SPACE, kModes };
    static constexpr int kLines = 8;

    struct Params {
        int mode = HALL;            // Room, Hall, Plate, Space
        float size = 0.5f;          // 0..1: the lines 0.5x..1.5x the mode's
        float decayS = 2.5f;        // 0.1..30: RT60 at low and mid frequencies
        float predelayMs = 20.0f;   // 0..250
        float dampHz = 6000.0f;     // 1000..20000: the decay time halves here and keeps falling above
        float lowCutHz = 150.0f;    // 20..1000: high-pass (12 dB/oct) on the wet input; <= 20: off
        float mod = 0.3f;           // 0..1: depth of the lines' modulation
        float width = 1.0f;         // 0..1: stereo width of the wet (0 = mono wet)
        bool freeze = false;        // endless decay, input muted
        float mix = 0.3f;           // 0..1 dry / wet
        float shimmer = 0.0f;       // 0..1: how much of the feedback's pitched path circulates
        int shimmerInterval = 0;    // 0: +12, 1: +7, 2: +19, 3: -12 semitones
    };
    enum Interval : int { UP_OCTAVE, UP_FIFTH, UP_TWELFTH, DOWN_OCTAVE, kIntervals };

    Reverb();
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const;

    // Whether the last chunk's loop gains or damping moved (the slower network loop), and whether
    // the shimmer's shifter ran: for tests.
    bool gliding() const { return glide_; }
    bool shimmering() const { return shimOn_; }

private:
    enum Phase : int { RUN, FADE_OUT, FADE_IN };

    // The wet input's path before the network: predelay and low cut. A chunk runs it from a local
    // copy, in registers (as members, every store into a buffer would make the compiler reload them).
    struct InputPath {
        float* buf = nullptr;                  // the predelay, L and R interleaved
        uint32_t w = 0;
        int tapA = 0, tapB = 0, fade = 0;      // its taps (samples), the crossfade between them
        float ic1L = 0.0f, ic2L = 0.0f, ic1R = 0.0f, ic2R = 0.0f;   // the low cut's state
        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;                       // its update while g is still
        Ramp g;                                // its g while it glides
        bool moving = false;
        uint32_t age = 0;      // samples written since reset (up to the buffer's size)
        float tiny = 1e-20f;   // flips sign every sample: keeps decaying states out of the denormals

        void tick(float xl, float xr, float& ol, float& or_);
    };

    // The shimmer path's filters before the shifter: a one-pole high-pass, two 2-pole low-passes.
    struct ShimmerFilter {
        float hp = 0.0f, ic1a = 0.0f, ic2a = 0.0f, ic1b = 0.0f, ic2b = 0.0f;   // state
        float hpA = 0.0f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;                       // coefficients

        void clear() { hp = ic1a = ic2a = ic1b = ic2b = 0.0f; }
        float tick(float x);
    };

    void loadMode(int m);
    void forget();
    void setInterval(int interval);
    void shimmerAfresh();
    void chunkSetup(int n);
    void nextSegment();
    template <bool Glide, bool Young, bool Shimmer>
    void runNetwork(float* L, float* R, int n);

    // The network's memory, one block: the lines (each a power of two plus a guard copy of its
    // first samples, so an 8-point read never wraps), the in-loop allpasses and the input
    // diffusers (both interleaved, 8 per index).
    std::vector<float> pool_;
    float* line_[kLines] = {};
    uint32_t mask_[kLines] = {};
    float* ap_ = nullptr;
    float* diff_ = nullptr;
    uint32_t w_ = 0;                // write counter shared by every buffer (each masks it)
    uint32_t age_ = 0, youngEnd_ = 0;   // samples since the network started afresh, and how many make it whole
    bool young_ = true;                 // this segment masks reads from before then (runNetwork's Young)

    // The active mode.
    int mode_ = HALL;
    f4 base_[2] = {};               // line lengths at scale 1, samples
    uint32_t apLen_[kLines] = {}, diffLen_[kLines] = {};
    float apG_ = 0.0f, diffG_[2] = {}, maxBase_ = 1.0f, meanBase_ = 1.0f, meanAp_ = 0.0f, build_ = 0.0f;

    // The read positions: straight lines between points kSegment samples apart (counted from
    // reset), where the sines and the size are evaluated.
    f4 lfoS_[2] = {}, lfoC_[2] = {}, lfoE_[2] = {};   // the lines' sines: a magic circle, a segment a step
    f4 lfoStartS_[2] = {}, lfoStartC_[2] = {};         // ... where they start
    f4 pos_[2] = {}, posStep_[2] = {}, posEnd_[2] = {};
    int segLeft_ = 0;
    float scale_ = 1.0f, depth_ = 0.0f;   // the size's scale (gliding), the modulation depth in samples

    // Loop gains (with the Hadamard's 1 / sqrt(8)) and damping poles, gliding across a chunk.
    f4 g_[2] = {}, gTgt_[2] = {}, gStep_[2] = {};
    f4 pole_[2] = {}, poleTgt_[2] = {}, poleStep_[2] = {};
    f4 lp_[2] = {};                 // the damping filters' state
    float coefS_ = 0.0f, coefDecay_ = 0.0f, coefDamp_ = 0.0f, coefFz_ = 0.0f;   // what the targets are for

    std::vector<float> preBuf_;
    InputPath input_;
    float lcOct_ = 0.0f, lcHz_ = 0.0f, lcG_ = 0.0f;   // the low cut: log2 Hz now, where it is going, its g

    Ramp in_, dry_, wet_, width_;
    float fz_ = 0.0f, gate_ = 1.0f;   // freeze amount, the mode change's gate
    Phase phase_ = RUN;

    // Shimmer.
    std::vector<float> shimBuf_;
    PitchShift shift_;
    ShimmerFilter shimFilter_;
    Ramp shimCos_, shimSin_;          // what stays, what is pitched
    float shimGate_ = 1.0f, shimAngle_ = 0.0f;   // the interval change's fade; the angle (gliding)
    double shimPP_ = 0.0, shimCC_ = 0.0;           // the pitched path's correlation with what stays, its power
    float shimBeta_ = 0.0f;                        // ... their ratio: how much of it is taken out
    int shimInterval_ = UP_OCTAVE;
    bool shimOn_ = false;             // the shifter runs (shimmer on, or still fading out)

    Params p_;
    bool fresh_ = true, jumpCoefs_ = true, glide_ = false;
};

} // namespace ef
