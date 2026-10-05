// The chorus against theory: mix 0 is the input exactly; depth 0 is one echo at exactly the delay
// (as DelayLine::readCubic reads it); constant power on noise; L == R at width 0; the LFO's period
// and the sides' phases; the low cut's response; no clicks. Then the contract: extremes, random
// jumps, NaN input, block sizes, reset, tail.
#include "signal.h"
#include "../dsp/chorus.h"

#include <cstdio>

namespace {

using namespace eft;
using namespace ef;
using P = Chorus::Params;

constexpr int kModes = 3;
const char* const kName[kModes] = {"Chorus", "Ensemble", "Dimension"};
// A side's echo at depth 0 with a mono input: its voices coincide (Chorus 2, Ensemble 3 at
// 1 / sqrt(voices) each; Dimension one voice and 20% of the other side's identical one).
const double kCoherent[kModes] = {std::sqrt(2.0), std::sqrt(3.0), 1.0};

P params(int mode) {
    P p;
    p.mode = mode;
    return p;
}

bool same(const Buf& a, const Buf& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i])) return false;
    return true;
}
double maxDiff(const Buf& a, const Buf& b, size_t from = 0) {
    double m = 0.0;
    for (size_t i = from; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}
// A click is broadband, a chorused sine is not: the largest fourth difference over the level. A
// sine's is (2 sin(pi f / rate))^4, 1.6e-5 at 440 Hz (pitched up a third by the chorus at its
// widest, 4e-5); a step's 3x the step, a corner in a gain's path 2x the change of slope.
double jolt(const Buf& v, size_t from, size_t to) {
    double c = 0.0;
    for (size_t i = from + 4; i < to; ++i)
        c = std::max(c, static_cast<double>(std::fabs(v[i] - 4.0f * v[i - 1] + 6.0f * v[i - 2] - 4.0f * v[i - 3] + v[i - 4])));
    return c / std::max(1e-3, static_cast<double>(peak(v, from, to)));
}

// A fresh chorus over L / R with constant parameters.
void render(const P& p, Buf& L, Buf& R, int chunk = kChunk) {
    Chorus c;
    run(c, p, L, R, Transport{}, chunk);
}

void dryAtMixZero() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.mix = 0.0f;
        p.depth = 1.0f;
        p.rateHz = 3.0f;
        p.lowCutHz = 300.0f;
        p.width = 0.7f;
        const Buf inL = whiteNoise(20000, 0.5f, 1), inR = whiteNoise(20000, 0.5f, 2);
        Buf L = inL, R = inR;
        render(p, L, R);
        CHECK(same(L, inL) && same(R, inR));
    }
}

// Depth 0: the voices sit at the base delay, so an impulse comes back once, exactly delayMs later
// (10 ms = 441 samples), at the voices' coherent sum; nothing anywhere else. At 7.5 ms (330.75
// samples) the cubic spreads it over four samples whose sum is that height and whose centre of
// mass is exactly the delay (the interpolation reproduces straight lines).
void echoAtDepthZero() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.depth = 0.0f;
        p.mix = 1.0f;
        p.lowCutHz = 20.0f;
        p.delayMs = 10.0f;
        Buf L = impulseAt(2000, 100), R = L;
        render(p, L, R);
        bool clean = true;
        for (int i = 0; i < 2000; ++i)
            if (i != 541) clean = clean && L[i] == 0.0f && R[i] == 0.0f;
        CHECK(clean);
        CHECK(std::fabs(L[541] - kCoherent[m]) < 1e-5 && std::fabs(R[541] - kCoherent[m]) < 1e-5);

        p.delayMs = 7.5f;
        L = impulseAt(2000, 100);
        R = L;
        render(p, L, R);
        double sum = 0.0, moment = 0.0;
        for (int i = 0; i < 2000; ++i) {
            sum += L[i];
            moment += static_cast<double>(L[i]) * (i - 100);
        }
        CHECK(std::fabs(sum - kCoherent[m]) < 1e-5);
        CHECK(std::fabs(moment / sum - 330.75) < 1e-3);

        // A stereo input stays stereo: an impulse on the left alone leaves the right silent, but
        // for the Dimension's cross-mix (20% of the left's echo; the left keeps 80%).
        p.delayMs = 10.0f;
        L = impulseAt(2000, 100);
        R = Buf(2000, 0.0f);
        render(p, L, R);
        if (m == Chorus::kDimension) {
            CHECK(std::fabs(L[541] - 0.8) < 1e-6 && std::fabs(R[541] - 0.2) < 1e-6);
        } else {
            CHECK(peak(R) == 0.0f && std::fabs(L[541] - kCoherent[m]) < 1e-5);
        }
    }
}

// The voices read the lines exactly as DelayLine::readCubic would (the guard samples, the wrap and
// the four-at-once read): Dimension at depth 0 and width 0, mono input, against the reference.
void matchesDelayLine() {
    P p = params(Chorus::kDimension);
    p.depth = 0.0f;
    p.width = 0.0f;
    p.mix = 1.0f;
    p.lowCutHz = 20.0f;
    p.delayMs = 7.3f;
    const float d = static_cast<float>(7.3f * (static_cast<double>(kRate) / 1000.0));   // as the chorus has it
    const Buf in = whiteNoise(30000, 0.5f, 7);
    Buf L = in, R = in;
    render(p, L, R);
    DelayLine ref(4096);
    Buf want(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        ref.write(in[i]);
        want[i] = ref.readCubic(d);
    }
    CHECK(maxDiff(L, want) < 2e-6);
    CHECK(maxDiff(R, want) < 2e-6);
}

// Constant power: decorrelated voices at 1 / sqrt(voices) keep the wet as loud as the input, and
// the dry / wet cross-fade keeps the sum there. On white noise at mix 0.5, within 1.5 dB.
void levelOnNoise() {
    for (int m = 0; m < kModes; ++m)
        for (int mono = 0; mono < 2; ++mono) {
            P p = params(m);
            p.mix = 0.5f;
            const Buf inL = whiteNoise(3 * 44100, 0.5f, 3), inR = mono ? inL : whiteNoise(3 * 44100, 0.5f, 4);
            Buf L = inL, R = inR;
            render(p, L, R);
            const double dl = db(rms(L, 22050) / rms(inL, 22050)), dr = db(rms(R, 22050) / rms(inR, 22050));
            CHECK(std::fabs(dl) < 1.5 && std::fabs(dr) < 1.5);
            if (!(std::fabs(dl) < 1.5 && std::fabs(dr) < 1.5)) std::printf("  %s mono %d: %.2f / %.2f dB\n", kName[m], mono, dl, dr);
        }
}

// Width 0 with a mono input: both sides identical, sample for sample. Width 1: they differ.
void monoAtWidthZero() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.width = 0.0f;
        p.depth = 1.0f;
        p.rateHz = 2.3f;
        p.lowCutHz = 150.0f;
        p.mix = 0.7f;
        const Buf in = whiteNoise(30000, 0.5f, 5);
        Buf L = in, R = in;
        render(p, L, R);
        CHECK(same(L, R));
        p.width = 1.0f;
        L = in;
        R = in;
        render(p, L, R);
        CHECK(maxDiff(L, R) > 0.05);
    }
}

// The LFO's period: impulses every `spacing` samples and the echo clusters they bring back (mix 1,
// no low cut). With the period `steps` spacings long, a cluster comes back exactly `steps` impulses
// later and not before (but where the mode is symmetric: the Chorus's antiphase pair repeats every
// half cycle). distance(k): the clusters k impulses apart, relative.
struct Clusters {
    std::vector<Buf> l, r;
};
Clusters clusters(const P& p, int spacing, int count, int from, int to) {
    const int n = spacing * count;
    Buf L(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; i += spacing) L[static_cast<size_t>(i)] = 1.0f;
    Buf R = L;
    render(p, L, R);
    Clusters c;
    for (int j = 0; j < count; ++j) {
        c.l.emplace_back(L.begin() + j * spacing + from, L.begin() + j * spacing + to);
        c.r.emplace_back(R.begin() + j * spacing + from, R.begin() + j * spacing + to);
    }
    return c;
}
double distance(const std::vector<Buf>& a, const std::vector<Buf>& b, int k, int first) {
    double d = 0.0, e = 0.0;
    for (size_t j = static_cast<size_t>(first); j + static_cast<size_t>(k) < a.size(); ++j)
        for (size_t i = 0; i < a[j].size(); ++i) {
            const double x = a[j][i] - b[j + static_cast<size_t>(k)][i];
            d += x * x;
            e += static_cast<double>(a[j][i]) * a[j][i];
        }
    return d / e;
}

void lfoRate() {
    struct Case {
        float rate;
        int steps;   // 44100 / rate / 1050
    };
    for (const Case cs : {Case{1.75f, 24}, Case{7.0f, 6}, Case{0.35f, 120}})
        for (int m = 0; m < kModes; ++m) {
            P p = params(m);
            p.rateHz = cs.rate;
            p.depth = 1.0f;
            p.delayMs = 10.0f;
            p.mix = 1.0f;
            p.lowCutHz = 20.0f;
            const int first = 2;   // the line has filled
            const Clusters c = clusters(p, 1050, first + 2 * cs.steps + 1, 441 - 200, 441 + 200);
            const int half = m == Chorus::kChorus ? cs.steps / 2 : cs.steps;   // the shortest repeat
            CHECK(distance(c.l, c.l, half, first) < 1e-5 && distance(c.r, c.r, half, first) < 1e-5);
            bool moves = true;
            for (int k = 1; k < half; ++k) moves = moves && distance(c.l, c.l, k, first) > 1e-3;
            CHECK(moves);
            if (!moves || !(distance(c.l, c.l, half, first) < 1e-5)) std::printf("  %s at %.2f Hz\n", kName[m], cs.rate);
            // The sides' phases at full width: the Chorus's right pair a quarter cycle on from the
            // left (to within half a sample of LFO phase: the magic circle's cosine runs half a step
            // behind its sine, which leaves its distance ~(A w / 2)^2, a few 1e-4 at 1.75 Hz, where
            // a 1/24 cycle error would give ~2), the Dimension's right voice half a cycle (its
            // cross-mix is symmetric).
            if (m == Chorus::kChorus && cs.steps % 4 == 0) {
                const double q = distance(c.r, c.l, cs.steps / 4, first);
                const double next = std::min(distance(c.r, c.l, cs.steps / 4 - 1, first), distance(c.r, c.l, cs.steps / 4 + 1, first));
                CHECK(q < 1e-3 && q < 1e-3 * next);
            }
            if (m == Chorus::kDimension && cs.steps % 2 == 0) CHECK(distance(c.r, c.l, cs.steps / 2, first) < 1e-5);
        }
}

// The wet's low cut: a Butterworth high-pass by the bilinear transform, prewarped at the cutoff,
// |H|^2 = r^4 / (1 + r^4) with r = tan(pi f / rate) / tan(pi fc / rate). Off at 20 Hz.
void lowCut() {
    P p = params(Chorus::kDimension);
    p.depth = 0.0f;
    p.width = 0.0f;
    p.mix = 1.0f;
    p.lowCutHz = 1000.0f;
    Chorus c;
    for (double f : {250.0, 700.0, 1000.0, 1500.0, 4000.0}) {
        const double r = std::tan(eft::kPi * f / kRate) / std::tan(eft::kPi * 1000.0 / kRate);
        const double want = 10.0 * std::log10(r * r * r * r / (1.0 + r * r * r * r));
        CHECK(std::fabs(gainAt(c, p, f) - want) < 0.05);
    }
    p.lowCutHz = 20.0f;
    CHECK(std::fabs(gainAt(c, p, 40.0)) < 0.01);
}

// A 440 Hz sine through each mode, then abrupt changes of every parameter at chunk edges, the low
// cut off (20 Hz) and on again among them: no clicks. Each change's first 0.1 s against the same
// settings settled (the segment's last 0.15 s), by the measure above: within 3x, or under 0.01
// (a step of 0.3% of the level, -50 dB; the cross-fades' corners stay under half that).
void noClicks() {
    for (int m = 0; m < kModes; ++m) {
        const size_t seg = 413 * kChunk;   // 0.3 s
        Buf L = sine(440.0, static_cast<int>(14 * seg), 0.5f), R = L;
        Chorus c;
        P p = params(m);
        bool clean = true;
        for (int s = 0; s < 14; ++s) {
            switch (s) {   // the change at the start of segment s
            case 3: p.delayMs = 30.0f; break;
            case 4: p.depth = 1.0f; break;
            case 5: p.width = 0.0f; break;
            case 6: p.rateHz = 6.0f; break;
            case 7: p.lowCutHz = 1000.0f; break;
            case 8: p.mix = 1.0f; break;
            case 9: p.mode = (m + 1) % kModes; break;   // all wet: the fade is a whole cross-fade
            case 10: p.lowCutHz = 20.0f; break;          // off, from 1 kHz
            case 11: p.lowCutHz = 300.0f; break;         // on again
            case 12: p.mix = 0.0f; break;
            case 13:
                p.mix = 0.5f;
                p.delayMs = 22.0f;
                p.depth = 0.2f;
                break;
            default: break;
            }
            const size_t from = static_cast<size_t>(s) * seg;
            for (size_t i = from; i < from + seg; i += kChunk) {
                c.set(p, Transport{});
                c.process(&L[i], &R[i], kChunk);
            }
            if (s == 0) continue;   // the chorus's own start
            for (const Buf* x : {&L, &R}) {
                const double first = jolt(*x, from, from + 4410), settled = jolt(*x, from + seg - 6615, from + seg);
                const bool ok = first < std::max(0.01, 3.0 * settled);
                clean = clean && ok;
                if (!ok) std::printf("  %s, change %d: %.5f after it, %.5f settled\n", kName[m], s, first, settled);
            }
        }
        CHECK(clean);
    }
}

// Parameter extremes with loud noise: finite, and no louder than the voices' coherent sum allows
// (sqrt 3 of a full-scale input, a cubic read's overshoot, plus the dry).
void extremes() {
    const Buf inL = whiteNoise(22050, 1.0f, 11), inR = whiteNoise(22050, 1.0f, 12);
    for (int m = 0; m < kModes; ++m)
        for (int bits = 0; bits < 64; ++bits) {
            P p = params(m);
            p.rateHz = bits & 1 ? 10.0f : 0.03f;
            p.depth = bits & 2 ? 1.0f : 0.0f;
            p.delayMs = bits & 4 ? 40.0f : 1.0f;
            p.width = bits & 8 ? 1.0f : 0.0f;
            p.lowCutHz = bits & 16 ? 1000.0f : 20.0f;
            p.mix = bits & 32 ? 1.0f : 0.5f;
            Buf L = inL, R = inR;
            render(p, L, R);
            CHECK(allFinite(L) && allFinite(R) && peak(L) < 4.0f && peak(R) < 4.0f);
        }
}

// New, random (and out-of-range, and NaN) parameters every chunk, modes included: finite, bounded.
void randomJumps() {
    uint32_t s = 99;
    auto r = [&s] { return 0.5f + 0.5f * randBipolar(s); };
    Buf L = whiteNoise(5 * 44100, 1.0f, 13), R = whiteNoise(5 * 44100, 1.0f, 14);
    Chorus c;
    for (size_t i = 0; i < L.size(); i += kChunk) {
        P p;
        p.mode = static_cast<int>(r() * 5.0f) - 1;
        p.rateHz = r() * 12.0f - 1.0f;
        p.depth = r() * 1.4f - 0.2f;
        p.delayMs = r() * 50.0f - 5.0f;
        p.lowCutHz = r() * 1200.0f;
        p.width = r() * 1.4f - 0.2f;
        p.mix = r() * 1.4f - 0.2f;
        if (r() < 0.02f) p.depth = std::nanf("");
        if (r() < 0.02f) p.delayMs = INFINITY;
        if (r() < 0.02f) p.mix = std::nanf("");
        c.set(p, Transport{});
        c.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kChunk, L.size() - i)));
    }
    CHECK(allFinite(L) && allFinite(R) && peak(L) < 4.0f && peak(R) < 4.0f);
}

// A NaN or an infinity in the input never reaches the lines: the output is what a 0 there gives.
void nanInput() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.depth = 0.8f;
        Buf L = whiteNoise(20000, 0.5f, 15), R = whiteNoise(20000, 0.5f, 16);
        Buf zl = L, zr = R;
        L[1000] = std::nanf("");
        R[2000] = INFINITY;
        L[3000] = R[3000] = -INFINITY;
        zl[1000] = zr[2000] = zl[3000] = zr[3000] = 0.0f;
        render(p, L, R);
        render(p, zl, zr);
        CHECK(allFinite(L) && allFinite(R) && same(L, zl) && same(R, zr));
    }
}

// Blocks of 1, 7 and 32 samples (set() before each) with constant parameters: the same output.
void blockSizes() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.depth = 0.8f;
        p.rateHz = 3.0f;
        p.lowCutHz = 200.0f;
        p.mix = 0.6f;
        const Buf inL = whiteNoise(20000, 0.5f, 17), inR = whiteNoise(20000, 0.5f, 18);
        Buf L32 = inL, R32 = inR, L7 = inL, R7 = inR, L1 = inL, R1 = inR;
        render(p, L32, R32, 32);
        render(p, L7, R7, 7);
        render(p, L1, R1, 1);
        CHECK(maxDiff(L32, L7) < 1e-6 && maxDiff(R32, R7) < 1e-6 && maxDiff(L32, L1) < 1e-6 && maxDiff(R32, R1) < 1e-6);
    }
}

// reset() forgets everything: afterwards the chorus plays exactly as a new one.
void resetClears() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        Chorus used;
        Buf L = whiteNoise(30000, 0.8f, 19), R = whiteNoise(30000, 0.8f, 20);
        run(used, p, L, R);
        used.reset();
        const Buf inL = whiteNoise(10000, 0.5f, 21), inR = whiteNoise(10000, 0.5f, 22);
        Buf a = inL, b = inR, c = inL, d = inR;
        run(used, p, a, b);
        render(p, c, d);
        CHECK(same(a, c) && same(b, d));
    }
}

// tailSamples(): an impulse has died 60 dB down by then, and not long before (the voices end within
// a swing of the base delay; with the low cut, its ring adds).
void tail() {
    for (int m = 0; m < kModes; ++m)
        for (float cut : {20.0f, 120.0f}) {
            P p = params(m);
            p.depth = 1.0f;
            p.mix = 1.0f;
            p.lowCutHz = cut;
            Chorus c;
            Buf L = impulseAt(8000, 0), R = L;
            run(c, p, L, R);
            const float top = std::max(peak(L), peak(R));
            int last = 0;
            for (int i = 0; i < 8000; ++i)
                if (std::fabs(L[static_cast<size_t>(i)]) > 1e-3f * top || std::fabs(R[static_cast<size_t>(i)]) > 1e-3f * top) last = i;
            CHECK(last <= c.tailSamples() && last >= c.tailSamples() / 2);
            if (!(last <= c.tailSamples() && last >= c.tailSamples() / 2))
                std::printf("  %s cut %.0f: rings to %d, tail %d\n", kName[m], cut, last, c.tailSamples());
        }
}

} // namespace

void chorusTests() {
    dryAtMixZero();
    echoAtDepthZero();
    matchesDelayLine();
    levelOnNoise();
    monoAtWidthZero();
    lfoRate();
    lowCut();
    noClicks();
    extremes();
    randomJumps();
    nanInput();
    blockSizes();
    resetClears();
    tail();
}
