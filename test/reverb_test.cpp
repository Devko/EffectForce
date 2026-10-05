// dsp/reverb.h: decay time against the target (Schroeder integration), damping, evenness across
// frequency, echo density, level, predelay, freeze, stereo decorrelation and width, mix, low cut,
// modulation, and robustness (extremes, random jumps, NaN input, block sizes, reset and stale
// buffers, clicks on changes, tailSamples()).
#include "signal.h"
#include "../dsp/reverb.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

using namespace eft;
using ef::Reverb;

using P = Reverb::Params;
const char* const kModeNames[] = {"Room", "Hall", "Plate", "Space"};

int secs(double s) { return static_cast<int>(s * ef::kRate); }
size_t at(double s) { return static_cast<size_t>(secs(s)); }

// Wet only, no predelay, no low cut, no damping: the network's own decay.
P wetOnly(int mode, float decay) {
    P p;
    p.mode = mode;
    p.decayS = decay;
    p.predelayMs = 0.0f;
    p.lowCutHz = 20.0f;
    p.dampHz = 20000.0f;
    p.mix = 1.0f;
    return p;
}

struct Stereo {
    Buf L, R;
};

Stereo render(Reverb& r, const P& p, Buf L, Buf R, int chunk = ef::kChunk) {
    r.reset();
    run(r, p, L, R, {}, chunk);
    return {std::move(L), std::move(R)};
}
// Like run(), with `change(pos, p)` before each chunk's set().
template <class F>
void runChanging(Reverb& r, P p, Buf& L, Buf& R, F change) {
    for (size_t pos = 0; pos < L.size(); pos += ef::kChunk) {
        const int n = static_cast<int>(std::min<size_t>(ef::kChunk, L.size() - pos));
        change(pos, p);
        r.set(p, {});
        r.process(&L[pos], &R[pos], n);
    }
}
Stereo impulseResponse(Reverb& r, const P& p, int n) {
    const Buf x = impulseAt(n, 0, 1.0f);
    return render(r, p, x, x);
}

// An RBJ band-pass (0 dB at its centre), Q 1.4: about an octave.
Buf bandpass(const Buf& x, double hz, double q = 1.4) {
    const double w = 2.0 * kPi * hz / ef::kRate, al = std::sin(w) / (2.0 * q), a0 = 1.0 + al;
    const double b0 = al / a0, b2 = -al / a0, a1 = -2.0 * std::cos(w) / a0, a2 = (1.0 - al) / a0;
    Buf y(x.size());
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double v = b0 * x[i] + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x[i];
        y2 = y1;
        y1 = v;
        y[i] = static_cast<float>(v);
    }
    return y;
}

// Schroeder's backward integration of L^2 + R^2: the energy decay curve in dB (0 at the start).
std::vector<double> edc(const Buf& L, const Buf& R) {
    std::vector<double> e(L.size());
    double acc = 0.0;
    for (size_t i = L.size(); i-- > 0;) {
        acc += static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i];
        e[i] = acc;
    }
    const double total = e[0] > 0.0 ? e[0] : 1.0;
    for (double& v : e) v = 10.0 * std::log10(v / total + 1e-300);
    return e;
}

// The decay time from a least-squares line through the EDC between hi and lo dB (T30 for
// -5..-35), extrapolated to 60 dB. 0 if the EDC never gets there.
double decayTime(const std::vector<double>& e, double hi = -5.0, double lo = -35.0) {
    size_t a = 0;
    while (a < e.size() && e[a] > hi) ++a;
    size_t b = a;
    while (b < e.size() && e[b] > lo) ++b;
    if (b >= e.size() || b <= a + 10) return 0.0;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    const double n = static_cast<double>(b - a);
    for (size_t i = a; i < b; ++i) {
        const double x = static_cast<double>(i) / ef::kRate;
        sx += x;
        sy += e[i];
        sxx += x * x;
        sxy += x * e[i];
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);   // dB per second
    return slope < 0.0 ? -60.0 / slope : 0.0;
}
double bandDecay(const Stereo& s, double hz) { return decayTime(edc(bandpass(s.L, hz), bandpass(s.R, hz)), -5.0, -25.0); }

double rmsLR(const Stereo& s, size_t from, size_t to) {
    const double l = rms(s.L, from, to), r = rms(s.R, from, to);
    return std::sqrt(0.5 * (l * l + r * r));
}

size_t firstAbove(const Stereo& s, float level) {
    size_t i = 0;
    while (i < s.L.size() && std::fabs(s.L[i]) <= level && std::fabs(s.R[i]) <= level) ++i;
    return i;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// --- decay and damping -------------------------------------------------------------------------

// RT60 = decayS: the broadband T30 within 25% (a little short: the highs always decay a little
// faster), the 500 Hz octave's within 10% (Jot's gains set the low and mid decay exactly).
void decay() {
    Reverb r;
    struct Case {
        int mode;
        float decay;
    };
    for (const Case c : {Case{Reverb::ROOM, 1.0f}, Case{Reverb::ROOM, 4.0f}, Case{Reverb::HALL, 1.0f},
                         Case{Reverb::HALL, 4.0f}, Case{Reverb::PLATE, 2.0f}, Case{Reverb::SPACE, 2.0f}}) {
        const Stereo s = impulseResponse(r, wetOnly(c.mode, c.decay), secs(c.decay * 0.9 + 0.4));
        const double broad = decayTime(edc(s.L, s.R)), mid = bandDecay(s, 500.0);
        std::printf("  reverb: %-5s decay %.1f s: RT60 %.2f s broadband, %.2f s at 500 Hz\n", kModeNames[c.mode], c.decay, broad, mid);
        CHECK(near(broad, c.decay, 0.25 * c.decay));
        CHECK(near(mid, c.decay, 0.1 * c.decay));
    }
    // The decay doesn't depend on size or modulation: each line's gain follows its length.
    for (float size : {0.0f, 1.0f}) {
        for (float mod : {0.0f, 1.0f}) {
            P p = wetOnly(Reverb::HALL, 2.0f);
            p.size = size;
            p.mod = mod;
            const Stereo s = impulseResponse(r, p, secs(2.3));
            CHECK(near(bandDecay(s, 500.0), 2.0, 0.2));
        }
    }
}

// Above dampHz the decay shortens, as 1 + (f / dampHz)^2 at low and mid frequencies: at 3 kHz the
// 8 kHz octave dies much faster than the 500 Hz one, at 20 kHz about as fast (theory: 0.86x).
void damping() {
    Reverb r;
    P p = wetOnly(Reverb::HALL, 2.0f);
    p.mod = 0.0f;
    p.dampHz = 3000.0f;
    Stereo s = impulseResponse(r, p, secs(2.4));
    double low = bandDecay(s, 500.0), mid = bandDecay(s, 2000.0), high = bandDecay(s, 8000.0);
    std::printf("  reverb: damp 3 kHz: %.2f s at 500 Hz, %.2f s at 2 kHz, %.2f s at 8 kHz\n", low, mid, high);
    CHECK(near(low, 2.0, 0.2));
    CHECK(high < 0.5 * low);
    CHECK(mid < 0.85 * low && mid > high);   // 1 + (2/3)^2: 0.69x, the octave band averages a little up
    p.dampHz = 20000.0f;
    s = impulseResponse(r, p, secs(2.4));
    low = bandDecay(s, 500.0);
    high = bandDecay(s, 8000.0);
    std::printf("  reverb: damp 20 kHz: %.2f s at 500 Hz, %.2f s at 8 kHz\n", low, high);
    CHECK(near(low, 2.0, 0.2));
    CHECK(high > 0.78 * low && high < 1.0 * low);
    // Every mode damps alike (Space's long lines a little less at a short decay: a one-pole can't
    // take the tens of dB a pass they would need there).
    for (int mode = 0; mode < Reverb::kModes; ++mode) {
        P q = wetOnly(mode, 1.5f);
        q.dampHz = 3000.0f;
        s = impulseResponse(r, q, secs(1.8));
        CHECK(bandDecay(s, 8000.0) < 0.6 * bandDecay(s, 500.0));
    }
}

// --- smoothness --------------------------------------------------------------------------------

// No metallic ringing: the decay time in narrow bands (41 Goertzel bins, 200..2000 Hz) is even.
// Resonances (an allpass in the loop too long or too strong, coinciding line lengths) ring on
// longer at their frequencies; exponentially decaying noise itself spreads by about 6% here.
void evenDecay() {
    Reverb r;
    std::printf("  reverb: decay time in 41 narrow bands, spread and longest / median:");
    for (int mode = 0; mode < Reverb::kModes; ++mode) {
        P p = wetOnly(mode, 2.0f);
        p.mod = 0.0f;
        const Stereo s = impulseResponse(r, p, secs(1.6));
        const size_t win = 2048, hop = 1024, from = at(0.15);
        std::vector<double> rts;
        for (double hz = 200.0; hz <= 2000.0; hz += 45.0) {
            std::vector<double> e;   // energy per window, then its backward integral
            for (size_t i = from; i + win <= s.L.size(); i += hop) {
                const double l = magnitude(s.L, hz, i, i + win), rr = magnitude(s.R, hz, i, i + win);
                e.push_back(l * l + rr * rr);
            }
            for (size_t i = e.size() - 1; i-- > 0;) e[i] += e[i + 1];
            double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, n = 0.0;
            for (size_t i = 0; i < e.size(); ++i) {
                const double y = 10.0 * std::log10(e[i] / e[0]);
                if (y < -25.0) break;
                const double x = static_cast<double>(i * hop) / ef::kRate;
                sx += x;
                sy += y;
                sxx += x * x;
                sxy += x * y;
                n += 1.0;
            }
            rts.push_back(-60.0 / ((n * sxy - sx * sy) / (n * sxx - sx * sx)));
        }
        std::vector<double> sorted = rts;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        double mean = 0.0, var = 0.0;
        for (double t : rts) mean += t / static_cast<double>(rts.size());
        for (double t : rts) var += (t - mean) * (t - mean) / static_cast<double>(rts.size());
        const double spread = std::sqrt(var) / mean;
        std::printf(" %s %.1f%% %.2fx", kModeNames[mode], 100.0 * spread, sorted.back() / median);
        CHECK(near(median, 2.0, 0.2));
        CHECK(spread < 0.1);
        CHECK(sorted.back() < 1.3 * median);
    }
    std::printf("\n");
}

// A dense tail: soon after the first echoes the response is as dense as Gaussian noise (Abel and
// Huang's normalized echo density: the share of samples beyond one standard deviation in a 20 ms
// window, over the 31.7% of a Gaussian), and stays so; no sparse, fluttering stretches.
void echoDensity() {
    Reverb r;
    std::printf("  reverb: echo density 100 ms after the first echo on (Space 250 ms), of Gaussian's:");
    for (int mode = 0; mode < Reverb::kModes; ++mode) {
        const Stereo s = impulseResponse(r, wetOnly(mode, 2.0f), secs(1.0));
        const size_t first = firstAbove(s, 1e-5f), half = at(0.01);
        double lowest = 2.0;
        for (size_t c = first + at(mode == Reverb::SPACE ? 0.25 : 0.1); c + half < s.L.size(); c += half) {
            double sum = 0.0;
            for (size_t i = c - half; i < c + half; ++i) sum += static_cast<double>(s.L[i]) * s.L[i];
            const double sd = std::sqrt(sum / static_cast<double>(2 * half));
            size_t beyond = 0;
            for (size_t i = c - half; i < c + half; ++i) beyond += std::fabs(s.L[i]) > sd ? 1 : 0;
            lowest = std::min(lowest, static_cast<double>(beyond) / static_cast<double>(2 * half) / 0.3173105);
        }
        std::printf(" %s %.2f", kModeNames[mode], lowest);
        CHECK(lowest > 0.75);
    }
    std::printf("\n");
}

// The wet's level: noise through a 2.5 s tail is about as loud in every mode and size (the input
// follows the network's length), within 3 dB of -3 dB.
void level() {
    Reverb r;
    const Buf x = whiteNoise(secs(4.0), 0.5f, 21), y = whiteNoise(secs(4.0), 0.5f, 22);
    double lo = 0.0, hi = -100.0;
    for (int mode = 0; mode < Reverb::kModes; ++mode) {
        for (float size : {0.0f, 0.5f, 1.0f}) {
            P p = wetOnly(mode, 2.5f);
            p.size = size;
            p.dampHz = 6000.0f;
            p.lowCutHz = 150.0f;
            const Stereo s = render(r, p, x, y);
            const double gain = db(rmsLR(s, at(3.0), at(4.0)) / rms(x, at(3.0), at(4.0)));
            CHECK(near(gain, -3.0, 3.0));
            lo = std::min(lo, gain);
            hi = std::max(hi, gain);
        }
    }
    std::printf("  reverb: noise through a 2.5 s tail, every mode and size: wet %.1f..%.1f dB\n", lo, hi);
}

// --- timing ------------------------------------------------------------------------------------

// Nothing before the predelay; the first echo after it as soon as the shortest line (times the
// size) allows.
void predelay() {
    Reverb r;
    struct Case {
        int mode;
        float pre, size, firstMs;   // the shortest line at this size
    };
    for (const Case c : {Case{Reverb::HALL, 100.0f, 0.5f, 30.1f}, Case{Reverb::ROOM, 0.0f, 0.5f, 10.0f},
                         Case{Reverb::SPACE, 250.0f, 1.0f, 112.5f}, Case{Reverb::PLATE, 37.0f, 0.0f, 4.5f}}) {
        P p = wetOnly(c.mode, 1.0f);
        p.predelayMs = c.pre;
        p.size = c.size;
        p.lowCutHz = 150.0f;
        const Stereo s = impulseResponse(r, p, secs(0.6));
        const size_t pre = static_cast<size_t>(c.pre * ef::kRate / 1000.0f + 0.5f);
        if (pre > 0) CHECK(peak(s.L, 0, pre) == 0.0f && peak(s.R, 0, pre) == 0.0f);
        const size_t first = firstAbove(s, 1e-4f);
        CHECK(first >= pre && first <= pre + static_cast<size_t>((c.firstMs + 1.0f) * ef::kRate / 1000.0f));
    }
}

// The level 10 s after freezing within 3 dB of the level 1 s after it, never above it; the input
// (still playing) doesn't get in. Unfrozen, the tail decays again at decayS, without a jump.
void freeze() {
    Reverb r;
    P p = wetOnly(Reverb::HALL, 2.0f);
    p.dampHz = 6000.0f;
    p.lowCutHz = 150.0f;
    p.mod = 0.5f;
    Buf L = whiteNoise(secs(16), 0.5f), R = whiteNoise(secs(16), 0.5f, 7);
    for (size_t i = at(12.0); i < L.size(); ++i) L[i] = R[i] = 0.0f;   // silence from the thaw on
    r.reset();
    runChanging(r, p, L, R, [](size_t pos, P& q) { q.freeze = pos >= at(1.0) && pos < at(12.0); });
    const Stereo s{L, R};
    const double ref = rmsLR(s, at(2.0), at(2.5)), late = rmsLR(s, at(11.0), at(11.5));
    double highest = 0.0;
    for (double t = 2.0; t < 11.5; t += 0.5) highest = std::max(highest, rmsLR(s, at(t), at(t + 0.5)));
    std::printf("  reverb: frozen 1 s %.1f dB, 10 s %.1f dB, highest %.1f dB\n", db(ref), db(late), db(highest));
    CHECK(ref > 0.05);
    CHECK(near(db(late), db(ref), 3.0));
    CHECK(db(highest) < db(ref) + 1.0);
    // The thaw: no drop as it starts (the first 20 ms within 1.5 dB of the last frozen 20 ms; the
    // click test listens closer), then the decay is decayS's again.
    CHECK(near(db(rmsLR(s, at(12.0), at(12.02))), db(rmsLR(s, at(11.96), at(11.98))), 1.5));
    const Stereo after{Buf(L.begin() + secs(12.1), L.end()), Buf(R.begin() + secs(12.1), R.end())};
    const double t60 = decayTime(edc(after.L, after.R));
    std::printf("  reverb: after the thaw RT60 %.2f s\n", t60);
    CHECK(near(t60, 2.0, 0.5));
}

// --- stereo, mix, low cut, modulation --------------------------------------------------------

double correlation(const Buf& a, const Buf& b, size_t from, size_t to) {
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = from; i < to; ++i) {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return ab / std::sqrt(aa * bb + 1e-30);
}

// A mono source: at width 1 the tail's sides are uncorrelated (orthogonal taps), at width 0 the
// wet is exactly mono.
void stereo() {
    Reverb r;
    Buf burst = whiteNoise(secs(1.8), 0.5f);
    for (size_t i = at(0.1); i < burst.size(); ++i) burst[i] = 0.0f;
    std::printf("  reverb: a mono burst's tail, L/R correlation:");
    for (int mode = 0; mode < Reverb::kModes; ++mode) {
        P p = wetOnly(mode, 2.0f);
        const Stereo s = render(r, p, burst, burst);
        const double c = correlation(s.L, s.R, at(0.3), at(1.8));
        std::printf(" %s %.3f", kModeNames[mode], c);
        CHECK(std::fabs(c) < 0.5);
        p.width = 0.0f;
        const Stereo m = render(r, p, burst, burst);
        CHECK(m.L == m.R);
        CHECK(rms(m.L, at(0.3), at(1.8)) > 0.01);
    }
    std::printf("\n");
}

// Mix 0 is the dry signal bit for bit, whatever else is set; mix 1 has no dry left.
void mix() {
    Reverb r;
    const Buf L = whiteNoise(secs(1.0), 0.5f), R = whiteNoise(secs(1.0), 0.5f, 9);
    for (int mode = 0; mode < Reverb::kModes; ++mode) {
        P p;
        p.mode = mode;
        p.mix = 0.0f;
        p.mod = 1.0f;
        p.decayS = 10.0f;
        const Stereo s = render(r, p, L, R);
        CHECK(s.L == L && s.R == R);
    }
    // Mix 1 with a long predelay: silence before the wet arrives.
    P p = wetOnly(Reverb::HALL, 1.0f);
    p.predelayMs = 200.0f;
    const Stereo s = render(r, p, L, R);
    CHECK(peak(s.L, 0, at(0.2)) == 0.0f);
}

// The low cut takes the lows out of the wet (12 dB / octave), the highs stay.
void lowCut() {
    Reverb r;
    auto wetLevel = [&](double hz, float lowCut) {
        P p = wetOnly(Reverb::HALL, 1.0f);
        p.lowCutHz = lowCut;
        const Buf x = sine(hz, secs(1.5), 0.5f);
        return rmsLR(render(r, p, x, x), at(0.8), at(1.5));
    };
    CHECK(db(wetLevel(60.0, 1000.0) / wetLevel(60.0, 20.0)) < -30.0);   // theory -50 dB
    CHECK(db(wetLevel(400.0, 800.0) / wetLevel(400.0, 20.0)) < -5.0);   // an octave under: -12 dB
    CHECK(near(db(wetLevel(5000.0, 1000.0) / wetLevel(5000.0, 20.0)), 0.0, 1.0));
}

// Modulation changes the tail (and not its level): a sine comes out with sidebands around it.
void modulation() {
    Reverb r;
    auto spread = [&](float mod) {
        P p = wetOnly(Reverb::SPACE, 3.0f);
        p.mod = mod;
        const Buf x = sine(1000.0, secs(2.5), 0.5f);
        const Stereo s = render(r, p, x, x);
        // Energy beside the tone (a 1000 Hz sine: everything else is what the modulation made).
        const Buf side = bandpass(s.L, 1000.0, 200.0);
        double off = 0.0, on = 0.0;
        for (size_t i = at(1.5); i < s.L.size(); ++i) {
            on += static_cast<double>(s.L[i]) * s.L[i];
            const double d = static_cast<double>(s.L[i]) - side[i];
            off += d * d;
        }
        return std::make_pair(db(std::sqrt(off / on)), db(rms(s.L, at(1.5), s.L.size())));
    };
    const auto still = spread(0.0f), moving = spread(1.0f);
    std::printf("  reverb: a 1 kHz sine's energy off the tone: mod 0 %.1f dB, mod 1 %.1f dB\n", still.first, moving.first);
    CHECK(moving.first > still.first + 10.0);
    CHECK(near(moving.second, still.second, 1.5));
}

// --- robustness --------------------------------------------------------------------------------

// Every mode at the extremes with full-scale noise for 3 s: finite and bounded.
void extremes() {
    Reverb r;
    const Buf L = whiteNoise(secs(3.0), 1.0f, 3), R = whiteNoise(secs(3.0), 1.0f, 4);
    float worst = 0.0f;
    for (int mode = 0; mode < Reverb::kModes; ++mode) {
        for (int c = 0; c < 4; ++c) {
            P p;
            p.mode = mode;
            p.mix = 1.0f;
            p.mod = 1.0f;
            p.size = c == 0 || c == 2 ? 1.0f : 0.0f;
            p.decayS = c == 1 ? 0.1f : 30.0f;
            p.dampHz = c == 1 ? 1000.0f : 20000.0f;
            p.lowCutHz = c == 1 ? 1000.0f : 20.0f;
            p.predelayMs = c == 0 ? 250.0f : 0.0f;
            p.width = c == 1 ? 0.0f : 1.0f;
            Buf l = L, rr = R;
            r.reset();
            runChanging(r, p, l, rr, [c](size_t pos, P& q) { q.freeze = c == 2 && pos >= at(1.0); });
            CHECK(allFinite(l) && allFinite(rr));
            worst = std::max({worst, peak(l), peak(rr)});
        }
    }
    std::printf("  reverb: full-scale noise at the extremes: peak %.2f\n", worst);
    CHECK(worst < 20.0f);
}

// New random parameters every chunk (out of range and NaN too): finite and bounded.
void randomJumps() {
    Reverb r;
    Buf L = whiteNoise(secs(3.0), 1.0f, 11), R = whiteNoise(secs(3.0), 1.0f, 12);
    uint32_t seed = 99;
    auto uni = [&](float lo, float hi) { return lo + (hi - lo) * (0.5f + 0.5f * ef::randBipolar(seed)); };
    runChanging(r, P{}, L, R, [&](size_t, P& p) {
        p.mode = static_cast<int>(uni(-2.0f, 6.0f));
        p.size = uni(-0.5f, 1.5f);
        p.decayS = uni(-1.0f, 40.0f);
        p.predelayMs = uni(-10.0f, 300.0f);
        p.dampHz = uni(500.0f, 25000.0f);
        p.lowCutHz = uni(0.0f, 1500.0f);
        p.mod = uni(-0.5f, 1.5f);
        p.width = uni(-1.0f, 2.0f);
        p.freeze = uni(0.0f, 1.0f) < 0.2f;
        p.mix = uni(-0.2f, 1.2f);
        if (uni(0.0f, 1.0f) < 0.05f) p.size = p.decayS = p.dampHz = p.mod = p.mix = std::nanf("");
    });
    CHECK(allFinite(L) && allFinite(R));
    CHECK(peak(L) < 20.0f && peak(R) < 20.0f);
}

// A NaN or infinity in the input is a 0 to the reverb: the output is the same as with a 0 there.
void nanInput() {
    Reverb r;
    P p;
    p.mix = 0.5f;
    Buf L = whiteNoise(secs(1.0), 0.5f), R = whiteNoise(secs(1.0), 0.5f, 5);
    Buf L0 = L, R0 = R;
    L[1000] = std::nanf("");
    R[3000] = INFINITY;
    L[5000] = -INFINITY;
    L0[1000] = R0[3000] = L0[5000] = 0.0f;
    const Stereo a = render(r, p, L, R), b = render(r, p, L0, R0);
    CHECK(allFinite(a.L) && allFinite(a.R));
    CHECK(a.L == b.L && a.R == b.R);
}

// Constant parameters: the same output whatever the block size (everything per sample is the same).
void blockSizes() {
    Reverb r;
    P p;
    p.mod = 1.0f;
    p.mix = 0.5f;
    const Buf L = whiteNoise(secs(1.0), 0.5f), R = whiteNoise(secs(1.0), 0.5f, 2);
    const Stereo ref = render(r, p, L, R, 32);
    for (int chunk : {1, 7}) {
        const Stereo s = render(r, p, L, R, chunk);
        CHECK(s.L == ref.L && s.R == ref.R);
    }
}

// reset() clears everything: afterwards it plays exactly as a new one.
void resetClears() {
    Reverb used, fresh;
    P p;
    p.mix = 1.0f;
    p.freeze = true;
    Buf L = whiteNoise(secs(0.5), 0.5f), R = L;
    run(used, p, L, R);
    p.freeze = false;
    p.mode = Reverb::SPACE;
    used.reset();
    Buf a = impulseAt(secs(1.0), 10), b = a, c = a, d = a;
    run(used, p, a, b);
    run(fresh, p, c, d);
    CHECK(a == c && b == d);
}

// reset() clears nothing big (the rack calls it on the audio thread), yet nothing from before it
// ever comes out: a loud long tail, reset, then silence in, with the size, predelay and mode moving
// so that the reads sweep the old buffers. Out comes silence (but for the -400 dB that keeps the
// states out of the denormals). The same after a mode change, which starts the network afresh.
void noStaleTail() {
    Reverb r;
    P p;
    p.mode = Reverb::SPACE;
    p.size = 1.0f;
    p.decayS = 30.0f;
    p.dampHz = 20000.0f;
    p.mod = 1.0f;
    p.predelayMs = 250.0f;
    p.mix = 1.0f;
    Buf L = whiteNoise(secs(1.5), 1.0f, 31), R = whiteNoise(secs(1.5), 1.0f, 32);
    run(r, p, L, R);
    CHECK(rms(L, at(1.0), at(1.5)) > 0.1);
    r.reset();
    Buf l(at(3.0), 0.0f), rr(at(3.0), 0.0f);
    runChanging(r, p, l, rr, [](size_t pos, P& q) {
        const double t = static_cast<double>(pos) / ef::kRate;
        q.size = t < 0.5 ? 1.0f : (t < 1.5 ? 0.0f : 1.0f);
        q.predelayMs = t < 1.0 ? 250.0f : 100.0f;
        q.mode = t < 2.0 ? Reverb::SPACE : Reverb::HALL;
    });
    CHECK(peak(l) < 1e-12f && peak(rr) < 1e-12f);

    // A mode change: the old tail fades out (12 ms) and never comes back. (The input stops half a
    // second before, so the low cut's own state has settled: what is still in the input path goes
    // on into the new mode, as it should.)
    p = wetOnly(Reverb::HALL, 30.0f);
    p.lowCutHz = 150.0f;
    L = whiteNoise(secs(3.0), 1.0f, 33);
    R = whiteNoise(secs(3.0), 1.0f, 34);
    for (size_t i = at(0.5); i < L.size(); ++i) L[i] = R[i] = 0.0f;
    r.reset();
    runChanging(r, p, L, R, [](size_t pos, P& q) { q.mode = pos < at(1.0) ? Reverb::HALL : Reverb::SPACE; });
    CHECK(rms(L, at(0.9), at(1.0)) > 0.1);
    CHECK(peak(L, at(1.02)) < 1e-12f && peak(R, at(1.02)) < 1e-12f);
}

// With nothing moving, the network takes its quicker loop (the gains and poles held still).
void steadyPath() {
    Reverb r;
    P p;
    Buf L = whiteNoise(secs(0.2), 0.5f), R = L;
    bool moved = false;
    runChanging(r, p, L, R, [&](size_t pos, P&) { moved = moved || (pos > 0 && r.gliding()); });
    CHECK(!moved && !r.gliding());
    runChanging(r, p, L, R, [](size_t pos, P& q) { q.decayS = pos % 64 == 0 ? 2.0f : 3.0f; });
    CHECK(r.gliding());
}

// tailSamples(): the impulse response is down 60 dB before it, and gone (-80 dB) after it.
void tail() {
    Reverb r;
    for (float decay : {1.0f, 4.0f}) {
        P p = wetOnly(Reverb::HALL, decay);
        p.predelayMs = 50.0f;
        p.dampHz = 6000.0f;
        r.reset();
        r.set(p, {});
        const int n = r.tailSamples();
        const Stereo s = impulseResponse(r, p, n + secs(0.5));
        const size_t win = at(0.02);
        double top = 0.0;
        size_t t60 = 0;
        for (size_t i = 0; i + win <= s.L.size(); i += win) top = std::max(top, rmsLR(s, i, i + win));
        for (size_t i = 0; i + win <= s.L.size(); i += win)
            if (rmsLR(s, i, i + win) > top * 1e-3) t60 = i + win;
        std::printf("  reverb: decay %.0f s: tailSamples %.2f s, -60 dB at %.2f s\n", decay, n / ef::kRate, t60 / ef::kRate);
        CHECK(static_cast<size_t>(n) > t60 && static_cast<size_t>(n) < 2 * t60);
        CHECK(rmsLR(s, static_cast<size_t>(n), s.L.size()) < top * 1e-4);
    }
    P p;
    p.freeze = true;
    r.set(p, {});
    CHECK(r.tailSamples() > secs(3600.0));
}

// No clicks: a low sine through the wet while one thing changes. The wet stays a (modulated) sine:
// no sample-to-sample step much beyond what its own level allows.
void clicks() {
    Reverb r;
    struct Change {
        const char* what;
        void (*apply)(P&, double);   // t seconds after the change
        bool fade;   // a crossfade across one chunk (the module contract): up to 2 / 32 of the level more
    };
    const Change changes[] = {
        {"size", [](P& p, double) { p.size = 1.0f; }, false},
        {"predelay", [](P& p, double) { p.predelayMs = 250.0f; }, false},
        {"mode", [](P& p, double) { p.mode = Reverb::PLATE; }, false},
        {"freeze and thaw", [](P& p, double t) { p.freeze = t < 0.4; }, false},
        {"mod", [](P& p, double) { p.mod = 1.0f; }, false},
        {"low cut", [](P& p, double) { p.lowCutHz = 1000.0f; }, false},
        {"damp", [](P& p, double) { p.dampHz = 1000.0f; }, false},
        {"decay", [](P& p, double) { p.decayS = 30.0f; }, false},
        {"width", [](P& p, double) { p.width = 0.0f; }, true},
        {"mix", [](P& p, double) { p.mix = 0.0f; }, true},
    };
    const double hz = 150.0, sineStep = 2.0 * std::sin(kPi * hz / ef::kRate) * 1.3;   // a sine's step, and a pitch bend
    for (const Change& c : changes) {
        P p = wetOnly(Reverb::HALL, 2.0f);
        p.size = 0.0f;
        p.dampHz = 2000.0f;
        Buf L = sine(hz, secs(2.0), 0.5f), R = L;
        r.reset();
        runChanging(r, p, L, R, [&](size_t pos, P& q) {
            if (pos >= at(1.0)) c.apply(q, static_cast<double>(pos - at(1.0)) / ef::kRate);
        });
        const double bound = sineStep + (c.fade ? 2.0 / ef::kChunk : 0.0);
        bool ok = true;
        for (size_t w = at(0.5); w + 512 <= L.size(); w += 512) {
            const float lim = static_cast<float>(bound) * std::max(peak(L, w - 512, w + 512), peak(R, w - 512, w + 512)) + 1e-4f;
            ok = ok && maxStep(L, w, w + 512) <= lim && maxStep(R, w, w + 512) <= lim;
        }
        if (!ok) std::printf("  reverb: a click on changing %s\n", c.what);
        CHECK(ok);
    }
}

} // namespace

void reverbTests() {
    decay();
    damping();
    evenDecay();
    echoDensity();
    level();
    predelay();
    freeze();
    stereo();
    mix();
    lowCut();
    modulation();
    extremes();
    randomJumps();
    nanInput();
    blockSizes();
    resetClears();
    noStaleTail();
    steadyPath();
    tail();
    clicks();
}
