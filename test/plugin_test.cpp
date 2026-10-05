// The test suite: every module measured on its own (test/<module>_test.cpp), the rack and the
// modulation directly, then the whole plugin driven through its VST2 entry points the way MPC
// drives an insert effect (128-frame blocks in place, 0..1 params, the Force's taps and turns).
// Built with ASan/UBSan by `make test`, for the Force's CPU under qemu by `make test-arm`.
#include "host.h"
#include "../plugin/surface.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

int eft::g_fail = 0, eft::g_pass = 0;
long long eft::g_msPerEvent = 1000;

namespace eft {

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt) {
    HostLog* log = e ? static_cast<HostLog*>(e->user) : nullptr;
    if (op == vst::audioMasterVersion) return 2400;
    if (!log) return 0;
    if (op == vst::audioMasterUpdateDisplay) ++log->updates;
    if (op == vst::audioMasterAutomate) {
        log->automated[index] = opt;
        ++log->automateCount[index];
    }
    if (op == vst::audioMasterGetTime) return log->noTime ? 0 : reinterpret_cast<intptr_t>(&log->time);
    return 0;
}

std::string fixtureDir() {
    static const std::string dir = [] {
        char tmpl[] = "/tmp/eftest.XXXXXX";
        const char* d = mkdtemp(tmpl);
        return std::string(d ? d : "/tmp/eftest");
    }();
    return dir;
}

namespace {

using namespace ef;

void testBasics() {
    std::printf("== the plugin as MPC sees it\n");
    Host h;
    AEffect* e = h.e;
    CHECK(e->magic == vst::kMagic && e->numInputs == 2 && e->numOutputs == 2);
    CHECK((e->flags & vst::effFlagsCanReplacing) && (e->flags & vst::effFlagsProgramChunks) && !(e->flags & vst::effFlagsIsSynth));
    CHECK(e->initialDelay == 0);   // zero latency: MPC can't compensate (docs/PROBE.md)
    CHECK(e->numParams == P_COUNT && e->uniqueID == vst::fourcc("EfFc"));
    CHECK(h.op(vst::effGetPlugCategory) == vst::kPlugCategEffect);
    char b[64] = {};
    h.op(vst::effGetEffectName, 0, 0, b);
    CHECK(std::string(b) == "EffectForce");
    CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>("plugAsChannelInsert")) == 1);
    CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>("receiveVstMidiEvent")) == -1);
    CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>("bypass")) == -1);
    VstSpeakerArrangementHead st{1, 2}, mono{0, 1};
    CHECK(h.op(vst::effSetSpeakerArrangement, 0, reinterpret_cast<intptr_t>(&st), &st) == 1);
    CHECK(h.op(vst::effSetSpeakerArrangement, 0, reinterpret_cast<intptr_t>(&mono), &st) == 0);
    // MPC sets parameter 0 when it loads a plugin: a readout ignores it. Only MOVE moves a module.
    h.setN(P_STATUS, 0.7f);
    CHECK(h.get(P_STATUS) == 0.0f);
    const float order1 = h.get(P_ORDER_1);
    h.setN(P_ORDER_1, 1.0f);
    CHECK(h.get(P_ORDER_1) == order1);
    CHECK(h.op(vst::effCanBeAutomated, P_ORDER_1) == 0 && h.op(vst::effCanBeAutomated, P_REV_MIX) == 1);
    // Names fit MPC's labels (surface.py checks the ones on the pages) and tell parameters apart.
    for (int i = 0; i < P_COUNT; ++i) CHECK(!h.name(i).empty() && h.name(i).size() <= 24);
}

void testPassThrough() {
    std::printf("== everything off: the input, bit for bit\n");
    const Buf l = whiteNoise(kBlocksPerSec * kBlock, 0.5f, 3), r = whiteNoise(kBlocksPerSec * kBlock, 0.5f, 4);
    {
        Host h;
        h.run(l, r);
        CHECK(h.L == l && h.R == r);
    }
    {
        Host h;
        h.inPlace = false;   // a host with separate buffers
        h.run(l, r, 100);
        CHECK(h.L == l && h.R == r);
    }
    {   // the legacy accumulating process() adds to what is there
        Host h;
        Buf a = l, b = r, oL(l.size(), 1.0f), oR(l.size(), 1.0f);
        float* in[2] = {a.data(), b.data()};
        float* out[2] = {oL.data(), oR.data()};
        h.e->process(h.e, in, out, static_cast<int32_t>(l.size()));
        float worst = 0.0f;
        for (size_t i = 0; i < l.size(); ++i) worst = std::max(worst, std::fabs(oL[i] - 1.0f - l[i]));
        CHECK(worst < 1e-6f);
    }
    {   // a NaN from the host never leaves the plugin (nothing on: the rest passes untouched)
        Host h;
        Buf x = l;
        x[1000] = std::nanf("");
        x[2000] = INFINITY;
        h.run(x, r);
        CHECK(h.finite && h.L[1000] == 0.0f && h.L[2000] == 0.0f && h.L[999] == l[999] && h.L[3000] == l[3000]);   // inf - inf in the mix: NaN, silenced
    }
    {   // no input buffers: silence, not a crash
        Host h;
        float oL[kBlock], oR[kBlock];
        float* out[2] = {oL, oR};
        h.e->processReplacing(h.e, nullptr, out, kBlock);
        CHECK(oL[0] == 0.0f && oR[kBlock - 1] == 0.0f);
    }
}

void testEveryModule() {
    std::printf("== each module through the plugin\n");
    const Buf in = whiteNoise(kBlocksPerSec * kBlock, 0.3f, 9);
    for (int m = 0; m < kNumModules; ++m) {
        Host h;
        h.on(kModuleOnParam[m]);
        if (m == M_EQ) h.set(P_EQ_MG, 6.0f);   // flat by default: exactly the input
        h.run(in);
        h.run(in);   // past every fade
        CHECK(h.finite);
        CHECK(h.L != in);   // it does something
        CHECK(peak(h.L) < 4.0f);
        // Off again: back to the input exactly once the fade is over.
        h.option(kModuleOnParam[m], "Off");
        h.run(in);
        h.run(in);
        CHECK(h.L == in);
    }
}

void testSuspend() {
    std::printf("== suspend and resume (Stop, the slot's ON button)\n");
    // Long echoes, then silence: the delay rings on.
    auto ringing = [](Host& h) {
        h.on(P_DLY_ON);
        h.set(P_DLY_FB, 0.9f);
        h.set(P_DLY_MIX, 1.0f);
        h.run(impulseAt(kBlock * 8, 10));
    };
    {   // Stop: suspended ~100 ms, resumed: the tail survives
        Host h;
        ringing(h);
        {
            Turn quick(50);   // suspend and resume 50 ms apart
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        h.silence(kBlocksPerSec * kBlock);
        CHECK(peak(h.L) > 0.05f);   // still echoing
    }
    {   // the ON button off for a while: everything is cleared when it comes back
        Host h;
        ringing(h);
        {
            Turn slow(2000);
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        h.silence(kBlocksPerSec * kBlock);
        CHECK(peak(h.L) < 1e-6f);   // -120 dB: the delay's denormal guard, not an echo
    }
    // The tail MPC is told: long with the delay ringing, "none" (1) with nothing on.
    Host t;
    CHECK(t.op(vst::effGetTailSize) == 1);
    ringing(t);
    CHECK(t.op(vst::effGetTailSize) > kBlocksPerSec * kBlock);
}

// The option lists the plugin shows are the modules' own.
void testLists() {
    std::printf("== option lists\n");
    for (int k = 0; k < Pulse::kPatterns; ++k)
        CHECK(std::string(PARAM_INFO[P_PLS_PATTERN].opts[k]) == Pulse::kPatternNames[k]);
    for (int k = 0; k < kNumDelayDivs; ++k) CHECK(std::string(PARAM_INFO[P_DLY_DIV].opts[k]) == kDelayDivs[k].name);
    for (int k = 0; k < kNumLfoDivs; ++k) CHECK(std::string(PARAM_INFO[P_PHS_DIV].opts[k]) == kLfoDivs[k].name);
}

void testStatus() {
    std::printf("== the status line\n");
    Host h;
    h.on(P_REV_ON);
    h.on(P_CHR_ON);
    h.silence(kBlocksPerSec * kBlock);
    const std::string s = h.display(P_STATUS);
    CHECK(s.find("2 OF 10 ON") == 0);
    CHECK(s.find("CPU") != std::string::npos);
    CHECK(h.log.updates > 0);
}

void testStress() {
    std::printf("== random settings, loud input\n");
    uint32_t seed = 7;
    auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    Host h;
    float worst = 0.0f;
    const Buf loud = whiteNoise(kBlock * 40, 1.0f, 11);
    for (int round = 0; round < 30; ++round) {
        for (int i = 0; i < P_COUNT; ++i) {
            const Kind k = PARAM_INFO[i].kind;
            if (k == Kind::Synth) h.setN(i, rnd());
        }
        h.set(P_OUT_GAIN, -12.0f);
        h.run(loud);
        worst = std::max(worst, peak(h.L));
    }
    CHECK(h.finite);
    std::printf("  peak over 30 random racks: %.2f\n", worst);
    CHECK(worst <= 8.0f);   // the output guard's ceiling, +18 dBFS
}

} // namespace
} // namespace eft

int main() {
    using namespace eft;
    const std::string root = fixtureDir();
    std::filesystem::create_directories(root + "/presets");
    std::filesystem::create_directories(root + "/data");
    setenv("EF_PRESET_ROOTS", (root + "/presets").c_str(), 1);
    setenv("EF_DATA_DIR", (root + "/data").c_str(), 1);
    setenv("EF_FIXED_SEED", "1", 1);   // every instance the same random numbers: they are compared
    // Host events a second apart unless a test says otherwise (eft::Turn): stepping never
    // mistakes two of them for one turn, whatever the machine's speed (and qemu's).
    ef::Surface::clock = [] {
        static long long t = 0;
        return t += g_msPerEvent;
    };

    std::printf("== modules\n");
    dspTests();
    driveTests();
    filterTests();
    eqTests();
    compTests();
    chorusTests();
    phaserTests();
    pulseTests();
    grainTests();
    delayTests();
    reverbTests();
    looperTests();
    rackTests();
    modTests();

    testBasics();
    testPassThrough();
    testEveryModule();
    testSuspend();
    testStatus();
    testLists();
    chainTests();
    scenesTests();
    fxTests();
    presetTests();
    testStress();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
