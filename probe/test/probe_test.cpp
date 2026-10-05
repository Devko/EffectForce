// The probe through its VST2 entry points: what MPC sees (an effect, 2 in / 2 out), the delay's
// timing, the edge cases a host can throw at an insert (in place, no input, NaN, odd blocks), the
// saved state, the latency test, the readouts and the monitor's log.
#include "host.h"
#include "../plugin/trace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>

int eft::g_fail = 0, eft::g_pass = 0;

namespace {

using namespace eft;
using namespace ef;

std::string g_dir;   // EF_TRACE_DIR for the run: the log and the latency flag live here

std::string logText() {
    std::ifstream f(g_dir + "/effectforce.log");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// 120 BPM: an eighth is 0.25 s.
constexpr int kEighth = kRate / 4;

void identity() {
    Host h;
    AEffect* e = h.e;
    CHECK(e->magic == vst::kMagic);
    CHECK(e->numInputs == 2 && e->numOutputs == 2);
    CHECK(e->flags & vst::effFlagsCanReplacing);
    CHECK(e->flags & vst::effFlagsProgramChunks);
    CHECK(!(e->flags & vst::effFlagsIsSynth));
    CHECK(!(e->flags & vst::effFlagsHasEditor));
    CHECK(e->initialDelay == 0);
    CHECK(e->numParams == P_COUNT);
    CHECK(e->uniqueID == vst::fourcc("EfPb"));
    CHECK(h.op(vst::effGetPlugCategory) == vst::kPlugCategEffect);
    char b[64] = {};
    h.op(vst::effGetEffectName, 0, 0, b);
    CHECK(std::string(b) == "EffectForce Probe");
    CHECK(h.canDo("plugAsChannelInsert") == 1);
    CHECK(h.canDo("receiveVstTimeInfo") == 1);
    CHECK(h.canDo("receiveVstMidiEvent") == -1);
    CHECK(h.canDo("bypass") == -1);
    CHECK(h.canDo("somethingNew") == 0);

    // Names fit under a knob and tell parameters apart; the first parameter is a read-only readout
    // (MPC sets parameter 0 when it loads a plugin).
    for (int i = 0; i < P_COUNT; ++i) {
        CHECK(!h.name(i).empty() && h.name(i).size() <= 13);
        for (int j = 0; j < i; ++j) CHECK(h.name(i) != h.name(j));
    }
    CHECK(PARAM_INFO[0].kind == Kind::Readout);
    e->setParameter(e, P_STATUS, 0.7f);
    CHECK(e->getParameter(e, P_STATUS) == 0.0f);
    CHECK(h.op(vst::effCanBeAutomated, P_STATUS) == 0);
    CHECK(h.op(vst::effCanBeAutomated, P_MIX) == 1);

    // Out of range is clamped, a NaN ignored.
    e->setParameter(e, P_MIX, 3.0f);
    CHECK(e->getParameter(e, P_MIX) == 1.0f);
    e->setParameter(e, P_MIX, std::nanf(""));
    CHECK(e->getParameter(e, P_MIX) == 1.0f);

    // Speaker arrangements: stereo in and out accepted, anything else refused.
    VstSpeakerArrangementHead st{1, 2}, mono{0, 1};
    CHECK(h.op(vst::effSetSpeakerArrangement, 0, reinterpret_cast<intptr_t>(&st), &st) == 1);
    CHECK(h.op(vst::effSetSpeakerArrangement, 0, reinterpret_cast<intptr_t>(&mono), &st) == 0);
}

void dryAndGain() {
    {   // mix 0, gain 0 dB: the input comes out bit for bit
        Host h;
        h.wet(0.0f);
        const std::vector<float> l = noise(kRate, 1), r = noise(kRate, 2);
        h.run(l, r);
        CHECK(h.L == l && h.R == r);
    }
    {   // +6.02 dB doubles it, from the first sample (the first block jumps to the targets)
        Host h;
        h.wet(0.0f);
        h.set(P_GAIN, 20.0f * std::log10(2.0f));
        const std::vector<float> x = noise(kRate);
        h.run(x);
        float worst = 0.0f;
        for (size_t i = 0; i < x.size(); ++i) worst = std::max(worst, std::fabs(h.L[i] - 2.0f * x[i]));
        CHECK(worst < 1e-5f);
        CHECK(h.display(P_GAIN) == "+6.0 dB");
    }
}

void echoTiming() {
    {   // 120 BPM, 1/8: the echo a quarter second later, the dry gone at mix 100%
        Host h;
        h.wet(1.0f);
        h.run(impulse(kRate, 100));
        CHECK(std::fabs(h.L[100]) < 1e-6f);
        CHECK(argmaxAbs(h.L) == static_cast<size_t>(100 + kEighth));
        CHECK(std::fabs(h.L[100 + kEighth] - 1.0f) < 1e-4f);
        CHECK(h.R == h.L);
        CHECK(h.display(P_STATUS).find("ECHO 250 ms") != std::string::npos);
    }
    {   // 90 BPM, dotted quarter: 1.5 beats = 1 s
        Host h;
        h.log.time.tempo = 90.0;
        h.wet(1.0f);
        h.setOption(P_DIV, "1/4.");
        CHECK(h.display(P_DIV) == "1/4.");
        h.run(impulse(2 * kRate, 10));
        CHECK(argmaxAbs(h.L) == static_cast<size_t>(10 + kRate));
    }
    {   // feedback: each repeat a factor fb quieter
        Host h;
        h.wet(1.0f, 0.5f);
        h.run(impulse(kRate, 0));
        CHECK(std::fabs(h.L[kEighth] - 1.0f) < 1e-4f);
        CHECK(std::fabs(h.L[2 * kEighth] - 0.5f) < 1e-4f);
        CHECK(std::fabs(h.L[3 * kEighth] - 0.25f) < 1e-4f);
    }
    {   // a tempo change glides the time: no clicks, no NaN, and the new time once settled
        Host h;
        h.wet(1.0f, 0.3f);
        h.run(noise(kRate));
        h.log.time.tempo = 60.0;   // 1/8 = 0.5 s
        h.silence(3 * kRate);      // the glide and the old echoes die away
        CHECK(h.finite);
        h.run(impulse(kRate, 0));
        CHECK(argmaxAbs(h.L) == static_cast<size_t>(2 * kEighth));
    }
    {   // no time info from the host: 120 BPM
        Host h;
        h.log.timeNull = true;
        h.wet(1.0f);
        h.run(impulse(kRate, 0));
        CHECK(argmaxAbs(h.L) == static_cast<size_t>(kEighth));
        CHECK(h.display(P_TIMING) == "NO TIME INFO FROM MPC");
    }
}

void hostEdgeCases() {
    // In place (in == out) gives exactly what separate buffers give.
    {
        Host a, b;
        b.inPlace = true;
        for (Host* h : {&a, &b}) h->wet(0.5f, 0.6f);
        const std::vector<float> l = noise(kRate, 7), r = noise(kRate, 8);
        a.run(l, r);
        b.run(l, r);
        CHECK(a.L == b.L && a.R == b.R);
        CHECK(contains(b.display(P_HOST), "IN PLACE"));
        CHECK(contains(a.display(P_HOST), "SEPARATE BUFFERS"));
        CHECK(contains(a.display(P_HOST), "STEREO IN"));
    }
    // Odd block sizes give what 128-frame blocks give; the legacy process() adds to its output.
    {
        Host ref;
        ref.wet(0.4f, 0.5f);
        const std::vector<float> x = noise(kRate / 2, 3);
        ref.run(x);
        for (int block : {1, 7, 333, 512, 1024}) {
            Host h;
            h.wet(0.4f, 0.5f);
            h.run(x, block);
            CHECK(h.L == ref.L);
        }
        Host legacy;
        legacy.wet(0.4f, 0.5f);
        std::vector<float> l = x, r = x, oL(x.size(), 1.0f), oR(x.size(), 1.0f);
        float* in[2] = {l.data(), r.data()};
        float* out[2] = {oL.data(), oR.data()};
        legacy.e->process(legacy.e, in, out, static_cast<int32_t>(x.size()));
        float worst = 0.0f;
        for (size_t i = 0; i < x.size(); ++i) worst = std::max(worst, std::fabs(oL[i] - 1.0f - ref.L[i]));
        CHECK(worst < 1e-6f);
    }
    // No input buffers: silence, not a crash.
    {
        Host h;
        h.nullInput = true;
        h.run(noise(kRate));
        CHECK(peakAbs(h.L) == 0.0f && peakAbs(h.R) == 0.0f);
        CHECK(contains(h.display(P_HOST), "NO INPUT"));
    }
    // A NaN or infinity in the input never reaches the output or the feedback loop.
    {
        Host h;
        h.wet(0.5f, 0.9f);
        std::vector<float> x = noise(kRate);
        x[100] = std::nanf("");
        x[200] = INFINITY;
        x[300] = -INFINITY;
        h.run(x);
        h.run(impulse(kRate, 0));
        CHECK(h.finite);
        CHECK(std::fabs(h.L[kEighth]) > 0.1f);   // still echoing
    }
}

void tails() {
    Host h;
    h.wet(0.5f, 0.6f);
    h.silence(kBlock);   // the tail follows the settings the audio thread has seen
    const intptr_t tail = h.op(vst::effGetTailSize);
    CHECK(tail > 10 * kEighth && tail < 20 * kEighth);   // 0.6^14 < 1e-3: 14 repeats
    // Echoes keep coming long after the one input sample ...
    h.run(impulse(static_cast<int>(tail), 0));
    CHECK(peakAbs(h.L, kRate) > 0.05f);
    // ... and are gone (-60 dB) by the tail the plugin reports.
    h.silence(kRate);
    CHECK(peakAbs(h.L) < 1e-3f);
}

void state() {
    Host a;
    a.set(P_GAIN, 3.5f);
    a.setOption(P_DIV, "1/4T");
    a.set(P_FB, 0.7f);
    a.set(P_MIX, 0.55f);
    const std::string s = a.chunk();
    CHECK(contains(s, "effectforce-probe 1\n"));
    CHECK(contains(s, "div=1/4T\n"));

    Host b;
    CHECK(b.load(s) == 1);
    CHECK(std::fabs(b.real(P_GAIN) - 3.5f) < 1e-4f);
    CHECK(b.display(P_DIV) == "1/4T");
    CHECK(std::fabs(b.real(P_FB) - 0.7f) < 1e-4f);
    CHECK(b.display(P_MIX) == "55%");

    Host c;
    CHECK(c.load("polyforce 4\ngain=6\n") == 0);   // not ours
    CHECK(c.real(P_GAIN) == 0.0f);
    CHECK(c.load("effectforce-probe 1\nfuture=1\ngain=-30\nfb=x\n") == 1);   // unknown key skipped, range clamped
    CHECK(c.real(P_GAIN) == -24.0f);
    CHECK(std::fabs(c.real(P_FB) - 0.4f) < 1e-4f);
}

void latencyTest() {
    const std::string flag = g_dir + "/effectforce.latency";
    std::ofstream(flag).put('\n');
    {
        Host h;
        CHECK(h.e->initialDelay == 4410);
        h.wet(0.0f);
        h.run(impulse(kRate, 10));
        CHECK(argmaxAbs(h.L) == 4420u);
        CHECK(contains(h.display(P_EVENTS), "LAT 4410"));
    }
    std::remove(flag.c_str());
    Host h;
    CHECK(h.e->initialDelay == 0);
}

void readouts() {
    Host h;
    CHECK(h.display(P_HOST) == "NO AUDIO YET");
    CHECK(h.display(P_TIMING) == "NO AUDIO YET");
    h.play(true);
    h.log.time.ppqPos = 8.0;
    h.run(noise(300 * kBlock));
    const std::string timing = h.display(P_TIMING);
    CHECK(contains(timing, "TEMPO 120.00"));
    CHECK(contains(timing, "PLAYING"));
    CHECK(contains(timing, "4/4"));
    const std::string host = h.display(P_HOST);
    CHECK(contains(host, "44100 HZ"));
    CHECK(contains(host, "BLOCK 128"));
    CHECK(contains(h.display(P_EVENTS), "BYPASS 0 "));
    CHECK(h.log.updates > 0);

    // MPC switching the insert off and on shows, and makes MPC re-read the text.
    const int before = h.log.updates;
    h.op(vst::effSetBypass, 0, 1);
    h.run(noise(kBlock * 20));
    CHECK(contains(h.display(P_EVENTS), "BYPASS 1 (ON)"));
    CHECK(h.log.updates > before);
    h.op(vst::effSetBypass, 0, 0);
    CHECK(contains(h.display(P_EVENTS), "BYPASS 2 (OFF)"));

    // Block sizes that vary show as a range.
    h.run(noise(1000), 100);
    CHECK(contains(h.display(P_HOST), "BLOCK 100-128"));
}

void monitorLog() {
    std::ofstream(g_dir + "/effectforce.log", std::ios::trunc);   // only this test's lines (the log appends)
    {
        Host h;
        h.run(noise(kRate / 2));
        sleepMs(250);
        h.op(vst::effSetBypass, 0, 1);
        h.set(P_MIX, 0.75f);
        sleepMs(600);   // no process calls: the monitor reports the stop
        h.run(noise(kRate / 4));
        // Silence for over a second of wall-clock time, still processed: what a reverb's tail needs.
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1300)) h.silence(kBlock * 8);
        sleepMs(250);
    }   // closing logs the rest
    const std::string log = logText();
    CHECK(contains(log, "created"));
    CHECK(contains(log, "process started"));
    CHECK(contains(log, "host: \"Fake MPC\" / \"probe_test\""));
    CHECK(contains(log, "host canDo yes: sendVstTimeInfo"));
    CHECK(contains(log, "buffers: separate in and out"));
    CHECK(contains(log, "time info: flags"));
    CHECK(contains(log, "effSetSampleRate"));
    CHECK(contains(log, "effSetBypass"));
    CHECK(contains(log, "param mix        = 75%"));
    CHECK(contains(log, "process stopped"));
    CHECK(contains(log, "process resumed"));
    CHECK(contains(log, "input silent for 1 s, process still called"));
    CHECK(contains(log, "closed after"));
}

std::string makeDir() {
    char tmpl[] = "/tmp/ef_probe_test_XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? d : "/tmp";
}

} // namespace

int main() {
    g_dir = makeDir();
    setenv("EF_TRACE_DIR", g_dir.c_str(), 1);

    identity();
    dryAndGain();
    echoTiming();
    hostEdgeCases();
    tails();
    state();
    latencyTest();
    readouts();
    monitorLog();

    std::remove((g_dir + "/effectforce.log").c_str());
    rmdir(g_dir.c_str());
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
