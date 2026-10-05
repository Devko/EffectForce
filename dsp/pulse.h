#pragma once
// Pulse (docs/DESIGN.md, "Modules"): a synced tremolo, an auto-pan and a rhythmic (trance) gate.
// Every mode is a gain per side and sample, so nothing depends on the input's past and nothing
// rings on (tailSamples() is 0).
//
// Time: one phase runs over 16 periods of the base rate: the LFO's cycle (Tremolo, Auto-Pan) or
// the gate's step. Free, the base rate is `rateHz`; synced, one period is `divBeats` at MPC's
// tempo, and while MPC plays the phase follows the song position (lockedPhase over 16 periods), so
// the LFO's cycles and the gate's steps sit on the beat grid and the gate's step 0 falls on beat
// multiples of 16 divisions (the bar's downbeat for 1/16 steps). A small slip is caught up an
// eighth per chunk; a jump (a locate, a loop that isn't a whole number of 16 periods, a new
// division) cross-fades from the old phase to the new one over kFadeSamples (5.8 ms) instead of
// stepping. Stopped, it runs on at the tempo's rate.
//
// Tremolo and Auto-Pan: the LFO u (-1..1) starts each cycle at 0 rising, so the first half-cycle is
// the upper one. `shape` morphs it from a sine (0) through a triangle (0.5) to a square (1):
//   tri = the triangle: 0, 1, 0, -1 at phase 0, 1/4, 1/2, 3/4
//   c   = clamp(k tri, -1, 1), k = 1 up to shape 0.5, then kMax^(2 shape - 1),
//         kMax = period / (2 kSquareEdgeMs) (at least 2): the square's edges take 4 ms
//   u   = c + |1 - 2 shape| (sin(pi/2 c) - c)
// so the square's edges are half a sine wave (no corners), whatever the rate.
//   Tremolo: gain = 1 - depth (1 - u) / 2, linear in amplitude: the peaks stay at the dry level
//   (it never boosts) and every shape is symmetric about the middle, so the average gain is
//   1 - depth / 2 for every shape. The right side's LFO runs `stereo` degrees ahead of the left's.
//   Auto-Pan: a constant-power balance, p = depth u (-1 left .. 1 right), angle
//   theta = pi/4 (1 + p), gains sqrt2 cos(theta) (left) and sqrt2 sin(theta) (right): unity in the
//   middle, +3 dB on the side it pans to, L^2 + R^2 the input's power throughout, so a centred
//   source keeps its level as it sweeps. Depth is the width; depth 0 holds it in the middle.
//
// Gate: 16 steps, a 16-bit mask per pattern (kPatternSteps). An open step opens for `length` of
// the step: its gain rises from closed at the step's start and is back at closed exactly at
// `length` of the step. Both ramps are an S-curve (smoothstep) over the edge time
//   edge = smooth + shape max(0, length step / 2 - smooth)
// so here `shape` runs the other way, as softness: 0 gives `smooth` edges (a hard gate), 1 a swell
// that fills the open part (a raised-cosine pulse). Where the open part is shorter than two edges,
// the ramps meet halfway at a lower peak: no edge is ever faster than `smooth`. At length 1 open
// steps in a row run into each other without a dip. Closed is 1 - depth (depth 0.5: half closed).
// The ramps are a slope limit on the gate's state, so a pattern change (it takes effect at the
// next step boundary), a new length or edge time, or a phase jump never steps the gain.
//
// Mix blends dry and wet gains: gain' = 1 + mix (gain - 1); mix 0 is the input exactly. A mode
// change cross-fades from the old mode's gains to the new one's over kFadeSamples, the old one
// running on at its own rate and shape. Depth and mix glide over about 10 ms; the shape and the
// rate (free, or synced and stopped) over 20 ms, the square's k following the rate so its edges
// keep their 4 ms; stereo over 20 ms, but never so fast that it steepens the square's edges more
// than twofold (a half-cycle shift takes at least half a cycle with the square).
#include "common.h"

#include <cstdint>

namespace ef {

class Pulse {
public:
    enum Mode { kTremolo, kAutoPan, kGate };

    struct Params {
        int mode = kTremolo;     // 0 Tremolo, 1 Auto-Pan, 2 Gate
        bool sync = true;
        float rateHz = 4.0f;     // 0.1..20 when free: the LFO's rate (Tremolo, Auto-Pan), the step rate (Gate)
        double divBeats = 0.25;  // synced: the LFO's period or the gate's step, quarter-note beats (1/64 note .. 16 bars)
        float depth = 1.0f;      // 0..1 (Auto-Pan: the width)
        float shape = 0.0f;      // 0..1: sine, triangle (0.5), square (1); Gate: hard (0) to soft (1) edges
        float stereo = 0.0f;     // 0..180 degrees: Tremolo's right side ahead of its left
        int pattern = 0;         // Gate: kPatternNames
        float length = 0.5f;     // Gate: the open part of an open step, 0.05..1
        float smooth = 3.0f;     // Gate: the shortest edge, 0.5..50 ms
        float mix = 1.0f;        // 0..1 dry / wet
    };

    // The gate's patterns: step 0 first, 'x' open. Step 0 is on the downbeat.
    static constexpr int kPatterns = 16;
    static constexpr const char* kPatternNames[] = {
        "1/16", "1/8", "1/4", "Offbeat", "Off 16ths", "Dotted", "Tresillo", "Gallop",
        "Rev Gallop", "Trance 1", "Trance 2", "Trance 3", "Pump", "Stutter", "Half Bar", "Build",
    };
    static constexpr const char* kPatternSteps[] = {
        "xxxxxxxxxxxxxxxx",   // 1/16: every step (with length < 1, a 16th chop)
        "x.x.x.x.x.x.x.x.",   // 1/8
        "x...x...x...x...",   // 1/4: the beats
        "..x...x...x...x.",   // Offbeat: the eighths between the beats (the trance bass's)
        ".x.x.x.x.x.x.x.x",   // Off 16ths: the sixteenths between the eighths
        "x..x..x..x..x..x",   // Dotted: dotted eighths across the bar
        "x..x..x.x..x..x.",   // Tresillo: 3-3-2, twice
        "x.xxx.xxx.xxx.xx",   // Gallop: an eighth and two sixteenths
        "xxx.xxx.xxx.xxx.",   // Rev Gallop: two sixteenths and an eighth
        "x.xx.xx.x.xx.xx.",   // Trance 1
        "xx.xx.x.xx.xx.x.",   // Trance 2
        "x.xxx.x.x.xxx.x.",   // Trance 3
        ".xxx.xxx.xxx.xxx",   // Pump: closed on every beat, a sidechain duck
        "x.x.x.x.xxxxxxxx",   // Stutter: eighths, then sixteenths
        "xxxxxxxx........",   // Half Bar: open for the first half
        "x...x...x.x.xxxx",   // Build: quarters, eighths, sixteenths
    };

    static constexpr float kMinRateHz = 0.1f, kMaxRateHz = 20.0f;
    static constexpr double kMinDivBeats = 0.0625, kMaxDivBeats = 64.0;
    static constexpr float kMinLength = 0.05f, kMinSmoothMs = 0.5f, kMaxSmoothMs = 50.0f;
    static constexpr float kSquareEdgeMs = 4.0f;   // Tremolo and Auto-Pan's square: a full swing
    static constexpr int kFadeSamples = 8 * kChunk;   // mode changes and phase jumps

    Pulse();
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return 0; }

private:
    struct Voice {
        uint64_t acc = 0;       // the phase over 16 periods, 2^64 to a turn: the top 4 bits are the step
        uint64_t inc = 0;       // fading out: its phase per sample
        float k = 1.0f;         // fading out: its square's k
        int mode = kTremolo;
        float env = 0.0f;       // Gate: 0 closed .. 1 open, before the S-curve
        uint32_t step = 0;      // Gate: the step `mask` belongs to
        uint32_t mask = 0;      // Gate: the pattern playing
        uint32_t pending = 0;   // Gate: the pattern from the next step boundary on
    };
    struct Lin {   // a value moving in a straight line to its target over a chunk
        float cur = 0.0f, step = 0.0f, target = 0.0f;
    };

    void gains(Voice& v, bool fading, float* gl, float* gr, int n);
    void lfoGains(Voice& v, uint64_t inc, float k0, float kStep, float* gl, float* gr, int n);
    void gateGains(Voice& v, uint64_t inc, float* gl, float* gr, int n);
    bool gateOpen(const Voice& v) const;
    void startGate(Voice& v) const;

    Voice cur_, old_;            // old_: the one fading out
    int fade_ = kFadeSamples;    // samples into the cross-fade (kFadeSamples: none)
    uint64_t inc_ = 0;           // phase per sample
    // What period_ and inc_ were worked out from.
    struct PeriodKey {
        bool sync = false;
        double div = -1.0, bpm = -1.0;
        float rate = -1.0f;
    } periodKey_;
    double period_ = 1.0, incDen_ = -1.0;
    float logPeriod_ = 0.0f;     // log2 of the period in samples, gliding

    // The gate's ramps: the slope per sample, where in a step (2^28 to a step) an open step's gate
    // starts closing, and whether open steps in a row join.
    float slew_ = 1.0f;
    uint32_t gateOff_ = 0;
    bool legato_ = false;

    // Depth, mix, the square's k, the sine's weight and the right side's offset (cycles): each
    // glides per chunk, then follows in a straight line across the chunk.
    Lin depth_, mix_, k_, w_, off_;
    float depthGlide_[2] = {}, mixGlide_[2] = {}, shapeGlide_ = 0.0f, offGlide_ = 0.0f;
    int left_ = 0;   // samples until the ramps reach their targets
    bool fresh_ = true;

    alignas(16) float gl_[kChunk] = {}, gr_[kChunk] = {}, ol_[kChunk] = {}, or_[kChunk] = {};
};

static_assert(sizeof Pulse::kPatternNames / sizeof Pulse::kPatternNames[0] == Pulse::kPatterns, "a name per pattern");
static_assert(sizeof Pulse::kPatternSteps / sizeof Pulse::kPatternSteps[0] == Pulse::kPatterns, "steps per pattern");

} // namespace ef
