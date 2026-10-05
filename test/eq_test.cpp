// dsp/eq.h: responses against the RBJ cookbook's analog prototypes under the prewarped bilinear
// transform (what Simper's SVF mixes are), the flat bit-exact pass-through, switching bands in and
// out, sweeps, and robustness (extremes, random jumps, NaN input, block sizes, reset).
#include "signal.h"
#include "../dsp/eq.h"

#include <complex>
#include <cstring>

namespace {

using namespace eft;
using ef::Eq;

using P = Eq::Params;
using cd = std::complex<double>;

double gain(Eq& eq, const P& p, double hz) { return gainAt(eq, p, hz, 32768, 16384); }

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The analog prototypes at the frequency the bilinear transform maps hz to, with the corner fc
// landing exactly: s = j tan(pi hz / rate) / tan(pi fc / rate).
cd sAt(double hz, double fc) { return cd(0.0, std::tan(eft::kPi * hz / ef::kRate) / std::tan(eft::kPi * fc / ef::kRate)); }
double bellDb(double hz, double f0, double q, double gdb) {
    const cd s = sAt(hz, f0);
    const double a = std::pow(10.0, gdb / 40.0);
    return db(std::abs((s * s + s * (a / q) + 1.0) / (s * s + s / (a * q) + 1.0)));
}
double lowShelfDb(double hz, double fc, double gdb) {
    const cd s = sAt(hz, fc);
    const double a = std::pow(10.0, gdb / 40.0), b = std::sqrt(2.0 * a);
    return db(std::abs(a * (s * s + b * s + a) / (a * s * s + b * s + 1.0)));
}
double highShelfDb(double hz, double fc, double gdb) {
    const cd s = sAt(hz, fc);
    const double a = std::pow(10.0, gdb / 40.0), b = std::sqrt(2.0 * a);
    return db(std::abs(a * (a * s * s + b * s + 1.0) / (s * s + b * s + a)));
}
double butterDb(double hz, double fc, bool high) {
    const cd s = sAt(hz, fc);
    const cd d = s * s + std::sqrt(2.0) * s + 1.0;
    return db(std::abs((high ? s * s : cd(1.0)) / d));
}

// The largest third difference: a sine of amplitude a at hz makes at most a (2 sin(pi hz / rate))^3,
// a step or a kink makes about its own size.
double maxD3(const Buf& y, size_t from = 3) {
    double m = 0.0;
    for (size_t i = std::max<size_t>(from, 3); i < y.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(y[i]) - 3.0 * y[i - 1] + 3.0 * y[i - 2] - y[i - 3]));
    return m;
}
double sineD3(double amp, double hz) { return amp * std::pow(2.0 * std::sin(eft::kPi * hz / ef::kRate), 3.0); }
double sineStep(double amp, double hz) { return amp * 2.0 * std::sin(eft::kPi * hz / ef::kRate); }

bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }

P bell(float hz, float gdb, float q) {
    P p;
    p.midFreq = hz;
    p.midGainDb = gdb;
    p.midQ = q;
    return p;
}

void bellBand() {
    Eq eq;
    // Its gain at the centre is the gain asked for, for any Q; around it the cookbook's peaking EQ.
    for (float f0 : {100.0f, 1000.0f, 10000.0f}) {
        for (float q : {0.3f, 1.0f, 4.0f, 8.0f}) {
            for (float g : {-18.0f, -6.0f, 6.0f, 18.0f}) {
                const P p = bell(f0, g, q);
                CHECK(near(gain(eq, p, f0), g, 0.3));
                CHECK(near(gain(eq, p, f0), g, 0.02));
                for (double r : {0.5, 0.9, 1.5}) CHECK(near(gain(eq, p, f0 * r), bellDb(f0 * r, f0, q, g), 0.1));
            }
        }
    }
    // Boost and cut at the same Q are mirror images.
    CHECK(near(gain(eq, bell(1000, 9, 2), 800), -gain(eq, bell(1000, -9, 2), 800), 0.02));
}

void shelves() {
    Eq eq;
    // Low shelf, +12 at 300 Hz: the full gain a decade below, nothing a decade above, half at the corner.
    for (float g : {12.0f, -12.0f, 18.0f}) {
        P p;
        p.lowFreq = 300.0f;
        p.lowGainDb = g;
        CHECK(near(gain(eq, p, 30), g, 1.0));
        CHECK(near(gain(eq, p, 3000), 0.0, 0.1));
        CHECK(near(gain(eq, p, 300), 0.5 * g, 0.05));
        for (double hz : {30.0, 100.0, 200.0, 300.0, 450.0, 1000.0, 3000.0}) CHECK(near(gain(eq, p, hz), lowShelfDb(hz, 300, g), 0.1));
        // Q 0.707: no overshoot on either side.
        double prev = g > 0 ? 1e9 : -1e9;
        bool monotonic = true;
        for (double hz = 20.0; hz < 20000.0; hz *= 1.5) {
            const double v = gain(eq, p, hz);
            monotonic = monotonic && (g > 0 ? v <= prev + 1e-3 : v >= prev - 1e-3);
            prev = v;
        }
        CHECK(monotonic);
    }
    // High shelf at 3 kHz, the mirror image.
    for (float g : {12.0f, -12.0f, 18.0f}) {
        P p;
        p.highFreq = 3000.0f;
        p.highGainDb = g;
        CHECK(near(gain(eq, p, 16000), g, 1.0));
        CHECK(near(gain(eq, p, 300), 0.0, 0.1));
        CHECK(near(gain(eq, p, 3000), 0.5 * g, 0.05));
        for (double hz : {300.0, 1000.0, 2000.0, 3000.0, 4500.0, 9000.0, 16000.0}) CHECK(near(gain(eq, p, hz), highShelfDb(hz, 3000, g), 0.1));
    }
    // The extremes of the corners.
    P lo;
    lo.lowFreq = 30.0f;
    lo.lowGainDb = -18.0f;
    CHECK(near(gain(eq, lo, 30), -9.0, 0.05) && near(gain(eq, lo, 1000), 0.0, 0.05));
    P hi;
    hi.highFreq = 16000.0f;
    hi.highGainDb = 18.0f;
    CHECK(near(gain(eq, hi, 16000), 9.0, 0.05) && near(gain(eq, hi, 1000), 0.0, 0.05));
}

void cuts() {
    Eq eq;
    // Low cut: Butterworth, -3 dB at the cutoff, 12 dB / octave below.
    for (float fc : {40.0f, 200.0f, 1000.0f}) {
        P p;
        p.lowCutHz = fc;
        CHECK(near(gain(eq, p, fc), -3.01, 0.5));
        for (double r : {0.25, 0.5, 1.0, 2.0, 10.0}) CHECK(near(gain(eq, p, fc * r), butterDb(fc * r, fc, true), 0.1));
    }
    P lc;
    lc.lowCutHz = 400.0f;
    CHECK(near(gain(eq, lc, 50) - gain(eq, lc, 25), 12.0, 0.2));
    // High cut: the same above.
    for (float fc : {1000.0f, 5000.0f, 15000.0f}) {
        P p;
        p.highCutHz = fc;
        CHECK(near(gain(eq, p, fc), -3.01, 0.5));
        for (double r : {0.1, 0.5, 1.0, 1.25, 2.0}) {
            if (fc * r < 20000.0) CHECK(near(gain(eq, p, fc * r), butterDb(fc * r, fc, false), 0.1));
        }
    }
    P hc;
    hc.highCutHz = 1000.0f;
    CHECK(near(gain(eq, hc, 2000) - gain(eq, hc, 4000), 12.0, 0.5));
    // Off at the ends of their ranges.
    P off;
    off.lowCutHz = 20.0f;
    off.highCutHz = 20000.0f;
    CHECK(gain(eq, off, 25) == 0.0 && gain(eq, off, 19000) == 0.0);
}

// Runs eq over L / R with set() every chunk from params(chunk index).
template <class F>
void runWith(Eq& eq, Buf& L, Buf& R, F params) {
    for (size_t pos = 0, c = 0; pos < L.size(); pos += ef::kChunk, ++c) {
        const int n = static_cast<int>(std::min<size_t>(ef::kChunk, L.size() - pos));
        eq.set(params(c), {});
        eq.process(&L[pos], &R[pos], n);
    }
}

void flat() {
    // Flat: the input, bit for bit (NaN included: nothing touches it).
    Eq eq;
    Buf in = whiteNoise(10000, 1.0f, 3);
    in[500] = std::nanf("");
    in[600] = INFINITY;
    Buf L = in, R = in;
    run(eq, P{}, L, R);
    CHECK(same(L, in) && same(R, in));
    // Back to flat after every band was busy: bit-exact again once the glides have landed.
    eq.reset();
    const Buf noise = whiteNoise(44100, 0.5f, 4);
    L = noise;
    R = noise;
    runWith(eq, L, R, [](size_t c) {
        P p;
        if (c < 300) {
            p.lowCutHz = 80.0f;
            p.lowGainDb = 6.0f;
            p.midGainDb = -4.0f;
            p.highGainDb = 3.0f;
            p.highCutHz = 9000.0f;
        }
        return p;
    });
    const size_t landed = (300 + 1) * ef::kChunk;
    CHECK(!same(L, noise));
    CHECK(std::memcmp(&L[landed], &noise[landed], (noise.size() - landed) * sizeof(float)) == 0);
    CHECK(std::memcmp(&R[landed], &noise[landed], (noise.size() - landed) * sizeof(float)) == 0);
}

void switching() {
    // Bands in and out every 8 chunks under a sine: the change glides over a chunk, so no step
    // much beyond the sine's own (switched in one sample instead: 10..36 times it, measured), and
    // between, once the glide out has landed, the input itself.
    struct Case {
        double hz;
        P on;
    };
    P shelf, lowCut, highCut;
    shelf.lowFreq = 200.0f;
    shelf.lowGainDb = 6.0f;
    lowCut.lowCutHz = 100.0f;
    highCut.highCutHz = 2000.0f;
    const Case cases[] = {{100, shelf}, {300, lowCut}, {300, highCut}, {300, bell(300, -18, 2)}};
    for (const Case& k : cases) {
        Eq eq;
        const Buf in = sine(k.hz, 44100 / 2, 0.25f);
        Buf L = in, R = in;
        runWith(eq, L, R, [&](size_t c) { return (c / 8) % 2 ? k.on : P{}; });
        CHECK(allFinite(L));
        CHECK(maxStep(L) <= 2.0 * sineStep(peak(L), k.hz));
        bool exact = true;   // chunks 1..7 of every flat stretch
        for (size_t c = 16; c + 8 <= L.size() / ef::kChunk; c += 16)
            exact = exact && std::memcmp(&L[(c + 1) * ef::kChunk], &in[(c + 1) * ef::kChunk], 7 * ef::kChunk * sizeof(float)) == 0;
        CHECK(exact);
    }
}

// The bell swept exponentially 100 Hz .. 10 kHz every 50 ms over a sine at hz: shape 0 up and back
// down, 1 up and a jump back.
void sweepRun(Eq& eq, float gdb, float q, double hz, int shape, Buf& L, Buf& R) {
    const int period = 2205;
    eq.reset();
    L = sine(hz, 44100 / 2, 0.1f);
    R = sine(hz, 44100 / 2, 0.1f, 1.0);
    runWith(eq, L, R, [&](size_t c) {
        const int pos = static_cast<int>(c) * ef::kChunk;
        double ph = static_cast<double>(pos % period) / period;
        if (shape == 0 && (pos / period) % 2) ph = 1.0 - ph;
        return bell(static_cast<float>(100.0 * std::pow(100.0, ph)), gdb, q);
    });
}

void sweeps() {
    // The bell's frequency sweeping its whole range in 50 ms at +-18 dB: no step beyond the sine's
    // own, and (where the bell doesn't ring by itself) no kink: a third difference near a sine's,
    // where a frequency stepped once a chunk read 40..57 times it (measured, 200 Hz, Q <= 1).
    // R's sine starts out of silence with a step, so the measurements skip the first sweep.
    const size_t onset = 2205;
    Eq eq;
    Buf L, R;
    for (float g : {-18.0f, 18.0f}) {
        for (float q : {0.3f, 1.0f, 8.0f}) {
            for (double hz : {200.0, 2000.0}) {
                sweepRun(eq, g, q, hz, 0, L, R);
                CHECK(allFinite(L) && allFinite(R));
                const double pk = std::max(peak(L), peak(R));
                CHECK(std::max(maxStep(L, onset), maxStep(R, onset)) <= 1.25 * sineStep(pk, hz));
                if (hz < 1000.0 && q <= 1.0f) CHECK(std::max(maxD3(L, onset), maxD3(R, onset)) <= 10.0 * sineD3(pk, hz));
                // Jumping back every 50 ms instead: finite, and steps still near the sine's.
                sweepRun(eq, g, q, hz, 1, L, R);
                CHECK(allFinite(L) && allFinite(R));
                CHECK(std::max(maxStep(L, onset), maxStep(R, onset)) <= 2.0 * sineStep(std::max(peak(L), peak(R)), hz));
            }
        }
    }
}

void extremes() {
    // Loud noise at every corner of the parameter space.
    Eq eq;
    float worst = 0.0f;
    bool finite = true;
    for (float lc : {20.0f, 1000.0f})
        for (float lf : {30.0f, 500.0f})
            for (float lg : {-18.0f, 18.0f})
                for (float mf : {100.0f, 10000.0f})
                    for (float mg : {-18.0f, 18.0f})
                        for (float mq : {0.3f, 8.0f})
                            for (float hf : {1000.0f, 16000.0f})
                                for (float hg : {-18.0f, 18.0f})
                                    for (float hc : {1000.0f, 20000.0f}) {
                                        const P p = {lc, lf, lg, mf, mg, mq, hf, hg, hc};
                                        eq.reset();
                                        Buf L = whiteNoise(8192, 1.0f, 5), R = whiteNoise(8192, 1.0f, 6);
                                        run(eq, p, L, R);
                                        finite = finite && allFinite(L) && allFinite(R);
                                        worst = std::max(worst, std::max(peak(L), peak(R)));
                                    }
    CHECK(finite);
    CHECK(worst < 200.0f);   // a Q 8 bell at +18 on a +18 shelf rings up to 65 (measured)
    // Five seconds of every boost stacked around one frequency.
    const P stack = {1000.0f, 500.0f, 18.0f, 700.0f, 18.0f, 8.0f, 1000.0f, 18.0f, 1000.0f};
    eq.reset();
    Buf L = whiteNoise(5 * 44100, 1.0f, 15), R = whiteNoise(5 * 44100, 1.0f, 16);
    run(eq, stack, L, R);
    CHECK(allFinite(L) && allFinite(R) && peak(L) < 200.0f && peak(R) < 200.0f);
    // Out-of-range and non-finite parameters are clamped.
    for (float w : {-1e30f, -5.0f, 1e9f, INFINITY, -INFINITY, std::nanf("")}) {
        eq.reset();
        Buf A = whiteNoise(4096, 1.0f, 17), B = A;
        run(eq, P{w, w, w, w, w, w, w, w, w}, A, B);
        CHECK(allFinite(A) && allFinite(B) && peak(A) < 200.0f);
    }
}

void randomJumps() {
    // Every parameter somewhere new every chunk (out of range too, bands dropping to 0 dB and
    // back), under loud noise, for 3 s.
    Eq eq;
    uint32_t s = 777;
    const int n = 44100 * 3;
    Buf L = whiteNoise(n, 1.0f, 8), R = whiteNoise(n, 1.0f, 9);
    const auto r = [&](float lo, float hi) { return lo + (hi - lo) * (0.5f + 0.5f * ef::randBipolar(s)); };
    runWith(eq, L, R, [&](size_t) {
        P p = {r(0, 1200), r(10, 600), r(-25, 25), r(50, 12000), r(-25, 25), r(0.1f, 10), r(500, 18000), r(-25, 25), r(800, 22000)};
        if (ef::xorshift(s) % 4 == 0) p.lowGainDb = p.midGainDb = p.highGainDb = 0.0f;
        if (ef::xorshift(s) % 50 == 0) p.midFreq = std::nanf("");
        if (ef::xorshift(s) % 50 == 0) p.midQ = INFINITY;
        return p;
    });
    CHECK(allFinite(L) && allFinite(R));
    CHECK(peak(L) < 200.0f && peak(R) < 200.0f);   // 19 measured
}

P busy() {
    P p;
    p.lowCutHz = 60.0f;
    p.lowFreq = 150.0f;
    p.lowGainDb = 5.0f;
    p.midFreq = 900.0f;
    p.midGainDb = -7.0f;
    p.midQ = 3.0f;
    p.highFreq = 5000.0f;
    p.highGainDb = 4.0f;
    p.highCutHz = 12000.0f;
    return p;
}

void nanInput() {
    // NaN and infinities in the input act as silence for the filters: the output is exactly what
    // zeros there give, finite throughout. Absurd finite levels stay finite too.
    Eq a, b;
    Buf clean = sine(300, 8192, 0.5f);
    clean[1000] = clean[2000] = clean[3000] = 0.0f;
    Buf bad = clean;
    bad[1000] = std::nanf("");
    bad[2000] = INFINITY;
    bad[3000] = -INFINITY;
    Buf cl = clean, cr = clean, bl = bad, br = bad;
    run(a, busy(), cl, cr);
    run(b, busy(), bl, br);
    CHECK(allFinite(bl) && allFinite(br));
    CHECK(same(cl, bl) && same(cr, br));
    // Only the bell on: the first band that runs is the one that cleans the input.
    a.reset();
    b.reset();
    cl = clean, cr = clean, bl = bad, br = bad;
    run(a, bell(500, 12, 4), cl, cr);
    run(b, bell(500, 12, 4), bl, br);
    CHECK(allFinite(bl) && same(cl, bl));

    Buf huge = whiteNoise(4096, 1.0f, 21), h2 = huge;
    huge[100] = 1e30f;
    huge[200] = -3e38f;
    a.reset();
    run(a, busy(), huge, h2);
    CHECK(allFinite(huge) && allFinite(h2));
}

void blockSizes() {
    // Constant parameters: chunks of 1, 7 and 32 give the same output, bit for bit.
    for (const P& p : {busy(), bell(8000, -18, 8), P{}}) {
        Eq eq;
        const Buf inL = whiteNoise(4000, 0.7f, 31), inR = whiteNoise(4000, 0.7f, 32);
        Buf L32 = inL, R32 = inR;
        run(eq, p, L32, R32, {}, 32);
        for (int chunk : {1, 7}) {
            eq.reset();
            Buf L = inL, R = inR;
            run(eq, p, L, R, {}, chunk);
            CHECK(same(L, L32) && same(R, R32));
        }
    }
}

void resetClears() {
    // After reset() the EQ is as good as new: no state, and the next set() jumps.
    Eq used, fresh;
    Buf L = whiteNoise(5000, 1.0f, 41), R = L;
    run(used, bell(100, 18, 8), L, R);
    used.reset();
    Buf a = impulseAt(3000, 0), b = a, c = a, d = a;
    run(used, busy(), a, b);
    run(fresh, busy(), c, d);
    CHECK(same(a, c) && same(b, d));
    CHECK(used.tailSamples() == fresh.tailSamples());
}

// How long the output stays above -60 dB once a sine at hz stops, against its level before.
int ringAfterSine(Eq& eq, const P& p, double hz, int hold) {
    eq.reset();
    const size_t stop = static_cast<size_t>(hold);
    Buf L = sine(hz, hold + 3 * hold + 44100, 0.25f);
    std::fill(L.begin() + static_cast<std::ptrdiff_t>(stop), L.end(), 0.0f);
    Buf R = L;
    run(eq, p, L, R);
    const float before = peak(L, stop - 4410, stop);
    size_t last = stop;
    for (size_t i = stop; i < L.size(); ++i)
        if (std::fabs(L[i]) > 1e-3f * before) last = i;
    return static_cast<int>(last - stop);
}
// How long the impulse response stays above -60 dB of its peak.
int ringOfImpulse(Eq& eq, const P& p, int len) {
    eq.reset();
    Buf L = impulseAt(len, 0), R = L;
    run(eq, p, L, R);
    const float pk = peak(L);
    size_t last = 0;
    for (size_t i = 0; i < L.size(); ++i)
        if (std::fabs(L[i]) > 1e-3f * pk) last = i;
    return static_cast<int>(last);
}

void tail() {
    // tailSamples() covers the ring after a sine at the band's frequency stops (-60 dB of the level
    // before), and isn't more than 3 times it; the impulse response is shorter still. Flat: 0.
    Eq eq;
    struct Case {
        P p;
        double hz;
    };
    P ls, hs, lc, hc, ls2;
    ls.lowFreq = 30.0f;
    ls.lowGainDb = 18.0f;
    ls2.lowFreq = 500.0f;
    ls2.lowGainDb = -18.0f;
    hs.highFreq = 3000.0f;
    hs.highGainDb = 12.0f;
    lc.lowCutHz = 200.0f;
    hc.highCutHz = 1000.0f;
    const Case cases[] = {{bell(100, 18, 8), 100}, {bell(1000, 18, 1), 1000}, {bell(100, -18, 0.3f), 100}, {bell(1000, -18, 8), 1000},
                          {bell(1000, -1, 8), 1000}, {bell(300, 6, 0.3f), 300}, {ls, 30}, {ls2, 500}, {hs, 3000}, {lc, 200}, {hc, 1000}, {busy(), 900}};
    for (const Case& k : cases) {
        eq.reset();
        Buf L(64, 0.0f), R = L;
        run(eq, k.p, L, R);
        const int est = eq.tailSamples();
        const int ring = ringAfterSine(eq, k.p, k.hz, 2 * est + 8192);
        CHECK(ring <= est);
        CHECK(est < 3 * ring + 64);
        CHECK(ringOfImpulse(eq, k.p, 3 * est + 44100) <= est);
    }
    // The longest: +18 dB at Q 8 and 100 Hz rings for about half a second (poles' Q 22.6).
    eq.reset();
    Buf L(64, 0.0f), R = L;
    run(eq, bell(100, 18, 8), L, R);
    CHECK(eq.tailSamples() > ef::kRate / 2);
    // Nothing to ring: flat (whatever the frequencies), cuts off, and after reset().
    P flat;
    flat.lowFreq = 30.0f;
    flat.midFreq = 100.0f;
    flat.midQ = 8.0f;
    run(eq, flat, L, R);
    CHECK(eq.tailSamples() == 0);
    run(eq, bell(100, 18, 8), L, R);
    eq.reset();
    CHECK(eq.tailSamples() == 0);
}

} // namespace

void eqTests() {
    bellBand();
    shelves();
    cuts();
    flat();
    switching();
    sweeps();
    extremes();
    randomJumps();
    nanInput();
    blockSizes();
    resetClears();
    tail();
}
