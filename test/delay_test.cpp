// dsp/delay.h: echo timing against theory (free, synced at several tempos, spread, Ping-Pong), the
// feedback ratio and the cuts against the one-pole responses, the glide landing exactly, drive,
// limiter, ducking, wow depth, and robustness (feedback 1 for 30 s, extremes, random jumps, NaN
// input, block sizes, reset, the tail).
#include "signal.h"
#include "../dsp/delay.h"

#include <complex>
#include <cstring>

namespace {

using namespace eft;
using ef::Delay;
using P = Delay::Params;

constexpr int kSr = 44100;

// A plain delay to measure: wet only, no feedback, the cuts wide open, no wow, drive or duck.
P plain(float ms) {
    P p;
    p.mode = Delay::STEREO;
    p.sync = false;
    p.timeMs = ms;
    p.feedback = 0.0f;
    p.spread = 0.0f;
    p.lowCutHz = 20.0f;
    p.highCutHz = 20000.0f;
    p.wow = 0.0f;
    p.drive = 0.0f;
    p.duck = 0.0f;
    p.mix = 1.0f;
    return p;
}
P synced(double beats) {
    P p = plain(375.0f);
    p.sync = true;
    p.divBeats = beats;
    return p;
}
ef::Transport tempo(double bpm) {
    ef::Transport t;
    t.bpm = bpm;
    return t;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }
size_t loudest(const Buf& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    size_t best = from;
    for (size_t i = from; i < to; ++i)
        if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
    return best;
}
Buf silence(int n) { return Buf(static_cast<size_t>(n), 0.0f); }
Buf concat(Buf a, const Buf& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Runs p for the first `at` samples, then q, in chunks of `chunk`.
void runSplit(Delay& d, const P& p, const P& q, Buf& L, Buf& R, size_t at, ef::Transport t = {}, int chunk = ef::kChunk) {
    Buf l1(L.begin(), L.begin() + static_cast<std::ptrdiff_t>(at)), r1(R.begin(), R.begin() + static_cast<std::ptrdiff_t>(at));
    Buf l2(L.begin() + static_cast<std::ptrdiff_t>(at), L.end()), r2(R.begin() + static_cast<std::ptrdiff_t>(at), R.end());
    run(d, p, l1, r1, t, chunk);
    run(d, q, l2, r2, t, chunk);
    L = concat(l1, l2);
    R = concat(r1, r2);
}

// A sine through a sine fades in over 10 ms (its own start is no click to measure).
Buf fadedSine(double hz, int n, float amp, double phase = 0.0) {
    Buf x = sine(hz, n, amp, phase);
    for (int i = 0; i < 441; ++i) x[static_cast<size_t>(i)] *= static_cast<float>(i) / 441.0f;
    return x;
}

// The cuts (dsp/delay.h): a = 1 - exp(-2 pi fc / rate), low-pass a / (1 - (1 - a) z^-1),
// high-pass (1 - a)(1 - z^-1) / (1 - (1 - a) z^-1); a high cut of 20 kHz is off.
double coef(double hz) { return 1.0 - std::exp(-2.0 * kPi * hz / ef::kRate); }
std::complex<double> zInv(double hz) { return std::polar(1.0, -2.0 * kPi * hz / ef::kRate); }
std::complex<double> lowPass(double fc, double hz) {
    if (fc >= 20000.0) return 1.0;
    const double a = coef(fc);
    return a / (1.0 - (1.0 - a) * zInv(hz));
}
std::complex<double> highPass(double fc, double hz) {
    const double a = coef(fc);
    return (1.0 - a) * (1.0 - zInv(hz)) / (1.0 - (1.0 - a) * zInv(hz));
}
double h0() { return 1.0 - coef(20.0); }   // the 20 Hz high-pass's first sample

// An impulse at `at` read back `delay` samples later through the 4-point Hermite kernel and the
// 20 Hz high-pass, in double: what one echo of the plain delay must look like.
Buf echoOf(int n, int at, double delay, double amp = 1.0) {
    const int i = static_cast<int>(delay);
    const double f = delay - i;
    std::vector<double> x(static_cast<size_t>(n), 0.0);
    const double w[4] = {(-f * f * f + 2 * f * f - f) / 2, (3 * f * f * f - 5 * f * f + 2) / 2, (-3 * f * f * f + 4 * f * f + f) / 2,
                         (f * f * f - f * f) / 2};
    for (int k = 0; k < 4; ++k) {
        const int pos = at + i - 1 + k;
        if (pos >= 0 && pos < n) x[static_cast<size_t>(pos)] += amp * w[k];
    }
    const double a = coef(20.0);
    double s = 0.0;
    Buf y(static_cast<size_t>(n));
    for (size_t k = 0; k < y.size(); ++k) {
        s += a * (x[k] - s);
        y[k] = static_cast<float>(x[k] - s);
    }
    return y;
}

// The plain delay's answer to an impulse, L and R against echoOf() at their delays.
bool echoes(Delay& d, const P& p, ef::Transport t, double delayL, double delayR) {
    const int at = 64, n = at + static_cast<int>(std::max(delayL, delayR)) + 64;
    d.reset();
    Buf L = impulseAt(n, at), R = L;
    run(d, p, L, R, t);
    const Buf el = echoOf(n, at, delayL), er = echoOf(n, at, delayR);
    double worst = 0.0;
    for (size_t i = 0; i < L.size(); ++i) worst = std::max({worst, std::fabs(static_cast<double>(L[i]) - el[i]), std::fabs(static_cast<double>(R[i]) - er[i])});
    bool ok = worst < 1e-5;
    if (delayL == std::floor(delayL)) ok = ok && loudest(L) == static_cast<size_t>(at + delayL) && near(L[loudest(L)], h0(), 1e-6);
    if (!ok) std::printf("  echo %.6f / %.6f: off by %g\n", delayL, delayR, worst);
    return ok;
}

void timing() {
    Delay d;
    // Free: timeMs later, between samples as the Hermite kernel puts it.
    for (float ms : {1.0f, 2.5f, 10.0f, 250.0f, 333.3f, 1000.0f, 2000.0f}) {
        const double delay = static_cast<double>(ms) * kSr / 1000.0;
        CHECK(echoes(d, plain(ms), {}, delay, delay));
    }
    // Synced: beats x 60 / BPM seconds, at several tempos; whole numbers of samples are exact.
    const double cases[][2] = {{0.5, 120}, {1.5, 90},   {1.0 / 6, 100}, {4.0, 60},  {4.0, 30},
                               {0.0625, 175}, {0.75, 128}, {1.0 / 3, 140}, {2.0, 87}, {1.0 / 12, 300}};
    for (const auto& c : cases) {
        const double delay = c[0] * 60.0 / c[1] * kSr;
        CHECK(echoes(d, synced(c[0]), tempo(c[1]), delay, delay));
    }
    // The longest: 1 bar at 30 BPM, 8 s. Slower is clamped there.
    CHECK(echoes(d, synced(4.0), tempo(20.0), 8.0 * kSr, 8.0 * kSr));
    // Out of range: 1 ms and 2 s.
    CHECK(echoes(d, plain(0.01f), {}, 44.1, 44.1));
    CHECK(echoes(d, plain(5000.0f), {}, 2.0 * kSr, 2.0 * kSr));
}

void spread() {
    // R's time is L's x (1 + spread); L stays where it was.
    Delay d;
    for (float s : {-0.5f, -0.25f, 0.3f, 0.5f}) {
        P p = plain(200.0f);
        p.spread = s;
        CHECK(echoes(d, p, {}, 8820.0, 8820.0 * (1.0 + s)));
        P q = synced(0.5);
        q.spread = s;
        CHECK(echoes(d, q, tempo(120.0), 11025.0, 11025.0 * (1.0 + s)));
    }
    // Stereo keeps the sides apart: L's input never comes out on R.
    d.reset();
    P p = plain(50.0f);
    p.feedback = 0.7f;
    Buf L = whiteNoise(20000, 0.5f, 3), R = silence(20000);
    run(d, p, L, R);
    CHECK(peak(R) < 1e-12f && peak(L) > 0.1f);
}

void pingPong() {
    Delay d;
    const int t = 4410;
    const double h = h0();
    // The mono sum enters L; the repeats alternate L, R, L..., each fb times the last.
    {
        P p = plain(100.0f);
        p.mode = Delay::PING_PONG;
        p.feedback = 0.5f;
        Buf L = impulseAt(7 * t + 100, 0), R = L;
        run(d, p, L, R);
        for (int k = 1; k <= 6; ++k) {
            const Buf& on = k % 2 ? L : R;
            const Buf& off = k % 2 ? R : L;
            const size_t at = static_cast<size_t>(k * t);
            CHECK(near(on[at], std::pow(0.5, k - 1) * std::pow(h, k), 1e-5));
            CHECK(std::fabs(off[at]) < 1e-6);
            CHECK(loudest(on, at - t / 2, at + t / 2) == at);
        }
        CHECK(peak(L, 0, t) < 1e-6f && peak(R, 0, 2 * t) < 1e-6f);
    }
    // A side alone counts half (the mono sum), and still starts on L.
    {
        d.reset();
        P p = plain(100.0f);
        p.mode = Delay::PING_PONG;
        p.feedback = 0.5f;
        Buf L = silence(3 * t), R = impulseAt(3 * t, 0);
        run(d, p, L, R);
        CHECK(near(L[t], 0.5 * h, 1e-5) && std::fabs(R[t]) < 1e-6);
        CHECK(near(R[2 * t], 0.25 * h * h, 1e-5) && std::fabs(L[2 * t]) < 1e-6);
    }
    // With spread, R's turns take L's time x (1 + spread): L at t, R at 2.5 t, L at 3.5 t, R at 5 t.
    {
        d.reset();
        P p = plain(100.0f);
        p.mode = Delay::PING_PONG;
        p.feedback = 0.5f;
        p.spread = 0.5f;
        Buf L = impulseAt(6 * t, 0), R = L;
        run(d, p, L, R);
        const int times[] = {t, t * 5 / 2, t * 7 / 2, 5 * t};
        for (int k = 0; k < 4; ++k) {
            const Buf& on = k % 2 ? R : L;
            const size_t at = static_cast<size_t>(times[k]);
            CHECK(loudest(on, at - t / 2, at + t / 2) == at);
            CHECK(near(on[at], std::pow(0.5, k) * std::pow(h, k + 1), 1e-5));
        }
    }
}

void mono() {
    // Mono: both sides the same, the echo of the mono sum, whatever the spread and the wow.
    Delay d;
    P p = plain(120.0f);
    p.mode = Delay::MONO;
    p.feedback = 0.6f;
    p.spread = 0.4f;
    p.wow = 0.7f;
    p.drive = 0.5f;
    p.lowCutHz = 150.0f;
    p.highCutHz = 6000.0f;
    const Buf inL = whiteNoise(30000, 0.5f, 5), inR = sine(330.0, 30000, 0.5f);
    Buf L = inL, R = inR;
    run(d, p, L, R);
    CHECK(same(L, R));
    // The same as Stereo without spread fed the mono sum on both sides.
    Buf sum(inL.size());
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = 0.5f * inL[i] + 0.5f * inR[i];
    Delay s;
    P q = p;
    q.mode = Delay::STEREO;
    q.spread = 0.0f;
    Buf A = sum, B = sum;
    run(s, q, A, B);
    CHECK(same(A, L));
    // Through a Fade too.
    P f = p;
    f.glide = Delay::FADE;
    P g = f;
    g.timeMs = 170.0f;
    Delay m;
    Buf C = inL, D = inR;
    runSplit(m, f, g, C, D, 10000);
    CHECK(same(C, D));
}

void feedbackRatio() {
    // Repeat k of an impulse is fb^(k-1) h0^k of it: the cuts wide open still leave the 20 Hz
    // high-pass's first sample h0 per pass. Feedback 1 holds.
    Delay d;
    const int t = 4410;
    const double h = h0();
    for (float fb : {0.25f, 0.6f, 0.9f, 1.0f}) {
        P p = plain(100.0f);
        p.feedback = fb;
        d.reset();
        Buf L = impulseAt(7 * t + 10, 0, 0.5f), R = L;
        run(d, p, L, R);
        for (int k = 1; k <= 6; ++k) {
            const double expect = 0.5 * std::pow(fb, k - 1) * std::pow(h, k);
            CHECK(near(L[static_cast<size_t>(k * t)], expect, 1e-5 * expect + 1e-8));
        }
    }
}

void cuts() {
    // Repeat n of a tone has been through the cuts n times and the feedback n - 1 times:
    // fb^(n-1) |H_lp H_hp|^n. Tone bursts 0.3 s long, repeats 0.5 s apart.
    struct Case {
        float lowCut, highCut;
        double hz;
    };
    const Case cases[] = {{1000.0f, 20000.0f, 300.0}, {2000.0f, 20000.0f, 700.0}, {20.0f, 2000.0f, 6000.0},
                          {20.0f, 500.0f, 2000.0},    {300.0f, 3000.0f, 1000.0},  {100.0f, 8000.0f, 12000.0}};
    const int t = 22050, len = 13230;
    Delay d;
    for (const Case& c : cases) {
        P p = plain(500.0f);
        p.feedback = 0.8f;
        p.lowCutHz = c.lowCut;
        p.highCutHz = c.highCut;
        Buf burst = sine(c.hz, len, 0.5f);
        for (int i = 0; i < len; ++i) burst[static_cast<size_t>(i)] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / len));
        Buf L = concat(burst, silence(4 * t)), R = L;
        d.reset();
        run(d, p, L, R);
        const double ref = magnitude(burst, c.hz);
        const double once = std::abs(lowPass(c.highCut, c.hz) * highPass(c.lowCut, c.hz));
        double last = 0.0;
        for (int k = 1; k <= 3; ++k) {
            const size_t from = static_cast<size_t>(k * t);
            const double got = db(magnitude(L, c.hz, from, from + len) / ref);
            const double expect = db(std::pow(0.8, k - 1) * std::pow(once, k));
            if (!near(got, expect, 0.2)) std::printf("  cuts %g / %g at %g Hz, repeat %d: %.2f dB, expected %.2f\n", c.lowCut, c.highCut, c.hz, k, got, expect);
            CHECK(near(got, expect, 0.2));
            if (k > 1) CHECK(got < last - 1.0);   // each repeat further down
            last = got;
        }
    }
}

void glide() {
    // 120 -> 60 BPM at 1/8: the time glides from 11025 to 22050 samples (the probe's 60 ms
    // one-pole, stepped every 32 samples) and lands on 22050 exactly; float would stall short.
    Delay d;
    const P p = synced(0.5);
    Buf L = whiteNoise(kSr, 0.5f, 9), R = L;
    run(d, p, L, R, tempo(120.0));
    CHECK(d.timeSamples(0) == 11025.0 && d.timeSamples(1) == 11025.0);
    const int part = 83 * 32;   // 60.2 ms
    Buf A = silence(part), B = A;
    run(d, p, A, B, tempo(60.0));
    const double expect = 22050.0 - 11025.0 * std::exp(-part / (0.060 * kSr));
    CHECK(near(d.timeSamples(0), expect, 1e-6 * expect));
    Buf C = silence(3 * kSr), D = C;
    run(d, p, C, D, tempo(60.0));
    CHECK(allFinite(C) && allFinite(D));
    CHECK(d.timeSamples(0) == 22050.0 && d.timeSamples(1) == 22050.0);
    // And an impulse now comes back 22050 samples later, on the sample.
    const int n = 22050 + 200;
    Buf E = impulseAt(n, 64), F = E;
    run(d, p, E, F, tempo(60.0));
    const Buf e = echoOf(n, 64, 22050.0);
    double worst = 0.0;
    for (size_t i = 0; i < E.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(E[i]) - e[i]));
    CHECK(worst < 1e-5);
    CHECK(loudest(E) == 64 + 22050 && E[64 + 22050] == F[64 + 22050]);

    // Changing the target every chunk (automation) never lands but never misbehaves.
    d.reset();
    Buf G = whiteNoise(2 * kSr, 0.5f, 10), H = G;
    for (size_t pos = 0; pos < G.size(); pos += ef::kChunk) {
        P q = plain(10.0f + 990.0f * static_cast<float>((pos / ef::kChunk) % 2));
        q.feedback = 0.7f;
        d.set(q, {});
        d.process(&G[pos], &H[pos], static_cast<int>(std::min<size_t>(ef::kChunk, G.size() - pos)));
    }
    CHECK(allFinite(G) && peak(G) < 4.0f);
}

// The plain delay without feedback in double: the drive's shaper on what is written, the 20 Hz
// high-pass on what is read `delay` samples later.
Buf driven(const Buf& x, int delay, double drive) {
    const double a = coef(20.0);
    double s = 0.0;
    Buf y(x.size());
    for (size_t n = 0; n < x.size(); ++n) {
        const double u = n >= static_cast<size_t>(delay) ? x[n - static_cast<size_t>(delay)] : 0.0;
        const double c = std::clamp(u, -0.5, 0.5);
        const double v = u + drive * (c - 4.0 / 3.0 * c * c * c - u);
        s += a * (v - s);
        y[n] = static_cast<float>(v - s);
    }
    return y;
}

void fadeLands() {
    // Fade, 120 -> 60 BPM at 1/8: the old head keeps reading 11025 samples back while the new one
    // at 22050 fades in over 50 ms; then 22050 exactly, and an impulse comes back on the sample.
    Delay d;
    P p = synced(0.5);
    p.glide = Delay::FADE;
    Buf L = whiteNoise(kSr, 0.5f, 9), R = L;
    run(d, p, L, R, tempo(120.0));
    CHECK(d.timeSamples(0) == 11025.0);
    Buf A = silence(30 * 32), B = A;
    run(d, p, A, B, tempo(60.0));
    CHECK(d.timeSamples(0) == 11025.0 && d.timeSamples(1) == 11025.0);   // no glide: still the old head
    Buf C = silence(71 * 32), D = C;
    run(d, p, C, D, tempo(60.0));
    CHECK(d.timeSamples(0) == 22050.0 && d.timeSamples(1) == 22050.0);   // 50 ms on: the new one
    Buf S = silence(22050), T = S;   // the noise out of the line
    run(d, p, S, T, tempo(60.0));
    const int n = 22050 + 200;
    Buf E = impulseAt(n, 64), F = E;
    run(d, p, E, F, tempo(60.0));
    const Buf e = echoOf(n, 64, 22050.0);
    double worst = 0.0;
    for (size_t i = 0; i < E.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(E[i]) - e[i]));
    CHECK(worst < 1e-5 && loudest(E) == 64 + 22050);
    // A change under half a sample snaps: no fade, the new time at once.
    P q = plain(100.0f);
    q.glide = Delay::FADE;
    d.reset();
    Buf G = silence(64), H = G;
    run(d, q, G, H);
    q.timeMs = 100.01f;   // 4410.44
    run(d, q, G, H);
    CHECK(d.timeSamples(0) == static_cast<double>(100.01f) * kSr / 1000.0);
}

// The frequency of x over [from, from + len): zero crossings, placed between samples, per second.
double frequency(const Buf& x, size_t from, size_t len) {
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = from + 1; i < from + len; ++i) {
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / (static_cast<double>(x[i - 1]) - x[i]);
            if (first < 0.0) first = t;
            last = t;
            ++count;
        }
    }
    return count > 1 ? 0.5 * (count - 1) * kSr / (last - first) : 0.0;
}

void fadeNoBend() {
    // A 1 kHz sine, 100% wet, through a tempo change. Tape bends its pitch far; Fade doesn't: with
    // the heads in phase (120 -> 60 BPM at 1/8: 250 cycles apart) not at all, a third of a cycle
    // apart (100 -> 90 BPM) only the crossfade's phase walk, under 12 Hz.
    const double bpms[2][2] = {{120.0, 60.0}, {100.0, 90.0}};
    for (int c = 0; c < 2; ++c) {
        for (int glide : {Delay::TAPE, Delay::FADE}) {
            Delay d;
            P p = synced(0.5);
            p.glide = glide;
            const size_t at = 32 * 1000;   // the change
            Buf L = sine(1000.0, 3 * kSr, 0.5f), R = L;
            Buf l1(L.begin(), L.begin() + at), r1(R.begin(), R.begin() + at), l2(L.begin() + at, L.end()), r2(R.begin() + at, R.end());
            run(d, p, l1, r1, tempo(bpms[c][0]));
            run(d, p, l2, r2, tempo(bpms[c][1]));
            const Buf out = concat(l1, l2);
            double worst = 0.0;
            for (size_t m = at - 4410; m + 441 <= at + 22050; m += 441) worst = std::max(worst, std::fabs(frequency(out, m, 441) - 1000.0));
            if (glide == Delay::TAPE) CHECK(worst > 100.0);
            else CHECK(worst < (c == 0 ? 0.05 : 12.0));
        }
    }
}

void fadeLevel() {
    // Steady noise, 100% wet, 300 -> 400 ms: the two heads read unrelated noise, and the equal-power
    // crossfade keeps the level through the 50 ms (a straight one would dip 3 dB halfway).
    Delay d;
    P p = plain(300.0f);
    p.glide = Delay::FADE;
    P q = p;
    q.timeMs = 400.0f;
    const size_t at = 32 * 1000;
    Buf L = whiteNoise(2 * kSr, 0.5f, 91), R = L;
    runSplit(d, p, q, L, R, at);
    const double before = rms(L, at - 13230, at);
    for (size_t m = at; m < at + 3 * 882; m += 882) CHECK(near(db(rms(L, m, m + 882) / before), 0.0, 1.0));
}

void fadeClicks() {
    // Two 200 Hz sines through the delay, fb 0.5, mix 0.5: one time change, the time moved every
    // chunk (an LFO on it), Tape <-> Fade every chunk under a slower LFO, and switching while
    // the time jumps and Ping-Pong's spread changes. No step beyond twice what a sine at the
    // output's peak makes.
    const int n = 1378 * ef::kChunk;
    for (int what = 0; what < 4; ++what) {
        Delay d;
        Buf L = fadedSine(200.0, n, 0.5f), R = fadedSine(200.0, n, 0.5f, 1.0);
        for (int pos = 0, c = 0; pos < n; pos += ef::kChunk, ++c) {
            const double sec = static_cast<double>(pos) / kSr;
            P p = plain(100.0f);
            p.feedback = 0.5f;
            p.mix = 0.5f;
            p.glide = Delay::FADE;
            switch (what) {
                case 0: p.timeMs = pos < n / 2 ? 100.0f : 137.0f; break;
                case 1: p.timeMs = static_cast<float>(150.0 + 100.0 * std::sin(2.0 * kPi * 2.0 * sec)); break;
                case 2:
                    p.timeMs = static_cast<float>(150.0 + 20.0 * std::sin(2.0 * kPi * sec));
                    p.glide = c % 2 ? Delay::TAPE : Delay::FADE;
                    break;
                default:
                    p.mode = Delay::PING_PONG;
                    p.timeMs = (c / 37) % 2 ? 180.0f : 100.0f;
                    p.spread = (c / 23) % 2 ? 0.3f : -0.2f;
                    p.glide = (c / 50) % 2 ? Delay::TAPE : Delay::FADE;
                    break;
            }
            d.set(p, {});
            d.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], ef::kChunk);
        }
        const double natural = std::max(peak(L), peak(R)) * 2.0 * kPi * 200.0 / kSr;
        CHECK(allFinite(L) && allFinite(R));
        CHECK(std::max(maxStep(L), maxStep(R)) < 2.0 * natural);
    }
}

// Tape's output on three busy scenarios, hashed (FNV-1a over the bits of L, then R).
uint64_t tapeHash(int scenario) {
    Delay d;
    const int n = 3 * kSr;
    Buf L = whiteNoise(n, 0.7f, 81), R = sine(330.0, n, 0.6f);
    for (size_t pos = 0, c = 0; pos < L.size(); pos += ef::kChunk, ++c) {
        P p;
        ef::Transport t;
        t.bpm = 120.0;
        p.wow = 0.6f;
        p.drive = 0.4f;
        p.duck = 0.5f;
        p.spread = 0.25f;
        if (scenario == 1) {
            p.mode = Delay::PING_PONG;
            p.feedback = 0.8f;
            p.wow = 1.0f;
            p.spread = -0.3f;
            if (pos > 44100) t.bpm = 87.0;
        } else if (scenario == 2) {
            p.mode = static_cast<int>((c / 200) % 3);
            p.sync = false;
            p.timeMs = 5.0f + 1495.0f * static_cast<float>((c * 7919) % 100) / 99.0f * ((c / 10) % 2);
            p.drive = 1.0f;
            p.duck = 1.0f;
            p.feedback = 0.95f;
        }
        d.set(p, t);
        d.process(&L[pos], &R[pos], static_cast<int>(std::min<size_t>(ef::kChunk, L.size() - pos)));
    }
    uint64_t h = 1469598103934665603ull;
    for (const Buf* b : {&L, &R})
        for (float v : *b) {
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            for (int k = 0; k < 4; ++k) h = (h ^ ((bits >> (8 * k)) & 0xffu)) * 1099511628211ull;
        }
    return h;
}

void tapeUnchanged() {
    // Tape is bit for bit what it was before Fade came: the hashes were taken from that code, on
    // x86 and on the device's build (NEON's reciprocal estimate and GCC's contractions differ; a new
    // compiler may move the device's, not x86's). Not against the profile-guided objects (make
    // test-arm-pgo): their profile moves the contractions with every change of the code, so no hash
    // stays; the plain builds (make test, test-arm) keep the check.
#if EF_PGO_OBJECTS
    std::printf("  (the Tape hash is checked in the plain builds, not against profile-guided objects)\n");
    return;
#endif
#if EF_NEON
    const uint64_t before[3] = {0x690cbedd772c6b55ull, 0x49268bdd94b108f1ull, 0x81639e54566b2b03ull};
#else
    const uint64_t before[3] = {0x6b493ad57ae03251ull, 0xead2494d4735b109ull, 0x557c2696329e4b99ull};
#endif
    for (int k = 0; k < 3; ++k) CHECK(tapeHash(k) == before[k]);
}

void drive() {
    Delay d;
    // The shaper, c - 4/3 c^3 with c = u clamped to +-1/2, blended in by the drive: the echo
    // against the model, from -54 dBFS to -1 dBFS (0 is linear, 1 flattens at 1/3).
    for (float amp : {0.002f, 0.25f, 0.6f, 0.9f}) {
        for (float drv : {0.0f, 0.5f, 1.0f}) {
            P p = plain(50.0f);
            p.drive = drv;
            Buf L = sine(500.0, 20000, amp), R = L;
            const Buf in = L;
            d.reset();
            run(d, p, L, R);
            const Buf m = driven(in, 2205, drv);
            double worst = 0.0;
            for (size_t i = 0; i < L.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(L[i]) - m[i]));
            CHECK(worst < 2e-6 * amp + 1e-7);
        }
    }
    // On A sin: the fundamental A - A^3, the 3rd harmonic A^3 / 3 (the high-pass takes 0.08% at 500 Hz).
    {
        P p = plain(50.0f);
        p.drive = 1.0f;
        const double a = 0.25, hp = std::abs(highPass(20.0, 500.0));
        Buf L = sine(500.0, 30000, static_cast<float>(a)), R = L, ref = sine(500.0, 30000, 1.0f);
        d.reset();
        run(d, p, L, R);
        const double unit = magnitude(ref, 500.0, 4000);   // the measurement's own scale for a 1.0 sine
        CHECK(near(magnitude(L, 500.0, 4000) / unit, hp * (a - a * a * a), 1e-4));
        CHECK(near(magnitude(L, 1500.0, 4000) / unit, a * a * a / 3.0, 0.01 * a * a * a / 3.0));
    }
}

void limiter() {
    Delay d;
    // A sine at +9.5 dBFS comes back held at 0 dBFS.
    P p = plain(50.0f);
    Buf L = sine(500.0, 30000, 3.0f), R = L;
    run(d, p, L, R);
    CHECK(peak(L, 6000) < 1.005f && peak(L, 6000) > 0.98f);
    // Under 0 dBFS it does nothing (drive 0's linearity above already runs at -1 dBFS).
    d.reset();
    Buf M = sine(500.0, 30000, 0.95f), N = M;
    const Buf in = M;
    run(d, p, M, N);
    CHECK(near(magnitude(M, 500.0, 6000) / magnitude(in, 500.0, 6000), std::abs(highPass(20.0, 500.0)), 1e-4));
}

// The plain delay's level over a window, through a model of the duck's envelope (dsp/delay.h:
// peak follower, 5 ms attack, 250 ms release) and its law 1 / (1 + 16 duck env).
std::vector<double> duckModel(const Buf& in, float duck) {
    const double att = 1.0 - std::exp(-1.0 / (0.005 * kSr)), rel = 1.0 - std::exp(-1.0 / (0.25 * kSr));
    std::vector<double> g(in.size());
    double env = 0.0;
    for (size_t i = 0; i < in.size(); ++i) {
        const double level = std::fabs(in[i]);
        env += (level > env ? att : rel) * (level - env);
        g[i] = 1.0 / (1.0 + 16.0 * duck * env);
    }
    return g;
}

void ducking() {
    // Ducked against unducked: the loop is the same, so their ratio is the wet's gain alone. While
    // the input plays it follows the law; after it stops the repeats come back up.
    const int playing = kSr, n = 3 * kSr;
    const Buf in = concat(sine(1000.0, playing, 0.5f), silence(n - playing));
    for (float duck : {0.5f, 1.0f}) {
        Delay a, b;
        P p = plain(100.0f);
        p.feedback = 0.7f;
        Buf L0 = in, R0 = in, L1 = in, R1 = in;
        run(a, p, L0, R0);
        p.duck = duck;
        run(b, p, L1, R1);
        const std::vector<double> g = duckModel(in, duck);
        for (int w = 0; w < 5; ++w) {   // during the input, after the first echo
            const size_t from = static_cast<size_t>(0.2 * kSr) + static_cast<size_t>(w) * 6615, to = from + 6615;
            double model = 0.0, weight = 0.0;
            for (size_t i = from; i < to; ++i) {
                model += g[i] * g[i] * L0[i] * L0[i];
                weight += L0[i] * L0[i];
            }
            const double got = db(rms(L1, from, to) / rms(L0, from, to)), expect = db(std::sqrt(model / weight));
            CHECK(near(got, expect, 0.3));
            CHECK(got < (duck == 1.0f ? -15.0 : -10.0));
        }
        // 1.25 s after the input stops: within half a dB again.
        const size_t from = static_cast<size_t>(2.25 * kSr), to = from + 4410;
        const double back = db(rms(L1, from, to) / rms(L0, from, to));
        CHECK(back > -0.5 && back <= 0.0);
        CHECK(near(back, db(g[from + 2205]), 0.2));
        CHECK(same(R1, L1));
    }
}

// The echo's delay, sample by sample, from its phase against a 100 Hz input: the correlation with
// sin and cos over one period (441 samples) at points `step` apart from `from` on.
std::vector<double> delays(const Buf& out, double hz, double nominal, size_t from, size_t to, size_t step) {
    const int period = static_cast<int>(std::lround(kSr / hz));
    const double w = 2.0 * kPi * hz / kSr, theta = std::arg(highPass(20.0, hz));
    std::vector<double> d;
    for (size_t m = from; m + static_cast<size_t>(period) <= to; m += step) {
        double i = 0.0, q = 0.0;
        for (int k = 0; k < period; ++k) {
            const double ph = w * static_cast<double>(m + static_cast<size_t>(k));
            i += out[m + static_cast<size_t>(k)] * std::sin(ph);
            q += out[m + static_cast<size_t>(k)] * std::cos(ph);
        }
        // out = g sin(w (n - D) + theta): psi = atan2(q, i) = theta - w D, modulo a period.
        double x = theta - std::atan2(q, i) - w * nominal;
        x = std::remainder(x, 2.0 * kPi);
        d.push_back(nominal + x / w);
    }
    return d;
}

void wow() {
    // 200 ms (20 periods of 100 Hz). Without wow the delay is constant; with wow 1 it swings by the
    // documented +-3 ms wow plus +-0.2 ms flutter (+-141 samples) around it, half that at wow 0.5,
    // and R differs from L (its wow a quarter cycle ahead).
    const int n = 5 * kSr;
    const double nominal = 8820.0;
    double swing[3] = {};
    for (int k = 0; k < 3; ++k) {
        Delay d;
        P p = plain(200.0f);
        p.wow = 0.5f * static_cast<float>(k);
        Buf L = sine(100.0, n, 0.5f), R = L;
        run(d, p, L, R);
        const size_t from = static_cast<size_t>(0.5 * kSr), to = from + 4 * kSr;   // two wow cycles
        const std::vector<double> dl = delays(L, 100.0, nominal, from, to, 64), dr = delays(R, 100.0, nominal, from, to, 64);
        double lo = 1e9, hi = -1e9, mean = 0.0, apart = 0.0;
        for (size_t i = 0; i < dl.size(); ++i) {
            lo = std::min(lo, dl[i]);
            hi = std::max(hi, dl[i]);
            mean += dl[i];
            apart = std::max(apart, std::fabs(dl[i] - dr[i]));
        }
        mean /= static_cast<double>(dl.size());
        swing[k] = hi - lo;
        CHECK(near(mean, nominal, k == 0 ? 0.01 : 10.0));
        if (k == 0) CHECK(swing[0] < 0.01 && apart < 0.01);
        if (k == 2) CHECK(apart > 100.0);   // a quarter cycle: up to sqrt(2) x 132
    }
    const double depth = 0.003 * kSr, flutter = 0.0002 * kSr;
    CHECK(swing[2] > 2.0 * (depth - flutter) && swing[2] < 2.0 * (depth + flutter) + 1.0);
    CHECK(near(swing[1] / swing[2], 0.5, 0.05));
    // Short times keep the depth under a quarter of the time: 10 ms swings at most +-2.5 ms.
    Delay d;
    P p = plain(10.0f);
    p.wow = 1.0f;
    Buf L = sine(100.0, n, 0.5f), R = L;
    run(d, p, L, R);
    CHECK(allFinite(L) && allFinite(R));
}

void holds() {
    // Feedback 1, drive 1, loud noise for 30 s: bounded, and still ringing after the input stops.
    for (float drv : {1.0f, 0.0f}) {
        Delay d;
        P p;   // the defaults: 1/8. at 120 BPM, cuts 100 Hz .. 8 kHz
        p.feedback = 1.0f;
        p.drive = drv;
        p.mix = 1.0f;
        const Buf in = concat(whiteNoise(30 * kSr, 1.0f, 11), silence(3 * kSr));
        Buf L = in, R = concat(whiteNoise(30 * kSr, 1.0f, 12), silence(3 * kSr));
        run(d, p, L, R, tempo(120.0));
        CHECK(allFinite(L) && allFinite(R));
        CHECK(peak(L) < 4.0f && peak(R) < 4.0f);
        CHECK(rms(L, 32 * kSr, 33 * kSr) > 1e-3 && rms(R, 32 * kSr, 33 * kSr) > 1e-3);
        CHECK(d.tailSamples() >= (1 << 30));
    }
    // With the cuts open and under 0 dBFS, feedback 1 holds a burst at its level.
    Delay d;
    P p = plain(250.0f);
    p.feedback = 1.0f;
    const Buf in = concat(whiteNoise(kSr / 5, 0.25f, 13), silence(6 * kSr));
    Buf L = in, R = in;
    run(d, p, L, R);
    const double early = rms(L, kSr, 2 * kSr), late = rms(L, 5 * kSr, 6 * kSr);
    CHECK(near(db(late / early), 0.0, 0.3));
}

void smooth() {
    // Each parameter jumping between its extremes every chunk, on two 200 Hz sines (faded in): no
    // step in the output beyond twice what a sine at the output's peak makes. Left unsmoothed, a
    // jump would step by up to the whole level at once.
    const int n = 1378 * ef::kChunk;   // 1 s
    for (int what = 0; what < 8; ++what) {
        Delay d;
        Buf L = sine(200.0, n, 0.5f), R = sine(200.0, n, 0.5f, 1.0);
        for (int i = 0; i < 441; ++i) {
            L[static_cast<size_t>(i)] *= static_cast<float>(i) / 441.0f;
            R[static_cast<size_t>(i)] *= static_cast<float>(i) / 441.0f;
        }
        for (int pos = 0; pos < n; pos += ef::kChunk) {
            const bool odd = (pos / ef::kChunk) % 2;
            P p = plain(100.0f);
            p.feedback = 0.5f;
            p.mix = 0.5f;
            switch (what) {
                case 0: p.mix = odd ? 1.0f : 0.0f; break;
                case 1: p.feedback = odd ? 0.95f : 0.0f; break;
                case 2: p.lowCutHz = odd ? 2000.0f : 20.0f, p.highCutHz = odd ? 500.0f : 20000.0f; break;
                case 3: p.drive = odd ? 1.0f : 0.0f; break;
                case 4: p.wow = odd ? 1.0f : 0.0f; break;
                case 5: p.duck = odd ? 1.0f : 0.0f; break;
                case 6: p.mode = odd ? Delay::PING_PONG : ((pos / ef::kChunk) % 4 ? Delay::MONO : Delay::STEREO); break;
                default:
                    p.mix = odd ? 1.0f : 0.0f;
                    p.feedback = odd ? 0.95f : 0.0f;
                    p.lowCutHz = odd ? 2000.0f : 20.0f;
                    p.highCutHz = odd ? 500.0f : 20000.0f;
                    p.drive = p.wow = p.duck = odd ? 1.0f : 0.0f;
                    p.mode = odd ? Delay::PING_PONG : Delay::MONO;
                    break;
            }
            d.set(p, {});
            d.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], ef::kChunk);
        }
        const double natural = std::max(peak(L), peak(R)) * 2.0 * kPi * 200.0 / kSr;
        CHECK(allFinite(L) && allFinite(R));
        CHECK(std::max(maxStep(L), maxStep(R)) < 2.0 * natural);
    }
}

void mixZero() {
    // Mix 0 is the input, bit for bit, whatever the delay does.
    Delay d;
    for (int mode = 0; mode < Delay::kModes; ++mode) {
        P p;
        p.mode = mode;
        p.feedback = 0.9f;
        p.wow = 1.0f;
        p.drive = 1.0f;
        p.duck = 1.0f;
        p.spread = -0.3f;
        p.mix = 0.0f;
        const Buf inL = whiteNoise(30000, 1.0f, 21), inR = whiteNoise(30000, 1.0f, 22);
        Buf L = inL, R = inR;
        d.reset();
        run(d, p, L, R);
        CHECK(same(L, inL) && same(R, inR));
    }
}

void extremes() {
    // Loud noise at random corners of the parameter space and both tempo extremes, half a second
    // each, and the two outermost corners for 4 s.
    Delay d;
    uint32_t s = 777;
    float worst = 0.0f;
    bool finite = true;
    const auto corner = [&](uint32_t bits) {
        P p;
        p.mode = static_cast<int>(bits % 3);
        p.sync = bits & 8;
        p.timeMs = bits & 16 ? 2000.0f : 1.0f;
        p.divBeats = bits & 16 ? 4.0 : 0.0625;
        p.feedback = bits & 32 ? 1.0f : 0.0f;
        p.spread = bits & 64 ? 0.5f : -0.5f;
        p.lowCutHz = bits & 128 ? 2000.0f : 20.0f;
        p.highCutHz = bits & 256 ? 20000.0f : 500.0f;
        p.wow = bits & 512 ? 1.0f : 0.0f;
        p.drive = bits & 1024 ? 1.0f : 0.0f;
        p.duck = bits & 2048 ? 1.0f : 0.0f;
        p.mix = bits & 4096 ? 1.0f : 0.0f;
        p.glide = bits & 16384 ? Delay::FADE : Delay::TAPE;
        return p;
    };
    for (int k = 0; k < 120; ++k) {
        const uint32_t bits = ef::xorshift(s);
        d.reset();
        Buf L = whiteNoise(kSr / 2, 1.0f, bits), R = whiteNoise(kSr / 2, 1.0f, bits + 1);
        run(d, corner(bits), L, R, tempo(bits & 8192 ? 300.0 : 30.0));
        finite = finite && allFinite(L) && allFinite(R);
        worst = std::max({worst, peak(L), peak(R)});
    }
    for (uint32_t bits : {0u, 0xffffu, 0xfff2u}) {
        d.reset();
        Buf L = whiteNoise(4 * kSr, 1.0f, 31), R = whiteNoise(4 * kSr, 1.0f, 32);
        run(d, corner(bits), L, R, tempo(bits & 8192 ? 300.0 : 30.0));
        finite = finite && allFinite(L) && allFinite(R);
        worst = std::max({worst, peak(L), peak(R)});
    }
    CHECK(finite);
    CHECK(worst < 4.0f);
    // Out-of-range and non-finite parameters and tempos are clamped.
    const float wild[] = {-1e30f, -5.0f, 1e9f, INFINITY, -INFINITY, std::nanf("")};
    for (float w : wild) {
        P p;
        p.mode = w > 0.0f ? 7 : -3;
        p.glide = w > 0.0f ? 5 : -2;
        p.sync = w > 0.0f;
        p.timeMs = p.feedback = p.spread = p.lowCutHz = p.highCutHz = p.wow = p.drive = p.duck = p.mix = w;
        p.divBeats = w;
        d.reset();
        Buf A = whiteNoise(kSr, 1.0f, 33), B = A;
        run(d, p, A, B, tempo(w));
        CHECK(allFinite(A) && allFinite(B) && peak(A) < 4.0f);
        CHECK(d.tailSamples() > 0);
    }
}

void randomJumps() {
    // Every parameter somewhere new every chunk (out of range too, now and then NaN), the tempo too,
    // under loud noise, for 10 s.
    Delay d;
    uint32_t s = 4242;
    const int n = 10 * kSr;
    Buf L = whiteNoise(n, 1.0f, 41), R = whiteNoise(n, 1.0f, 42);
    for (int pos = 0; pos < n; pos += ef::kChunk) {
        P p;
        p.mode = static_cast<int>(ef::xorshift(s) % 5) - 1;
        p.sync = ef::xorshift(s) & 1;
        p.timeMs = static_cast<float>(std::exp2(ef::randBipolar(s) * 6.0 + 6.0));
        p.divBeats = std::exp2(ef::randBipolar(s) * 5.0 - 1.0);
        p.feedback = 0.6f + 0.6f * ef::randBipolar(s);
        p.spread = 0.7f * ef::randBipolar(s);
        p.lowCutHz = static_cast<float>(std::exp2(ef::randBipolar(s) * 6.0 + 7.0));
        p.highCutHz = static_cast<float>(std::exp2(ef::randBipolar(s) * 3.0 + 12.0));
        p.wow = 0.5f + 0.75f * ef::randBipolar(s);
        p.drive = 0.5f + 0.75f * ef::randBipolar(s);
        p.duck = 0.5f + 0.75f * ef::randBipolar(s);
        p.mix = 0.5f + 0.75f * ef::randBipolar(s);
        p.glide = static_cast<int>(ef::xorshift(s) % 4) - 1;
        if (ef::xorshift(s) % 50 == 0) p.timeMs = std::nanf("");
        if (ef::xorshift(s) % 50 == 0) p.feedback = std::nanf("");
        if (ef::xorshift(s) % 50 == 0) p.spread = INFINITY;
        d.set(p, tempo(30.0 + 270.0 * (0.5 + 0.5 * ef::randBipolar(s))));
        d.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], std::min(ef::kChunk, n - pos));
    }
    CHECK(allFinite(L) && allFinite(R));
    CHECK(peak(L) < 4.0f && peak(R) < 4.0f);
}

void nanInput() {
    // NaN and infinities in the input act as silence: the output is exactly what zeros there give
    // (in the dry too), and finite throughout. Absurd finite levels stay finite.
    for (int mode = 0; mode < Delay::kModes; ++mode) {
        Delay a, b;
        P p;
        p.mode = mode;
        p.feedback = 0.8f;
        p.wow = 0.5f;
        p.drive = 0.3f;
        p.duck = 0.5f;
        p.spread = 0.2f;
        p.mix = 0.6f;
        Buf clean = sine(300.0, 30000, 0.5f);
        clean[1000] = clean[2000] = clean[3000] = 0.0f;
        Buf bad = clean;
        bad[1000] = std::nanf("");
        bad[2000] = INFINITY;
        bad[3000] = -INFINITY;
        Buf cl = clean, cr = clean, bl = bad, br = bad;
        br[2000] = std::nanf("");
        cr[2000] = 0.0f;
        run(a, p, cl, cr);
        run(b, p, bl, br);
        CHECK(allFinite(bl) && allFinite(br));
        CHECK(same(cl, bl) && same(cr, br));

        Buf huge = whiteNoise(30000, 1.0f, 51), h2 = huge;
        huge[100] = 1e30f;
        huge[200] = -3e38f;
        h2[200] = 3.4e38f;
        h2[300] = -3.4e38f;
        a.reset();
        p.mix = 0.5f;
        run(a, p, huge, h2);
        CHECK(allFinite(huge) && allFinite(h2));
        CHECK(peak(huge, 400) < 4.0f && peak(h2, 400) < 4.0f);

        // While a Fade runs (from sample 672 to about 2900) too.
        Delay e, f;
        P g = p;
        g.glide = Delay::FADE;
        g.mix = 0.6f;
        P h = g;
        h.feedback = 0.8f;
        h.timeMs = 250.0f;
        Buf cl2 = clean, cr2 = clean, bl2 = bad, br2 = bad;
        br2[2000] = std::nanf("");
        runSplit(e, g, h, cl2, cr2, 640);
        runSplit(f, g, h, bl2, br2, 640);
        CHECK(allFinite(bl2) && allFinite(br2));
        CHECK(same(cl2, bl2) && same(cr2, br2));
    }
}

void blockSizes() {
    // Constant parameters: chunks of 1, 7 and 32 give the same output, bit for bit.
    std::vector<P> sets;
    {
        P p;   // the defaults, with everything on
        p.wow = 0.6f;
        p.drive = 0.4f;
        p.duck = 0.7f;
        p.spread = 0.25f;
        sets.push_back(p);
        p.mode = Delay::PING_PONG;
        p.feedback = 0.9f;
        sets.push_back(p);
        p.mode = Delay::MONO;
        p.sync = false;
        p.timeMs = 3.0f;
        sets.push_back(p);
    }
    for (const P& p : sets) {
        Delay d;
        const Buf inL = whiteNoise(40000, 0.8f, 61), inR = sine(220.0, 40000, 0.7f);
        Buf L32 = inL, R32 = inR;
        run(d, p, L32, R32, tempo(133.0), 32);
        for (int chunk : {1, 7}) {
            d.reset();
            Buf L = inL, R = inR;
            run(d, p, L, R, tempo(133.0), chunk);
            CHECK(same(L, L32) && same(R, R32));
        }
    }
    // A time change (at a sample all three chunkings share): both glides, wow on, Ping-Pong.
    for (int glide : {Delay::TAPE, Delay::FADE}) {
        P p = sets[1], q = sets[1];
        p.glide = q.glide = glide;
        q.divBeats = 1.0;
        q.spread = -0.4f;
        Delay d;
        const Buf inL = whiteNoise(40000, 0.8f, 62), inR = sine(220.0, 40000, 0.7f);
        Buf L32 = inL, R32 = inR;
        runSplit(d, p, q, L32, R32, 22400, tempo(133.0), 32);
        for (int chunk : {1, 7}) {
            d.reset();
            Buf L = inL, R = inR;
            runSplit(d, p, q, L, R, 22400, tempo(133.0), chunk);
            CHECK(same(L, L32) && same(R, R32));
        }
    }
}

void resetClears() {
    // After reset() the delay is as good as new: nothing of what the lines held comes back (they
    // aren't cleared, only hidden), and the next set() jumps.
    Delay used, fresh;
    P p = plain(1500.0f);
    p.feedback = 0.95f;
    p.wow = 1.0f;
    Buf L = whiteNoise(3 * kSr, 1.0f, 71), R = L;
    run(used, p, L, R);
    used.reset();
    P q;
    q.sync = false;
    q.timeMs = 1800.0f;
    q.feedback = 0.5f;
    q.mode = Delay::PING_PONG;
    q.spread = -0.2f;
    q.wow = 0.3f;
    q.mix = 1.0f;
    Buf a = impulseAt(4 * kSr, 100), b = a, c = a, e = a;
    run(used, q, a, b);
    run(fresh, q, c, e);
    CHECK(same(a, c) && same(b, e));
    // Silence after reset: silence out, though the time reaches back over the old noise.
    Buf M = whiteNoise(3 * kSr, 1.0f, 72), N = M;
    run(used, p, M, N);
    used.reset();
    Buf s = silence(3 * kSr), t = s;
    run(used, plain(2000.0f), s, t);
    CHECK(peak(s) < 1e-15f && peak(t) < 1e-15f);

    // The lines full of loud noise, then reset: times jumping (and gliding) between 1 ms and the
    // 8 s maximum, with the wow and spread, sweep the taps across all of it. Still nothing comes
    // out, and with an input the output is a new delay's to the bit.
    const auto sweep = [](Delay& d, Buf& l, Buf& r) {
        const int times[] = {2000, 1, 700, 8000, 5, 1500};
        for (size_t pos = 0, c = 0; pos < l.size(); pos += ef::kChunk, ++c) {
            const int ms = times[(c / 40) % 6];
            P p = ms == 8000 ? synced(4.0) : plain(static_cast<float>(ms));
            p.mode = Delay::PING_PONG;
            p.feedback = 0.7f;
            p.spread = 0.5f;
            p.wow = 1.0f;
            d.set(p, tempo(30.0));
            d.process(&l[pos], &r[pos], static_cast<int>(std::min<size_t>(ef::kChunk, l.size() - pos)));
        }
    };
    for (bool input : {false, true}) {
        Delay a, b;
        P loud = plain(2000.0f);
        loud.feedback = 0.9f;
        Buf N1 = whiteNoise(9 * kSr, 1.0f, 73), N2 = whiteNoise(9 * kSr, 1.0f, 74);
        run(a, loud, N1, N2);
        a.reset();
        Buf l1 = input ? sine(440.0, 6 * kSr, 0.3f) : silence(6 * kSr), r1 = l1, l2 = l1, r2 = l1;
        sweep(a, l1, r1);
        sweep(b, l2, r2);
        CHECK(same(l1, l2) && same(r1, r2));
        if (!input) CHECK(peak(l1) < 1e-15f && peak(r1) < 1e-15f);
    }
}

void tail() {
    // tailSamples() covers the measured ring of a tone burst (at the cuts' passband) down to
    // -60 dB, without much to spare.
    struct Case {
        int mode;
        float ms, fb, spread, wow;
    };
    const Case cases[] = {{Delay::STEREO, 100.0f, 0.0f, 0.0f, 0.0f},    {Delay::STEREO, 100.0f, 0.3f, 0.0f, 0.0f},
                          {Delay::STEREO, 100.0f, 0.5f, 0.5f, 0.0f},    {Delay::STEREO, 250.0f, 0.9f, 0.0f, 1.0f},
                          {Delay::PING_PONG, 100.0f, 0.7f, 0.5f, 0.0f}, {Delay::MONO, 40.0f, 0.95f, 0.0f, 0.0f}};
    const int len = 882;   // 20 ms of 1 kHz
    Buf burst = sine(1000.0, len, 1.0f);
    for (int i = 0; i < len; ++i) burst[static_cast<size_t>(i)] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / len));
    Delay d;
    for (const Case& c : cases) {
        P p = plain(c.ms);
        p.mode = c.mode;
        p.feedback = c.fb;
        p.spread = c.spread;
        p.wow = c.wow;
        d.reset();
        d.set(p, {});
        const int predicted = d.tailSamples();
        Buf L = concat(burst, silence(predicted + kSr)), R = L;
        run(d, p, L, R);
        size_t last = 0;
        for (size_t i = 0; i < L.size(); ++i)
            if (std::fabs(L[i]) > 1e-3f || std::fabs(R[i]) > 1e-3f) last = i;
        const double period = c.ms * 0.001 * kSr * (1.0 + c.spread) + c.wow * 141.0;
        CHECK(static_cast<double>(last) <= predicted);
        CHECK(predicted <= last + 4.0 * period + 0.06 * kSr);
    }
    P p = plain(100.0f);
    p.feedback = 1.0f;
    d.set(p, {});
    CHECK(d.tailSamples() == (1 << 30));
}

} // namespace

void delayTests() {
    timing();
    spread();
    pingPong();
    mono();
    feedbackRatio();
    cuts();
    glide();
    fadeLands();
    fadeNoBend();
    fadeLevel();
    fadeClicks();
    tapeUnchanged();
    drive();
    limiter();
    ducking();
    wow();
    holds();
    smooth();
    mixZero();
    extremes();
    randomJumps();
    nanInput();
    blockSizes();
    resetClears();
    tail();
}
