#include "delay.h"
#include "simd.h"

#include <cstring>

namespace ef {

namespace {

// L and R side by side in a two-lane vector: NEON's 64-bit registers on the device, GCC's generic
// vectors elsewhere (the x86 tests run the same arithmetic lane by lane). Comparisons and min / max
// stay in the vector unit: VFP's scalar compares stall on the transfer of their flags.
#if EF_NEON
using Lr = float32x2_t;
using LrMask = uint32x2_t;
#else
typedef float Lr __attribute__((vector_size(8)));
typedef int32_t LrMask __attribute__((vector_size(8)));
#endif

EF_INLINE Lr both(float x) { return Lr{x, x}; }

// Straight between memory and the lanes: built from two scalars, GCC goes through the stack.
EF_INLINE Lr loadLr(const float* l, const float* r) {
#if EF_NEON
    return vld1_lane_f32(r, vld1_dup_f32(l), 1);
#else
    return Lr{*l, *r};
#endif
}
EF_INLINE void storeLr(float* l, float* r, Lr v) {
#if EF_NEON
    vst1_lane_f32(l, v, 0);
    vst1_lane_f32(r, v, 1);
#else
    *l = v[0];
    *r = v[1];
#endif
}
EF_INLINE Lr pairLr(float l, float r) {
#if EF_NEON
    return vset_lane_f32(r, vdup_n_f32(l), 1);
#else
    return Lr{l, r};
#endif
}

EF_INLINE Lr minLr(Lr a, Lr b) {
#if EF_NEON
    return vmin_f32(a, b);
#else
    return a < b ? a : b;
#endif
}
EF_INLINE Lr maxLr(Lr a, Lr b) {
#if EF_NEON
    return vmax_f32(a, b);
#else
    return a > b ? a : b;
#endif
}
EF_INLINE Lr absLr(Lr a) {
#if EF_NEON
    return vabs_f32(a);
#else
    return a < Lr{} ? -a : a;
#endif
}
EF_INLINE Lr swapLr(Lr a) {   // (R, L)
#if EF_NEON
    return vrev64_f32(a);
#else
    return Lr{a[1], a[0]};
#endif
}
EF_INLINE Lr louderLr(Lr a) {   // the larger lane, in both
#if EF_NEON
    return vpmax_f32(a, a);
#else
    return maxLr(a, swapLr(a));
#endif
}
EF_INLINE LrMask greaterLr(Lr a, Lr b) {
#if EF_NEON
    return vcgt_f32(a, b);
#else
    return a > b;
#endif
}
EF_INLINE Lr pickLr(LrMask m, Lr yes, Lr no) {
#if EF_NEON
    return vbsl_f32(m, yes, no);
#else
    return m ? yes : no;
#endif
}
// 1 / d for d >= 1: NEON's estimate and one Newton-Raphson step (about 1e-5), no VFP divider.
EF_INLINE Lr recipLr(Lr d) {
#if EF_NEON
    const Lr e = vrecpe_f32(d);
    return vmul_f32(e, vrecps_f32(d, e));
#else
    return both(1.0f) / d;
#endif
}
// NaN and infinities become 0, by their bits (an exponent of all ones).
EF_INLINE Lr finiteLr(Lr x) {
#if EF_NEON
    const uint32x2_t bits = vand_u32(vreinterpret_u32_f32(x), vdup_n_u32(0x7fffffffu));
    return vbsl_f32(vclt_u32(bits, vdup_n_u32(0x7f800000u)), x, vdup_n_f32(0.0f));
#else
    LrMask bits;
    std::memcpy(&bits, &x, sizeof bits);
    return (bits & 0x7fffffff) < 0x7f800000 ? x : Lr{};
#endif
}
// common.h's hermite() for both lanes.
EF_INLINE Lr hermiteLr(Lr xm1, Lr x0, Lr x1, Lr x2, Lr t) {
    const Lr c1 = both(0.5f) * (x1 - xm1);
    const Lr c2 = xm1 - both(2.5f) * x0 + both(2.0f) * x1 - both(0.5f) * x2;
    const Lr c3 = both(0.5f) * (x2 - xm1) + both(1.5f) * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// A parameter clamped to its range; NaN (which no comparison catches) becomes `nan`.
template <class T>
T clampOr(T x, T lo, T hi, T nan) {
    return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan);
}

constexpr int kSeg = 32;                      // the glide, the wow and the duck gain step per segment
constexpr double kMaxTime = 8.0 * kRate;      // 1 bar at 30 BPM
constexpr double kMinTime = 0.001 * kRate;    // 1 ms
constexpr float kWowDepth = 0.003f * kRate;   // +-3 ms at wow 1
constexpr float kFlutter = 0.2f / 3.0f;       // the flutter's depth against the wow's: +-0.2 ms
constexpr float kWowHz = 0.5f, kFlutterHzL = 6.0f, kFlutterHzR = 6.6f;
constexpr float kWowLagR = 0.25f;             // cycles
// The lines: the longest time plus the wow's depth, and the cubic read's two samples on either side.
constexpr int kLen = static_cast<int>(kMaxTime) + static_cast<int>(kWowDepth * (1.0f + kFlutter)) + 8;
constexpr int kGuard = 3;                     // the first samples again past the end: a read never wraps
constexpr int kMaxAge = kLen - 3;             // the oldest whole delay a read may ask for
constexpr float kClamp = 8.0f;                // +18 dBFS: the most the limiter ever sees
constexpr float kTiny = 1e-18f;               // a DC offset (-360 dB) under everything in the loop: no denormals
constexpr float kDuckLaw = 16.0f;
constexpr float kMonoStep = 1.0f / 16.0f;     // Mono's share of R's wow, per segment: 12 ms to switch
constexpr int kTailForever = 1 << 30;         // feedback 1: 6.8 hours
constexpr int kFadeSegs = 69;                 // Fade's crossfade: 69 segments, 50 ms

float onePole(float hz) { return 1.0f - std::exp(-2.0f * kPi * hz / kRate); }
float perSegment(float seconds) { return 1.0f - std::exp(-kSeg / (seconds * kRate)); }
float perSample(float seconds) { return 1.0f - std::exp(-1.0f / (seconds * kRate)); }

// Set when the plugin loads (no guard to take on the audio thread).
const float kWowSmooth = perSegment(0.1f), kDuckSmooth = perSegment(0.02f);   // the amounts glide
const float kRelease = std::exp(1.0f / (0.1f * kRate));                        // the limiter's, per sample
const float kAttack = perSample(0.005f), kFall = perSample(0.25f);             // the duck's envelope

// Both lines read at their times: the taps at ages i - 1 .. i + 2, four samples in a row, through
// the Hermite kernel.
EF_INLINE Lr readLr(const float* bl, const float* br, int w, double teL, double teR) {
    const int il = std::clamp(static_cast<int>(teL), 2, kMaxAge), ir = std::clamp(static_cast<int>(teR), 2, kMaxAge);
    const Lr frac = pairLr(static_cast<float>(teL - il), static_cast<float>(teR - ir));
    int ql = w - il - 2, qr = w - ir - 2;
    if (ql < 0) ql += kLen;
    if (qr < 0) qr += kLen;
    const float* pl = bl + ql;
    const float* pr = br + qr;
    return hermiteLr(loadLr(pl + 3, pr + 3), loadLr(pl + 2, pr + 2), loadLr(pl + 1, pr + 1), loadLr(pl, pr), frac);
}

// Fade's equal-power gains for the old and the new head at the end of segment `fade` of the
// crossfade (none running: 1 and 0), exact at both ends.
void fadeGains(int fade, float& a, float& b) {
    const float x = fade < 0 ? 0.0f : static_cast<float>(fade + 1) / kFadeSegs;
    a = x <= 0.0f ? 1.0f : (x >= 1.0f ? 0.0f : sinQuarter(0.5f * kPi * (1.0f - x)));
    b = x <= 0.0f ? 0.0f : (x >= 1.0f ? 1.0f : sinQuarter(0.5f * kPi * x));
}

// A smoothed amount one segment on; snaps the last bit so it lands (wow 0 is exactly no wow).
float approach(float cur, float target, float coef) {
    const float d = target - cur;
    return std::fabs(d) < 1e-5f ? target : cur + d * coef;
}
float wrap1(float x) { return x >= 1.0f ? x - 1.0f : x; }

// Within a millionth of a sample of a whole number: whole (t > 0).
double wholeIfClose(double t) {
    const double r = floorFast(t + 0.5);
    return std::fabs(t - r) < 1e-6 ? r : t;
}

// Until a line has been written all the way round since reset(), it still holds what came before.
// Before a run of n samples at times te + d .. te + n d, zero what of that its taps can reach
// (ages past `written`; those written since stay): a few dozen samples, and only the first time
// round, instead of 1.4 MB at once or a check on every tap.
void hideOld(float* line, int w, int written, double te, double d, int n) {
    const double lo = std::min(te + d, te + n * d), hi = std::max(te + d, te + n * d);
    // At sample k a tap of age a reads what is k samples younger now; ages under 1 are written
    // in the run before they're read.
    const int from = std::max({written + 1, std::clamp(static_cast<int>(lo), 2, kMaxAge) - n, 1});
    const int to = std::clamp(static_cast<int>(hi), 2, kMaxAge) + 2;
    for (int a = from; a <= to; ++a) {
        const int j = w - a < 0 ? w - a + kLen : w - a;
        line[j] = 0.0f;
        if (j < kGuard) line[kLen + j] = 0.0f;
    }
}

} // namespace

Delay::Delay()
    : glide_(1.0 - std::exp(-kSeg / (0.060 * kRate))),
      lineL_(static_cast<size_t>(kLen + kGuard), 0.0f),
      lineR_(static_cast<size_t>(kLen + kGuard), 0.0f) {
    reset();
}

void Delay::reset() {
    w_ = 0;
    written_ = 0;
    lpL_ = lpR_ = hpL_ = hpR_ = 0.0f;
    gainL_ = gainR_ = 1.0f;
    env_ = 0.0f;
    phWow_ = phFlutL_ = phFlutR_ = 0.0f;
    segLeft_ = 0;
    fadeL_ = fadeR_ = -1;
    fading_ = false;
    fresh_ = true;
}

void Delay::set(const Params& p, const Transport& t) {
    const int mode = std::clamp(p.mode, 0, kModes - 1);
    glideType_ = std::clamp(p.glide, 0, kGlides - 1);   // a fade under way finishes first
    double base;
    if (p.sync) {
        const double beats = clampOr(p.divBeats, kDelayDivs[0].beats, kDelayDivs[kNumDelayDivs - 1].beats, 0.75);
        base = divSeconds(beats, clampOr(t.bpm, 1.0, 1000.0, 120.0)) * kRate;
    } else {
        base = static_cast<double>(clampOr(p.timeMs, 1.0f, 2000.0f, 375.0f)) * kRate / 1000.0;
    }
    const double spread = clampOr(p.spread, -0.5f, 0.5f, 0.0f);
    tgtL_ = wholeIfClose(std::clamp(base, kMinTime, kMaxTime));
    tgtR_ = mode == MONO ? tgtL_ : wholeIfClose(std::clamp(tgtL_ * (1.0 + spread), 0.5 * kMinTime, kMaxTime));

    const float fb = clampOr(p.feedback, 0.0f, 1.0f, 0.0f);
    tgt_[FB] = fb;
    tgt_[DRIVE] = clampOr(p.drive, 0.0f, 1.0f, 0.0f);
    tgt_[MIX] = clampOr(p.mix, 0.0f, 1.0f, 0.0f);
    const float lc = clampOr(p.lowCutHz, 20.0f, 2000.0f, 100.0f), hc = clampOr(p.highCutHz, 500.0f, 20000.0f, 20000.0f);
    if (lc != lastLowCut_) {
        lastLowCut_ = lc;
        tgt_[HP] = onePole(lc);
    }
    if (hc != lastHighCut_) {
        lastHighCut_ = hc;
        tgt_[LP] = hc >= 20000.0f ? 1.0f : onePole(hc);
    }
    tgt_[MONO_IN] = mode == STEREO ? 0.0f : 1.0f;
    tgt_[CROSS] = mode == PING_PONG ? 1.0f : 0.0f;
    tgt_[R_IN] = mode == PING_PONG ? 0.0f : 1.0f;
    wowTgt_ = clampOr(p.wow, 0.0f, 1.0f, 0.0f);
    duckTgt_ = clampOr(p.duck, 0.0f, 1.0f, 0.0f);
    monoTgt_ = mode == MONO ? 1.0f : 0.0f;

    if (fresh_) {   // jump: no ramps, no glide, the wow and the duck where they would be by now
        for (int k = 0; k < kRamps; ++k) cur_[k] = tgt_[k];
        tL_ = tgtL_;
        tR_ = tgtR_;
        wowAmt_ = wowTgt_;
        duckAmt_ = duckTgt_;
        mono_ = monoTgt_;
        float ml, mr;
        wowNow(tL_, tR_, ml, mr);
        teL_ = teEndL_ = tL_ + ml;
        teR_ = teEndR_ = tR_ + mr;
        teStepL_ = teStepR_ = 0.0;
        tBL_ = tL_;
        tBR_ = tR_;
        teBL_ = teBEndL_ = teL_;
        teBR_ = teBEndR_ = teR_;
        teBStepL_ = teBStepR_ = 0.0;
        for (int k = 0; k < 4; ++k) {
            gain_[k] = gainEnd_[k] = k < 2 ? 1.0f : 0.0f;
            gainStep_[k] = 0.0f;
        }
        duck_ = duckEnd_ = 1.0f / (1.0f + kDuckLaw * duckAmt_ * env_);
        duckStep_ = 0.0f;
        fresh_ = false;
    }

    // Down to -60 dB: repeat k is fb^(k-1) of the first (at the cuts' passband), at most k times
    // the longer side's time away (in Ping-Pong too: the sides take turns). Plus the wow and the
    // 20 Hz high-pass's ring. Only when the feedback or the times change (the logs only for the
    // feedback).
    const double longest = std::max({tgtL_, tgtR_, tL_, tR_, tBL_, tBR_}) + wowTgt_ * kWowDepth * (1.0f + kFlutter);
    if (fb != tailFb_ || longest != tailLongest_) {
        if (fb != tailFb_) {
            tailFb_ = fb;
            repeats_ = fb > 1e-6f && fb < 1.0f ? std::floor(std::log(1e-3) / std::log(static_cast<double>(fb))) + 1.0 : 1.0;
        }
        tailLongest_ = longest;
        tail_ = fb >= 1.0f ? kTailForever
                           : static_cast<int>(std::min(longest * repeats_ + 0.06 * kRate, static_cast<double>(kTailForever)));
    }
}

// The wow's offsets (samples) for heads at times tl and tr, at the phases and amounts now.
void Delay::wowNow(double tl, double tr, float& l, float& r) const {
    const float depthL = wowAmt_ * std::min(kWowDepth, 0.25f * static_cast<float>(tl));
    const float depthR = wowAmt_ * std::min(kWowDepth, 0.25f * static_cast<float>(tr));
    l = depthL * (sinCycle(phWow_) + kFlutter * sinCycle(phFlutL_));
    const float own = depthR * (sinCycle(wrap1(phWow_ + kWowLagR)) + kFlutter * sinCycle(phFlutR_));
    r = (1.0f - mono_) * own + mono_ * l;   // exact at 0 and 1: Mono's R is L
}

// Every 32 samples: the glide, the wow and the duck gain at the segment's end, and their steps.
void Delay::segment() {
    constexpr float kWowInc = kSeg * kWowHz / kRate, kFlutIncL = kSeg * kFlutterHzL / kRate,
                    kFlutIncR = kSeg * kFlutterHzR / kRate;

    teL_ = teEndL_;   // the last segment lands exactly where it was aimed
    teR_ = teEndR_;
    teBL_ = teBEndL_;
    teBR_ = teBEndR_;
    for (int k = 0; k < 4; ++k) gain_[k] = gainEnd_[k];
    duck_ = duckEnd_;

    wowAmt_ = approach(wowAmt_, wowTgt_, kWowSmooth);
    duckAmt_ = approach(duckAmt_, duckTgt_, kDuckSmooth);
    mono_ = mono_ < monoTgt_ ? std::min(monoTgt_, mono_ + kMonoStep) : std::max(monoTgt_, mono_ - kMonoStep);
    phWow_ = wrap1(phWow_ + kWowInc);
    phFlutL_ = wrap1(phFlutL_ + kFlutIncL);
    phFlutR_ = wrap1(phFlutR_ + kFlutIncR);

    // Each side's heads. A fade that is over hands the reading to its new head. Then Tape glides;
    // Fade snaps a change under half a sample, or starts a fade to the target (true).
    const auto heads = [this](double& t, double& tB, double& te, double& teB, float& gA, float& gB, int& fade,
                              double target) {
        if (fade >= 0 && ++fade == kFadeSegs) {
            t = tB;
            te = teB;
            gA = 1.0f;
            gB = 0.0f;
            fade = -1;
        }
        if (fade >= 0) return false;
        if (glideType_ == TAPE) {
            t = glideTime(t, target);
        } else if (std::fabs(target - t) < 0.5) {
            t = target;
        } else {
            tB = target;
            fade = 0;
            return true;
        }
        tB = t;
        return false;
    };
    const bool newL = heads(tL_, tBL_, teL_, teBL_, gain_[0], gain_[2], fadeL_, tgtL_);
    const bool newR = heads(tR_, tBR_, teR_, teBR_, gain_[1], gain_[3], fadeR_, tgtR_);

    float ml, mr;
    wowNow(tL_, tR_, ml, mr);
    teEndL_ = tL_ + ml;
    teEndR_ = tR_ + mr;
    teStepL_ = (teEndL_ - teL_) * (1.0 / kSeg);
    teStepR_ = (teEndR_ - teR_) * (1.0 / kSeg);

    fading_ = fadeL_ >= 0 || fadeR_ >= 0;
    if (fading_) {
        wowNow(tBL_, tBR_, ml, mr);
        teBEndL_ = tBL_ + ml;
        teBEndR_ = tBR_ + mr;
        if (newL) teBL_ = teBEndL_;   // a new head starts where it will be: its gain is still near 0
        if (newR) teBR_ = teBEndR_;
    }
    if (fadeL_ < 0) {   // no fade on this side: B reads what A does, at gain 0
        teBL_ = teL_;
        teBEndL_ = teEndL_;
    }
    if (fadeR_ < 0) {
        teBR_ = teR_;
        teBEndR_ = teEndR_;
    }
    teBStepL_ = (teBEndL_ - teBL_) * (1.0 / kSeg);
    teBStepR_ = (teBEndR_ - teBR_) * (1.0 / kSeg);
    fadeGains(fadeL_, gainEnd_[0], gainEnd_[2]);
    fadeGains(fadeR_, gainEnd_[1], gainEnd_[3]);
    for (int k = 0; k < 4; ++k) gainStep_[k] = (gainEnd_[k] - gain_[k]) * (1.0f / kSeg);

    duckEnd_ = 1.0f / (1.0f + kDuckLaw * duckAmt_ * env_);
    duckStep_ = (duckEnd_ - duck_) * (1.0f / kSeg);
    segLeft_ = kSeg;
}

void Delay::process(float* L, float* R, int n) {
    if (n <= 0) return;
    const float inv = 1.0f / static_cast<float>(n);
    bool moving = false;
    for (int k = 0; k < kRamps; ++k) {
        step_[k] = (tgt_[k] - cur_[k]) * inv;
        moving = moving || tgt_[k] != cur_[k];
    }
    for (int i = 0; i < n;) {
        if (segLeft_ == 0) segment();
        const int m = std::min(n - i, segLeft_);
        if (moving) {
            if (fading_) run<true, true>(L + i, R + i, m);
            else run<true, false>(L + i, R + i, m);
        } else {
            if (fading_) run<false, true>(L + i, R + i, m);
            else run<false, false>(L + i, R + i, m);
        }
        segLeft_ -= m;
        i += m;
    }
    for (int k = 0; k < kRamps; ++k) cur_[k] = tgt_[k];   // lands exactly
}

template <bool Moving, bool Fading>
void Delay::run(float* L, float* R, int n) {
    // Locals: the compiler can't keep members in registers across the stores to L, R and the lines.
    float* const bl = lineL_.data();
    float* const br = lineR_.data();
    int w = w_, written = written_;
    double teL = teL_, teR = teR_;
    const double dl = teStepL_, dr = teStepR_;
    Lr lp = {lpL_, lpR_}, hp = {hpL_, hpR_}, gain = {gainL_, gainR_}, env = both(env_), duck = both(duck_);
    const Lr dDuck = both(duckStep_), attack = both(kAttack), fall = both(kFall), release = both(kRelease);
    Lr fb = both(cur_[FB]), drive = both(cur_[DRIVE]), mix = both(cur_[MIX]), aLp = both(cur_[LP]), aHp = both(cur_[HP]);
    Lr monoIn = both(cur_[MONO_IN]), cross = both(cur_[CROSS]), rIn = Lr{1.0f, cur_[R_IN]};
    const Lr sFb = both(step_[FB]), sDrive = both(step_[DRIVE]), sMix = both(step_[MIX]), sLp = both(step_[LP]),
             sHp = both(step_[HP]), sMonoIn = both(step_[MONO_IN]), sCross = both(step_[CROSS]), sRIn = Lr{0.0f, step_[R_IN]};
    double teBL = teBL_, teBR = teBR_;
    const double dbl = teBStepL_, dbr = teBStepR_;
    Lr gainA = {gain_[0], gain_[1]}, gainB = {gain_[2], gain_[3]};
    const Lr sGainA = {gainStep_[0], gainStep_[1]}, sGainB = {gainStep_[2], gainStep_[3]};
    if (written < kLen) {
        hideOld(bl, w, written, teL, dl, n);
        hideOld(br, w, written, teR, dr, n);
        if (Fading) {
            hideOld(bl, w, written, teBL, dbl, n);
            hideOld(br, w, written, teBR, dbr, n);
        }
    }

    for (int k = 0; k < n; ++k) {
        if (Moving) {
            fb += sFb;
            drive += sDrive;
            mix += sMix;
            aLp += sLp;
            aHp += sHp;
            monoIn += sMonoIn;
            cross += sCross;
            rIn += sRIn;
        }
        teL += dl;
        teR += dr;
        duck += dDuck;
        const Lr x = finiteLr(loadLr(L + k, R + k));

        Lr r = readLr(bl, br, w, teL, teR);
        if (Fading) {   // and the new head, crossfaded in
            teBL += dbl;
            teBR += dbr;
            gainA += sGainA;
            gainB += sGainB;
            r = r * gainA + readLr(bl, br, w, teBL, teBR) * gainB;
        }

        // The cuts, at the tap: the wet and the feedback both pass them.
        lp += aLp * (r - lp);
        hp += aHp * (lp - hp);
        const Lr y = lp - hp;

        // Into the lines: the input (or its mono sum; none into R in Ping-Pong) and the feedback
        // (from the other side in Ping-Pong). Halves first: L + R could overflow.
        const Lr sum = both(0.5f) * x + both(0.5f) * swapLr(x);
        const Lr in = (x * (both(1.0f) - monoIn) + sum * monoIn) * rIn;
        const Lr back = y * (both(1.0f) - cross) + swapLr(y) * cross;
        const Lr u = minLr(maxLr(in + fb * back + both(kTiny), both(-kClamp)), both(kClamp));

        const Lr c = minLr(maxLr(u, both(-0.5f)), both(0.5f));
        Lr v = u + drive * (c - both(4.0f / 3.0f) * c * c * c - u);

        // The limiter: the gain recovers, then drops at once to what keeps this sample at 1.
        const Lr a = absLr(v);
        gain = minLr(gain * release, both(1.0f));
        gain = pickLr(greaterLr(a * gain, both(1.0f)), recipLr(maxLr(a, both(1.0f))), gain);
        v *= gain;

        storeLr(bl + w, br + w, v);
        if (w < kGuard) storeLr(bl + kLen + w, br + kLen + w, v);
        w = w + 1 == kLen ? 0 : w + 1;

        // The duck's envelope follows the input's louder side (up to +18 dBFS: a wild sample
        // mustn't hold the wet down for long).
        const Lr level = minLr(louderLr(absLr(x)), both(kClamp)) + both(kTiny);
        env += pickLr(greaterLr(level, env), attack, fall) * (level - env);

        storeLr(L + k, R + k, x * (both(1.0f) - mix) + y * duck * mix);   // exact at mix 0
    }

    w_ = w;
    written_ = std::min(written + n, kLen);
    teL_ = teL;
    teR_ = teR;
    lpL_ = lp[0];
    lpR_ = lp[1];
    hpL_ = hp[0];
    hpR_ = hp[1];
    gainL_ = gain[0];
    gainR_ = gain[1];
    env_ = env[0];
    duck_ = duck[0];
    if (Fading) {
        teBL_ = teBL;
        teBR_ = teBR;
        gain_[0] = gainA[0];
        gain_[1] = gainA[1];
        gain_[2] = gainB[0];
        gain_[3] = gainB[1];
    }
    if (Moving) {
        cur_[FB] = fb[0];
        cur_[DRIVE] = drive[0];
        cur_[MIX] = mix[0];
        cur_[LP] = aLp[0];
        cur_[HP] = aHp[0];
        cur_[MONO_IN] = monoIn[0];
        cur_[CROSS] = cross[0];
        cur_[R_IN] = rIn[1];
    }
}

} // namespace ef
