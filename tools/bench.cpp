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

// The performance layer at its busiest (docs/DESIGN.md "Performance: scenes and the looper"): scene 2
// switches four modules in as sends and moves a dozen parameters; the looper rolls 1/16 of a bar; the
// fader sweeps from A to B and back every 2 s, so every chunk works the morph.
const char* kPerformState =
    "effectforce 1\nlp_on=On\nscene_b=2\nscene2.lp_mix=1\nscene2.lp_rep=1/16\nscene2.lp_speed=0.75\n"
    "scene2.flt_on=On\nscene2.flt_cut=300\nscene2.flt_res=0.6\nscene2.dly_on=On\nscene2.dly_fb=0.8\n"
    "scene2.rev_on=On\nscene2.rev_mix=0.7\nscene2.rev_freeze=On\nscene2.drv_on=On\nscene2.drv_amt=30\n"
    "scene2.chr_mix=0.9\nscene2.phs_depth=1\nscene2.mac_1=1\nscene2.in_gain=-6\nscene2.mix=0.8\n";

// Scene moves at their busiest (docs/DESIGN.md "Scene moves"): the fader at B, scene 2 moving 14 locks
// (7 on a log curve) as a Loop over a beat, so every chunk works every one of them out again.
const char* kMovesState =
    "effectforce 1\nxfade=1\nscene_b=2\nscene2.move=1 beat\nscene2.play=Loop\n"
    "scene2.flt_on=On\nscene2.flt_cut=18000>200\nscene2.flt_res=0.1>0.7\nscene2.eq_on=On\nscene2.eq_mf=300>3000\n"
    "scene2.dly_on=On\nscene2.dly_lc=50>800\nscene2.dly_hc=12000>2000\nscene2.dly_fb=0.2>0.7\nscene2.rev_on=On\n"
    "scene2.rev_damp=15000>2000\nscene2.rev_decay=1>8\nscene2.rev_mix=0.1>0.6\nscene2.chr_on=On\nscene2.chr_rate=0.2>4\n"
    "scene2.phs_on=On\nscene2.phs_center=0.1>0.9\nscene2.mac_1=0>1\nscene2.in_gain=0>-6\nscene2.mix=0.6>1\n";

enum Mode { kPlain, kPerform, kMoves, kHeld };   // kHeld: kMovesState with LENGTH Off, the move's reference

Result runCase(void* lib, int seconds, const char* name, int module, bool all, int mode = kPlain) {
    const bool perform = mode == kPerform;
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
    if (mode != kPlain) {   // a project's state: it changes only what it lists
        std::string state = perform ? kPerformState : kMovesState;
        if (mode == kHeld) state.replace(state.find("move=1 beat"), 11, "move=Off");
        e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(state.size()), state.data(), 0.0f);
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
    // Performing: the looper records a bar first. Moving: the move is under way (it starts on a beat).
    const int warm = perform ? 3 * 44100 / kBlock : mode >= kMoves ? 44100 / kBlock : 64;
    for (int b = 0; b < warm; ++b) {   // the patch settles, buffers warm up
        fill();
        e->processReplacing(e, io, io, kBlock);
        g_time.ppqPos += kBlock / 44100.0 * g_time.tempo / 60.0;
    }
    const int blocks = seconds * 44100 / kBlock;
    std::vector<double> t(static_cast<size_t>(blocks));
    for (int b = 0; b < blocks; ++b) {
        fill();
        if (perform) {   // the fader: a triangle, A -> B -> A every 2 s (689 blocks)
            const double ph = std::fmod(b / 689.0, 1.0);
            e->setParameter(e, P_XFADE, static_cast<float>(ph < 0.5 ? 2.0 * ph : 2.0 - 2.0 * ph));
        }
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
    check(runCase(lib, seconds, "looper rolling, fader sweeping", -1, false, kPerform));
    check(runCase(lib, seconds, "all that, everything on heaviest", -1, true, kPerform));
    check(runCase(lib, seconds, "scene holding 14 locks still", -1, false, kHeld));
    check(runCase(lib, seconds, "scene moving 14 locks, every chunk", -1, false, kMoves));
    check(runCase(lib, seconds, "that, everything on heaviest", -1, true, kMoves));
    dlclose(lib);
    return fail ? 1 : 0;
}
