// dsp/filter.h: responses against the bilinear-transformed analog prototypes, resonance, spread,
// drive, mix, and robustness (extremes, random jumps, NaN input, block sizes, reset).
#include "signal.h"
#include "../dsp/filter.h"

#include <complex>
#include <cstring>
#include <utility>

namespace {

using namespace eft;
using ef::Filter;

using P = Filter::Params;

P params(int type, float hz, float res = 0.0f, float drive = 0.0f, float spread = 0.0f, float mix = 1.0f) {
    P p;
    p.type = type;
    p.cutoffHz = hz;
    p.res = res;
    p.drive = drive;
    p.spread = spread;
    p.mix = mix;
    return p;
}

// The gain in dB on L and R of a sine at hz with peak amplitude amp.
std::pair<double, double> gainLR(Filter& f, const P& p, double hz, float amp = 0.25f) {
    const int settle = 8192, len = 16384;
    f.reset();
    Buf L = sine(hz, settle + len, amp), R = L;
    const Buf in = L;
    run(f, p, L, R);
    const double ref = magnitude(in, hz, settle);
    return {db(magnitude(L, hz, settle) / ref), db(magnitude(R, hz, settle) / ref)};
}
double gain(Filter& f, const P& p, double hz) { return gainLR(f, p, hz).first; }

// The analog prototypes at the prewarped frequency: what the bilinear transform makes of them.
double warp(double hz, double fc) { return std::tan(eft::kPi * hz / ef::kRate) / std::tan(eft::kPi * fc / ef::kRate); }
double butterDb(double hz, double fc, int order, bool high) {
    const double w = high ? 1.0 / warp(hz, fc) : warp(hz, fc);
    return -10.0 * std::log10(1.0 + std::pow(w, 2 * order));
}
double qOf(double res) { return std::sqrt(0.5) * std::pow(22.627417, res); }
double bandDb(double hz, double fc, double q) {   // k s / (s^2 + k s + 1), 0 dB at fc
    const double w = warp(hz, fc), k = 1.0 / q;
    return db(k * w / std::sqrt((1.0 - w * w) * (1.0 - w * w) + k * k * w * w));
}
double notchDb(double hz, double fc, double q) {
    const double w = warp(hz, fc), k = 1.0 / q;
    return db(std::fabs(1.0 - w * w) / std::sqrt((1.0 - w * w) * (1.0 - w * w) + k * k * w * w));
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The largest third difference: a sine of amplitude a at hz makes at most a (2 sin(pi hz / rate))^3,
// a step or a kink makes about its own size. A click detector far more sensitive than maxStep.
double maxD3(const Buf& y, size_t from = 3) {
    double m = 0.0;
    for (size_t i = std::max<size_t>(from, 3); i < y.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(y[i]) - 3.0 * y[i - 1] + 3.0 * y[i - 2] - y[i - 3]));
    return m;
}
double sineD3(double amp, double hz) { return amp * std::pow(2.0 * std::sin(eft::kPi * hz / ef::kRate), 3.0); }

void slopes() {
    Filter f;
    // LP 12: -3 dB at the cutoff, 12 dB / octave, flat below.
    CHECK(near(gain(f, params(Filter::LP12, 1000), 1000), -3.01, 0.5));
    CHECK(near(gain(f, params(Filter::LP12, 1000), 4000), -24.0, 1.5));
    CHECK(near(gain(f, params(Filter::LP12, 1000), 100), 0.0, 0.05));
    // LP 24: Butterworth, 24 dB / octave.
    CHECK(near(gain(f, params(Filter::LP24, 1000), 1000), -3.01, 0.5));
    CHECK(near(gain(f, params(Filter::LP24, 1000), 4000), -48.0, 2.0));
    CHECK(near(gain(f, params(Filter::LP24, 1000), 100), 0.0, 0.05));
    // HP mirrors them.
    CHECK(near(gain(f, params(Filter::HP12, 1000), 1000), -3.01, 0.5));
    CHECK(near(gain(f, params(Filter::HP12, 1000), 250), -24.0, 1.5));
    CHECK(near(gain(f, params(Filter::HP12, 1000), 10000), 0.0, 0.1));
    CHECK(near(gain(f, params(Filter::HP24, 1000), 1000), -3.01, 0.5));
    CHECK(near(gain(f, params(Filter::HP24, 1000), 250), -48.0, 2.0));
    CHECK(near(gain(f, params(Filter::HP24, 1000), 10000), 0.0, 0.1));

    // And the bilinear-transformed Butterworth exactly, across the band.
    const double fcs[] = {100.0, 1000.0, 8000.0};
    const double ratios[] = {0.25, 0.5, 0.8, 1.0, 1.25, 2.0, 4.0};
    for (double fc : fcs) {
        for (double r : ratios) {
            const double hz = fc * r;
            if (hz > 19000.0) continue;
            CHECK(near(gain(f, params(Filter::LP12, static_cast<float>(fc)), hz), butterDb(hz, fc, 2, false), 0.1));
            CHECK(near(gain(f, params(Filter::LP24, static_cast<float>(fc)), hz), butterDb(hz, fc, 4, false), 0.1));
            CHECK(near(gain(f, params(Filter::HP12, static_cast<float>(fc)), hz), butterDb(hz, fc, 2, true), 0.1));
            CHECK(near(gain(f, params(Filter::HP24, static_cast<float>(fc)), hz), butterDb(hz, fc, 4, true), 0.1));
        }
    }
}

void bandAndNotch() {
    Filter f;
    // BP: 0 dB at its peak, the peak at the cutoff, the bilinear band-pass around it.
    for (float res : {0.0f, 0.5f, 1.0f}) {
        const P p = params(Filter::BP, 1000, res);
        CHECK(near(gain(f, p, 1000), 0.0, 0.05));
        for (double hz : {300.0, 500.0, 800.0, 1250.0, 2000.0, 3000.0}) {
            CHECK(gain(f, p, hz) < -0.1);
            CHECK(near(gain(f, p, hz), bandDb(hz, 1000, qOf(res)), 0.1));
        }
    }
    // Scanning for the peak finds the cutoff.
    double best = -1e9, bestHz = 0.0;
    for (double hz = 500.0; hz < 2000.0; hz *= 1.02) {
        const double g = gain(f, params(Filter::BP, 1000, 0.6f), hz);
        if (g > best) best = g, bestHz = hz;
    }
    CHECK(near(bestHz / 1000.0, 1.0, 0.02));

    // Notch: deep at the cutoff, flat far from it, narrower with res.
    for (float res : {0.0f, 0.5f, 1.0f}) {
        const P p = params(Filter::NOTCH, 1000, res);
        CHECK(gain(f, p, 1000) < -30.0);
        for (double hz : {100.0, 500.0, 2000.0, 10000.0}) CHECK(near(gain(f, p, hz), notchDb(hz, 1000, qOf(res)), 0.1));
    }
    CHECK(gain(f, params(Filter::NOTCH, 1000, 1.0f), 900) > gain(f, params(Filter::NOTCH, 1000, 0.0f), 900) + 10.0);
}

void resonance() {
    Filter f;
    // The gain at the cutoff is Q (the bilinear transform keeps it there): 0.707 * 22.6^res.
    for (int type : {Filter::LP12, Filter::LP24, Filter::HP12, Filter::HP24}) {
        double prev = -1e9;
        for (float res : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            const double g = gain(f, params(type, 1000, res), 1000);
            CHECK(near(g, db(qOf(res)), 0.15));
            CHECK(g > prev + 3.0);   // each step raises the peak
            prev = g;
        }
    }
    CHECK(near(gain(f, params(Filter::LP12, 1000, 1.0f), 1000), 24.08, 0.15));
    // The passband stays at 0 dB.
    CHECK(near(gain(f, params(Filter::LP24, 1000, 1.0f), 100), 0.0, 0.1));
    CHECK(near(gain(f, params(Filter::HP24, 1000, 1.0f), 10000), 0.0, 0.1));
}

// The frequency where a lowpass on L (or R) is down 3 dB, by bisection in octaves.
double minus3(Filter& f, P p, bool right) {
    double lo = std::log2(50.0), hi = std::log2(15000.0);
    for (int i = 0; i < 18; ++i) {
        const double mid = 0.5 * (lo + hi);
        const auto g = gainLR(f, p, std::exp2(mid));
        ((right ? g.second : g.first) > -3.0103 ? lo : hi) = mid;
    }
    return std::exp2(0.5 * (lo + hi));
}

void spread() {
    Filter f;
    for (float s : {-1.0f, -0.4f, 0.5f, 1.0f}) {
        const P p = params(Filter::LP12, 1000, 0.0f, 0.0f, s);
        const double l = minus3(f, p, false), r = minus3(f, p, true);
        CHECK(near(r / l, std::exp2(s), 0.01 * std::exp2(s)));
        CHECK(near(l, 1000.0 * std::exp2(-0.5 * s), 5.0));
        CHECK(near(r, 1000.0 * std::exp2(0.5 * s), 5.0));
    }
    // No spread: the two sides are identical.
    Buf L = whiteNoise(4096, 0.5f, 7), R = L;
    f.reset();
    run(f, params(Filter::LP24, 700, 0.6f, 0.4f), L, R);
    CHECK(std::memcmp(L.data(), R.data(), L.size() * sizeof(float)) == 0);
}

// Gain in dB of a module run at amplitude amp (the drive's level dependence).
double gainAtLevel(Filter& f, const P& p, double hz, float amp) { return gainLR(f, p, hz, amp).first; }

void drive() {
    Filter f;
    // Drive 0 is linear: the gain doesn't depend on the level, from -60 dBFS to +12.
    const P clean = params(Filter::LP12, 3000, 0.3f);
    const double quiet = gainAtLevel(f, clean, 500, 0.001f), loud = gainAtLevel(f, clean, 500, 4.0f);
    CHECK(near(quiet, loud, 1e-3));
    // Driven: +24 dB in, -12 out, so +12 dB on quiet signals and far less on loud ones.
    const P hot = params(Filter::LP12, 3000, 0.3f, 1.0f);
    CHECK(near(gainAtLevel(f, hot, 500, 0.001f), quiet + 12.04, 0.1));
    CHECK(gainAtLevel(f, hot, 500, 1.0f) < quiet + 12.04 - 12.0);
    // A little drive is a little nonlinear: a full-scale sine grows a 3rd harmonic.
    f.reset();
    Buf L = sine(500, 16384, 1.0f), R = L;
    run(f, params(Filter::LP12, 20000, 0.0f, 0.1f), L, R);
    CHECK(magnitude(L, 1500, 4096) > 1e-3);
    // Drive never lets the level run away: bounded for any input.
    f.reset();
    Buf N = whiteNoise(44100, 100.0f, 3), M = N;
    run(f, params(Filter::LP12, 20000, 0.0f, 1.0f), N, M);
    CHECK(allFinite(N) && peak(N) < 1.2f);   // softclip's 1, times 0.25, plus the filter's overshoot
}

void mix() {
    Filter f;
    // Mix 0 is the input, bit for bit, whatever the filter does.
    for (int type = 0; type < Filter::kTypes; ++type) {
        f.reset();
        const Buf in = whiteNoise(8192, 0.9f, 11u + static_cast<uint32_t>(type));
        Buf L = in, R = in;
        run(f, params(type, 300, 0.9f, 0.8f, 0.7f, 0.0f), L, R);
        CHECK(std::memcmp(L.data(), in.data(), in.size() * sizeof(float)) == 0);
        CHECK(std::memcmp(R.data(), in.data(), in.size() * sizeof(float)) == 0);
    }
    // Half: the average of dry and wet (a sine at the cutoff, LP 12: the wet is 0.707 at -90 degrees).
    const double g = gain(f, params(Filter::LP12, 1000, 0.0f, 0.0f, 0.0f, 0.5f), 1000);
    CHECK(near(g, db(0.5 * std::abs(std::complex<double>(1.0, 0.0) + std::complex<double>(0.0, -std::sqrt(0.5)))), 0.1));
}

// R's sine starts out of silence with a step, and the filter rings at it: the click tests skip
// the first sweep.
constexpr size_t kOnset = 2205;

// Runs f over a sine at hz (R a radian later) with the cutoff swept exponentially 20 Hz .. 20 kHz
// every 50 ms: shape 0 up and back down, 1 up and a jump back, 2 down and a jump back.
void sweepRun(Filter& f, int type, float res, double hz, int shape, Buf& L, Buf& R) {
    const int n = 44100 / 2, period = 2205;
    f.reset();
    L = sine(hz, n, 0.5f);
    R = sine(hz, n, 0.5f, 1.0);
    for (int pos = 0; pos < n; pos += ef::kChunk) {
        double ph = static_cast<double>(pos % period) / period;
        if (shape == 0 && (pos / period) % 2) ph = 1.0 - ph;
        if (shape == 2) ph = 1.0 - ph;
        f.set(params(type, static_cast<float>(20.0 * std::pow(1000.0, ph)), res, 0.0f, 0.3f), {});
        f.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], std::min(ef::kChunk, n - pos));
    }
}

void sweep() {
    // Continuous sweeps, ten octaves in 50 ms: no step beyond what a sine at the input's frequency
    // and the output's peak makes (resonance adds its own ring at the moving cutoff), and no kink:
    // the third difference stays near a sine's, where coefficients stepped once a chunk instead of
    // gliding read 40..280 times a sine's (measured, at 200 Hz).
    Filter f;
    Buf L, R;
    for (int type = 0; type < Filter::kTypes; ++type) {
        for (float res : {0.0f, 0.5f}) {
            for (double hz : {200.0, 2000.0}) {
                sweepRun(f, type, res, hz, 0, L, R);
                CHECK(allFinite(L) && allFinite(R));
                const double pk = std::max(peak(L), peak(R)), natural = pk * 2.0 * eft::kPi * hz / ef::kRate;
                CHECK(std::max(maxStep(L, kOnset), maxStep(R, kOnset)) <= (res == 0.0f ? 1.35 : 2.0) * natural);
                if (hz < 1000.0) CHECK(std::max(maxD3(L, kOnset), maxD3(R, kOnset)) <= 20.0 * sineD3(pk, hz));
            }
        }
    }
    // Full resonance and jumps across the whole range in one chunk: finite, and no louder than a
    // sine sitting on the peak (+24 dB) would be.
    for (int shape = 0; shape < 3; ++shape) {
        for (int type = 0; type < Filter::kTypes; ++type) {
            for (float res : {0.0f, 0.5f, 1.0f}) {
                if (shape == 0 && res < 1.0f) continue;
                sweepRun(f, type, res, 200.0, shape, L, R);
                CHECK(allFinite(L) && allFinite(R));
                CHECK(std::max(peak(L), peak(R)) < 0.5f * 16.0f);
            }
        }
    }
}

void typeSwitch() {
    // LP 12 <-> LP 24 every four chunks, far above a sine: the second stage was kept running, so the
    // switch doesn't dip (a stage starting from silence would) and leaves no kink.
    Filter f;
    Buf L = sine(200, 22050, 0.5f), R = L;
    for (size_t pos = 0; pos < L.size(); pos += ef::kChunk) {
        const int n = static_cast<int>(std::min<size_t>(ef::kChunk, L.size() - pos));
        f.set(params((pos / ef::kChunk / 4) % 2 ? Filter::LP24 : Filter::LP12, 4000, 0.3f), {});
        f.process(&L[pos], &R[pos], n);
    }
    CHECK(allFinite(L));
    const Buf in = sine(200, 22050, 0.5f);
    double off = 0.0;
    for (size_t i = 4410; i < L.size(); ++i) off = std::max(off, std::fabs(static_cast<double>(L[i]) - in[i]));
    CHECK(off < 0.07);   // LP 24 lags the sine by 6 degrees there: 0.053 (a cold stage: 0.45)
    CHECK(maxStep(L, 4410) <= 1.25 * 0.5 * 2.0 * eft::kPi * 200 / ef::kRate);
    // Any type to any other every chunk, with resonance: finite and bounded.
    uint32_t s = 99;
    Buf N = whiteNoise(44100, 1.0f, 4), M = whiteNoise(44100, 1.0f, 5);
    for (size_t pos = 0; pos < N.size(); pos += ef::kChunk) {
        const int n = static_cast<int>(std::min<size_t>(ef::kChunk, N.size() - pos));
        f.set(params(static_cast<int>(ef::xorshift(s) % Filter::kTypes), 500, 1.0f), {});
        f.process(&N[pos], &M[pos], n);
    }
    CHECK(allFinite(N) && allFinite(M) && peak(N) < 20.0f && peak(M) < 20.0f);
}

void extremes() {
    // Loud noise at every corner of the parameter space, a second each.
    Filter f;
    float worst = 0.0f;
    bool finite = true;
    for (int type = 0; type < Filter::kTypes; ++type)
        for (float hz : {20.0f, 20000.0f})
            for (float res : {0.0f, 1.0f})
                for (float drv : {0.0f, 1.0f})
                    for (float spr : {-1.0f, 1.0f}) {
                        f.reset();
                        Buf L = whiteNoise(44100, 1.0f, 5), R = whiteNoise(44100, 1.0f, 6);
                        run(f, params(type, hz, res, drv, spr), L, R);
                        finite = finite && allFinite(L) && allFinite(R);
                        worst = std::max(worst, std::max(peak(L), peak(R)));
                    }
    CHECK(finite);
    CHECK(worst < 20.0f);   // white noise through a Q of 30 at most: 6.7 measured
    // Five seconds of the sharpest corner.
    f.reset();
    Buf L = whiteNoise(5 * 44100, 1.0f, 15), R = whiteNoise(5 * 44100, 1.0f, 16);
    run(f, params(Filter::LP24, 20, 1.0f, 1.0f, -1.0f), L, R);
    CHECK(allFinite(L) && allFinite(R) && peak(L) < 20.0f && peak(R) < 20.0f);
    // Out-of-range and non-finite parameters are clamped.
    const float wild[] = {-1e30f, -5.0f, 1e9f, INFINITY, -INFINITY, std::nanf("")};
    for (float w : wild) {
        f.reset();
        Buf A = whiteNoise(4096, 1.0f, 17), B = A;
        run(f, params(w > 0.0f ? 1000 : -1000, w, w, w, w, w), A, B);
        CHECK(allFinite(A) && allFinite(B) && peak(A) < 20.0f);
    }
}

void randomJumps() {
    // Every parameter somewhere new every chunk (out of range too), under loud noise, for 3 s.
    Filter f;
    uint32_t s = 12345;
    const int n = 44100 * 3;
    Buf L = whiteNoise(n, 1.0f, 8), R = whiteNoise(n, 1.0f, 9);
    for (int pos = 0; pos < n; pos += ef::kChunk) {
        P p;
        p.type = static_cast<int>(ef::xorshift(s) % 9) - 1;
        p.cutoffHz = static_cast<float>(std::exp2(ef::randBipolar(s) * 8.0 + 9.0));
        p.res = 0.75f + 0.75f * ef::randBipolar(s);
        p.drive = 0.5f + 0.75f * ef::randBipolar(s);
        p.spread = 2.0f * ef::randBipolar(s);
        p.mix = 0.5f + 0.75f * ef::randBipolar(s);
        if (ef::xorshift(s) % 50 == 0) p.cutoffHz = std::nanf("");
        if (ef::xorshift(s) % 50 == 0) p.res = INFINITY;
        if (ef::xorshift(s) % 50 == 0) p.spread = -INFINITY;
        f.set(p, {});
        f.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], std::min(ef::kChunk, n - pos));
    }
    CHECK(allFinite(L) && allFinite(R));
    CHECK(peak(L) < 20.0f && peak(R) < 20.0f);   // 5.0 measured
}

void nanInput() {
    // NaN and infinities in the input act as silence: the output is exactly what zeros there give,
    // and finite throughout. Absurd finite levels stay finite too.
    for (int type = 0; type < Filter::kTypes; ++type) {
        Filter a, b;
        const P p = params(type, 800, 0.8f, 0.5f, 0.3f, 0.7f);
        Buf clean = sine(300, 8192, 0.5f);
        clean[1000] = clean[2000] = clean[3000] = 0.0f;
        Buf bad = clean;
        bad[1000] = std::nanf("");
        bad[2000] = INFINITY;
        bad[3000] = -INFINITY;
        Buf cl = clean, cr = clean, bl = bad, br = bad;
        run(a, p, cl, cr);
        run(b, p, bl, br);
        CHECK(allFinite(bl) && allFinite(br));
        CHECK(std::memcmp(cl.data(), bl.data(), cl.size() * sizeof(float)) == 0);
        CHECK(std::memcmp(cr.data(), br.data(), cr.size() * sizeof(float)) == 0);

        Buf huge = whiteNoise(4096, 1.0f, 21), h2 = huge;
        huge[100] = 1e30f;
        huge[200] = -3e38f;
        a.reset();
        run(a, params(type, 20000, 1.0f), huge, h2);
        CHECK(allFinite(huge) && allFinite(h2));
    }
}

void blockSizes() {
    // Constant parameters: chunks of 1, 7 and 32 give the same output, bit for bit.
    const P sets[] = {params(Filter::LP24, 800, 0.7f, 0.5f, 0.5f, 0.7f), params(Filter::NOTCH, 3000, 0.3f, 0.0f, -1.0f),
                      params(Filter::HP12, 150, 1.0f, 1.0f, 0.2f, 1.0f), params(Filter::BP, 20000, 0.5f)};
    for (const P& p : sets) {
        Filter f;
        const Buf inL = whiteNoise(4000, 0.7f, 31), inR = whiteNoise(4000, 0.7f, 32);
        Buf L32 = inL, R32 = inR;
        run(f, p, L32, R32, {}, 32);
        for (int chunk : {1, 7}) {
            f.reset();
            Buf L = inL, R = inR;
            run(f, p, L, R, {}, chunk);
            CHECK(std::memcmp(L.data(), L32.data(), L.size() * sizeof(float)) == 0);
            CHECK(std::memcmp(R.data(), R32.data(), R.size() * sizeof(float)) == 0);
        }
    }
}

void resetClears() {
    // After reset() the filter is as good as new: no state, and the next set() jumps.
    Filter used, fresh;
    Buf L = whiteNoise(5000, 1.0f, 41), R = L;
    run(used, params(Filter::LP24, 100, 1.0f, 1.0f, 1.0f), L, R);
    used.reset();
    const P p = params(Filter::HP24, 2000, 0.4f, 0.2f, -0.3f, 0.8f);
    Buf a = impulseAt(3000, 0), b = a, c = a, d = a;
    run(used, p, a, b);
    run(fresh, p, c, d);
    CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(b.data(), d.data(), b.size() * sizeof(float)) == 0);
}

void tail() {
    // tailSamples() covers the ring down to -60 dB of the impulse response's peak, and not by much more.
    const P sets[] = {params(Filter::LP24, 200, 1.0f), params(Filter::BP, 1000, 0.7f), params(Filter::HP12, 5000, 0.5f, 0.0f, 0.5f)};
    for (const P& p : sets) {
        Filter f;
        Buf L = impulseAt(44100 * 2, 0), R = L;
        run(f, p, L, R);
        const float pk = std::max(peak(L), peak(R));
        size_t last = 0;
        for (size_t i = 0; i < L.size(); ++i)
            if (std::fabs(L[i]) > 1e-3f * pk || std::fabs(R[i]) > 1e-3f * pk) last = i;
        CHECK(static_cast<int>(last) <= f.tailSamples());
        CHECK(f.tailSamples() < 3 * static_cast<int>(last) + 64);
    }
}

} // namespace

void filterTests() {
    slopes();
    bandAndNotch();
    resonance();
    spread();
    drive();
    mix();
    sweep();
    typeSwitch();
    extremes();
    randomJumps();
    nanInput();
    blockSizes();
    resetClears();
    tail();
}
