#include "comp.h"

#include <algorithm>

namespace ef {

namespace {

using namespace cmp;

constexpr float kXoLowHz = 88.3f, kXoHighHz = 2500.0f;
constexpr float kMinKnee = 1e-4f;   // half a knee, log2 units: "no knee" without dividing by zero

// OTT per band (low, mid, high, and the spare lane as high); see comp.h.
constexpr float kOttUpDb[4] = {-28.0f, -34.0f, -28.0f, -28.0f};
constexpr float kOttDownDb[4] = {-20.0f, -22.0f, -22.0f, -22.0f};
constexpr float kOttAttackMs[4] = {47.8f, 22.4f, 13.5f, 13.5f};
constexpr float kOttReleaseMs[4] = {282.0f, 282.0f, 132.0f, 132.0f};
constexpr float kOttDownSlope = 1.0f - 1.0f / 66.7f;
constexpr float kOttUpSlope = 1.0f - 1.0f / 4.17f;
constexpr float kOttCapDb = 30.0f;        // the most an upward gain lifts
constexpr float kOttFloorDb = -90.0f;     // no lift below it ...
constexpr float kOttFloorSlope = 2.0f;    // ... and up to 2 dB more per dB above it
constexpr float kOttKneeDb = 6.0f;        // the downward knee's width

f4 lanesDb(const float (&db)[4]) { return f4{db[0], db[1], db[2], db[3]} * splat(kLog2PerDb); }

} // namespace

Comp::Comp() {
    xo1_ = Svf4::coefs(kXoLowHz);
    xo2_ = Svf4::coefs(kXoHighHz);
    for (int i = 0; i < kChunk; ++i) {
        detLevel_[i] = gain_[i] = fadeL_[i] = fadeR_[i] = 0.0f;
        bandL_[i] = bandR_[i] = peaks_[i] = levels_[i] = splat(0.0f);
    }
    reset();
}

void Comp::reset() {
    clearComp();
    clearOtt();
    fadeLeft_ = 0;
    fresh_ = true;
}

void Comp::clearComp() {
    peak_ = level_ = 0.0f;
    scf_.clear();
    scOn_ = false;
}

void Comp::clearOtt() {
    for (Svf4& s : xo_) s.clear();
    oPeak_ = oLevel_ = splat(0.0f);
}

void Comp::set(const Params& p, const Transport&) {
    const int mode = p.mode >= 1 ? 1 : 0;
    bool jump = fresh_;
    if (fresh_) {
        mode_ = mode;
        fadeLeft_ = 0;
        fresh_ = false;
    } else if (mode != mode_) {
        // The outgoing mode keeps its last settings for the fade; the incoming one starts
        // cleared (its state is from long ago, if any) and at its targets.
        from_ = mode_;
        mode_ = mode;
        fadeLeft_ = kChunk;
        if (mode == 0) clearComp();
        else clearOtt();
        jump = true;
    }
    if (mode_ == 0) setComp(p, jump);
    else setOtt(p, jump);
}

void Comp::setComp(const Params& p, bool jump) {
    const float v[kCompValues] = {
        param(p.thresholdDb, -60.0f, 0.0f) * kLog2PerDb,
        1.0f - 1.0f / param(p.ratio, 1.0f, 20.0f),
        std::max(param(p.kneeDb, 0.0f, 24.0f) * 0.5f * kLog2PerDb, kMinKnee),
        param(p.makeupDb, -12.0f, 24.0f) * kLog2PerDb,
        param(p.mix, 0.0f, 1.0f),
    };
    if (jump) comp_.jump(v);
    else comp_.to(v, kChunk);

    // The times only change coefficients: a jump can't click.
    const float att = param(p.attackMs, 0.1f, 100.0f), rel = param(p.releaseMs, 10.0f, 2000.0f);
    if (att != attackMs_) {
        attackMs_ = att;
        ca_ = smoothCoef(att * 0.001f);
    }
    if (rel != releaseMs_) {
        releaseMs_ = rel;
        cr_ = smoothCoef(rel * 0.001f);
    }

    const float sc = param(p.scLowCutHz, 20.0f, 500.0f);
    const bool on = sc > 20.0f;
    if (on && !scOn_) scf_.clear();   // from off: start from silence, not from a state of long ago
    scOn_ = on;
    if (on && sc != scHz_) {
        scHz_ = sc;
        sc_ = Svf2::coefs(sc);
    }
}

void Comp::setOtt(const Params& p, bool jump) {
    const float v[kOttValues] = {
        param(p.ottDepth, 0.0f, 1.0f),
        dbToGain(param(p.makeupDb, -12.0f, 24.0f)),
        param(p.ottUp, 0.0f, 1.0f) * kOttUpSlope,
        param(p.ottDown, 0.0f, 1.0f) * kOttDownSlope,
        param(p.ottLowDb, -12.0f, 12.0f) * kLog2PerDb,
        param(p.ottMidDb, -12.0f, 12.0f) * kLog2PerDb,
        param(p.ottHighDb, -12.0f, 12.0f) * kLog2PerDb,
    };
    if (jump) ott_.jump(v);
    else ott_.to(v, kChunk);

    const float time = param(p.ottTime, 0.1f, 10.0f);
    if (time != ottTime_) {
        ottTime_ = time;
        float a[4], r[4];
        for (int b = 0; b < 4; ++b) {
            a[b] = smoothCoef(kOttAttackMs[b] * time * 0.001f);
            r[b] = smoothCoef(kOttReleaseMs[b] * time * 0.001f);
        }
        oca_ = f4{a[0], a[1], a[2], a[3]};
        ocr_ = f4{r[0], r[1], r[2], r[3]};
    }
}

void Comp::process(float* L, float* R, int n) {
    for (; n > kChunk; n -= kChunk, L += kChunk, R += kChunk) process(L, R, kChunk);   // outside the contract
    if (n <= 0) return;
    if (fadeLeft_ == 0) {
        run(mode_, L, R, n);
        return;
    }
    std::copy(L, L + n, fadeL_);
    std::copy(R, R + n, fadeR_);
    run(from_, fadeL_, fadeR_, n);
    run(mode_, L, R, n);
    for (int i = 0; i < n && fadeLeft_ > 0; ++i, --fadeLeft_) {
        const float w = static_cast<float>(kChunk + 1 - fadeLeft_) * (1.0f / kChunk);   // 1/32 .. 1
        L[i] = fadeL_[i] + (L[i] - fadeL_[i]) * w;
        R[i] = fadeR_[i] + (R[i] - fadeR_[i]) * w;
    }
}

void Comp::run(int mode, float* L, float* R, int n) {
    if (mode == 1) runOtt(L, R, n);
    else if (scOn_) runComp<true>(L, R, n);
    else runComp<false>(L, R, n);
}

template <bool Sidechain>
void Comp::runComp(float* L, float* R, int n) {
    // Pass 1, what recurses: the input cleaned in place, the sidechain's low cut, the detector.
    // Locals: the compiler can't keep members in registers across the stores to L and R.
    Svf2 sc = scf_;
    sc.flush();
    const Svf2::Coefs c = sc_;
    const f2 k = splat2(kButterK), ca = splat2(ca_), cr = splat2(cr_), tiny = splat2(kTiny), zero = splat2(0.0f);
    f2 peak = splat2(peak_), level = splat2(level_);
    for (int i = 0; i < n; ++i) {
        const f2 x = clean2(load2(L + i, R + i));
        store2(x, L + i, R + i);
        f2 d = x;
        if (Sidechain) {
            f2 v1, v2;
            sc.tick(x, c, v1, v2);
            d = x - k * v1 - v2;
        }
        const f2 r = absMax2(d);
        // kTiny: once the input stops, both fall to exact zero (max) instead of into denormals.
        peak = max2(r, peak + cr * (r - peak) - tiny);
        level = max2(zero, level + ca * (peak - level) - tiny);
        detLevel_[i] = level[0];
    }
    if (Sidechain) scf_ = sc;
    peak_ = peak[0];
    level_ = level[0];

    // Pass 2, four samples at a time (a chunk's tail reads a few stale levels, harmlessly): the
    // gain computer, (1 - 1/ratio) x the level's excess over the threshold, a parabola across the
    // knee (h = half its width): 0 below thr - h, the excess above thr + h. Then makeup and mix.
    const Glide<kCompValues> g = comp_;
    const f4 len = splat(static_cast<float>(g.len)), zero4 = splat(0.0f), one = splat(1.0f), lvFloor = splat(kFloor);
    auto value = [&](int j, f4 at) { return pastEnd(at, len, splat(g.target[j]), splat(g.base[j]) + splat(g.step[j]) * at); };
    for (int i = 0; i < n; i += 4) {
        const f4 at = splat(static_cast<float>(g.done + i)) + f4{1.0f, 2.0f, 3.0f, 4.0f};
        const f4 h = value(KNEE, at);
        const f4 over = log2Fast4(max4(load4(detLevel_ + i), lvFloor)) - value(THR, at);
        const f4 u = min4(max4(over + h, zero4), h + h);
        const f4 gr = value(SLOPE, at) * (u * u * recip4<1>(splat(4.0f) * h) + max4(over - h, zero4));
        const f4 wet = exp2Fast4(value(MAKEUP, at) - gr);
        store4(gain_ + i, one + value(MIX, at) * (wet - one));   // dry + mix (wet - dry); mix 0: exactly 1
    }
    for (int i = 0; i < n; ++i) {
        L[i] *= gain_[i];
        R[i] *= gain_[i];
    }
    comp_.advance(n);
}

void Comp::runOtt(float* L, float* R, int n) {
    // Pass 1, what recurses: crossovers and detectors, sample by sample, the state in locals.
    Svf4 s0 = xo_[0], s1 = xo_[1], s2 = xo_[2], s3 = xo_[3];
    s0.flush();
    s1.flush();
    s2.flush();
    s3.flush();
    const Svf4::Coefs c1 = xo1_, c2 = xo2_;
    const f4 k = splat(kButterK), k2 = splat(2.0f * kButterK), zero = splat(0.0f), tiny = splat(kTiny);
    const f4 ca = oca_, cr = ocr_;
    f4 peak = oPeak_, level = oLevel_;
    for (int i = 0; i < n; ++i) {
        f4 v1, v2;
        // 88.3 Hz, first sections: lanes (L, R, -, -).
        const f4 x1 = widen(clean2(load2(L + i, R + i)));
        s0.tick(x1, c1, v1, v2);
        // Second sections: (LP L, LP R, HP L, HP R).
        const f4 x2 = lowHalves(v2, x1 - k * v1 - v2);
        s1.tick(x2, c1, v1, v2);
        // 2.5 kHz: the low band through its allpass; the upper part's first sections.
        const f4 x3 = lowHigh(v2, x2 - k * v1 - v2);   // (low L, low R, upper L, upper R)
        s2.tick(x3, c2, v1, v2);
        const f4 low = x3 - k2 * v1;
        const f4 x4 = highHalves(v2, x3 - k * v1 - v2);
        s3.tick(x4, c2, v1, v2);
        const f4 midHigh = lowHigh(v2, x4 - k * v1 - v2);   // (mid L, mid R, high L, high R)

        f4 bl, br;
        toChannels(low, midHigh, bl, br);
        // Each band's detector, linked across the channels: Comp's, in four lanes.
        const f4 r = max4(abs4(bl), abs4(br));
        peak = max4(r, peak + cr * (r - peak) - tiny);
        level = max4(zero, level + ca * (peak - level) - tiny);
        bandL_[i] = bl;
        bandR_[i] = br;
        peaks_[i] = peak;
        levels_[i] = level;
    }
    xo_[0] = s0;
    xo_[1] = s1;
    xo_[2] = s2;
    xo_[3] = s3;
    oPeak_ = peak;
    oLevel_ = level;

    // Pass 2: each band's gain computer, depth and makeup, and the bands' sum.
    const Glide<kOttValues> g = ott_;
    const f4 tUp = lanesDb(kOttUpDb), tDown = lanesDb(kOttDownDb);
    const f4 h = splat(0.5f * kOttKneeDb * kLog2PerDb), h2 = h + h, inv4h = splat(1.0f) / (splat(4.0f) * h);
    const f4 cap = splat(kOttCapDb * kLog2PerDb), floorLv = splat(kOttFloorDb * kLog2PerDb);
    const f4 floorSlope = splat(kOttFloorSlope), lvFloor = splat(kFloor);
    const f4 bandBase = f4{g.base[BAND], g.base[BAND + 1], g.base[BAND + 2], 0.0f};
    const f4 bandStep = f4{g.step[BAND], g.step[BAND + 1], g.step[BAND + 2], 0.0f};
    const f4 bandTarget = f4{g.target[BAND], g.target[BAND + 1], g.target[BAND + 2], 0.0f};
    for (int i = 0; i < n; ++i) {
        const int at = g.done + i + 1;
        const f4 bands = at >= g.len ? bandTarget : bandBase + bandStep * splat(static_cast<float>(at));
        const f4 lvDown = log2Fast4(max4(levels_[i], lvFloor));
        const f4 lvUp = log2Fast4(max4(peaks_[i], lvFloor));
        const f4 over = lvDown - tDown;
        const f4 u = min4(max4(over + h, zero), h2);
        const f4 down = splat(g.at(DOWN, at)) * (u * u * inv4h + max4(over - h, zero));
        const f4 lift = max4(zero, min4(min4(splat(g.at(UP, at)) * (tUp - lvUp), cap), floorSlope * (lvUp - floorLv)));
        const f4 wet = exp2Fast4(lift - down + bands);
        // Makeup x (1 - depth + depth x wet): the dry here is the bands' unprocessed sum.
        const float m = g.at(OMAKEUP, at), md = m * g.at(DEPTH, at);
        const f4 gain = splat(m - md) + splat(md) * wet;
        sum2(gain * bandL_[i], gain * bandR_[i], L + i, R + i);
    }
    ott_.advance(n);
}

} // namespace ef
