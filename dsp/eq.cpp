#include "eq.h"

namespace ef {

namespace {

constexpr float kSqrt2 = 1.414213562f;   // k at Q 0.707: Butterworth cuts, shelves without overshoot
constexpr float kMaxHz = 0.45f * kRate;

bool sameParams(const Eq::Params& a, const Eq::Params& b) {
    return a.lowCutHz == b.lowCutHz && a.lowFreq == b.lowFreq && a.lowGainDb == b.lowGainDb && a.midFreq == b.midFreq &&
           a.midGainDb == b.midGainDb && a.midQ == b.midQ && a.highFreq == b.highFreq && a.highGainDb == b.highGainDb &&
           a.highCutHz == b.highCutHz;
}

// 10^(dB / 40): libm, per chunk. Exactly 1 at 0 dB, so a flat band's mix is exactly (1, 0, 0).
float shelfA(float db) { return std::pow(10.0f, db * 0.025f); }

// A band's values in Eq's order: g, k, the mix.
void put(f2* v, float g, float k, float m0, float m1, float m2) {
    v[0] = splat2(g);
    v[1] = splat2(k);
    v[2] = splat2(m0);
    v[3] = splat2(m1);
    v[4] = splat2(m2);
}

} // namespace

Eq::Eq() { reset(); }

void Eq::reset() {
    for (Band& b : band_) {
        b.s.clear();
        b.on = false;
    }
    fresh_ = true;
    tail_ = 0;
}

void Eq::set(const Params& in, const Transport&) {
    Params p;
    p.lowCutHz = clampParam(in.lowCutHz, 20.0f, 1000.0f, 20.0f);
    p.lowFreq = clampParam(in.lowFreq, 30.0f, 500.0f, 100.0f);
    p.lowGainDb = clampParam(in.lowGainDb, -18.0f, 18.0f, 0.0f);
    p.midFreq = clampParam(in.midFreq, 100.0f, 10000.0f, 1000.0f);
    p.midGainDb = clampParam(in.midGainDb, -18.0f, 18.0f, 0.0f);
    p.midQ = clampParam(in.midQ, 0.3f, 8.0f, 1.0f);
    p.highFreq = clampParam(in.highFreq, 1000.0f, 16000.0f, 6000.0f);
    p.highGainDb = clampParam(in.highGainDb, -18.0f, 18.0f, 0.0f);
    p.highCutHz = clampParam(in.highCutHz, 1000.0f, 20000.0f, 20000.0f);
    if (!fresh_ && sameParams(p, last_)) return;   // glides under way go on
    last_ = p;

    f2 t[kBands][kValues];
    // Off at the ends of their ranges, with a little room: a value worked out by the fast exp2 (the
    // modulation, a scene's move) may land a hair inside them.
    const bool lowCut = p.lowCutHz > 20.01f, highCut = p.highCutHz < 19990.0f;
    put(t[LOW_CUT], svfG(p.lowCutHz), kSqrt2, 1.0f, lowCut ? -kSqrt2 : 0.0f, lowCut ? -1.0f : 0.0f);
    const float al = shelfA(p.lowGainDb);
    put(t[LOW_SHELF], svfG(p.lowFreq) / std::sqrt(al), kSqrt2, 1.0f, kSqrt2 * (al - 1.0f), al * al - 1.0f);
    const float am = shelfA(p.midGainDb), km = 1.0f / (p.midQ * am);
    put(t[BELL], svfG(p.midFreq), km, 1.0f, km * (am * am - 1.0f), 0.0f);
    const float ah = shelfA(p.highGainDb);
    put(t[HIGH_SHELF], svfG(p.highFreq) * std::sqrt(ah), kSqrt2, ah * ah, kSqrt2 * (1.0f - ah) * ah, 1.0f - ah * ah);
    put(t[HIGH_CUT], svfG(std::min(p.highCutHz, kMaxHz)), kSqrt2, highCut ? 0.0f : 1.0f, 0.0f, highCut ? 1.0f : 0.0f);

    // The ring of the slowest pole: the poles of s^2 + k s + 1 at the analog frequency w the band's
    // g stands for decay at w k / 2, or once they are real (k > 2, a wide deep cut) the slower one
    // at w (k / 2 - sqrt(k^2 / 4 - 1)). It has to fall 60 dB plus the band's gain either way (a
    // boost rings at the boosted level, a cut's state at the uncut one), and a chunk covers the
    // transient of the input stopping.
    tail_ = 0;
    const float gainDb[kBands] = {0.0f, p.lowGainDb, p.midGainDb, p.highGainDb, 0.0f};
    for (int i = 0; i < kBands; ++i) {
        if (passes(t[i])) continue;
        const double w = 2.0 * std::atan(static_cast<double>(t[i][G][0])) * kRate, h = 0.5 * t[i][K][0];
        const double decay = h < 1.0 ? w * h : w / (h + std::sqrt(h * h - 1.0));
        const double nepers = 6.9078 + 0.11513 * std::fabs(gainDb[i]);   // ln(1000), and dB * ln(10) / 20
        tail_ = std::max(tail_, static_cast<int>(nepers * kRate / decay) + kChunk);
    }

    for (int i = 0; i < kBands; ++i) {
        Band& b = band_[i];
        const f2* v = t[i];
        b.a = SvfUpdate::of(v[G], v[K]);
        if (fresh_) {
            b.s.clear();
            b.gl.jump(v);
            b.on = !passes(v);
        } else if (b.on) {
            if (!b.gl.aimsAt(v)) b.gl.to(v);   // another band's change leaves this glide alone
        } else if (!passes(v)) {
            // Waking: a silent state, the response fading in from the pass-through over the chunk.
            f2 from[kValues];
            for (int j = 0; j < kValues; ++j) from[j] = v[j];
            from[M] = splat2(1.0f);
            from[M + 1] = from[M + 2] = splat2(0.0f);
            b.s.clear();
            b.gl.jump(from);
            b.gl.to(v);
            b.on = true;
        }
    }
    fresh_ = false;
}

void Eq::process(float* L, float* R, int n) {
    bool first = true;   // only the first band that runs sees the raw input
    for (Band& b : band_) {
        if (!b.on) continue;
        if (first) run<true>(b, L, R, n);
        else run<false>(b, L, R, n);
        first = false;
        if (!b.gl.moving() && passes(b.gl.target())) b.on = false;   // landed on 0 dB / off
    }
}

template <bool Sanitize>
void Eq::run(Band& band, float* L, float* R, int n) {
    // Locals: the compiler can't keep members in registers across the stores to L and R.
    Glide<kValues, 1> g = band.gl;
    SvfState s = band.s;
    const auto sample = [&](int i, const f2* m, const SvfUpdate& a) {
        f2 x = load2(L + i, R + i);
        if (Sanitize) x = safeIn(x);
        store2(L + i, R + i, s.tick(x, a, m));
    };
    int i = 0;
    for (; i < n && g.moving(); ++i) {
        g.next();
        const f2* v = g.cur();
        sample(i, v + M, SvfUpdate::fast(v[G], v[K]));
    }
    if (i < n) {
        const f2 m[3] = {g.cur()[M], g.cur()[M + 1], g.cur()[M + 2]};
        const SvfUpdate a = band.a;
        for (; i < n; ++i) sample(i, m, a);
    }
    s.flushTiny();
    band.gl = g;
    band.s = s;
}

} // namespace ef
