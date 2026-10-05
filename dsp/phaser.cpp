// Phaser and flanger: see phaser.h.
#include "phaser.h"

#include <cfloat>
#include <cmath>

namespace ef {

namespace {

#if EF_NEON
using Pair = float32x2_t;     // a sample of L and R
using Phase4 = uint32x4_t;    // LFO phases, 32-bit fixed point
#else
typedef float Pair __attribute__((vector_size(8)));
typedef uint32_t Phase4 __attribute__((vector_size(16)));
#endif

constexpr double kTwo32 = 4294967296.0;
constexpr float kHalfPi = 1.57079633f, kQuarterPi = 0.785398163f;

// Out-of-range values clamped, NaN to `nan`.
float clampParam(float x, float lo, float hi, float nan) { return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan); }

// Per-chunk glides: the sweep's centre and span and the stereo offset (20 ms), feedback and mix
// (two 5 ms one-poles in a row: they set off without a corner, which a cross-fade between a
// resonant wet and the dry would show). A jump in the flanger's delay bends the pitch instead of
// skipping across the line, and no jump clicks.
float chunkGlide(float seconds) { return 1.0f - std::exp(-static_cast<float>(kChunk) / (seconds * kRate)); }
const float kGlideSweep = chunkGlide(0.02f);
const float kGlideLevel = chunkGlide(0.005f);
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
float fade(float gate) {
    const float g = clampf(gate, 0.0f, 1.0f);
    return g * g * (3.0f - 2.0f * g);
}

// The sweep in log2: of the allpass's prewarped break frequency w = pi f / rate, or of the delay.
const float kLog2W = std::log2(kPi / kRate);
const float kPhaserLow = kLog2W + std::log2(Phaser::kSweepLowHz), kPhaserHigh = kLog2W + std::log2(Phaser::kSweepHighHz);
const float kPhaserCentre = kLog2W + std::log2(Phaser::kCenterLowHz);
const float kPhaserCentreSpan = std::log2(Phaser::kCenterHighHz / Phaser::kCenterLowHz);
const float kFlangerLow = std::log2(Phaser::kFlangerMin), kFlangerHigh = std::log2(Phaser::kFlangerMax);
const float kFlangerCentre = std::log2(Phaser::kDelayLowMs * kRate / 1000.0f);
const float kFlangerCentreSpan = std::log2(Phaser::kDelayHighMs / Phaser::kDelayLowMs);

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
EF_INLINE Pair load2(const float* p) {
#if EF_NEON
    return vld1_f32(p);
#else
    return Pair{p[0], p[1]};
#endif
}
// a times b's lane K.
template <int K>
EF_INLINE Pair mulLane(Pair a, Pair b) {
#if EF_NEON
    return vmul_lane_f32(a, b, K);
#else
    return a * pair(b[K]);
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
// Lanes under 1e-20 to zero: a dying echo or a silent state stops instead of going on as
// denormals (slow on x86).
EF_INLINE Pair dropTiny(Pair x) {
#if EF_NEON
    return vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(x), vcage_f32(x, vdup_n_f32(1e-20f))));
#else
    return Pair{std::fabs(x[0]) < 1e-20f ? 0.0f : x[0], std::fabs(x[1]) < 1e-20f ? 0.0f : x[1]};
#endif
}

// The feedback's saturator: 4 (u - 4/27 u^3) of u = x / 4, flat from |x| = 6 at 4 (with slope 0
// there); about 1% compression at full scale.
EF_INLINE Pair saturate(Pair x) {
    Pair u = x * pair(0.25f);
#if EF_NEON
    u = vmin_f32(vmax_f32(u, vdup_n_f32(-1.5f)), vdup_n_f32(1.5f));
#else
    u = Pair{clampf(u[0], -1.5f, 1.5f), clampf(u[1], -1.5f, 1.5f)};
#endif
    return u * (pair(4.0f) - pair(16.0f / 27.0f) * u * u);
}

// The LFO's shapes from a phase v in [0, 1) that runs a quarter cycle ahead: the triangle is 0 at
// the LFO's phase 0 and 1 at a quarter; sin(pi / 2 triangle) is the sine (fastmath.h's sinQuarter).
EF_INLINE f4 triangle4(f4 v) { return splat(4.0f) * min4(v, splat(1.0f) - v) - splat(1.0f); }
EF_INLINE f4 sine4(f4 t) {
    const f4 x = splat(kHalfPi) * t, x2 = x * x;
    return x * (splat(1.0f) + x2 * (splat(-1.666666667e-1f) +
                                    x2 * (splat(8.333333333e-3f) + x2 * (splat(-1.984126984e-4f) + x2 * splat(2.755731922e-6f)))));
}
EF_INLINE f4 wrap4(f4 v) { return v - __builtin_convertvector(__builtin_convertvector(v, i4), f4); }   // v >= 0

// tan(x) for |x| <= pi/4: fastmath.h's tanFast without its upper half (an odd polynomial).
EF_INLINE f4 tanQuarter4(f4 x) {
    const f4 t = x * x;
    f4 p = splat(8.657055907e-3f);
    p = p * t + splat(4.348253831e-3f);
    p = p * t + splat(2.366562374e-2f);
    p = p * t + splat(5.362507701e-2f);
    p = p * t + splat(1.333622932e-1f);
    p = p * t + splat(3.333325386e-1f);
    p = p * t + splat(1.0f);
    return p * x;
}

} // namespace

Phaser::Phaser() : line_(static_cast<size_t>(2 * kStride), 0.0f) {
    static_assert(kFlangerMax + 2.0f < static_cast<float>(kLine), "the flanger's longest read fits its line");
}

// The allpasses start from rest after a mode change; the flanger's lines are fed in every mode, so
// a flanger always starts on the input's recent past (a cleared line would bring the input back
// with a jump, one delay after the switch).
void Phaser::clearPaths() {
    for (auto& s : ap_) s[0] = s[1] = 0.0f;
    yl_ = yr_ = 0.0f;
}

void Phaser::reset() {
    clearPaths();
    std::fill(line_.begin(), line_.end(), 0.0f);
    w_ = 0;
    acc_ = 0;
    left_ = 0;
    fresh_ = true;
}

void Phaser::set(const Params& p, const Transport& t) {
    const int want = p.mode < kPhaser4 ? kPhaser4 : (p.mode > kFlanger ? kFlanger : p.mode);
    const float depth = clampParam(p.depth, 0.0f, 1.0f, 0.0f);
    const float center = clampParam(p.center, 0.0f, 1.0f, 0.5f);
    const float fb = clampParam(p.feedback, -0.95f, 0.95f, 0.0f);
    const float stereo = clampParam(p.stereo, 0.0f, 180.0f, 0.0f);
    const float mix = clampParam(p.mix, 0.0f, 1.0f, 0.0f);

    // The LFO: free at the rate, synced at the division's rate at MPC's tempo, and while MPC plays
    // pulled onto the song position's phase. In steady playback they differ by a few 2^-32 of a
    // cycle a chunk (the rate's rounding): the LFO takes the song's phase outright. Up to 1/256 of
    // a cycle (a tempo glitch) it catches up an eighth of the way per chunk. More (a locate, a
    // play from elsewhere, a loop the division doesn't divide) would be a jump in every delay or
    // coefficient: the wet fades out, the LFO jumps while it is silent, and the wet fades back in.
    const double shortest = kLfoDivs[0].beats, longest = kLfoDivs[kNumLfoDivs - 1].beats;
    const double div = p.divBeats >= shortest ? std::min(p.divBeats, longest) : (p.divBeats < shortest ? shortest : 4.0);
    const double bpm = t.bpm >= 20.0 ? std::min(t.bpm, 999.0) : (t.bpm < 20.0 ? 20.0 : 120.0);
    const double hz = p.sync ? bpm / 60.0 / div : clampParam(p.rateHz, 0.01f, 20.0f, 0.3f);
    inc_ = static_cast<uint32_t>(hz / kRate * kTwo32 + 0.5);
    uint32_t locked = acc_;
    bool behind = false;   // a jump to make while the wet is out
    if (p.sync && t.playing && t.valid) {
        const double ph = lockedPhase(t, div);
        if (ph >= 0.0 && ph <= 1.0) {
            locked = static_cast<uint32_t>(static_cast<uint64_t>(ph * kTwo32));   // 1.0 wraps to 0
            const int32_t err = static_cast<int32_t>(locked - acc_);
            const uint32_t size = err < 0 ? 0u - static_cast<uint32_t>(err) : static_cast<uint32_t>(err);
            if (fresh_ || size < (1u << 12)) acc_ = locked;
            else if (size < (1u << 24)) acc_ += static_cast<uint32_t>(err / 8);
            else behind = true;
        }
    }

    // A new mode or a phase jump: the wet fades out, the paths switch or the LFO jumps while it is
    // silent, and it fades back in. Its feedback fades with it, so nothing reaches the flanger's
    // line in a step that would come back a delay later.
    bool snap = fresh_;
    if (fresh_) {
        mode_ = want;
        gate_ = 1.0f;
    } else if ((want != mode_ || behind) && gate_ <= 0.0f && left_ == 0) {
        if (want != mode_) {
            mode_ = want;
            snap = true;
            clearPaths();
        }
        if (behind) acc_ = locked;
        behind = false;
        // Unheard for 4 chunks more: to allpasses starting from rest (or on new coefficients) the
        // input is a sine switched on, broadband for a moment.
        gate_ = -4.0f * kGateStep;
    }
    if (!fresh_) gate_ = want == mode_ && !behind ? std::min(gate_ + kGateStep, 1.0f) : std::max(gate_ - kGateStep, 0.0f);
    const bool flanger = mode_ == kFlanger;
    const float cenT = flanger ? kFlangerCentre + center * kFlangerCentreSpan : kPhaserCentre + center * kPhaserCentreSpan;
    const float spanT = depth * (flanger ? kFlangerOctaves : kPhaserOctaves);
    const float offT = stereo / 360.0f;
    if (fresh_) {
        offGlide_ = offT;
        fbGlide_[0] = fbGlide_[1] = fb;
        mixGlide_[0] = mixGlide_[1] = mix;
    } else {
        offGlide_ = glideTo(offGlide_, offT, kGlideSweep, 1e-5f);
        glide2(fbGlide_, fb, kGlideLevel, 1e-5f);
        glide2(mixGlide_, mix, kGlideLevel, 1e-5f);
    }
    if (snap) {
        cenGlide_ = cenT;
        spanGlide_ = spanT;
    } else {
        cenGlide_ = glideTo(cenGlide_, cenT, kGlideSweep, 1e-4f);
        spanGlide_ = glideTo(spanGlide_, spanT, kGlideSweep, 1e-4f);
    }

    const float target[5] = {cenGlide_, spanGlide_, offGlide_, fbGlide_[1] * fade(gate_), mixGlide_[1] * fade(gate_)};
    Lin* const ramp[5] = {&cen_, &span_, &off_, &fb_, &mix_};
    if (fresh_) {
        for (int k = 0; k < 5; ++k) *ramp[k] = Lin{target[k], 0.0f, target[k]};
        left_ = 0;
    } else {
        if (snap) {
            cen_ = Lin{cenGlide_, 0.0f, cenGlide_};
            span_ = Lin{spanGlide_, 0.0f, spanGlide_};
        }
        bool changed = false;
        for (int k = 0; k < 5; ++k) changed = changed || target[k] != ramp[k]->target;
        // Unchanged targets leave a running ramp alone (with blocks shorter than a chunk, set()
        // comes more often than a ramp's length).
        if (changed) {
            for (int k = 0; k < 5; ++k) {
                ramp[k]->target = target[k];
                ramp[k]->step = (target[k] - ramp[k]->cur) * (1.0f / kChunk);
            }
            left_ = kChunk;
        }
    }
    fresh_ = false;

    // How long it rings: until the feedback has taken a recirculating sound 60 dB down, each pass
    // as long as the loop's delay. The phaser's is longest at the bottom of the sweep, where its
    // allpasses' group delay at DC is 1 / K each (K = tan w); one pass of its impulse response
    // (a gamma-like hump, falling as (1 - 2K)^n) lasts about (10 + 2 N) / (2 K).
    const float passes = std::fabs(fb) > 1e-3f ? 9.966f / -log2Fast(std::fabs(fb)) : 0.0f;   // log2(1000) = 9.966
    if (flanger) {
        const float longest = exp2Fast(std::min(cenT + spanT, kFlangerHigh));
        tail_ = static_cast<int>(longest * (1.0f + passes)) + 4;
    } else {
        const float stages = 4.0f * static_cast<float>(mode_ + 1);
        const float k = tanFast(exp2Fast(std::max(cenT - spanT, kPhaserLow)));
        tail_ = static_cast<int>((10.0f + 2.0f * stages) / (2.0f * k) + passes * (1.0f + stages / k)) + 4;
    }
}

// The chunk's control values per sample as [L, R] pairs: the LFO from its fixed-point phase (the
// right side's offset added; for the phaser its triangle made a sine), the sweep in log2 around the
// centre, 2^ that, and for the phaser the allpass coefficient a = (tan w - 1) / (tan w + 1)
// = tan(w - pi/4). A pass for each, so each one's constants stay in registers; feedback and mix
// ride along with the sweep. Two samples to a vector, everything local (the stores are floats, as
// the members are).
void Phaser::sweep(int n) {
    const bool flanger = mode_ == kFlanger;
    const float lo = flanger ? kFlangerLow : kPhaserLow, hi = flanger ? kFlangerHigh : kPhaserHigh;
    const uint32_t inc = inc_, start = acc_ + 0x40000000u;   // a quarter cycle ahead, as triangle4() takes it
    const Phase4 lanes{0u, 0u, inc, inc};
    const f4 lim = splat(static_cast<float>(left_));
    const f4 cen = splat(cen_.cur), cenStep = splat(cen_.step), span = splat(span_.cur), spanStep = splat(span_.step);
    const f4 off{0.0f, off_.cur, 0.0f, off_.cur}, offStep{0.0f, off_.step, 0.0f, off_.step};
    const f4 fm{fb_.cur, mix_.cur, fb_.cur, mix_.cur}, fmStep{fb_.step, mix_.step, fb_.step, mix_.step};
    float* const ctl = ctl_;
    float* const fbMix = fbMix_;
    for (int i = 0; i < n; i += 2) {
        const float fi = static_cast<float>(i);
        const f4 k = min4(f4{fi + 1.0f, fi + 1.0f, fi + 2.0f, fi + 2.0f}, lim);   // the ramps' progress
        const uint32_t a = start + static_cast<uint32_t>(i) * inc;
        const Phase4 ph = Phase4{a, a, a, a} + lanes;
        store4(ctl + 2 * i, triangle4(wrap4(__builtin_convertvector(ph >> 8, f4) * splat(0x1p-24f) + off + k * offStep)));
    }
    const int end = 2 * n + (n & 1) * 2;   // whole vectors
    if (!flanger)
        for (int j = 0; j < end; j += 4) store4(ctl + j, sine4(load4(ctl + j)));
    for (int i = 0; i < n; i += 2) {
        const float fi = static_cast<float>(i);
        const f4 k = min4(f4{fi + 1.0f, fi + 1.0f, fi + 2.0f, fi + 2.0f}, lim);
        store4(ctl + 2 * i, min4(max4(cen + k * cenStep + (span + k * spanStep) * load4(ctl + 2 * i), splat(lo)), splat(hi)));
        store4(fbMix + 2 * i, fm + k * fmStep);
    }
    for (int j = 0; j < end; j += 4) store4(ctl + j, exp2Fast4(load4(ctl + j)));
    if (!flanger)
        for (int j = 0; j < end; j += 4) store4(ctl + j, tanQuarter4(load4(ctl + j) - splat(kQuarterPi)));
    acc_ += static_cast<uint32_t>(n) * inc;
}

void Phaser::process(float* L, float* R, int n) {
    sweep(n);
    switch (mode_) {
    case kPhaser4: runPhaser<4>(L, R, n); break;
    case kPhaser8: runPhaser<8>(L, R, n); break;
    case kPhaser12: runPhaser<12>(L, R, n); break;
    default: runFlanger(L, R, n); break;
    }

    Lin* const ramp[5] = {&cen_, &span_, &off_, &fb_, &mix_};
    if (left_ > 0) {
        if (n >= left_) {
            for (Lin* r : ramp) *r = Lin{r->target, 0.0f, r->target};
            left_ = 0;
        } else {
            for (Lin* r : ramp) r->cur += static_cast<float>(n) * r->step;
            left_ -= n;
        }
    }
}

// N first-order allpasses per side, L and R side by side in a two-lane vector. A stage in
// transposed direct form: v = a u + s, s' = u - a v. Its state follows s' = (1 - a^2) u - a s, so
// it stays bounded however a moves (|a| < 1); with the saturated feedback the whole loop does.
template <int N>
void Phaser::runPhaser(float* L, float* R, int n) {
    Pair s[N];
    for (int k = 0; k < N; ++k) s[k] = Pair{ap_[k][0], ap_[k][1]};
    Pair y{yl_, yr_};
    float* const buf = line_.data();
    int w = w_;
    const float* const ctl = ctl_;
    const float* const fbMix = fbMix_;
    const bool dryOnly = left_ == 0 && mix_.cur == 0.0f;   // mix 0: the input exactly
    for (int i = 0; i < n; ++i) {
        const Pair x = finite(loadPair(L + i, R + i));
        w = (w + 1) & (kLine - 1);   // the flanger's lines, for a later switch (see clearPaths())
        storePair(buf + w, buf + kStride + w, x);
        if (w < 3) storePair(buf + kLine + w, buf + kStride + kLine + w, x);
        const Pair a = load2(ctl + 2 * i), fm = load2(fbMix + 2 * i);   // fm: feedback, mix
        Pair u = x + mulLane<0>(saturate(y), fm);
        for (int k = 0; k < N; ++k) {
            const Pair v = a * u + s[k];
            s[k] = u - a * v;
            u = v;
        }
        y = u;
        storePair(L + i, R + i, dryOnly ? x : x + mulLane<1>(u - x, fm));
    }
    // States that fell silent go to zero rather than through denormals (slow on x86).
    for (int k = 0; k < N; ++k) {
        s[k] = dropTiny(s[k]);
        ap_[k][0] = s[k][0];
        ap_[k][1] = s[k][1];
    }
    y = dropTiny(y);
    yl_ = y[0];
    yr_ = y[1];
    w_ = w;
}

// Each side's line holds the input plus the saturated feedback. The delay is read before this
// sample is written, so the newest sample in the line is one old: d samples is d - 1 back, read
// as DelayLine::readCubic would. A line's guard repeats its first three samples, so a side's four
// taps are one contiguous load (oldest first), and one transpose step pairs L's and R's.
void Phaser::runFlanger(float* L, float* R, int n) {
    float* const buf = line_.data();
    int w = w_;
    const float* const ctl = ctl_;
    const float* const fbMix = fbMix_;
    const bool dryOnly = left_ == 0 && mix_.cur == 0.0f;
#if EF_NEON
    using Int2 = int32x2_t;
#else
    typedef int32_t Int2 __attribute__((vector_size(8)));
#endif
    for (int i = 0; i < n; ++i) {
        const Pair x = finite(loadPair(L + i, R + i));
        const Pair d = load2(ctl + 2 * i) - pair(1.0f);   // >= 1
        const Int2 k = __builtin_convertvector(d, Int2);
        const Pair t = d - __builtin_convertvector(k, Pair);
        const Int2 p = ((Int2{w - 2, w - 2} - k) & Int2{kLine - 1, kLine - 1}) + Int2{0, kStride};
#if EF_NEON
        const float32x4x2_t r = vtrnq_f32(vld1q_f32(buf + vget_lane_s32(p, 0)), vld1q_f32(buf + vget_lane_s32(p, 1)));
        const Pair x2 = vget_low_f32(r.val[0]), x1 = vget_low_f32(r.val[1]);
        const Pair x0 = vget_high_f32(r.val[0]), xm1 = vget_high_f32(r.val[1]);
#else
        const float *ql = buf + p[0], *qr = buf + p[1];
        const Pair x2{ql[0], qr[0]}, x1{ql[1], qr[1]}, x0{ql[2], qr[2]}, xm1{ql[3], qr[3]};
#endif
        // common.h's hermite() in Laurent de Soras's arrangement: c1 as there, a = c3, b = -c2.
        const Pair c1 = pair(0.5f) * (x1 - xm1), v = x0 - x1, w2 = c1 + v;
        const Pair a = w2 + v + pair(0.5f) * (x2 - x0), b = w2 + a;
        const Pair y = ((a * t - b) * t + c1) * t + x0;

        const Pair fm = load2(fbMix + 2 * i);
        const Pair u = dropTiny(x + mulLane<0>(saturate(y), fm));
        w = (w + 1) & (kLine - 1);
        storePair(buf + w, buf + kStride + w, u);
        if (w < 3) storePair(buf + kLine + w, buf + kStride + kLine + w, u);
        storePair(L + i, R + i, dryOnly ? x : x + mulLane<1>(y - x, fm));
    }
    w_ = w;
}

int Phaser::tailSamples() const { return tail_; }

} // namespace ef
