#pragma once
// Signals and measurements for the module tests: generators, levels, a single-frequency magnitude
// (Goertzel), and running a module over a buffer in control chunks the way the rack does.
#include "check.h"
#include "../dsp/common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace eft {

using Buf = std::vector<float>;
constexpr double kPi = 3.14159265358979323846;

inline Buf sine(double hz, int n, float amp = 0.5f, double phase = 0.0) {
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * kPi * hz * i / ef::kRate + phase));
    return x;
}
inline Buf whiteNoise(int n, float amp = 0.5f, uint32_t seed = 1) {
    Buf x(static_cast<size_t>(n));
    for (float& s : x) {
        seed = seed * 1664525u + 1013904223u;
        s = amp * static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f;
    }
    return x;
}
inline Buf impulseAt(int n, int at, float v = 1.0f) {
    Buf x(static_cast<size_t>(n), 0.0f);
    x[static_cast<size_t>(at)] = v;
    return x;
}

inline double rms(const Buf& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    double s = 0.0;
    for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
    return to > from ? std::sqrt(s / static_cast<double>(to - from)) : 0.0;
}
inline double db(double v) { return v > 1e-12 ? 20.0 * std::log10(v) : -240.0; }
inline float peak(const Buf& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    float m = 0.0f;
    for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(x[i]));
    return m;
}
inline bool allFinite(const Buf& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}
// The largest jump between neighbouring samples: a click detector for smooth test signals.
inline float maxStep(const Buf& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

// Amplitude of the `hz` component of x[from..to) (Goertzel, Hann-windowed), as a sine's peak.
inline double magnitude(const Buf& x, double hz, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    const size_t n = to - from;
    const double w = 2.0 * kPi * hz / ef::kRate, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        wsum += win;
        const double s0 = x[from + i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}
// The gain in dB a module applies to a sine at hz (after `settle` samples of settling).
template <class M>
double gainAt(M& m, const typename M::Params& p, double hz, int settle = 8192, int len = 16384);

// Runs a module over L / R in chunks of `chunk` samples (set() before each), as the rack does.
// The transport advances while playing.
template <class M>
void run(M& m, const typename M::Params& p, Buf& L, Buf& R, ef::Transport t = {}, int chunk = ef::kChunk) {
    for (size_t pos = 0; pos < L.size(); pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), L.size() - pos));
        m.set(p, t);
        m.process(&L[pos], &R[pos], n);
        if (t.playing) t.beats += n / static_cast<double>(ef::kRate) * t.bpm / 60.0;
    }
}

template <class M>
double gainAt(M& m, const typename M::Params& p, double hz, int settle, int len) {
    m.reset();
    Buf L = sine(hz, settle + len, 0.25f), R = L;
    const Buf in = L;
    run(m, p, L, R);
    return db(magnitude(L, hz, static_cast<size_t>(settle)) / magnitude(in, hz, static_cast<size_t>(settle)));
}

} // namespace eft
