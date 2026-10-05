#pragma once
// What every module shares: MPC's rate, the control chunk, the transport, sync divisions,
// smoothing, interpolation and a delay line. The module contract is in docs/DESIGN.md, "Modules":
//
//   Module();                                       allocates (UI thread)
//   void reset();                                   clears all state; the next set() jumps to its targets
//   void set(const Params& p, const Transport& t);  once per chunk, before process()
//   void process(float* L, float* R, int n);        in place, 1 <= n <= kChunk
//   int  tailSamples() const;                       how long it rings after the input stops (0: none)
//
// No allocation, locks or exceptions in set / process; finite output for any finite input and any
// parameters; a NaN or infinity in the input never gets into a feedback path (sanitize()).
#include "fastmath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ef {

constexpr float kRate = 44100.0f;   // MPC OS always runs 44.1 kHz
constexpr int kChunk = 32;          // control rate: set() once per chunk of at most this many samples

// MPC's transport at the chunk's first sample.
struct Transport {
    double bpm = 120.0;     // always usable: 120 when MPC doesn't say
    double beats = 0.0;     // song position in quarter notes (valid only if `valid`)
    bool playing = false;
    bool valid = false;
};

// Sync divisions in quarter-note beats, shortest first (a Q-Link turn walks them in order). The
// names are the options surface.py shows, same order.
struct Division {
    const char* name;
    double beats;
};
inline constexpr Division kDelayDivs[] = {
    {"1/64", 0.0625}, {"1/32T", 1.0 / 12}, {"1/32", 0.125}, {"1/16T", 1.0 / 6}, {"1/16", 0.25}, {"1/8T", 1.0 / 3},
    {"1/16.", 0.375}, {"1/8", 0.5}, {"1/4T", 2.0 / 3}, {"1/8.", 0.75}, {"1/4", 1.0}, {"1/2T", 4.0 / 3},
    {"1/4.", 1.5}, {"1/2", 2.0}, {"1/2.", 3.0}, {"1 bar", 4.0},
};
inline constexpr int kNumDelayDivs = static_cast<int>(sizeof kDelayDivs / sizeof kDelayDivs[0]);
inline constexpr Division kLfoDivs[] = {
    {"1/16", 0.25}, {"1/8T", 1.0 / 3}, {"1/8", 0.5}, {"1/4T", 2.0 / 3}, {"1/8.", 0.75}, {"1/4", 1.0},
    {"1/4.", 1.5}, {"1/2", 2.0}, {"1/2.", 3.0}, {"1 bar", 4.0}, {"2 bars", 8.0}, {"4 bars", 16.0},
    {"8 bars", 32.0}, {"16 bars", 64.0},
};
inline constexpr int kNumLfoDivs = static_cast<int>(sizeof kLfoDivs / sizeof kLfoDivs[0]);

inline double divSeconds(double beats, double bpm) { return beats * 60.0 / bpm; }

// A synced LFO's phase (0..1) from MPC's song position: the LFO stays on the beat whatever happens
// between (loops, jumps, tempo changes). `offset` in cycles.
inline double lockedPhase(const Transport& t, double periodBeats, double offset = 0.0) {
    const double c = t.beats / periodBeats + offset;
    return c - std::floor(c);
}

// --- levels -----------------------------------------------------------------------------------

inline float dbToGain(float db) { return exp2Fast(db * 0.166096404744f); }   // log2(10) / 20
inline float gainToDb(float g) { return g > 1e-10f ? 6.02059991328f * log2Fast(g) : -200.0f; }
inline float sanitize(float x) { return std::isfinite(x) ? x : 0.0f; }

// --- smoothing --------------------------------------------------------------------------------

// The per-sample coefficient of a one-pole smoother with time constant `seconds`.
inline float smoothCoef(float seconds) { return 1.0f - std::exp(-1.0f / (seconds * kRate)); }

// A value that moves in a straight line to its target over one chunk: set() calls to(), process()
// calls next() per sample. jump() for the first set() after reset().
class Ramp {
public:
    void jump(float v) {
        cur_ = target_ = v;
        step_ = 0.0f;
        left_ = 0;
    }
    void to(float target, int n) {
        target_ = target;
        left_ = std::max(n, 1);
        step_ = (target_ - cur_) / static_cast<float>(left_);
    }
    float next() {
        if (left_ > 0) {
            cur_ += step_;
            if (--left_ == 0) cur_ = target_;
        }
        return cur_;
    }
    float value() const { return cur_; }
    float target() const { return target_; }

private:
    float cur_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
    int left_ = 0;
};

// --- interpolation and delay lines --------------------------------------------------------------

// 4-point, 3rd-order Hermite between x0 (t = 0) and x1 (t = 1).
inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// A mono delay line, power-of-two sized: write one sample, read any number of taps behind it.
// Delays are in samples, measured from the sample just written (delay 1 = the previous one).
class DelayLine {
public:
    explicit DelayLine(int minSize = 1) { resize(minSize); }

    void resize(int minSize) {   // allocates: UI thread only
        int n = 1;
        while (n < minSize + 4) n <<= 1;
        buf_.assign(static_cast<size_t>(n), 0.0f);
        mask_ = n - 1;
        w_ = 0;
    }
    int capacity() const { return mask_ - 3; }   // the longest delay a cubic read can reach
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.0f); }

    void write(float x) {
        w_ = (w_ + 1) & mask_;
        buf_[static_cast<size_t>(w_)] = x;
    }
    float at(int delay) const { return buf_[static_cast<size_t>((w_ - delay) & mask_)]; }
    float readLinear(float delay) const {
        const int i = static_cast<int>(delay);
        const float f = delay - static_cast<float>(i);
        const float a = at(i), b = at(i + 1);
        return a + (b - a) * f;
    }
    float readCubic(float delay) const {   // delay >= 1
        const int i = static_cast<int>(delay);
        const float f = delay - static_cast<float>(i);
        return hermite(at(i - 1), at(i), at(i + 1), at(i + 2), f);
    }

private:
    std::vector<float> buf_;
    int mask_ = 0, w_ = 0;
};

} // namespace ef
