#pragma once
// Comp (docs/DESIGN.md, "Modules"): one module, two modes.
//
// Comp: a feed-forward compressor, stereo-linked. The detector reads the louder channel's peak
// (max |L|, |R|, after the sidechain low cut): a sine's level is its peak in dBFS, and a source
// panned hard to one side pulls both sides down by the same amount, so the image stays put. It
// is Giannoulis, Massberg & Reiss's "smooth decoupled" peak detector, two one-poles in the
// linear domain: an instant attack with the release time first (it holds a waveform's peaks
// with little ripple), then the attack time. A level step therefore reaches 63% of its change
// in about the attack time; on the way down the level falls by 8.7 dB per release time
// constant, so 63% of a 7.5 dB gain reduction comes back in about 0.8 of the release time. The
// gain computer works in log2 units (1 = 6.02 dB): threshold, ratio and a quadratic soft knee.
// Mix is parallel compression: dry plus compressed-with-makeup, folded into one gain per sample
// (no latency, so the two can't comb), and mix 0 is the input bit for bit (on the device a
// denormal input comes out as zero: NEON flushes it).
//
// OTT: three bands split by Linkwitz-Riley 4th-order crossovers at 88.3 Hz and 2.5 kHz, each band
// compressed down above one threshold and up below a lower one, after Xfer's OTT (Ableton's
// Multiband Dynamics "OTT" preset):
//
//   band    up below   down above   attack    release   (x ottTime)
//   low     -28 dBFS   -20 dBFS     47.8 ms   282 ms
//   mid     -34 dBFS   -22 dBFS     22.4 ms   282 ms
//   high    -28 dBFS   -22 dBFS     13.5 ms   132 ms
//
// Down: 66.7:1 (practically a limit) with a 6 dB soft knee, its slope scaled by ottDown. Up:
// 4.17:1, its slope scaled by ottUp, at most +30 dB, and nothing under -90 dBFS: the lift allowed
// grows by 2 dB per dB above it (full from -75), so silence and a noise floor stay down. Times and
// ratios as the preset is usually quoted. Its thresholds (around -41 and -30..-36 dB, then 6..10 dB
// of band output gain) read a quieter meter; these are peak levels with that gain folded in, so a
// band between its two thresholds passes unchanged and no makeup hides in the bands. The upward
// gain reads the detector before its attack stage: it drops at once when a band gets louder (a
// transient after a quiet passage would otherwise get the whole lift for the attack time). Depth
// is the dry / wet against the bands' own unprocessed sum (the crossovers' allpass), so it blends
// gains, not phases: no notches at the crossovers. Makeup comes last.
//
// The bands: split at 88.3 Hz, the upper part split again at 2.5 kHz, the low band through the
// 2.5 kHz allpass, so low + mid + high is exactly an allpass (flat). Each Linkwitz-Riley is two
// Butterworth state-variable filters (TPT) in series, both channels and two filters at a time in
// four lanes; the three bands' detectors and gain computers are four lanes too.
//
// Speed: per sample only what recurses (filters, detectors); the gain computers run after, Comp's
// four samples at a time. Vector lanes throughout: a scalar float compare (max, clamp) moves
// flags from the FPU to the core on ARMv7 and stalls.
//
// A mode switch fades from the old mode to the new one over a chunk; the new one starts cleared.
#include "common.h"
#include "simd.h"

#include <cfloat>
#include <cmath>

namespace ef {

namespace cmp {

constexpr float kLog2PerDb = 0.166096404744f;   // log2(10) / 20: dB to the gain computers' log2 units
constexpr float kButterK = 1.41421356f;         // 1 / Q of a Butterworth section; two in series: Linkwitz-Riley
constexpr float kMaxIn = 1e4f;                  // +80 dBFS: past it the input is clamped, so nothing overflows
constexpr float kTiny = 1e-20f;                 // the detectors fall to exact zero instead of into denormals
constexpr float kFloor = 1e-15f;                // the level log2 sees for silence (-300 dBFS)

// A parameter into [lo, hi]; NaN (which no comparison catches) becomes lo.
inline float param(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }

// --- two lanes: L and R ------------------------------------------------------------------------

#if EF_NEON
using f2 = float32x2_t;
#else
typedef float f2 __attribute__((vector_size(8)));
#endif

EF_INLINE f2 splat2(float x) { return f2{x, x}; }
EF_INLINE f2 load2(const float* l, const float* r) {
#if EF_NEON
    return vld1_lane_f32(r, vld1_lane_f32(l, vdup_n_f32(0.0f), 0), 1);
#else
    return f2{*l, *r};
#endif
}
EF_INLINE void store2(f2 v, float* l, float* r) {
#if EF_NEON
    vst1_lane_f32(l, v, 0);
    vst1_lane_f32(r, v, 1);
#else
    *l = v[0];
    *r = v[1];
#endif
}
EF_INLINE f2 max2(f2 a, f2 b) {
#if EF_NEON
    return vmax_f32(a, b);
#else
    return f2{a[0] > b[0] ? a[0] : b[0], a[1] > b[1] ? a[1] : b[1]};
#endif
}
// The larger of |lane 0| and |lane 1|, in both.
EF_INLINE f2 absMax2(f2 v) {
#if EF_NEON
    const f2 a = vabs_f32(v);
    return vpmax_f32(a, a);
#else
    const float m = std::max(std::fabs(v[0]), std::fabs(v[1]));
    return f2{m, m};
#endif
}

// A NaN or infinity from the host becomes silence before it reaches a filter or a detector, and
// a finite value past kMaxIn is clamped; anything else passes bit for bit (a select, not
// arithmetic, so even a denormal survives NEON's flush to zero).
EF_INLINE f2 clean2(f2 x) {
#if EF_NEON
    const f2 k = vdup_n_f32(kMaxIn);
    const f2 clamped = vmin_f32(vmax_f32(x, vneg_f32(k)), k);
    return vbsl_f32(vcale_f32(x, k), x, vbsl_f32(vcale_f32(x, vdup_n_f32(FLT_MAX)), clamped, vdup_n_f32(0.0f)));
#else
    auto one = [](float v) { return std::isfinite(v) ? clampf(v, -kMaxIn, kMaxIn) : 0.0f; };
    return f2{one(x[0]), one(x[1])};
#endif
}

// (L, R) to (L, R, 0, 0).
EF_INLINE f4 widen(f2 x) {
#if EF_NEON
    return vcombine_f32(x, vdup_n_f32(0.0f));
#else
    return f4{x[0], x[1], 0.0f, 0.0f};
#endif
}

// --- four lanes --------------------------------------------------------------------------------

EF_INLINE f4 abs4(f4 v) {
#if EF_NEON
    return vabsq_f32(v);
#else
    return max4(v, -v);
#endif
}

// log2(x) for normal x > 0: fastmath.h's log2Fast four at a time (the same polynomial).
EF_INLINE f4 log2Fast4(f4 x) {
#if EF_NEON
    const int32x4_t bits = vreinterpretq_s32_f32(x);
    const f4 e = vcvtq_f32_s32(vsubq_s32(vshrq_n_s32(bits, 23), vdupq_n_s32(127)));
    const f4 m = vreinterpretq_f32_s32(vorrq_s32(vandq_s32(bits, vdupq_n_s32(0x007FFFFF)), vdupq_n_s32(0x3F800000)));
#else
    i4 bits;
    __builtin_memcpy(&bits, &x, sizeof bits);
    const f4 e = __builtin_convertvector((bits >> 23) - 127, f4);
    const i4 mb = (bits & 0x007FFFFF) | 0x3F800000;
    f4 m;
    __builtin_memcpy(&m, &mb, sizeof m);
#endif
    const f4 u = m - splat(1.0f);
    f4 p = splat(-2.584141108e-2f);
    p = p * u + splat(1.217977931e-1f);
    p = p * u - splat(2.779052116e-1f);
    p = p * u + splat(4.575491284e-1f);
    p = p * u - splat(7.181452413e-1f);
    p = p * u + splat(1.442544942e+0f);
    return e + u * p;
}

// The lanes of `a` where k >= len, else those of `b`.
EF_INLINE f4 pastEnd(f4 k, f4 len, f4 a, f4 b) {
#if EF_NEON
    return vbslq_f32(vcgeq_f32(k, len), a, b);
#else
    return k >= len ? a : b;
#endif
}

// Lane shuffles for the crossover: (a0, a1, b0, b1), (a2, a3, b2, b3) and (a0, a1, b2, b3).
EF_INLINE f4 lowHalves(f4 a, f4 b) {
#if EF_NEON
    return vcombine_f32(vget_low_f32(a), vget_low_f32(b));
#else
    return f4{a[0], a[1], b[0], b[1]};
#endif
}
EF_INLINE f4 highHalves(f4 a, f4 b) {
#if EF_NEON
    return vcombine_f32(vget_high_f32(a), vget_high_f32(b));
#else
    return f4{a[2], a[3], b[2], b[3]};
#endif
}
EF_INLINE f4 lowHigh(f4 a, f4 b) {
#if EF_NEON
    return vcombine_f32(vget_low_f32(a), vget_high_f32(b));
#else
    return f4{a[0], a[1], b[2], b[3]};
#endif
}

// (lowL, lowR, ., .) and (midL, midR, highL, highR) to one vector per channel: (low, mid, high, 0).
EF_INLINE void toChannels(f4 low, f4 midHigh, f4& l, f4& r) {
#if EF_NEON
    const float32x4x2_t z = vuzpq_f32(vcombine_f32(vget_low_f32(low), vget_low_f32(midHigh)),
                                      vcombine_f32(vget_high_f32(midHigh), vdup_n_f32(0.0f)));
    l = z.val[0];
    r = z.val[1];
#else
    l = f4{low[0], midHigh[0], midHigh[2], 0.0f};
    r = f4{low[1], midHigh[1], midHigh[3], 0.0f};
#endif
}

// The lane sums of l and r, into *outL and *outR.
EF_INLINE void sum2(f4 l, f4 r, float* outL, float* outR) {
#if EF_NEON
    const f2 s = vpadd_f32(vadd_f32(vget_low_f32(l), vget_high_f32(l)), vadd_f32(vget_low_f32(r), vget_high_f32(r)));
    store2(s, outL, outR);
#else
    *outL = (l[0] + l[2]) + (l[1] + l[3]);
    *outR = (r[0] + r[2]) + (r[1] + r[3]);
#endif
}

// Two or four lanes alike: x in every lane, and |v| under 1e-20 to zero.
template <class V>
EF_INLINE V splatV(float x) {
    if constexpr (sizeof(V) == 8) return splat2(x);
    else return splat(x);
}
template <class V>
EF_INLINE V flushV(V v) {
#if EF_NEON
    if constexpr (sizeof(V) == 8) return vbsl_f32(vcalt_f32(v, vdup_n_f32(1e-20f)), vdup_n_f32(0.0f), v);
    else return vbslq_f32(vcaltq_f32(v, vdupq_n_f32(1e-20f)), vdupq_n_f32(0.0f), v);
#else
    for (int i = 0; i < static_cast<int>(sizeof(V) / sizeof(float)); ++i)
        if (std::fabs(v[i]) < 1e-20f) v[i] = 0.0f;
    return v;
#endif
}

// --- filters -----------------------------------------------------------------------------------

// A Butterworth TPT state-variable filter (Zavalishin; Simper's form) in every lane of V. v1 is
// the band output, v2 the low; the high is x - k v1 - v2, the allpass x - 2k v1.
template <class V>
struct Svf {
    struct Coefs {
        V a1, a2, a3;
    };
    static Coefs coefs(float hz) {
        const float g = std::tan(kPi * hz / kRate);
        const float a1 = 1.0f / (1.0f + g * (g + kButterK));
        return {splatV<V>(a1), splatV<V>(g * a1), splatV<V>(g * g * a1)};
    }

    V ic1{}, ic2{};

    void clear() { ic1 = ic2 = V{}; }
    void flush() {   // once per chunk: a decayed state goes to zero, not on into denormals
        ic1 = flushV(ic1);
        ic2 = flushV(ic2);
    }
    EF_INLINE void tick(V x, const Coefs& c, V& v1, V& v2) {
        const V v3 = x - ic2;
        v1 = c.a1 * ic1 + c.a2 * v3;
        v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = v1 + v1 - ic1;
        ic2 = v2 + v2 - ic2;
    }
};

// --- ramps -------------------------------------------------------------------------------------

// N values moving in straight lines to their targets over a chunk, as common.h's Ramp does, but
// readable at any sample of the ramp (Comp's gain computer reads four at a time).
template <int N>
struct Glide {
    float base[N] = {}, step[N] = {}, target[N] = {};
    int done = 0, len = 0;   // samples into the ramp, and its length (0: at the targets)

    // Value j, k samples into the ramp.
    float at(int j, int k) const { return k >= len ? target[j] : base[j] + step[j] * static_cast<float>(k); }

    void jump(const float* v) {
        for (int j = 0; j < N; ++j) {
            base[j] = target[j] = v[j];
            step[j] = 0.0f;
        }
        done = len = 0;
    }
    void to(const float* v, int n) {
        for (int j = 0; j < N; ++j) {
            base[j] = at(j, done);
            target[j] = v[j];
            step[j] = (target[j] - base[j]) / static_cast<float>(n);
        }
        done = 0;
        len = n;
    }
    void advance(int n) { done = std::min(done + n, len); }
};

} // namespace cmp

class Comp {
public:
    struct Params {
        int mode = 0;                 // 0 Comp, 1 OTT
        // Comp
        float thresholdDb = -18.0f;   // -60..0 (a sine's peak)
        float ratio = 4.0f;           // 1..20
        float attackMs = 10.0f;       // 0.1..100
        float releaseMs = 150.0f;     // 10..2000
        float kneeDb = 6.0f;          // 0..24, the soft knee's whole width
        float scLowCutHz = 20.0f;     // 20..500: a 12 dB / octave high-pass on the detector only; <= 20 = off
        float makeupDb = 0.0f;        // -12..+24: Comp's wet signal, OTT's output
        float mix = 1.0f;             // 0..1 dry / wet (parallel compression)
        // OTT
        float ottDepth = 0.6f;        // 0..1 dry / wet
        float ottTime = 1.0f;         // 0.1..10: multiplies the attack and release times
        float ottUp = 1.0f;           // 0..1 upward compression
        float ottDown = 1.0f;         // 0..1 downward compression
        float ottLowDb = 0.0f, ottMidDb = 0.0f, ottHighDb = 0.0f;   // -12..+12 band gains
    };

    Comp();
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return 0; }

private:
    using Svf2 = cmp::Svf<cmp::f2>;
    using Svf4 = cmp::Svf<f4>;

    void setComp(const Params& p, bool jump);
    void setOtt(const Params& p, bool jump);
    void clearComp();
    void clearOtt();
    void run(int mode, float* L, float* R, int n);
    template <bool Sidechain>
    void runComp(float* L, float* R, int n);
    void runOtt(float* L, float* R, int n);

    // Comp. Glide: threshold, slope (1 - 1 / ratio), half the knee, makeup (log2 units), mix.
    enum : int { THR, SLOPE, KNEE, MAKEUP, MIX, kCompValues };
    cmp::Glide<kCompValues> comp_;
    float attackMs_ = -1.0f, releaseMs_ = -1.0f;
    float ca_ = 1.0f, cr_ = 1.0f;          // the detector's per-sample coefficients
    float peak_ = 0.0f, level_ = 0.0f;     // the detector: peak hold (release), then attack
    bool scOn_ = false;
    float scHz_ = -1.0f;
    Svf2::Coefs sc_ = {};
    Svf2 scf_;                             // the sidechain's low cut, L and R
    float detLevel_[kChunk], gain_[kChunk];  // between the detector pass and the gain pass

    // OTT. Glide: depth, makeup (a gain), up and down slopes, band gains (log2 units). Lanes:
    // low, mid, high (and a spare).
    enum : int { DEPTH, OMAKEUP, UP, DOWN, BAND, kOttValues = BAND + 3 };
    cmp::Glide<kOttValues> ott_;
    Svf4::Coefs xo1_ = {}, xo2_ = {};      // the crossovers at 88.3 Hz and 2.5 kHz
    Svf4 xo_[4];
    f4 oPeak_ = splat(0.0f), oLevel_ = splat(0.0f), oca_ = splat(1.0f), ocr_ = splat(1.0f);
    float ottTime_ = -1.0f;
    f4 bandL_[kChunk], bandR_[kChunk], peaks_[kChunk], levels_[kChunk];   // between the passes

    int mode_ = 0, from_ = 0, fadeLeft_ = 0;   // a mode switch fades from `from_` over a chunk
    bool fresh_ = true;
    float fadeL_[kChunk], fadeR_[kChunk];
};

} // namespace ef
