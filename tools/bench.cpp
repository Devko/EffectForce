// CPU bench for the built plugin: dlopen()s the .so like MPC does, feeds noise at 44.1 kHz in
// 128-frame blocks, in place, and times every processReplacing call with the thread's own CPU clock.
// Same verdict rule as sd88me's tools/bench.sh (percent of the 2902 us block): PASS p99 <= 15% and
// max <= 50%, WARN p99 <= 35% and max <= 80%, else FAIL.
//
//   efbench <plugin.so> [-s seconds] [-c cpu]
//
// Cases: everything off; each module alone with busy settings (compare them with the budgets in
// docs/DESIGN.md; the verdict is the block's, below); then every module on at its heaviest (Drive 2x oversampled, OTT,
// Phaser 12, the gate, the densest pitched grain cloud, ping-pong delay with wow and ducking, the Space
// reverb fully modulated with shimmer) with all eight
// matrix slots moving targets from both LFOs and the envelope: the worst a preset can reach.
// Hermetic: the plugin reads no user folders and saves nothing.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <sched.h>
#include <string>
#include <vector>

namespace {

using namespace ef;

constexpr int kBlock = 128;
constexpr double kBudgetUs = kBlock * 1e6 / 44100.0;   // 2902 us

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == vst::audioMasterVersion) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

double cpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

float norm(int id, float v) {
    const ParamSpec& s = PARAM_SPECS[id];
    float n = 0.0f;
    if (s.curve == Curve::Log) n = std::log(v / s.lo) / std::log(s.hi / s.lo);
    else if (s.curve == Curve::Pow) n = s.hi > 0.0f ? std::cbrt(v / s.hi) : 0.0f;
    else n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f;
    return std::clamp(n, 0.0f, 1.0f);
}

int option(int id, const char* name) {
    for (int o = 0; o < PARAM_INFO[id].nopts; ++o)
        if (!std::strcmp(PARAM_INFO[id].opts[o], name)) return o;
    std::fprintf(stderr, "%s has no option %s\n", PARAM_INFO[id].key, name);
    std::exit(2);
}

struct Setter {
    AEffect* e;
    void operator()(int id, float v) const { e->setParameter(e, id, norm(id, v)); }
    void opt(int id, const char* name) const { (*this)(id, static_cast<float>(option(id, name))); }
};

// Busy settings for one module (its on switch included).
void busy(const Setter& set, int module, bool heaviest) {
    switch (module) {
        case M_DRIVE:
            set.opt(P_DRV_ON, "On");
            set.opt(P_DRV_TYPE, heaviest ? "Fold" : "Tube");
            set(P_DRV_AMT, 24.0f);
            set(P_DRV_TONE, 0.3f);
            set(P_DRV_BIAS, 0.4f);
            set(P_DRV_MIX, 0.8f);
            break;
        case M_FILTER:
            set.opt(P_FLT_ON, "On");
            set.opt(P_FLT_TYPE, "LP 24");
            set(P_FLT_CUT, 1200.0f);
            set(P_FLT_RES, 0.7f);
            set(P_FLT_DRIVE, 0.5f);
            set(P_FLT_SPREAD, 0.5f);
            break;
        case M_EQ:
            set.opt(P_EQ_ON, "On");
            set(P_EQ_LC, 60.0f);
            set(P_EQ_LG, 4.0f);
            set(P_EQ_MG, -5.0f);
            set(P_EQ_HG, 3.0f);
            set(P_EQ_HC, 15000.0f);
            break;
        case M_COMP:
            set.opt(P_CMP_ON, "On");
            set.opt(P_CMP_MODE, heaviest ? "OTT" : "Comp");
            set(P_CMP_THR, -24.0f);
            set(P_CMP_SC, 120.0f);
            set(P_OTT_DEPTH, 0.8f);
            break;
        case M_CHORUS:
            set.opt(P_CHR_ON, "On");
            set.opt(P_CHR_MODE, "Ensemble");
            set(P_CHR_DEPTH, 0.8f);
            break;
        case M_PHASER:
            set.opt(P_PHS_ON, "On");
            set.opt(P_PHS_MODE, heaviest ? "Phaser 12" : "Phaser 8");
            set(P_PHS_RATE, 2.0f);
            set(P_PHS_FB, 0.7f);
            break;
        case M_PULSE:
            set.opt(P_PLS_ON, "On");
            set.opt(P_PLS_MODE, heaviest ? "Gate" : "Tremolo");
            set(P_PLS_DEPTH, 0.8f);
            break;
        case M_GRAIN:
            set.opt(P_GRN_ON, "On");
            set.opt(P_GRN_MODE, "Cloud");
            set(P_GRN_DENSITY, 1.0f);
            set(P_GRN_PITCH, 12.0f);
            set(P_GRN_SPREAD, 0.8f);
            set(P_GRN_FB, heaviest ? 0.5f : 0.0f);
            break;
        case M_DELAY:
            set.opt(P_DLY_ON, "On");
            set.opt(P_DLY_MODE, "Ping-Pong");
            set(P_DLY_FB, 0.7f);
            set(P_DLY_WOW, 0.6f);
            set(P_DLY_DRIVE, 0.5f);
            set(P_DLY_DUCK, 0.5f);
            set(P_DLY_SPREAD, 0.2f);
            break;
        case M_REVERB:
            set.opt(P_REV_ON, "On");
            set.opt(P_REV_MODE, heaviest ? "Space" : "Hall");
            set(P_REV_DECAY, 6.0f);
            set(P_REV_MOD, 1.0f);
            set(P_REV_SIZE, 0.8f);
            set(P_REV_SHIM, heaviest ? 0.6f : 0.0f);
            break;
        default: break;
    }
}

// All eight slots busy: both LFOs and the envelope on targets across the chain.
void matrix(const Setter& set) {
    const struct { const char* src; const char* dst; float amt; } slots[kNumModSlots] = {
        {"LFO 1", "Filter Cutoff", 0.4f}, {"LFO 2", "Phaser Center", 0.3f}, {"Envelope", "Delay Mix", -0.3f},
        {"LFO 1", "Reverb Damp", 0.2f}, {"Macro 1", "Drive Amount", 0.5f}, {"LFO 2", "Chorus Depth", 0.3f},
        {"Envelope", "Comp Thresh", 0.2f}, {"LFO 1", "Delay Time", 0.05f},
    };
    set(P_L1_RATE, 3.0f);
    set.opt(P_L2_WAVE, "Smooth");
    set(P_MAC_1, 0.5f);
    const int stride = P_M2_SRC - P_M1_SRC;
    for (int k = 0; k < kNumModSlots; ++k) {
        set.opt(P_M1_SRC + k * stride, slots[k].src);
        set.opt(P_M1_DST + k * stride, slots[k].dst);
        set(P_M1_AMT + k * stride, slots[k].amt);
    }
}

struct Result { double avg, p99, max; };

Result runCase(void* lib, int seconds, const char* name, int module, bool all) {
    auto entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(lib, "VSTPluginMain"));
    AEffect* e = entry ? entry(master) : nullptr;
    if (!e) {
        std::fprintf(stderr, "no VSTPluginMain, or it made no plugin\n");
        std::exit(1);
    }
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    const Setter set{e};
    if (all) {
        for (int m = 0; m < kNumModules; ++m) busy(set, m, true);
        matrix(set);
    } else if (module >= 0) {
        busy(set, module, false);
    }
    std::vector<float> L(kBlock), R(kBlock);
    float* io[2] = {L.data(), R.data()};
    uint32_t seed = 12345u;
    auto fill = [&] {   // noise around -12 dBFS, a little different per side
        for (int i = 0; i < kBlock; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float x = static_cast<float>(static_cast<int32_t>(seed)) * (0.25f / 2147483648.0f);
            L[static_cast<size_t>(i)] = x;
            R[static_cast<size_t>(i)] = 0.7f * x + 0.05f;
        }
    };
    for (int b = 0; b < 64; ++b) {   // the patch settles, buffers warm up
        fill();
        e->processReplacing(e, io, io, kBlock);
    }
    const int blocks = seconds * 44100 / kBlock;
    std::vector<double> t(static_cast<size_t>(blocks));
    for (int b = 0; b < blocks; ++b) {
        fill();
        const double t0 = cpuUs();
        e->processReplacing(e, io, io, kBlock);
        t[static_cast<size_t>(b)] = cpuUs() - t0;
        g_time.ppqPos += kBlock / 44100.0 * g_time.tempo / 60.0;
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    std::vector<double> s = t;
    std::sort(s.begin(), s.end());
    double sum = 0.0;
    for (double v : t) sum += v;
    const Result r{100.0 * sum / blocks / kBudgetUs, 100.0 * s[static_cast<size_t>(blocks * 0.99)] / kBudgetUs,
                   100.0 * s.back() / kBudgetUs};
    const char* verdict = r.p99 <= 15.0 && r.max <= 50.0 ? "PASS" : r.p99 <= 35.0 && r.max <= 80.0 ? "WARN" : "FAIL";
    std::printf("  %-34s avg %5.2f%%  p99 %5.2f%%  max %5.2f%%  %s\n", name, r.avg, r.p99, r.max, verdict);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.so> [-s seconds] [-c cpu]\n", argv[0]);
        return 2;
    }
    int seconds = 3, cpu = 1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "-s")) seconds = std::max(1, std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "-c")) cpu = std::atoi(argv[i + 1]);
    }
    if (cpu >= 0) {   // -c -1: don't pin (x86 runs, CI)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0) std::printf("(could not pin to cpu %d)\n", cpu);
    }
    // Hermetic: no user folders, nothing saved.
    setenv("EF_PRESET_ROOTS", "/nonexistent-efbench", 1);
    setenv("EF_DATA_DIR", "", 1);
    g_time.sampleRate = 44100.0;
    g_time.tempo = 120.0;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    std::printf("%s, %d s per case\n", argv[1], seconds);
    bool fail = false;
    const auto check = [&fail](const Result& r) { fail = fail || !(r.p99 <= 35.0 && r.max <= 80.0); };
    check(runCase(lib, seconds, "everything off", -1, false));
    for (int m = 0; m < kNumModules; ++m) {
        const std::string name = std::string(PARAM_INFO[P_ORDER_1].opts[m]) + " alone";
        check(runCase(lib, seconds, name.c_str(), m, false));
    }
    check(runCase(lib, seconds, "everything on, heaviest, 8 mod slots", -1, true));
    dlclose(lib);
    return fail ? 1 : 0;
}
