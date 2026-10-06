// The profile-guided build's trainer (make arm-plugin with PGO): an instrumented copy of the plugin
// is linked in and plays the reference mix (tools/loudness.h) through VSTPluginMain under qemu-arm,
// the way MPC drives an insert: every module on its own through every one of its options (types,
// modes, sync), then everything at once with the matrix busy, then every factory preset. The
// modules' options are found from the parameter table (a module's keys share its on switch's
// prefix), so a new module is trained without touching this file. The profile only steers the
// compiler (which paths are hot); what the trainer leaves out is still optimised as usual
// (-fprofile-partial-training).
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "fx_library.h"
#include "loudness.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

using namespace ef;

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == vst::audioMasterVersion) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

std::vector<float> g_L, g_R;   // the reference mix
size_t g_at = 0;

// Plays `seconds` of the reference mix through the plugin, in place, 128 frames at a time.
void play(AEffect* e, double seconds) {
    float L[128], R[128];
    float* io[2] = {L, R};
    const int blocks = static_cast<int>(seconds * 44100.0 / 128.0);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < 128; ++i) {
            L[i] = g_L[g_at];
            R[i] = g_R[g_at];
            g_at = (g_at + 1) % g_L.size();
        }
        e->processReplacing(e, io, io, 128);
        g_time.ppqPos += 128.0 / 44100.0 * g_time.tempo / 60.0;
    }
}

AEffect* fresh() {
    AEffect* e = VSTPluginMain(master);
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    return e;
}

void close(AEffect* e) { e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f); }

void option(AEffect* e, int id, int o) {
    const int n = PARAM_INFO[id].nopts;
    e->setParameter(e, id, n > 1 ? static_cast<float>(o) / static_cast<float>(n - 1) : 0.0f);
}

std::string prefixOf(const char* key) {
    const char* u = std::strchr(key, '_');
    return u ? std::string(key, static_cast<size_t>(u - key + 1)) : std::string();
}

} // namespace

int main() {
    g_time.sampleRate = 44100.0;
    g_time.tempo = 120.0;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;
    efl::referenceMix(g_L, g_R);
    int runs = 0;

    // Each module alone, through every option of every choice it has, at busy settings in between.
    for (int m = 0; m < kNumModules; ++m) {
        AEffect* e = fresh();
        const int on = kModuleOnParam[m];
        option(e, on, 1);
        const std::string pre = prefixOf(PARAM_INFO[on].key);
        for (int id = 0; id < P_COUNT; ++id) {
            const bool mine = prefixOf(PARAM_INFO[id].key) == pre || (pre == "cmp_" && prefixOf(PARAM_INFO[id].key) == "ott_");
            if (!mine || id == on || PARAM_INFO[id].kind != Kind::Synth) continue;
            if (PARAM_INFO[id].nopts > 0) {
                for (int o = 0; o < PARAM_INFO[id].nopts; ++o) {
                    option(e, id, o);
                    play(e, 0.12);
                    ++runs;
                }
                option(e, id, 0);
            } else {
                e->setParameter(e, id, 0.7f);   // away from the default: the paths a turned knob takes
                play(e, 0.05);
            }
        }
        play(e, 0.3);
        close(e);
    }

    // Everything on, the matrix busy: both LFOs, the envelope and the macros on targets across the rack.
    {
        AEffect* e = fresh();
        for (int m = 0; m < kNumModules; ++m) option(e, kModuleOnParam[m], 1);
        const int stride = P_M2_SRC - P_M1_SRC;
        for (int k = 0; k < kNumModSlots; ++k) {
            option(e, P_M1_SRC + k * stride, 1 + k % (kNumModSources - 1));
            option(e, P_M1_DST + k * stride, 1 + (k * 5) % (kNumModTargets - 1));
            e->setParameter(e, P_M1_AMT + k * stride, k % 2 ? 0.7f : 0.3f);
        }
        play(e, 2.0);
        ++runs;
        close(e);
    }

    // The performance layer: each Perform preset, the looper recording a bar, then the fader swept from
    // A to B and back (the morph, the sends, the looper's grab, rolls and speeds); Perform Mixer through
    // every scene it has.
    for (int i = 0; i < kNumFactoryPresets; ++i) {
        if (std::strcmp(kFactoryPresets[i].category, "Perform") != 0) continue;
        AEffect* e = fresh();
        const std::string text = kFactoryPresets[i].text;
        e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(text.size()), const_cast<char*>(text.data()), 0.0f);
        play(e, 2.2);
        option(e, P_LP_CAPTURE, 0);   // REC, Capture Last: a kept loop for the scenes that play it
        e->setParameter(e, P_LP_REC, 1.0f);
        play(e, 0.1);
        const bool mixer = !std::strcmp(kFactoryPresets[i].name, "Perform Mixer");
        for (int sc = 1; sc < (mixer ? kNumScenes : 2); ++sc) {
            option(e, P_SCENE_B, sc);
            for (int k = 0; k <= 20; ++k) {
                e->setParameter(e, P_XFADE, k <= 10 ? k / 10.0f : (20 - k) / 10.0f);
                play(e, 0.05);
            }
        }
        close(e);
        ++runs;
    }

    // Scene moves (docs/DESIGN.md "Scene moves"): each effect of the FX library that moves, as scene 2,
    // its move under way at B.
    for (int k = 0; k < kNumFx; ++k) {
        const std::string fx = kFxLibrary[k].text;
        if (fx.find('>') == std::string::npos) continue;
        std::string state = "effectforce 1\nlp_on=On\nscene_b=2\n";
        for (size_t at = fx.find('\n') + 1; at < fx.size();) {   // its lines, past the header, as scene 2's
            const size_t end = std::min(fx.find('\n', at), fx.size());
            state += "scene2." + fx.substr(at, end - at) + "\n";
            at = end + 1;
        }
        AEffect* e = fresh();
        e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(state.size()), const_cast<char*>(state.data()), 0.0f);
        play(e, 2.0);
        e->setParameter(e, P_XFADE, 1.0f);
        play(e, 2.5);
        close(e);
        ++runs;
    }

    // The factory presets, as users will mostly play them, each on a fresh instance.
    for (int i = 0; i < kNumFactoryPresets; ++i) {
        AEffect* e = fresh();
        const std::string text = kFactoryPresets[i].text;
        e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(text.size()), const_cast<char*>(text.data()), 0.0f);
        play(e, 0.5);
        close(e);
        ++runs;
    }
    std::printf("pgo trainer: %d runs\n", runs);
    return 0;
}
