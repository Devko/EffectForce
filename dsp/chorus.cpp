// Chorus: see chorus.h.
#include "chorus.h"

#include <cfloat>
#include <cmath>

namespace ef {

namespace {

#if EF_NEON
using Pair = float32x2_t;   // a sample of L and R
#else
typedef float Pair __attribute__((vector_size(8)));
#endif

constexpr float kMs = kRate / 1000.0f;      // samples per millisecond
constexpr float kMinRead = 1.0f;            // no read closer than one sample behind the newest
constexpr float kMaxRead = static_cast<float>(Chorus::kLine - kChunk - 4);   // see run()
constexpr float kLfoPeak = 1.01f;           // a magic circle's mixes reach a hair over 1 (1 + e / 4)
constexpr float kSqrt2 = 1.41421356f;       // the low cut's damping: Butterworth
constexpr float kChorusGain = 0.70710678f;  // 1 / sqrt(2): two voices per side
constexpr float kEnsembleGain = 0.57735027f;   // 1 / sqrt(3)
constexpr float kFastShare = Chorus::kEnsembleFastMs / Chorus::kEnsembleSlowMs;
constexpr float kHalfPi = 1.57079633f;

// Per-chunk glides toward a new delay (50 ms), depth, width or low cut (20 ms) and mix (two 5 ms
// one-poles in a row: it sets off without a corner): a jump in delay time bends the pitch like tape
// instead of skipping across the line, and no jump clicks.
float chunkGlide(float seconds) { return 1.0f - std::exp(-static_cast<float>(kChunk) / (seconds * kRate)); }
const float kGlideDelay = chunkGlide(0.05f);
const float kGlideDepth = chunkGlide(0.02f);
const float kGlideMix = chunkGlide(0.005f);

float glideTo(float cur, float target, float k, float snap) {
    const float next = cur + (target - cur) * k;
    return std::fabs(target - next) < snap ? target : next;
}
float glide2(float (&g)[2], float target, float k, float snap) {
    g[0] = glideTo(g[0], target, k, snap);
    g[1] = glideTo(g[1], g[0], k, snap);
    return g[1];
}
// A mode change fades the wet out and in over 8 chunks each, along a smoothstep (no corners).
constexpr float kGateStep = 0.125f;
float fade(float gate) { return gate * gate * (3.0f - 2.0f * gate); }

// Out-of-range values clamped, NaN to the bottom of the range.
float clampParam(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }

EF_INLINE i4 splatInt(int x) { return i4{x, x, x, x}; }
EF_INLINE Pair pair(float x) { return Pair{x, x}; }

EF_INLINE Pair loadPair(const float* l, const float* r) {
#if EF_NEON
    return vld1_lane_f32(r, vld1_dup_f32(l), 1);
#else
    return Pair{*l, *r};
#endif
}
EF_INLINE void storePair(float* l, float* r, Pair x) {
#if EF_NEON
    vst1_lane_f32(l, x, 0);
    vst1_lane_f32(r, x, 1);
#else
    *l = x[0];
    *r = x[1];
#endif
}
// sanitize() on both lanes.
EF_INLINE Pair finite(Pair x) {
#if EF_NEON
    return vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(x), vcale_f32(x, vdup_n_f32(FLT_MAX))));
#else
    return Pair{sanitize(x[0]), sanitize(x[1])};
#endif
}

// Four cubic reads at once, each as DelayLine::readCubic: lane k reads d[k] (>= 1) samples behind
// the newest sample (at w) of the line starting at buf + off[k]. A line's guard repeats its first
// three samples, so a lane's four taps are one contiguous load, oldest first; a transpose turns
// the four loads into the tap columns the interpolation wants.
EF_INLINE f4 readCubic4(const float* buf, int w, i4 off, f4 d) {
    const i4 i = __builtin_convertvector(d, i4);   // d >= 1: truncation is the floor
    const f4 t = d - __builtin_convertvector(i, f4);
    const i4 p = ((splatInt(w - 2) - i) & splatInt(Chorus::kLine - 1)) + off;
#if EF_NEON
    // Lanes 0 and 2 in one register, 1 and 3 in the other, half a lane's taps at a time: then one
    // transpose step makes the columns.
    const float *q0 = buf + vgetq_lane_s32(p, 0), *q1 = buf + vgetq_lane_s32(p, 1);
    const float *q2 = buf + vgetq_lane_s32(p, 2), *q3 = buf + vgetq_lane_s32(p, 3);
    const float32x4x2_t old = vtrnq_f32(vcombine_f32(vld1_f32(q0), vld1_f32(q2)), vcombine_f32(vld1_f32(q1), vld1_f32(q3)));
    const float32x4x2_t nu =
        vtrnq_f32(vcombine_f32(vld1_f32(q0 + 2), vld1_f32(q2 + 2)), vcombine_f32(vld1_f32(q1 + 2), vld1_f32(q3 + 2)));
    const f4 x2 = old.val[0], x1 = old.val[1], x0 = nu.val[0], xm1 = nu.val[1];
#else
    const float *q0 = buf + p[0], *q1 = buf + p[1], *q2 = buf + p[2], *q3 = buf + p[3];
    const f4 x2{q0[0], q1[0], q2[0], q3[0]}, x1{q0[1], q1[1], q2[1], q3[1]};
    const f4 x0{q0[2], q1[2], q2[2], q3[2]}, xm1{q0[3], q1[3], q2[3], q3[3]};
#endif
    // common.h's hermite() lane by lane, in Laurent de Soras's arrangement (one constant, so fewer
    // registers): c1 as there, a = c3, b = -c2.
    const f4 half = splat(0.5f);
    const f4 c1 = half * (x1 - xm1), v = x0 - x1, w2 = c1 + v;
    const f4 a = w2 + v + half * (x2 - x0), b = w2 + a;
    return ((a * t - b) * t + c1) * t + x0;
}

// Lane sums: [a0 + a1, a2 + a3], and [(a0 + a1) + (a2 + a3), (b0 + b1) + (b2 + b3)].
EF_INLINE Pair pairSums(f4 a) {
#if EF_NEON
    return vpadd_f32(vget_low_f32(a), vget_high_f32(a));
#else
    return Pair{a[0] + a[1], a[2] + a[3]};
#endif
}
EF_INLINE Pair totals(f4 a, f4 b) {
#if EF_NEON
    return vpadd_f32(pairSums(a), pairSums(b));
#else
    return Pair{(a[0] + a[1]) + (a[2] + a[3]), (b[0] + b[1]) + (b[2] + b[3])};
#endif
}
// The Dimension's sides: each voice (lanes 0 and 1) with kCross of the other.
EF_INLINE Pair crossMix(f4 a) {
#if EF_NEON
    const Pair v = vget_low_f32(a);
    return v + pair(Chorus::kCross) * (vrev64_f32(v) - v);
#else
    const Pair v{a[0], a[1]}, r{a[1], a[0]};
    return v + pair(Chorus::kCross) * (r - v);
#endif
}

bool same(f4 a, f4 b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

// sin and cos of 0..pi/2 by fastmath.h's sinQuarter, exact at 0 (so a width of 0 turns nothing).
void sinCos(float a, float& s, float& c) {
    s = a > 0.0f ? sinQuarter(a) : 0.0f;
    c = a > 0.0f ? sinQuarter(kHalfPi - a) : 1.0f;
}

} // namespace

Chorus::Chorus() : buf_(static_cast<size_t>(2 * kStride), 0.0f) {}

void Chorus::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    w_ = 0;
    ic1l_ = ic2l_ = ic1r_ = ic2r_ = 0.0f;
    s_ = sf_ = 0.0f;
    c_ = cf_ = 1.0f;
    acc_ = 0;
    left_ = 0;
    fresh_ = true;
}

// The voices' coefficients for the current mode and the glided depth and width, per group of four
// voices. Sine modes: the weights of the LFO's sine and cosine, a voice's amplitude times the
// cosine and sine of its phase offset (its delay swings by amplitude * sin(LFO phase + offset)).
// The Ensemble's fast LFO has the slow one's offsets at a fixed share of its amplitude, so it adds
// to the slow one's sine and cosine before they are weighted. Dimension: the amplitude and the
// phase offset in cycles. Returns the largest swing in samples.
float Chorus::voiceTargets(f4 (&t)[2][2]) const {
    for (auto& g : t) g[0] = g[1] = splat(0.0f);
    const float room = (baseGlide_ - kMinRead) / kLfoPeak;   // the most a voice may swing
    const float depth = depthGlide_, width = widthGlide_;

    if (mode_ == kDimension) {
        // Left and right voice (lanes 0 and 1; 2 and 3 idle at the base delay), the right one up
        // to half a cycle later.
        const float a = std::min(depth * kDimensionMs * kMs, room);
        t[0][0] = f4{a, a, 0.0f, 0.0f};
        t[0][1] = f4{0.0f, 0.5f * width, 0.0f, 0.5f * width};
        return a;
    }

    // The right voices' phases turn from the left ones' by up to 90 degrees (Chorus) or 60
    // (Ensemble: halfway between the left ones).
    float sn, cs;
    sinCos((mode_ == kChorus ? kHalfPi : kPi / 3.0f) * width, sn, cs);
    if (mode_ == kChorus) {
        // Each side's pair at 0 and 180 degrees: lanes L0 L1 R0 R1.
        const float a = std::min(depth * kChorusMs * kMs, room);
        t[0][0] = f4{a, -a, a * cs, -(a * cs)};
        t[0][1] = f4{0.0f, 0.0f, a * sn, -(a * sn)};
        return a;
    }
    // Ensemble: three voices 120 degrees apart per side (lane 3 idle), a group per side.
    const float a = std::min(depth * kEnsembleSlowMs * kMs, room / (1.0f + kFastShare));
    const float h = 0.866025404f;   // sin 120
    const f4 lc{1.0f, -0.5f, -0.5f, 0.0f}, ls{0.0f, h, -h, 0.0f};
    t[0][0] = splat(a) * lc;
    t[0][1] = splat(a) * ls;
    t[1][0] = splat(a) * (splat(cs) * lc - splat(sn) * ls);
    t[1][1] = splat(a) * (splat(sn) * lc + splat(cs) * ls);
    return a * (1.0f + kFastShare);
}

void Chorus::set(const Params& p, const Transport&) {
    const int want = p.mode < kChorus ? kChorus : (p.mode > kDimension ? kDimension : p.mode);
    const float rate = clampParam(p.rateHz, 0.03f, 10.0f);
    // In double: whole ms are whole samples (in float, the reassociation -funsafe-math-optimizations
    // allows makes 10 ms 440.99998).
    const float base = static_cast<float>(clampParam(p.delayMs, 1.0f, 40.0f) * (static_cast<double>(kRate) / 1000.0));
    const float depth = clampParam(p.depth, 0.0f, 1.0f);
    const float width = clampParam(p.width, 0.0f, 1.0f);
    const float mix = clampParam(p.mix, 0.0f, 1.0f);
    const bool cutOn = p.lowCutHz > 20.0f;
    const float cutHz = clampParam(p.lowCutHz, 20.0f, 1000.0f);

    // The LFOs only change speed: their phase runs on.
    e_ = 2.0f * sinQuarter(kPi * rate / kRate);
    ef_ = 2.0f * sinQuarter(kPi * kFastRatio * rate / kRate);
    inc_ = static_cast<uint32_t>(rate * (4294967296.0f / kRate) + 0.5f);

    bool snap = fresh_;
    if (fresh_) {
        mode_ = want;
        gate_ = 1.0f;
        baseGlide_ = base;
        depthGlide_ = depth;
        widthGlide_ = width;
        mixGlide_[0] = mixGlide_[1] = mix;
    } else {
        // A new mode: the wet fades out, the voices switch while it is silent, and it fades back in.
        if (want != mode_ && gate_ == 0.0f && left_ == 0) {
            mode_ = want;
            snap = true;
        }
        gate_ = want == mode_ ? std::min(gate_ + kGateStep, 1.0f) : std::max(gate_ - kGateStep, 0.0f);
        baseGlide_ = glideTo(baseGlide_, base, kGlideDelay, 1e-3f);
        depthGlide_ = glideTo(depthGlide_, depth, kGlideDepth, 1e-5f);
        widthGlide_ = glideTo(widthGlide_, width, kGlideDepth, 1e-5f);
        glide2(mixGlide_, mix, kGlideMix, 1e-5f);
    }

    // Constant-power dry / wet, exactly 1 and 0 at the ends.
    const float m = mixGlide_[1] * fade(gate_);
    const float dryT = m > 0.0f ? (m < 1.0f ? sinQuarter(kHalfPi * (1.0f - m)) : 0.0f) : 1.0f;
    const float wetT = m > 0.0f ? (m < 1.0f ? sinQuarter(kHalfPi * m) : 1.0f) : 0.0f;
    // The low cut's g glides in octaves (20 ms), so a chunk moves it only a little. Switched off,
    // it glides down to 20 Hz first and only then fades out (over a chunk), where it takes next to
    // nothing away; switched on, it fades in there and glides up.
    const float gT = tanFast(kPi * cutHz / kRate);
    if (fresh_) {
        gGlide_ = gT;
    } else {
        const float octaves = log2Fast(gT / gGlide_);
        gGlide_ = std::fabs(octaves) < 1e-4f ? gT : gGlide_ * exp2Fast(octaves * kGlideDepth);
    }
    const float cutT = cutOn || gGlide_ != gT ? 1.0f : 0.0f;
    const float a1 = 1.0f / (1.0f + gGlide_ * (gGlide_ + kSqrt2));
    const float aT[3] = {a1, gGlide_ * a1, gGlide_ * gGlide_ * a1};
    f4 ct[2][2];
    const float swing = voiceTargets(ct);
    // How long it rings: the longest delay a voice reaches, and the low cut's ring (its envelope
    // falls 60 dB in about 1.55 / cutoff seconds).
    tail_ = static_cast<int>(std::max(base, baseGlide_) + kLfoPeak * swing + (cutT > 0.0f ? 1.6f * kRate / cutHz : 0.0f)) + 4;

    if (fresh_) {
        delay_ = delayTarget_ = baseGlide_;
        dry_ = dryTarget_ = dryT;
        wet_ = wetTarget_ = wetT;
        for (int k = 0; k < 3; ++k) a_[k] = aTarget_[k] = aT[k];
        cut_ = cutTarget_ = cutT;
        for (int g = 0; g < 2; ++g)
            for (int j = 0; j < 2; ++j) coef_[g][j] = coefTarget_[g][j] = ct[g][j];
        left_ = 0;
    } else {
        if (snap)
            for (int g = 0; g < 2; ++g)
                for (int j = 0; j < 2; ++j) coef_[g][j] = coefTarget_[g][j] = ct[g][j];
        bool changed = baseGlide_ != delayTarget_ || dryT != dryTarget_ || wetT != wetTarget_ || cutT != cutTarget_ ||
                       aT[0] != aTarget_[0] || aT[1] != aTarget_[1] || aT[2] != aTarget_[2];
        for (int g = 0; g < 2; ++g)
            for (int j = 0; j < 2; ++j) changed = changed || !same(ct[g][j], coefTarget_[g][j]);
        // Unchanged targets leave a running ramp alone (with blocks shorter than a chunk, set()
        // comes more often than a ramp's length).
        if (changed) {
            constexpr float inv = 1.0f / kChunk;
            delayTarget_ = baseGlide_;
            dryTarget_ = dryT;
            wetTarget_ = wetT;
            cutTarget_ = cutT;
            delayStep_ = (baseGlide_ - delay_) * inv;
            dryStep_ = (dryT - dry_) * inv;
            wetStep_ = (wetT - wet_) * inv;
            cutStep_ = (cutT - cut_) * inv;
            for (int k = 0; k < 3; ++k) {
                aTarget_[k] = aT[k];
                aStep_[k] = (aT[k] - a_[k]) * inv;
            }
            for (int g = 0; g < 2; ++g)
                for (int j = 0; j < 2; ++j) {
                    coefTarget_[g][j] = ct[g][j];
                    coefStep_[g][j] = (ct[g][j] - coef_[g][j]) * splat(inv);
                }
            left_ = kChunk;
        }
    }
    fresh_ = false;

    // A low cut that has faded out stops (and starts again from rest); states that fell silent go
    // to zero rather than through denormals (slow on x86).
    if (cut_ == 0.0f && cutTarget_ == 0.0f) ic1l_ = ic2l_ = ic1r_ = ic2r_ = 0.0f;
    for (float* v : {&ic1l_, &ic2l_, &ic1r_, &ic2r_})
        if (std::fabs(*v) < 1e-15f) *v = 0.0f;
}

void Chorus::process(float* L, float* R, int n) {
    switch (mode_) {
    case kChorus: run<kChorus>(L, R, n); break;
    case kEnsemble: run<kEnsemble>(L, R, n); break;
    default: run<kDimension>(L, R, n); break;
    }
}

// A chunk in two passes, each with less to keep in registers: the input into the lines, then the
// voices out of them. Reads stay shorter than kMaxRead, so none reaches back to where this
// chunk's later samples are already written. Everything per sample is local: L and R are floats,
// so the compiler would otherwise reload every member after each store.

// Pass 1: the input through the wet's low cut (Simper's trapezoidal SVF, high-pass = input - k band
// - low, faded in and out by `cut`) into the lines. L and R side by side in a two-lane vector.
void Chorus::writeLines(const float* L, const float* R, int n) {
    float* const buf = buf_.data();
    int w = w_;
    Pair va1 = pair(a_[0]), va2 = pair(a_[1]), va3 = pair(a_[2]), vcut = pair(cut_);
    const Pair sa1 = pair(aStep_[0]), sa2 = pair(aStep_[1]), sa3 = pair(aStep_[2]), scut = pair(cutStep_);
    Pair ic1 = Pair{ic1l_, ic1r_}, ic2 = Pair{ic2l_, ic2r_};
    const bool cutOn = cut_ > 0.0f || cutTarget_ > 0.0f;
    int left = left_;
    for (int i = 0; i < n; ++i) {
        if (left > 0) {
            va1 += sa1;
            va2 += sa2;
            va3 += sa3;
            vcut += scut;
            if (--left == 0) {
                va1 = pair(aTarget_[0]);
                va2 = pair(aTarget_[1]);
                va3 = pair(aTarget_[2]);
                vcut = pair(cutTarget_);
            }
        }
        const Pair x = finite(loadPair(L + i, R + i));
        Pair h = x;
        if (cutOn) {
            const Pair v3 = x - ic2;
            const Pair v1 = va1 * ic1 + va2 * v3;
            const Pair v2 = ic2 + va2 * ic1 + va3 * v3;
            ic1 = pair(2.0f) * v1 - ic1;
            ic2 = pair(2.0f) * v2 - ic2;
            h = x - vcut * (pair(kSqrt2) * v1 + v2);
        }
        w = (w + 1) & (kLine - 1);
        storePair(buf + w, buf + kStride + w, h);
        if (w < 3) storePair(buf + kLine + w, buf + kStride + kLine + w, h);
    }
    a_[0] = va1[0];
    a_[1] = va2[0];
    a_[2] = va3[0];
    cut_ = vcut[0];
    ic1l_ = ic1[0];
    ic1r_ = ic1[1];
    ic2l_ = ic2[0];
    ic2r_ = ic2[1];
}

// Pass 2: the LFOs, each voice's delay from the base delay, its coefficients and the LFOs (four
// voices to a vector), four cubic reads at once, the sides' sums, dry / wet.
template <int M>
void Chorus::run(float* L, float* R, int n) {
    writeLines(L, R, n);

    constexpr int G = M == kEnsemble ? 2 : 1;
    // The lines the lanes read: Chorus L L R R; Ensemble a group per side (lane 3 idle); Dimension
    // L R (2 and 3 idle).
    const i4 off[2] = {M == kChorus ? i4{0, 0, kStride, kStride} : (M == kEnsemble ? splatInt(0) : i4{0, kStride, 0, kStride}),
                       splatInt(kStride)};
    const f4 sum3 = f4{kEnsembleGain, kEnsembleGain, kEnsembleGain, 0.0f};
    const float* const buf = buf_.data();
    int w = w_;

    f4 co[G][2], st[G][2];
    for (int k = 0; k < G; ++k)
        for (int j = 0; j < 2; ++j) {
            co[k][j] = coef_[k][j];
            st[k][j] = coefStep_[k][j];
        }
    float delay = delay_, dry = dry_, wet = wet_;
    const float delayStep = delayStep_, dryStep = dryStep_, wetStep = wetStep_;
    const bool dryOnly = left_ == 0 && wet == 0.0f;   // mix 0: the input exactly
    float s = s_, c = c_, sf = sf_, cf = cf_;
    const float e = e_, ef = ef_;
    uint32_t acc = acc_;
    const uint32_t inc = inc_;
    int left = left_;

    for (int i = 0; i < n; ++i) {
        if (left > 0) {
            for (int k = 0; k < G; ++k)
                for (int j = 0; j < 2; ++j) co[k][j] += st[k][j];
            delay += delayStep;
            dry += dryStep;
            wet += wetStep;
            if (--left == 0) {
                for (int k = 0; k < G; ++k)
                    for (int j = 0; j < 2; ++j) co[k][j] = coefTarget_[k][j];
                delay = delayTarget_;
                dry = dryTarget_;
                wet = wetTarget_;
            }
        }
        w = (w + 1) & (kLine - 1);

        f4 d[G];
        if constexpr (M == kDimension) {
            // Triangle: -1 at phase 0, 1 at half a cycle; the right voice's phase is offset.
            acc += inc;
            f4 ph = splat(static_cast<float>(acc >> 8) * 0x1p-24f) + co[0][1];
            ph = ph - __builtin_convertvector(__builtin_convertvector(ph, i4), f4);
            d[0] = splat(delay) + co[0][0] * (splat(4.0f) * min4(ph, splat(1.0f) - ph) - splat(1.0f));
        } else {
            // Magic circles: sine and (half a step behind) cosine; the rotation is exactly
            // area-preserving, so the amplitude holds for good without renormalising.
            s += e * c;
            c -= e * s;
            float ls = s, lc = c;
            if constexpr (M == kEnsemble) {
                sf += ef * cf;
                cf -= ef * sf;
                ls += kFastShare * sf;
                lc += kFastShare * cf;
            }
            for (int k = 0; k < G; ++k) d[k] = splat(delay) + co[k][0] * splat(ls) + co[k][1] * splat(lc);
        }
        f4 y[G];
        for (int k = 0; k < G; ++k) y[k] = readCubic4(buf, w, off[k], min4(max4(d[k], splat(kMinRead)), splat(kMaxRead)));
        Pair wv;
        if constexpr (M == kChorus) wv = pair(kChorusGain) * pairSums(y[0]);
        else if constexpr (M == kEnsemble) wv = totals(y[0] * sum3, y[1] * sum3);
        else wv = crossMix(y[0]);
        const Pair x = finite(loadPair(L + i, R + i));
        storePair(L + i, R + i, dryOnly ? x : pair(dry) * x + pair(wet) * wv);
    }

    w_ = w;
    for (int k = 0; k < G; ++k)
        for (int j = 0; j < 2; ++j) coef_[k][j] = co[k][j];
    delay_ = delay;
    dry_ = dry;
    wet_ = wet;
    s_ = s;
    c_ = c;
    sf_ = sf;
    cf_ = cf;
    acc_ = acc;
    left_ = left;
}

int Chorus::tailSamples() const { return tail_; }

} // namespace ef
