#pragma once
// A fake MPC for the insert effect: drives the plugin through its VST2 entry points the way MPC
// does (128-frame blocks, in place, 0..1 params, the Force's taps and Q-Link detents), serves
// transport time, and records what the plugin pushes back (audioMasterAutomate,
// audioMasterUpdateDisplay).
#include "signal.h"
#include "../plugin/vst2.h"
#include "../plugin/rack_map.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback master);

namespace eft {

constexpr int kBlock = 128;
constexpr int kBlocksPerSec = 44100 / kBlock;

struct HostLog {
    int updates = 0;
    std::map<int, float> automated;   // index -> last value pushed
    std::map<int, int> automateCount;
    VstTimeInfo time{};
    bool noTime = false;              // getTime returns null
};

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt);   // plugin_test.cpp

struct Host {
    AEffect* e;
    HostLog log;
    Buf L, R;              // the last run's output
    bool inPlace = true;   // MPC's way; false: separate input and output buffers
    bool finite = true;    // every output sample so far

    Host() : e(VSTPluginMain(hostMaster)) {
        e->user = &log;
        log.time.sampleRate = 44100.0;
        log.time.tempo = 120.0;
        log.time.timeSigNumerator = log.time.timeSigDenominator = 4;
        log.time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
        op(vst::effOpen);
        op(vst::effMainsChanged, 0, 1);
    }
    ~Host() { op(vst::effClose); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    intptr_t op(int32_t code, int32_t idx = 0, intptr_t val = 0, void* ptr = nullptr, float opt = 0.0f) {
        return e->dispatcher(e, code, idx, val, ptr, opt);
    }

    void set(int id, float value) { e->setParameter(e, id, ef::paramNorm(id, value)); }
    void setN(int id, float n) { e->setParameter(e, id, n); }
    float get(int id) { return e->getParameter(e, id); }
    float value(int id) { return ef::paramValue(id, get(id)); }
    void option(int id, const char* name) {
        for (int o = 0; o < ef::PARAM_INFO[id].nopts; ++o)
            if (!std::strcmp(ef::PARAM_INFO[id].opts[o], name)) set(id, static_cast<float>(o));
    }
    void on(int onParam) { option(onParam, "On"); }

    // A tap on a button, as a Force sends it: MPC toggles the value it last read back. A button
    // reads back 0 (it springs back), so a tap is a single 1, never followed by a release.
    void press(int id) { e->setParameter(e, id, get(id) > 0.5f ? 0.0f : 1.0f); }
    // A tap on a tile or toggle: MPC toggles what it read back.
    void tap(int id) { press(id); }
    // One Q-Link detent (dir +1 / -1), as a Force sends it: the value MPC last read back plus
    // 1/128 of the range, rounded to 1/1000 (MPC OS 3.9.1, measured by sd88me/mpc-vst-plugins,
    // docs/NOTES.md "Input probe"). Several in a Turn are one gesture.
    void detent(int id, int dir) {
        e->setParameter(e, id, std::round((get(id) + static_cast<float>(dir) / 128.0f) * 1000.0f) / 1000.0f);
    }

    void play(bool on) {
        if (on) log.time.flags |= vst::kVstTransportPlaying;
        else log.time.flags &= ~vst::kVstTransportPlaying;
    }

    // Feeds l / r through the plugin in blocks of `block` frames; the output lands in L / R. The
    // transport advances while playing.
    void run(const Buf& l, const Buf& r, int block = kBlock) {
        L.assign(l.size(), 0.0f);
        R.assign(l.size(), 0.0f);
        Buf bl(static_cast<size_t>(block)), br(bl.size());
        for (size_t pos = 0; pos < l.size(); pos += static_cast<size_t>(block)) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(block), l.size() - pos));
            std::copy(l.begin() + static_cast<long>(pos), l.begin() + static_cast<long>(pos) + n, bl.begin());
            std::copy(r.begin() + static_cast<long>(pos), r.begin() + static_cast<long>(pos) + n, br.begin());
            if (inPlace) {
                float* io[2] = {bl.data(), br.data()};
                e->processReplacing(e, io, io, n);
                std::copy(bl.begin(), bl.begin() + n, L.begin() + static_cast<long>(pos));
                std::copy(br.begin(), br.begin() + n, R.begin() + static_cast<long>(pos));
            } else {
                float* in[2] = {bl.data(), br.data()};
                float* out[2] = {L.data() + pos, R.data() + pos};
                e->processReplacing(e, in, out, n);
            }
            if (log.time.flags & vst::kVstTransportPlaying) {
                log.time.samplePos += n;
                log.time.ppqPos += n / 44100.0 * log.time.tempo / 60.0;
            }
        }
        for (size_t i = 0; i < L.size(); ++i)
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
    }
    void run(const Buf& mono, int block = kBlock) { run(mono, mono, block); }
    void silence(int samples) { run(Buf(static_cast<size_t>(samples), 0.0f)); }
    void blocks(int n) { silence(n * kBlock); }

    std::string display(int id) {
        char b[256] = {};
        op(vst::effGetParamDisplay, id, 0, b);
        return b;
    }
    std::string name(int id) {
        char b[256] = {};
        op(vst::effGetParamName, id, 0, b);
        return b;
    }
    std::string chunk() {
        void* data = nullptr;
        const intptr_t size = op(vst::effGetChunk, 0, 0, &data);
        return size > 0 && data ? std::string(static_cast<const char*>(data)) : std::string();
    }
    intptr_t load(const std::string& s) {
        return op(vst::effSetChunk, 0, static_cast<intptr_t>(s.size()), const_cast<char*>(s.data()));
    }
};

std::string fixtureDir();   // per-run temp folder (removed at exit)

// The surface's clock moves this far per host event (plugin_test.cpp): a second, so separate
// events never read as one gesture, whatever the machine's speed.
extern long long g_msPerEvent;
// Within a Turn, events come a few ms apart, as a Q-Link turn's detents or a tile's release echo.
struct Turn {
    explicit Turn(long long ms = 5) : was(g_msPerEvent) { g_msPerEvent = ms; }
    ~Turn() { g_msPerEvent = was; }
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
    long long was;
};

// The plugin's suites (each in its own test/*_test.cpp).
void rackTests();
void modTests();
void chainTests();
void presetTests();

} // namespace eft

// The modules' suites: global, so each also builds on its own (make test-module, test/module_main.cpp).
void dspTests();
void driveTests();
void filterTests();
void eqTests();
void compTests();
void chorusTests();
void phaserTests();
void pulseTests();
void grainTests();
void delayTests();
void reverbTests();
