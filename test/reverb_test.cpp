// dsp/reverb.h: decay time against the target (Schroeder integration), damping, evenness across
// frequency, echo density, level, predelay, freeze, stereo decorrelation and width, mix, low cut,
// modulation, shimmer (pitch, bounds, clicks, off bit for bit as before), and robustness
// (extremes, random jumps, NaN input, block sizes, reset and stale buffers, clicks on changes,
// tailSamples()).
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
        p.shimmer = uni(-0.5f, 1.5f);
        p.shimmerInterval = static_cast<int>(uni(-2.0f, 6.0f));
        if (uni(0.0f, 1.0f) < 0.05f) p.size = p.decayS = p.dampHz = p.mod = p.mix = p.shimmer = std::nanf("");
    });
    CHECK(allFinite(L) && allFinite(R));
    CHECK(peak(L) < 20.0f && peak(R) < 20.0f);
}

// A NaN or infinity in the input is a 0 to the reverb: the output is the same as with a 0 there.
void nanInput() {
    Reverb r;
    for (float shimmer : {0.0f, 0.6f}) {
        P p;
        p.mix = 0.5f;
        p.shimmer = shimmer;
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
}

// Constant parameters: the same output whatever the block size (everything per sample is the same).
void blockSizes() {
    Reverb r;
    for (float shimmer : {0.0f, 0.7f}) {
        P p;
        p.mod = 1.0f;
        p.mix = 0.5f;
        p.shimmer = shimmer;
        p.shimmerInterval = Reverb::UP_FIFTH;
        const Buf L = whiteNoise(secs(1.0), 0.5f), R = whiteNoise(secs(1.0), 0.5f, 2);
        const Stereo ref = render(r, p, L, R, 32);
        for (int chunk : {1, 7}) {
            const Stereo s = render(r, p, L, R, chunk);
            CHECK(s.L == ref.L && s.R == ref.R);
        }
    }
}

// reset() clears everything: afterwards it plays exactly as a new one.
void resetClears() {
    Reverb used, fresh;
    P p;
    p.mix = 1.0f;
    p.freeze = true;
    p.shimmer = 1.0f;
    Buf L = whiteNoise(secs(0.5), 0.5f), R = L;
    run(used, p, L, R);
    p.freeze = false;
    p.mode = Reverb::SPACE;
    p.shimmer = 0.5f;
    p.shimmerInterval = Reverb::DOWN_OCTAVE;
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
    p.shimmer = 1.0f;
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

// A fingerprint of the output (FNV-1a over the samples' bits, -0 as 0).
uint64_t fingerprint(const Buf& L, const Buf& R) {
    uint64_t h = 1469598103934665603ull;
    for (const Buf* b : {&L, &R}) {
        for (float v : *b) {
            uint32_t bits;
            const float x = v == 0.0f ? 0.0f : v;
            std::memcpy(&bits, &x, sizeof bits);
            for (int k = 0; k < 4; ++k) {
                h ^= (bits >> (8 * k)) & 0xffu;
                h *= 1099511628211ull;
            }
        }
    }
    return h;
}

// Every mode through one second of everything moving: size, predelay, freeze, modulation, decay,
// damping, low cut, width, mix, and a mode change.
uint64_t goldenRun(int mode) {
    Reverb r;
    Buf L = whiteNoise(secs(1.0), 0.5f, 41 + static_cast<uint32_t>(mode)), R = whiteNoise(secs(1.0), 0.5f, 51);
    for (size_t i = at(0.3); i < at(0.6); ++i) L[i] = R[i] = 0.0f;
    P p;
    p.mode = mode;
    runChanging(r, p, L, R, [mode](size_t pos, P& q) {
        const double t = static_cast<double>(pos) / ef::kRate;
        q.size = t < 0.2 ? 0.5f : 0.9f;
        q.predelayMs = t < 0.4 ? 20.0f : 60.0f;
        q.freeze = t >= 0.5 && t < 0.7;
        q.mod = t < 0.1 ? 0.3f : 0.8f;
        q.decayS = t < 0.25 ? 2.5f : 8.0f;
        q.dampHz = t < 0.35 ? 6000.0f : 3000.0f;
        q.lowCutHz = t < 0.45 ? 150.0f : 400.0f;
        q.width = t < 0.55 ? 1.0f : 0.6f;
        q.mix = t < 0.65 ? 0.3f : 0.7f;
        q.mode = t < 0.8 ? mode : (mode + 1) % Reverb::kModes;
    });
    return fingerprint(L, R);
}

// Shimmer 0 is the reverb as it was before shimmer, bit for bit: the fingerprints it had then (the
// x86 test build and the device's differ: GCC fuses multiply-adds for NEON). The plain builds only
// (make test, test-arm, GCC 13): against profile-guided objects (make test-arm-pgo, CI's GCC 11) the
// profile and the compiler move the fusing, so no fingerprint stays (as the Delay's Tape hash).
void golden() {
#if EF_PGO_OBJECTS
    std::printf("  (the reverb's fingerprints are checked in the plain builds, not against profile-guided objects)\n");
    return;
#endif
#if EF_NEON
    const uint64_t before[Reverb::kModes] = {0xdd8cbf74d06f96c3ull, 0x39827f258658b59cull, 0xc476a1cf2992a1aeull, 0xb88c6356e4a4538eull};
#else
    const uint64_t before[Reverb::kModes] = {0x982cafd935fb49caull, 0x7769d083c90a8076ull, 0xb926f8e8c89084baull, 0xc81be01a1943df90ull};
#endif
    for (int mode = 0; mode < Reverb::kModes; ++mode) CHECK(goldenRun(mode) == before[mode]);
}

// --- shimmer -----------------------------------------------------------------------------------

// The amplitude of the hz component of x[from..to) (Goertzel through a precomputed Hann window).
double toneAt(const Buf& x, double hz, size_t from, const std::vector<double>& window) {
    const double w = 2.0 * kPi * hz / ef::kRate, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0, sum = 0.0;
    for (size_t i = 0; i < window.size(); ++i) {
        const double s0 = x[from + i] * window[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
        sum += window[i];
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / sum;
}

// A 440 Hz tone grows a component at 440 x the interval in the wet, where there was none, and it
// is at that pitch to within a few cents (the shifter splices in phase: a clean line, not a smear
// between lines half the grain rate apart). Measured while the tone plays: once it stops, the
// tail is the network's own modes around 440 Hz, which spread over tens of cents with or without
// shimmer.
void shimmerPitch() {
    Reverb r;
    const double ratio[Reverb::kIntervals] = {2.0, 1.4983070768766815, 3.1748021039363987, 0.5};
    const size_t from = at(1.5), len = at(2.0);
    std::vector<double> window(len);
    for (size_t i = 0; i < len; ++i) window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(len));
    std::printf("  reverb: a 440 Hz tone's shimmer, gain at the new pitch and cents off it:");
    for (int iv = 0; iv < Reverb::kIntervals; ++iv) {
        const double target = 440.0 * ratio[iv];
        double level[2] = {};
        Stereo s;
        for (int on = 0; on < 2; ++on) {
            P p = wetOnly(Reverb::HALL, 8.0f);
            p.mod = 0.0f;
            p.dampHz = 8000.0f;
            p.shimmer = on ? 0.5f : 0.0f;
            p.shimmerInterval = iv;
            const Buf x = sine(440.0, secs(3.5), 0.3f);
            s = render(r, p, x, x);
            level[on] = toneAt(s.L, target, from, window);
        }
        // The peak within 50 cents: every 5 cents, then every quarter cent around the best.
        double best = 0.0, top = -1.0;
        for (double c = -50.0; c <= 50.0; c += 5.0) {
            const double m = toneAt(s.L, target * std::pow(2.0, c / 1200.0), from, window);
            if (m > top) {
                top = m;
                best = c;
            }
        }
        const double coarse = best;
        for (double c = coarse - 5.0; c <= coarse + 5.0; c += 0.25) {
            const double m = toneAt(s.L, target * std::pow(2.0, c / 1200.0), from, window);
            if (m > top) {
                top = m;
                best = c;
            }
        }
        std::printf(" %.0f Hz %+.0f dB %+.1f", target, db(level[1] / level[0]), best);
        CHECK(db(level[1] / level[0]) > 30.0);
        CHECK(std::fabs(best) < 5.0);
    }
    std::printf("\n");
}

// Shimmer 1 at the extremes: every mode (each with another interval), decay 30 s, size 1, full
// modulation, loud noise and a tone for 2 s, frozen from 10 s to 20 s, 30 s in all. The energy
// never rises over what the input left (the shifter adds none; what climbs out of its band
// leaves), frozen it doesn't grow. And a frozen chord with shimmer 0.5, where the pitched copy and
// what stays could add up if they were alike: it doesn't grow either.
void shimmerBounded() {
    Reverb r;
    Buf x = whiteNoise(secs(30.0), 0.5f, 61);
    const Buf tone = sine(220.0, secs(2.0), 0.3f);
    for (size_t i = 0; i < x.size(); ++i) x[i] = i < tone.size() ? x[i] + tone[i] : 0.0f;
    double worstRise = -100.0, worstFrozen = -100.0;
    float worstPeak = 0.0f;
    for (int mode = 0; mode < Reverb::kModes; ++mode) {
        P p;
        p.mode = mode;
        p.decayS = 30.0f;
        p.size = 1.0f;
        p.mod = 1.0f;
        p.dampHz = 20000.0f;
        p.lowCutHz = 20.0f;
        p.mix = 1.0f;
        p.shimmer = 1.0f;
        p.shimmerInterval = mode;
        Buf L = x, R = x;
        r.reset();
        runChanging(r, p, L, R, [](size_t pos, P& q) { q.freeze = pos >= at(10.0) && pos < at(20.0); });
        CHECK(allFinite(L) && allFinite(R));
        worstPeak = std::max({worstPeak, peak(L), peak(R)});
        const Stereo s{L, R};
        const double ref = rmsLR(s, at(2.0), at(3.0));
        for (double t = 3.0; t < 30.0; t += 1.0) worstRise = std::max(worstRise, db(rmsLR(s, at(t), at(t + 1.0)) / ref));
        const double frozen = rmsLR(s, at(11.0), at(12.0));
        for (double t = 12.0; t < 20.0; t += 1.0) worstFrozen = std::max(worstFrozen, db(rmsLR(s, at(t), at(t + 1.0)) / frozen));
    }
    // The chord, frozen with shimmer 0.5.
    P p = wetOnly(Reverb::HALL, 30.0f);
    p.shimmer = 0.5f;
    Buf c = sine(220.0, secs(30.0), 0.2f);
    const Buf e = sine(330.0, secs(30.0), 0.2f), a = sine(440.0, secs(30.0), 0.2f);
    for (size_t i = 0; i < c.size(); ++i) c[i] = i < at(2.0) ? c[i] + e[i] + a[i] : 0.0f;
    Buf L = c, R = c;
    r.reset();
    runChanging(r, p, L, R, [](size_t pos, P& q) { q.freeze = pos >= at(2.0); });
    const Stereo s{L, R};
    const double held = rmsLR(s, at(3.0), at(4.0));
    double chordRise = -100.0;
    for (double t = 4.0; t < 30.0; t += 1.0) chordRise = std::max(chordRise, db(rmsLR(s, at(t), at(t + 1.0)) / held));
    std::printf("  reverb: shimmer 1, decay 30 s, 30 s: highest second %+.1f dB over the input's last, frozen %+.1f dB, "
                "peak %.2f; a frozen chord at shimmer 0.5: %+.1f dB\n", worstRise, worstFrozen, worstPeak, chordRise);
    CHECK(worstRise < 1.0);
    CHECK(worstFrozen < 1.0);
    CHECK(worstPeak < 20.0f);
    CHECK(chordRise < 1.0);
}

// The highs (above 12 kHz, 4th order) of x: where a click shows. A low tone through the shimmer
// has hardly any there; a step has.
Buf highs(const Buf& x) {
    Buf y = x;
    for (int stage = 0; stage < 2; ++stage) {
        const double w = 2.0 * kPi * 12000.0 / ef::kRate, al = std::sin(w) / (2.0 * 0.7071), a0 = 1.0 + al;
        const double b0 = (1.0 + std::cos(w)) / 2.0 / a0, b1 = -(1.0 + std::cos(w)) / a0, a1 = -2.0 * std::cos(w) / a0, a2 = (1.0 - al) / a0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        for (float& v : y) {
            const double o = b0 * v + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = v;
            y2 = y1;
            y1 = o;
            v = static_cast<float>(o);
        }
    }
    return y;
}

// No clicks when the shimmer comes in, goes out, or changes interval (which fades it out and in):
// the highs around the change stay where they are before it and once it has settled.
void shimmerClicks() {
    Reverb r;
    struct Change {
        const char* what;
        float before, after;
        int ivBefore, ivAfter;
    };
    for (const Change c : {Change{"in", 0.0f, 1.0f, 0, 0}, Change{"out", 1.0f, 0.0f, 0, 0}, Change{"+12 to +19", 1.0f, 1.0f, 0, 2},
                           Change{"+12 to -12", 1.0f, 1.0f, 0, 3}, Change{"+7 to +12", 0.6f, 0.6f, 1, 0}}) {
        P p = wetOnly(Reverb::HALL, 3.0f);
        p.dampHz = 4000.0f;
        Buf L = sine(150.0, secs(3.0), 0.5f), R = L;
        r.reset();
        runChanging(r, p, L, R, [&](size_t pos, P& q) {
            const bool after = pos >= at(1.5);
            q.shimmer = after ? c.after : c.before;
            q.shimmerInterval = after ? c.ivAfter : c.ivBefore;
        });
        const Buf h = highs(L);
        const float around = peak(h, at(1.45), at(1.7)), calm = std::max(peak(h, at(1.0), at(1.45)), peak(h, at(2.2), at(3.0)));
        const bool ok = around <= 3.0f * calm + 1e-5f;
        if (!ok) std::printf("  reverb: a click as the shimmer goes %s (%.2g against %.2g)\n", c.what, around, calm);
        CHECK(ok);
    }
    // Shimmer back at 0: the shifter stops.
    P p;
    p.shimmer = 0.5f;
    Buf L = whiteNoise(secs(0.2), 0.5f), R = L;
    r.reset();
    run(r, p, L, R);
    CHECK(r.shimmering());
    p.shimmer = 0.0f;
    Buf l = whiteNoise(secs(0.05), 0.5f), rr = l;   // the angle glides out over 20 ms
    run(r, p, l, rr);
    CHECK(!r.shimmering());
}

} // namespace

// The shifter after a restart (shimmer switched on, or a new interval): a steady tone through it
// starts silent (its head reaches back before anything it has heard) and once it sounds it keeps
// sounding, at every interval: the first splice never jumps past what was written since.
void shifterOnset() {
    const float ratios[Reverb::kIntervals] = {2.0f, 1.49830708f, 3.17480210f, 0.5f};
    std::vector<float> buf(ef::PitchShift::bufferSize(ratios[2], ratios[3], 3528));
    for (int iv = 0; iv < Reverb::kIntervals; ++iv) {
        ef::PitchShift ps;
        ps.attach(buf.data(), static_cast<uint32_t>(buf.size()));
        ps.setRatio(ratios[iv], 3528);
        for (int i = 0; i < 20000; ++i) ps.tick(std::sin(0.05f * static_cast<float>(i)));   // stale material first
        ps.restart();
        int gaps = 0, heard = 0;
        bool quiet = true;
        for (int b = 0; b < 80; ++b) {
            double e = 0.0;
            for (int i = 0; i < 221; ++i) {
                const int n = b * 221 + i;
                const float y = ps.tick(0.5f * std::sin(2.0f * 3.14159265f * 300.0f * static_cast<float>(n) / ef::kRate));
                e += static_cast<double>(y) * y;
            }
            const bool now = std::sqrt(e / 221.0) < 0.05;
            gaps += heard && now && !quiet;
            heard += !now;
            quiet = now;
        }
        CHECK(heard > 60 && gaps == 0);
        if (!(heard > 60 && gaps == 0)) std::printf("  shifter %d: %d blocks heard, %d dropouts\n", iv, heard, gaps);
    }
}

// Shimmer switched on in the middle of a tail takes the pitched path's share from what stays only
// once the shifter sounds: until then the tail is the one without shimmer, bit for bit, and it
// never dips. And tailSamples() covers a short shimmering tail (the pitched echoes wait in the
// shifter, so they ring past Decay): Room, size 0, decay 0.3 s, +19.
void shimmerWakes() {
    Reverb r;
    for (int iv = 0; iv < Reverb::kIntervals; ++iv) {
        P p = wetOnly(Reverb::HALL, 4.0f);
        p.shimmerInterval = iv;
        Buf x = whiteNoise(secs(2.0), 0.3f, 7);
        std::fill(x.begin() + secs(0.5), x.end(), 0.0f);
        const Stereo plain = render(r, p, x, x);
        const size_t on = at(1.0);
        r.reset();
        Buf L = x, R = x;
        for (size_t pos = 0; pos < L.size(); pos += ef::kChunk) {
            p.shimmer = pos >= on ? 1.0f : 0.0f;
            r.set(p, {});
            r.process(&L[pos], &R[pos], static_cast<int>(std::min<size_t>(ef::kChunk, L.size() - pos)));
        }
        size_t same = on;
        while (same < L.size() && L[same] == plain.L[same] && R[same] == plain.R[same]) ++same;
        const double dip = db(rmsLR({L, R}, on, on + at(0.1)) / rmsLR(plain, on, on + at(0.1)));
        CHECK(same > on + at(0.02) && dip > -0.03);
        if (!(same > on + at(0.02) && dip > -0.03))
            std::printf("  shimmer %d on: the same for %.1f ms, the next 100 ms %+.2f dB\n", iv, (same - on) * 1000.0 / ef::kRate, dip);
    }
    P p = wetOnly(Reverb::ROOM, 0.3f);
    p.size = 0.0f;
    p.shimmer = 1.0f;
    p.shimmerInterval = Reverb::UP_TWELFTH;
    r.reset();
    r.set(p, {});
    const int n = r.tailSamples();
    const Stereo s = impulseResponse(r, p, n + secs(1.0));
    const size_t win = at(0.02);
    double top = 0.0;
    size_t t60 = 0;
    for (size_t i = 0; i + win <= s.L.size(); i += win) top = std::max(top, rmsLR(s, i, i + win));
    for (size_t i = 0; i + win <= s.L.size(); i += win)
        if (rmsLR(s, i, i + win) > top * 1e-3) t60 = i + win;
    std::printf("  reverb: Room 0.3 s, shimmer +19: tailSamples %.2f s, -60 dB at %.2f s\n", n / ef::kRate, t60 / ef::kRate);
    CHECK(static_cast<size_t>(n) > t60 && static_cast<size_t>(n) < 3 * t60);
}

void reverbTests() {
    golden();
    shimmerPitch();
    shimmerBounded();
    shimmerClicks();
    shifterOnset();
    shimmerWakes();
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
