// Looper against its design (docs/DESIGN.md, "Performance: scenes and the looper"): the input is a
// ramp that carries each sample's own index (index x 1e-6), so every output sample says which
// recorded frame it came from. Checked: untouched until it has buffers, is armed and Loop is up; the
// grab is the last whole cell of Length on the grid and plays in phase with it; Repeat and a shorter
// Length roll a slice on the grid; Speed's rate, reverse and tape stop; Hold; a grab waits until its
// cell is recorded; Length halves at slow tempos; a long loop survives the ring wrapping; a locate
// puts the head back on the grid; no clicks on a seamless loop; NaN; block sizes; reset.
#include "signal.h"
#include "../dsp/looper.h"

#include <functional>
#include <memory>

namespace {

using namespace eft;
using namespace ef;
using P = Looper::Params;

constexpr double kTag = 1e-6;   // the ramp: sample i carries i x kTag

Buf ramp(int n, int from = 0) {
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<float>((from + i) * kTag);
    return x;
}
double index(float v) { return v / kTag; }

Transport playing(double bpm, double beats = 0.0) {
    Transport t;
    t.bpm = bpm;
    t.beats = beats;
    t.playing = true;
    t.valid = true;
    return t;
}

// Runs the looper over L / R in chunks, the parameters from `at(sample)` before each chunk; the
// transport advances while it plays.
void drive(Looper& lp, Buf& L, Buf& R, Transport t, const std::function<P(int)>& at, int chunk = kChunk) {
    for (size_t pos = 0; pos < L.size(); pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), L.size() - pos));
        lp.set(at(static_cast<int>(pos)), t);
        lp.process(&L[pos], &R[pos], n);
        if (t.playing) t.beats += n / static_cast<double>(kRate) * t.bpm / 60.0;
    }
}

std::unique_ptr<Looper> armed() {
    auto lp = std::make_unique<Looper>();
    lp->allocate();
    return lp;
}

P params(float loop, double lengthBeats = 4.0, double repeat = 0.0, float speed = 1.0f, bool hold = false) {
    P p;
    p.on = true;
    p.loop = loop;
    p.lengthBeats = lengthBeats;
    p.repeatBeats = repeat;
    p.speed = speed;
    p.hold = hold;
    return p;
}

bool same(const Buf& a, const Buf& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i])) return false;
    return true;
}

// --- tests ------------------------------------------------------------------------------------

void untouchedUntilUsed() {
    std::printf("== looper: untouched without buffers, unarmed or at Loop 0\n");
    const Buf in = whiteNoise(60000, 0.5f, 3);
    {   // no buffers: even armed with Loop up, the input passes bit for bit
        Looper lp;
        Buf L = in, R = in;
        drive(lp, L, R, playing(120.0), [](int) { return params(1.0f); });
        CHECK(same(L, in) && same(R, in) && !lp.engaged() && !lp.allocated());
    }
    {   // buffers, armed, Loop 0: records, plays nothing
        auto lp = armed();
        Buf L = in, R = in;
        drive(*lp, L, R, playing(120.0), [](int) { return params(0.0f); });
        CHECK(same(L, in) && same(R, in) && !lp->engaged() && lp->allocated());
    }
    {   // not armed: Loop up does nothing
        auto lp = armed();
        Buf L = in, R = in;
        drive(*lp, L, R, playing(120.0), [](int) {
            P p = params(1.0f);
            p.on = false;
            return p;
        });
        CHECK(same(L, in) && same(R, in) && !lp->engaged());
    }
}

void grabsTheLastCell() {
    std::printf("== looper: the grab is the last whole cell, in phase with the grid\n");
    // 120 BPM: a beat is 22050 frames, a bar 88200. Loop up at beat 9.5: the cell is the last whole
    // one of its length (1 bar: bar 1, frames 88200..176400), and at sample i it plays its frame
    // i mod length.
    for (double length : {4.0, 1.0, 8.0}) {
        auto lp = armed();
        const int e = static_cast<int>(9.5 * 22050) / kChunk * kChunk;   // on a chunk's edge, as the rack calls it
        const int n = 400000;
        Buf L = ramp(n), R = ramp(n);
        drive(*lp, L, R, playing(120.0), [&](int s) { return params(s >= e ? 1.0f : 0.0f, length); });
        const double cell = length * 22050.0;
        const double eBeats = e / 22050.0;
        const double start = (std::floor(eBeats / length) - 1.0) * cell;   // the cell's first frame
        CHECK(lp->engaged() && lp->loopFrames() == static_cast<int>(cell));
        double worst = 0.0;
        for (int i = e + 2000; i < n; i += 37) {
            const double want = start + std::fmod(i, cell);
            double d = std::fabs(index(L[static_cast<size_t>(i)]) - want);
            // across the wrap's 5 ms crossfade the two heads mix: skip it
            const double into = std::fmod(i, cell);
            if (into < Looper::kFade + 2) continue;
            worst = std::max(worst, d);
        }
        std::printf("  length %.0f beats: worst frame error %.3f\n", length, worst);
        CHECK(worst < 0.5);
        // Until Loop went up, the input passed untouched.
        bool untouched = true;
        for (int i = 0; i < e; ++i) untouched = untouched && L[static_cast<size_t>(i)] == static_cast<float>(i * kTag);
        CHECK(untouched);
    }
}

void liveLoopMix() {
    std::printf("== looper: Loop crossfades live and loop\n");
    auto lp = armed();
    const int n = 200000, e = 100000 / kChunk * kChunk;
    Buf L = ramp(n), R = ramp(n);
    drive(*lp, L, R, playing(120.0), [&](int s) { return params(s >= e ? 0.25f : 0.0f); });
    // At 25%: 0.75 live + 0.25 loop (e is in bar 1, so the loop is bar 0: frame i mod 88200).
    double worst = 0.0;
    for (int i = e + 2000; i < n; i += 101) {
        if (std::fmod(i, 88200.0) < Looper::kFade + 2) continue;
        const double want = 0.75 * i + 0.25 * std::fmod(i, 88200.0);
        worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - want));
    }
    std::printf("  worst error %.3f frames\n", worst);
    CHECK(worst < 0.5);
}

void letsGoAndGrabsAgain() {
    std::printf("== looper: back at 0 it lets go; up again grabs a new cell, or (Hold) the same\n");
    for (bool hold : {false, true}) {
        auto lp = armed();
        const int n = 600000;
        const int on1 = 100000 / kChunk * kChunk, off1 = 200000 / kChunk * kChunk, on2 = 400000 / kChunk * kChunk;
        Buf L = ramp(n), R = ramp(n);
        drive(*lp, L, R, playing(120.0), [&](int s) {
            const bool up = (s >= on1 && s < off1) || s >= on2;
            return params(up ? 1.0f : 0.0f, 4.0, 0.0, 1.0f, hold);
        });
        // Between the two: live again (after the 3 ms fade).
        bool live = true;
        for (int i = off1 + 2000; i < on2; i += 97) live = live && std::fabs(index(L[static_cast<size_t>(i)]) - i) < 0.5;
        CHECK(live);
        // The second time: bar 4 (frames 264600..352800, the last whole bar before on2) or, held, bar 1 again.
        const double start = hold ? 0.0 : 264600.0;
        double worst = 0.0;
        for (int i = on2 + 2000; i < n; i += 97) {
            if (std::fmod(i, 88200.0) < Looper::kFade + 2) continue;
            worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - (start + std::fmod(i, 88200.0))));
        }
        std::printf("  hold %d: worst %.3f\n", hold, worst);
        CHECK(worst < 0.5);
        // Let go once more: Hold keeps it (LOOP KEPT on the line); without Hold, Loop grabs anew (LISTENING).
        Buf X = ramp(20000, n), Y = X;
        drive(*lp, X, Y, playing(120.0, n / 22050.0), [&](int) { return params(0.0f, 4.0, 0.0, 1.0f, hold); });
        CHECK(lp->status().state == (hold ? Looper::kKept : Looper::kListening));
    }
}

void waitsForItsCell() {
    std::printf("== looper: just armed, the grab waits until a whole cell is recorded\n");
    auto lp = armed();
    const int n = 300000;
    Buf L = ramp(n), R = ramp(n);
    // Song at beat 2 when armed with Loop up: bar 0's cell (beats 0..4) was never recorded whole; the
    // first whole cell is bar 1 (beats 4..8, recorded by beat 8: frame 132300 from the start here).
    drive(*lp, L, R, playing(120.0, 2.0), [](int) { return params(1.0f); });
    const int ready = 6 * 22050;   // beat 8 is 6 beats after the start
    bool live = true;
    for (int i = 0; i < ready - kChunk; ++i) live = live && L[static_cast<size_t>(i)] == static_cast<float>(i * kTag);
    CHECK(live);
    // Then it plays that bar (frames 44100..132300 of the input) on the grid.
    double worst = 0.0;
    for (int i = ready + 2000; i < n; i += 61) {
        const double beat = 2.0 + i / 22050.0;
        const double into = std::fmod(beat, 4.0) * 22050.0;
        if (into < Looper::kFade + 2) continue;
        worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - (44100.0 + into)));
    }
    std::printf("  worst %.3f\n", worst);
    CHECK(worst < 0.5);
}

void repeatRolls() {
    std::printf("== looper: Repeat and a shorter Length roll a slice on the grid\n");
    // Engaged at beat 5.5 with a 1-bar loop, then Repeat 1/4 (one beat) from beat 6.25: the slice is
    // the beat the head is in (loop frames 44100..66150, beat 2 of the bar), on the grid.
    for (bool viaLength : {false, true}) {
        auto lp = armed();
        const int n = 260000;
        const int e = (5 * 22050 + 11025) / kChunk * kChunk, r = (6 * 22050 + 5512) / kChunk * kChunk;
        Buf L = ramp(n), R = ramp(n);
        drive(*lp, L, R, playing(120.0), [&](int s) {
            if (s < r) return params(s >= e ? 1.0f : 0.0f, 4.0);
            return viaLength ? params(1.0f, 1.0) : params(1.0f, 4.0, 1.0);
        });
        CHECK(std::fabs(lp->sliceFrames() - 22050.0) < 1e-6 && std::fabs(lp->sliceStart() - 44100.0) < 1e-6);
        double worst = 0.0;
        for (int i = r + Looper::kFade + 2; i < n; i += 53) {
            const double into = std::fmod(i, 22050.0);
            if (into < Looper::kFade + 2) continue;
            worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - (44100.0 + into)));
        }
        std::printf("  %s: worst %.3f\n", viaLength ? "Length 1/4" : "Repeat 1/4", worst);
        CHECK(worst < 0.5);
    }
    // Rolling in: 1/4 -> 1/8 -> 1/16, each inside the one before.
    auto lp = armed();
    const int n = 300000, e = 110016;
    Buf L = ramp(n), R = ramp(n);
    double lastStart = -1.0, lastLen = 1e9;
    bool nested = true;
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        const int s = static_cast<int>(pos);
        const double rep = s < 150000 ? 0.0 : s < 180000 ? 1.0 : s < 200000 ? 0.5 : 0.25;
        Transport t = playing(120.0, s / 22050.0);
        lp->set(params(s >= e ? 1.0f : 0.0f, 4.0, rep), t);
        lp->process(&L[pos], &R[pos], kChunk);
        if (rep > 0.0 && (lp->sliceStart() != lastStart || lp->sliceFrames() != lastLen)) {
            nested = nested && (lastLen > 1e8 || (lp->sliceStart() >= lastStart && lp->sliceStart() + lp->sliceFrames() <= lastStart + lastLen + 1e-6));
            lastStart = lp->sliceStart();
            lastLen = lp->sliceFrames();
        }
    }
    CHECK(nested && std::fabs(lastLen - 5512.5) < 1e-6);
}

void speedRates() {
    std::printf("== looper: Speed plays at its rate, backwards below 0, and fades out towards 0\n");
    for (float speed : {0.5f, 2.0f, -1.0f}) {
        auto lp = armed();
        const int n = 250000, e = 100000 / kChunk * kChunk;
        Buf L = ramp(n), R = ramp(n);
        drive(*lp, L, R, playing(120.0), [&](int s) { return params(s >= e ? 1.0f : 0.0f, 4.0, 0.0, speed); });
        // Between wraps the source index moves by `speed` per sample.
        int ok = 0, total = 0;
        for (int i = e + 3000; i + 1 < n; i += 89) {
            const double d = index(L[static_cast<size_t>(i + 1)]) - index(L[static_cast<size_t>(i)]);
            if (std::fabs(d) > 1000.0) continue;   // a wrap
            ++total;
            ok += std::fabs(d - speed) < 0.05;
        }
        std::printf("  speed %+.1f: %d of %d steps at the rate\n", speed, ok, total);
        CHECK(total > 100 && ok >= total * 0.97);   // a few land inside a wrap's crossfade
    }
    // Stopped: silence (no held DC).
    auto lp = armed();
    const int n = 200000, e = 100000 / kChunk * kChunk;
    Buf L = whiteNoise(n, 0.5f, 9), R = whiteNoise(n, 0.5f, 10);
    drive(*lp, L, R, playing(120.0), [&](int s) { return params(s >= e ? 1.0f : 0.0f, 4.0, 0.0, 0.0f); });
    CHECK(peak(L, static_cast<size_t>(e + 4000)) < 1e-6f && peak(R, static_cast<size_t>(e + 4000)) < 1e-6f);
    // A slow-down to 0 over a second: the level falls with the speed, nothing jumps.
    auto lp2 = armed();
    Buf L2 = sine(441.0, n, 0.5f), R2 = L2;
    drive(*lp2, L2, R2, playing(120.0), [&](int s) {
        const float sp = s < e ? 1.0f : std::max(0.0f, 1.0f - (s - e) / 44100.0f);
        return params(s >= e ? 1.0f : 0.0f, 4.0, 0.0, sp);
    });
    CHECK(allFinite(L2) && maxStep(L2, static_cast<size_t>(e)) < 0.05f && peak(L2, static_cast<size_t>(e + 44100 + 1000)) < 1e-6f);
}

void seamless() {
    std::printf("== looper: a loop of whole cycles plays on without a click\n");
    // 441 Hz at 120 BPM: a bar is 882 whole cycles, so the loop is the sine itself.
    auto lp = armed();
    const int n = 400000, e = 120000 / kChunk * kChunk;
    Buf L = sine(441.0, n, 0.5f), R = sine(441.0, n, 0.5f, 1.0);
    const Buf in = L;
    drive(*lp, L, R, playing(120.0), [&](int s) { return params(s >= e ? 1.0f : 0.0f); });
    double worst = 0.0;
    for (int i = 0; i < n; ++i) worst = std::max(worst, static_cast<double>(std::fabs(L[static_cast<size_t>(i)] - in[static_cast<size_t>(i)])));
    std::printf("  worst difference from the input %.2e, max step %.4f (the sine's own %.4f)\n", worst, maxStep(L), maxStep(in));
    CHECK(worst < 1e-3 && maxStep(L) < maxStep(in) * 1.05f);
    // A loop that isn't seamless (noise) still steps no more at the wrap than inside.
    auto lp2 = armed();
    Buf N = sine(310.0, n, 0.5f), M = N;   // 77.5 cycles in the half-beat loop: a seam to cross
    drive(*lp2, N, M, playing(120.0), [&](int s) { return params(s >= e ? 1.0f : 0.0f, 0.5); });
    CHECK(maxStep(N, static_cast<size_t>(e + 2000)) < 0.05f);
}

void slowTempoHalves() {
    std::printf("== looper: a Length over 20 s halves until it fits\n");
    auto lp = armed();
    const int n = 1600000, e = 1500000 / kChunk * kChunk;
    Buf L = ramp(n), R = ramp(n);
    drive(*lp, L, R, playing(60.0), [&](int s) { return params(s >= e ? 1.0f : 0.0f, 32.0); });
    std::printf("  8 bars at 60 BPM: %d frames (%.1f beats)\n", lp->loopFrames(), lp->loopBeats());
    CHECK(lp->loopFrames() == 705600 && lp->loopBeats() == 16.0);
    // 96 BPM: 8 bars is exactly 20 s and stays; 120 BPM: 8 bars (16 s) too.
    for (double bpm : {96.0, 120.0}) {
        auto l8 = armed();
        Buf A = ramp(n), B = ramp(n);
        drive(*l8, A, B, playing(bpm), [&](int s) { return params(s >= e ? 1.0f : 0.0f, 32.0); });
        CHECK(l8->loopFrames() == static_cast<int>(32.0 * 44100.0 * 60.0 / bpm) && l8->loopBeats() == 32.0);
    }
}

void longLoopSurvivesTheRing() {
    std::printf("== looper: a 20 s loop plays right while the ring wraps underneath\n");
    auto lp = armed();
    // 96 BPM, 8 bars = 882000 frames. Engaged at 45 s, played for 60 s: the ring (2097152 frames)
    // wraps more than twice meanwhile.
    const int n = 105 * 44100, e = 45 * 44100 / kChunk * kChunk;
    Buf L = ramp(n), R = ramp(n);
    drive(*lp, L, R, playing(96.0), [&](int s) { return params(s >= e ? 1.0f : 0.0f, 32.0); });
    const double spb = 44100.0 * 60.0 / 96.0, cell = 32.0 * spb;
    const double start = (std::floor(e / cell) - 1.0) * cell;
    double worst = 0.0;
    for (int i = e + 2000; i < n; i += 211) {
        const double into = std::fmod(i, cell);
        if (into < Looper::kFade + 2) continue;
        worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - (start + into)));
    }
    std::printf("  worst %.3f frames, copy done %d\n", worst, lp->copyDone());
    CHECK(worst < 1.0 && lp->copyDone());   // the ramp's float resolution at index 1.8e6 is ~0.12
}

// REC (Octatrack style): the parameters before each chunk from `at`, REC pressed at the samples in
// `presses` (each bumps the count).
P recParams(float loop, double length, int capture, uint32_t rec, bool hold = true) {
    P p = params(loop, length, 0.0, 1.0f, hold);
    p.capture = capture;
    p.rec = rec;
    return p;
}

void recLast() {
    std::printf("== looper: REC Last keeps the cell just played; Loop plays it later\n");
    auto lp = armed();
    const int n = 500000;
    const int press = static_cast<int>(9.5 * 22050) / kChunk * kChunk, up = 400000 / kChunk * kChunk;
    Buf L = ramp(n), R = ramp(n);
    drive(*lp, L, R, playing(120.0), [&](int s) {
        return recParams(s >= up ? 1.0f : 0.0f, 4.0, Looper::kLast, s >= press ? 1u : 0u);
    });
    // Kept at the press: bar 1 (frames 88200..176400); live until Loop goes up, then that bar on the grid.
    bool live = true;
    for (int i = 0; i < up; i += 13) live = live && L[static_cast<size_t>(i)] == static_cast<float>(i * kTag);
    CHECK(live && lp->hasLoop());
    double worst = 0.0;
    for (int i = up + 2000; i < n; i += 53) {
        const double into = std::fmod(i, 88200.0);
        if (into < Looper::kFade + 2) continue;
        worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - (88200.0 + into)));
    }
    std::printf("  worst %.3f\n", worst);
    CHECK(worst < 0.5);
}

void recNext() {
    std::printf("== looper: REC Next records the coming cell on the grid (a late press takes the one begun)\n");
    // 1 bar. Pressed at beat 5.5: the next line is beat 8, the cell beats 8..12, ready at 12.
    // Pressed at beat 8.5 (half a beat late): the cell begun at 8 still.
    for (double pressBeat : {5.5, 8.5}) {
        auto lp = armed();
        const int n = 600000;
        const int press = static_cast<int>(pressBeat * 22050) / kChunk * kChunk, up = 450000 / kChunk * kChunk;
        Buf L = ramp(n), R = ramp(n);
        int waiting = 0, recording = 0, sawReady = -1;
        for (size_t pos = 0; pos < L.size(); pos += kChunk) {
            const int s = static_cast<int>(pos);
            lp->set(recParams(s >= up ? 1.0f : 0.0f, 4.0, Looper::kNext, s >= press ? 1u : 0u), playing(120.0, s / 22050.0));
            lp->process(&L[pos], &R[pos], kChunk);
            const Looper::Status st = lp->status();
            waiting += st.capture == 1;
            recording += st.capture == 2;
            if (sawReady < 0 && st.state == Looper::kKept) sawReady = s;
        }
        std::printf("  pressed at beat %.1f: waited %d chunks, recorded %d, kept at sample %d\n", pressBeat, waiting,
                    recording, sawReady);
        CHECK(sawReady >= 12 * 22050 && sawReady < 12 * 22050 + 2 * kChunk);   // ready as beat 12 ends the cell
        CHECK(pressBeat < 8.0 ? waiting > 0 : waiting == 0);
        CHECK(recording > 0);
        double worst = 0.0;
        for (int i = up + 2000; i < n; i += 53) {
            const double into = std::fmod(i, 88200.0);
            if (into < Looper::kFade + 2) continue;
            worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - (176400.0 + into)));
        }
        std::printf("  plays frames 176400.. of the cell: worst %.3f\n", worst);
        CHECK(worst < 0.5);
    }
}

void recCancelAndUnarmed() {
    std::printf("== looper: REC twice cancels; REC unarmed does nothing\n");
    auto lp = armed();
    const int n = 400000, p1 = 120000 / kChunk * kChunk, p2 = 130000 / kChunk * kChunk;
    Buf L = ramp(n), R = ramp(n);
    drive(*lp, L, R, playing(120.0), [&](int s) {
        return recParams(0.0f, 4.0, Looper::kNext, s >= p2 ? 2u : s >= p1 ? 1u : 0u);
    });
    CHECK(!lp->hasLoop() && !lp->capturing());
    auto un = armed();
    Buf A = ramp(n), B = ramp(n);
    drive(*un, A, B, playing(120.0), [&](int s) {
        P p = recParams(0.0f, 4.0, Looper::kLast, s >= p1 ? 1u : 0u);
        p.on = false;
        return p;
    });
    CHECK(!un->hasLoop());
}

void recReplacesWhilePlaying() {
    std::printf("== looper: a capture while a loop plays replaces it, crossfaded, on the grid\n");
    auto lp = armed();
    // A 1-bar loop grabbed at beat 5.5 (bar 0), playing; REC Next pressed at beat 9.5 (past the beat
    // a late press may be: the cell 12..16), ready at beat 16: from there the loop is bar 3 (frames
    // 264600..352800).
    const int n = 520000, up = static_cast<int>(5.5 * 22050) / kChunk * kChunk;
    const int press = static_cast<int>(9.5 * 22050) / kChunk * kChunk;
    Buf L = sine(441.0, n, 0.5f), R = L;
    Buf T = ramp(n), U = ramp(n);
    auto at = [&](int s) { return recParams(s >= up ? 1.0f : 0.0f, 4.0, Looper::kNext, s >= press ? 1u : 0u, false); };
    drive(*lp, T, U, playing(120.0), at);
    double worst = 0.0;
    for (int i = 16 * 22050 + 2000; i < n; i += 37) {
        const double into = std::fmod(i, 88200.0);
        if (into < Looper::kFade + 2) continue;
        worst = std::max(worst, std::fabs(index(T[static_cast<size_t>(i)]) - (264600.0 + into)));
    }
    std::printf("  after the switch: worst %.3f\n", worst);
    CHECK(worst < 0.5);
    // A sine of whole cycles per bar: the switch is seamless, no click.
    auto ls = armed();
    drive(*ls, L, R, playing(120.0), at);
    CHECK(maxStep(L, static_cast<size_t>(up + 2000)) < 0.0315f * 1.05f);
}

// One jump at a time: a capture landing on the line where the playing loop wraps (a loop of the
// same length, in phase with the grid) waits for the wrap's crossfade; a Repeat change near a slice's
// end waits too. A sine that doesn't fit the bar has a seam at every wrap: a crossfade dropped halfway
// shows as a step, one played out stays as smooth as the seamless loop's.
void oneJumpAtATime() {
    std::printf("== looper: a capture on the playing loop's wrap, a roll near a slice's end: no click\n");
    // 310.3 Hz: 620.6 cycles a bar, so every bar starts at another phase (and every wrap is a seam).
    const int n = 400000, up = static_cast<int>(5.5 * 22050) / kChunk * kChunk;
    {
        // The bar loop wraps on the line at beat 12; REC Last a chunk or two after it, in its crossfade.
        auto lp = armed();
        const int press = (12 * 22050 + 40) / kChunk * kChunk;
        Buf L = sine(310.3, n, 0.5f), R = L;
        drive(*lp, L, R, playing(120.0), [&](int s) {
            return recParams(s >= up ? 1.0f : 0.0f, 4.0, Looper::kLast, s >= press ? 1u : 0u);
        });
        const float step = maxStep(L, static_cast<size_t>(up + 2000));
        std::printf("  REC Last in the wrap's crossfade: largest step %.4f\n", step);
        CHECK(step < 0.05f);
    }
    {
        // Repeat 1/16 from beat 6; at beat 9 (a slice's line, its wrap's crossfade under way) 1/8.
        auto lp = armed();
        const int change = 9 * 22050 / kChunk * kChunk + kChunk;
        Buf L = sine(310.3, n, 0.5f), R = L;
        drive(*lp, L, R, playing(120.0), [&](int s) {
            return params(s >= up ? 1.0f : 0.0f, 4.0, s < 6 * 22050 ? 0.0 : s < change ? 0.25 : 0.5);
        });
        const float step = maxStep(L, static_cast<size_t>(up + 2000));
        std::printf("  a roll changed in its wrap's crossfade: largest step %.4f\n", step);
        CHECK(step < 0.05f);
    }
}

// Blend switched while the loop plays at 100%: the live input glides in (Layer) and out (Swap).
void blendGlides() {
    std::printf("== looper: switching Blend glides the live input in and out\n");
    auto lp = armed();
    const int n = 300000, e = 110016, flip = 200000 / kChunk * kChunk;
    Buf L = sine(441.0, n, 0.5f), R = L;   // whole cycles per bar: the loop is the live sine itself
    drive(*lp, L, R, playing(120.0), [&](int s) {
        P p = params(s >= e ? 1.0f : 0.0f);
        p.layer = s >= flip && s < flip + 30000;
        return p;
    });
    const float step = maxStep(L, static_cast<size_t>(e + 2000));
    std::printf("  largest step %.4f (the sine's 0.0314, doubled while layered)\n", step);
    CHECK(step < 0.07f);
}

void layerKeepsLive() {
    std::printf("== looper: Layer keeps the live input under the loop\n");
    auto lp = armed();
    const int n = 300000, e = 110016;
    Buf L = ramp(n), R = ramp(n);
    drive(*lp, L, R, playing(120.0), [&](int s) {
        P p = params(s >= e ? 1.0f : 0.0f);
        p.layer = true;
        return p;
    });
    double worst = 0.0;
    for (int i = e + 2000; i < n; i += 61) {
        if (std::fmod(i, 88200.0) < Looper::kFade + 2) continue;
        worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - (i + std::fmod(i, 88200.0))));
    }
    std::printf("  live + loop: worst %.3f\n", worst);
    CHECK(worst < 0.5);
}

void recSurvivesASequenceLoop() {
    std::printf("== looper: a sequence looping back while REC waits doesn't lose the capture\n");
    auto lp = armed();
    // The sequence is 2 bars (8 beats) long and loops; REC Next for 1 bar pressed at beat 6.5: the
    // next line is beat 8, which the sequence reaches as beat 0 again. The capture counts samples.
    const int n = 400000, press = static_cast<int>(6.5 * 22050) / kChunk * kChunk;
    Buf L = ramp(n), R = ramp(n);
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        const int s = static_cast<int>(pos);
        lp->set(recParams(0.0f, 4.0, Looper::kNext, s >= press ? 1u : 0u), playing(120.0, std::fmod(s / 22050.0, 8.0)));
        lp->process(&L[pos], &R[pos], kChunk);
    }
    CHECK(lp->hasLoop() && lp->loopFrames() == 88200);
}

void locateBackOnTheGrid() {
    std::printf("== looper: after a locate the head crossfades back onto the grid\n");
    auto lp = armed();
    const int n = 300000, e = 100000 / kChunk * kChunk, jump = 180000 / kChunk * kChunk;
    Buf L = ramp(n), R = ramp(n);
    Transport t = playing(120.0);
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        if (static_cast<int>(pos) == jump) t.beats -= 1.3;   // MPC locates 1.3 beats back
        lp->set(params(static_cast<int>(pos) >= e ? 1.0f : 0.0f), t);
        lp->process(&L[pos], &R[pos], kChunk);
        t.beats += kChunk / static_cast<double>(kRate) * 2.0;
    }
    // After the jump the loop plays frame (beat mod 4) of bar 1 (frames 0..88200).
    double worst = 0.0;
    for (int i = jump + 1000; i < n; i += 59) {
        const double beat = i / 22050.0 - 1.3;
        const double into = std::fmod(beat, 4.0) * 22050.0;
        if (into < Looper::kFade + 2 || into > 88200.0 - 2) continue;
        worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - into));
    }
    std::printf("  worst %.3f\n", worst);
    CHECK(worst < 0.5);
}

void stoppedRunsOn() {
    std::printf("== looper: stopped, the grid runs on at the tempo\n");
    auto lp = armed();
    const int n = 300000, e = 110000 / kChunk * kChunk;
    Buf L = ramp(n), R = ramp(n);
    Transport t;
    t.bpm = 120.0;   // valid but not playing: MPC stopped
    t.valid = true;
    drive(*lp, L, R, t, [&](int s) { return params(s >= e ? 1.0f : 0.0f); });
    const double start = (std::floor(e / 88200.0) - 1.0) * 88200.0;
    double worst = 0.0;
    for (int i = e + 2000; i < n; i += 71) {
        const double into = std::fmod(i, 88200.0);
        if (into < Looper::kFade + 2) continue;
        worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - (start + into)));
    }
    CHECK(worst < 0.5);
}

void nanAndExtremes() {
    std::printf("== looper: NaN in the recording, extreme settings\n");
    auto lp = armed();
    const int n = 300000, e = 200000 / kChunk * kChunk;
    Buf L = whiteNoise(n, 0.5f, 21), R = whiteNoise(n, 0.5f, 22);
    for (int i = 1000; i < 90000; i += 777) L[static_cast<size_t>(i)] = std::nanf("");
    R[5000] = INFINITY;
    drive(*lp, L, R, playing(133.0), [&](int s) {
        P p = params(s >= e ? 1.0f : 0.0f, 16.0, 0.125, (s / 3001) % 2 ? 2.0f : -1.0f);
        return p;
    });
    CHECK(allFinite(Buf(L.begin() + e + 1000, L.end())) && allFinite(Buf(R.begin() + e + 1000, R.end())));
    // NaN parameters and a NaN tempo.
    auto lp2 = armed();
    Buf A = whiteNoise(50000, 0.5f, 23), B = A;
    Transport t = playing(120.0);
    for (size_t pos = 0; pos < A.size(); pos += kChunk) {
        P p = params(pos > 20000 ? std::nanf("") : 1.0f, std::nan(""), std::nan(""), std::nanf(""));
        if (pos > 30000) t.bpm = std::nan("");
        lp2->set(p, t);
        lp2->process(&A[pos], &B[pos], static_cast<int>(std::min<size_t>(kChunk, A.size() - pos)));
    }
    CHECK(allFinite(A) && allFinite(B));
}

void blockSizes() {
    std::printf("== looper: chunk sizes 1..32 play the same\n");
    // The changes at multiples of every chunk size tried (224 = 7 x 32), so they land on the same samples.
    const int n = 200000, e = 446 * 224, rep = 670 * 224;
    Buf ref = ramp(n), refR = ramp(n);
    {
        auto lp = armed();
        drive(*lp, ref, refR, playing(120.0), [&](int s) { return params(s >= e ? 1.0f : 0.0f, 4.0, s >= rep ? 0.5 : 0.0); });
    }
    for (int chunk : {1, 7, 16}) {
        auto lp = armed();
        Buf L = ramp(n), R = ramp(n);
        drive(*lp, L, R, playing(120.0), [&](int s) { return params(s >= e ? 1.0f : 0.0f, 4.0, s >= rep ? 0.5 : 0.0); }, chunk);
        double worst = 0.0;
        for (int i = 0; i < n; ++i) worst = std::max(worst, std::fabs(index(L[static_cast<size_t>(i)]) - index(ref[static_cast<size_t>(i)])));
        std::printf("  chunk %d: worst %.4f frames\n", chunk, worst);
        CHECK(worst < 0.5);
    }
}

void resetForgets() {
    std::printf("== looper: reset forgets the recording and the loop\n");
    auto lp = armed();
    const int n = 200000;
    Buf L = ramp(n), R = ramp(n);
    drive(*lp, L, R, playing(120.0), [&](int s) { return params(s >= 100000 ? 1.0f : 0.0f); });
    CHECK(lp->engaged() && lp->hasLoop());
    lp->reset();
    CHECK(!lp->engaged() && !lp->hasLoop());
    // Loop up straight after: nothing recorded since, so nothing to play yet.
    Buf A = ramp(20000), B = ramp(20000);
    const Buf in = A;
    drive(*lp, A, B, playing(120.0, 2.0), [](int) { return params(1.0f, 4.0, 0.0, 1.0f, true); });
    CHECK(same(A, in) && !lp->engaged());
}

} // namespace

void looperTests() {
    untouchedUntilUsed();
    grabsTheLastCell();
    liveLoopMix();
    letsGoAndGrabsAgain();
    waitsForItsCell();
    repeatRolls();
    speedRates();
    seamless();
    slowTempoHalves();
    longLoopSurvivesTheRing();
    recLast();
    recNext();
    recCancelAndUnarmed();
    recReplacesWhilePlaying();
    oneJumpAtATime();
    blendGlides();
    layerKeepsLive();
    recSurvivesASequenceLoop();
    locateBackOnTheGrid();
    stoppedRunsOn();
    nanAndExtremes();
    blockSizes();
    resetForgets();
}
