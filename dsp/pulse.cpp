// Pulse: see pulse.h.
#include "pulse.h"

#include <cfloat>
#include <cmath>

namespace ef {

namespace {

// Four phases in 32-bit fixed point (2^32 to a turn).
#if EF_NEON
using Turns4 = uint32x4_t;
#else
typedef uint32_t Turns4 __attribute__((vector_size(16)));
#endif

constexpr double kTwo64 = 18446744073709551616.0;
// One period (an LFO cycle, a gate step) of the phase. A slip under 2^-20 of a period (rounding)
// is taken outright, one under 1/256 caught up an eighth per chunk; more is a jump.
constexpr uint64_t kPeriod = uint64_t{1} << 60;
constexpr uint64_t kSnap = kPeriod >> 20, kSlip = kPeriod >> 8;
constexpr uint32_t kStepFrac = 0x0FFFFFFFu;   // the position within a step, of the phase's top 32 bits
constexpr float kStepUnit = 268435456.0f;     // 2^28: a whole step in those

constexpr float kHalfPi = 1.57079633f, kQuarterPi = 0.785398163f, kSqrt2 = 1.41421356f;

// Out-of-range values clamped, NaN to `nan`.
float clampParam(float x, float lo, float hi, float nan) { return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan); }

// The patterns as masks: bit k for step k.
struct Masks {
    uint32_t m[Pulse::kPatterns] = {};
    constexpr Masks() {
        for (int p = 0; p < Pulse::kPatterns; ++p)
            for (int k = 0; k < 16; ++k)
                if (Pulse::kPatternSteps[p][k] == 'x') m[p] |= 1u << k;
    }
};
constexpr Masks kMasks;

constexpr bool wellFormed() {
    for (const char* s : Pulse::kPatternSteps) {
        int n = 0;
        while (s[n] == 'x' || s[n] == '.') ++n;
        if (n != 16 || s[n] != '\0') return false;
    }
    for (const char* s : Pulse::kPatternNames) {
        int n = 0;
        while (s[n] != '\0') ++n;
        if (n == 0 || n > 10) return false;
    }
    return true;
}
static_assert(wellFormed(), "16 steps of 'x' or '.' per pattern, names of 1 to 10 characters");

// Per-chunk glides: depth and mix through two 5 ms one-poles in a row (they set off without a
// corner), the shape and the stereo offset through one of 20 ms.
float chunkGlide(float seconds) { return 1.0f - std::exp(-static_cast<float>(kChunk) / (seconds * kRate)); }
const float kGlideLevel = chunkGlide(0.005f);
const float kGlideShape = chunkGlide(0.02f);
float glideTo(float cur, float target, float k, float snap) {
    const float next = cur + (target - cur) * k;
    return std::fabs(target - next) < snap ? target : next;
}
void glide2(float (&g)[2], float target, float k, float snap) {
    g[0] = glideTo(g[0], target, k, snap);
    g[1] = glideTo(g[1], g[0], k, snap);
}

EF_INLINE Turns4 loadTurns4(const uint32_t* p) {
#if EF_NEON
    return vld1q_u32(p);
#else
    return Turns4{p[0], p[1], p[2], p[3]};
#endif
}

// sanitize() on four lanes.
EF_INLINE f4 finite4(f4 x) {
#if EF_NEON
    return vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(x), vcaleq_f32(x, vdupq_n_f32(FLT_MAX))));
#else
    return f4{sanitize(x[0]), sanitize(x[1]), sanitize(x[2]), sanitize(x[3])};
#endif
}

// sin(x) for |x| <= pi/2: fastmath.h's sinQuarter, four at a time.
EF_INLINE f4 sinQuarter4(f4 x) {
    const f4 x2 = x * x;
    return x * (splat(1.0f) +
                x2 * (splat(-1.666666667e-1f) + x2 * (splat(8.333333333e-3f) + x2 * (splat(-1.984126984e-4f) + x2 * splat(2.755731922e-6f)))));
}

// The LFO (pulse.h) from its phase in fixed point, a quarter cycle ahead: there the triangle is
// 4 min(v, 1 - v) - 1. Clamped k times steeper, then bent toward its sine by w.
EF_INLINE f4 lfo4(Turns4 ph, f4 k, f4 w) {
    const f4 v = __builtin_convertvector(ph >> 8, f4) * splat(0x1p-24f);
    const f4 tri = splat(4.0f) * min4(v, splat(1.0f) - v) - splat(1.0f);
    const f4 c = min4(max4(k * tri, splat(-1.0f)), splat(1.0f));
    return c + w * (sinQuarter4(splat(kHalfPi) * c) - c);
}

// The ramps' progress for samples i + 1 .. i + 4, at most `lim`.
EF_INLINE f4 progress4(int i, f4 lim) {
    const float fi = static_cast<float>(i);
    return min4(f4{fi + 1.0f, fi + 2.0f, fi + 3.0f, fi + 4.0f}, lim);
}

} // namespace

Pulse::Pulse() { reset(); }

void Pulse::reset() {
    periodKey_ = PeriodKey{};
    incDen_ = -1.0;
    cur_ = Voice{};
    old_ = Voice{};
    fade_ = kFadeSamples;
    left_ = 0;
    fresh_ = true;
}

bool Pulse::gateOpen(const Voice& v) const {
    const uint32_t hi = static_cast<uint32_t>(v.acc >> 32), s = hi >> 28;
    const bool next = (v.pending >> ((s + 1) & 15u)) & 1u;
    return ((v.mask >> s) & 1u) && ((hi & kStepFrac) < gateOff_ || (legato_ && next));
}

// A gate starting here: the pattern from now on, open or closed as the step is.
void Pulse::startGate(Voice& v) const {
    v.step = static_cast<uint32_t>(v.acc >> 60);
    v.mask = v.pending;
    v.env = gateOpen(v) ? 1.0f : 0.0f;
}

void Pulse::set(const Params& p, const Transport& t) {
    const int want = p.mode < kTremolo ? kTremolo : (p.mode > kGate ? kGate : p.mode);
    const float rate = clampParam(p.rateHz, kMinRateHz, kMaxRateHz, 4.0f);
    const double div = p.divBeats >= kMinDivBeats ? std::min(p.divBeats, kMaxDivBeats) : (p.divBeats < kMinDivBeats ? kMinDivBeats : 0.25);
    const float depth = clampParam(p.depth, 0.0f, 1.0f, 1.0f);
    const float shape = clampParam(p.shape, 0.0f, 1.0f, 0.0f);
    const float stereo = clampParam(p.stereo, 0.0f, 180.0f, 0.0f);
    const int pattern = p.pattern < 0 ? 0 : (p.pattern >= kPatterns ? kPatterns - 1 : p.pattern);
    const float length = clampParam(p.length, kMinLength, 1.0f, 0.5f);
    const float smooth = clampParam(p.smooth, kMinSmoothMs, kMaxSmoothMs, 3.0f);
    const float mix = clampParam(p.mix, 0.0f, 1.0f, 1.0f);
    const double bpm = t.bpm >= 20.0 ? std::min(t.bpm, 999.0) : (t.bpm < 20.0 ? 20.0 : 120.0);

    // Synced and playing, the phase is the song position over 16 periods. In steady playback the
    // two differ by rounding only: the phase takes the song's outright. A tempo glitch is caught
    // up an eighth per chunk. A jump would step every gain: the gains cross-fade from the phase
    // running on to the song's (one jump at a time: a jump during a fade waits for its end).
    const bool locking = p.sync && t.playing && t.valid;
    uint64_t locked = 0;
    bool have = false;
    if (locking) {
        const double c = t.beats / (16.0 * div), ph = c - floorFast(c);
        if (ph >= 0.0 && ph < 1.0) {
            locked = static_cast<uint64_t>(ph * kTwo64);
            have = true;
        }
    }
    bool jump = false;
    if (have && !fresh_) {
        const uint64_t err = locked - cur_.acc;   // mod 2^64
        const uint64_t size = err >> 63 ? 0 - err : err;
        if (size < kSnap) cur_.acc = locked;
        else if (size < kSlip) cur_.acc += static_cast<uint64_t>(static_cast<int64_t>(err) / 8);
        else jump = true;
    }
    const bool handOver = !fresh_ && fade_ >= kFadeSamples && (want != cur_.mode || jump);
    if (handOver) {   // the old voice runs on as it is: its phase, rate and square
        old_ = cur_;
        old_.inc = inc_;
        old_.k = k_.cur;
        fade_ = 0;
    }

    // A period in samples (the phase turns once in 16 of them). It glides in log2, unless the
    // song position leads or a voice starts.
    if (p.sync != periodKey_.sync || div != periodKey_.div || bpm != periodKey_.bpm || rate != periodKey_.rate) {
        periodKey_ = {p.sync, div, bpm, rate};   // (two divisions: only when these change)
        period_ = p.sync ? div * 60.0 / bpm * kRate : kRate / rate;
    }
    const double period = period_;
    const float logPeriod = log2Fast(static_cast<float>(period));
    if (fresh_ || handOver || locking) logPeriod_ = logPeriod;
    else logPeriod_ = glideTo(logPeriod_, logPeriod, kGlideShape, 1e-5f);
    const bool there = logPeriod_ == logPeriod;
    const float steps = there ? static_cast<float>(period) : exp2Fast(logPeriod_);
    const double den = there ? period : static_cast<double>(steps);
    if (den != incDen_) {   // (a division and a 64-bit conversion, done in software on ARMv7)
        incDen_ = den;
        inc_ = static_cast<uint64_t>(kTwo64 / 16.0 / den);
    }

    // The gate's edges (pulse.h): the slope, and where an open step starts closing so that it is
    // closed at `length` of the step, or halfway through its open part if two edges don't fit.
    const float smoothS = smooth * 0.001f * kRate;
    const float edge = smoothS + shape * std::max(0.0f, 0.5f * length * steps - smoothS);
    slew_ = 1.0f / edge;
    gateOff_ = static_cast<uint32_t>(std::max(length - edge / steps, 0.5f * length) * kStepUnit);
    legato_ = (1.0f - length) * steps < 0.5f;   // no gap to speak of between open steps

    cur_.pending = kMasks.m[pattern];
    if (fresh_) {
        cur_.mode = want;
        if (have) cur_.acc = locked;
        startGate(cur_);
        fade_ = kFadeSamples;
    } else if (handOver) {
        if (jump) cur_.acc = locked;
        if (want != cur_.mode) {
            cur_.mode = want;
            startGate(cur_);
        } else {   // a jump: the gate's state carries on from where it was, toward the new step
            cur_.step = static_cast<uint32_t>(cur_.acc >> 60);
            cur_.mask = cur_.pending;
        }
    }

    // The LFO's shape: the square's k from the period (its edges take kSquareEdgeMs whatever the
    // rate) and the sine's weight w. The stereo offset moves at most 1 / (2 k edge) of a cycle a
    // sample: then it steepens the right side's edges at most twofold.
    const float edgeS = kSquareEdgeMs * 0.001f * kRate;
    const float off = stereo / 360.0f;
    if (fresh_) {
        depthGlide_[0] = depthGlide_[1] = depth;
        mixGlide_[0] = mixGlide_[1] = mix;
        shapeGlide_ = shape;
    } else {
        glide2(depthGlide_, depth, kGlideLevel, 1e-5f);
        glide2(mixGlide_, mix, kGlideLevel, 1e-5f);
        shapeGlide_ = glideTo(shapeGlide_, shape, kGlideShape, 1e-5f);
    }
    const float kMax = std::max(steps / (2.0f * edgeS), 2.0f);
    const float k = exp2Fast(std::max(0.0f, 2.0f * shapeGlide_ - 1.0f) * log2Fast(kMax));
    const float w = std::fabs(1.0f - 2.0f * shapeGlide_);
    if (fresh_) {
        offGlide_ = off;
    } else {
        const float most = static_cast<float>(kChunk) / (2.0f * k * edgeS);
        const float next = offGlide_ + clampf((off - offGlide_) * kGlideShape, -most, most);
        offGlide_ = std::fabs(off - next) < 1e-5f ? off : next;
    }

    const float target[5] = {depthGlide_[1], mixGlide_[1], k, w, offGlide_};
    Lin* const ramp[5] = {&depth_, &mix_, &k_, &w_, &off_};
    if (fresh_) {
        for (int i = 0; i < 5; ++i) *ramp[i] = Lin{target[i], 0.0f, target[i]};
        left_ = 0;
    } else {
        // A new voice takes its square as it is (it starts unheard).
        if (handOver) k_ = Lin{k, 0.0f, k};
        bool changed = false;
        for (int i = 0; i < 5; ++i) changed = changed || target[i] != ramp[i]->target;
        // Unchanged targets leave a running ramp alone (with blocks shorter than a chunk, set()
        // comes more often than a ramp's length).
        if (changed) {
            for (int i = 0; i < 5; ++i) {
                ramp[i]->target = target[i];
                ramp[i]->step = (target[i] - ramp[i]->cur) * (1.0f / kChunk);
            }
            left_ = kChunk;
        }
    }
    fresh_ = false;
}

// The voice fading out runs on at its own rate and square.
void Pulse::gains(Voice& v, bool fading, float* gl, float* gr, int n) {
    const uint64_t inc = fading ? v.inc : inc_;
    if (v.mode == kGate) gateGains(v, inc, gl, gr, n);
    else if (fading) lfoGains(v, inc, v.k, 0.0f, gl, gr, n);
    else lfoGains(v, inc, k_.cur, k_.step, gl, gr, n);
}

// Tremolo and Auto-Pan, four samples at a time: the LFO's phase per sample from the 64-bit phase
// (exact, so blocks of any size see the same phase), then the shape and the gains. The arrays run
// to whole vectors; the lanes past n are never used.
void Pulse::lfoGains(Voice& v, uint64_t inc, float kc, float ks, float* gl, float* gr, int n) {
    alignas(16) uint32_t ph[kChunk];
    const int m = (n + 3) & ~3;
    uint64_t a = v.acc;
    for (int i = 0; i < m; ++i) {
        ph[i] = static_cast<uint32_t>(a >> 28) + 0x40000000u;   // the LFO's turn is the phase's lower 60 bits
        a += inc;
    }
    v.acc += inc * static_cast<uint64_t>(n);

    const f4 lim = splat(static_cast<float>(left_)), one = splat(1.0f);
    const f4 d0 = splat(depth_.cur), dS = splat(depth_.step);
    const f4 k0 = splat(kc), kS = splat(ks), w0 = splat(w_.cur), wS = splat(w_.step);
    if (v.mode == kAutoPan) {
        const f4 q = splat(kQuarterPi), s2 = splat(kSqrt2);
        for (int i = 0; i < m; i += 4) {
            const f4 r = progress4(i, lim);
            const f4 pan = (d0 + r * dS) * lfo4(loadTurns4(ph + i), k0 + r * kS, w0 + r * wS);
            store4(gl + i, s2 * sinQuarter4(q - q * pan));
            store4(gr + i, s2 * sinQuarter4(q + q * pan));
        }
        return;
    }
    const f4 half = splat(0.5f);
    if (off_.cur == 0.0f && off_.target == 0.0f) {   // both sides alike
        for (int i = 0; i < m; i += 4) {
            const f4 r = progress4(i, lim);
            const f4 g = one - half * (d0 + r * dS) * (one - lfo4(loadTurns4(ph + i), k0 + r * kS, w0 + r * wS));
            store4(gl + i, g);
            store4(gr + i, g);
        }
        return;
    }
    // The right side's offset in fixed point: its phase wraps by itself.
    const f4 o0 = splat(off_.cur * 4294967296.0f), oS = splat(off_.step * 4294967296.0f);
    for (int i = 0; i < m; i += 4) {
        const f4 r = progress4(i, lim);
        const f4 d = half * (d0 + r * dS), k = k0 + r * kS, w = w0 + r * wS;
        const Turns4 phase = loadTurns4(ph + i);
        store4(gl + i, one - d * (one - lfo4(phase, k, w)));
        store4(gr + i, one - d * (one - lfo4(phase + __builtin_convertvector(o0 + r * oS, Turns4), k, w)));
    }
}

// The gate, a sample at a time: whether the step is open (with the mask that was pending when the
// step began) and not yet closing, and the state slewing toward it at the edge's slope. Then the
// S-curve and the depth.
void Pulse::gateGains(Voice& v, uint64_t inc, float* gl, float* gr, int n) {
    uint64_t a = v.acc;
    float e = v.env;
    uint32_t step = v.step, mask = v.mask;
    const uint32_t pending = v.pending, off = gateOff_, legato = legato_ ? 1u : 0u;
    const float slope[2] = {-slew_, slew_};   // closing, opening
    alignas(16) float env[kChunk];
#if EF_NEON
    // The state in a NEON lane: VFP has no min / max, its compares would stall on the flags.
    float32x2_t e2 = vdup_n_f32(e);
    const float32x2_t zero = vdup_n_f32(0.0f), one = vdup_n_f32(1.0f);
#endif
    for (int i = 0; i < n; ++i) {
        const uint32_t hi = static_cast<uint32_t>(a >> 32), s = hi >> 28;
        if (s != step) {
            step = s;
            mask = pending;
        }
        const uint32_t next = pending >> ((s + 1) & 15u);
        const uint32_t on = (mask >> s) & ((hi & kStepFrac) < off ? 1u : (legato & next)) & 1u;
#if EF_NEON
        e2 = vmin_f32(vmax_f32(vadd_f32(e2, vld1_dup_f32(slope + on)), zero), one);
        vst1_lane_f32(env + i, e2, 0);
#else
        e = std::min(std::max(e + slope[on], 0.0f), 1.0f);
        env[i] = e;
#endif
        a += inc;
    }
#if EF_NEON
    e = vget_lane_f32(e2, 0);
#endif
    v.acc = a;
    v.env = e;
    v.step = step;
    v.mask = mask;
    for (int i = n; i < ((n + 3) & ~3); ++i) env[i] = 0.0f;

    // The S-curve and the depth, four at a time (the lanes past n are never used).
    const f4 lim = splat(static_cast<float>(left_)), d0 = splat(depth_.cur), dS = splat(depth_.step);
    for (int i = 0; i < n; i += 4) {
        const f4 x = load4(env + i), d = d0 + progress4(i, lim) * dS;
        const f4 g = splat(1.0f) - d * (splat(1.0f) - x * x * (splat(3.0f) - splat(2.0f) * x));
        store4(gl + i, g);
        store4(gr + i, g);
    }
}

void Pulse::process(float* L, float* R, int n) {
    n = std::min(n, kChunk);
    if (n <= 0) return;
    const int m = (n + 3) & ~3;
    gains(cur_, false, gl_, gr_, n);

    // A mode change or a jump: the old voice runs on and hands over along a smoothstep.
    if (fade_ < kFadeSamples) {
        gains(old_, true, ol_, or_, n);
        const f4 at = splat(static_cast<float>(fade_)), lim = splat(static_cast<float>(kFadeSamples));
        for (int i = 0; i < m; i += 4) {
            const f4 x = progress4(i, lim - at);
            const f4 u = (at + x) * splat(1.0f / kFadeSamples);
            const f4 c = u * u * (splat(3.0f) - splat(2.0f) * u);
            const f4 a = load4(ol_ + i), b = load4(or_ + i);
            store4(gl_ + i, a + c * (load4(gl_ + i) - a));
            store4(gr_ + i, b + c * (load4(gr_ + i) - b));
        }
        fade_ = std::min(fade_ + n, kFadeSamples);
    }

    // Dry and wet: the input times 1 + mix (gain - 1), exactly the input at mix 0. A NaN or an
    // infinity in the input comes out as silence.
    const f4 lim = splat(static_cast<float>(left_)), one = splat(1.0f);
    const f4 x0 = splat(mix_.cur), xS = splat(mix_.step);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const f4 mix = x0 + progress4(i, lim) * xS;
        store4(L + i, finite4(load4(L + i)) * (one + mix * (load4(gl_ + i) - one)));
        store4(R + i, finite4(load4(R + i)) * (one + mix * (load4(gr_ + i) - one)));
    }
    for (; i < n; ++i) {
        const float mix = mix_.cur + static_cast<float>(std::min(i + 1, left_)) * mix_.step;
        L[i] = sanitize(L[i]) * (1.0f + mix * (gl_[i] - 1.0f));
        R[i] = sanitize(R[i]) * (1.0f + mix * (gr_[i] - 1.0f));
    }

    Lin* const ramp[5] = {&depth_, &mix_, &k_, &w_, &off_};
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

} // namespace ef
