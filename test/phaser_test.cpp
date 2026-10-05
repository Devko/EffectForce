// The phaser and flanger against theory: mix 0 is the input exactly; at depth 0 the notches (and,
// with feedback, the whole response) are where the allpass cascade's or the comb's formula puts
// them; the flanger reads as DelayLine::readCubic; a synced LFO's period is the division at MPC's
// tempo and its phase follows the song position through jumps; stereo 180 sweeps the sides in
// antiphase; feedback 0.95 stays bounded. Then the contract: random jumps, NaN input, block
// sizes, reset, tail, no clicks.
#include "signal.h"
#include "../dsp/phaser.h"

#include <complex>
#include <cstdio>

namespace {

using namespace eft;
using namespace ef;
using P = Phaser::Params;
using cd = std::complex<double>;

constexpr int kModes = 4;
const char* const kName[kModes] = {"Phaser 4", "Phaser 8", "Phaser 12", "Flanger"};
constexpr double kTwoPi = 2.0 * eft::kPi;

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
double maxDiff(const Buf& a, const Buf& b, size_t from = 0, size_t to = 0) {
    if (to == 0) to = a.size();
    double m = 0.0;
    for (size_t i = from; i < to; ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}

void render(const P& p, Buf& L, Buf& R, Transport t = {}, int chunk = kChunk) {
    Phaser ph;
    run(ph, p, L, R, t, chunk);
}

// The phaser's break frequency at `center` (50 Hz..8 kHz, log) and the flanger's delay in samples
// (0.2..10 ms, log).
double breakHz(double center) { return 50.0 * std::pow(160.0, center); }
double delaySamples(double center) { return 0.2e-3 * std::pow(50.0, center) * kRate; }
// N first-order allpasses at break K = tan(pi fb / rate): exp(j phase) at f.
cd cascade(int stages, double fb, double f) {
    const double k = std::tan(eft::kPi * fb / kRate), t = std::tan(eft::kPi * f / kRate);
    return std::polar(1.0, -2.0 * stages * std::atan(t / k));
}

void dryAtMixZero() {
    for (int m = 0; m < kModes; ++m)
        for (int sync = 0; sync < 2; ++sync) {
            P p = params(m);
            p.mix = 0.0f;
            p.depth = 1.0f;
            p.feedback = 0.9f;
            p.rateHz = 5.0f;
            p.sync = sync;
            Transport t;
            t.playing = t.valid = sync;
            const Buf inL = whiteNoise(20000, 0.5f, 1), inR = whiteNoise(20000, 0.5f, 2);
            Buf L = inL, R = inR;
            render(p, L, R, t);
            CHECK(same(L, inL) && same(R, inR));
        }
}

// Depth 0, mix 0.5: dry plus the cascade, (1 + exp(j phase)) / 2, so |cos(N atan(t / K))|, with
// t = tan(pi f / rate): notches where the cascade turns 180, 540, ... degrees, unity halfway. With
// feedback through a one-sample delay, the wet is H / (1 - fb z^-1 H).
void phaserNotches() {
    for (int m = 0; m < 3; ++m) {
        const int stages = 4 * (m + 1);
        const double fb = breakHz(0.5), k = std::tan(eft::kPi * fb / kRate);
        P p = params(m);
        p.depth = 0.0f;
        p.feedback = 0.0f;
        p.mix = 0.5f;
        p.center = 0.5f;
        Phaser ph;
        bool deep = true, onCurve = true;
        for (int n = 0; n < stages / 2; ++n) {
            const double f = kRate / eft::kPi * std::atan(k * std::tan((2 * n + 1) * eft::kPi / (2 * stages)));
            const double g = gainAt(ph, p, f);
            deep = deep && g < -40.0;
            if (!(g < -40.0)) std::printf("  %s notch %d at %.1f Hz: %.1f dB\n", kName[m], n, f, g);
        }
        for (double f : {100.0, 300.0, fb, 1500.0, 5000.0}) {
            const double want = db(std::fabs(std::cos(stages * std::atan(std::tan(eft::kPi * f / kRate) / k))));
            if (want > -20.0) onCurve = onCurve && std::fabs(gainAt(ph, p, f) - want) < 0.1;
        }
        CHECK(deep);
        CHECK(onCurve);

        p.feedback = 0.5f;
        bool fbCurve = true;
        for (double f : {150.0, 450.0, 900.0, 2500.0, 6000.0}) {
            const cd h = cascade(stages, fb, f), z = std::polar(1.0, -kTwoPi * f / kRate);
            const double want = db(std::abs(0.5 * (1.0 + h / (1.0 - 0.5 * z * h))));
            const double got = gainAt(ph, p, f);
            fbCurve = fbCurve && std::fabs(got - want) < 0.15;
            if (!(std::fabs(got - want) < 0.15)) std::printf("  %s fb 0.5 at %.0f Hz: %.2f dB, theory %.2f\n", kName[m], f, got, want);
        }
        CHECK(fbCurve);
    }
}

// The flanger at depth 0, mix 0.5: (1 + z^-D) / 2, so |cos(pi f D / rate)|, notches at
// f = (2k + 1) rate / (2D); with feedback the wet is z^-D / (1 - fb z^-D).
void flangerNotches() {
    P p = params(Phaser::kFlanger);
    p.depth = 0.0f;
    p.mix = 0.5f;
    // D = 88 samples (whole: the cubic adds nothing), and 62.37 (fractional).
    for (double center : {std::log(88.0 / delaySamples(0.0)) / std::log(50.0), 0.5}) {
        const double d = delaySamples(center);
        p.center = static_cast<float>(center);
        p.feedback = 0.0f;
        Phaser ph;
        bool deep = true;
        for (int k = 0; k < 4; ++k) {
            const double f = (2 * k + 1) * kRate / (2.0 * d), g = gainAt(ph, p, f);
            deep = deep && g < -35.0;
            if (!(g < -35.0)) std::printf("  flanger D %.2f notch %d at %.1f Hz: %.1f dB\n", d, k, f, g);
        }
        CHECK(deep);
        for (float fb : {0.0f, 0.5f, -0.5f}) {
            p.feedback = fb;
            bool curve = true;
            for (double f : {100.0, 400.0, 1000.0, 1900.0}) {
                const cd z = std::polar(1.0, -kTwoPi * f * d / kRate);
                const double want = db(std::abs(0.5 * (1.0 + z / (1.0 - static_cast<double>(fb) * z))));
                if (want < -25.0) continue;
                const double got = gainAt(ph, p, f);
                curve = curve && std::fabs(got - want) < 0.15;
                if (!(std::fabs(got - want) < 0.15))
                    std::printf("  flanger D %.2f fb %.1f at %.0f Hz: %.2f dB, theory %.2f\n", d, fb, f, got, want);
            }
            CHECK(curve);
        }
    }
}

// The flanger's line, read before the sample is written, is DelayLine::readCubic at the delay
// (measured from an impulse's centre of mass, which the cubic puts exactly at a fractional delay).
void flangerMatchesDelayLine() {
    P p = params(Phaser::kFlanger);
    p.depth = 0.0f;
    p.feedback = 0.0f;
    p.mix = 1.0f;
    p.center = 0.37f;
    Buf L = impulseAt(400, 0), R = L;
    render(p, L, R);
    double sum = 0.0, moment = 0.0;
    for (int i = 0; i < 400; ++i) {
        sum += L[static_cast<size_t>(i)];
        moment += static_cast<double>(L[static_cast<size_t>(i)]) * i;
    }
    const double d = moment / sum;
    CHECK(std::fabs(sum - 1.0) < 1e-5 && std::fabs(d - delaySamples(0.37)) < 1e-3);
    const Buf in = whiteNoise(20000, 0.5f, 3);
    L = in;
    R = in;
    render(p, L, R);
    DelayLine ref(2048);
    Buf want(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        ref.write(in[i]);
        want[i] = ref.readCubic(static_cast<float>(d));
    }
    CHECK(maxDiff(L, want) < 2e-5 && maxDiff(R, want) < 2e-5);
}

// The LFO's period: impulses every `spacing` samples through the flanger (mix 1, no feedback), and
// the echo clusters they bring back. With the period `steps` spacings long, a cluster comes back
// exactly `steps` impulses later and not before.
std::vector<Buf> clusters(const P& p, Transport t, int spacing, int count, int width, Buf* right = nullptr) {
    const int n = spacing * count;
    Buf L(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; i += spacing) L[static_cast<size_t>(i)] = 1.0f;
    Buf R = L;
    render(p, L, R, t);
    if (right) *right = R;
    std::vector<Buf> c;
    for (int j = 0; j < count; ++j) c.emplace_back(L.begin() + j * spacing, L.begin() + j * spacing + width);
    return c;
}
double distance(const std::vector<Buf>& c, int k, int first) {
    double d = 0.0, e = 0.0;
    for (size_t j = static_cast<size_t>(first); j + static_cast<size_t>(k) < c.size(); ++j)
        for (size_t i = 0; i < c[j].size(); ++i) {
            const double x = c[j][i] - c[j + static_cast<size_t>(k)][i];
            d += x * x;
            e += static_cast<double>(c[j][i]) * c[j][i];
        }
    return d / e;
}

P flanger(float center) {
    P p = params(Phaser::kFlanger);
    p.depth = 1.0f;
    p.feedback = 0.0f;
    p.mix = 1.0f;
    p.center = center;   // 0.2: 19.3 samples, swept 4.8..77
    p.stereo = 0.0f;
    return p;
}

void syncedPeriod() {
    struct Case {
        double bpm, div;
        int spacing, steps;   // 60 / bpm * div * rate = spacing * steps
        bool playing;
    };
    for (const Case cs : {Case{120.0, 1.0, 1050, 21, true}, Case{100.0, 0.75, 1323, 15, true}, Case{140.0, 4.0, 1050, 72, true},
                          Case{120.0, 1.0, 1050, 21, false}}) {
        P p = flanger(0.2f);
        p.sync = true;
        p.divBeats = cs.div;
        p.rateHz = 13.0f;   // ignored when synced
        Transport t;
        t.bpm = cs.bpm;
        t.playing = cs.playing;
        t.valid = true;
        const std::vector<Buf> c = clusters(p, t, cs.spacing, 2 + 2 * cs.steps + 1, 100);
        CHECK(distance(c, cs.steps, 2) < 1e-5);
        bool moves = true;
        for (int k = 1; k < cs.steps; ++k) moves = moves && distance(c, k, 2) > 1e-3;
        CHECK(moves);
        if (!(distance(c, cs.steps, 2) < 1e-5) || !moves) std::printf("  sync at %.0f BPM, %.3f beats\n", cs.bpm, cs.div);
    }
    // Free: the rate.
    P p = flanger(0.2f);
    p.rateHz = 1.75f;   // 25200 samples = 24 * 1050
    const std::vector<Buf> c = clusters(p, Transport{}, 1050, 2 + 48 + 1, 100);
    CHECK(distance(c, 24, 2) < 1e-5 && distance(c, 23, 2) > 1e-3 && distance(c, 12, 2) > 1e-3);
}

// Synced and playing, the LFO's phase is the song position over the division, whatever came
// before: a run that starts elsewhere and jumps to where another run is plays exactly as that one
// once it has caught up (the wet out for 8 chunks, the jump, 4 chunks unheard, 8 back in: 20
// chunks; no feedback, so the lines hold the same input). And the delay itself: at a quarter cycle
// the triangle's top (4x the base delay), at three quarters its bottom (1/4).
void followsTransport() {
    P p = flanger(0.2f);
    p.sync = true;
    p.divBeats = 2.0;   // 1 s at 120 BPM
    const Buf in = whiteNoise(44100, 0.5f, 4);
    Buf a = in, ar = in, b = in, br = in;
    Phaser pa, pb;
    const int jump = 300 * kChunk;
    for (int i = 0; i < 44100; i += kChunk) {
        Transport t;
        t.playing = t.valid = true;
        t.beats = i / kRate * 2.0;
        pa.set(p, t);
        pa.process(&a[static_cast<size_t>(i)], &ar[static_cast<size_t>(i)], std::min(kChunk, 44100 - i));
        if (i < jump) t.beats += 5.37;
        pb.set(p, t);
        pb.process(&b[static_cast<size_t>(i)], &br[static_cast<size_t>(i)], std::min(kChunk, 44100 - i));
    }
    CHECK(maxDiff(a, b, 0, jump) > 0.01);
    const size_t caught = jump + 21 * kChunk;
    CHECK(maxDiff(a, b, caught) == 0.0 && maxDiff(ar, br, caught) == 0.0);
    CHECK(maxDiff(a, b, jump, caught) > 0.01);   // it did take that long, fading through the dry

    // A small slip (1/500 of a cycle, under the jump's 1/256) is caught up an eighth per chunk
    // instead, the wet staying in: within 2^-20 of a cycle after 57 chunks, exact from then on, as
    // a run that had the shifted song position all along.
    Buf c = in, cr = in, d = in, dr = in;
    Phaser pc, pd;
    for (int i = 0; i < 44100; i += kChunk) {
        Transport t;
        t.playing = t.valid = true;
        t.beats = i / kRate * 2.0 + 0.004;
        pc.set(p, t);
        pc.process(&c[static_cast<size_t>(i)], &cr[static_cast<size_t>(i)], std::min(kChunk, 44100 - i));
        if (i < jump) t.beats -= 0.004;
        pd.set(p, t);
        pd.process(&d[static_cast<size_t>(i)], &dr[static_cast<size_t>(i)], std::min(kChunk, 44100 - i));
    }
    const size_t slewed = jump + 60 * kChunk;
    CHECK(maxDiff(c, d, jump, slewed) > 0.0 && maxDiff(c, d, slewed) == 0.0 && maxDiff(cr, dr, slewed) == 0.0);
    // Faded out (mix 1 here), a chunk is the input exactly: the jump has some, the slip none.
    auto dry = [&in](const Buf& x) {
        int chunks = 0;
        for (size_t k = static_cast<size_t>(jump); k < static_cast<size_t>(jump) + 21 * kChunk; k += kChunk)
            chunks += maxDiff(x, in, k, k + kChunk) == 0.0;
        return chunks;
    };
    CHECK(dry(b) > 0 && dry(d) == 0);

    // Echo times at the triangle's ends, where the delay stands still for a moment.
    const double base = delaySamples(0.2);
    for (const double phase : {0.25, 0.75}) {
        const double want = base * (phase < 0.5 ? 4.0 : 0.25);
        const int at = static_cast<int>(phase * kRate) - static_cast<int>(want);   // the echo lands on the phase
        Buf L = impulseAt(44100, at), R = L;
        Transport t;
        t.playing = t.valid = true;
        render(p, L, R, t);
        double sum = 0.0, moment = 0.0;
        for (int i = at; i < at + 150; ++i) {
            sum += L[static_cast<size_t>(i)];
            moment += static_cast<double>(L[static_cast<size_t>(i)]) * (i - at);
        }
        CHECK(std::fabs(moment / sum - want) < 0.05);
        if (!(std::fabs(moment / sum - want) < 0.05))
            std::printf("  phase %.2f: echo at %.3f, want %.3f\n", phase, moment / sum, want);
    }
}

// Stereo 180: the sides sweep in antiphase. The flanger's delays mirror around the base delay in
// octaves (dL dR = base^2); a phaser's right side plays what its left plays half a cycle later
// (a sine whose period divides a quarter cycle as input), at 0 degrees the same, at 90 a quarter.
void stereoAntiphase() {
    P p = flanger(0.3f);
    p.stereo = 180.0f;
    p.rateHz = 0.5f;
    Buf R;
    const std::vector<Buf> l = clusters(p, Transport{}, 512, 160, 140, &R);
    const double base = delaySamples(0.3);
    bool mirrored = true;
    for (int j = 2; j < 160; ++j) {
        double sl = 0.0, ml = 0.0, sr = 0.0, mr = 0.0;
        for (int i = 0; i < 140; ++i) {
            sl += l[static_cast<size_t>(j)][static_cast<size_t>(i)];
            ml += static_cast<double>(l[static_cast<size_t>(j)][static_cast<size_t>(i)]) * i;
            sr += R[static_cast<size_t>(j * 512 + i)];
            mr += static_cast<double>(R[static_cast<size_t>(j * 512 + i)]) * i;
        }
        mirrored = mirrored && std::fabs(std::log2(ml / sl * (mr / sr) / (base * base))) < 0.02;
    }
    CHECK(mirrored);

    P q = params(Phaser::kPhaser8);
    q.rateHz = 0.5f;   // a cycle of 88200 samples: a quarter is 441 periods of 882 Hz
    for (const float deg : {180.0f, 90.0f, 0.0f}) {
        q.stereo = deg;
        Buf L = sine(882.0, 4 * 44100, 0.3f);
        Buf Rq = L;
        render(q, L, Rq);
        const size_t shift = static_cast<size_t>(deg / 360.0f * 88200.0f);
        double diff = 0.0;
        for (size_t i = 66150; i < 132300; ++i) diff = std::max(diff, static_cast<double>(std::fabs(Rq[i] - L[i + shift])));
        CHECK(diff < 1e-4);
        if (!(diff < 1e-4)) std::printf("  phaser stereo %.0f: %g\n", deg, diff);
    }
}

// Feedback +-0.95 at the fastest rate and full depth, loud noise: finite and bounded. The
// flanger's bound is the theory's: the saturator holds the feedback under 0.95 * 4, so the line
// under 1 + 3.8, the cubic read under 1.25 x that, and the mix between that and the input.
void feedbackStable() {
    const Buf inL = whiteNoise(44100, 1.0f, 5), inR = whiteNoise(44100, 1.0f, 6);
    for (int m = 0; m < kModes; ++m)
        for (float fb : {0.95f, -0.95f})
            for (float center : {0.0f, 0.5f, 1.0f})
                for (float mix : {0.5f, 1.0f}) {
                    P p = params(m);
                    p.feedback = fb;
                    p.rateHz = 20.0f;
                    p.depth = 1.0f;
                    p.center = center;
                    p.mix = mix;
                    Buf L = inL, R = inR;
                    render(p, L, R);
                    const float top = std::max(peak(L), peak(R));
                    const float bound = m == Phaser::kFlanger ? 6.0f : 20.0f;
                    CHECK(allFinite(L) && allFinite(R) && top < bound);
                    if (!(top < bound)) std::printf("  %s fb %.2f centre %.1f mix %.1f: peak %.2f\n", kName[m], fb, center, mix, top);
                }
}

// New, random (out-of-range, NaN) parameters and transport every chunk, modes and sync included:
// finite, bounded.
void randomJumps() {
    uint32_t s = 77;
    auto r = [&s] { return 0.5f + 0.5f * randBipolar(s); };
    Buf L = whiteNoise(5 * 44100, 1.0f, 7), R = whiteNoise(5 * 44100, 1.0f, 8);
    Phaser ph;
    for (size_t i = 0; i < L.size(); i += kChunk) {
        P p;
        p.mode = static_cast<int>(r() * 6.0f) - 1;
        p.sync = r() < 0.5f;
        p.rateHz = r() * 30.0f - 2.0f;
        p.divBeats = r() * 80.0 - 4.0;
        p.depth = r() * 1.4f - 0.2f;
        p.center = r() * 1.4f - 0.2f;
        p.feedback = r() * 2.4f - 1.2f;
        p.stereo = r() * 300.0f - 50.0f;
        p.mix = r() * 1.4f - 0.2f;
        if (r() < 0.02f) p.center = std::nanf("");
        if (r() < 0.02f) p.feedback = std::nanf("");
        if (r() < 0.02f) p.divBeats = std::nan("");
        if (r() < 0.02f) p.rateHz = INFINITY;
        Transport t;
        t.playing = r() < 0.7f;
        t.valid = r() < 0.9f;
        t.bpm = r() * 1200.0;
        t.beats = r() * 1e4 - 100.0;
        if (r() < 0.02f) t.bpm = std::nan("");
        if (r() < 0.02f) t.beats = INFINITY;
        ph.set(p, t);
        ph.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kChunk, L.size() - i)));
    }
    CHECK(allFinite(L) && allFinite(R) && peak(L) < 20.0f && peak(R) < 20.0f);
}

// A NaN or an infinity in the input never reaches a line or an allpass: the output is what a 0
// there gives.
void nanInput() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.feedback = 0.8f;
        Buf L = whiteNoise(20000, 0.5f, 9), R = whiteNoise(20000, 0.5f, 10);
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

// Blocks of 1, 7 and 32 samples with constant parameters: the same output (to the last bit where
// the compiler runs every sample through the same instructions; an ulp where it peels one). Synced
// and playing, each block takes the LFO's phase from the song position, which comes out a few
// 2^-32 of a cycle apart for different blocks; where that crosses one of the 2^-24 steps the
// sweep uses, the flanger's delay moves 2e-5 samples: within 1e-4.
void blockSizes() {
    for (int m = 0; m < kModes; ++m)
        for (int sync = 0; sync < 2; ++sync) {
            P p = params(m);
            p.rateHz = 3.0f;
            p.depth = 0.9f;
            p.feedback = 0.7f;
            p.sync = sync;
            p.divBeats = 0.5;
            Transport t;
            t.playing = t.valid = sync;
            t.beats = 3.25;
            const Buf inL = whiteNoise(20000, 0.5f, 11), inR = whiteNoise(20000, 0.5f, 12);
            Buf L32 = inL, R32 = inR, L7 = inL, R7 = inR, L1 = inL, R1 = inR;
            render(p, L32, R32, t, 32);
            render(p, L7, R7, t, 7);
            render(p, L1, R1, t, 1);
            const double tol = sync ? 1e-4 : 1e-6;
            CHECK(maxDiff(L32, L7) < tol && maxDiff(R32, R7) < tol && maxDiff(L32, L1) < tol && maxDiff(R32, R1) < tol);
        }
}

// reset() forgets everything: afterwards it plays exactly as a new one.
void resetClears() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.feedback = 0.9f;
        Phaser used;
        Buf L = whiteNoise(30000, 0.8f, 13), R = whiteNoise(30000, 0.8f, 14);
        run(used, p, L, R);
        used.reset();
        const Buf inL = whiteNoise(10000, 0.5f, 15), inR = whiteNoise(10000, 0.5f, 16);
        Buf a = inL, b = inR, c = inL, d = inR;
        run(used, p, a, b);
        render(p, c, d);
        CHECK(same(a, c) && same(b, d));
    }
}

// tailSamples() against an impulse response's fall to 60 dB under its peak: no later than the
// estimate, and the estimate not far beyond (the flanger's echoes fall by the feedback per pass;
// the phaser's estimate takes its loop's longest delay, at DC, so it may run long).
void tail() {
    struct Case {
        int mode;
        float feedback;
        double slack;   // the estimate may be this many times the measurement
    };
    for (const Case cs : {Case{Phaser::kFlanger, 0.9f, 1.3}, Case{Phaser::kFlanger, -0.9f, 1.3}, Case{Phaser::kFlanger, 0.0f, 1.5},
                          Case{Phaser::kPhaser8, 0.5f, 4.0}, Case{Phaser::kPhaser12, 0.0f, 2.0}, Case{Phaser::kPhaser4, -0.7f, 4.0}}) {
        P p = params(cs.mode);
        p.depth = 0.0f;
        p.mix = 1.0f;
        p.feedback = cs.feedback;
        Phaser ph;
        Buf L = impulseAt(60000, 0), R = L;
        run(ph, p, L, R);
        const float top = peak(L);
        int last = 0;
        for (int i = 0; i < 60000; ++i)
            if (std::fabs(L[static_cast<size_t>(i)]) > 1e-3f * top) last = i;
        const int est = ph.tailSamples();
        CHECK(last <= est && est <= cs.slack * last + 8);
        if (!(last <= est && est <= cs.slack * last + 8))
            std::printf("  %s fb %.1f: rings to %d, tail %d\n", kName[cs.mode], cs.feedback, last, est);
    }
}

// A 440 Hz sine through each mode, then abrupt changes of every parameter at chunk edges: no
// clicks. A click is broadband, a phased or flanged sine is not, so the measure is the largest
// fourth difference over the level: a sine's is (2 sin(pi f / rate))^4, 1.6e-5 at 440 Hz; a
// step's 3x the step, a corner in a gain's path 2x the change of slope. After each change, its
// first 0.1 s against the same settings settled (the segment's last 0.15 s: a fast flanger's
// triangle turns its pitch around at once, a corner of its own): within 3x, or under 0.01 (a step
// of 0.3% of the level, -50 dB; the cross-fades' corners stay under half that, a cleared delay
// line coming back in a step would be 0.1).
double jolt(const Buf& v, size_t from, size_t to) {
    double c = 0.0;
    for (size_t i = from + 4; i < to; ++i)
        c = std::max(c, static_cast<double>(std::fabs(v[i] - 4.0f * v[i - 1] + 6.0f * v[i - 2] - 4.0f * v[i - 3] + v[i - 4])));
    return c / std::max(1e-3, static_cast<double>(peak(v, from, to)));
}
void noClicks() {
    for (int m = 0; m < kModes; ++m) {
        const size_t seg = 413 * kChunk;   // 0.3 s
        Buf L = sine(440.0, static_cast<int>(12 * seg), 0.5f), R = L;
        Phaser ph;
        P p = params(m);
        bool clean = true;
        for (int s = 0; s < 12; ++s) {
            switch (s) {   // the change at the start of segment s
            case 3: p.center = 0.7f; break;
            case 4: p.depth = 1.0f; break;
            case 5: p.feedback = -0.6f; break;
            case 6: p.stereo = 180.0f; break;
            case 7: p.rateHz = 1.5f; break;
            case 8: p.mix = 1.0f; break;
            case 9: p.mode = (m + 1) % kModes; break;   // all wet: the fade is a whole cross-fade
            case 10: p.mix = 0.0f; break;
            case 11:
                p.mix = 0.5f;
                p.center = 0.3f;
                break;
            default: break;
            }
            const size_t from = static_cast<size_t>(s) * seg;
            for (size_t i = from; i < from + seg; i += kChunk) {
                ph.set(p, Transport{});
                ph.process(&L[i], &R[i], kChunk);
            }
            if (s == 0) continue;   // the effect's own start
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

// Synced, the LFO follows the transport through its jumps without a click: a locate, a stop and a
// play from elsewhere, a 1-bar loop wrapping under a 2-bar LFO (half a cycle each pass). The
// flanger's read would jump up to 250 samples here, a phaser's coefficients all at once; instead
// the wet fades out, the LFO catches up, and the wet fades back in. Each event's 0.15 s against a
// quiet stretch before it, by the fourth-difference measure above: within 3x, or under 0.01.
void transportClicks() {
    for (int m : {static_cast<int>(Phaser::kFlanger), static_cast<int>(Phaser::kPhaser12)}) {
        P p = params(m);
        p.sync = true;
        p.divBeats = 8.0;
        p.depth = 1.0f;
        p.feedback = 0.8f;
        const int n = 3500 * kChunk;
        Buf L = sine(440.0, n, 0.5f), R = L;
        Phaser ph;
        const int locate = 1100 * kChunk, stop = 1500 * kChunk, play = 1700 * kChunk, loop = 2200 * kChunk;
        int wrap = 0;
        for (int i = 0; i < n; i += kChunk) {
            Transport t;
            t.valid = true;
            t.playing = i < stop || i >= play;
            const double s = i / kRate * 2.0;   // beats at 120 BPM
            if (i < locate) t.beats = 0.3 + s;
            else if (i < play) t.beats = 3.0 + std::min(s, stop / kRate * 2.0);
            else if (i < loop) t.beats = 9.1 + (s - play / kRate * 2.0);
            else {
                t.beats = 15.0 + (s - loop / kRate * 2.0);   // the loop is bars 4..5: beats 12 to 16
                if (t.beats >= 16.0) {
                    if (wrap == 0) wrap = i;
                    t.beats -= 4.0;
                }
            }
            ph.set(p, t);
            ph.process(&L[static_cast<size_t>(i)], &R[static_cast<size_t>(i)], kChunk);
        }
        bool clean = true;
        for (int at : {locate, play, wrap}) {
            const size_t from = static_cast<size_t>(at);
            for (const Buf* x : {&L, &R}) {
                const double event = jolt(*x, from, from + 6615), quiet = jolt(*x, from - 11025, from - 2205);
                const bool ok = event < std::max(0.01, 3.0 * quiet);
                clean = clean && ok;
                if (!ok) std::printf("  %s, transport event at %zu: %.5f, before it %.5f\n", kName[m], from, event, quiet);
            }
        }
        CHECK(wrap > 0 && clean);
    }
}

} // namespace

void phaserTests() {
    dryAtMixZero();
    phaserNotches();
    flangerNotches();
    flangerMatchesDelayLine();
    syncedPeriod();
    followsTransport();
    stereoAntiphase();
    feedbackStable();
    randomJumps();
    nanInput();
    blockSizes();
    resetClears();
    tail();
    noClicks();
    transportClicks();
}
