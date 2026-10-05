// dsp/grain.h against theory: mix 0 is the input exactly; pitched grains land on the interval (to
// a few cents) with the original gone, and what a shift would push past Nyquist is filtered out
// instead of aliasing; reversed grains run a chirp backwards; slices, repeats and notes start on
// MPC's grid and follow it through a locate without a click; Arp steps through its pattern on the
// song position, Mosaic's slices move by octaves, Stretch crawls; hold freezes what the grains
// play; the level holds across densities; feedback 0.95 stays bounded and keeps the texture going.
// Then the contract: no clicks (steady, on changes), extremes, random jumps, NaN input, block
// sizes, reset, tail.
#include "signal.h"
#include "../dsp/grain.h"

#include <complex>
#include <cstdio>
#include <cstring>

namespace {

using namespace eft;
using ef::Grain;
using ef::Transport;
using P = Grain::Params;

constexpr int kModes = Grain::kModes;
const char* const kName[kModes] = {"Cloud", "Stretch", "Mosaic", "Stutter", "Arp"};
constexpr int kSr = 44100;
constexpr int kCh = ef::kChunk;

// Free-running, all wet: what most tests measure.
P params(int mode, float ms = 120.0f) {
    P p;
    p.mode = mode;
    p.sync = false;
    p.sizeMs = ms;
    p.mix = 1.0f;
    return p;
}

bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }
double maxDiff(const Buf& a, const Buf& b, size_t from = 0) {
    double m = 0.0;
    for (size_t i = from; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}
Buf silence(int n) { return Buf(static_cast<size_t>(n), 0.0f); }
Buf concat(Buf a, const Buf& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
float peak2(const Buf& L, const Buf& R, size_t from = 0) { return std::max(peak(L, from), peak(R, from)); }

void render(const P& p, Buf& L, Buf& R, Transport t = {}, int chunk = kCh) {
    Grain g;
    run(g, p, L, R, t, chunk);
}

// Runs g in chunks with the song position beats(i) at each chunk's first sample, playing at bpm.
template <class F>
void runSong(Grain& g, const P& p, Buf& L, Buf& R, double bpm, F beats) {
    for (size_t i = 0; i < L.size(); i += kCh) {
        Transport t;
        t.bpm = bpm;
        t.playing = t.valid = true;
        t.beats = beats(static_cast<int>(i));
        g.set(p, t);
        g.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kCh, L.size() - i)));
    }
}

double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

// The strongest a component of x[from, to) within +-50 cents of `hz` is. Grains of a tone add at
// random phases, so their spectrum is speckled: the peak of a single long window wanders by cents,
// but the level near the tone is clear.
double near(const Buf& x, double hz, size_t from, size_t to) {
    double top = 0.0;
    for (double c = -50.0; c <= 50.0; c += 2.0) top = std::max(top, magnitude(x, hz * std::pow(2.0, c / 1200.0), from, to));
    return top;
}

// The frequency of x[from, from + len) by its zero crossings, placed between samples.
double zcHz(const Buf& x, size_t from, size_t len) {
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

// The frequency of the tone near `hz` in x[from, to): x shifted down by hz (a complex signal),
// summed over a few periods, and the phase that turns through in 20 ms, averaged weighted by level.
// Grains of one tone at random phases are still that tone, the phase of their sum wandering as
// they come and go: the wander averages out, and where they cancel (it moves fastest) the weight is
// small. (The peak of one long spectrum is speckled by the random phases and wanders by cents.)
double toneHz(const Buf& x, double hz, size_t from, size_t to) {
    using cd = std::complex<double>;
    const double w = 2.0 * kPi * hz / kSr;
    const size_t avg = std::max<size_t>(441, static_cast<size_t>(4.0 * kSr / hz)), lag = 882;
    std::vector<cd> sum(to - from + 1, cd(0.0, 0.0));
    for (size_t i = from; i < to; ++i) sum[i - from + 1] = sum[i - from] + static_cast<double>(x[i]) * std::polar(1.0, -w * static_cast<double>(i));
    cd c(0.0, 0.0);
    for (size_t i = 0; i + avg + lag < sum.size(); i += 8) c += std::conj(sum[i + avg] - sum[i]) * (sum[i + lag + avg] - sum[i + lag]);
    return hz + std::arg(c) / (2.0 * kPi * static_cast<double>(lag) / kSr);
}

// A click is broadband, grains of a sine are not: the largest fourth difference over the level.
// A sine's is (2 sin(pi f / rate))^4, 1.6e-5 at 440 Hz, 2.5e-4 at 880; a step's 3x the step, a
// corner in a gain's path 2x the change of slope.
double jolt(const Buf& v, size_t from, size_t to) {
    double c = 0.0;
    for (size_t i = from + 4; i < to; ++i)
        c = std::max(c, static_cast<double>(std::fabs(v[i] - 4.0f * v[i - 1] + 6.0f * v[i - 2] - 4.0f * v[i - 3] + v[i - 4])));
    return c / std::max(1e-3, static_cast<double>(peak(v, from, to)));
}

// An exponential chirp from f0 to f1 over n samples.
Buf chirp(double f0, double f1, int n, float amp = 0.5f) {
    Buf x(static_cast<size_t>(n));
    const double k = std::log(f1 / f0) / n;
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * kPi * f0 / kSr * (std::exp(k * i) - 1.0) / k));
    return x;
}

void dryAtMixZero() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.mix = 0.0f;
        p.feedback = 0.9f;
        p.density = 1.0f;
        p.pitch = 7.0f;
        p.reverse = 0.5f;
        p.spread = 1.0f;
        const Buf inL = whiteNoise(30000, 0.5f, 1), inR = whiteNoise(30000, 0.5f, 2);
        Buf L = inL, R = inR;
        render(p, L, R);
        CHECK(same(L, inL) && same(R, inR));
    }
}

// A steady 440 Hz sine through Cloud (and Stretch): the grains are at 440 x 2^(pitch/12). Sparse
// and undetuned (density 0, spread 0: mostly one grain at a time), to a fifth of a cent. At a
// moderate density, with Cloud's +-3 cents of random detune (symmetric), within 3 cents over 10 s
// (Stretch 5: its grains come from a few places only, so their phases wander longer); the
// original 30 dB under the shifted tone.
void pitchShift() {
    struct Case {
        int mode;
        float semis;
    };
    for (const Case c : {Case{Grain::kCloud, 12.0f}, Case{Grain::kCloud, -12.0f}, Case{Grain::kCloud, 7.0f}, Case{Grain::kCloud, 0.0f},
                         Case{Grain::kCloud, 19.0f}, Case{Grain::kCloud, -24.0f}, Case{Grain::kCloud, 24.0f},
                         Case{Grain::kStretch, 12.0f}, Case{Grain::kStretch, -5.0f}})
        for (const bool sparse : {true, false}) {
            P p = params(c.mode, sparse ? 300.0f : 120.0f);
            p.pitch = c.semis;
            p.density = sparse ? 0.0f : 0.4f;
            p.spread = sparse ? 0.0f : 0.5f;
            const int n = (sparse ? 5 : 12) * kSr;
            Buf L = sine(440.0, n, 0.5f), R = L;
            render(p, L, R);
            const double want = 440.0 * std::pow(2.0, c.semis / 12.0);
            const size_t from = 2 * kSr, to = static_cast<size_t>(n);
            const double f = toneHz(L, want, from, to), top = near(L, want, from, from + kSr), orig = near(L, 440.0, from, from + kSr);
            const double tol = sparse ? (c.mode == Grain::kCloud ? 0.2 : 0.5) : (c.mode == Grain::kCloud ? 3.0 : 5.0);
            const bool ok = std::fabs(cents(f, want)) < tol && (c.semis == 0.0f || orig < 0.03 * top) && top > 0.05;
            CHECK(ok);
            if (!ok)
                std::printf("  %s %+.0f %s: %.3f Hz (%.2f cents), level %.3f, 440 Hz at %.4f\n", kName[c.mode], c.semis,
                            sparse ? "sparse" : "dense", f, cents(f, want), top, orig);
        }
}

// A shift up reads a band-limited copy of the buffer: what would land past 22 kHz was filtered out
// first. A tone that would come out at 26..30 kHz comes out not at all (under -60 dB, where a cubic
// read of the full-rate buffer folds it back at -10 dB or so); one that lands at 18 kHz passes.
void antiAliasing() {
    struct Case {
        float semis;
        double hz;
        bool passes;
    };
    for (const Case c : {Case{12.0f, 15000.0, false}, Case{24.0f, 7000.0, false}, Case{19.0f, 9000.0, false}, Case{12.0f, 9000.0, true},
                         Case{24.0f, 4500.0, true}, Case{0.0f, 18000.0, true}}) {
        P p = params(Grain::kCloud);
        p.pitch = c.semis;
        p.spread = 0.0f;   // no detune
        p.density = 0.5f;
        const Buf in = sine(c.hz, 3 * kSr, 0.5f);
        Buf L = in, R = in;
        render(p, L, R);
        const double gain = db(rms(L, kSr) / rms(in, kSr));
        const bool ok = c.passes ? gain > -4.0 && gain < 3.0 : gain < -60.0;
        CHECK(ok);
        if (!ok) std::printf("  %+.0f semitones of %.0f Hz: %.1f dB\n", c.semis, c.hz, gain);
    }
}

// A rising chirp: a forward grain rises, a reversed one falls. Mosaic, one slice back and nothing
// pitched: every 200 ms slice is the one before, so with reverse 1 every slice falls, with 0 every
// one rises. Cloud, sparse 150 ms grains: most of its 20 ms windows fall from the one before with
// reverse 1, almost none with 0.
void reverseGrains() {
    const int n = 6 * kSr;
    const Buf in = chirp(300.0, 3000.0, n);
    for (const float rev : {0.0f, 1.0f}) {
        P p = params(Grain::kMosaic, 200.0f);
        p.spread = 0.0f;
        p.density = 0.0f;
        p.reverse = rev;
        Buf L = in, R = in;
        render(p, L, R);
        const int s = 8820, w = 882;
        int rising = 0, falling = 0;
        for (int k = 3; k < n / s; ++k) {
            const double f1 = zcHz(L, static_cast<size_t>(k * s + 600), w), f2 = zcHz(L, static_cast<size_t>((k + 1) * s - 600 - w), w);
            (f2 > f1 ? rising : falling)++;
        }
        CHECK(rev > 0.0f ? (falling >= 20 && rising == 0) : (rising >= 20 && falling == 0));
        if (rev > 0.0f ? rising : falling) std::printf("  Mosaic reverse %.0f: %d slices rise, %d fall\n", rev, rising, falling);

        P q = params(Grain::kCloud, 150.0f);
        q.spread = 0.0f;
        q.density = 0.0f;
        q.reverse = rev;
        L = in;
        R = in;
        render(q, L, R);
        int up = 0, down = 0;
        for (size_t i = kSr; i + 2 * w < static_cast<size_t>(n); i += w) {
            if (rms(L, i, i + 2 * w) < 0.05) continue;
            const double f1 = zcHz(L, i, w), f2 = zcHz(L, i + w, w);
            (f2 > f1 ? up : down)++;
        }
        const double share = down / static_cast<double>(up + down);
        CHECK(rev > 0.0f ? share > 0.75 : share < 0.25);
        if (!(rev > 0.0f ? share > 0.75 : share < 0.25)) std::printf("  Cloud reverse %.0f: %.2f of the windows fall\n", rev, share);
    }
}

// 125 BPM, sixteenths: 5292 samples a slice, 21168 a beat. A click 1000 samples after every beat;
// Mosaic (eight slices back) and Stutter (always) play whole grid slices, so every click that comes
// out is exactly 1000 samples after a grid line, and Mosaic's land on other sixteenths too.
void syncContent() {
    const int s = 5292, beat = 4 * s, n = 40 * beat;
    for (const int m : {static_cast<int>(Grain::kMosaic), static_cast<int>(Grain::kStutter)}) {
        P p = params(m);
        p.sync = true;
        p.sizeBeats = 0.25;
        p.spread = 1.0f;
        p.density = m == Grain::kStutter ? 1.0f : 0.0f;
        Transport t;
        t.bpm = 125.0;
        t.playing = t.valid = true;
        Buf L = silence(n);
        for (int i = 1000; i < n; i += beat) L[static_cast<size_t>(i)] = 1.0f;
        Buf R = L;
        render(p, L, R, t);
        int count = 0, moved = 0, off = 0;
        for (int i = 0; i < n; ++i) {
            const float v = std::max(std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)]));
            if (v < 0.25f) continue;
            ++count;
            off += (i - 1000) % s != 0;
            moved += (i - 1000) % beat != 0;
        }
        CHECK(count > 30 && off == 0 && moved > 10);
        if (!(count > 30 && off == 0 && moved > 10)) std::printf("  %s: %d clicks, %d off the grid, %d moved\n", kName[m], count, off, moved);
    }
}

// Where slices start, seen on a constant input: each slice (repeat, stretch of live input) has its
// own pan, so the output is flat but for the 5 ms crossfades that start on boundaries. 125 BPM
// sixteenths, then a locate from beat 12.4 to 37.3 (a fifth into a slice): a slice starts right
// there, partway, and the next ones on the new grid. The output changes only inside those
// crossfades, and at most of them it does. Then the same with a sine: the locate's crossfade is no
// harder than any other boundary's.
void syncBoundaries() {
    const double bpm = 125.0, spb = kSr * 60.0 / bpm;   // 21168 samples a beat
    const int n = 30 * 21168, at = 8200 * kCh;          // the locate, at beat 12.398
    const auto beats = [&](int i) { return i < at ? i / spb : 37.3 + (i - at) / spb; };
    // The grid's boundaries as samples: the first sample at or past each sixteenth.
    std::vector<int> grid;
    for (double g = 0.0; g < 12.4; g += 0.25) grid.push_back(static_cast<int>(std::ceil(g * spb - 1e-6)));
    grid.push_back(at);
    for (double g = 37.5; g < 60.0; g += 0.25) grid.push_back(at + static_cast<int>(std::ceil((g - 37.3) * spb - 1e-6)));
    for (const int m : {static_cast<int>(Grain::kMosaic), static_cast<int>(Grain::kStutter)}) {
        P p = params(m);
        p.sync = true;
        p.sizeBeats = 0.25;
        p.spread = 1.0f;
        p.density = m == Grain::kStutter ? 0.7f : 0.0f;
        Grain g;
        Buf L(static_cast<size_t>(n), 0.5f), R = L;
        runSong(g, p, L, R, bpm, beats);
        std::vector<char> fade(static_cast<size_t>(n), 0);   // inside a crossfade from a boundary
        for (const int b : grid)
            for (int i = b; i <= b + 222 && i < n; ++i) fade[static_cast<size_t>(i)] = 1;
        int stray = 0, moving = 0, seen = 0, locate = 0;
        for (size_t b = 1; b < grid.size(); ++b) {
            if (grid[b] < 2 * 21168) continue;
            bool moved = false;
            for (int i = grid[b] + 1; i <= grid[b] + 222 && i < n; ++i) moved = moved || std::fabs(L[i] - L[i - 1]) > 1e-6f;
            seen += moved;
            locate += moved && grid[b] == at;
        }
        for (int i = 2 * 21168; i < n; ++i) {
            if (!(std::fabs(L[static_cast<size_t>(i)] - L[static_cast<size_t>(i - 1)]) > 1e-6f)) continue;
            ++moving;
            if (!fade[static_cast<size_t>(i - 1)]) {
                if (!stray) std::printf("  %s: the output moves at %d, outside a crossfade\n", kName[m], i);
                ++stray;
            }
        }
        CHECK(stray == 0 && moving > 1000 && seen > 30);
        if (m == Grain::kMosaic) CHECK(locate == 1);

        // The same locate on a 300 Hz sine.
        Grain h;
        Buf S = sine(300.0, n, 0.5f), T = S;
        runSong(h, p, S, T, bpm, beats);
        const double event = jolt(S, at - 8, at + 2000), quiet = jolt(S, at - 40000, at - 2000);
        CHECK(event < std::max(0.01, 3.0 * quiet));
        if (!(event < std::max(0.01, 3.0 * quiet))) std::printf("  %s locate: %.5f, before it %.5f\n", kName[m], event, quiet);
    }
}

// Arp at 120 BPM, eighths (11025 samples), density 0.5: the pattern 0, 7, 12, 19 on a 440 Hz sine,
// step = the slice's number in the song. Each slice's middle is its step's pitch, to a cent.
void arpSteps() {
    P p = params(Grain::kArp);
    p.sync = true;
    p.sizeBeats = 0.5;
    p.density = 0.5f;
    p.spread = 0.0f;
    Transport t;
    t.bpm = 120.0;
    t.playing = t.valid = true;
    t.beats = 0.0;
    const int s = 11025, n = 20 * s;
    Buf L = sine(440.0, n, 0.5f), R = L;
    render(p, L, R, t);
    const int steps[4] = {0, 7, 12, 19};
    bool ok = true;
    for (int k = 2; k < 20; ++k) {
        const double want = 440.0 * std::pow(2.0, steps[k % 4] / 12.0);
        const size_t from = static_cast<size_t>(k * s + 2000), to = from + 6000;
        const double f = toneHz(L, want, from, to);
        const bool good = std::fabs(cents(f, want)) < 1.0 && magnitude(L, want, from, to) > 0.3;
        ok = ok && good;
        if (!good) std::printf("  Arp slice %d: %.2f Hz, want %.2f\n", k, f, want);
    }
    CHECK(ok);
}

// Mosaic at density 1: about 70% of the slices an octave up or down. On a 440 Hz sine every
// slice's middle is 220, 440 or 880 Hz, the other two far under it, and each of them occurs.
void mosaicOctaves() {
    P p = params(Grain::kMosaic, 200.0f);
    p.density = 1.0f;
    p.spread = 0.5f;
    const int s = 8820, n = 40 * s;
    Buf L = sine(440.0, n, 0.5f), R = L;
    render(p, L, R);
    int seen[3] = {}, unclear = 0;
    for (int k = 4; k < 40; ++k) {
        const size_t from = static_cast<size_t>(k * s + 1500), to = from + 5000;
        double m[3];
        for (int o = 0; o < 3; ++o) m[o] = magnitude(L, 220.0 * (1 << o), from, to);
        const int best = static_cast<int>(std::max_element(m, m + 3) - m);
        bool clear = m[best] > 0.2;
        for (int o = 0; o < 3; ++o) clear = clear && (o == best || m[o] < 0.05 * m[best]);
        unclear += !clear;
        ++seen[best];
    }
    CHECK(unclear == 0 && seen[0] > 0 && seen[1] > 0 && seen[2] > 0);
    if (unclear || !seen[0] || !seen[1] || !seen[2]) std::printf("  Mosaic octaves: %d / %d / %d, %d unclear\n", seen[0], seen[1], seen[2], unclear);
}

// Stretch crawls: 300 Hz for 2 s, then 600 Hz. Its head starts 30 ms back and falls behind at 7/8
// of real time; at spread 0.6 it starts again from the present 4 s back, 4.54 s in. Until then it
// plays the first 0.6 s of input, 300 Hz long after the input changed; afterwards 600 Hz.
void stretchCrawls() {
    P p = params(Grain::kStretch);
    p.spread = 0.6f;
    p.density = 0.6f;
    Buf L = concat(sine(300.0, 2 * kSr, 0.5f), sine(600.0, 5 * kSr, 0.5f)), R = L;
    render(p, L, R);
    const size_t a = static_cast<size_t>(2.3 * kSr), b = static_cast<size_t>(4.4 * kSr), c = static_cast<size_t>(5.0 * kSr);
    CHECK(magnitude(L, 300.0, a, b) > 0.1 && magnitude(L, 600.0, a, b) < 0.01 * magnitude(L, 300.0, a, b));
    CHECK(magnitude(L, 600.0, c) > 0.1 && magnitude(L, 300.0, c) < 0.01 * magnitude(L, 600.0, c));
}

// Stutter at density 0 never repeats: all wet, it is the live input exactly.
void stutterLive() {
    P p = params(Grain::kStutter);
    p.density = 0.0f;
    const Buf inL = whiteNoise(20000, 0.5f, 3), inR = whiteNoise(20000, 0.5f, 4);
    Buf L = inL, R = inR;
    render(p, L, R);
    CHECK(same(L, inL) && same(R, inR));
}

// Hold: after it, the input no longer matters. Two runs with the same input up to the hold and then
// a 700 Hz tone or silence play the same (but for the 5 ms the live input takes to leave
// Stutter), and still play.
void holdFreezes() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.density = 0.6f;
        p.spread = 0.5f;
        p.reverse = 0.3f;
        const int pre = 2756 * kCh, post = 4134 * kCh;
        Buf a = concat(sine(300.0, pre, 0.5f), sine(700.0, post, 0.5f)), b = concat(sine(300.0, pre, 0.5f), silence(post));
        Buf ar = a, br = b;
        Grain ga, gb;
        for (size_t i = 0; i < a.size(); i += kCh) {
            p.hold = i >= static_cast<size_t>(pre);
            ga.set(p, Transport{});
            ga.process(&a[i], &ar[i], kCh);
            gb.set(p, Transport{});
            gb.process(&b[i], &br[i], kCh);
        }
        const size_t from = static_cast<size_t>(pre + 512);
        const bool ok = same(Buf(a.begin() + from, a.end()), Buf(b.begin() + from, b.end())) &&
                        same(Buf(ar.begin() + from, ar.end()), Buf(br.begin() + from, br.end())) && rms(a, from) > 0.05;
        CHECK(ok);
        if (!ok) std::printf("  %s hold: differs by %g, level %.3f\n", kName[m], maxDiff(a, b, from), rms(a, from));
    }
}

// Hold pressed just after a grid line (the natural gesture: on the beat), synced Stutter at 120 BPM
// sixteenths: the slice just played can't play through (its end is the newest frame), so the held
// stutter repeats the one before it, never silence. Free 10 ms slices pitched up +12 and +24 (a
// repeat would catch up with the input) repeat too, from an earlier slice.
void stutterAlwaysRepeats() {
    const double spb = kSr * 60.0 / 120.0;
    const long line = static_cast<long>(std::ceil(40 * 0.25 * spb));   // the grid line at beat 10
    for (const float density : {0.0f, 1.0f})
        for (const int e : {0, 200, 600}) {
            P p = params(Grain::kStutter);
            p.sync = true;
            p.sizeBeats = 0.25;
            p.density = density;
            const long holdAt = (line + e + kCh - 1) / kCh * kCh;
            const int n = kCh * 8000;
            Buf L = sine(440.0, n, 0.5f), R = L;
            Grain g;
            for (int c = 0; c * kCh < n; ++c) {
                Transport t;
                t.bpm = 120.0;
                t.playing = t.valid = true;
                t.beats = c * kCh / spb;
                p.hold = c * kCh >= holdAt;
                g.set(p, t);
                g.process(&L[static_cast<size_t>(c * kCh)], &R[static_cast<size_t>(c * kCh)], kCh);
            }
            const double held = rms(L, static_cast<size_t>(holdAt + kSr / 2));
            CHECK(held > 0.3);
            if (!(held > 0.3)) std::printf("  hold %ld samples after a grid line, density %.0f: held level %.3f\n", holdAt - line, density, held);
        }
    for (const float pitch : {12.0f, 24.0f}) {
        P p = params(Grain::kStutter, 10.0f);
        p.density = 1.0f;
        p.pitch = pitch;
        const Buf in = sine(440.0, 3 * kSr, 0.5f);
        Buf L = in, R = in;
        render(p, L, R);
        int differ = 0;
        for (size_t i = 0; i < in.size(); ++i) differ += std::fabs(L[i] - in[i]) > 1e-4f;
        CHECK(differ > static_cast<int>(in.size()) / 2);
        if (!(differ > static_cast<int>(in.size()) / 2)) std::printf("  10 ms, +%.0f: %d of %zu samples repeated\n", pitch, differ, in.size());
    }
}

// MPC's transport: 128-frame blocks with the song position at each block's start, Engine::render
// adding a chunk's worth per chunk. Arp and Mosaic on 1-bar slices while MPC loops 2 beats (a loop
// shorter than a slice), and Arp on a 1-bar loop a whole number of blocks long (the wrap on a block
// edge): every wrap starts a slice, so they keep playing. Then Mosaic on sixteenths, a 1-bar loop
// wrapping mid-block on the grid, on a constant input (the output moves only in the crossfades from
// boundaries): the wrap is the boundary it already had, not a second one 64 samples later.
template <class F>
void runHost(Grain& g, const P& p, Buf& L, Buf& R, double bpm, F hostBeats) {
    for (size_t pos = 0; pos < L.size(); pos += 128) {
        Transport t;
        t.bpm = bpm;
        t.playing = t.valid = true;
        t.beats = hostBeats(static_cast<long>(pos));
        for (size_t c = pos; c < std::min(pos + 128, L.size()); c += kCh) {
            const int n = static_cast<int>(std::min<size_t>(kCh, L.size() - c));
            g.set(p, t);
            g.process(&L[c], &R[c], n);
            t.beats += n / static_cast<double>(kSr) * bpm / 60.0;
        }
    }
}
void hostLoops() {
    struct Case { int mode; double bpm, loopBeats; };
    const double aligned = 4 * 60 * kSr / (128.0 * 650.0);   // a bar of exactly 650 blocks
    for (const Case c : {Case{Grain::kArp, 125.0, 2.0}, Case{Grain::kMosaic, 125.0, 2.0}, Case{Grain::kArp, aligned, 4.0}}) {
        P p = params(c.mode);
        p.sync = true;
        p.sizeBeats = 4.0;
        p.density = 0.5f;
        const double spb = kSr * 60.0 / c.bpm;
        const long loop = std::lround(c.loopBeats * spb);
        Buf L = whiteNoise(15 * kSr, 0.3f, 21), R = L;
        Grain g;
        runHost(g, p, L, R, c.bpm, [&](long pos) { return (pos % loop) / spb; });
        const double wet = rms(L, static_cast<size_t>(8 * kSr));
        CHECK(wet > 0.05);
        if (!(wet > 0.05)) std::printf("  %s, %.0f-beat loop at %.2f BPM: level %.3f\n", kName[c.mode], c.loopBeats, c.bpm, wet);
    }
    const double bpm = 125.0, spb = kSr * 60.0 / bpm;
    const long bar = std::lround(4 * spb);   // 661.5 blocks
    P p = params(Grain::kMosaic);
    p.sync = true;
    p.sizeBeats = 0.25;
    p.spread = 1.0f;
    Buf L(static_cast<size_t>(5 * bar), 0.5f), R = L;
    Grain g;
    runHost(g, p, L, R, bpm, [&](long pos) { return (pos % bar) / spb; });
    std::vector<char> fade(L.size(), 0);   // inside a crossfade from a grid line
    for (long b = 0; b < 5; ++b)
        for (int k = 0; k < 16; ++k) {
            const long at = b * bar + static_cast<long>(std::ceil(k * 0.25 * spb - 1e-6));
            for (long i = at; i <= at + 222 && i < static_cast<long>(L.size()); ++i) fade[static_cast<size_t>(i)] = 1;
        }
    int stray = 0;
    for (size_t i = static_cast<size_t>(bar); i < L.size(); ++i) stray += std::fabs(L[i] - L[i - 1]) > 1e-6f && !fade[i - 1];
    CHECK(stray == 0);
    if (stray) std::printf("  Mosaic, a loop wrapping mid-block: the output moves %d times outside a boundary's crossfade\n", stray);
}

// Held, Mosaic's slices stay on the recording's grid: syncContent's clicks (1000 samples after
// every beat) come out 1000 samples after a grid line, wherever in its slice Hold was pressed.
void heldOnTheGrid() {
    const int s = 5292, beat = 4 * s, n = 40 * beat;
    for (const int e : {0, 1600, 3200}) {
        P p = params(Grain::kMosaic);
        p.sync = true;
        p.sizeBeats = 0.25;
        p.spread = 1.0f;
        p.density = 0.0f;
        Buf L = silence(n);
        for (int i = 1000; i < n; i += beat) L[static_cast<size_t>(i)] = 1.0f;
        Buf R = L;
        const int holdAt = (20 * beat + e) / kCh * kCh;
        Grain g;
        for (int c = 0; c * kCh < n; ++c) {
            Transport t;
            t.bpm = 125.0;
            t.playing = t.valid = true;
            t.beats = c * kCh / (kSr * 60.0 / 125.0);
            p.hold = c * kCh >= holdAt;
            g.set(p, t);
            g.process(&L[static_cast<size_t>(c * kCh)], &R[static_cast<size_t>(c * kCh)], kCh);
        }
        int count = 0, off = 0;
        for (int i = holdAt + beat; i < n; ++i)
            if (std::fabs(L[static_cast<size_t>(i)]) > 0.25f) {
                ++count;
                off += (i - 1000) % s != 0;
            }
        CHECK(count > 10 && off == 0);
        if (!(count > 10 && off == 0)) std::printf("  held %d samples into a slice: %d clicks, %d off the grid\n", holdAt - 20 * beat, count, off);
    }
}

// A mode change and Hold in the same chunk: the mode change fades every grain out from the
// chunk's start; Hold stops the recording, so a Stutter repeat reading 11.5 ms behind it is about
// to reach the newest frames and guard() fades it faster, from partway into the chunk. Blocks of
// 32, 7 and 1 play the same: the faster fade changes nothing before it starts.
void releasesAgree() {
    int checked = 0;
    for (const int at : {644, 651, 658, 665, 672, 679})
        for (const int to : {static_cast<int>(Grain::kCloud), static_cast<int>(Grain::kMosaic)}) {
            P p = params(Grain::kStutter, 11.5f);
            p.density = 1.0f;
            const Buf inL = whiteNoise(kSr, 0.5f, 31), inR = whiteNoise(kSr, 0.5f, 32);
            Buf out[3][2];
            const int sizes[3] = {32, 7, 1};
            for (int b = 0; b < 3; ++b) {
                Buf L = inL, R = inR;
                Grain g;
                for (int pos = 0; pos < kSr;) {
                    const int n = std::min(sizes[b], kSr - pos);
                    P q = p;
                    if (pos >= 7 * kCh * at / 7) {   // chunk `at`, a multiple of 7 samples too
                        q.mode = to;
                        q.hold = true;
                    }
                    g.set(q, Transport{});
                    g.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], n);
                    pos += n;
                }
                out[b][0] = L;
                out[b][1] = R;
            }
            const double d = std::max({maxDiff(out[0][0], out[1][0]), maxDiff(out[0][1], out[1][1]), maxDiff(out[0][0], out[2][0]),
                                       maxDiff(out[0][1], out[2][1])});
            checked += d < 1e-6;
            if (!(d < 1e-6)) std::printf("  Stutter to %s with Hold at chunk %d: blocks differ by %g\n", kName[to], at, d);
        }
    CHECK(checked == 12);
}

// Overlapping grains of noise sum at 1 / sqrt(3/8 overlap): all wet, Cloud's level is the
// input's to within 3 dB at every density, for a mono and a stereo input; Stretch's too.
void level() {
    for (const int m : {static_cast<int>(Grain::kCloud), static_cast<int>(Grain::kStretch)})
        for (int mono = 0; mono < 2; ++mono)
            for (const float d : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                P p = params(m);
                p.density = d;
                const Buf inL = whiteNoise(6 * kSr, 0.5f, 5), inR = mono ? inL : whiteNoise(6 * kSr, 0.5f, 6);
                Buf L = inL, R = inR;
                render(p, L, R);
                const double dl = db(rms(L, kSr) / rms(inL, kSr)), dr = db(rms(R, kSr) / rms(inR, kSr));
                const bool ok = std::fabs(dl) < 3.0 && std::fabs(dr) < 3.0;
                CHECK(ok);
                if (!ok) std::printf("  %s density %.2f mono %d: %.2f / %.2f dB\n", kName[m], d, mono, dl, dr);
            }
}

// Feedback 0.95 and loud noise for 30 s (10 s in the lighter modes): finite and bounded (the
// limiter keeps the buffer under 0 dBFS). After the input stops the texture goes on in Cloud,
// Stretch and Mosaic (Arp's stacked steps climb out of the band within a second or so, Stutter
// lets the silence in).
void feedbackBounded() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.feedback = 0.95f;
        p.density = 1.0f;
        p.spread = m == Grain::kStretch ? 0.0f : 0.5f;   // Stretch's grains alike: the loudest loop
        p.pitch = m == Grain::kCloud ? 12.0f : 0.0f;
        const int loud = (m == Grain::kCloud ? 30 : 10) * kSr, quiet = 3 * kSr;
        Buf L = concat(whiteNoise(loud, 1.0f, 7), silence(quiet)), R = concat(whiteNoise(loud, 1.0f, 8), silence(quiet));
        render(p, L, R);
        const float top = peak2(L, R);
        CHECK(allFinite(L) && allFinite(R) && top < 4.0f);
        const double after = rms(L, static_cast<size_t>(loud + 2 * kSr));
        const bool texture = m == Grain::kCloud || m == Grain::kStretch || m == Grain::kMosaic;
        if (texture) CHECK(after > 0.02);
        if (!(top < 4.0f) || (texture && !(after > 0.02)))
            std::printf("  %s feedback 0.95: peak %.2f, %.4f rms 2 s after the input stopped\n", kName[m], top, after);
    }
}

// Cloud and Stretch on a steady 440 Hz sine: no clicks, by the fourth-difference measure, with
// and without pitch, sparse and dense.
void steadyNoClicks() {
    for (const int m : {static_cast<int>(Grain::kCloud), static_cast<int>(Grain::kStretch)})
        for (const float semis : {0.0f, 12.0f, -7.0f})
            for (const float d : {0.0f, 1.0f}) {
                P p = params(m);
                p.pitch = semis;
                p.density = d;
                p.reverse = 0.5f;
                Buf L = sine(440.0, 3 * kSr, 0.5f), R = L;
                render(p, L, R);
                const double j = std::max(jolt(L, kSr, 3 * kSr), jolt(R, kSr, 3 * kSr));
                CHECK(j < 0.002);
                if (!(j < 0.002)) std::printf("  %s %+.0f density %.0f: jolt %.5f\n", kName[m], semis, d, j);
            }
}

// A 440 Hz sine through each mode, then abrupt changes at chunk edges (pitch, size, density,
// spread, reverse, feedback, mix, sync, hold, the mode): no clicks. Each change's first 0.1 s
// against the settings around it settled (the last 0.15 s of this segment or the one before,
// whichever has more: a mode change may bring older material back for a while): within 3x, or
// under 0.01 (a step of 0.3% of the level). The fourth difference grows with frequency to the
// fourth, so the pitch goes down and the feedback stays moderate: shifted up again and again by
// Arp's steps through the loop, the buffer would fill with high partials that read as jolts.
void changesNoClicks() {
    for (int m = 0; m < kModes; ++m) {
        const size_t seg = 413 * kCh;   // 0.3 s
        const int segs = 14;
        Buf L = sine(440.0, static_cast<int>(segs * seg), 0.5f), R = L;
        Grain g;
        P p = params(m);
        bool clean = true;
        for (int s = 0; s < segs; ++s) {
            switch (s) {   // the change at the start of segment s
                case 3: p.pitch = -7.0f; break;
                case 4: p.sizeMs = 300.0f; break;
                case 5: p.density = 1.0f; break;
                case 6: p.spread = 1.0f; break;
                case 7: p.reverse = 1.0f; break;
                case 8: p.feedback = 0.4f; break;
                case 9: p.hold = true; break;
                case 10: p.hold = false; break;
                case 11: p.sync = true; break;
                case 12: p.mode = (m + 1) % kModes; break;
                case 13: p.mix = 0.5f; break;
                default: break;
            }
            const size_t from = static_cast<size_t>(s) * seg;
            for (size_t i = from; i < from + seg; i += kCh) {
                g.set(p, Transport{});
                g.process(&L[i], &R[i], kCh);
            }
            if (s < 2) continue;   // the grains' own start
            for (const Buf* x : {&L, &R}) {
                const double first = jolt(*x, from, from + 4410);
                const double settled = std::max(jolt(*x, from + seg - 6615, from + seg), jolt(*x, from - 6615, from));
                const bool ok = first < std::max(0.01, 3.0 * settled);
                clean = clean && ok;
                if (!ok) std::printf("  %s, change %d: %.5f after it, %.5f settled\n", kName[m], s, first, settled);
            }
        }
        CHECK(clean && allFinite(L) && allFinite(R));
    }
}

// The extremes with loud noise, a second each: finite and bounded.
void extremes() {
    const Buf inL = whiteNoise(kSr, 1.0f, 9), inR = whiteNoise(kSr, 1.0f, 10);
    for (int m = 0; m < kModes; ++m)
        for (int bits = 0; bits < 32; ++bits) {
            P p = params(m, bits & 1 ? 1000.0f : 10.0f);
            p.pitch = bits & 2 ? 24.0f : -24.0f;
            p.density = bits & 4 ? 1.0f : 0.0f;
            p.spread = bits & 8 ? 1.0f : 0.0f;
            p.reverse = p.feedback = bits & 16 ? 0.95f : 0.0f;
            Transport t;
            if (bits & 1) {
                p.sync = true;
                p.sizeBeats = bits & 4 ? 4.0 : 1.0 / 16;
                t.bpm = bits & 4 ? 20.0 : 999.0;
                t.playing = t.valid = true;
            }
            Buf L = inL, R = inR;
            Grain g;
            for (size_t i = 0; i < L.size(); i += kCh) {
                p.hold = (bits & 16) && i > L.size() / 2;
                g.set(p, t);
                g.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kCh, L.size() - i)));
                t.beats += kCh / static_cast<double>(kSr) * t.bpm / 60.0;
            }
            const bool ok = allFinite(L) && allFinite(R) && peak2(L, R) < 8.0f;
            CHECK(ok);
            if (!ok) std::printf("  %s extremes %d: peak %.2f\n", kName[m], bits, peak2(L, R));
        }
}

// New, random (out-of-range, NaN) parameters and transport every chunk, modes and hold included:
// finite, bounded.
void randomJumps() {
    uint32_t s = 4242;
    auto r = [&s] { return 0.5f + 0.5f * ef::randBipolar(s); };
    Buf L = whiteNoise(10 * kSr, 1.0f, 11), R = whiteNoise(10 * kSr, 1.0f, 12);
    Grain g;
    for (size_t i = 0; i < L.size(); i += kCh) {
        P p;
        p.mode = static_cast<int>(r() * 7.0f) - 1;
        p.sync = r() < 0.5f;
        p.sizeMs = r() * 1200.0f - 100.0f;
        p.sizeBeats = r() * 6.0 - 1.0;
        p.density = r() * 1.4f - 0.2f;
        p.pitch = r() * 60.0f - 30.0f;
        p.reverse = r() * 1.4f - 0.2f;
        p.spread = r() * 1.4f - 0.2f;
        p.feedback = r() * 1.4f - 0.2f;
        p.hold = r() < 0.3f;
        p.mix = r() * 1.4f - 0.2f;
        if (r() < 0.02f) p.pitch = std::nanf("");
        if (r() < 0.02f) p.sizeMs = INFINITY;
        if (r() < 0.02f) p.sizeBeats = std::nan("");
        if (r() < 0.02f) p.feedback = std::nanf("");
        if (r() < 0.02f) p.density = std::nanf("");
        Transport t;
        t.playing = r() < 0.7f;
        t.valid = r() < 0.9f;
        t.bpm = r() * 1200.0;
        t.beats = r() * 1e4 - 100.0;
        if (r() < 0.02f) t.bpm = std::nan("");
        if (r() < 0.02f) t.beats = INFINITY;
        g.set(p, t);
        g.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kCh, L.size() - i)));
    }
    CHECK(allFinite(L) && allFinite(R) && peak2(L, R) < 8.0f);
}

// A NaN or an infinity in the input never reaches the buffer: the output is what a 0 there gives.
void nanInput() {
    for (int m = 0; m < kModes; ++m) {
        P p = params(m);
        p.feedback = 0.8f;
        p.mix = 0.7f;
        Buf L = whiteNoise(3 * kSr, 0.5f, 13), R = whiteNoise(3 * kSr, 0.5f, 14);
        Buf zl = L, zr = R;
        for (const int i : {1000, 30000, 60000}) {
            L[static_cast<size_t>(i)] = std::nanf("");
            R[static_cast<size_t>(i + 7)] = INFINITY;
            L[static_cast<size_t>(i + 50)] = -INFINITY;
            zl[static_cast<size_t>(i)] = zr[static_cast<size_t>(i + 7)] = zl[static_cast<size_t>(i + 50)] = 0.0f;
        }
        render(p, L, R);
        render(p, zl, zr);
        CHECK(allFinite(L) && allFinite(R) && same(L, zl) && same(R, zr));
    }
}

// Blocks of 1, 7 and 32 samples with constant settings, free, synced and stopped, synced and
// playing: the same output (grains, slices and notes start on the same samples).
void blockSizes() {
    for (int m = 0; m < kModes; ++m)
        for (int sync = 0; sync < 3; ++sync) {
            P p = params(m);
            p.sync = sync > 0;
            p.sizeBeats = 0.25;
            p.density = 0.7f;
            p.pitch = 5.0f;
            p.reverse = 0.4f;
            p.spread = 0.7f;
            p.feedback = 0.5f;
            Transport t;
            t.bpm = 128.0;
            t.playing = t.valid = sync == 2;
            t.beats = 3.37;
            const Buf inL = whiteNoise(3 * kSr, 0.5f, 15), inR = whiteNoise(3 * kSr, 0.5f, 16);
            Buf L32 = inL, R32 = inR, L7 = inL, R7 = inR, L1 = inL, R1 = inR;
            render(p, L32, R32, t, 32);
            render(p, L7, R7, t, 7);
            render(p, L1, R1, t, 1);
            const double d = std::max({maxDiff(L32, L7), maxDiff(R32, R7), maxDiff(L32, L1), maxDiff(R32, R1)});
            CHECK(d < 1e-6);
            if (!(d < 1e-6)) std::printf("  %s sync %d: blocks differ by %g\n", kName[m], sync, d);
        }
}

// reset() hides the buffer without clearing it: after loud noise, silence in is silence out in
// every mode (held or not, reaching as far back as it can, the modes changing), and an input plays
// exactly as through a new one.
void resetHides() {
    for (int m = 0; m < kModes; ++m) {
        Grain g;
        P loud = params(m);
        loud.feedback = 0.9f;
        loud.density = 1.0f;
        loud.spread = 1.0f;
        Buf NL = whiteNoise(9 * kSr, 1.0f, 17), NR = whiteNoise(9 * kSr, 1.0f, 18);
        run(g, loud, NL, NR);
        for (const bool hold : {false, true}) {
            g.reset();
            Buf L = silence(9 * kSr), R = L;
            for (size_t i = 0; i < L.size(); i += kCh) {
                P q = params((m + static_cast<int>(i / (kSr / 2))) % kModes, 300.0f);
                q.spread = 1.0f;
                q.density = 1.0f;
                q.reverse = 0.5f;
                q.pitch = (i / kSr) % 2 ? 24.0f : -12.0f;
                q.hold = hold;
                g.set(q, Transport{});
                g.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kCh, L.size() - i)));
            }
            CHECK(peak2(L, R) < 1e-6f);
        }
        g.reset();
        Grain fresh;
        P q = params(m);
        q.spread = 1.0f;
        q.reverse = 0.5f;
        Buf a = sine(330.0, 4 * kSr, 0.5f), b = whiteNoise(4 * kSr, 0.3f, 19), c = a, d = b;
        run(g, q, a, b);
        run(fresh, q, c, d);
        CHECK(same(a, c) && same(b, d));
    }
}

// tailSamples(): noise for 3 s, then silence. The output has died 60 dB under its peak by the tail
// after the input stopped; hold makes it (all but) endless.
void tail() {
    struct Case {
        int mode;
        float spread, feedback;
    };
    for (const Case c : {Case{Grain::kCloud, 0.5f, 0.0f}, Case{Grain::kCloud, 1.0f, 0.0f}, Case{Grain::kCloud, 0.3f, 0.5f},
                         Case{Grain::kStretch, 0.5f, 0.0f}, Case{Grain::kMosaic, 1.0f, 0.0f}, Case{Grain::kStutter, 0.5f, 0.0f},
                         Case{Grain::kArp, 0.5f, 0.0f}}) {
        P p = params(c.mode);
        p.spread = c.spread;
        p.feedback = c.feedback;
        p.density = 1.0f;
        p.reverse = 0.5f;
        Grain g;
        g.set(p, Transport{});
        const int est = g.tailSamples(), on = 3 * kSr;
        Buf L = concat(whiteNoise(on, 0.5f, 20), silence(est + kSr)), R = L;
        run(g, p, L, R);
        const float top = peak(L);
        int last = 0;
        for (size_t i = 0; i < L.size(); ++i)
            if (std::fabs(L[i]) > 1e-3f * top) last = static_cast<int>(i);
        CHECK(last - on <= est);
        if (!(last - on <= est)) std::printf("  %s spread %.1f fb %.1f: rings %d, tail %d\n", kName[c.mode], c.spread, c.feedback, last - on, est);
    }
    Grain g;
    P p = params(Grain::kCloud);
    p.hold = true;
    g.set(p, Transport{});
    CHECK(g.tailSamples() >= (1 << 30));
}

} // namespace

void grainTests() {
    dryAtMixZero();
    pitchShift();
    antiAliasing();
    reverseGrains();
    syncContent();
    syncBoundaries();
    arpSteps();
    mosaicOctaves();
    stretchCrawls();
    stutterLive();
    stutterAlwaysRepeats();
    hostLoops();
    heldOnTheGrid();
    holdFreezes();
    releasesAgree();
    level();
    feedbackBounded();
    steadyNoClicks();
    changesNoClicks();
    extremes();
    randomJumps();
    nanInput();
    blockSizes();
    resetHides();
    tail();
}
