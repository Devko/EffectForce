// dsp/comp.h: the compressor's static curve, knee, attack and release, stereo link, sidechain low
// cut, makeup and mix against theory; OTT's crossover sum, upward and downward compression, band
// gains, times and silence; and the contract (extremes, random jumps, NaN input, block sizes,
// reset, ramps, mode switches).
#include "signal.h"
#include "../dsp/comp.h"

#include <cfloat>
#include <cstring>
#include <utility>

namespace {

using namespace eft;
using ef::Comp;
using ef::kChunk;

using P = Comp::Params;

double amp(double dB) { return std::pow(10.0, dB / 20.0); }
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }

P comp(float thr, float ratio, float knee = 0.0f, float att = 1.0f, float rel = 50.0f) {
    P p;
    p.mode = 0;
    p.thresholdDb = thr;
    p.ratio = ratio;
    p.kneeDb = knee;
    p.attackMs = att;
    p.releaseMs = rel;
    p.scLowCutHz = 20.0f;
    p.makeupDb = 0.0f;
    p.mix = 1.0f;
    return p;
}

P ott(float up, float down, float low = 0.0f, float mid = 0.0f, float high = 0.0f) {
    P p;
    p.mode = 1;
    p.ottDepth = 1.0f;
    p.ottTime = 1.0f;
    p.ottUp = up;
    p.ottDown = down;
    p.ottLowDb = low;
    p.ottMidDb = mid;
    p.ottHighDb = high;
    p.makeupDb = 0.0f;
    return p;
}

// The settled gain in dB on a sine at `inDb` (its peak, dBFS) on both channels.
double gainOf(const P& p, double hz, double inDb, int settle = 22050, int len = 16384) {
    Comp c;
    Buf L = sine(hz, settle + len, static_cast<float>(amp(inDb))), R = L;
    const Buf in = L;
    run(c, p, L, R);
    return db(magnitude(L, hz, static_cast<size_t>(settle)) / magnitude(in, hz, static_cast<size_t>(settle)));
}

// The static curve in theory (Giannoulis, Massberg & Reiss): a quadratic knee of width w.
double theory(double in, double thr, double ratio, double w) {
    const double o = in - thr;
    if (w <= 0.0) return o < 0.0 ? in : thr + o / ratio;
    if (2.0 * o < -w) return in;
    if (2.0 * std::fabs(o) <= w) return in + (1.0 / ratio - 1.0) * (o + 0.5 * w) * (o + 0.5 * w) / (2.0 * w);
    return thr + o / ratio;
}

// --- Comp -------------------------------------------------------------------------------------

void staticCurve() {
    // -20 dBFS, 4:1, hard knee, fast times: the settled level of a 1 kHz sine from -60 to 0 dBFS.
    double worst = 0.0;
    for (int d = -60; d <= 0; d += 2) worst = std::max(worst, std::fabs(d + gainOf(comp(-20, 4), 1000, d) - theory(d, -20, 4, 0)));
    std::printf("comp static curve: worst %.3f dB off theory\n", worst);
    CHECK(worst < 0.5);
    // Other thresholds and ratios.
    for (float thr : {-40.0f, -10.0f})
        for (float ratio : {2.0f, 10.0f, 20.0f})
            for (double d : {-50.0, -30.0, -6.0, 0.0}) CHECK(near(d + gainOf(comp(thr, ratio), 1000, d), theory(d, thr, ratio, 0), 0.5));
    // Ratio 1 doesn't compress at all.
    CHECK(near(gainOf(comp(-60, 1), 1000, 0), 0.0, 1e-3));
}

void knee() {
    // -20 dBFS, 4:1, from -32 to -8 in 0.5 dB steps: a 12 dB knee follows the parabola, and the
    // curve's slope moves from 1 to 1/4 a little at a time; without the knee it breaks at once.
    std::vector<double> soft, hard;
    double worst = 0.0;
    for (double d = -32.0; d <= -8.0 + 1e-9; d += 0.5) {
        soft.push_back(d + gainOf(comp(-20, 4, 12), 1000, d));
        hard.push_back(d + gainOf(comp(-20, 4, 0), 1000, d));
        worst = std::max(worst, std::fabs(soft.back() - theory(d, -20, 4, 12)));
    }
    double bendSoft = 0.0, bendHard = 0.0, lo = 1e9, hi = -1e9;
    for (size_t i = 2; i < soft.size(); ++i) {
        const double s0 = (soft[i - 1] - soft[i - 2]) / 0.5, s1 = (soft[i] - soft[i - 1]) / 0.5;
        const double h0 = (hard[i - 1] - hard[i - 2]) / 0.5, h1 = (hard[i] - hard[i - 1]) / 0.5;
        bendSoft = std::max(bendSoft, std::fabs(s1 - s0));
        bendHard = std::max(bendHard, std::fabs(h1 - h0));
        lo = std::min(lo, s1);
        hi = std::max(hi, s1);
    }
    std::printf("comp knee: worst %.3f dB off theory, slope steps %.3f (soft) %.3f (hard), slopes %.3f..%.3f\n", worst, bendSoft, bendHard, lo, hi);
    CHECK(worst < 0.5);
    CHECK(bendSoft < 0.1);   // the parabola's 0.031 per step, plus measurement
    CHECK(bendHard > 0.5);   // the hard knee's 0.75 at once
    CHECK(lo > 0.2 && hi < 1.05);   // continuous: never a jump, never steeper than 1:1
    // The knee is centred on the threshold: 3/8 of the way to the hard curve there (w / 8 x 3/4).
    CHECK(near(-20.0 + gainOf(comp(-20, 4, 12), 1000, -20), -20.0 - 1.125, 0.1));
}

struct Steps {
    double before, settled, attackMs, releaseMs;
};

// A 1 kHz sine at -40 dBFS steps to -10 and back (-20 dBFS, 4:1, hard knee): the gain reduction
// read sample by sample (output over input where the input is large enough to divide by).
Steps stepResponse(float att, float rel) {
    const int at = 22050, back = at + 88200, n = back + 132300;
    Buf in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double a = i >= at && i < back ? amp(-10) : amp(-40);
        in[static_cast<size_t>(i)] = static_cast<float>(a * std::sin(2.0 * kPi * 1000.0 * i / ef::kRate));
    }
    Buf L = in, R = in;
    Comp c;
    run(c, comp(-20, 4, 0, att, rel), L, R);
    auto valid = [&](int i) { return std::fabs(in[static_cast<size_t>(i)]) > 0.5 * (i >= at && i < back ? amp(-10) : amp(-40)); };
    auto gr = [&](int i) { return -db(std::fabs(static_cast<double>(L[static_cast<size_t>(i)]) / in[static_cast<size_t>(i)])); };
    Steps s{0.0, 0.0, 0.0, 0.0};
    for (int i = at - 4410; i < at; ++i)
        if (valid(i)) s.before = std::max(s.before, std::fabs(gr(i)));
    int count = 0;
    for (int i = back - 4410; i < back; ++i)
        if (valid(i)) s.settled += gr(i), ++count;
    s.settled /= count;
    int a = at, r = back;
    while (a < back && !(valid(a) && gr(a) >= 0.632 * s.settled)) ++a;
    while (r < n && !(valid(r) && gr(r) <= 0.368 * s.settled)) ++r;
    s.attackMs = (a - at) * 1000.0 / ef::kRate;
    s.releaseMs = (r - back) * 1000.0 / ef::kRate;
    return s;
}

void attackRelease() {
    const std::pair<float, float> times[] = {{10.0f, 200.0f}, {30.0f, 500.0f}, {2.0f, 50.0f}};
    for (const auto& t : times) {
        const Steps s = stepResponse(t.first, t.second);
        std::printf("comp step: attack %.0f ms -> 63%% in %.1f ms, release %.0f ms -> 63%% back in %.1f ms (GR %.2f dB)\n",
                    t.first, s.attackMs, t.second, s.releaseMs, s.settled);
        CHECK(s.before < 0.01);                    // -40 dBFS: no gain reduction
        CHECK(near(s.settled, 7.5, 0.3));          // 10 dB over at 4:1
        CHECK(s.attackMs > 0.5 * t.first && s.attackMs < 1.5 * t.first);
        CHECK(s.releaseMs > 0.5 * t.second && s.releaseMs < 1.5 * t.second);
    }
}

void stereoLink() {
    // One side loud (-6 dBFS at 1 kHz), the other quiet (-40 at 300 Hz): both get the loud side's
    // gain, sample for sample (-20 dBFS, 4:1: 10.5 dB of reduction).
    for (int loudLeft = 0; loudLeft < 2; ++loudLeft) {
        const int n = 44100;
        const Buf loud = sine(1000, n, static_cast<float>(amp(-6))), quiet = sine(300, n, static_cast<float>(amp(-40)));
        Buf L = loudLeft ? loud : quiet, R = loudLeft ? quiet : loud;
        const Buf inL = L, inR = R;
        Comp c;
        run(c, comp(-20, 4, 0, 5, 100), L, R);
        double worst = 0.0;
        for (size_t i = 22050; i < static_cast<size_t>(n); ++i)
            if (std::fabs(inL[i]) > 0.003f && std::fabs(inR[i]) > 0.003f)
                worst = std::max(worst, std::fabs(db(std::fabs(static_cast<double>(L[i]) / inL[i])) - db(std::fabs(static_cast<double>(R[i]) / inR[i]))));
        CHECK(worst < 1e-3);
        const Buf& qIn = loudLeft ? inR : inL;
        const Buf& qOut = loudLeft ? R : L;
        CHECK(near(db(magnitude(qOut, 300, 22050) / magnitude(qIn, 300, 22050)), -10.5, 0.5));
    }
}

// A 12 dB / octave Butterworth high-pass's gain in dB at hz (bilinear, prewarped).
double highPassDb(double hz, double fc) {
    const double w4 = std::pow(std::tan(kPi * hz / ef::kRate) / std::tan(kPi * fc / ef::kRate), 4.0);
    return 10.0 * std::log10(w4 / (1.0 + w4));
}

void sidechain() {
    // A loud 40 Hz tone (-6 dBFS; -30 dBFS, 10:1): 21.6 dB of reduction with the low cut off; with
    // it on, the detector sees the tone through the high-pass, at 200 Hz 28 dB down: under the
    // threshold.
    P p = comp(-30, 10, 0, 5, 200);
    const double gOff = gainOf(p, 40, -6, 44100);
    p.scLowCutHz = 200.0f;
    const double gOn = gainOf(p, 40, -6, 44100);
    std::printf("comp sidechain: 40 Hz at -6 dBFS: %.2f dB (off), %.2f dB (200 Hz low cut)\n", gOff, gOn);
    CHECK(near(gOff, -21.6, 0.5));
    CHECK(gOn > -0.5);
    for (float fc : {25.0f, 50.0f, 100.0f}) {
        p.scLowCutHz = fc;
        const double seen = -6.0 + highPassDb(40, fc);
        CHECK(near(gainOf(p, 40, -6, 44100), -0.9 * std::max(0.0, seen + 30.0), 0.5));
    }
    // Well above the cut it reads as before.
    p.scLowCutHz = 200.0f;
    P off = p;
    off.scLowCutHz = 20.0f;
    CHECK(near(gainOf(p, 3000, -6), gainOf(off, 3000, -6), 0.1));
}

void makeupAndMix() {
    // Makeup alone is a plain gain.
    P p = comp(-20, 1);
    p.makeupDb = 6.0f;
    CHECK(near(gainOf(p, 1000, -10), 6.0, 0.01));
    p.makeupDb = -12.0f;
    CHECK(near(gainOf(p, 1000, -10), -12.0, 0.01));
    // With compression it adds on top: -8 dBFS at -20 / 4:1 is -17, +6 is -11.
    p = comp(-20, 4);
    p.makeupDb = 6.0f;
    CHECK(near(gainOf(p, 1000, -8), -3.0, 0.5));
    p.makeupDb = 24.0f;
    CHECK(near(gainOf(p, 1000, -8), 15.0, 0.5));

    // Mix 0 is the input, bit for bit, whatever the compressor does.
    P dry = comp(-60, 20, 24, 0.1f, 10);
    dry.makeupDb = 24.0f;
    dry.scLowCutHz = 300.0f;
    dry.mix = 0.0f;
    const Buf in = whiteNoise(44100, 1.0f, 21);
    Buf L = in, R = in;
    Comp c;
    run(c, dry, L, R);
    CHECK(same(L, in) && same(R, in));
    // Half: the average of the dry and the compressed (-6 dBFS at -20 / 4:1: -10.5 dB).
    p = comp(-20, 4);
    p.mix = 0.5f;
    CHECK(near(gainOf(p, 1000, -6), db(0.5 + 0.5 * amp(-10.5)), 0.2));
}

// --- OTT --------------------------------------------------------------------------------------

// The three bands' magnitudes in theory: Linkwitz-Riley 4 is a Butterworth squared, prewarped as
// the bilinear transform has it, and the bands are in phase, so with gains they add as numbers.
double ottTheory(double hz, double low, double mid, double high) {
    auto w4 = [&](double fc) { return std::pow(std::tan(kPi * hz / ef::kRate) / std::tan(kPi * fc / ef::kRate), 4.0); };
    const double a = w4(88.3), b = w4(2500.0);
    const double lp1 = 1.0 / (1.0 + a), hp1 = a / (1.0 + a), lp2 = 1.0 / (1.0 + b), hp2 = b / (1.0 + b);
    return db(amp(low) * lp1 + amp(mid) * hp1 * lp2 + amp(high) * hp1 * hp2);
}

const double kSweep[] = {30, 45, 60, 88.3, 120, 200, 400, 800, 1500, 2500, 4000, 8000, 12000, 16000};

void ottFlat() {
    // Nothing compresses: the bands sum to an allpass, flat from 30 Hz to 16 kHz.
    Comp c;
    double worst = 0.0;
    for (double hz : kSweep) worst = std::max(worst, std::fabs(gainAt(c, ott(0, 0), hz)));
    std::printf("ott flat: worst %.4f dB\n", worst);
    CHECK(worst < 0.2);
    // Depth 0 is flat too, whatever the settings: the dry is the bands' own sum.
    P p = ott(1, 1, 12, -12, 12);
    p.ottDepth = 0.0f;
    worst = 0.0;
    for (double hz : kSweep) worst = std::max(worst, std::fabs(gainAt(c, p, hz)));
    CHECK(worst < 0.2);
}

void ottBands() {
    // Band gains move their own band, by the crossovers' theory.
    Comp c;
    const double sets[][3] = {{6, 0, 0}, {0, 6, 0}, {0, 0, 6}, {-12, 12, -6}, {12, -12, 12}};
    double worst = 0.0;
    for (const auto& g : sets)
        for (double hz : kSweep)
            worst = std::max(worst, std::fabs(gainAt(c, ott(0, 0, static_cast<float>(g[0]), static_cast<float>(g[1]), static_cast<float>(g[2])), hz) -
                                              ottTheory(hz, g[0], g[1], g[2])));
    std::printf("ott bands: worst %.4f dB off theory\n", worst);
    CHECK(worst < 0.1);
    CHECK(near(gainAt(c, ott(0, 0, 6, 0, 0), 30), 6.0, 0.1));
    CHECK(near(gainAt(c, ott(0, 0, 0, 6, 0), 500), 6.0, 0.1));
    CHECK(near(gainAt(c, ott(0, 0, 0, 0, 6), 10000), 6.0, 0.1));
    CHECK(near(gainAt(c, ott(0, 0, 6, 0, 0), 10000), 0.0, 0.1));
}

void ottUpDown() {
    // Up: the mid band (1 kHz) at -50 dBFS, 16 dB under its upward threshold, comes up 4.17:1.
    const double up = gainOf(ott(1, 0), 1000, -50);
    std::printf("ott: -50 dBFS up %.2f dB, -6 dBFS down %.2f dB\n", up, gainOf(ott(0, 1), 1000, -6));
    CHECK(up > 9.0);
    CHECK(near(up, 16.0 * (1.0 - 1.0 / 4.17), 1.0));
    CHECK(near(gainOf(ott(1, 0), 400, -70), 36.0 * (1.0 - 1.0 / 4.17), 1.0));
    CHECK(near(gainOf(ott(1, 0), 400, -74), 30.0, 1.0));   // the cap
    CHECK(near(gainOf(ott(1, 0), 400, -85), 10.0, 1.0));   // the floor: 2 dB per dB over -90
    CHECK(near(gainOf(ott(1, 0), 400, -100), 0.0, 0.1));   // under it: nothing
    CHECK(near(gainOf(ott(0.5f, 0), 400, -50), 8.0 * (1.0 - 1.0 / 4.17), 1.0));   // half the slope
    // Down: -6 dBFS, 16 dB over the mid band's downward threshold, comes down to about it.
    const double down = gainOf(ott(0, 1), 1000, -6);
    CHECK(down < -10.0);
    CHECK(near(gainOf(ott(0, 1), 400, -6), -16.0 * (1.0 - 1.0 / 66.7), 1.0));
    CHECK(near(gainOf(ott(0, 0.5f), 400, -6), -8.0 * (1.0 - 1.0 / 66.7), 1.0));
    // Between the two thresholds nothing happens: +0.14 dB, the low band's leak at 400 Hz (-52 dB)
    // lifted 17 dB. Two seconds' settling: the low band's detector holds the sine's switch-on
    // transient for its 282 ms release, and until then lifts the leak by the full 30.
    CHECK(near(gainOf(ott(1, 1), 400, -29, 88200), 0.14, 0.2));
    // Depth blends gains: half of a 12.2 dB lift is 1 + 0.5 (4.07 - 1) = +8.1 dB.
    P half = ott(1, 0);
    half.ottDepth = 0.5f;
    CHECK(near(gainOf(half, 400, -50), db(0.5 + 0.5 * amp(16.0 * (1.0 - 1.0 / 4.17))), 1.0));
    // Makeup comes last.
    P made = ott(1, 1);
    made.makeupDb = 6.0f;
    CHECK(near(gainOf(made, 400, -29, 88200), 6.14, 0.2));
}

void ottTime() {
    // After a loud second the upward gain comes back at the release time x ottTime: 0.2 s after
    // the drop it is all back at 0.1x, hardly started at 10x.
    double g[2];
    const float times[2] = {0.1f, 10.0f};
    for (int k = 0; k < 2; ++k) {
        P p = ott(1, 1);
        p.ottTime = times[k];
        const int drop = 44100, n = drop + 8820 + 8192;
        Buf L(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = static_cast<float>((i < drop ? amp(-6) : amp(-50)) * std::sin(2.0 * kPi * 400.0 * i / ef::kRate));
        Buf R = L;
        const Buf in = L;
        Comp c;
        run(c, p, L, R);
        g[k] = db(magnitude(L, 400, static_cast<size_t>(drop + 8820)) / magnitude(in, 400, static_cast<size_t>(drop + 8820)));
    }
    std::printf("ott time: 0.2 s after a drop, %.2f dB (0.1x), %.2f dB (10x)\n", g[0], g[1]);
    CHECK(near(g[0], 16.0 * (1.0 - 1.0 / 4.17), 1.0));
    CHECK(g[1] < g[0] - 6.0);
}

void ottSilence() {
    // Exact zero stays exact zero, however hard everything is turned up.
    P p = ott(1, 1, 12, 12, 12);
    p.makeupDb = 24.0f;
    Comp c;
    Buf L(44100, 0.0f), R(44100, 0.0f);
    run(c, p, L, R);
    CHECK(peak(L) == 0.0f && peak(R) == 0.0f);
    // And after a signal stops, it comes back to exact zero (no lifted denormal dust).
    Buf A = whiteNoise(44100, 0.5f, 31), B = whiteNoise(44100, 0.5f, 32);
    A.resize(88200, 0.0f);
    B.resize(88200, 0.0f);
    run(c, p, A, B);
    CHECK(peak(A, 66150) == 0.0f && peak(B, 66150) == 0.0f);
}

// --- the contract -----------------------------------------------------------------------------

P corner(int mode, bool high, bool mixed) {
    P p;
    p.mode = mode;
    p.thresholdDb = high ? 0.0f : -60.0f;
    p.ratio = high ? 20.0f : 1.0f;
    p.attackMs = high != mixed ? 100.0f : 0.1f;
    p.releaseMs = high ? 2000.0f : 10.0f;
    p.kneeDb = high != mixed ? 24.0f : 0.0f;
    p.scLowCutHz = high ? 500.0f : 20.0f;
    p.makeupDb = high ? 24.0f : -12.0f;
    p.mix = high != mixed ? 1.0f : 0.0f;
    p.ottDepth = high ? 1.0f : 0.0f;
    p.ottTime = high != mixed ? 10.0f : 0.1f;
    p.ottUp = high ? 1.0f : 0.0f;
    p.ottDown = high != mixed ? 1.0f : 0.0f;
    p.ottLowDb = p.ottHighDb = high ? 12.0f : -12.0f;
    p.ottMidDb = high != mixed ? 12.0f : -12.0f;
    return p;
}

void extremes() {
    // Loud noise (+-1) for three seconds, then silence, at the corners of the parameter space.
    float worst[2] = {0.0f, 0.0f};
    bool finite = true, quiet = true;
    for (int mode = 0; mode < 2; ++mode)
        for (int high = 0; high < 2; ++high)
            for (int mixed = 0; mixed < 2; ++mixed) {
                Comp c;
                Buf L = whiteNoise(44100 * 3, 1.0f, 41), R = whiteNoise(44100 * 3, 1.0f, 42);
                L.resize(44100 * 4, 0.0f);
                R.resize(44100 * 4, 0.0f);
                run(c, corner(mode, high, mixed), L, R);
                finite = finite && allFinite(L) && allFinite(R);
                worst[mode] = std::max(worst[mode], std::max(peak(L), peak(R)));
                quiet = quiet && peak(L, 44100 * 3 + 22050) == 0.0f && peak(R, 44100 * 3 + 22050) == 0.0f;
            }
    std::printf("extremes: worst peak %.3f (Comp), %.3f (OTT)\n", worst[0], worst[1]);
    CHECK(finite);
    CHECK(quiet);
    // Comp: at most the +24 dB makeup on a full-scale input. OTT: +24 dB makeup and +12 dB band
    // gains (63x) until the downward detectors catch up (attack up to 478 ms), on bands that
    // together peak at up to about 3x the input.
    CHECK(worst[0] <= 15.9f);
    CHECK(worst[1] < 200.0f);
}

void randomJumps() {
    // Every parameter jumps every chunk, now and then to NaN or infinity, over loud noise, quiet
    // noise and silence (the upward gain at work).
    Comp c;
    uint32_t s = 777;
    const int n = 44100 * 4;
    Buf L = whiteNoise(n, 1.0f, 51), R = whiteNoise(n, 1.0f, 52);
    for (int i = 0; i < n; ++i) {
        const int part = (i / 11025) % 4;
        const float scale = part == 0 ? 1.0f : part == 1 ? 1e-3f : part == 2 ? 0.0f : 1e-5f;
        L[static_cast<size_t>(i)] *= scale;
        R[static_cast<size_t>(i)] *= scale;
    }
    auto rnd = [&](float lo, float hi) { return lo + (hi - lo) * (0.5f + 0.75f * ef::randBipolar(s)); };   // past both ends too
    for (int pos = 0; pos < n; pos += kChunk) {
        P p;
        p.mode = static_cast<int>(ef::xorshift(s) % 4) - 1;
        p.thresholdDb = rnd(-60, 0);
        p.ratio = rnd(1, 20);
        p.attackMs = rnd(0.1f, 100);
        p.releaseMs = rnd(10, 2000);
        p.kneeDb = rnd(0, 24);
        p.scLowCutHz = rnd(20, 500);
        p.makeupDb = rnd(-12, 24);
        p.mix = rnd(0, 1);
        p.ottDepth = rnd(0, 1);
        p.ottTime = rnd(0.1f, 10);
        p.ottUp = rnd(0, 1);
        p.ottDown = rnd(0, 1);
        p.ottLowDb = rnd(-12, 12);
        p.ottMidDb = rnd(-12, 12);
        p.ottHighDb = rnd(-12, 12);
        if (ef::xorshift(s) % 40 == 0) p.thresholdDb = std::nanf("");
        if (ef::xorshift(s) % 40 == 0) p.ratio = INFINITY;
        if (ef::xorshift(s) % 40 == 0) p.attackMs = -INFINITY;
        if (ef::xorshift(s) % 40 == 0) p.ottTime = std::nanf("");
        if (ef::xorshift(s) % 40 == 0) p.ottMidDb = INFINITY;
        if (ef::xorshift(s) % 40 == 0) p.makeupDb = std::nanf("");
        c.set(p, {});
        c.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], std::min(kChunk, n - pos));
    }
    std::printf("random jumps: peak %.3f\n", std::max(peak(L), peak(R)));
    CHECK(allFinite(L) && allFinite(R));
    CHECK(std::max(peak(L), peak(R)) < 200.0f);   // the bound of extremes()
}

void nanInput() {
    // A NaN or infinity is silence: the output is the same as for a zero there, sample for sample.
    P sc = comp(-30, 6, 6, 5, 100);
    sc.scLowCutHz = 150.0f;
    for (const P& p : {comp(-30, 6, 6, 5, 100), sc, ott(1, 1)}) {
        Buf L = whiteNoise(22050, 0.5f, 61), R = whiteNoise(22050, 0.5f, 62);
        Buf L0 = L, R0 = R;
        L[1000] = std::nanf("");
        R[5000] = INFINITY;
        L[9000] = -INFINITY;
        R[9000] = std::nanf("");
        L0[1000] = R0[5000] = L0[9000] = R0[9000] = 0.0f;
        Comp a, b;
        run(a, p, L, R);
        run(b, p, L0, R0);
        CHECK(allFinite(L) && allFinite(R));
        CHECK(same(L, L0) && same(R, R0));
    }
    // Huge finite samples are clamped to +80 dBFS on the way in: nothing overflows, whatever the
    // gains (Comp: +24 dB at most; OTT: as extremes(), 63x on bands up to about 3x the input).
    for (int mode = 0; mode < 2; ++mode) {
        Buf L = whiteNoise(22050, 0.5f, 63), R = whiteNoise(22050, 0.5f, 64);
        for (size_t i = 100; i < L.size(); i += 997) {
            L[i] = (i / 997) % 2 ? FLT_MAX : -1e30f;
            R[i] = -FLT_MAX;
        }
        Comp c;
        run(c, corner(mode, true, false), L, R);
        CHECK(allFinite(L) && allFinite(R));
        CHECK(std::max(peak(L), peak(R)) < (mode == 0 ? 1e4f * 15.9f : 1e4f * 200.0f));
    }
}

void blockSizes() {
    // Constant parameters: chunks of 1, 7 and 32 give the same output.
    P sc = comp(-30, 6, 6, 5, 100);
    sc.scLowCutHz = 150.0f;
    for (const P& p : {comp(-30, 6, 6, 5, 100), sc, ott(1, 1, 3, -2, 4)}) {
        Buf ref[2];
        for (int k = 0; k < 3; ++k) {
            const int chunk = k == 0 ? 32 : k == 1 ? 7 : 1;
            Buf L = whiteNoise(22050, 0.5f, 71), R = sine(330, 22050, 0.3f);
            Comp c;
            run(c, p, L, R, {}, chunk);
            if (k == 0) {
                ref[0] = L;
                ref[1] = R;
            } else {
                CHECK(same(L, ref[0]) && same(R, ref[1]));
            }
        }
    }
}

void resetClears() {
    // After reset() a used instance plays like a new one.
    for (const P& p : {comp(-30, 6, 6, 5, 100), ott(1, 1)}) {
        Comp used, fresh;
        Buf L = whiteNoise(22050, 1.0f, 81), R = whiteNoise(22050, 1.0f, 82);
        run(used, p, L, R);
        used.reset();
        Buf A = sine(200, 22050, 0.01f), B = A, A0 = A, B0 = A;
        run(used, p, A, B);
        run(fresh, p, A0, B0);
        CHECK(same(A, A0) && same(B, B0));
        // The compressor holds nothing to ring out; OTT's crossovers do (the 88 Hz allpass, ~770 samples).
        CHECK(used.tailSamples() == (p.mode == 1 ? 1024 : 0) && used.tailSamples() == fresh.tailSamples());
    }
}

void ramps() {
    // A parameter jump spreads over the next chunk instead of stepping (a constant input: what
    // moves is the gain). Comp: the threshold, -40 to 0 dBFS (25 dB less reduction); OTT: a band
    // gain, -12 to +12 dB.
    const int kinds = 2;
    for (int k = 0; k < kinds; ++k) {
        P a = k == 0 ? comp(-40, 20) : ott(0, 0, -12), b = a;
        if (k == 0) b.thresholdDb = 0.0f;
        else b.ottLowDb = 12.0f;
        Comp c;
        Buf L(8192, 0.5f), R(8192, 0.5f);
        const int at = 4096;
        for (int pos = 0; pos < 8192; pos += kChunk) {
            c.set(pos < at ? a : b, {});
            c.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], kChunk);
        }
        const float before = L[at - 1], after = L[at + kChunk - 1], total = std::fabs(after - before);
        CHECK(total > 0.3f);
        CHECK(maxStep(L, at - 1, at + kChunk) < 0.25f * total);
        CHECK(L[at + 2 * kChunk] == after);   // and lands within the chunk
    }
}

void modeSwitch() {
    // Comp <-> OTT every 0.1 s on a 200 Hz sine, neither compressing (OTT is then the crossovers'
    // allpass): the switch fades over a chunk. A hard switch would step by up to the difference
    // between the dry and the allpassed sine, about 30x the sine's own largest step.
    P cp = comp(0, 1), op = ott(0, 0);
    Comp c;
    const int n = 44100;
    Buf L = sine(200, n, 0.25f), R = L;
    const Buf in = L;
    for (int pos = 0; pos < n; pos += kChunk) {
        c.set((pos / 4410) % 2 ? op : cp, {});
        c.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], std::min(kChunk, n - pos));
    }
    const double ratio = maxStep(L) / maxStep(in);
    std::printf("mode switch: largest step %.2fx the sine's\n", ratio);
    CHECK(allFinite(L) && allFinite(R));
    CHECK(ratio < 3.0);
}

} // namespace

void compTests() {
    staticCurve();
    knee();
    attackRelease();
    stereoLink();
    sidechain();
    makeupAndMix();
    ottFlat();
    ottBands();
    ottUpDown();
    ottTime();
    ottSilence();
    extremes();
    randomJumps();
    nanInput();
    blockSizes();
    resetClears();
    ramps();
    modeSwitch();
}
