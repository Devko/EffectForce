#pragma once
// The reference material and the loudness measure the factory presets are level-matched with
// (tools/levels.cpp sets their Output, test/preset_test.cpp checks they stay matched).
//
// The material: four bars at 120 BPM of chords (detuned saws), a bass line (square), kick, snare and
// hats, 8 s. Each category plays what it is made for (a lead preset with a low cut isn't judged on a
// kick drum): Synth and Pads the chords, Bass the bass line, Drums the drums, the rest all of it.
// Whatever the parts, the reference plays at kRefLufs with its peaks under -1 dBFS: the level a track
// reaches an insert at, so compressors and drives work as they would on it. Loudness: ITU-R BS.1770
// K-weighting (the shelf and the high-pass, as libebur128 derives them), both sides, the whole signal.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace efl {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRefLufs = -18.0;
constexpr double kRefPeak = 0.89;   // -1 dBFS

struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

inline Biquad kShelf() {
    const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    const double k = std::tan(kPi * f0 / kSr), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    return {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
            2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
}

inline Biquad kHighpass() {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / kSr), a0 = 1.0 + k / q + k * k;
    return {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
}

// K-weighted loudness of the whole stereo signal, LUFS.
inline double lufs(const std::vector<float>& L, const std::vector<float>& R) {
    Biquad s[2] = {kShelf(), kShelf()}, h[2] = {kHighpass(), kHighpass()};
    double sum = 0.0;
    for (size_t i = 0; i < L.size(); ++i) {
        const double l = h[0].run(s[0].run(L[i])), r = h[1].run(s[1].run(R[i]));
        sum += l * l + r * r;
    }
    return -0.691 + 10.0 * std::log10(std::max(sum / static_cast<double>(std::max<size_t>(L.size(), 1)), 1e-20));
}

enum Part : int { CHORDS = 1, BASS = 2, DRUMS = 4, ALL = 7 };

// The parts a factory category is matched on (its folder name without the number).
inline int partsFor(const std::string& category) {
    if (category == "Synth" || category == "Pads") return CHORDS;
    if (category == "Bass") return BASS;
    if (category == "Drums") return DRUMS;
    return ALL;
}

inline void referenceMix(std::vector<float>& L, std::vector<float>& R, int parts = ALL) {
    const size_t n = static_cast<size_t>(8.0 * kSr);
    L.assign(n, 0.0f);
    R.assign(n, 0.0f);
    const double beat = 0.5;   // 120 BPM
    const double chords[4][3] = {{220.0, 261.6, 329.6}, {174.6, 220.0, 261.6}, {196.0, 246.9, 293.7}, {164.8, 207.7, 246.9}};
    const double bass[4] = {55.0, 43.65, 49.0, 41.2};
    uint32_t seed = 1;
    auto noise = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(static_cast<int32_t>(seed)) / 2147483648.0;
    };
    double hatLp = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSr;
        const int bar = std::min(3, static_cast<int>(t / 2.0));
        const double inBar = t - bar * 2.0;
        const double env = std::min(1.0, inBar / 0.01) *
                           (inBar < 1.8 ? 0.7 + 0.3 * std::exp(-inBar * 4.0) : std::max(0.0, (2.0 - inBar) / 0.2));
        double cl = 0.0, cr = 0.0;
        for (int v = 0; v < 3; ++v) {
            const double f = chords[bar][v];
            cl += 2.0 * std::fmod(t * f * 1.003, 1.0) - 1.0;
            cr += 2.0 * std::fmod(t * f * 0.997 + 0.3, 1.0) - 1.0;
        }
        cl *= 0.06 * env;
        cr *= 0.06 * env;
        const double inBeat = std::fmod(t, beat);
        const int beatNo = static_cast<int>(t / beat);
        const double b = (std::fmod(t * bass[bar], 1.0) < 0.5 ? 0.12 : -0.12) * std::exp(-inBeat * 6.0);
        const double kick = 0.5 * std::sin(2.0 * kPi * (50.0 * inBeat + 70.0 / 25.0 * (1.0 - std::exp(-inBeat * 25.0)))) *
                            std::exp(-inBeat * 18.0);
        const double snare = beatNo % 2 ? 0.25 * noise() * std::exp(-inBeat * 30.0) : 0.0;
        const double eighth = std::fmod(t, beat / 2.0);
        const double hn = noise();
        hatLp += 0.5 * (hn - hatLp);
        const double hat = 0.06 * (hn - hatLp) * std::exp(-eighth * 120.0);
        const double c = parts & CHORDS ? 1.0 : 0.0, bs = parts & BASS ? 1.0 : 0.0, d = parts & DRUMS ? 1.0 : 0.0;
        L[i] = static_cast<float>(c * cl + bs * b + d * (kick + snare + hat * 1.2));
        R[i] = static_cast<float>(c * cr + bs * b + d * (kick + snare * 0.9 + hat));
    }
    double pk = 0.0;
    for (size_t i = 0; i < n; ++i) pk = std::max(pk, static_cast<double>(std::max(std::fabs(L[i]), std::fabs(R[i]))));
    const double g = std::min(std::pow(10.0, (kRefLufs - lufs(L, R)) / 20.0), kRefPeak / std::max(pk, 1e-9));
    for (size_t i = 0; i < n; ++i) {
        L[i] = static_cast<float>(L[i] * g);
        R[i] = static_cast<float>(R[i] * g);
    }
}

} // namespace efl
