// eflevels: the factory presets' levels. dlopen()s the built plugin like MPC, plays the reference mix
// (tools/loudness.h: chords, a bass line and drum hits, 8 s at 120 BPM) through every preset in
// presets/Factory and compares the loudness out with the loudness in (BS.1770 K-weighting, the whole
// mix, both sides). A preset is level-matched when its Output makes the two equal: an insert that changes the
// sound, not the level. `make preset-levels` writes each preset's out_gain; `make levels` reports.
//
//   eflevels <plugin.so> <presets/Factory> [--write] [-t tolerance dB]
//
// Hermetic: the plugin reads no user folders and saves nothing.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../plugin/vst2.h"
#include "loudness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
constexpr int kBlock = 128;
using efl::kSr;
// At most +-9 dB: a band-pass preset is matched on the whole reference, and more would make it far too
// hot on material inside its band (surface.py refuses more in a factory preset).
constexpr double kOutMin = -9.0, kOutMax = 9.0;

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == vst::audioMasterVersion) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

// Plays the mix through the plugin with `state` loaded; returns the output loudness and peak.
struct Played {
    double lufs, peak;
};
Played play(void* lib, const std::string& state, const std::vector<float>& inL, const std::vector<float>& inR) {
    auto entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(lib, "VSTPluginMain"));
    AEffect* e = entry ? entry(master) : nullptr;
    if (!e) {
        std::fprintf(stderr, "no VSTPluginMain, or it made no plugin\n");
        std::exit(1);
    }
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(state.size()), const_cast<char*>(state.data()), 0.0f);
    g_time.ppqPos = 0.0;
    std::vector<float> L = inL, R = inR;
    for (size_t pos = 0; pos < L.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, L.size() - pos));
        float* io[2] = {L.data() + pos, R.data() + pos};
        e->processReplacing(e, io, io, n);
        g_time.ppqPos += n / kSr * g_time.tempo / 60.0;
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    double pk = 0.0;
    for (size_t i = 0; i < L.size(); ++i) pk = std::max(pk, static_cast<double>(std::max(std::fabs(L[i]), std::fabs(R[i]))));
    return {efl::lufs(L, R), pk};
}

std::string withOutGain(const std::string& text, double db) {
    std::istringstream in(text);
    std::string line, out;
    bool done = false;
    char b[64];
    std::snprintf(b, sizeof b, "out_gain=%.1f", db);
    while (std::getline(in, line)) {
        if (line.compare(0, 9, "out_gain=") == 0) {
            if (!done) out += std::string(b) + "\n";
            done = true;
            continue;
        }
        out += line + "\n";
    }
    if (!done) out += std::string(b) + "\n";
    return out;
}

double currentOutGain(const std::string& text) {
    const size_t at = text.find("\nout_gain=");
    return at == std::string::npos ? 0.0 : std::atof(text.c_str() + at + 10);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <plugin.so> <presets/Factory> [--write] [-t dB]\n", argv[0]);
        return 2;
    }
    bool write = false;
    double tol = 1.0;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--write")) write = true;
        else if (!std::strcmp(argv[i], "-t") && i + 1 < argc) tol = std::atof(argv[++i]);
    }
    setenv("EF_PRESET_ROOTS", "/nonexistent-eflevels", 1);
    setenv("EF_DATA_DIR", "", 1);
    setenv("EF_FIXED_SEED", "1", 1);
    g_time.sampleRate = kSr;
    g_time.tempo = 120.0;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    std::vector<fs::path> files;
    for (const auto& d : fs::recursive_directory_iterator(argv[2]))
        if (d.path().extension() == ".efp") files.push_back(d.path());
    std::sort(files.begin(), files.end());
    int off = 0;
    for (const fs::path& f : files) {
        std::ifstream is(f);
        std::stringstream ss;
        ss << is.rdbuf();
        std::string text = ss.str();
        // The folder is the category: "03_Pads" -> "Pads".
        const std::string folder = f.parent_path().filename().string();
        const size_t us = folder.find('_');
        std::vector<float> inL, inR;
        efl::referenceMix(inL, inR, efl::partsFor(us == std::string::npos ? folder : folder.substr(us + 1)));
        const double ref = efl::lufs(inL, inR);
        double out = currentOutGain(text);
        Played p = play(lib, text, inL, inR);
        double diff = p.lufs - ref;
        if (write && std::fabs(diff) > 0.2) {
            // The output gain scales only the wet part when Mix is under 100%: a few rounds settle it.
            for (int round = 0; round < 6 && std::fabs(diff) > 0.1; ++round) {
                out = std::clamp(std::round((out - diff) * 10.0) / 10.0, kOutMin, kOutMax);
                text = withOutGain(text, out);
                p = play(lib, text, inL, inR);
                diff = p.lufs - ref;
            }
            std::ofstream(f, std::ios::binary) << text;
        }
        const bool capped = std::fabs(out) >= kOutMax - 0.05;   // as close as the cap allows
        const bool bad = std::fabs(diff) > tol && !capped;
        off += bad;
        std::printf("  %-44s %+5.1f dB  out_gain %+5.1f  peak %5.2f%s\n", f.lexically_relative(argv[2]).string().c_str(),
                    diff, out, p.peak, bad ? "  <-- not matched" : capped ? "  (capped)" : "");
    }
    dlclose(lib);
    std::printf("%zu presets, %d outside +-%.1f dB\n", files.size(), off, tol);
    return off ? 1 : 0;
}
