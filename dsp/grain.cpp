// Grain: see grain.h.
#include "grain.h"
#include "simd.h"

#include <cfloat>
#include <cmath>
#include <cstring>

namespace ef {

namespace {

// L and R side by side in a two-lane vector: NEON's 64-bit registers on the device, GCC's generic
// vectors elsewhere (the x86 tests run the same arithmetic lane by lane).
#if EF_NEON
using Gp = float32x2_t;
#else
typedef float Gp __attribute__((vector_size(8)));
#endif

EF_INLINE Gp both(float x) { return Gp{x, x}; }
EF_INLINE Gp loadLr(const float* l, const float* r) {
#if EF_NEON
    return vld1_lane_f32(r, vld1_dup_f32(l), 1);
#else
    return Gp{*l, *r};
#endif
}
EF_INLINE void storeLr(float* l, float* r, Gp v) {
#if EF_NEON
    vst1_lane_f32(l, v, 0);
    vst1_lane_f32(r, v, 1);
#else
    *l = v[0];
    *r = v[1];
#endif
}
EF_INLINE Gp loadPair(const float* p) {
#if EF_NEON
    return vld1_f32(p);
#else
    return Gp{p[0], p[1]};
#endif
}
EF_INLINE void storePair(float* p, Gp v) {
#if EF_NEON
    vst1_f32(p, v);
#else
    p[0] = v[0];
    p[1] = v[1];
#endif
}
EF_INLINE Gp swapLr(Gp v) {
#if EF_NEON
    return vrev64_f32(v);
#else
    return Gp{v[1], v[0]};
#endif
}
// sanitize() on both lanes.
EF_INLINE Gp finiteLr(Gp x) {
#if EF_NEON
    return vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(x), vcale_f32(x, vdup_n_f32(FLT_MAX))));
#else
    return Gp{sanitize(x[0]), sanitize(x[1])};
#endif
}
EF_INLINE Gp clampLr(Gp x, float lim) {
#if EF_NEON
    return vmin_f32(vmax_f32(x, vdup_n_f32(-lim)), vdup_n_f32(lim));
#else
    return Gp{clampf(x[0], -lim, lim), clampf(x[1], -lim, lim)};
#endif
}
// Lanes under 1e-20 to zero: a texture dying away in the loop stops instead of going on as
// denormals (slow on x86).
EF_INLINE Gp dropTiny(Gp x) {
#if EF_NEON
    return vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(x), vcage_f32(x, vdup_n_f32(1e-20f))));
#else
    return Gp{std::fabs(x[0]) < 1e-20f ? 0.0f : x[0], std::fabs(x[1]) < 1e-20f ? 0.0f : x[1]};
#endif
}

// common.h's hermite() on both sides of an interleaved stereo line: q points at x[-1]'s left
// sample (L-1 R-1 L0 R0 L1 R1 L2 R2), t is the fraction past x0. Laurent de Soras's arrangement
// (one constant, fewer registers): c1 as there, a = c3, b = -c2.
template <class V>
EF_INLINE V cubicOf(V xm1, V x0, V x1, V x2, V tt, V half) {
    const V c1 = half * (x1 - xm1), v = x0 - x1, w = c1 + v;
    const V a = w + v + half * (x2 - x0), b = w + a;
    return ((a * tt - b) * tt + c1) * tt + x0;
}
EF_INLINE Gp cubic(const float* q, float t) {
#if EF_NEON
    const float32x4_t lo = vld1q_f32(q), hi = vld1q_f32(q + 4);
    const Gp xm1 = vget_low_f32(lo), x0 = vget_high_f32(lo), x1 = vget_low_f32(hi), x2 = vget_high_f32(hi);
#else
    const Gp xm1{q[0], q[1]}, x0{q[2], q[3]}, x1{q[4], q[5]}, x2{q[6], q[7]};
#endif
    return cubicOf(xm1, x0, x1, x2, both(t), both(0.5f));
}
// The same for two samples at once, lane by lane the same arithmetic: q and r their windows, t
// their fractions (t0 t0 t1 t1); the result L R of the first, then of the second.
EF_INLINE f4 cubic2(const float* q, const float* r, f4 t) {
#if EF_NEON
    const float32x4_t a = vld1q_f32(q), b = vld1q_f32(q + 4), c = vld1q_f32(r), d = vld1q_f32(r + 4);
    const f4 xm1 = vcombine_f32(vget_low_f32(a), vget_low_f32(c)), x0 = vcombine_f32(vget_high_f32(a), vget_high_f32(c));
    const f4 x1 = vcombine_f32(vget_low_f32(b), vget_low_f32(d)), x2 = vcombine_f32(vget_high_f32(b), vget_high_f32(d));
#else
    const f4 xm1{q[0], q[1], r[0], r[1]}, x0{q[2], q[3], r[2], r[3]}, x1{q[4], q[5], r[4], r[5]}, x2{q[6], q[7], r[6], r[7]};
#endif
    return cubicOf(xm1, x0, x1, x2, t, splat(0.5f));
}
// Each lane twice: (x0 x0 x1 x1) and (x2 x2 x3 x3).
EF_INLINE void store4Twice(float* p, f4 x) {
#if EF_NEON
    const float32x4x2_t z = vzipq_f32(x, x);
    vst1q_f32(p, z.val[0]);
    vst1q_f32(p + 4, z.val[1]);
#else
    for (int i = 0; i < 4; ++i) p[2 * i] = p[2 * i + 1] = x[i];
#endif
}
EF_INLINE f4 swapPairs(f4 v) {
#if EF_NEON
    return vrev64q_f32(v);
#else
    return f4{v[1], v[0], v[3], v[2]};
#endif
}

// The larger magnitude of the two lanes, in both.
EF_INLINE Gp peakLr(Gp v) {
#if EF_NEON
    const float32x2_t m = vabs_f32(v);
    return vpmax_f32(m, m);
#else
    return both(std::max(std::fabs(v[0]), std::fabs(v[1])));
#endif
}
EF_INLINE Gp minLr(Gp a, Gp b) {
#if EF_NEON
    return vmin_f32(a, b);
#else
    return Gp{std::min(a[0], b[0]), std::min(a[1], b[1])};
#endif
}
EF_INLINE float lane0(Gp v) {
#if EF_NEON
    return vget_lane_f32(v, 0);
#else
    return v[0];
#endif
}
// Lane 0 over x, decided in the integer unit.
EF_INLINE bool above(Gp v, float x) {
#if EF_NEON
    return vget_lane_u32(vcgt_f32(v, vdup_n_f32(x)), 0) != 0;
#else
    return v[0] > x;
#endif
}

// The envelope's gain from its ramp a (0..1): sin^2(pi / 2 a). sin as fastmath.h's sinQuarter, its
// last coefficient moved (2.7557e-6 -> 2.6949e-6) so it reaches exactly 1 at pi / 2 instead of
// overshooting by 4e-6 (error 2.6e-7 throughout): a release starting at a ramp a hair under 1
// would otherwise step the gain up by 7e-6. Never over 1.
constexpr float kS3 = -1.666666667e-1f, kS5 = 8.333333333e-3f, kS7 = -1.984126984e-4f, kS9 = 2.694884624e-6f;
EF_INLINE f4 window4(f4 a) {
    const f4 x = a * splat(1.57079633f), x2 = x * x;
    const f4 s = x * (splat(1.0f) + x2 * (splat(kS3) + x2 * (splat(kS5) + x2 * (splat(kS7) + x2 * splat(kS9)))));
    const f4 w = min4(s * s, splat(1.0f));
#if EF_NEON
    return vbslq_f32(vcgeq_f32(a, splat(1.0f)), splat(1.0f), w);   // exactly 1 when sustained
#else
    return a >= splat(1.0f) ? splat(1.0f) : w;
#endif
}
float window1(float a) {
    if (a >= 1.0f) return 1.0f;
    const float x = 1.57079633f * a, x2 = x * x;
    const float s = x * (1.0f + x2 * (kS3 + x2 * (kS5 + x2 * (kS7 + x2 * kS9))));
    return std::min(s * s, 1.0f);
}

// A parameter clamped to its range; NaN (which no comparison catches) becomes `nan`.
template <class T>
T clampOr(T x, T lo, T hi, T nan) {
    return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan);
}

// std::ceil, which is a libm call on ARMv7.
double ceilFast(double x) { return -floorFast(-x); }

// x into [0, size).
double wrapTo(double x, double size) {
    const double r = x - floorFast(x / size) * size;
    return r >= size ? r - size : (r < 0.0 ? 0.0 : r);
}

constexpr double kSr = 44100.0;
constexpr int kMaxAge = 8 * 44100;              // how far back a grain may start or reach: 8 s
constexpr int kKeep = kMaxAge + 22050;          // what stays readable: 0.5 s more, for grains a hold's end overtakes
constexpr int kFrames = (kKeep + 64 + 3) / 4 * 4;   // level 0's ring (a multiple of 4: levels 1 and 2 halve it)
constexpr int kGuard = 256;                     // frames repeated past a ring's end: a call's reads never wrap
constexpr int kOpen = 1 << 24;                  // a slice's end until it is released
constexpr int kTailForever = 1 << 30;

// Margins in level-0 frames: the youngest a read on each level may be (the chunk being rendered is
// written after the grains: 32 frames, a level's pair or quad and the taps, and 2 to spare), and
// how far short of the oldest readable frame it stays.
constexpr double kNewest[3] = {40.0, 48.0, 64.0};
constexpr double kOldest[3] = {4.0, 12.0, 24.0};
// Level L's frame m stands for level-0 time 2^L m + kOffset[L]: the decimator takes the pair
// (2m, 2m + 1) and delays low frequencies by 3.19 input samples (its allpasses' delay at DC).
constexpr double kTau = 3.19;
constexpr double kOffset[3] = {0.0, 1.0 - kTau, 3.0 - 3.0 * kTau};
constexpr float kLevel1 = 1.26f, kLevel2 = 2.2f;   // a grain faster than these reads level 1, 2

constexpr int kFade = 220;          // slice crossfades, the live input's: 5 ms
constexpr int kModeFade = 882;      // a mode change fades every grain out: 20 ms
constexpr int kSafeFade = 441;      // a grain about to leave the readable region: 10 ms
constexpr int kHoldFade = 220;      // the recording's seam at a hold: 5 ms
constexpr double kMinSlice = 0.01 * kSr, kMaxSlice = 2.0 * kSr;
constexpr double kStretchSpeed = 0.125;
constexpr double kJump = 128.0;     // samples: a song position further than this (a host block) from where it should be jumped
constexpr float kDetune = 6.0f;     // Cloud's random detune at spread 1, +- cents
constexpr float kInClamp = 8.0f;    // +18 dBFS: the most the limiter ever sees of the input
constexpr uint32_t kSeed = 0x9e3779b9u;

// Arp's interval patterns (semitones over `pitch`), by density.
constexpr int kPatterns = 5;
constexpr int kPatternLen[kPatterns] = {2, 3, 4, 6, 8};
constexpr int kPattern[kPatterns][8] = {
    {0, 12}, {0, 7, 12}, {0, 7, 12, 19}, {0, 12, 7, 19, 12, 24}, {0, 7, 12, 19, 24, 19, 12, 7}};

// Mix and feedback glide per chunk (two 5 ms one-poles in a row: they set off without a corner).
const float kGlide = 1.0f - std::exp(-static_cast<float>(kChunk) / (0.005f * kRate));
float glideTo(float cur, float target) {
    const float next = cur + (target - cur) * kGlide;
    return std::fabs(target - next) < 1e-5f ? target : next;
}
float glide2(float (&g)[2], float target) {
    g[0] = glideTo(g[0], target);
    g[1] = glideTo(g[1], g[0]);
    return g[1];
}

// The feedback path: one-pole cuts (a = 1 - exp(-2 pi fc / rate)), the limiter's recovery.
const float kLowPass = 1.0f - std::exp(-2.0f * kPi * 12000.0f / kRate);
const float kHighPass = 1.0f - std::exp(-2.0f * kPi * 20.0f / kRate);
const float kRelease = std::exp(1.0f / (0.1f * kRate));

// Folds a step's pitch into what the buffer's levels serve: over +24 down, under -36 up, by octaves.
float fold(float semis) {
    while (semis > 24.0f) semis -= 12.0f;
    while (semis < -36.0f) semis += 12.0f;
    return semis;
}
double speedOf(float semis) { return std::exp2(static_cast<double>(semis) / 12.0); }
int levelOf(double rate) {
    const double s = std::fabs(rate);
    return s > kLevel2 ? 2 : (s > kLevel1 ? 1 : 0);
}

} // namespace

Grain::Grain() {
    for (int l = 0; l < kLevels; ++l) buf_[l].assign(static_cast<size_t>(2 * ((kFrames >> l) + kGuard)), 0.0f);
    reset();
}

void Grain::reset() {
    for (Voice& v : voice_) v.on = false;
    w0_ = -1;
    written_ = 0;
    now_ = 0.0;
    tailKey_.mode = -1;
    speedPitch_ = 0.0f;
    speed_ = 1.0;
    rng_ = kSeed;
    lastBoundary_ = 0.0;
    sliceIndex_ = 0.0;
    expectBeats_ = 0.0;
    holdLag_ = 0.0;
    indexValid_ = false;
    enter_ = dropAll_ = false;
    nextSpawn_ = 0.0;
    head_ = headTime_ = 0.0;
    cap_ = 0.0;
    left_ = repeat_ = 0;
    stuttering_ = false;
    step_ = 0;
    live_ = liveFrom_ = liveTo_ = 0.0f;
    liveK_ = 0;
    lpL_ = lpR_ = hpL_ = hpR_ = 0.0f;
    limit_ = rec_ = 1.0f;
    down1_.reset();
    down2_.reset();
    early0_[0] = early0_[1] = early1_[0] = early1_[1] = 0.0f;
    fresh_ = true;
}

float Grain::uniform() { return static_cast<float>(xorshift(rng_) >> 8) * (1.0f / 16777216.0f); }

void Grain::set(const Params& p, const Transport& t) {
    const int want = std::clamp(p.mode, 0, kModes - 1);
    density_ = clampOr(p.density, 0.0f, 1.0f, 0.5f);
    pitch_ = clampOr(p.pitch, -24.0f, 24.0f, 0.0f);
    reverse_ = clampOr(p.reverse, 0.0f, 1.0f, 0.0f);
    spread_ = clampOr(p.spread, 0.0f, 1.0f, 0.5f);
    const float fb = clampOr(p.feedback, 0.0f, 0.95f, 0.0f);
    const float mix = clampOr(p.mix, 0.0f, 1.0f, 0.5f);

    // The slice (and grain) length: synced, the division at MPC's tempo, halved or doubled into
    // 10 ms..2 s so it stays on the grid; free, the milliseconds. In double: whole ms are whole
    // samples.
    bpm_ = clampOr(t.bpm, 20.0, 999.0, 120.0);
    if (p.sync) {
        double b = clampOr(p.sizeBeats, kDelayDivs[0].beats, kDelayDivs[kNumDelayDivs - 1].beats, 0.25);
        double s = b * 60.0 / bpm_ * kSr;
        while (s > kMaxSlice) {
            b *= 0.5;
            s *= 0.5;
        }
        while (s < kMinSlice) {
            b *= 2.0;
            s *= 2.0;
        }
        sliceBeats_ = b;
        slice_ = s;
    } else {
        slice_ = static_cast<double>(clampOr(p.sizeMs, 10.0f, 1000.0f, 120.0f)) * (kSr / 1000.0);
    }
    locked_ = p.sync && t.playing && t.valid && std::fabs(t.beats) < 1e9;   // false for NaN
    beats_ = locked_ ? t.beats : 0.0;
    if (!locked_) indexValid_ = false;

    if (fresh_) {
        mode_ = want;
        hold_ = p.hold;
        holdLag_ = 0.0;
        mixGlide_[0] = mixGlide_[1] = mix;
        fbGlide_[0] = fbGlide_[1] = fb;
        mix_ = mixEnd_ = mix;
        fb_ = fbEnd_ = fb;
        live_ = mode_ == kStutter && !hold_ ? 1.0f : 0.0f;
        enter_ = true;
        fresh_ = false;
    } else {
        if (want != mode_) {   // every grain fades out, the new mode starts at once
            mode_ = want;
            dropAll_ = true;
            enter_ = true;
            left_ = 0;
            stuttering_ = false;
        }
        if (p.hold != hold_) {
            hold_ = p.hold;
            if (hold_) {
                holdLag_ = std::max(0.0, now_ - lastBoundary_);
                freeze();
                if (mode_ == kStutter && !stuttering_) enter_ = true;   // the live input goes, the last slice repeats
            } else {
                // Recording again: the decimators start from rest and the input fades in, so the
                // seam is a dip.
                rec_ = 0.0f;
                down1_.reset();
                down2_.reset();
                early0_[0] = early0_[1] = early1_[0] = early1_[1] = 0.0f;
                left_ = 0;   // a held stutter ends at the next boundary
            }
        }
        mix_ = mixEnd_;
        fb_ = fbEnd_;
        mixEnd_ = glide2(mixGlide_, mix);
        fbEnd_ = glide2(fbGlide_, fb);
    }

    // How long it rings: whatever a grain may still read of the input (how far back it starts,
    // and its length at its speed), then for feedback until the loop has taken it 60 dB down, a
    // pass at most that long each. (Worked out again only when what it depends on changes.)
    const TailKey key{mode_, pitch_, spread_, density_, fb, slice_, hold_};
    if (key == tailKey_) return;
    tailKey_ = key;
    const double s = slice_;
    float extra = 0.0f;
    if (mode_ == kMosaic) extra = 12.0f;
    if (mode_ == kArp) extra = 24.0f;
    const double speed = speedOf(fold(pitch_ + extra)) * (1.0 + kDetune / 1200.0);
    double reach = 0.0;
    switch (mode_) {
        case kCloud: reach = 0.05 * kSr + static_cast<double>(spread_ * spread_) * kMaxAge; break;
        case kStretch: reach = (1.0 + 5.0 * spread_ + 0.005 + 0.06 * spread_) * kSr; break;
        case kMosaic: reach = (2.0 + std::floor(spread_ * 7.999f)) * s * std::max(speed, 1.0); break;
        case kStutter: reach = (2.0 + 7.0 * density_) * s * std::max(speed, 1.0); break;
        default: reach = (spread_ + speed) * (s + kFade + kSafeFade) + kNewest[0]; break;
    }
    reach = std::min(reach, static_cast<double>(kMaxAge)) + (speed + 1.0) * (s + kFade) + kSafeFade;
    const double passes = fb > 1e-3f ? std::log(1e-3) / std::log(static_cast<double>(fb)) : 0.0;
    tail_ = hold_ ? kTailForever : static_cast<int>(std::min(reach * (1.0 + passes), static_cast<double>(kTailForever)));
}

// Engaging hold: the last 5 ms recorded fade to silence (in place, each level), so a grain that
// reaches the newest frames finds a fade rather than the oldest material in a step.
void Grain::freeze() {
    for (int l = 0; l < kLevels; ++l) {
        const int frames = kFrames >> l;
        const int count = std::min(kHoldFade >> l, written_ >> l);
        float* const b = buf_[l].data();
        int f = ((w0_ + 1) >> l) - 1;   // the level's newest frame
        for (int i = 0; i < count; ++i, --f) {
            if (f < 0) f += frames;
            const float g = window1(static_cast<float>(i) / static_cast<float>(count));
            b[2 * f] *= g;
            b[2 * f + 1] *= g;
            if (f < kGuard) {
                b[2 * (frames + f)] = b[2 * f];
                b[2 * (frames + f) + 1] = b[2 * f + 1];
            }
        }
    }
}

// The level-0 age of ring position p0 at sample k of this call: how far behind the frame that
// sample writes (held: behind the newest).
double Grain::ageAt(double p0, int k) const {
    const double w = hold_ ? static_cast<double>(w0_) : static_cast<double>(w0_) + 1.0 + k;
    return wrapTo(w - p0, kFrames);
}

// Where a grain starting at sample k may start (its level-0 age, lo..hi), reading `span` samples
// at `rate` (level-0 frames per sample): all its taps stay younger than what is readable and older
// than kNewest throughout, recording or held. False: nowhere.
bool Grain::room(double rate, double span, int k, double& lo, double& hi) const {
    const int level = levelOf(rate);
    const double rec = hold_ ? 0.0 : 1.0;
    const double vel = rec - rate;   // how fast its age grows
    const double now = std::min(written_ + rec * (k + 1), static_cast<double>(kMaxAge));
    const double later = std::min(written_ + rec * (k + 1 + span), static_cast<double>(kMaxAge));
    const double margin = 4.0 + kOldest[level];   // 2 to spare over guard()'s, as at the young end
    lo = kNewest[level] + 2.0 + std::max(0.0, -vel * span);
    hi = std::min(now - margin, later - margin - vel * span);
    return lo <= hi;
}

// A new voice at sample k, starting `want` samples back (as near as room() allows; `exact`: there
// or not at all), `length` samples long (kOpen for a slice: until released), fading in and out
// over fadeIn and fadeOut. Panned: the side away from the pan loses gain at constant power (sqrt 2
// at the near side when hard over), and the grain's own stereo narrows toward mono as it moves
// out, so a hard-panned grain keeps both channels' material.
bool Grain::spawn(int k, double want, double rate, double span, int length, int fadeIn, int fadeOut, float pan, float amp,
                  bool slice, bool exact) {
    Voice* v = nullptr;
    for (Voice& c : voice_)
        if (!c.on) {
            v = &c;
            break;
        }
    double lo, hi;
    if (!v || !room(rate, span, k, lo, hi)) return false;
    const double age = std::clamp(want, lo, hi);
    if (exact && std::fabs(age - want) > 0.5) return false;
    const int level = levelOf(rate);
    const double scale = static_cast<double>(1 << level);
    const double w = hold_ ? static_cast<double>(w0_) : static_cast<double>(w0_) + 1.0 + k;
    v->start = wrapTo((w - age - kOffset[level]) / scale, kFrames >> level);
    v->rate = rate / scale;
    v->level = level;
    v->t = -k;
    v->end = slice ? kOpen : length;
    v->relEnd = kOpen;
    v->inSlope = 1.0f / static_cast<float>(std::max(fadeIn, 1));
    v->outSlope = 1.0f / static_cast<float>(std::max(fadeOut, 1));
    v->relSlope = 1.0f;
    v->priors = 0;
    const float pc = clampf(pan, -1.0f, 1.0f), m = 0.5f * std::fabs(pc);
    float gl = 1.0f, gr = 1.0f;
    if (pc != 0.0f) {
        const float th = (pc + 1.0f) * (0.25f * kPi);
        gl = 1.41421356f * std::cos(th);
        gr = 1.41421356f * std::sin(th);
    }
    v->gd[0] = amp * gl * (1.0f - m);
    v->gd[1] = amp * gr * (1.0f - m);
    v->gx[0] = amp * gl * m;
    v->gx[1] = amp * gr * m;
    v->slice = slice;
    v->on = true;
    return true;
}

// The envelope's ramp at the voice's own time t.
float Grain::ramp(const Voice& v, int t) const {
    const float tf = static_cast<float>(t);
    float a = std::min(tf * v.inSlope, (static_cast<float>(v.end) - tf) * v.outSlope);
    a = std::min(a, std::min((static_cast<float>(v.relEnd) - tf) * v.relSlope, 1.0f));
    for (int i = 0; i < v.priors; ++i) a = std::min(a, (static_cast<float>(v.priorEnd[i]) - tf) * v.priorSlope[i]);
    return std::max(a, 0.0f);
}

// Fades a voice out over `fade` samples from sample k on. The release's term starts at the
// envelope's value there and only falls faster than any fade under way (a fade-out ending
// sooner is left alone): from k on it is the lowest line, and before k, where it would be the
// higher, the release it replaces stays in force for the rest of this call. So before k nothing
// changes.
void Grain::release(Voice& v, int k, int fade) {
    const int at = v.t + k;
    if (at <= 0) {   // it hadn't started
        v.on = false;
        return;
    }
    if (std::min(v.end, v.relEnd) - at <= fade) return;
    const float slope = ramp(v, at) / static_cast<float>(fade);
    if (k > 0 && v.relEnd < kOpen && v.priors < 2) {   // (two at most: guard() and an event, after one at 0)
        v.priorEnd[v.priors] = v.relEnd;
        v.priorSlope[v.priors] = v.relSlope;
        ++v.priors;
    }
    v.relSlope = slope;
    v.relEnd = at + fade;
}

void Grain::releaseAll(int k, int fade, bool slicesOnly) {
    for (Voice& v : voice_)
        if (v.on && (v.slice || !slicesOnly)) release(v, k, fade);
}

// Each call, before the new grains: a voice whose path (at the speed the write head moves now)
// would reach the newest frames or the oldest readable one fades out in time. A slice that runs
// longer than planned, a hold, a release of hold, a tempo change: nothing reads a frame it
// shouldn't. The release starts the latest it can and still fade over 10 ms, a time in samples,
// so blocks of any size release it on the same sample.
void Grain::guard(int n) {
    const double rec = hold_ ? 0.0 : 1.0;
    const double grow = !hold_ && written_ < kKeep ? 1.0 : 0.0;   // the oldest frame stays put while filling
    const double readable = std::min(written_ + rec, static_cast<double>(kKeep));
    for (Voice& v : voice_) {
        if (!v.on || v.t < 0) continue;
        // A slice no boundary has ended (the song position stalled) fades out before its own
        // time runs past what the envelope counts exactly, instead of stopping dead there.
        if (v.slice && v.t + n + kSafeFade >= kOpen / 2) release(v, 0, kSafeFade);
        const int level = v.level;
        const double scale = static_cast<double>(1 << level);
        const double age = ageAt((v.start + v.t * v.rate) * scale + kOffset[level], 0);
        const double vel = rec - v.rate * scale;
        const double young = age - kNewest[level], old = readable - 2.0 - kOldest[level] - age;
        if (young < 0.0 || old < 0.0) {   // out already (no path leads here)
            v.on = false;
            continue;
        }
        // Far from both ends for this call (the usual case): no divisions. A sample's margin over
        // the exact test below keeps the decision the same.
        const double far = n + kSafeFade + 1.0;
        if ((vel >= 0.0 || young >= far * -vel) && (vel <= grow || old >= far * (vel - grow))) continue;
        double hit = 1e30;
        if (vel < 0.0) hit = young / -vel;
        if (vel > grow) hit = std::min(hit, old / (vel - grow));
        const double from = hit - kSafeFade;
        if (from >= n) continue;
        const int k = from <= 0.0 ? 0 : static_cast<int>(ceilFast(from));
        release(v, k, std::max(1, static_cast<int>(std::min<double>(kSafeFade, std::floor(hit - k)))));
    }
}

// The slice grid, kept in every mode: the sample of this call a slice starts on (-1: none), and how
// far into its slice the grid is there: under a sample at a boundary (the grid line falls between
// samples), more after a locate. In 4096ths of a sample: the song position MPC reports comes out a
// few 1e-14 beats apart for different block sizes, which must not move a slice's source.
//
// Locked, the song position is checked against where the last call left it: a locate or a loop
// shows as a jump, whether or not it lands in another slice (a loop shorter than a slice, or one
// whose wrap falls on a call's edge, starts a slice at every wrap). A jump by whole slices (a loop
// on the grid wrapping mid-call, just after the boundary it had) moves the slice's number only:
// the slice under way is already the one the grid has there.
int Grain::boundary(int n, double& phase) {
    phase = 0.0;
    const double now = now_;
    const double tol = 1e-6;   // samples: a boundary on a call's edge counts once, whatever the blocks
    const auto at = [&phase](double x) { phase = floorFast(std::max(0.0, x) * 4096.0 + 0.5) * (1.0 / 4096.0); };
    if (locked_) {
        const double b = beats_ / sliceBeats_, expect = expectBeats_ / sliceBeats_;
        expectBeats_ = beats_ + n * bpm_ / (60.0 * kSr);
        const double m = floorFast(b + tol / slice_);
        const auto start = [&](double index) {   // a slice starts here, partway as the grid has it
            indexValid_ = true;
            sliceIndex_ = index;
            at(std::min((b - index) * slice_, slice_));
            lastBoundary_ = now - phase;
            return 0;
        };
        if (!indexValid_) return start(m);   // a start
        const bool due = floorFast(expect + tol / slice_) != sliceIndex_;   // a boundary on the last call's edge
        const double off = b - expect;
        if (std::fabs(off) * slice_ > kJump) {   // a locate or a loop
            const double whole = std::nearbyint(off);
            if (due || std::fabs(off - whole) * slice_ > kJump) return start(m);
            sliceIndex_ = m;
        } else if (m != sliceIndex_) {
            return start(m);
        }
        const double ahead = (m + 1.0 - b) * slice_;
        const int k = std::max(0, static_cast<int>(ceilFast(ahead - tol)));
        if (k >= n) return -1;
        sliceIndex_ = m + 1.0;
        at(k - ahead);
        lastBoundary_ = now + k - phase;
        return k;
    }
    const double ahead = lastBoundary_ + slice_ - now;
    if (ahead <= -1.0) {   // the slices got shorter: one starts now
        lastBoundary_ = now;
        return 0;
    }
    const int k = std::max(0, static_cast<int>(ceilFast(ahead - tol)));
    if (k >= n) return -1;
    lastBoundary_ += slice_;
    at(k - ahead);
    return k;
}

// Cloud and Stretch: Hann grains at intervals of length / overlap, jittered (Cloud +-30%, Stretch
// +-15%), at the absolute sample the interval puts them on.
void Grain::grains(int n) {
    const double now = now_;
    const bool stretch = mode_ == kStretch;
    const int length = std::max(static_cast<int>(slice_ + 0.5), 2);
    const float overlap = stretch ? 2.0f + 6.0f * density_ : 1.0f + 7.0f * density_;
    const float amp = 1.0f / std::sqrt(0.375f * overlap);
    const double interval = length / static_cast<double>(overlap);
    if (pitch_ != speedPitch_) {
        speedPitch_ = pitch_;
        speed_ = speedOf(pitch_);
    }
    const double speed = speed_;
    const double rec = hold_ ? 0.0 : 1.0;
    const double fresh = 0.03 * kSr;   // where Stretch's head starts, behind the present
    const double jitter = (0.005 + 0.06 * spread_) * kSr;
    if (stretch && enter_) {
        head_ = wrapTo(w0_ + rec - fresh, kFrames);
        headTime_ = now;
    }

    if (nextSpawn_ < now - interval) nextSpawn_ = now;
    while (nextSpawn_ < now + n) {
        const int k = nextSpawn_ <= now ? 0 : static_cast<int>(ceilFast(nextSpawn_ - now));
        if (k >= n) break;
        // Every draw first, the same ones whether or not the grain finds room.
        const float u1 = uniform(), u2 = uniform(), u3 = uniform(), u4 = uniform(), u5 = uniform(), u6 = uniform();
        nextSpawn_ += interval * (stretch ? 0.85 + 0.3 * u1 : 0.7 + 0.6 * u1);
        double rate = stretch ? speed : speedOf(pitch_ + kDetune * 0.01f * spread_ * (2.0f * u2 - 1.0f));
        if (u3 < reverse_) rate = -rate;
        double want;
        if (stretch) {
            // The play head at sample k (from where it was last placed, so blocks of any size
            // agree). Recording, it falls behind at 7/8 of real time and starts again from the
            // present once it is far back; held, it moves on through the frozen buffer and starts
            // again from the oldest material once a grain from it would reach the newest.
            const double avail = std::min(written_ + rec * (k + 1), static_cast<double>(kMaxAge));
            double age = ageAt(head_ + (now + k - headTime_) * kStretchSpeed, k);
            double to = -1.0;
            if (!hold_) {
                if (age > (1.0 + 5.0 * spread_) * kSr || age > avail) to = fresh;
            } else {
                const double nearest = kNewest[0] + 2.0 + length * speed;
                if (age < nearest || age > avail) to = std::max(nearest, avail - length * (speed + 1.0) - jitter - 64.0);
            }
            if (to >= 0.0) {
                age = to;
                head_ = wrapTo(w0_ + rec * (1.0 + k) - age, kFrames);
                headTime_ = now + k;
            }
            want = age + jitter * u4;
        } else {
            want = (0.02 + 0.03 * u4) * kSr + static_cast<double>(spread_ * spread_) * kMaxAge * u5;
        }
        spawn(k, want, rate, length, length, length / 2, length - length / 2, spread_ * (2.0f * u6 - 1.0f), amp, false, false);
    }
}

// Mosaic: one of the last few grid slices, maybe reversed, maybe an octave away. Slice j's source
// starts j slices before the grid line and keeps the grid: forward, its start plays on the line;
// reversed, its end. Of the slices whose source is all there in time (a pitched-up one from far
// enough back that it never catches up with the input) and still recorded, the nearest 1 + 7 x
// spread are the candidates; partway into a slice (a locate), the source is where the grid has it.
void Grain::mosaic(int k, double phase) {
    const double s = slice_;
    const int fade = std::min(kFade, static_cast<int>(s / 4.0));
    releaseAll(k, fade, true);
    const float u1 = uniform(), u2 = uniform(), u3 = uniform(), u4 = uniform(), u5 = uniform();
    float semis = pitch_;
    if (u2 < 0.7f * density_) semis += u3 < 0.5f ? 12.0f : -12.0f;
    const double speed = speedOf(fold(semis));
    const bool back = u4 < reverse_;
    const double rate = back ? -speed : speed, life = s - phase + fade + kSafeFade;
    // Held, ages count from the newest frame, which lies holdLag_ - 1 after the grid line.
    const double shift = (back ? phase * (1.0 + speed) - speed * s : phase * (1.0 - speed)) + (hold_ ? holdLag_ - 1.0 : 0.0);
    double lo, hi;
    if (!room(rate, life, k, lo, hi)) return;
    const int first = std::max(1, static_cast<int>(ceilFast((lo - shift) / s - 1e-9)));
    const int last = static_cast<int>(std::floor((hi - shift) / s + 1e-9));
    if (last < first) return;
    const int upto = std::min(last, std::max(first, 1 + static_cast<int>(spread_ * 7.999f)));
    const int j = std::min(upto, first + static_cast<int>(u1 * static_cast<float>(upto - first + 1)));
    spawn(k, j * s + shift, rate, life, kOpen, fade, fade, 0.8f * spread_ * (2.0f * u5 - 1.0f), 1.0f, true, true);
}

// Stutter: on a boundary, maybe start repeating the slice just played (the last max(1, speed)
// slices' worth, so a pitched repeat never catches up with the input), or play its next repeat, or
// let the live input back in. Held, the slice just played is the last one on the grid before the
// recording stopped.
void Grain::stutter(int k, double phase) {
    const double s = slice_;
    const int fade = std::min(kFade, static_cast<int>(s / 4.0));
    const float u1 = uniform(), u2 = uniform(), u3 = uniform();
    const double speed = speedOf(pitch_);
    bool repeat = false;
    if (stuttering_ && (hold_ || left_ > 0)) {
        if (!hold_) --left_;
        repeat = true;
    } else {
        stuttering_ = false;
        if (hold_ || u1 < density_) {
            const double line = hold_ ? w0_ - (holdLag_ - 1.0) : w0_ + 1.0 + k - phase;   // the grid line
            cap_ = wrapTo(line - std::max(1.0, speed) * s, kFrames);
            left_ = static_cast<int>(u2 * (1.0f + 7.0f * density_));
            repeat_ = 0;
            stuttering_ = repeat = true;
        }
    }
    releaseAll(k, fade, true);
    if (repeat) {
        const bool back = u3 < reverse_;
        const double rate = back ? -speed : speed, life = s - phase + fade + kSafeFade;
        const double from = ageAt(cap_, k);
        double want = back ? from - speed * s + phase * speed : from - phase * speed;
        // Forward, exactly a slice on the grid or no repeat; backwards, as near as there is room.
        // A slice whose end is too young to play through (held just after a grid line, or short
        // and pitched up: the repeat would catch up with the newest frames) gives way to the
        // nearest earlier one that can, for the rest of the stutter.
        double lo, hi;
        if (!back && room(rate, life, k, lo, hi) && want < lo - 0.5) {
            const double steps = std::ceil((lo - 0.5 - want) / s);   // (rare: once a stutter)
            if (want + steps * s <= hi + 0.5) {
                cap_ = wrapTo(cap_ - steps * s, kFrames);
                want += steps * s;
            }
        }
        const float pan = 0.6f * spread_ * (repeat_ & 1 ? 1.0f : -1.0f);
        ++repeat_;
        if (!spawn(k, want, rate, life, kOpen, fade, fade, pan, 1.0f, true, !back)) stuttering_ = false;
    }
    const float live = stuttering_ || hold_ ? 0.0f : 1.0f;
    if (k == 0) liveFrom_ = live;
    liveTo_ = live;
    liveK_ = k;
}

// Arp: a note at the pattern's next step, of what came in during the step before (up to 1 + spread
// steps back; reading the newest input instead, feedback would make a comb a millisecond long),
// overlapping the next note by the crossfade, its last 30% (at least the crossfade) fading out.
// With feedback the steps stack: each note plays the last one again, a step higher. Locked to the
// song position, the step is the slice's number.
void Grain::arp(int k, double phase) {
    const double s = slice_;
    const int fade = std::min(kFade, static_cast<int>(s / 4.0));
    const float u1 = uniform(), u2 = uniform();
    const int pat = std::min(kPatterns - 1, static_cast<int>(density_ * kPatterns));
    const int64_t len = kPatternLen[pat];
    const int64_t step = locked_ ? static_cast<int64_t>(sliceIndex_) : step_++;
    const int64_t at = ((step % len) + len) % len;
    const double speed = speedOf(fold(pitch_ + static_cast<float>(kPattern[pat][at])));
    const bool back = u1 < reverse_;
    const int length = std::max(static_cast<int>(s - phase + 0.5) + fade, 2 * fade);
    const int fadeOut = std::min(std::max(static_cast<int>(0.3 * s), fade), length / 2);
    const double want = (1.0 + spread_ * u2) * s + phase * (1.0 - speed);
    const float pan = 0.7f * spread_ * (at & 1 ? 1.0f : -1.0f);
    spawn(k, want, back ? -speed : speed, length + kSafeFade, length, fade, fadeOut, pan, 1.0f, false, false);
}

void Grain::process(float* L, float* R, int n) {
    if (n <= 0) return;
    std::fill(acc_, acc_ + 2 * n, 0.0f);
    if (dropAll_) {
        releaseAll(0, kModeFade, false);
        dropAll_ = false;
    }
    guard(n);

    // This call's grains, slices and notes. The grid runs in every mode, so a slice mode starts
    // in step.
    const double entry = now_ - lastBoundary_;
    double phase = 0.0;
    const int kb = boundary(n, phase);
    liveFrom_ = liveTo_ = mode_ == kStutter && !stuttering_ && !hold_ ? 1.0f : 0.0f;
    liveK_ = n;
    if (mode_ == kCloud || mode_ == kStretch) {
        grains(n);
    } else {
        void (Grain::*event)(int, double) = mode_ == kMosaic ? &Grain::mosaic : (mode_ == kStutter ? &Grain::stutter : &Grain::arp);
        if (enter_ && kb != 0) (this->*event)(0, std::clamp(entry, 0.0, std::max(0.0, slice_ - 1.0)));
        if (kb >= 0) (this->*event)(kb, phase);
    }
    enter_ = false;

    for (Voice& v : voice_)
        if (v.on) render(v, n);

    // The output, and the input with the feedback into level 0; its new frames also into `frames`
    // for the decimators, which run after it (each in a loop of its own: its state in registers).
    const float inv = 1.0f / static_cast<float>(n);
    const float mixStep = (mixEnd_ - mix_) * inv, fbStep = (fbEnd_ - fb_) * inv;
    float mix = mix_, fb = fb_, live = live_, rec = rec_;
    Gp limit = both(limit_);
    bool fadeIn = rec < 1.0f;
    const bool liveOn = live > 0.0f || liveFrom_ > 0.0f || liveTo_ > 0.0f;
    const bool recording = !hold_;
    Gp lp = Gp{lpL_, lpR_}, hp = Gp{hpL_, hpR_};
    float* const b0 = buf_[0].data();
    alignas(16) float frames[2 * kChunk];
    const int first = w0_ + 1 == kFrames ? 0 : w0_ + 1;   // level 0's first new frame
    int w = w0_;
    for (int k = 0; k < n; ++k) {
        mix += mixStep;
        fb += fbStep;
        const Gp x = finiteLr(loadLr(L + k, R + k));
        const Gp wet = loadPair(acc_ + 2 * k);
        Gp y = wet;
        if (liveOn) {   // it moves from the sample after an event, as a voice's envelope does
            y += x * both(window1(live));
            const float target = k < liveK_ ? liveFrom_ : liveTo_;
            live = target > live ? std::min(target, live + 1.0f / kFade) : std::max(target, live - 1.0f / kFade);
        }
        storeLr(L + k, R + k, x * both(1.0f - mix) + y * both(mix));   // exactly dry at 0, wet at 1
        if (!recording) continue;

        lp += both(kLowPass) * (wet - lp);
        hp += both(kHighPass) * (lp - hp);
        Gp v = clampLr(x, kInClamp) + (lp - hp) * both(fb);
        if (fadeIn) {   // after a hold: the recording fades in
            rec = std::min(1.0f, rec + 1.0f / kHoldFade);
            v *= both(window1(rec));
            fadeIn = rec < 1.0f;
        }
        // The limiter: the gain recovers, then drops at once to what keeps this sample at 1. (In
        // both lanes of a vector: VFP has no min, and its compares stall on the flags.)
        const Gp a = peakLr(v);
        limit = minLr(limit * both(kRelease), both(1.0f));
        if (above(a * limit, 1.0f)) limit = both(1.0f / lane0(a));
        v = dropTiny(v * limit);

        w = w + 1 == kFrames ? 0 : w + 1;
        storePair(b0 + 2 * w, v);
        if (w < kGuard) storePair(b0 + 2 * (kFrames + w), v);
        storePair(frames + 2 * k, v);
    }
    if (recording) {   // level 1 from level 0's new frames, level 2 from level 1's (in place)
        const int made = decimate<1>(down1_, early0_, frames, first, n, frames);
        decimate<2>(down2_, early1_, frames, first >> 1, made, frames);
    }
    lp = dropTiny(lp);
    hp = dropTiny(hp);
    lpL_ = lp[0];
    lpR_ = lp[1];
    hpL_ = hp[0];
    hpR_ = hp[1];
    live_ = live;
    rec_ = rec;
    limit_ = lane0(limit);
    if (recording) {
        w0_ = w;
        written_ = std::min(written_ + n, kKeep);
    }

    for (Voice& v : voice_)
        if (v.on) {
            v.priors = 0;   // the latest release is the lowest line from here on
            v.t += n;
            if (v.t >= std::min(v.end, v.relEnd) || v.t > kOpen / 2) v.on = false;
        }
    now_ += n;
}

// A level of the buffer from the one above it: `count` frames of that one (L R), the first at
// `first` in its ring. Each pair of frames (even, odd) makes one frame of this level, into its
// ring and, packed, into `out` (which may be `in`: it is written behind what is read); a pair
// begun in the last call finishes with its early frame from there (`early`), one this call
// begins waits there. Returns how many it made.
template <int Level>
int Grain::decimate(StereoDecimator& down, float (&early)[2], const float* in, int first, int count, float* out) {
    constexpr int frames = kFrames >> Level;
    float* const b = buf_[Level].data();
    StereoDecimator d = down;
    int made = 0, i = 0, f = first >> 1;   // f: the frame a pair makes
    const auto put = [&](Gp lr) {
        storePair(b + 2 * f, lr);
        if (f < kGuard) storePair(b + 2 * (frames + f), lr);
        storePair(out + 2 * made, lr);
        ++made;
        f = f + 1 == frames ? 0 : f + 1;
    };
    if ((first & 1) && count > 0) {
        float l, r;
        d.process(f4{early[0], in[0], early[1], in[1]}, l, r);
        put(Gp{l, r});
        i = 1;
    }
    for (; i + 2 <= count; i += 2) {
#if EF_NEON
        put(d.processPair(vld1q_f32(in + 2 * i)));
#else
        float l, r;
        d.process(f4{in[2 * i], in[2 * i + 2], in[2 * i + 1], in[2 * i + 3]}, l, r);
        put(Gp{l, r});
#endif
    }
    if (i < count) {
        early[0] = in[2 * i];
        early[1] = in[2 * i + 1];
    }
    down = d;
    return made;
}

// A voice's part of this call: its positions and envelope four samples at a time, then the reads.
// Positions go by the voice's own 32-sample segments: the segment's first position (in double) as
// a whole frame `base` and a float offset that stays at least 1 across the segment (so truncation
// is the floor), base moved into the ring (the guard frames past its end take what runs over). A
// read's offset depends only on where in its segment it falls, so blocks of any size compute every
// read alike.
void Grain::render(Voice& v, int n) {
    constexpr int kSeg = 32;
    const int t0 = v.t;
    const int k0 = std::max(0, -t0);
    const int k1 = std::min(n, std::min(v.end, v.relEnd) - t0);
    if (k1 <= k0) return;
    const int level = v.level;
    const int frames = kFrames >> level;
    const f4 lane = f4{0.0f, 1.0f, 2.0f, 3.0f}, four = splat(4.0f);
    const f4 rv = splat(static_cast<float>(v.rate));
    // The positions. Sample counts as floats are exact (under 2^24), so stepping them by 4 is
    // converting each.
    for (int k = k0; k < k1;) {
        const int seg = (t0 + k) & ~(kSeg - 1);   // the voice's own time, >= 0 here
        const int s0 = seg - t0, stop = std::min(k1, s0 + kSeg);
        const double p = v.start + static_cast<double>(seg) * v.rate;
        // (Backwards, two frames below: the last read's offset is 1 in exact arithmetic, a hair
        // under it in float, which must not truncate to 0.)
        const double base = v.rate < 0.0 ? floorFast(p + (kSeg - 1) * v.rate) - 2.0 : floorFast(p) - 1.0;
        int ib = static_cast<int>(base);   // a whole number, into the ring in integers
        if (ib >= frames || ib < 0) {   // (a voice that has run a while)
            ib %= frames;
            if (ib < 0) ib += frames;
        }
        const f4 fv = splat(static_cast<float>(p - base));
        const i4 first = i4{2 * (ib - 1), 2 * (ib - 1), 2 * (ib - 1), 2 * (ib - 1)};
        // Four at a time; a group running past `stop` is written over by the next stretch.
        f4 at = splat(static_cast<float>(k - s0)) + lane;
        for (int j = k; j < stop; j += 4, at += four) {
            const f4 o = fv + at * rv;
            const i4 io = __builtin_convertvector(o, i4);
            store4Twice(frac_ + 2 * (j - k0), o - __builtin_convertvector(io, f4));
            const i4 ix = first + io + io;
#if EF_NEON
            vst1q_s32(idx_ + (j - k0), ix);
#else
            std::memcpy(idx_ + (j - k0), &ix, sizeof ix);
#endif
        }
        k = stop;
    }
    // The envelope (a loop of its own: together they run out of NEON registers).
    {
        const f4 inS = splat(v.inSlope), outS = splat(v.outSlope), relS = splat(v.relSlope);
        const f4 endF = splat(static_cast<float>(v.end)), relF = splat(static_cast<float>(v.relEnd));
        f4 tau = splat(static_cast<float>(t0 + k0)) + lane;
        if (v.priors == 0) {
            for (int j = k0; j < k1; j += 4, tau += four) {
                // (Clamped at 1 by window4 itself.)
                const f4 a = max4(min4(min4(tau * inS, (endF - tau) * outS), (relF - tau) * relS), splat(0.0f));
                store4Twice(env_ + 2 * (j - k0), window4(a));
            }
        } else {   // a release partway into this call (rare): the lines it replaced too
            const bool two = v.priors > 1;
            const f4 p0S = splat(v.priorSlope[0]), p0F = splat(static_cast<float>(v.priorEnd[0]));
            const f4 p1S = splat(two ? v.priorSlope[1] : 1.0f), p1F = splat(static_cast<float>(two ? v.priorEnd[1] : kOpen));
            for (int j = k0; j < k1; j += 4, tau += four) {
                f4 a = min4(min4(tau * inS, (endF - tau) * outS), (relF - tau) * relS);
                a = min4(a, min4((p0F - tau) * p0S, (p1F - tau) * p1S));
                store4Twice(env_ + 2 * (j - k0), window4(max4(a, splat(0.0f))));
            }
        }
    }
    const float* const buf = buf_[level].data();
    if (v.gx[0] != 0.0f || v.gx[1] != 0.0f) readVoice<true>(v, buf, k0, k1 - k0);
    else readVoice<false>(v, buf, k0, k1 - k0);
}

// Two samples a step (a vector holds L R of each); an odd one at the end on its own.
template <bool Cross>
void Grain::readVoice(const Voice& v, const float* buf, int k0, int count) {
    const f4 gd = f4{v.gd[0], v.gd[1], v.gd[0], v.gd[1]}, gx = f4{v.gx[0], v.gx[1], v.gx[0], v.gx[1]};
    float* const acc = acc_ + 2 * k0;
    const int* const idx = idx_;
    const float* const frac = frac_;
    const float* const env = env_;
    int j = 0;
    for (; j + 2 <= count; j += 2) {
        const f4 y = cubic2(buf + idx[j], buf + idx[j + 1], load4(frac + 2 * j));
        f4 g = gd * y;
        if (Cross) g += gx * swapPairs(y);
        store4(acc + 2 * j, load4(acc + 2 * j) + g * load4(env + 2 * j));
    }
    if (j < count) {
        const Gp y = cubic(buf + idx[j], frac[2 * j]);
        Gp g = Gp{v.gd[0], v.gd[1]} * y;
        if (Cross) g += Gp{v.gx[0], v.gx[1]} * swapLr(y);
        storePair(acc + 2 * j, loadPair(acc + 2 * j) + g * both(env[2 * j]));
    }
}

} // namespace ef
