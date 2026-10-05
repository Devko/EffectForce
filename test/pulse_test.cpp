// Pulse against theory: the tremolo's envelope is pulse.h's law at the synced period and the free
// rate (depth 1 reaching silence at the trough, stereo 180 alternating the sides); the auto-pan
// keeps L^2 + R^2 at the input's power and puts the image where the pan law says; the gate opens
// and closes on the beat grid as its pattern, length and edges say, lands on the right step after
// a transport jump and switches patterns at the next step. Then the contract: mix 0, no clicks on
// changes and jumps, extremes, random jumps, NaN input, block sizes, reset, tail.
#include "signal.h"
#include "../dsp/pulse.h"

#include <cstdio>

namespace {

using namespace eft;
using namespace ef;
using P = Pulse::Params;

constexpr const char* kModeName[3] = {"Tremolo", "Auto-Pan", "Gate"};

bool same(const Buf& a, const Buf& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i])) return false;
    return true;
}
double maxDiff(const Buf& a, const Buf& b, size_t from = 0, size_t to = 0) {
    if (to == 0) to = a.size();
    double m = 0.0;
    for (size_t i = from; i < to; ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}
void render(const P& p, Buf& L, Buf& R, Transport t = {}, int chunk = kChunk) {
    Pulse m;
    run(m, p, L, R, t, chunk);
}
Transport transport(double bpm, double beats, bool playing) {
    Transport t;
    t.bpm = bpm;
    t.beats = beats;
    t.playing = playing;
    t.valid = true;
    return t;
}

// --- pulse.h's laws in double ------------------------------------------------------------------

// The LFO at phase `ph` (cycles) with a period of `period` samples.
double lfoRef(double ph, double shape, double period) {
    ph -= std::floor(ph);
    const double v = ph + 0.25 - std::floor(ph + 0.25);
    const double tri = 4.0 * std::min(v, 1.0 - v) - 1.0;
    const double kMax = std::max(period / (2.0 * Pulse::kSquareEdgeMs * 1e-3 * kRate), 2.0);
    const double k = shape > 0.5 ? std::pow(kMax, 2.0 * shape - 1.0) : 1.0;
    const double c = std::min(std::max(k * tri, -1.0), 1.0);
    return c + std::fabs(1.0 - 2.0 * shape) * (std::sin(eft::kPi / 2.0 * c) - c);
}
double tremoloRef(double ph, double depth, double shape, double period) { return 1.0 - depth * (1.0 - lfoRef(ph, shape, period)) / 2.0; }
// The auto-pan's angle: 0 left .. pi/2 right.
double panAngle(double ph, double depth, double shape, double period) { return eft::kPi / 4.0 * (1.0 + depth * lfoRef(ph, shape, period)); }

uint32_t maskOf(int pattern) {
    uint32_t m = 0;
    for (int k = 0; k < 16; ++k)
        if (Pulse::kPatternSteps[pattern][k] == 'x') m |= 1u << k;
    return m;
}
bool bit(uint32_t mask, int step) { return (mask >> (step & 15)) & 1u; }
double smoothstep(double e) { return e * e * (3.0 - 2.0 * e); }

// --- tests -------------------------------------------------------------------------------------

void dryAtMixZero() {
    for (int m = 0; m < 3; ++m)
        for (int sync = 0; sync < 2; ++sync) {
            P p;
            p.mode = m;
            p.mix = 0.0f;
            p.sync = sync;
            p.shape = 1.0f;
            p.stereo = 90.0f;
            const Buf inL = whiteNoise(20000, 0.5f, 1), inR = whiteNoise(20000, 0.5f, 2);
            Buf L = inL, R = inR;
            render(p, L, R, transport(120.0, 1.3, sync));
            CHECK(same(L, inL) && same(R, inR));
        }
}

// A sine in quadrature (sin left, cos right) and no stereo offset: sqrt(L^2 + R^2) is the gain
// itself, sample by sample. Against the law at the period: synced and playing from the song
// position, synced and stopped at the tempo's rate from phase 0, free at the rate. Depth 1 with the
// sine reaches silence at the trough, one trough per period.
void tremoloFollowsLfo() {
    struct Case {
        bool sync, playing;
        double bpm, div, beats;
        float rate, shape, depth;
    };
    for (const Case c : {Case{true, true, 128.0, 0.75, 5.3, 0.0f, 0.0f, 1.0f}, Case{true, true, 96.0, 2.0, 17.0, 0.0f, 0.5f, 0.6f},
                         Case{true, false, 140.0, 0.5, 0.0, 0.0f, 0.0f, 1.0f}, Case{false, false, 120.0, 0.25, 0.0, 3.3f, 0.0f, 1.0f},
                         Case{false, true, 120.0, 0.25, 3.0, 2.0f, 1.0f, 1.0f}, Case{false, false, 120.0, 0.25, 0.0, 1.1f, 0.3f, 0.8f},
                         Case{true, true, 110.0, 4.0, 2.0, 0.0f, 0.8f, 0.9f}, Case{false, false, 120.0, 0.25, 0.0, 0.5f, 1.0f, 0.5f}}) {
        P p;
        p.mode = Pulse::kTremolo;
        p.sync = c.sync;
        p.divBeats = c.div;
        p.rateHz = c.rate;
        p.shape = c.shape;
        p.depth = c.depth;
        const int n = 3 * 44100;
        const float amp = 0.5f;
        Buf L = sine(1000.0, n, amp), R = sine(1000.0, n, amp, eft::kPi / 2.0);
        render(p, L, R, transport(c.bpm, c.beats, c.playing));
        const double period = c.sync ? c.div * 60.0 / c.bpm * kRate : kRate / c.rate;
        const double ph0 = c.sync && c.playing ? c.beats / c.div : 0.0;
        double err = 0.0, low = 1.0;
        for (int i = 0; i < n; ++i) {
            const double got = std::hypot(static_cast<double>(L[i]), static_cast<double>(R[i])) / amp;
            err = std::max(err, std::fabs(got - tremoloRef(ph0 + i / period, c.depth, c.shape, period)));
            low = std::min(low, got);
        }
        CHECK(err < 1e-4);
        if (!(err < 1e-4)) std::printf("  tremolo sync %d play %d shape %.1f: off the law by %g\n", c.sync, c.playing, c.shape, err);
        if (c.shape == 0.0f && c.depth == 1.0f) {
            CHECK(low < 1e-4);   // -80 dB
            // The envelope's troughs a period apart: the middles of its stretches under 1% (where it
            // falls and rises steeply; the bottom itself is too flat to place to a sample).
            std::vector<double> mid;
            int first = -1;
            for (int i = 0; i < n; ++i) {
                const bool under = std::hypot(static_cast<double>(L[i]), static_cast<double>(R[i])) / amp < 0.01;
                if (under && first < 0) first = i;
                if (!under && first >= 0) {
                    if (first > 0) mid.push_back(0.5 * (first + i - 1));
                    first = -1;
                }
            }
            bool spaced = mid.size() >= 3;
            for (size_t k = 1; k < mid.size(); ++k) spaced = spaced && std::fabs(mid[k] - mid[k - 1] - period) <= 1.0;
            CHECK(spaced);
        }
    }
}

// Stereo: the right side's LFO runs ahead by `stereo` degrees. At 180 the sides' troughs
// alternate: each cycle's quietest moment on the left (phase 3/4) is the right's loudest.
void tremoloStereo() {
    for (const float deg : {180.0f, 90.0f}) {
        P p;
        p.mode = Pulse::kTremolo;
        p.sync = false;
        p.rateHz = 2.0f;
        p.stereo = deg;
        const int n = 2 * 44100;
        Buf L(n, 0.5f), R(n, 0.5f);
        render(p, L, R);
        const double period = kRate / 2.0;
        double err = 0.0;
        for (int i = 0; i < n; ++i) {
            err = std::max(err, std::fabs(L[i] / 0.5 - tremoloRef(i / period, 1.0, 0.0, period)));
            err = std::max(err, std::fabs(R[i] / 0.5 - tremoloRef(i / period + deg / 360.0, 1.0, 0.0, period)));
        }
        CHECK(err < 1e-4);
        if (deg == 180.0f) {
            bool alternate = true;
            for (int c = 0; c < 4; ++c) {
                const size_t from = static_cast<size_t>(c * period), to = static_cast<size_t>((c + 1) * period);
                const size_t lo = static_cast<size_t>(std::min_element(L.begin() + from, L.begin() + to) - L.begin());
                const size_t ro = static_cast<size_t>(std::min_element(R.begin() + from, R.begin() + to) - R.begin());
                alternate = alternate && std::fabs(static_cast<double>(lo) - (c + 0.75) * period) < 2.0 &&
                            std::fabs(static_cast<double>(ro) - (c + 0.25) * period) < 2.0 && R[lo] > 0.4999f && L[ro] > 0.4999f;
            }
            CHECK(alternate);
        }
    }
}

// Auto-Pan on a centred source (the same sine both sides): L^2 + R^2 is the input's power
// throughout, the angle atan2(R, L) is the law's, and depth 1 reaches both sides. At depth 0 the
// image stays in the middle at unity.
void autoPan() {
    struct Case {
        bool sync;
        double bpm, div, beats;
        float rate, shape, depth;
    };
    for (const Case c : {Case{false, 120.0, 0.25, 0.0, 1.5f, 0.0f, 1.0f}, Case{true, 100.0, 1.0, 6.5, 0.0f, 0.5f, 0.7f},
                         Case{false, 120.0, 0.25, 0.0, 2.0f, 1.0f, 1.0f}, Case{true, 128.0, 0.5, 0.0, 0.0f, 0.25f, 0.0f}}) {
        P p;
        p.mode = Pulse::kAutoPan;
        p.sync = c.sync;
        p.divBeats = c.div;
        p.rateHz = c.rate;
        p.shape = c.shape;
        p.depth = c.depth;
        p.stereo = 120.0f;   // not Auto-Pan's: it is always antiphase
        const int n = 2 * 44100;
        const Buf in = sine(441.0, n, 0.5f);
        Buf L = in, R = in;
        render(p, L, R, transport(c.bpm, c.beats, true));
        const double period = c.sync ? c.div * 60.0 / c.bpm * kRate : kRate / c.rate;
        const double ph0 = c.sync ? c.beats / c.div : 0.0;
        double power = 0.0, angle = 0.0, still = 0.0, leftMost = 1.0, rightMost = 1.0;
        for (int i = 0; i < n; ++i) {
            const double x = in[i];
            if (std::fabs(x) < 0.05) continue;
            const double l = L[i] / x, r = R[i] / x;
            power = std::max(power, std::fabs((l * l + r * r) / 2.0 - 1.0));
            angle = std::max(angle, std::fabs(std::atan2(r, l) - panAngle(ph0 + i / period, c.depth, c.shape, period)));
            still = std::max(still, std::max(std::fabs(l - 1.0), std::fabs(r - 1.0)));
            leftMost = std::min(leftMost, r);
            rightMost = std::min(rightMost, l);
        }
        CHECK(power < 2e-5 && angle < 1e-4);
        if (!(power < 2e-5 && angle < 1e-4)) std::printf("  auto-pan shape %.2f: power off by %g, angle by %g\n", c.shape, power, angle);
        if (c.depth == 0.0f) CHECK(still < 1e-6);
        if (c.depth == 1.0f) CHECK(leftMost < 1e-3 && rightMost < 1e-3);
    }
}

// The gate on a steady input (0.5 both sides, so the output is half the gain), checked step by
// step against the beat grid from the song position (step 0 on beat multiples of 16 divisions):
// a closed step is closed (1 - depth) throughout; an open one is fully open from one edge after its
// start to one edge before `length` of it, closed from `length` on, and halfway (the S-curve's
// middle) half an edge after its start and half an edge before its end. Joined open steps (length
// 1) have no edge between them. Where two edges don't fit, the peak is the S-curve at half the open
// part over the edge. Two samples of slack where a ramp starts or ends (the state moves on the
// first sample past a boundary and lands within one more).
struct GateCase {
    bool sync, playing;
    double bpm, div, beats;
    float rate;
    int pattern;
    float length, smooth, shape, depth;
};
struct GateCheck {
    int bad = 0;
    double worst = 0.0;
    void expect(double got, double want, double tol) {
        const double e = std::fabs(got - want);
        if (e > tol) ++bad;
        worst = std::max(worst, e - tol);
    }
};
// Checks out[from..to) (gains: out / 0.5) against the law with the song at `pos0` steps at sample 0.
GateCheck checkGate(const GateCase& c, const Buf& out, double pos0, size_t from, size_t to, uint32_t mask) {
    const double steps = c.sync ? c.div * 60.0 / c.bpm * kRate : kRate / c.rate;
    const double open = c.length * steps, smoothS = c.smooth * 1e-3 * kRate;
    const double edge = smoothS + c.shape * std::max(0.0, open / 2.0 - smoothS);
    const bool legato = (1.0 - c.length) * steps < 0.5;
    // Two samples' worth of the S-curve's steepest slope (1.5 / edge): where a ramp starts and turns.
    const double closed = 1.0 - c.depth, slope = 3.0 / edge * c.depth;
    GateCheck g;
    for (size_t i = from; i < to; ++i) {
        const double pos = pos0 + static_cast<double>(i) / steps, whole = std::floor(pos), tau = (pos - whole) * steps;
        const int k = static_cast<int>(static_cast<int64_t>(whole) & 15);
        const double gain = out[i] / 0.5;
        if (!bit(mask, k)) {
            g.expect(gain, closed, 1e-6);
            continue;
        }
        const bool rises = !(legato && bit(mask, k - 1)), falls = !(legato && bit(mask, k + 1));
        if (edge * 2.0 <= open) {
            const double start = rises ? edge + 2.0 : 0.0, end = falls ? open - edge - 2.0 : steps;
            if (tau >= start && tau <= end) g.expect(gain, 1.0, 1e-6);
            if (rises && std::fabs(tau - edge / 2.0) < 0.5) g.expect(gain, 1.0 - c.depth * 0.5, slope);
            if (falls && std::fabs(tau - (open - edge / 2.0)) < 0.5) g.expect(gain, 1.0 - c.depth * 0.5, slope);
        } else if (std::fabs(tau - open / 2.0) < 0.5) {
            g.expect(gain, 1.0 - c.depth * (1.0 - smoothstep(open / 2.0 / edge)), slope);
        }
        if (falls && tau >= open + 2.0) g.expect(gain, closed, 1e-6);
    }
    return g;
}

const GateCase kGateCases[] = {
    {true, true, 120.0, 0.25, 0.0, 0.0f, 1, 0.5f, 3.0f, 0.0f, 1.0f},     // 1/8 on the bar
    {true, true, 137.0, 0.25, 9.5, 0.0f, 9, 0.8f, 1.0f, 0.0f, 0.7f},     // Trance 1 from step 6
    {true, true, 100.0, 0.5, 3.0, 0.0f, 6, 1.0f, 5.0f, 0.0f, 1.0f},      // Tresillo in 1/8 steps, joined
    {false, false, 120.0, 0.25, 0.0, 8.0f, 15, 0.3f, 2.0f, 0.0f, 0.5f},  // free 8 steps a second, half closed
    {true, true, 120.0, 0.25, 4.0, 0.0f, 3, 0.6f, 3.0f, 1.0f, 1.0f},     // Offbeat, soft: swells over the open part
    {true, true, 160.0, 0.125, 1.0, 0.0f, 0, 0.1f, 5.0f, 0.0f, 1.0f},    // 1/32 steps too short for two edges
    {true, false, 90.0, 0.25, 0.0, 0.0f, 12, 0.7f, 0.5f, 0.4f, 0.9f},    // stopped: the tempo's rate from step 0
    {true, true, 128.0, 0.25, 31.0, 0.0f, 13, 1.0f, 10.0f, 0.0f, 1.0f},  // Stutter, joined, from the bar's last beat
};

void gateGrid() {
    for (const GateCase& c : kGateCases) {
        P p;
        p.mode = Pulse::kGate;
        p.sync = c.sync;
        p.divBeats = c.div;
        p.rateHz = c.rate;
        p.pattern = c.pattern;
        p.length = c.length;
        p.smooth = c.smooth;
        p.shape = c.shape;
        p.depth = c.depth;
        const int n = 3 * 44100;
        Buf L(n, 0.5f), R(n, 0.5f);
        render(p, L, R, transport(c.bpm, c.beats, c.playing));
        const double steps = c.sync ? c.div * 60.0 / c.bpm * kRate : kRate / c.rate;
        const double pos0 = c.sync && c.playing ? c.beats / c.div : 0.0;
        const size_t from = static_cast<size_t>((std::floor(pos0) + 1.0 - pos0) * steps) + 1;   // from the first whole step
        const GateCheck g = checkGate(c, L, pos0, from, L.size(), maskOf(c.pattern));
        CHECK(g.bad == 0 && same(L, R));
        if (g.bad) std::printf("  gate %s at %.0f BPM: %d samples off the law, worst by %g\n", Pulse::kPatternNames[c.pattern], c.bpm, g.bad, g.worst);
        // The open part's average: one edge short of `length` (an S-curve's area is half its edge).
        if (c.pattern == 1 && c.length == 0.5f) {
            const double smoothS = c.smooth * 1e-3 * kRate;
            double sum = 0.0;
            for (size_t i = from; i < from + static_cast<size_t>(16 * steps); ++i) sum += L[i] / 0.5;
            const double want = (8.0 * (c.length * steps - smoothS)) / (16.0 * steps);
            CHECK(std::fabs(sum / (16.0 * steps) - want) < 1e-3);
        }
    }
}

// The largest change between neighbouring samples: on a steady input, how fast the gain moves.
double steepest(const Buf& x, size_t from, size_t to) { return maxStep(x, from, to); }

// A transport jump to another bar position: after the cross-fade and an edge the gate is on the
// right step; on the way the gain moves no faster than an edge plus the cross-fade's slope.
void gateFollowsJumps() {
    const GateCase c{true, true, 120.0, 0.25, 0.0, 0.0f, 10, 0.5f, 3.0f, 0.0f, 1.0f};
    P p;
    p.mode = Pulse::kGate;
    p.pattern = c.pattern;
    p.length = c.length;
    p.smooth = c.smooth;
    p.shape = c.shape;
    const int n = 5500 * kChunk;
    Buf L(n, 0.5f), R(n, 0.5f);
    Pulse g;
    const int jumpAt = 1500 * kChunk;   // 1.09 s
    const double shift = 7.25 - 2.0;    // lands 7.25 beats on from 2 beats in: a different bar, step 13
    for (int i = 0; i < n; i += kChunk) {
        Transport t = transport(120.0, i * 2.0 / kRate, true);
        if (i >= jumpAt) t.beats += shift;
        g.set(p, t);
        g.process(&L[i], &R[i], kChunk);
    }
    const double steps = 0.25 * 60.0 / 120.0 * kRate, edge = 3e-3 * kRate;
    const size_t settled = static_cast<size_t>(jumpAt + Pulse::kFadeSamples + edge + 2);
    const GateCheck before = checkGate(c, L, 0.0, static_cast<size_t>(steps), jumpAt, maskOf(c.pattern));   // after the first step
    const GateCheck after = checkGate(c, L, shift / 0.25, settled, L.size(), maskOf(c.pattern));
    CHECK(before.bad == 0 && after.bad == 0);
    if (after.bad) std::printf("  gate after a jump: %d samples off the law, worst by %g\n", after.bad, after.worst);
    const double normal = steepest(L, 0, jumpAt), jump = steepest(L, jumpAt - 1, settled + static_cast<size_t>(steps));
    CHECK(jump <= normal + 0.5 * 1.5 / Pulse::kFadeSamples + 1e-6);
    // The landing differed: the old position would have been somewhere else.
    Buf L2(n, 0.5f), R2(n, 0.5f);
    render(p, L2, R2, transport(120.0, 0.0, true));
    CHECK(maxDiff(L, L2, settled, L.size()) > 0.4);

    // A one-bar loop (beats 16 back to 12) is seamless: the same as playing on. A three-beat loop
    // is a jump like the one above.
    for (const double loopLen : {4.0, 3.0}) {
        Buf a(n, 0.5f), ar(n, 0.5f), b(n, 0.5f), br(n, 0.5f);
        Pulse ga, gb;
        int wrap = 0;
        for (int i = 0; i < n; i += kChunk) {
            Transport t = transport(120.0, i * 2.0 / kRate, true);
            ga.set(p, t);
            ga.process(&a[i], &ar[i], kChunk);
            if (t.beats >= 16.0) {
                if (wrap == 0) wrap = i;
                t.beats = 16.0 - loopLen + std::fmod(t.beats - 16.0, loopLen);
            }
            gb.set(p, t);
            gb.process(&b[i], &br[i], kChunk);
        }
        if (loopLen == 4.0) CHECK(maxDiff(a, b) < 1e-6);
        else CHECK(steepest(b, static_cast<size_t>(wrap) - 1, static_cast<size_t>(wrap) + 4096) <= normal + 0.5 * 1.5 / Pulse::kFadeSamples + 1e-6);
    }
}

// Tremolo (square, depth 1: its gain jumps from 0 to 1 at a phase jump) and Auto-Pan through a
// locate, a stop and a play from elsewhere, a loop that isn't whole periods: the gain moves no
// faster than the square's own edges plus the cross-fade's slope. Then on the beat again.
void lfoFollowsJumps() {
    for (int m : {static_cast<int>(Pulse::kTremolo), static_cast<int>(Pulse::kAutoPan)}) {
        P p;
        p.mode = m;
        p.shape = 1.0f;
        p.divBeats = 1.0;
        const int n = 3500 * kChunk;
        Buf L(n, 0.5f), R(n, 0.5f);
        Pulse g;
        const int locate = 1100 * kChunk, stop = 1500 * kChunk, play = 1700 * kChunk, loop = 2200 * kChunk;
        int wrap = 0;
        for (int i = 0; i < n; i += kChunk) {
            Transport t = transport(120.0, 0.0, i < stop || i >= play);
            const double s = i * 2.0 / kRate;
            if (i < locate) t.beats = 0.3 + s;
            else if (i < play) t.beats = 3.6 + std::min(s, stop * 2.0 / kRate);
            else if (i < loop) t.beats = 9.1 + (s - play * 2.0 / kRate);
            else {
                t.beats = 14.6 + (s - loop * 2.0 / kRate);   // a loop of 2.5 beats, 13.5 to 16
                if (t.beats >= 16.0) {
                    if (wrap == 0) wrap = i;
                    t.beats = 13.5 + std::fmod(t.beats - 16.0, 2.5);
                }
            }
            g.set(p, t);
            g.process(&L[i], &R[i], kChunk);
        }
        const double normal = std::max(steepest(L, 1000, locate), steepest(R, 1000, locate));
        bool clean = wrap > 0;
        for (int at : {locate, play, wrap}) {
            const size_t from = static_cast<size_t>(at) - 1, to = static_cast<size_t>(at) + 4096;
            const double step = std::max(steepest(L, from, to), steepest(R, from, to));
            const bool ok = step <= normal + 1.5 / Pulse::kFadeSamples * (m == Pulse::kAutoPan ? 0.71 : 0.5) + 1e-6;
            clean = clean && ok;
            if (!ok) std::printf("  %s, transport event at %d: %g a sample, normally %g\n", kModeName[m], at, step, normal);
        }
        CHECK(clean);
        // On the beat again after the loop's wrap: the law at the song position.
        const double period = 0.5 * kRate;
        double err = 0.0;
        for (int i = wrap + 2 * Pulse::kFadeSamples; i < wrap + 4000; ++i) {
            const double beats = 13.5 + std::fmod(14.6 + (i * 2.0 / kRate - loop * 2.0 / kRate) - 16.0, 2.5);
            const double ph = beats / 1.0;
            const double want = m == Pulse::kTremolo ? 0.5 * tremoloRef(ph, 1.0, 1.0, period)
                                                     : 0.5 * std::sqrt(2.0) * std::cos(panAngle(ph, 1.0, 1.0, period));
            err = std::max(err, std::fabs(L[i] - want));
        }
        CHECK(err < 1e-3);
        if (!(err < 1e-3)) std::printf("  %s after the wrap: off the law by %g\n", kModeName[m], err);
    }
}

// A new pattern takes over at the next step boundary: the step playing finishes as it was.
void patternAtNextStep() {
    P p;
    p.mode = Pulse::kGate;
    p.pattern = 0;   // every step
    p.length = 0.5f;
    const int n = 1800 * kChunk;
    Buf L(n, 0.5f), R(n, 0.5f);
    Pulse g;
    const double steps = 0.25 * 60.0 / 120.0 * kRate;
    const int change = static_cast<int>(5.2 * steps) / kChunk * kChunk;   // early in step 5
    for (int i = 0; i < n; i += kChunk) {
        if (i == change) p.pattern = 2;   // 1/4: steps 0, 4, 8, 12
        g.set(p, transport(120.0, i * 2.0 / kRate, true));
        g.process(&L[i], &R[i], kChunk);
    }
    const GateCase c{true, true, 120.0, 0.25, 0.0, 0.0f, 0, 0.5f, 3.0f, 0.0f, 1.0f};
    const GateCheck old = checkGate(c, L, 0.0, static_cast<size_t>(steps), static_cast<size_t>(6 * steps), maskOf(0));
    const GateCheck now = checkGate(c, L, 0.0, static_cast<size_t>(6 * steps), L.size(), maskOf(2));
    CHECK(old.bad == 0 && now.bad == 0);
    CHECK(L[static_cast<size_t>(5.3 * steps)] == 0.5f && L[static_cast<size_t>(6.3 * steps)] == 0.0f && L[static_cast<size_t>(8.3 * steps)] == 0.5f);
}

// Abrupt changes of every parameter at chunk edges, on a steady input: after each change, the
// first 0.1 s moves no faster than the settings before or after it do when settled (the square's
// and the gate's edges, plus a cross-fade's slope), or under 0.01 a sample (a gain step of 2%).
void noClicks() {
    const size_t seg = 413 * kChunk;   // 0.3 s
    const int changes = 22;
    Buf L(changes * seg, 0.5f), R(changes * seg, 0.5f);
    Pulse g;
    P p;
    p.sync = false;
    p.depth = 0.5f;
    bool clean = true;
    double settledBefore = 0.0;
    for (int s = 0; s < changes; ++s) {
        switch (s) {   // the change at the start of segment s
        case 1: p.depth = 1.0f; break;
        case 2: p.shape = 1.0f; break;
        case 3: p.stereo = 180.0f; break;
        case 4: p.rateHz = 9.0f; break;
        case 5: p.rateHz = 0.1f; break;   // the square's k grows 90-fold...
        case 6: p.rateHz = 20.0f; break;  // ...and has to fall 200-fold with the rate
        case 7: p.mode = Pulse::kAutoPan; break;
        case 8: p.depth = 0.3f; break;
        case 9: p.shape = 0.0f; break;
        case 10:
            p.mode = Pulse::kGate;
            p.pattern = 9;
            p.depth = 1.0f;
            break;
        case 11: p.pattern = 3; break;
        case 12: p.length = 0.9f; break;
        case 13: p.smooth = 12.0f; break;
        case 14: p.shape = 0.5f; break;
        case 15: p.mix = 0.5f; break;
        case 16: p.sync = true; break;   // stopped: the tempo's rate
        case 17:
            p.mode = Pulse::kTremolo;
            p.shape = 1.0f;
            break;
        case 18: p.divBeats = 16.0; break;
        case 19: p.divBeats = 0.25; break;
        case 20: p.stereo = 0.0f; break;
        case 21:
            p.mix = 1.0f;
            p.depth = 0.0f;
            break;
        default: break;
        }
        const size_t from = static_cast<size_t>(s) * seg;
        for (size_t i = from; i < from + seg; i += kChunk) {
            g.set(p, transport(120.0, 0.0, false));
            g.process(&L[i], &R[i], kChunk);
        }
        double settled = 0.0;
        for (const Buf* x : {&L, &R}) settled = std::max(settled, steepest(*x, from + seg - 6615, from + seg));
        if (s > 0)
            for (const Buf* x : {&L, &R}) {
                const double first = steepest(*x, from - 1, from + 4410);
                const bool ok = first < std::max(0.01, 1.25 * std::max(settled, settledBefore));
                clean = clean && ok;
                if (!ok) std::printf("  change %d: %.5f a sample after it, settled %.5f before, %.5f after\n", s, first, settledBefore, settled);
            }
        settledBefore = settled;
    }
    CHECK(clean);
}

// Loud noise through every mode at the parameters' extremes (and past them): finite, and no
// sample louder than the auto-pan's sqrt2 times the input.
bool bounded(const Buf& in, const Buf& out) {
    for (size_t i = 0; i < in.size(); ++i)
        if (!std::isfinite(out[i]) || std::fabs(out[i]) > 1.41422f * std::fabs(in[i])) return false;
    return true;
}
void extremes() {
    uint32_t s = 5;
    bool ok = true;
    for (int trial = 0; trial < 300; ++trial) {
        auto pick = [&s](float a, float b) { return xorshift(s) & 1u ? a : b; };
        P p;
        p.mode = trial % 3;
        p.sync = xorshift(s) & 1u;
        p.rateHz = pick(0.1f, 20.0f);
        p.divBeats = xorshift(s) & 1u ? 0.0625 : 64.0;
        p.depth = pick(0.0f, 1.0f);
        p.shape = pick(0.0f, 1.0f);
        p.stereo = pick(0.0f, 180.0f);
        p.pattern = static_cast<int>(xorshift(s) % 16u);
        p.length = pick(0.05f, 1.0f);
        p.smooth = pick(0.5f, 50.0f);
        p.mix = pick(0.0f, 1.0f);
        if (trial % 7 == 0) {   // past the ends
            p.rateHz = pick(-5.0f, 1e6f);
            p.divBeats = xorshift(s) & 1u ? -1.0 : 1e9;
            p.depth = pick(-1.0f, 7.0f);
            p.shape = pick(-3.0f, 3.0f);
            p.length = pick(0.0f, 9.0f);
            p.smooth = pick(-1.0f, 1e5f);
            p.pattern = pick(-4.0f, 99.0f);
        }
        const Buf inL = whiteNoise(8192, 100.0f, 2 * trial + 1), inR = whiteNoise(8192, 100.0f, 2 * trial + 2);
        Buf L = inL, R = inR;
        render(p, L, R, transport(xorshift(s) & 1u ? 20.0 : 999.0, 3.7, xorshift(s) & 1u));
        ok = ok && bounded(inL, L) && bounded(inR, R);
    }
    CHECK(ok);
}

// New, random (out-of-range, NaN) parameters and transport every chunk: finite and bounded.
void randomJumps() {
    uint32_t s = 77;
    auto r = [&s] { return 0.5f + 0.5f * randBipolar(s); };
    const Buf inL = whiteNoise(5 * 44100, 1.0f, 7), inR = whiteNoise(5 * 44100, 1.0f, 8);
    Buf L = inL, R = inR;
    Pulse g;
    for (size_t i = 0; i < L.size(); i += kChunk) {
        P p;
        p.mode = static_cast<int>(r() * 5.0f) - 1;
        p.sync = r() < 0.5f;
        p.rateHz = r() * 30.0f - 2.0f;
        p.divBeats = r() * 80.0 - 4.0;
        p.depth = r() * 1.4f - 0.2f;
        p.shape = r() * 1.4f - 0.2f;
        p.stereo = r() * 300.0f - 50.0f;
        p.pattern = static_cast<int>(r() * 20.0f) - 2;
        p.length = r() * 1.4f - 0.2f;
        p.smooth = r() * 70.0f - 5.0f;
        p.mix = r() * 1.4f - 0.2f;
        if (r() < 0.02f) p.depth = std::nanf("");
        if (r() < 0.02f) p.shape = std::nanf("");
        if (r() < 0.02f) p.length = std::nanf("");
        if (r() < 0.02f) p.smooth = std::nanf("");
        if (r() < 0.02f) p.mix = std::nanf("");
        if (r() < 0.02f) p.stereo = std::nanf("");
        if (r() < 0.02f) p.divBeats = std::nan("");
        if (r() < 0.02f) p.rateHz = INFINITY;
        Transport t;
        t.playing = r() < 0.7f;
        t.valid = r() < 0.9f;
        t.bpm = r() * 1200.0;
        t.beats = r() * 1e4 - 100.0;
        if (r() < 0.02f) t.bpm = std::nan("");
        if (r() < 0.02f) t.beats = INFINITY;
        if (r() < 0.02f) t.beats = std::nan("");
        g.set(p, t);
        g.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kChunk, L.size() - i)));
    }
    CHECK(bounded(inL, L) && bounded(inR, R));
}

// A NaN or an infinity in the input comes out as silence there and changes nothing else.
void nanInput() {
    for (int m = 0; m < 3; ++m) {
        P p;
        p.mode = m;
        p.stereo = 60.0f;
        p.mix = 0.7f;
        Buf L = whiteNoise(20000, 0.5f, 9), R = whiteNoise(20000, 0.5f, 10);
        Buf zl = L, zr = R;
        L[1000] = std::nanf("");
        R[2000] = INFINITY;
        L[3001] = R[3001] = -INFINITY;
        zl[1000] = zr[2000] = zl[3001] = zr[3001] = 0.0f;
        render(p, L, R);
        render(p, zl, zr);
        CHECK(allFinite(L) && allFinite(R) && same(L, zl) && same(R, zr));
    }
}

// Blocks of 1, 7 and 32 samples with constant parameters: the same output. The phase is 64-bit
// fixed point, so free it is exact; synced and playing, each block takes the phase from the song
// position, which differs by double rounding only.
void blockSizes() {
    for (int m = 0; m < 3; ++m)
        for (int sync = 0; sync < 2; ++sync) {
            P p;
            p.mode = m;
            p.rateHz = 3.0f;
            p.sync = sync;
            p.divBeats = m == Pulse::kGate ? 0.25 : 0.75;
            p.shape = 0.8f;
            p.stereo = 70.0f;
            p.depth = 0.9f;
            p.mix = 0.8f;
            p.pattern = 11;
            p.length = 0.6f;
            const Transport t = transport(123.0, 3.1, sync);
            const Buf inL = whiteNoise(30000, 0.5f, 11), inR = whiteNoise(30000, 0.5f, 12);
            Buf L32 = inL, R32 = inR, L7 = inL, R7 = inR, L1 = inL, R1 = inR;
            render(p, L32, R32, t, 32);
            render(p, L7, R7, t, 7);
            render(p, L1, R1, t, 1);
            const double tol = sync ? 1e-6 : 0.0;
            const double d = std::max(std::max(maxDiff(L32, L7), maxDiff(R32, R7)), std::max(maxDiff(L32, L1), maxDiff(R32, R1)));
            CHECK(d <= tol);
            if (!(d <= tol)) std::printf("  %s sync %d: blocks differ by %g\n", kModeName[m], sync, d);
        }
}

// reset() forgets everything: afterwards it plays exactly as a new one.
void resetClears() {
    for (int m = 0; m < 3; ++m) {
        P p;
        p.mode = m;
        p.sync = false;
        p.rateHz = 5.0f;
        p.shape = 0.3f;
        Pulse used;
        Buf L = whiteNoise(30000, 0.8f, 13), R = whiteNoise(30000, 0.8f, 14);
        P q = p;
        q.mode = (m + 1) % 3;   // leave it mid-fade, at another phase and depth
        q.depth = 0.2f;
        run(used, p, L, R);
        used.set(q, Transport{});
        used.process(L.data(), R.data(), 17);
        used.reset();
        const Buf inL = whiteNoise(10000, 0.5f, 15), inR = whiteNoise(10000, 0.5f, 16);
        Buf a = inL, b = inR, c = inL, d = inR;
        run(used, p, a, b);
        render(p, c, d);
        CHECK(same(a, c) && same(b, d));
        CHECK(used.tailSamples() == 0);
    }
}

} // namespace

void pulseTests() {
    dryAtMixZero();
    tremoloFollowsLfo();
    tremoloStereo();
    autoPan();
    gateGrid();
    gateFollowsJumps();
    lfoFollowsJumps();
    patternAtNextStep();
    noClicks();
    extremes();
    randomJumps();
    nanInput();
    blockSizes();
    resetClears();
}
