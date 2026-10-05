#include "filter.h"

namespace ef {

namespace {

constexpr float kQ0 = 0.707106781f;    // Butterworth: no peak at res 0
constexpr float kQRange = 22.627417f;  // Q at res 1 is kQ0 * this = 16 (+24 dB)
constexpr float kK1 = 1.847759065f;    // 2 cos(pi / 8): the 4th-order Butterworth's low-Q stage, Q 0.541
constexpr float kK2 = 0.765366865f;    // 2 cos(3 pi / 8): its other stage, Q 1.307
constexpr float kMinHz = 10.0f;        // 20 Hz with an octave of spread is 14
constexpr float kMaxHz = 0.45f * kRate;
constexpr float kDriveDb = 24.0f;

bool sameParams(const Filter::Params& a, const Filter::Params& b) {
    return a.type == b.type && a.cutoffHz == b.cutoffHz && a.res == b.res && a.drive == b.drive && a.spread == b.spread &&
           a.mix == b.mix;
}

void mixOf(f2* m, float m0, float m1, float m2) {
    m[0] = splat2(m0);
    m[1] = splat2(m1);
    m[2] = splat2(m2);
}

} // namespace

Filter::Filter() { reset(); }

void Filter::reset() {
    s1_.clear();
    s2_.clear();
    fresh_ = true;
}

void Filter::set(const Params& in, const Transport&) {
    Params p;
    p.type = std::clamp(in.type, 0, kTypes - 1);
    p.cutoffHz = clampParam(in.cutoffHz, 20.0f, 20000.0f, 1000.0f);
    p.res = clampParam(in.res, 0.0f, 1.0f, 0.0f);
    p.drive = clampParam(in.drive, 0.0f, 1.0f, 0.0f);
    p.spread = clampParam(in.spread, -1.0f, 1.0f, 0.0f);
    p.mix = clampParam(in.mix, 0.0f, 1.0f, 1.0f);
    if (!fresh_ && sameParams(p, last_)) return;   // a glide under way goes on
    last_ = p;

    const float half = std::exp2(0.5f * p.spread);
    const float fl = clampf(p.cutoffHz / half, kMinHz, kMaxHz), fr = clampf(p.cutoffHz * half, kMinHz, kMaxHz);
    const float q = kQ0 * std::pow(kQRange, p.res);
    const bool steep = p.type == LP24 || p.type == HP24;
    const float k1 = steep ? kK1 : 1.0f / q;
    // Q2 = Q / 0.541. While the stage only passes through (kept warm for a switch to 24 dB), its
    // input is the 12 dB stage's resonant peak: at Q2 there it would store hundreds of times the
    // input and let it out at the switch. Butterworth's 1.307 until then.
    const float k2 = steep ? 1.0f / (q * kK1) : kK2;

    f2 t[kValues];
    t[G] = f2{svfG(fl), svfG(fr)};
    t[K1] = splat2(k1);
    t[K2] = splat2(k2);
    switch (p.type) {
        case LP12:
        case LP24: mixOf(t + M1, 0.0f, 0.0f, 1.0f); break;
        case HP12:
        case HP24: mixOf(t + M1, 1.0f, -k1, -1.0f); break;
        case BP: mixOf(t + M1, 0.0f, k1, 0.0f); break;
        default: mixOf(t + M1, 1.0f, -k1, 0.0f); break;
    }
    if (p.type == LP24) mixOf(t + M2, 0.0f, 0.0f, 1.0f);
    else if (p.type == HP24) mixOf(t + M2, 1.0f, -k2, -1.0f);
    else mixOf(t + M2, 1.0f, 0.0f, 0.0f);
    t[DRV_IN] = splat2(dbToGain(kDriveDb * p.drive));
    t[DRV_OUT] = splat2(dbToGain(-0.5f * kDriveDb * p.drive));
    t[DRV_BLEND] = splat2(std::min(1.0f, 8.0f * p.drive));
    t[MIX] = splat2(p.mix);
    a1_ = SvfUpdate::of(t[G], t[K1]);
    a2_ = SvfUpdate::of(t[G], t[K2]);

    // The ring of the sharpest stage down to -60 dB: ln(1000) * 2 Q / w at the lower cutoff.
    const float qMax = steep ? q * kK1 : q;
    tail_ = static_cast<int>(6.9078f * qMax * kRate / (kPi * std::min(fl, fr))) + 1;

    if (fresh_) {
        gl_.jump(t);
        fresh_ = false;
    } else {
        gl_.to(t);
    }
}

void Filter::process(float* L, float* R, int n) {
    if (gl_.cur()[DRV_BLEND][0] == 0.0f && gl_.target()[DRV_BLEND][0] == 0.0f) run<false>(L, R, n);
    else run<true>(L, R, n);
}

template <bool Drive>
void Filter::run(float* L, float* R, int n) {
    // Locals: the compiler can't keep members in registers across the stores to L and R.
    Glide<kValues, 1> g = gl_;
    SvfState s1 = s1_, s2 = s2_;
    const auto sample = [&](int i, const f2* v, const SvfUpdate& a1, const SvfUpdate& a2) {
        const f2 dry = safeIn(load2(L + i, R + i));
        f2 x = dry;
        if (Drive) x += v[DRV_BLEND] * (softclip2(x * v[DRV_IN]) * v[DRV_OUT] - x);
        x = s2.tick(s1.tick(x, a1, v + M1), a2, v + M2);
        store2(L + i, R + i, dry * (splat2(1.0f) - v[MIX]) + x * v[MIX]);   // exact at mix 0 and 1
    };
    int i = 0;
    for (; i < n && g.moving(); ++i) {
        g.next();
        const f2* v = g.cur();
        sample(i, v, SvfUpdate::fast(v[G], v[K1]), SvfUpdate::fast(v[G], v[K2]));
    }
    if (i < n) {
        f2 v[kValues];
        for (int j = 0; j < kValues; ++j) v[j] = g.cur()[j];
        const SvfUpdate a1 = a1_, a2 = a2_;
        for (; i < n; ++i) sample(i, v, a1, a2);
    }
    s1.flushTiny();
    s2.flushTiny();
    gl_ = g;
    s1_ = s1;
    s2_ = s2;
}

} // namespace ef
