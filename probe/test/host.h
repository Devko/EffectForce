#pragma once
// A fake MPC for an insert effect: drives the plugin through its VST2 entry points the way MPC does
// (128-frame blocks, 0..1 params, stereo in and out), serves transport time and the host queries
// the probe asks, and records what the plugin pushes back.
#include "check.h"
#include "../plugin/vst2.h"
#include "../plugin/parameters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback master);

namespace eft {

constexpr int kBlock = 128;
constexpr int kRate = 44100;

struct HostLog {
    int updates = 0, automates = 0, timeAsked = 0;
    bool timeNull = false;   // getTime returns null, as a host without a transport would
    VstTimeInfo time{};
};

inline intptr_t hostMaster(AEffect* e, int32_t op, int32_t, intptr_t, void* ptr, float) {
    HostLog* log = e ? static_cast<HostLog*>(e->user) : nullptr;
    if (!log) return 0;
    switch (op) {
        case vst::audioMasterGetTime:
            ++log->timeAsked;
            return log->timeNull ? 0 : reinterpret_cast<intptr_t>(&log->time);
        case vst::audioMasterUpdateDisplay: ++log->updates; return 1;
        case vst::audioMasterAutomate: ++log->automates; return 1;
        case vst::audioMasterVersion: return 2400;
        case vst::audioMasterGetSampleRate: return kRate;
        case vst::audioMasterGetBlockSize: return kBlock;
        case vst::audioMasterGetVendorString: std::strcpy(static_cast<char*>(ptr), "Fake MPC"); return 1;
        case vst::audioMasterGetProductString: std::strcpy(static_cast<char*>(ptr), "probe_test"); return 1;
        case vst::audioMasterCanDo: return std::strcmp(static_cast<const char*>(ptr), "sendVstTimeInfo") == 0 ? 1 : 0;
        default: return 0;
    }
}

struct Host {
    AEffect* e;
    HostLog log;
    std::vector<float> L, R;   // the last run's output
    bool inPlace = false;      // process with in == out, as some hosts do
    bool nullInput = false;    // process with no input buffers
    bool finite = true;        // every output sample so far

    Host() : e(VSTPluginMain(hostMaster)) {
        e->user = &log;
        log.time.sampleRate = kRate;
        log.time.tempo = 120.0;
        log.time.timeSigNumerator = log.time.timeSigDenominator = 4;
        log.time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTimeSigValid;
        op(vst::effOpen);
        op(vst::effSetSampleRate, 0, 0, nullptr, static_cast<float>(kRate));
        op(vst::effSetBlockSize, 0, kBlock);
        op(vst::effMainsChanged, 0, 1);
    }
    ~Host() { op(vst::effClose); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    intptr_t op(int32_t code, int32_t idx = 0, intptr_t val = 0, void* ptr = nullptr, float opt = 0.0f) {
        return e->dispatcher(e, code, idx, val, ptr, opt);
    }
    intptr_t canDo(const char* s) { return op(vst::effCanDo, 0, 0, const_cast<char*>(s)); }

    void set(int id, float real) { e->setParameter(e, id, ef::normOf(id, real)); }
    float real(int id) { return ef::realOf(id, e->getParameter(e, id)); }
    void setOption(int id, const char* name) {
        const ef::ParamInfo& p = ef::PARAM_INFO[id];
        for (int o = 0; o < p.nOpts; ++o)
            if (!std::strcmp(p.opts[o], name)) set(id, static_cast<float>(o));
    }
    // A plain delay: dry only (mix 0) or wet only (mix 1), no feedback, unity gain.
    void wet(float mix, float fb = 0.0f) {
        set(ef::P_GAIN, 0.0f);
        set(ef::P_MIX, mix);
        set(ef::P_FB, fb);
    }

    void play(bool on) {
        if (on) log.time.flags |= vst::kVstTransportPlaying;
        else log.time.flags &= ~vst::kVstTransportPlaying;
    }

    // Feeds l / r through the plugin in blocks of `block` frames (the last may be shorter);
    // the output lands in L / R. The transport advances while playing.
    void run(const std::vector<float>& l, const std::vector<float>& r, int block = kBlock) {
        L.assign(l.size(), 0.0f);
        R.assign(l.size(), 0.0f);
        std::vector<float> bl(static_cast<size_t>(block)), br(bl.size());
        for (size_t pos = 0; pos < l.size(); pos += static_cast<size_t>(block)) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(block), l.size() - pos));
            std::copy(l.begin() + static_cast<long>(pos), l.begin() + static_cast<long>(pos) + n, bl.begin());
            std::copy(r.begin() + static_cast<long>(pos), r.begin() + static_cast<long>(pos) + n, br.begin());
            if (inPlace) {
                float* io[2] = {bl.data(), br.data()};
                e->processReplacing(e, nullInput ? nullptr : io, io, n);
                std::copy(bl.begin(), bl.begin() + n, L.begin() + static_cast<long>(pos));
                std::copy(br.begin(), br.begin() + n, R.begin() + static_cast<long>(pos));
            } else {
                float* in[2] = {bl.data(), br.data()};
                float* out[2] = {L.data() + pos, R.data() + pos};
                e->processReplacing(e, nullInput ? nullptr : in, out, n);
            }
            if (log.time.flags & vst::kVstTransportPlaying) {
                log.time.samplePos += n;
                log.time.ppqPos += n / static_cast<double>(kRate) * log.time.tempo / 60.0;
            }
        }
        for (size_t i = 0; i < L.size(); ++i)
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
    }
    void run(const std::vector<float>& mono, int block = kBlock) { run(mono, mono, block); }
    void silence(int samples) { run(std::vector<float>(static_cast<size_t>(samples), 0.0f)); }

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

// Signal helpers
inline std::vector<float> impulse(int length, int at, float v = 1.0f) {
    std::vector<float> x(static_cast<size_t>(length), 0.0f);
    x[static_cast<size_t>(at)] = v;
    return x;
}
inline std::vector<float> noise(int length, uint32_t seed = 1) {
    std::vector<float> x(static_cast<size_t>(length));
    for (float& s : x) {
        seed = seed * 1664525u + 1013904223u;
        s = static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f * 0.5f;
    }
    return x;
}
inline size_t argmaxAbs(const std::vector<float>& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    size_t best = from;
    for (size_t i = from; i < to; ++i)
        if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
    return best;
}
inline float peakAbs(const std::vector<float>& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    float m = 0.0f;
    for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(x[i]));
    return m;
}

} // namespace eft
