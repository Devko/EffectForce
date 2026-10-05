// Modulation: the LFOs (free and locked to MPC's beat), the envelope follower, and the matrix
// moving parameters through the engine (plugin/engine.h).
#include "host.h"
#include "../plugin/engine.h"

#include <cstdio>
#include <memory>

namespace {

using namespace eft;
using namespace ef;

// The LFO's values chunk by chunk over `seconds`.
std::vector<float> trace(Lfo& lfo, const LfoParams& p, Transport t, double seconds) {
    std::vector<float> v;
    const int chunks = static_cast<int>(seconds * kRate / kChunk);
    for (int c = 0; c < chunks; ++c) {
        v.push_back(lfo.next(p, t, kChunk));
        t.beats += kChunk / static_cast<double>(kRate) * t.bpm / 60.0;
    }
    return v;
}

int upCrossings(const std::vector<float>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i) n += v[i - 1] < 0.0f && v[i] >= 0.0f;
    return n;
}

void lfos() {
    Lfo lfo;
    lfo.reset(1);
    LfoParams p;
    p.rateHz = 2.0f;
    const auto v = trace(lfo, p, Transport{}, 3.0);
    CHECK(std::abs(upCrossings(v) - 6) <= 1);   // 2 Hz for 3 s
    float lo = 1.0f, hi = -1.0f;
    for (float x : v) lo = std::min(lo, x), hi = std::max(hi, x);
    CHECK(lo < -0.99f && hi > 0.99f && lo >= -1.0001f && hi <= 1.0001f);

    // Every wave stays in -1..1; S&H holds within a cycle and changes between cycles.
    for (int w = 0; w < LW_COUNT; ++w) {
        Lfo l;
        l.reset(3);
        p.wave = w;
        p.rateHz = 5.0f;
        for (float x : trace(l, p, Transport{}, 1.0)) CHECK(x >= -1.0001f && x <= 1.0001f);
    }
    {
        Lfo l;
        l.reset(5);
        p.wave = LW_SH;
        p.rateHz = 1.0f;
        const auto s = trace(l, p, Transport{}, 2.5);
        const size_t perCycle = static_cast<size_t>(kRate / kChunk);
        CHECK(s[10] == s[perCycle - 10]);              // held through the first cycle
        CHECK(s[perCycle + 10] != s[10]);              // a new value in the second
        CHECK(s[2 * perCycle + 10] != s[perCycle + 10]);
    }

    // Smooth glides from one random value to the next at any phase: no step bigger than a glide's.
    for (float phase : {0.0f, 0.25f, 0.5f, 0.8f}) {
        Lfo s;
        s.reset(11);
        LfoParams q;
        q.wave = LW_SMOOTH;
        q.rateHz = 1.0f;
        q.phase = phase;
        const auto v = trace(s, q, Transport{}, 5.0);
        float worst = 0.0f;
        for (size_t i = 1; i < v.size(); ++i) worst = std::max(worst, std::fabs(v[i] - v[i - 1]));
        CHECK(worst < 0.01f);   // a full -1..1 glide over 1378 chunks moves at most 0.0023 per chunk
    }

    // Synced while playing: the phase is the song position's, whatever came before.
    Lfo l;
    l.reset(7);
    LfoParams q;
    q.wave = LW_SAW_UP;
    q.sync = true;
    q.divBeats = 4.0;   // a bar
    Transport t;
    t.playing = t.valid = true;
    t.bpm = 100.0;
    t.beats = 5.0;      // a quarter into the second bar
    CHECK(std::fabs(l.next(q, t, kChunk) - (2.0f * 0.25f - 1.0f)) < 1e-4f);
    t.beats = 30.0;     // a jump: half way through bar 8
    CHECK(std::fabs(l.next(q, t, kChunk) - 0.0f) < 1e-4f);
    q.phase = 0.25f;    // the phase knob shifts it against the bar
    CHECK(std::fabs(l.next(q, t, kChunk) - 0.5f) < 1e-4f);
    // Stopped, it runs on at the tempo's rate: 100 BPM, a bar = 2.4 s.
    Lfo f;
    f.reset(9);
    q.phase = 0.0f;
    q.wave = LW_SINE;
    Transport stopped;
    stopped.bpm = 100.0;
    CHECK(std::abs(upCrossings(trace(f, q, stopped, 4.8)) - 2) <= 1);
}

void follower() {
    EnvFollower env;
    env.set(0.005f, 0.1f, 0.0f);
    const Buf loud = sine(1000.0, kChunk * 200, 0.5f), quiet(kChunk * 800, 0.0f);
    float e = 0.0f;
    for (size_t pos = 0; pos < loud.size(); pos += kChunk) e = env.process(&loud[pos], &loud[pos], kChunk);
    CHECK(e > 0.4f && e <= 0.5f);   // the sine's peak, gain 0 dB
    for (size_t pos = 0; pos < quiet.size(); pos += kChunk) e = env.process(&quiet[pos], &quiet[pos], kChunk);
    CHECK(e < 0.01f);               // released: 0.58 s is almost six of its 100 ms
    env.set(0.005f, 0.1f, 24.0f);
    for (size_t pos = 0; pos < loud.size(); pos += kChunk) e = env.process(&loud[pos], &loud[pos], kChunk);
    CHECK(e == 1.0f);               // the gain pushes it to the top
}

void matrix() {
    auto eng = std::make_unique<Engine>();
    float norm[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) norm[i] = PARAM_INFO[i].def;
    const auto setOpt = [&norm](int id, const char* name) {
        for (int o = 0; o < PARAM_INFO[id].nopts; ++o)
            if (!std::strcmp(PARAM_INFO[id].opts[o], name)) norm[id] = paramNorm(id, static_cast<float>(o));
    };
    Buf L(kChunk * 4, 0.0f), R = L;
    eng->setParams(norm);
    eng->render(L.data(), R.data(), static_cast<int>(L.size()), Transport{});
    CHECK(&eng->current() == &eng->patch());   // no slots: the knobs as they are

    // Macro 1 at full, +50% on the cutoff: the cutoff knob's value moved half its range up.
    norm[P_MAC_1] = 1.0f;
    setOpt(P_M1_SRC, "Macro 1");
    setOpt(P_M1_DST, "Filter Cutoff");
    norm[P_M1_AMT] = paramNorm(P_M1_AMT, 0.5f);
    eng->setParams(norm);
    eng->render(L.data(), R.data(), static_cast<int>(L.size()), Transport{});
    const float want = paramValue(P_FLT_CUT, norm[P_FLT_CUT] + 0.5f);
    CHECK(std::fabs(eng->current().filter.cutoffHz - want) < 0.01f * want);
    CHECK(eng->patch().filter.cutoffHz == paramValue(P_FLT_CUT, norm[P_FLT_CUT]));
    // A reset (a long suspend clears every tail) leaves the macros where their knobs are.
    eng->reset();
    eng->render(L.data(), R.data(), static_cast<int>(L.size()), Transport{});
    CHECK(std::fabs(eng->current().filter.cutoffHz - want) < 0.01f * want);

    // A second slot on the same target adds up, and the sum is clamped to the knob's range.
    setOpt(P_M2_SRC, "Macro 1");
    setOpt(P_M2_DST, "Filter Cutoff");
    norm[P_M2_AMT] = paramNorm(P_M2_AMT, 0.9f);
    eng->setParams(norm);
    eng->render(L.data(), R.data(), static_cast<int>(L.size()), Transport{});
    CHECK(std::fabs(eng->current().filter.cutoffHz - 20000.0f) < 1.0f);

    // A synced delay's time: the knob's move scales the synced time.
    setOpt(P_M3_SRC, "Macro 1");
    setOpt(P_M3_DST, "Delay Time");
    norm[P_M3_AMT] = paramNorm(P_M3_AMT, 0.1f);
    setOpt(P_DLY_SYNC, "Sync");
    eng->setParams(norm);
    eng->render(L.data(), R.data(), static_cast<int>(L.size()), Transport{});
    const float ratio = paramValue(P_DLY_TIME, norm[P_DLY_TIME] + 0.1f) / paramValue(P_DLY_TIME, norm[P_DLY_TIME]);
    CHECK(std::fabs(eng->current().delay.divBeats / eng->patch().delay.divBeats - ratio) < 1e-3);

    // The envelope follows the input: a loud input opens what it modulates.
    auto e2 = std::make_unique<Engine>();
    for (int i = 0; i < P_COUNT; ++i) norm[i] = PARAM_INFO[i].def;
    setOpt(P_M1_SRC, "Envelope");
    setOpt(P_M1_DST, "Reverb Mix");
    norm[P_M1_AMT] = paramNorm(P_M1_AMT, 0.5f);
    e2->setParams(norm);
    Buf loud = sine(300.0, kChunk * 64, 0.9f), loudR = loud;
    e2->render(loud.data(), loudR.data(), static_cast<int>(loud.size()), Transport{});
    CHECK(e2->source(MS_ENV) > 0.5f);
    CHECK(e2->current().reverb.mix > e2->patch().reverb.mix + 0.2f);
}

} // namespace

void eft::modTests() {
    std::printf("== modulation\n");
    lfos();
    follower();
    matrix();
}
