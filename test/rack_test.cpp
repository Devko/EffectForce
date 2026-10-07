// The rack on its own (dsp/rack.h): the order, switching modules on and off, levels and the global
// mix, with the real modules in it.
#include "signal.h"
#include "../dsp/rack.h"

#include <cstdio>
#include <memory>

namespace eft {
void rackTests();
}

namespace {

using namespace eft;
using namespace ef;

// Runs the rack over L / R in chunks, with the patch `p` (changeable between calls).
void runRack(Rack& r, const RackPatch& p, Buf& L, Buf& R) {
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), L.size() - pos));
        r.process(p, Transport{}, &L[pos], &R[pos], n);
    }
}

void orders() {
    const int good[RM_COUNT] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0}, twice[RM_COUNT] = {0, 0, 2, 3, 4, 5, 6, 7, 8, 9},
              outside[RM_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10};
    CHECK(validOrder(good) && !validOrder(twice) && !validOrder(outside));
}

void passThrough() {
    auto r = std::make_unique<Rack>();
    RackPatch p;
    const Buf in = whiteNoise(8192, 0.5f, 5);
    Buf L = in, R = in;
    runRack(*r, p, L, R);
    CHECK(L == in && R == in);   // nothing on, unity: bit for bit
    CHECK(r->running() == 0);
    // Moving modules that are off changes nothing you hear: no dip, still bit for bit.
    std::swap(p.order[2], p.order[5]);
    L = in;
    R = in;
    runRack(*r, p, L, R);
    CHECK(L == in && R == in);
    // Mix 0: the input itself, whatever runs (the dry is taken before the input gain).
    p.mix = 0.0f;
    p.inDb = 6.0f;
    p.on[RM_DRIVE] = p.on[RM_REVERB] = true;
    auto r2 = std::make_unique<Rack>();
    L = in;
    R = in;
    runRack(*r2, p, L, R);
    CHECK(L == in && R == in);
}

void levels() {
    auto r = std::make_unique<Rack>();
    RackPatch p;
    p.inDb = 6.0206f;   // x2 in, then -6.02 out: back to unity, both jumped to at the first chunk
    p.outDb = -6.0206f;
    const Buf in = sine(1000.0, 4096);
    Buf L = in, R = in;
    runRack(*r, p, L, R);
    CHECK(std::fabs(rms(L) - rms(in)) < 1e-3 * rms(in));
    p.outDb = 0.0f;   // now +6 dB, ramped across a chunk
    runRack(*r, p, L = in, R = in);
    CHECK(std::fabs(db(rms(L, 256) / rms(in, 256)) - 6.02) < 0.05);
    CHECK(maxStep(L) < 0.2f);
}

void switching() {
    // An EQ boost at the sine's own frequency switched on and off: the level moves over ~10 ms,
    // never in a jump.
    auto r = std::make_unique<Rack>();
    RackPatch p;
    p.eq.midFreq = 500.0f;
    p.eq.midGainDb = 18.0f;
    p.eq.midQ = 1.0f;
    p.eq.lowCutHz = 20.0f;
    p.eq.highCutHz = 20000.0f;
    p.eq.lowFreq = 100.0f;
    p.eq.highFreq = 6000.0f;
    const Buf in = sine(500.0, 8192, 0.1f);
    Buf L = in, R = in;
    runRack(*r, p, L, R);
    const float before = maxStep(in);
    p.on[RM_EQ] = true;
    runRack(*r, p, L = in, R = in);
    CHECK(maxStep(L) < 9.0f * before);         // +18 dB is x7.9: the steps of the boosted sine, no click on top
    CHECK(rms(L, 4096) > 6.0 * rms(in, 4096)); // on
    CHECK(r->running() == 1);
    p.on[RM_EQ] = false;
    runRack(*r, p, L = in, R = in);
    CHECK(maxStep(L) < 9.0f * before);
    CHECK(L == in || rms(L, 4096) < 1.01 * rms(in, 4096));
    runRack(*r, p, L = in, R = in);
    CHECK(L == in && r->running() == 0);   // faded out: not processed at all
}

void reenable() {
    // A delay with long feedback switched off while it rings, then on again much later: it starts
    // from cleared state, no echo of what played before.
    auto r = std::make_unique<Rack>();
    RackPatch p;
    p.on[RM_DELAY] = true;
    p.delay.mode = 0;
    p.delay.sync = false;
    p.delay.timeMs = 100.0f;
    p.delay.feedback = 0.95f;
    p.delay.lowCutHz = 20.0f;
    p.delay.highCutHz = 20000.0f;
    p.delay.mix = 1.0f;
    Buf L = impulseAt(4410, 10), R = L;
    runRack(*r, p, L, R);
    p.on[RM_DELAY] = false;
    Buf S(44100, 0.0f), T = S;
    runRack(*r, p, S, T);
    p.on[RM_DELAY] = true;
    S.assign(44100, 0.0f);
    T = S;
    runRack(*r, p, S, T);
    CHECK(peak(S) < 1e-6f);   // -120 dB: what's left is the delay's denormal guard, not an echo
}

// A module a scene switches in rests once its send has been 0 for its tail, and wakes from cleared
// state: a compressor that squashed something loud before resting starts the next push without that
// gain reduction, exactly as one that never compressed anything.
void wakesFromRest() {
    RackPatch p;
    p.on[RM_COMP] = true;
    p.comp.thresholdDb = -40.0f;
    p.comp.ratio = 20.0f;
    p.comp.releaseMs = 2000.0f;
    auto used = std::make_unique<Rack>(), fresh = std::make_unique<Rack>();
    Buf L = whiteNoise(44100, 0.9f, 7), R = L;   // loud: deep gain reduction, a 2 s release
    runRack(*used, p, L, R);
    p.send[RM_COMP] = 0.0f;   // pulled out: it rests
    for (Rack* r : {used.get(), fresh.get()}) {
        Buf S(22050, 0.0f), T = S;
        runRack(*r, p, S, T);
    }
    CHECK(used->running() == 0 && fresh->running() == 0);
    p.send[RM_COMP] = 1.0f;   // pushed in again, a quiet sine
    Buf A = sine(200.0, 22050, 0.05f), B = A, A0 = A, B0 = A;
    runRack(*used, p, A, B);
    runRack(*fresh, p, A0, B0);
    CHECK(A == A0 && B == B0);
}

void reorder() {
    // A new order dips the output over 3 ms and comes back: no click, and the level returns.
    auto r = std::make_unique<Rack>();
    RackPatch p;
    p.on[RM_DRIVE] = p.on[RM_FILTER] = true;
    p.drive.type = 2;
    p.drive.driveDb = 18.0f;
    p.drive.tone = 0.0f;
    p.drive.bias = 0.0f;
    p.drive.outDb = 0.0f;
    p.drive.mix = 1.0f;
    p.filter.type = 0;
    p.filter.cutoffHz = 800.0f;
    p.filter.res = 0.3f;
    p.filter.drive = 0.0f;
    p.filter.spread = 0.0f;
    p.filter.mix = 1.0f;
    const Buf in = sine(200.0, 8192, 0.3f);
    Buf L = in, R = in;
    runRack(*r, p, L, R);
    const float step = maxStep(L, 4096);
    std::swap(p.order[0], p.order[1]);   // Filter before Drive now
    Buf L2 = in, R2 = in;
    runRack(*r, p, L2, R2);
    CHECK(maxStep(L2) < 1.5f * step + 0.01f);
    float lowest = 1.0f;   // the dip: the envelope reaches near silence somewhere in the first 10 ms
    for (size_t i = 0; i < 441; i += 32) lowest = std::min(lowest, peak(L2, i, i + 32));
    CHECK(lowest < 0.05f);
    CHECK(rms(L2, 4096) > 0.3 * rms(L, 4096));   // and back
}

} // namespace

void eft::rackTests() {
    std::printf("== rack\n");
    orders();
    passThrough();
    levels();
    switching();
    reenable();
    wakesFromRest();
    reorder();
}
