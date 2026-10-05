#include "probe.h"
#include "trace.h"
#include "vst2.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ef {

namespace {

const auto kLoaded = std::chrono::steady_clock::now();

// Getters MPC polls (on the Force: ~80 calls/s each for the names while the plugin's screen is
// open, hundreds for the parameters): logged the first time only, their rates in the heartbeat.
// Everything else is logged on every change.
bool chatty(int op) {
    switch (op) {
        case vst::effGetProgram: case vst::effGetProgramName: case vst::effGetParamLabel:
        case vst::effGetParamDisplay: case vst::effGetParamName: case vst::effEditIdle:
        case vst::effProcessEvents: case vst::effCanBeAutomated: case vst::effGetProgramNameIndexed:
        case vst::effGetParameterProperties: case vst::effGetEffectName: case vst::effGetVendorString:
        case vst::effGetProductString: case vst::effGetVendorVersion: case vst::effGetPlugCategory:
            return true;
        default:
            return false;
    }
}

const char* yesNo(int32_t v) { return v < 0 ? "?" : v ? "yes" : "no"; }

} // namespace

int32_t nowMs() {
    return static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - kLoaded).count());
}

int32_t threadId() { return static_cast<int32_t>(::syscall(SYS_gettid)); }

void ProbeState::noteOp(int32_t op, int32_t idx, intptr_t val) {
    if (op < 0 || op >= kOps) {
        opOther.fetch_add(1, std::memory_order_relaxed);
        opOtherLast.store(op, std::memory_order_relaxed);
        return;
    }
    opIdx[op].store(idx, std::memory_order_relaxed);
    opVal[op].store(val, std::memory_order_relaxed);
    if (opFirstTid[op].load(std::memory_order_relaxed) == 0) opFirstTid[op].store(threadId(), std::memory_order_relaxed);
    opCount[op].fetch_add(1, std::memory_order_release);
}

void ProbeState::noteCanDo(const char* s, int32_t answer) {
    const int n = nCanDo.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kCanDoMax; ++i)
        if (canDoReady[i].load(std::memory_order_acquire) && !std::strncmp(canDo[i], s, sizeof canDo[i] - 1)) return;
    const int slot = nCanDo.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kCanDoMax) return;
    std::strncpy(canDo[slot], s, sizeof canDo[slot] - 1);
    canDoAnswer[slot].store(answer, std::memory_order_relaxed);
    canDoReady[slot].store(1, std::memory_order_release);
}

void ProbeState::noteAudioThread() {
    const int32_t tid = threadId();
    if (isAudioTid(tid)) return;
    const int slot = nAudioTids.load(std::memory_order_relaxed);
    if (slot >= kAudioTids) return;
    audioTid[slot].store(tid, std::memory_order_relaxed);
    nAudioTids.store(slot + 1, std::memory_order_release);   // only audio threads write; MPC runs one block of an instance at a time
}

bool ProbeState::isAudioTid(int32_t tid) const {
    const int n = std::min(nAudioTids.load(std::memory_order_acquire), kAudioTids);
    for (int i = 0; i < n; ++i)
        if (audioTid[i].load(std::memory_order_relaxed) == tid) return true;
    return false;
}

// --- the monitor ----------------------------------------------------------------------------

Monitor::Monitor(int instance, const ProbeState& st, const Params& params, int latency)
    : instance_(instance), st_(st), params_(params), createdMs_(nowMs()) {
    for (int i = 0; i < P_COUNT; ++i) norm_[i] = params_.get(i);
    trace("#%d created (thread %d), reports latency %d samples", instance_, threadId(), latency);
    thread_ = std::thread([this] { run(); });
}

Monitor::~Monitor() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    tick();   // whatever happened since the last one
    trace("#%d closed after %.1f s, %u process calls", instance_, (nowMs() - createdMs_) / 1000.0,
          st_.calls.load());
}

void Monitor::run() {
    pthread_setname_np(pthread_self(), "ef-probe");
    std::unique_lock<std::mutex> lk(mtx_);
    while (!stop_) {
        cv_.wait_for(lk, std::chrono::milliseconds(100));
        if (stop_) break;
        lk.unlock();
        tick();
        lk.lock();
    }
}

void Monitor::tick() {
    const int I = instance_;
    const int32_t now = nowMs();

    if (!hostLogged_ && st_.hostReady.load(std::memory_order_acquire)) {
        hostLogged_ = true;
        const HostCensus& h = st_.host;
        trace("#%d host: \"%s\" / \"%s\" vendor version %d, VST %d, sample rate %d, block size %d, process level %d", I,
              h.vendor, h.product, h.vendorVersion, h.version, h.sampleRate, h.blockSize, h.processLevel);
        std::string yes, no;
        for (int i = 0; i < kHostCanDos; ++i) {
            std::string& to = h.canDo[i] > 0 ? yes : no;
            if (!to.empty()) to += ", ";
            to += kHostCanDo[i];
            if (h.canDo[i] < 0) to += "(-1)";
        }
        trace("#%d host canDo yes: %s", I, yes.empty() ? "-" : yes.c_str());
        trace("#%d host canDo no:  %s", I, no.empty() ? "-" : no.c_str());
    }

    // process: started, stopped, resumed, still called in silence
    const uint32_t calls = st_.calls.load(std::memory_order_acquire);
    const int32_t lastCall = st_.lastCallMs.load(std::memory_order_relaxed);
    const int32_t audible = st_.lastAudibleMs.load(std::memory_order_relaxed);
    if (calls != calls_) {
        if (!everRan_) {
            trace("#%d process started", I);
            startedMs_ = beatMs_ = now;
        } else if (!running_) {
            trace("#%d process resumed after about %d ms without calls", I, now - stoppedMs_);
        }
        running_ = everRan_ = true;
        calls_ = calls;
    } else if (running_ && now - lastCall > 300) {
        running_ = false;
        stoppedMs_ = lastCall;
        trace("#%d process stopped: no call for %d ms; input silent for %s before; transport %s", I, now - lastCall,
              audible < 0 ? "ever" : (std::to_string(std::max(0, lastCall - audible)) + " ms").c_str(),
              st_.timeFlags.load() >= 0 && (st_.timeFlags.load() & vst::kVstTransportPlaying) ? "playing" : "stopped");
    }
    if (running_) {
        const int32_t silent = lastCall - (audible < 0 ? startedMs_ : audible);
        const int32_t marks[] = {1, 10, 60};
        for (int32_t m : marks)
            if (silent >= m * 1000 && silentMark_ < m) {
                silentMark_ = m;
                trace("#%d input silent for %d s, process still called (%.0f calls/s)", I, m,
                      static_cast<double>(st_.callsPerSec.load()));
            }
        if (silent < 1000) silentMark_ = 0;
    }

    // block sizes, buffers, threads
    const int32_t mn = st_.minFrames.load(), mx = st_.maxFrames.load(), ip = st_.inPlace.load(),
                  stereo = st_.stereoSeen.load(), nulls = st_.nullInputs.load(), tids = st_.nAudioTids.load();
    if (mn != minFrames_ || mx != maxFrames_) {
        trace("#%d block sizes %d..%d", I, mn, mx);
        minFrames_ = mn;
        maxFrames_ = mx;
    }
    if (ip != inPlace_) {
        trace("#%d buffers: %s", I, ip ? "in place (in == out)" : "separate in and out");
        inPlace_ = ip;
    }
    if (stereo != stereo_) {
        trace("#%d input: the channels differ (stereo)", I);
        stereo_ = stereo;
    }
    if (nulls != nullInputs_) {
        trace("#%d %d blocks without input buffers", I, nulls);
        nullInputs_ = nulls;
    }
    if (tids != nTids_) {
        std::string list;
        for (int i = 0; i < std::min(tids, kAudioTids); ++i) list += " " + std::to_string(st_.audioTid[i].load());
        trace("#%d audio threads:%s", I, list.c_str());
        nTids_ = tids;
    }
    const uint32_t legacy = st_.legacyCalls.load();
    if (legacy != legacy_) {
        if (legacy_ == 0) trace("#%d MPC calls the accumulating process(), not only processReplacing", I);
        legacy_ = legacy;
    }

    // transport
    const int32_t flags = st_.timeFlags.load();
    const float tempo = st_.tempo.load();
    const int32_t sn = st_.sigNum.load(), sd = st_.sigDen.load();
    if (flags != INT32_MIN && (flags != flags_ || std::fabs(tempo - tempo_) > 0.005f || sn != sigNum_ || sd != sigDen_)) {
        if (flags < 0)
            trace("#%d time info: none (getTime returned null)", I);
        else
            trace("#%d time info: flags 0x%04x (%s%s%s%s%s), tempo %.2f, beat %.2f, %d/%d", I, static_cast<unsigned>(flags),
                  flags & vst::kVstTransportPlaying ? "playing" : "stopped",
                  flags & vst::kVstTransportRecording ? ", recording" : "",
                  flags & vst::kVstTempoValid ? ", tempo" : "", flags & vst::kVstPpqPosValid ? ", ppq" : "",
                  flags & vst::kVstTimeSigValid ? ", signature" : "", static_cast<double>(tempo),
                  static_cast<double>(st_.ppq.load()), sn, sd);
        flags_ = flags;
        tempo_ = tempo;
        sigNum_ = sn;
        sigDen_ = sd;
    }

    // the dispatcher
    for (int op = 0; op < kOps; ++op) {
        const uint32_t c = st_.opCount[op].load(std::memory_order_acquire);
        if (c == opCount_[op]) continue;
        const bool first = opCount_[op] == 0;
        opCount_[op] = c;
        if (!first && chatty(op)) continue;
        const char* name = vst::opcodeName(op);
        const int32_t tid = st_.opFirstTid[op].load();
        trace("#%d op %2d %-24s x%u  index %d value %ld%s", I, op, name ? name : "?", c, st_.opIdx[op].load(),
              static_cast<long>(st_.opVal[op].load()),
              first ? (st_.isAudioTid(tid) ? "  (first call on an audio thread)" : "  (first call off the audio threads)") : "");
    }
    const uint32_t other = st_.opOther.load();
    if (other != opOther_) {
        trace("#%d op %d (out of range) x%u", I, st_.opOtherLast.load(), other);
        opOther_ = other;
    }
    const int32_t si = st_.spkIn.load(), so = st_.spkOut.load();
    if (si != spkIn_ || so != spkOut_) {
        trace("#%d speaker arrangement: %d in, %d out", I, si, so);
        spkIn_ = si;
        spkOut_ = so;
    }
    const int nc = std::min(st_.nCanDo.load(std::memory_order_acquire), kCanDoMax);
    for (; nCanDo_ < nc; ++nCanDo_) {
        if (!st_.canDoReady[nCanDo_].load(std::memory_order_acquire)) break;
        trace("#%d MPC asked canDo(\"%s\"): we said %d", I, st_.canDo[nCanDo_], st_.canDoAnswer[nCanDo_].load());
    }

    // parameters
    for (int i = 0; i < P_COUNT; ++i) {
        const float n = params_.get(i);
        if (n == norm_[i]) continue;
        norm_[i] = n;
        const int32_t tid = st_.paramTid.load();
        trace("#%d param %-10s = %-8s (%.4f, set on %s)", I, PARAM_INFO[i].key, params_.display(i).c_str(),
              static_cast<double>(n), st_.isAudioTid(tid) ? "an audio thread" : "another thread");
    }

    // a heartbeat while running
    if (running_ && now - beatMs_ >= 10000) {
        const double secs = (now - beatMs_) / 1000.0;
        beatMs_ = now;
        // What MPC polls, per second since the last heartbeat (the busiest first).
        std::string polled;
        int order[kOps];
        int n = 0;
        for (int op = 0; op < kOps; ++op)
            if (chatty(op) && opCount_[op] != opCountAtBeat_[op]) order[n++] = op;
        std::sort(order, order + n, [this](int a, int b) {
            return opCount_[a] - opCountAtBeat_[a] > opCount_[b] - opCountAtBeat_[b];
        });
        for (int k = 0; k < n; ++k) {
            const int op = order[k];
            const char* name = vst::opcodeName(op);
            char b[64];
            std::snprintf(b, sizeof b, "%s%s %.0f", k ? ", " : "", name ? name + 3 : "?",   // without "eff"
                          (opCount_[op] - opCountAtBeat_[op]) / secs);
            polled += b;
        }
        std::copy(opCount_, opCount_ + kOps, opCountAtBeat_);
        trace("#%d running: %.0f calls/s, blocks %d..%d, in %.1f dB, out %.1f dB, CPU %.2f%%, in place %s; polled/s: %s",
              I, static_cast<double>(st_.callsPerSec.load()), mn, mx, static_cast<double>(st_.inDb.load()),
              static_cast<double>(st_.outDb.load()), static_cast<double>(st_.cpuPct.load()), yesNo(ip),
              polled.empty() ? "-" : polled.c_str());
    }
}

} // namespace ef
