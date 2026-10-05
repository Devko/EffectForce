// EffectForce Probe: a VST2 insert effect for MPC OS (Force / MPC standalone) that answers what
// MPC does with an effect plugin before EffectForce is built on it (docs/PROBE.md).
//
// The sound is a tempo-synced delay (dsp/delay.h), enough to hear tempo, tails, bypass and
// latency. Everything MPC does is recorded (plugin/probe.h) and logged to /tmp/effectforce.log by
// a monitor thread; four readouts on the page show the essentials without a shell.
//
// Threads: processReplacing runs on one of MPC's audio workers (which one changes between calls,
// instances run concurrently); parameters, display text and chunks come from MPC's UI side. Host
// callbacks are only made from processReplacing.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vst2.h"
#include "param_ids.h"
#include "parameters.h"
#include "probe.h"
#include "trace.h"
#include "../dsp/probe_delay.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace {

using namespace ef;

constexpr size_t kTextCap = 128;          // JUCE reads names/display text into 256 bytes; stay well inside
constexpr float kSampleRate = 44100.0f;   // MPC OS always runs 44.1 kHz
constexpr int kScratch = 512;
constexpr int kLatencyTest = 4410;        // 100 ms, reported when the flag file exists (docs/PROBE.md)
constexpr float kAudible = 1e-4f;         // -80 dBFS

// Denormals (the delay's decaying tail) are slow on the VFP unit. Flush them to zero for our own
// arithmetic only: MPC's callbacks run in its worker's own FP mode, handed back afterwards.
class FlushDenormals {
public:
    FlushDenormals() {
#if defined(__arm__)
        asm volatile("vmrs %0, fpscr" : "=r"(saved_));
        asm volatile("vmsr fpscr, %0" : : "r"(saved_ | (1u << 24)));   // FZ
#elif defined(__SSE__) || defined(__x86_64__)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040);   // FTZ | DAZ
#endif
    }
    ~FlushDenormals() {
#if defined(__arm__)
        asm volatile("vmsr fpscr, %0" : : "r"(saved_));
#elif defined(__SSE__) || defined(__x86_64__)
        _mm_setcsr(saved_);
#endif
    }
    FlushDenormals(const FlushDenormals&) = delete;
    FlushDenormals& operator=(const FlushDenormals&) = delete;

private:
    uint32_t saved_ = 0;
};

double threadCpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

std::atomic<int> g_instances{0};

// The latency test: instances created while <trace dir>/effectforce.latency exists report 100 ms
// and delay their output by as much.
int latencyFromFlag() {
    struct stat st{};
    return ::stat((traceDir() + "/effectforce.latency").c_str(), &st) == 0 ? kLatencyTest : 0;
}

struct Plugin {
    AEffect      fx;   // must stay the first member: MPC hands us &fx back
    audioMasterCallback master = nullptr;
    Params       params;
    ProbeState   probe;
    StereoDelay  delay{kSampleRate};
    LatencyLine  latency;
    std::string  chunk;                        // effGetChunk buffer: must outlive the call
    std::atomic<int32_t> hostRate{0};          // effSetSampleRate's value
    std::atomic<int32_t> tail{0};              // effGetTailSize's answer, kept by the audio thread
    std::atomic<float>   echoMs{0.0f};

    // audio thread only
    bool     askedHost = false;
    double   winUs = 0.0, winBudgetUs = 0.0;
    float    winIn = 0.0f, winOut = 0.0f;
    uint32_t winCalls = 0;
    int32_t  winStartMs = 0;
    uint32_t shownSig = 0;
    int      updateWait = 0;

    Monitor monitor;   // last: built after what it watches, gone before it

    explicit Plugin(int latencySamples)
        : latency(latencySamples), monitor(g_instances.fetch_add(1) + 1, probe, params, latencySamples) {}
};

Plugin* self(AEffect* e) { return static_cast<Plugin*>(e->object); }

// Copies at most cap - 1 bytes, never cutting a UTF-8 character in half.
void copyStr(void* dst, const std::string& s, size_t cap) {
    if (!dst || cap == 0) return;
    size_t n = std::min(s.size(), cap - 1);
    if (n < s.size())
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    std::memcpy(dst, s.data(), n);
    static_cast<char*>(dst)[n] = 0;
}

std::string db(float v) {
    if (v <= -100.0f) return "-INF";
    char b[16];
    std::snprintf(b, sizeof b, "%+.0f dB", static_cast<double>(v));
    return b;
}

// --- the readouts (UI thread, from the probe's atomics) ---------------------------------------

std::string readout(const Plugin* p, int id) {
    const ProbeState& s = p->probe;
    char b[160];
    switch (id) {
        case P_STATUS:
            std::snprintf(b, sizeof b, "IN %s     OUT %s     CPU %.1f%%     ECHO %d ms", db(s.inDb.load()).c_str(),
                          db(s.outDb.load()).c_str(), static_cast<double>(s.cpuPct.load()),
                          static_cast<int>(std::lround(p->echoMs.load())));
            return b;
        case P_HOST: {
            if (s.calls.load() == 0) return "NO AUDIO YET";
            const int mn = s.minFrames.load(), mx = s.maxFrames.load();
            char blocks[24];
            if (mn == mx) std::snprintf(blocks, sizeof blocks, "%d", mx);
            else std::snprintf(blocks, sizeof blocks, "%d-%d", mn, mx);
            const int rate = p->hostRate.load() ? p->hostRate.load()
                             : s.hostReady.load(std::memory_order_acquire) ? s.host.sampleRate : 0;
            std::snprintf(b, sizeof b, "%d HZ     BLOCK %s     %s     %s     %.0f CALLS/S", rate, blocks,
                          s.inPlace.load() == 1 ? "IN PLACE" : "SEPARATE BUFFERS",
                          s.nullInputs.load() ? "NO INPUT" : s.stereoSeen.load() ? "STEREO IN" : "L = R IN",
                          static_cast<double>(s.callsPerSec.load()));
            return b;
        }
        case P_TIMING: {
            const int32_t f = s.timeFlags.load();
            if (f == INT32_MIN) return "NO AUDIO YET";
            if (f < 0) return "NO TIME INFO FROM MPC";
            char tempo[16] = "-";
            if (f & vst::kVstTempoValid) std::snprintf(tempo, sizeof tempo, "%.2f", static_cast<double>(s.tempo.load()));
            std::snprintf(b, sizeof b, "TEMPO %s     %s     BEAT %.2f     %d/%d     FLAGS 0x%04X", tempo,
                          f & vst::kVstTransportPlaying ? "PLAYING" : "STOPPED", static_cast<double>(s.ppq.load()),
                          s.sigNum.load(), s.sigDen.load(), static_cast<unsigned>(f));
            return b;
        }
        case P_EVENTS: {
            const auto n = [&](int op) { return s.opCount[op].load(); };
            std::snprintf(b, sizeof b, "BYPASS %u%s     TAIL? %u     SUSPEND %u     START %u STOP %u     SAVE %u LOAD %u     LAT %d",
                          n(vst::effSetBypass), n(vst::effSetBypass) ? (s.opVal[vst::effSetBypass].load() ? " (ON)" : " (OFF)") : "",
                          n(vst::effGetTailSize), n(vst::effMainsChanged), n(vst::effStartProcess), n(vst::effStopProcess),
                          n(vst::effGetChunk), n(vst::effSetChunk), p->latency.samples());
            return b;
        }
        default: return "";
    }
}

// --- parameters (UI thread) ---------------------------------------------------------------

float getParameter(AEffect* e, int32_t i) {
    try {
        return self(e)->params.get(i);
    } catch (...) {
        return 0.0f;
    }
}

void setParameter(AEffect* e, int32_t i, float v) {
    try {
        Plugin* p = self(e);
        p->probe.paramSets.fetch_add(1, std::memory_order_relaxed);
        p->probe.paramTid.store(threadId(), std::memory_order_relaxed);
        p->params.set(i, v);
    } catch (...) {
    }
}

// --- audio thread -------------------------------------------------------------------------

// Asked once, on the first block: host callbacks only from process.
void askHost(Plugin* p) {
    HostCensus& h = p->probe.host;
    const auto call = [&](int32_t op, void* ptr = nullptr) {
        return static_cast<int32_t>(p->master(&p->fx, op, 0, 0, ptr, 0.0f));
    };
    h.version = call(vst::audioMasterVersion);
    h.sampleRate = call(vst::audioMasterGetSampleRate);
    h.blockSize = call(vst::audioMasterGetBlockSize);
    h.processLevel = call(vst::audioMasterGetCurrentProcessLevel);
    h.vendorVersion = call(vst::audioMasterGetVendorVersion);
    call(vst::audioMasterGetVendorString, h.vendor);
    call(vst::audioMasterGetProductString, h.product);
    h.vendor[sizeof h.vendor - 1] = h.product[sizeof h.product - 1] = 0;
    for (int i = 0; i < kHostCanDos; ++i) h.canDo[i] = call(vst::audioMasterCanDo, const_cast<char*>(kHostCanDo[i]));
    p->probe.hostReady.store(1, std::memory_order_release);
}

// MPC's tempo: the delay follows it. A host callback, so in MPC's own FP mode.
double readTempo(Plugin* p) {
    ProbeState& s = p->probe;
    double bpm = 120.0;
    const intptr_t r = p->master(&p->fx, vst::audioMasterGetTime, 0,
                                 vst::kVstPpqPosValid | vst::kVstTempoValid | vst::kVstBarsValid | vst::kVstTimeSigValid, nullptr, 0.0f);
    const VstTimeInfo* t = reinterpret_cast<const VstTimeInfo*>(r);
    if (!t) {
        s.timeFlags.store(-1, std::memory_order_relaxed);
        return bpm;
    }
    // A NaN or absurd value would stall the delay: ignore it.
    if ((t->flags & vst::kVstTempoValid) && std::isfinite(t->tempo) && t->tempo >= 1.0 && t->tempo <= 1000.0) bpm = t->tempo;
    s.tempo.store(static_cast<float>(t->tempo), std::memory_order_relaxed);
    s.ppq.store(std::isfinite(t->ppqPos) ? static_cast<float>(t->ppqPos) : 0.0f, std::memory_order_relaxed);
    s.sigNum.store(t->timeSigNumerator, std::memory_order_relaxed);
    s.sigDen.store(t->timeSigDenominator, std::memory_order_relaxed);
    s.timeFlags.store(t->flags & 0x7FFFFFFF, std::memory_order_relaxed);
    return bpm;
}

float peakOf(const float* x, int n) {
    float m = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float a = std::fabs(x[i]);
        if (a > m) m = a;   // a NaN never wins
    }
    return m;
}

float toDb(float peak) { return peak > 1e-5f ? 20.0f * std::log10(peak) : -200.0f; }

// What the readouts show, folded into a number: MPC re-reads them only when it changes.
uint32_t readoutSignature(const Plugin* p) {
    const ProbeState& s = p->probe;
    uint32_t h = 2166136261u;
    const auto mix = [&](int32_t v) { h = (h ^ static_cast<uint32_t>(v)) * 16777619u; };
    mix(static_cast<int32_t>(std::lround(s.inDb.load())));
    mix(static_cast<int32_t>(std::lround(s.outDb.load())));
    mix(static_cast<int32_t>(std::lround(s.cpuPct.load() * 10.0f)));
    mix(static_cast<int32_t>(std::lround(s.callsPerSec.load())));
    mix(static_cast<int32_t>(std::lround(p->echoMs.load())));
    mix(s.timeFlags.load());
    mix(static_cast<int32_t>(std::lround(s.tempo.load() * 100.0f)));
    mix(static_cast<int32_t>(std::lround(s.ppq.load() * 100.0f)));
    mix(s.minFrames.load());
    mix(s.maxFrames.load());
    mix(s.inPlace.load());
    mix(s.stereoSeen.load());
    mix(s.nullInputs.load() > 0);
    const int ops[] = {vst::effSetBypass, vst::effGetTailSize, vst::effMainsChanged, vst::effStartProcess,
                       vst::effStopProcess, vst::effGetChunk, vst::effSetChunk};
    for (int op : ops) mix(static_cast<int32_t>(s.opCount[op].load()));
    mix(static_cast<int32_t>(s.opVal[vst::effSetBypass].load()));
    return h;
}

void meter(Plugin* p, double us, int n, float inPeak, float outPeak) {
    ProbeState& s = p->probe;
    const double budget = static_cast<double>(n) * 1e6 / kSampleRate;
    if (p->winCalls == 0) p->winStartMs = nowMs();
    p->winUs += us;
    p->winBudgetUs += budget;
    p->winIn = std::max(p->winIn, inPeak);
    p->winOut = std::max(p->winOut, outPeak);
    ++p->winCalls;
    if (p->winBudgetUs >= 500000.0) {
        const int32_t wall = std::max(1, nowMs() - p->winStartMs);
        s.inDb.store(toDb(p->winIn), std::memory_order_relaxed);
        s.outDb.store(toDb(p->winOut), std::memory_order_relaxed);
        s.cpuPct.store(static_cast<float>(100.0 * p->winUs / p->winBudgetUs), std::memory_order_relaxed);
        s.callsPerSec.store(static_cast<float>(p->winCalls) * 1000.0f / static_cast<float>(wall), std::memory_order_relaxed);
        p->winUs = p->winBudgetUs = 0.0;
        p->winIn = p->winOut = 0.0f;
        p->winCalls = 0;
    }
    // The dispatcher's counters change between windows too: look every 16 blocks (~46 ms).
    if (++p->updateWait < 16) return;
    p->updateWait = 0;
    const uint32_t sig = readoutSignature(p);
    if (sig != p->shownSig) {
        p->shownSig = sig;
        if (p->master) p->master(&p->fx, vst::audioMasterUpdateDisplay, 0, 0, nullptr, 0.0f);
    }
}

void processReplacing(AEffect* e, float** in, float** out, int32_t n) {
    if (!out || !out[0] || !out[1] || n <= 0) return;
    Plugin* p = self(e);
    ProbeState& s = p->probe;
    const double t0 = threadCpuUs();
    const int32_t now = nowMs();
    float inPeak = 0.0f, outPeak = 0.0f;
    try {
        s.noteAudioThread();
        s.lastCallMs.store(now, std::memory_order_relaxed);
        s.frames.store(n, std::memory_order_relaxed);
        if (n < s.minFrames.load(std::memory_order_relaxed)) s.minFrames.store(n, std::memory_order_relaxed);
        if (n > s.maxFrames.load(std::memory_order_relaxed)) s.maxFrames.store(n, std::memory_order_relaxed);

        // No input buffers: process silence, in place.
        const float* inL = out[0];
        const float* inR = out[1];
        if (in && in[0] && in[1]) {
            inL = in[0];
            inR = in[1];
            s.inPlace.store(inL == out[0] && inR == out[1] ? 1 : 0, std::memory_order_relaxed);
        } else {
            s.nullInputs.fetch_add(1, std::memory_order_relaxed);
            std::memset(out[0], 0, sizeof(float) * static_cast<size_t>(n));
            std::memset(out[1], 0, sizeof(float) * static_cast<size_t>(n));
        }
        inPeak = std::max(peakOf(inL, n), peakOf(inR, n));
        if (inPeak > kAudible) s.lastAudibleMs.store(now, std::memory_order_relaxed);
        if (!s.stereoSeen.load(std::memory_order_relaxed) && std::memcmp(inL, inR, sizeof(float) * static_cast<size_t>(n)) != 0)
            s.stereoSeen.store(1, std::memory_order_relaxed);

        if (!p->askedHost && p->master) {
            p->askedHost = true;
            askHost(p);
        }
        const double bpm = p->master ? readTempo(p) : 120.0;

        const Params& pr = p->params;
        const double beats = kDivisionBeats[std::clamp(pr.option(P_DIV), 0, static_cast<int>(PARAM_INFO[P_DIV].hi))];
        const double delaySamples = beats * 60.0 / bpm * kSampleRate;
        p->echoMs.store(static_cast<float>(beats * 60000.0 / bpm), std::memory_order_relaxed);
        p->delay.set(std::pow(10.0f, pr.real(P_GAIN) / 20.0f), delaySamples, pr.real(P_FB), pr.real(P_MIX));
        p->tail.store(static_cast<int32_t>(std::min<int64_t>(p->delay.tailSamples() + p->latency.samples(), INT32_MAX)),
                      std::memory_order_relaxed);

        FlushDenormals ftz;
        p->delay.process(inL, inR, out[0], out[1], n);
        p->latency.process(out[0], out[1], n);
        outPeak = std::max(peakOf(out[0], n), peakOf(out[1], n));
    } catch (...) {   // nothing may throw into MPC: an escaping exception ends the whole process
        std::memset(out[0], 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(out[1], 0, sizeof(float) * static_cast<size_t>(n));
    }
    meter(p, threadCpuUs() - t0, n, inPeak, outPeak);
    s.calls.fetch_add(1, std::memory_order_release);
}

// Legacy accumulating entry point (MPC uses processReplacing): sub-blocks of kScratch.
void process(AEffect* e, float** in, float** out, int32_t n) {
    if (!out || !out[0] || !out[1]) return;
    Plugin* p = self(e);
    p->probe.legacyCalls.fetch_add(1, std::memory_order_relaxed);
    float tmpL[kScratch], tmpR[kScratch];
    for (int32_t pos = 0; pos < n; pos += kScratch) {
        const int32_t m = std::min<int32_t>(kScratch, n - pos);
        float* subIn[2] = {in && in[0] ? in[0] + pos : nullptr, in && in[1] ? in[1] + pos : nullptr};
        float* tmp[2] = {tmpL, tmpR};
        processReplacing(e, in ? subIn : nullptr, tmp, m);
        for (int32_t i = 0; i < m; ++i) {
            out[0][pos + i] += tmpL[i];
            out[1][pos + i] += tmpR[i];
        }
    }
}

// --- the dispatcher -----------------------------------------------------------------------

int32_t canDoAnswer(const char* s) {
    static const char* const yes[] = {"receiveVstTimeInfo", "plugAsChannelInsert", "plugAsSend", "2in2out"};
    static const char* const no[] = {"receiveVstEvents", "receiveVstMidiEvent", "sendVstEvents", "sendVstMidiEvent",
                                     "bypass", "offline", "midiProgramNames"};
    for (const char* y : yes)
        if (!std::strcmp(s, y)) return 1;
    for (const char* x : no)
        if (!std::strcmp(s, x)) return -1;
    return 0;   // don't know
}

intptr_t dispatch(Plugin* p, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
    const bool validIdx = idx >= 0 && idx < P_COUNT;
    switch (op) {
        case vst::effOpen: return 1;
        case vst::effClose: delete p; return 1;
        case vst::effGetProgram: return 0;
        case vst::effGetProgramName: copyStr(ptr, "Default", 24); return 0;
        case vst::effGetProgramNameIndexed:
            if (idx != 0) return 0;
            copyStr(ptr, "Default", 24);
            return 1;
        case vst::effGetPlugCategory: return vst::kPlugCategEffect;
        case vst::effGetEffectName:
        case vst::effGetProductString: copyStr(ptr, kPlugName, 32); return 1;
        case vst::effGetVendorString: copyStr(ptr, kPlugVendor, 32); return 1;
        case vst::effGetVendorVersion: return kPlugVersion;
        case vst::effGetVstVersion: return 2400;
        case vst::effCanBeAutomated: return validIdx && PARAM_INFO[idx].kind != Kind::Readout ? 1 : 0;
        case vst::effGetParamName: copyStr(ptr, validIdx ? PARAM_INFO[idx].name : "", kTextCap); return 0;
        case vst::effGetParamLabel: copyStr(ptr, "", 8); return 0;
        case vst::effGetParamDisplay:
            if (!validIdx) copyStr(ptr, "", kTextCap);
            else if (PARAM_INFO[idx].kind == Kind::Readout) copyStr(ptr, readout(p, idx), kTextCap);
            else copyStr(ptr, p->params.display(idx), kTextCap);
            return 0;
        case vst::effSetSampleRate:   // MPC OS is fixed at 44.1 kHz; the delay is built for it
            p->hostRate.store(static_cast<int32_t>(std::lround(opt)));
            return 1;
        case vst::effSetBlockSize: return 1;
        case vst::effMainsChanged: return 0;   // logged only: the probe wants to see tails, not cut them
        case vst::effSetBypass: return 0;      // no soft bypass: MPC bypasses itself, and the log shows how
        case vst::effGetTailSize: return p->tail.load();
        case vst::effSetSpeakerArrangement: {
            const auto* inArr = reinterpret_cast<const VstSpeakerArrangementHead*>(val);
            const auto* outArr = static_cast<const VstSpeakerArrangementHead*>(ptr);
            p->probe.spkIn.store(inArr ? inArr->numChannels : -1);
            p->probe.spkOut.store(outArr ? outArr->numChannels : -1);
            return inArr && outArr && inArr->numChannels == 2 && outArr->numChannels == 2 ? 1 : 0;
        }
        case vst::effCanDo: {
            const char* s = static_cast<const char*>(ptr);
            if (!s) return 0;
            const int32_t answer = canDoAnswer(s);
            p->probe.noteCanDo(s, answer);
            return answer;
        }
        case vst::effGetChunk:
            if (!ptr) return 0;
            p->chunk = p->params.save();
            *static_cast<void**>(ptr) = const_cast<char*>(p->chunk.c_str());
            return static_cast<intptr_t>(p->chunk.size() + 1);
        case vst::effSetChunk: {
            if (!ptr || val <= 0 || val > (1 << 16)) return 0;   // a state is ~100 bytes; more is not ours
            std::string s(static_cast<const char*>(ptr), static_cast<size_t>(val));
            while (!s.empty() && s.back() == '\0') s.pop_back();
            return p->params.load(s) ? 1 : 0;
        }
        default: return 0;
    }
}

intptr_t dispatcher(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
    try {
        Plugin* p = self(e);
        p->probe.noteOp(op, idx, op == vst::effSetSampleRate ? static_cast<intptr_t>(std::lround(opt)) : val);
        return dispatch(p, op, idx, val, ptr, opt);
    } catch (...) {
        return 0;
    }
}

AEffect* createPlugin(audioMasterCallback master) {
    const int latency = latencyFromFlag();
    Plugin* p = new Plugin(latency);
    p->master = master;

    AEffect* e = &p->fx;
    std::memset(e, 0, sizeof(*e));
    e->magic            = vst::kMagic;
    e->dispatcher       = dispatcher;
    e->process          = process;
    e->setParameter     = setParameter;
    e->getParameter     = getParameter;
    e->processReplacing = processReplacing;
    e->numParams        = P_COUNT;
    e->numInputs        = 2;
    e->numOutputs       = 2;
    e->flags            = vst::effFlagsCanReplacing | vst::effFlagsProgramChunks;
    e->initialDelay     = latency;
    e->uniqueID         = kPlugUid;
    e->version          = kPlugVersion;
    e->object           = p;
    return e;
}

} // namespace

// Nothing may throw into MPC: a failed creation reports "no plugin" instead.
extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(audioMasterCallback master) {
    try {
        return createPlugin(master);
    } catch (...) {
        return nullptr;
    }
}
