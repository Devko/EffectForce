#include "reverb.h"

#include <algorithm>
#include <cmath>

namespace ef {

namespace {

// --- the modes ---------------------------------------------------------------------------------

struct ModeDef {
    // The lines at scale 1, samples: primes spread 1:3 geometrically, in rank order 0 1 2 3 5 4 7 6,
    // so the lines L feeds (even) and those R feeds (odd) are about as long in total.
    int line[Reverb::kLines];
    int ap[Reverb::kLines];     // in-loop allpasses
    int diff[Reverb::kLines];   // input diffusers: L's four, then R's (all different: decorrelation)
    float apGain, diffGain1, diffGain2;   // allpass coefficients: in the loop; diffusers 1-2, 3-4
    float modHz, modDepth;                // the lines' modulation: base rate, samples at mod 1
};

// An allpass in the loop adds echoes every pass, but the loop's delay (so its decay time) swings
// with the allpass's group delay, (1 - g) / (1 + g) .. (1 + g) / (1 - g) times its length: long or
// strong ones ring at their resonances. These keep the decay time within the spread of noise
// across frequency (test/reverb_test.cpp measures it per 21 Hz bin).
constexpr ModeDef kModeDefs[Reverb::kModes] = {
    // Room: 10..30 ms, short diffusers, quick and dense.
    {{443, 521, 607, 709, 967, 827, 1321, 1129}, {157, 137, 113, 103, 79, 89, 59, 67},
     {41, 71, 109, 173, 47, 83, 127, 193}, 0.5f, 0.65f, 0.55f, 0.85f, 10.0f},
    // Hall: 30..92 ms, smooth, a little modulation.
    {{1327, 1553, 1823, 2141, 2953, 2503, 4057, 3457}, {353, 307, 269, 233, 179, 199, 131, 151},
     {97, 149, 251, 383, 107, 167, 277, 421}, 0.55f, 0.7f, 0.6f, 0.5f, 28.0f},
    // Plate: 9..28 ms and Dattorro's input coefficients: the fastest build-up.
    {{397, 467, 547, 647, 887, 761, 1237, 1051}, {101, 113, 131, 151, 199, 173, 251, 223},
     {97, 131, 251, 337, 107, 149, 227, 313}, 0.5f, 0.75f, 0.625f, 0.65f, 14.0f},
    // Space: 75..250 ms, slow and deep modulation.
    {{3307, 3929, 4663, 5531, 7817, 6581, 11027, 9283}, {1061, 919, 797, 691, 509, 601, 397, 457},
     {191, 313, 499, 743, 227, 349, 547, 821}, 0.6f, 0.7f, 0.6f, 0.22f, 110.0f},
};

// Each line's LFO rate as a multiple of the mode's: no two in step, none a simple ratio of another.
constexpr float kRateMul[Reverb::kLines] = {1.0f, 1.31f, 0.77f, 1.17f, 0.89f, 1.46f, 0.67f, 1.08f};

// --- sizes and times -----------------------------------------------------------------------------

constexpr int kTaps = 8;                        // Lagrange interpolation points
constexpr int kPhases = 128;                    // its table's rows (linear between them)
constexpr int kSegment = 32;                    // read positions: straight lines between points this far apart
constexpr uint32_t kApSize = 2048;              // in-loop allpasses (the longest is 1061)
constexpr uint32_t kDiffSize = 1024;            // input diffusers (the longest is 821)
constexpr uint32_t kGuard = kTaps;              // each line's first samples, again after its end
constexpr uint32_t kStagger = 16;               // floats between lines: their cache sets differ
constexpr float kMaxScale = 1.5f;               // size 1
constexpr int kPreMax = 11025;                  // 250 ms
constexpr uint32_t kPreSize = 16384;            // the predelay's frames (L and R interleaved)
constexpr int kPreFade = 1024;                  // a predelay change crossfades over 23 ms
constexpr float kGateStep = 1.0f / 512.0f;      // a mode change: 12 ms out, cleared, 12 ms in
constexpr float kFreezeStep = 1.0f / 2048.0f;   // into and out of freeze: 46 ms
constexpr float kSizeTau = 0.08f;               // the size glide's time constant, s
constexpr float kMaxBend = 0.1f;                // ... and the longest line moves <= 0.1 sample a sample
constexpr float kLowCutOff = 5.0f;              // the low cut "off": only sub-sonics (no DC in a freeze)
constexpr float kLowCutTau = 0.02f;             // its glide (in octaves), s: a jump would tick in the tail
constexpr float kLowCutK = 1.414213562f;        // 1 / Q: Butterworth
constexpr float kInMax = 1e4f;                  // the wet input's clip (+80 dB): nothing inside overflows
constexpr float kLoopMax = 1e7f;                // a backstop the loop never reaches from that input
constexpr float kInGain = 0.8f;                 // into each line, for a network as long as Hall's
constexpr float kRefLoop = 2705.0f;             // Hall's mean loop delay at size 0.5, samples
constexpr float kTailDb = 80.0f;                // tailSamples(): down this far
constexpr double kHadamard = 0.35355339059327373;   // 1 / sqrt(8): the 8x8 Hadamard orthonormal
constexpr float kShimRatio[Reverb::kIntervals] = {2.0f, 1.49830708f, 3.17480210f, 0.5f};   // +12, +7, +19, -12
constexpr int kShimGrain = 3528;                // the shifter's grains: 80 ms
constexpr float kShimLowPass = 9000.0f;         // the shimmer path's low-pass at most (Hz) ...
constexpr float kShimHighPass = 80.0f;          // ... and its high-pass
constexpr float kShimStep = 1.0f / 882.0f;      // an interval change: 20 ms out, 20 ms in
constexpr float kShimTurn = 1.5707963f / 882.0f;   // the shimmer's angle moves at most 90 degrees in 20 ms
constexpr double kShimCorr = 1.0 / 2205.0;      // the pitched path's correlation with what stays: over 50 ms

// --- the interpolator -------------------------------------------------------------------------

// 8-point Lagrange coefficients for a read t = 0..1 samples past an integer delay D, the window
// oldest first (delays D + 4 .. D - 3, t between the middle two: its best range). Odd-order
// Lagrange there never boosts (|H| <= 1 for every t: lossless loops stay lossless), is maximally
// flat at DC and loses under 0.02 dB at 8 kHz, 0.2 dB at 11 kHz at the worst fraction. Rows every
// 1 / 128 sample, linear between them: a blend of two passive rows is passive too.
struct alignas(16) LagrangeTable {
    float c[kPhases + 1][kTaps];
    float d[kPhases][kTaps];   // the step to the next row
};

constexpr LagrangeTable makeLagrange() {
    LagrangeTable t{};
    double c[kPhases + 1][kTaps] = {};
    for (int k = 0; k <= kPhases; ++k) {
        const double x = static_cast<double>(k) / kPhases;
        for (int j = 0; j < kTaps; ++j) {
            double v = 1.0;   // point j sits at 4 - j samples past D
            for (int m = 0; m < kTaps; ++m)
                if (m != j) v *= (x - (4 - m)) / static_cast<double>(m - j);
            c[k][j] = v;
            t.c[k][j] = static_cast<float>(v);
        }
    }
    for (int k = 0; k < kPhases; ++k)
        for (int j = 0; j < kTaps; ++j) t.d[k][j] = static_cast<float>(c[k + 1][j] - c[k][j]);
    return t;
}

constexpr LagrangeTable kLagrange = makeLagrange();

// --- vector helpers simd.h doesn't have -------------------------------------------------------

// (v0 + v2, v1 + v3, v0 - v2, v1 - v3): the Hadamard's middle butterflies, no constants needed.
EF_INLINE f4 butterflyHalves(f4 v) {
#if EF_NEON
    const float32x2_t lo = vget_low_f32(v), hi = vget_high_f32(v);
    return vcombine_f32(vadd_f32(lo, hi), vsub_f32(lo, hi));
#else
    return f4{v[0] + v[2], v[1] + v[3], v[0] - v[2], v[1] - v[3]};
#endif
}

// (v0 + v1, v0 - v1, v2 + v3, v2 - v3): its last ones.
EF_INLINE f4 butterflyPairs(f4 v) {
#if EF_NEON
    const float32x2x2_t u = vuzp_f32(vget_low_f32(v), vget_high_f32(v));   // (v0, v2), (v1, v3)
    const float32x2x2_t z = vzip_f32(vadd_f32(u.val[0], u.val[1]), vsub_f32(u.val[0], u.val[1]));
    return vcombine_f32(z.val[0], z.val[1]);
#else
    return f4{v[0] + v[1], v[0] - v[1], v[2] + v[3], v[2] - v[3]};
#endif
}

// (sum of a's lanes, of b's, of c's, of d's)
EF_INLINE f4 sums4(f4 a, f4 b, f4 c, f4 d) {
#if EF_NEON
    const float32x2_t ab = vpadd_f32(vadd_f32(vget_low_f32(a), vget_high_f32(a)), vadd_f32(vget_low_f32(b), vget_high_f32(b)));
    const float32x2_t cd = vpadd_f32(vadd_f32(vget_low_f32(c), vget_high_f32(c)), vadd_f32(vget_low_f32(d), vget_high_f32(d)));
    return vcombine_f32(ab, cd);
#else
    return f4{a[0] + a[1] + a[2] + a[3], b[0] + b[1] + b[2] + b[3], c[0] + c[1] + c[2] + c[3], d[0] + d[1] + d[2] + d[3]};
#endif
}

template <int N>
EF_INLINE float lane(f4 v) {
#if EF_NEON
    return vgetq_lane_f32(v, N);
#else
    return v[N];
#endif
}

// One line's 8-point read di + (ph + fr) / kPhases samples back from `w` (the index being written
// now): the window, its coefficients, and their products, still to be summed (sums4()).
EF_INLINE f4 lagrangeRead(const float* line, uint32_t mask, uint32_t w, int32_t di, int32_t ph, f4 fr) {
    const float* x = line + ((w - static_cast<uint32_t>(di) - 4u) & mask);
    const f4 ca = load4(kLagrange.c[ph]) + fr * load4(kLagrange.d[ph]);
    const f4 cb = load4(kLagrange.c[ph] + 4) + fr * load4(kLagrange.d[ph] + 4);
    return load4(x) * ca + load4(x + 4) * cb;
}

// A parameter clamped to its range; NaN (which no comparison catches) becomes `nan`.
float clampParam(float x, float lo, float hi, float nan) {
    return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan);
}

int predelaySamples(float ms) { return static_cast<int>(ms * (kRate / 1000.0f) + 0.5f); }

} // namespace

// --- setup -------------------------------------------------------------------------------------

Reverb::Reverb() : preBuf_(2 * kPreSize, 0.0f), shimBuf_(PitchShift::bufferSize(kShimRatio[UP_TWELFTH], kShimRatio[DOWN_OCTAVE], kShimGrain), 0.0f) {
    // Each line holds its longest reach in any mode: size 1, the deepest modulation, the read's window.
    uint32_t sizes[kLines];
    size_t total = 0;
    for (int k = 0; k < kLines; ++k) {
        float need = 0.0f;
        for (const ModeDef& m : kModeDefs)
            need = std::max(need, static_cast<float>(m.line[k]) * kMaxScale + m.modDepth + kTaps + 2.0f);
        uint32_t s = 1;
        while (static_cast<float>(s) < need) s <<= 1;
        sizes[k] = s;
        total += s + kGuard + kStagger * static_cast<uint32_t>(k + 1);
    }
    total += static_cast<size_t>(kApSize + kDiffSize) * kLines;
    pool_.assign(total, 0.0f);
    float* at = pool_.data();
    for (int k = 0; k < kLines; ++k) {
        line_[k] = at;
        mask_[k] = sizes[k] - 1;
        at += sizes[k] + kGuard + kStagger * static_cast<uint32_t>(k + 1);
    }
    ap_ = at;
    diff_ = at + static_cast<size_t>(kApSize) * kLines;
    // After this many samples every buffer has been written all over since a reset.
    youngEnd_ = std::max(*std::max_element(sizes, sizes + kLines) + kGuard, std::max(kApSize, kDiffSize));
    // The LFOs' start (golden-ratio spaced phases): a reset reverb plays as a new one.
    float sn[kLines], cs[kLines];
    for (int k = 0; k < kLines; ++k) {
        const double ph = 2.0 * 3.14159265358979 * std::fmod(0.618034 * k, 1.0);
        sn[k] = static_cast<float>(std::sin(ph));
        cs[k] = static_cast<float>(std::cos(ph));
    }
    lfoStartS_[0] = load4(sn);
    lfoStartS_[1] = load4(sn + 4);
    lfoStartC_[0] = load4(cs);
    lfoStartC_[1] = load4(cs + 4);
    shift_.attach(shimBuf_.data(), static_cast<uint32_t>(shimBuf_.size()));
    setInterval(UP_OCTAVE);
    reset();
}

// Nothing big is cleared: the rack calls this on the audio thread, and the buffers are 720 KB.
// What they hold from before is never read (forget(); the predelay counts its own age), so the
// reverb plays exactly as a new one, which reads zeros there.
void Reverb::reset() {
    forget();
    input_ = InputPath{};
    input_.buf = preBuf_.data();
    phase_ = RUN;
    gate_ = 1.0f;
    segLeft_ = 0;
    lfoS_[0] = lfoStartS_[0];
    lfoS_[1] = lfoStartS_[1];
    lfoC_[0] = lfoStartC_[0];
    lfoC_[1] = lfoStartC_[1];
    shimOn_ = false;
    shimGate_ = 1.0f;
    fresh_ = true;
}

// The network starts afresh without clearing its buffers: until every position has been written
// again (youngEnd_ samples), a read that reaches back before now comes out as zero (runNetwork's
// Young), and the 8 samples behind each line's write position, which a read window can straddle,
// are zeroed here.
void Reverb::forget() {
    age_ = 0;
    young_ = true;
    for (int k = 0; k < kLines; ++k) {
        for (uint32_t i = 1; i <= kGuard; ++i) {
            const uint32_t j = (w_ - i) & mask_[k];
            line_[k][j] = 0.0f;
            if (j < kGuard) line_[k][j + mask_[k] + 1] = 0.0f;
        }
    }
    lp_[0] = lp_[1] = splat(0.0f);
    shimmerAfresh();
}

// The shimmer's interval: the shifter's ratio and the path's filters (in silence: the caller fades).
void Reverb::setInterval(int interval) {
    shimInterval_ = interval;
    const float ratio = kShimRatio[interval];
    shift_.setRatio(ratio, kShimGrain);
    const float g = std::tan(kPi * std::min(kShimLowPass, kRate / 3.0f / ratio) / kRate);
    shimFilter_.a1 = 1.0f / (1.0f + g * (g + kLowCutK));
    shimFilter_.a2 = g * shimFilter_.a1;
    shimFilter_.a3 = g * shimFilter_.a2;
    const float gh = std::tan(kPi * kShimHighPass / kRate);
    shimFilter_.hpA = gh / (1.0f + gh);
    shift_.restart();
}

// The shimmer path starts over: the shifter forgets, the filters and the correlation are cleared.
void Reverb::shimmerAfresh() {
    shift_.restart();
    shimFilter_.clear();
    shimPP_ = shimCC_ = 0.0;
    shimBeta_ = 0.0f;
}

void Reverb::loadMode(int m) {
    const ModeDef& d = kModeDefs[m];
    mode_ = m;
    float b[kLines], e[kLines];
    maxBase_ = meanBase_ = meanAp_ = 0.0f;
    uint32_t apMax = 0, diffL = 0, diffR = 0;
    for (int k = 0; k < kLines; ++k) {
        b[k] = static_cast<float>(d.line[k]);
        maxBase_ = std::max(maxBase_, b[k]);
        meanBase_ += b[k] / kLines;
        apLen_[k] = static_cast<uint32_t>(d.ap[k]);
        diffLen_[k] = static_cast<uint32_t>(d.diff[k]);
        meanAp_ += static_cast<float>(apLen_[k]) / kLines;
        apMax = std::max(apMax, apLen_[k]);
        (k < 4 ? diffL : diffR) += diffLen_[k];
        // The magic circle's step for the line's rate, a segment at a time: 2 sin(pi f segment / rate).
        e[k] = 2.0f * std::sin(kPi * d.modHz * kRateMul[k] * kSegment / kRate);
    }
    base_[0] = load4(b);
    base_[1] = load4(b + 4);
    lfoE_[0] = load4(e);
    lfoE_[1] = load4(e + 4);
    apG_ = d.apGain;
    diffG_[0] = d.diffGain1;
    diffG_[1] = d.diffGain2;
    build_ = static_cast<float>(std::max(diffL, diffR) + apMax);
    lp_[0] = lp_[1] = splat(0.0f);
    jumpCoefs_ = true;
}

void Reverb::set(const Params& p, const Transport&) {
    p_.mode = p.mode < 0 ? 0 : (p.mode >= kModes ? kModes - 1 : p.mode);
    p_.size = clampParam(p.size, 0.0f, 1.0f, 0.5f);
    p_.decayS = clampParam(p.decayS, 0.1f, 30.0f, 2.5f);
    p_.predelayMs = clampParam(p.predelayMs, 0.0f, 250.0f, 20.0f);
    p_.dampHz = clampParam(p.dampHz, 1000.0f, 20000.0f, 6000.0f);
    p_.lowCutHz = clampParam(p.lowCutHz, 20.0f, 1000.0f, 150.0f);
    p_.mod = clampParam(p.mod, 0.0f, 1.0f, 0.3f);
    p_.width = clampParam(p.width, 0.0f, 1.0f, 1.0f);
    p_.freeze = p.freeze;
    p_.mix = clampParam(p.mix, 0.0f, 1.0f, 0.3f);
    p_.shimmer = clampParam(p.shimmer, 0.0f, 1.0f, 0.0f);
    p_.shimmerInterval = p.shimmerInterval < 0 ? 0 : (p.shimmerInterval >= kIntervals ? kIntervals - 1 : p.shimmerInterval);
}

int Reverb::tailSamples() const {
    if (p_.freeze) return 1 << 30;   // 6.8 hours: for good, as far as a host can tell
    const double pre = predelaySamples(p_.predelayMs);
    const double build = maxBase_ * (0.5 + p_.size) + build_;   // until every line has had its echo
    double ring = p_.decayS * kRate * (kTailDb / 60.0f);
    // Shimmer: of the eighth of the network's energy row 3 holds, the pitched share s (sin^2 of
    // its angle) waits in the shifter, D samples on average (its head's delay), before it comes
    // back. Per mean loop T the network keeps g of its energy, so the slowest decay u a pass
    // solves u = g (1 - s / 8 + s / 8 u^(-D / T)): longer than Decay, at short ones. (Cached: a
    // bisection, when the settings change.)
    if (p_.shimmer > 0.0f) {
        const float key[5] = {p_.decayS, p_.size, p_.shimmer, static_cast<float>(p_.shimmerInterval), static_cast<float>(p_.mode)};
        if (!std::equal(key, key + 5, tailKey_)) {
            std::copy(key, key + 5, tailKey_);
            const double ratio = kShimRatio[p_.shimmerInterval], speed = std::fabs(ratio - 1.0), jump = speed * kShimGrain;
            const double delay = (ratio > 1.0 ? PitchShift::kMinDelay + speed * PitchShift::kFade : PitchShift::kMinDelay + PitchShift::kSearch) + 0.5 * jump;
            const ModeDef& d = kModeDefs[p_.mode];
            double loop = 0.0;
            for (int k = 0; k < kLines; ++k) loop += (d.line[k] * (0.5 + p_.size) + d.ap[k]) / kLines;
            const double g = std::pow(10.0, -6.0 * loop / (kRate * static_cast<double>(p_.decayS))), sn = std::sin(p_.shimmer * 1.5707963);
            const double share = sn * sn / 8.0, r = delay / loop;
            double lo = 0.0, hi = 1.0;
            for (int i = 0; i < 40; ++i) {
                const double u = 0.5 * (lo + hi);
                (g * (1.0 - share + share * std::pow(u, -r)) > u ? lo : hi) = u;
            }
            tailShim_ = kTailDb / (-10.0 * std::log10(hi)) * loop;
        }
        ring = std::max(ring, tailShim_);
    }
    return static_cast<int>(pre + build + ring) + 1;
}

// --- per chunk ---------------------------------------------------------------------------------

// Everything that moves gets its target at the chunk's end, and a straight line there.
void Reverb::chunkSetup(int n) {
    const float fn = static_cast<float>(n);
    const bool jump = fresh_;
    if (fresh_) {
        fresh_ = false;
        loadMode(p_.mode);
        phase_ = RUN;
        gate_ = 1.0f;
        fz_ = p_.freeze ? 1.0f : 0.0f;
        scale_ = 0.5f + p_.size;
        input_.tapA = predelaySamples(p_.predelayMs);
        input_.fade = 0;
    }

    // A mode change fades out, starts the network afresh in the new mode (process()), fades in;
    // back to the old mode while fading out, it just fades back in.
    if ((phase_ == RUN || phase_ == FADE_IN) && p_.mode != mode_) phase_ = FADE_OUT;
    else if (phase_ == FADE_OUT && p_.mode == mode_) phase_ = FADE_IN;
    if (phase_ == FADE_OUT) gate_ = std::max(0.0f, gate_ - fn * kGateStep);
    else if (phase_ == FADE_IN) gate_ = std::min(1.0f, gate_ + fn * kGateStep);

    fz_ = p_.freeze ? std::min(1.0f, fz_ + fn * kFreezeStep) : std::max(0.0f, fz_ - fn * kFreezeStep);

    // Size: a one-pole glide whose speed is capped, so the tail bends in pitch, gently. The read
    // positions follow it segment by segment (nextSegment()).
    const float sTgt = 0.5f + p_.size;
    if (!jump && scale_ != sTgt) {
        const float lim = fn * kMaxBend / maxBase_;
        const float ds = clampf((sTgt - scale_) * (1.0f - std::exp(-fn / (kSizeTau * kRate))), -lim, lim);
        scale_ = std::fabs(sTgt - (scale_ + ds)) < 1e-6f ? sTgt : scale_ + ds;
    }
    depth_ = p_.mod * kModeDefs[mode_].modDepth;
    const bool snap = jump || jumpCoefs_;
    if (snap) {   // a new network: the reads start where they belong
        pos_[0] = base_[0] * splat(scale_) + splat(depth_) * lfoS_[0];
        pos_[1] = base_[1] * splat(scale_) + splat(depth_) * lfoS_[1];
        segLeft_ = 0;
    }

    // Loop gains and damping (Jot). Line k's loop delay Lk (line + allpass) loses
    // a = 60 Lk / (rate decay) dB a pass, so every path through the network falls 60 dB in decayS.
    // Its one-pole (1 - p) / (1 - p z^-1) loses 10 log10(1 + 2 K (1 - cos w)) dB, K = p / (1 - p)^2,
    // about 4.34 K w^2 at low frequencies: K = a / (4.34 wd^2) makes that a (f / fd)^2 dB a pass,
    // in proportion to the line's delay like its gain, so every line damps the same dB per second
    // (the decay rate 1 + (f / fd)^2 times the low one). Toward Nyquist a one-pole flattens: where a
    // long line at a short decay would need tens of dB a pass, it stops short. (Matching the loss
    // at fd instead overdamps long lines below it: Space came out 3 dB darker than Room.) Frozen:
    // gain 1, no damping. The gains carry the Hadamard's 1 / sqrt(8).
    // The gains don't depend on Damp: damping alone moving (modulated, say) leaves them be.
    const bool gains = snap || scale_ != coefS_ || p_.decayS != coefDecay_ || fz_ != coefFz_;
    if (gains || p_.dampHz != coefDamp_) {
        coefS_ = scale_;
        coefDecay_ = p_.decayS;
        coefDamp_ = p_.dampHz;
        coefFz_ = fz_;
        const ModeDef& d = kModeDefs[mode_];
        const double wd = 2.0 * 3.14159265358979 * p_.dampHz / kRate, fz = fz_;
        float g[kLines], pole[kLines];
        for (int k = 0; k < kLines; ++k) {
            const double len = d.line[k] * static_cast<double>(scale_) + d.ap[k];
            const double a = std::min(60.0 * len / (kRate * static_cast<double>(p_.decayS)), 600.0);
            const double kk = a / (4.342944819 * wd * wd);
            const double pk = std::min(2.0 * kk / (2.0 * kk + 1.0 + std::sqrt(4.0 * kk + 1.0)), 0.999);
            if (gains) g[k] = static_cast<float>(kHadamard * (fz + (1.0 - fz) * std::pow(10.0, -a / 20.0)));
            pole[k] = static_cast<float>((1.0 - fz) * pk);
        }
        if (gains) {
            gTgt_[0] = load4(g);
            gTgt_[1] = load4(g + 4);
        }
        poleTgt_[0] = load4(pole);
        poleTgt_[1] = load4(pole + 4);
    }
    if (snap) {
        g_[0] = gTgt_[0];
        g_[1] = gTgt_[1];
        pole_[0] = poleTgt_[0];
        pole_[1] = poleTgt_[1];
    }
    const f4 inv = splat(1.0f / fn);
    float steps[2 * kLines];   // the gains' and the poles' per-sample steps
    for (int h = 0; h < 2; ++h) {
        gStep_[h] = (gTgt_[h] - g_[h]) * inv;
        poleStep_[h] = (poleTgt_[h] - pole_[h]) * inv;
        store4(steps + 8 * h, gStep_[h]);
        store4(steps + 8 * h + 4, poleStep_[h]);
    }
    glide_ = false;
    for (float m : steps) glide_ = glide_ || m != 0.0f;

    // The input's level: a tail of the same decay time holds the same energy in any mode or size
    // (an impulse response's energy grows as decay / loop delay: fewer, longer loops ring less).
    const float norm = std::sqrt((meanBase_ * scale_ + meanAp_) / kRefLoop);
    const float in = (1.0f - fz_) * gate_ * kInGain * norm, wet = p_.mix * gate_;

    // The low cut glides in octaves: an SVF's state can't follow a jump of octaves in one chunk.
    const float lcHz = p_.lowCutHz <= 20.0f ? kLowCutOff : p_.lowCutHz;
    if (jump || lcHz != lcHz_) {
        lcHz_ = lcHz;
        const float lcTgt = std::log2(lcHz);
        lcOct_ = jump ? lcTgt : lcOct_ + (lcTgt - lcOct_) * (1.0f - std::exp(-fn / (kLowCutTau * kRate)));
        if (std::fabs(lcTgt - lcOct_) < 1e-4f) lcOct_ = lcTgt;
        else lcHz_ = 0.0f;   // still on its way: again next chunk
        lcG_ = std::tan(kPi * std::exp2(lcOct_) / kRate);
    }
    const float lcG = lcG_;

    if (jump) {
        in_.jump(in);
        dry_.jump(1.0f - p_.mix);
        wet_.jump(wet);
        width_.jump(p_.width);
        input_.g.jump(lcG);
    } else {
        in_.to(in, n);
        dry_.to(1.0f - p_.mix, n);
        wet_.to(wet, n);
        width_.to(p_.width, n);
        input_.g.to(lcG, n);
    }
    jumpCoefs_ = false;
    input_.moving = input_.g.value() != lcG;
    if (!input_.moving) {
        input_.a1 = 1.0f / (1.0f + lcG * (lcG + kLowCutK));
        input_.a2 = lcG * input_.a1;
        input_.a3 = lcG * input_.a2;
    }

    // Predelay: a crossfade to the new tap, one at a time (no pitch sweep through the buffer).
    const int pre = predelaySamples(p_.predelayMs);
    if (input_.fade == 0 && pre != input_.tapA) {
        input_.tapB = pre;
        input_.fade = 1;
    }

    // Shimmer: an interval change fades the pitched path out, switches in silence and fades it
    // back in. What stays and what is pitched share the power: cos and sin of shimmer x 90 degrees.
    // Off and settled, the shifter doesn't run. Asked for again, it starts afresh; as it starts
    // silent (its head reaches back before anything it has heard), the pitched path fades in once
    // it sounds, as after an interval change: taking the tail's share away earlier would dip it.
    const bool wake = !shimOn_ && p_.shimmer > 0.0f;
    if (wake) {
        shimmerAfresh();
        if (!jump) shimGate_ = 0.0f;
    }
    if (jump) {
        if (p_.shimmerInterval != shimInterval_) setInterval(p_.shimmerInterval);
    } else if (p_.shimmerInterval != shimInterval_) {
        if (!shimOn_ || shimGate_ == 0.0f) setInterval(p_.shimmerInterval);
        else shimGate_ = std::max(0.0f, shimGate_ - fn * kShimStep);
    }
    if (p_.shimmerInterval == shimInterval_ && (!(shimOn_ || wake) || shift_.sounding()))
        shimGate_ = std::min(1.0f, shimGate_ + fn * kShimStep);
    // The angle glides (90 degrees in 20 ms at most): switching what circulates in one chunk would
    // click in the tail.
    const float want = p_.shimmer * shimGate_ * (kPi / 2.0f);
    const float angle = jump ? want : shimAngle_ + clampf(want - shimAngle_, -fn * kShimTurn, fn * kShimTurn);
    float sn = shimSin_.target(), cs = shimCos_.target();
    if (jump || angle != shimAngle_) {
        shimAngle_ = angle;
        sn = angle > 0.0f ? std::sin(angle) : 0.0f;
        cs = angle > 0.0f ? std::cos(angle) : 1.0f;
    }
    if (jump) {
        shimSin_.jump(sn);
        shimCos_.jump(cs);
    } else {
        shimSin_.to(sn, n);
        shimCos_.to(cs, n);
    }
    shimOn_ = p_.shimmer > 0.0f || sn != 0.0f || shimSin_.value() != 0.0f;
}

// The read positions' next straight line: the sines a segment on, the size where it is going. The
// segments count from reset, not from the chunks, so the block size never changes the sound.
void Reverb::nextSegment() {
    for (int h = 0; h < 2; ++h) {
        lfoS_[h] = lfoS_[h] + lfoE_[h] * lfoC_[h];
        lfoC_[h] = lfoC_[h] - lfoE_[h] * lfoS_[h];
        posEnd_[h] = base_[h] * splat(scale_) + splat(depth_) * lfoS_[h];
        posStep_[h] = (posEnd_[h] - pos_[h]) * splat(1.0f / kSegment);
    }
    segLeft_ = kSegment;
    // Young or not changes at a segment's start: the two loops may round differently (NEON's fused
    // multiply-adds), and the segments, unlike the chunks, are the same whatever the block size.
    young_ = age_ < youngEnd_;
}

void Reverb::process(float* L, float* R, int n) {
    if (n <= 0) return;
    chunkSetup(n);
    for (int i = 0; i < n;) {
        if (segLeft_ == 0) nextSegment();
        const int m = std::min(n - i, segLeft_);
        if (young_) {
            if (shimOn_) {
                if (glide_) runNetwork<true, true, true>(L + i, R + i, m);
                else runNetwork<false, true, true>(L + i, R + i, m);
            } else {
                if (glide_) runNetwork<true, true, false>(L + i, R + i, m);
                else runNetwork<false, true, false>(L + i, R + i, m);
            }
            age_ = std::min(age_ + static_cast<uint32_t>(m), youngEnd_);
        } else {
            if (shimOn_) {
                if (glide_) runNetwork<true, false, true>(L + i, R + i, m);
                else runNetwork<false, false, true>(L + i, R + i, m);
            } else {
                if (glide_) runNetwork<true, false, false>(L + i, R + i, m);
                else runNetwork<false, false, false>(L + i, R + i, m);
            }
        }
        i += m;
        segLeft_ -= m;
        if (segLeft_ == 0) {   // lands exactly
            pos_[0] = posEnd_[0];
            pos_[1] = posEnd_[1];
            if (shimOn_) shimBeta_ = static_cast<float>(clampf(static_cast<float>(shimPP_ / (shimCC_ + 1e-24)), -1.0f, 1.0f));
        }
    }
    g_[0] = gTgt_[0];
    g_[1] = gTgt_[1];
    pole_[0] = poleTgt_[0];
    pole_[1] = poleTgt_[1];

    // A backstop the loop never reaches (it is passive and its input is clipped): if it ever did,
    // the network starts over rather than ringing on at a level nothing could have put there.
    float lp[kLines];
    store4(lp, lp_[0]);
    store4(lp + 4, lp_[1]);
    bool sane = true;
    for (float v : lp) sane = sane && std::fabs(v) < kLoopMax;
    if (!sane) forget();

    if (phase_ == FADE_OUT && gate_ == 0.0f) {   // silent: the new mode, in an empty network
        loadMode(p_.mode);
        forget();
        phase_ = FADE_IN;
    } else if (phase_ == FADE_IN && gate_ == 1.0f) {
        phase_ = RUN;
    }
}

// --- per sample --------------------------------------------------------------------------------

// Predelay and low cut, for the wet input.
EF_INLINE void Reverb::InputPath::tick(float xl, float xr, float& ol, float& or_) {
    w = (w + 1) & (kPreSize - 1);
    buf[2 * w] = clampf(xl, -kInMax, kInMax) + tiny;
    buf[2 * w + 1] = clampf(xr, -kInMax, kInMax) + tiny;
    tiny = -tiny;
    if (age < kPreSize) ++age;
    // A tap reaching back before the reset reads zeros, as a new predelay would.
    const uint32_t ta = static_cast<uint32_t>(tapA), tb = static_cast<uint32_t>(tapB);
    const float* a = buf + 2 * ((w - ta) & (kPreSize - 1));
    float pl = ta < age ? a[0] : 0.0f, pr = ta < age ? a[1] : 0.0f;
    if (fade > 0) {
        const float* b = buf + 2 * ((w - tb) & (kPreSize - 1));
        const float x = static_cast<float>(fade) * (1.0f / kPreFade);
        pl += ((tb < age ? b[0] : 0.0f) - pl) * x;
        pr += ((tb < age ? b[1] : 0.0f) - pr) * x;
        if (++fade > kPreFade) {
            tapA = tapB;
            fade = 0;
        }
    }

    // Simper's trapezoidal SVF as a high-pass; while g glides, its update follows every sample.
    float u1 = a1, u2 = a2, u3 = a3;
    if (moving) {
        const float gn = g.next();
        u1 = 1.0f / (1.0f + gn * (gn + kLowCutK));
        u2 = gn * u1;
        u3 = gn * u2;
    }
    const float v3l = pl - ic2L, v3r = pr - ic2R;
    const float v1l = u1 * ic1L + u2 * v3l, v1r = u1 * ic1R + u2 * v3r;
    const float v2l = ic2L + u2 * ic1L + u3 * v3l, v2r = ic2R + u2 * ic1R + u3 * v3r;
    ic1L = v1l + v1l - ic1L;
    ic2L = v2l + v2l - ic2L;
    ic1R = v1r + v1r - ic1R;
    ic2R = v2r + v2r - ic2R;
    ol = pl - kLowCutK * v1l - v2l;
    or_ = pr - kLowCutK * v1r - v2r;
}

// The shimmer path before the shifter: a trapezoidal one-pole high-pass, then two Simper SVF
// low-passes (Butterworth each).
EF_INLINE float Reverb::ShimmerFilter::tick(float x) {
    const float v = (x - hp) * hpA, low = v + hp;
    hp = low + v;
    const float u = x - low;
    const float v3a = u - ic2a, v1a = a1 * ic1a + a2 * v3a, v2a = ic2a + a2 * ic1a + a3 * v3a;
    ic1a = v1a + v1a - ic1a;
    ic2a = v2a + v2a - ic2a;
    const float v3b = v2a - ic2b, v1b = a1 * ic1b + a2 * v3b, v2b = ic2b + a2 * ic1b + a3 * v3b;
    ic1b = v1b + v1b - ic1b;
    ic2b = v2b + v2b - ic2b;
    return v2b;
}

// Part of a chunk (within one segment) through the whole reverb, in three passes so that each
// loop keeps few values live (NEON has 16 registers, and VFP's scalars are its lower eight): the
// input path into a local buffer, the network, then width and mix. The state sits in locals: as
// members, every store into a buffer would make the compiler reload them. Glide: the loop gains
// and damping poles move this chunk. Young: the network was started afresh (forget()) less than
// youngEnd_ samples ago, and a read reaching back before that, t samples on, is a zero. Shimmer:
// the pitched path runs.
template <bool Glide, bool Young, bool Shimmer>
void Reverb::runNetwork(float* L, float* R, int n) {
    float inL[kSegment], inR[kSegment], outL[kSegment], outR[kSegment];
    const uint32_t w0 = w_, age0 = age_;

    // The input: predelay, low cut, gate (freeze, a mode change: before the diffusers, which
    // never see a jump), four allpasses a side.
    {
        InputPath input = input_;
        Ramp gate = in_;
        float* const diff = diff_;
        const float dg1 = diffG_[0], dg2 = diffG_[1];
        uint32_t len[kLines];
        for (int k = 0; k < kLines; ++k) len[k] = diffLen_[k];
        for (int i = 0; i < n; ++i) {
            float il, ir;
            input.tick(sanitize(L[i]), sanitize(R[i]), il, ir);
            const float gi = gate.next();
            il *= gi;
            ir *= gi;
            const uint32_t w = w0 + static_cast<uint32_t>(i), dw = (w & (kDiffSize - 1)) * kLines, t = age0 + static_cast<uint32_t>(i);
            for (int k = 0; k < 4; ++k) {
                const float g = k < 2 ? dg1 : dg2;
                float zl = diff[((w - len[k]) & (kDiffSize - 1)) * kLines + static_cast<uint32_t>(k)];
                float zr = diff[((w - len[k + 4]) & (kDiffSize - 1)) * kLines + static_cast<uint32_t>(k + 4)];
                if (Young) {
                    zl = len[k] > t ? 0.0f : zl;
                    zr = len[k + 4] > t ? 0.0f : zr;
                }
                const float vl = il + g * zl, vr = ir + g * zr;
                diff[dw + static_cast<uint32_t>(k)] = vl;
                diff[dw + static_cast<uint32_t>(k + 4)] = vr;
                il = zl - g * vl;
                ir = zr - g * vr;
            }
            inL[i] = il;
            inR[i] = ir;
        }
        input_ = input;
        in_ = gate;
    }

    // The network.
    {
        const f4 apG = splat(apG_);
        float* const ap = ap_;
        float* lines[kLines];
        uint32_t masks[kLines], apLen[kLines];
        for (int k = 0; k < kLines; ++k) {
            lines[k] = line_[k];
            masks[k] = mask_[k];
            apLen[k] = apLen_[k];
        }
        f4 pos0 = pos_[0], pos1 = pos_[1];
        const f4 step0 = posStep_[0], step1 = posStep_[1];
        f4 g0 = g_[0], g1 = g_[1], p0 = pole_[0], p1 = pole_[1], lp0 = lp_[0], lp1 = lp_[1];
        const f4 gs0 = gStep_[0], gs1 = gStep_[1], ps0 = poleStep_[0], ps1 = poleStep_[1];
        float row3[kSegment];   // Shimmer: row 3's component, sample by sample (pitched after the loop)
        for (int i = 0; i < n; ++i) {
            const uint32_t w = w0 + static_cast<uint32_t>(i), t = age0 + static_cast<uint32_t>(i);

            // The lines, read with 8-point Lagrange at their (modulated, gliding) positions.
            pos0 += step0;
            pos1 += step1;
            // A loop GCC mustn't unroll: unrolled, it hoists every line's addresses and loads ahead
            // and spills them (ARM has 13 registers for them); rolled, it runs 10% fewer instructions.
            float d[kLines];
            store4(d, pos0);
            store4(d + 4, pos1);
            f4 part[kLines];
#pragma GCC unroll 1
            for (int k = 0; k < kLines; ++k) {
                const int32_t di = static_cast<int32_t>(d[k]);
                const float tp = (d[k] - static_cast<float>(di)) * kPhases;
                const int32_t ph = static_cast<int32_t>(tp);
                // Young: a window wholly before the fresh start reads zeros (one straddling it finds
                // the zeros forget() left behind the write position).
                if (Young && di >= static_cast<int32_t>(t) + 4) part[k] = splat(0.0f);
                else part[k] = lagrangeRead(lines[k], masks[k], w, di, ph, splat(tp - static_cast<float>(ph)));
            }
            const f4 r0 = sums4(part[0], part[1], part[2], part[3]), r1 = sums4(part[4], part[5], part[6], part[7]);

            // In-loop allpasses: echo density grows every pass.
            float z[kLines];
            for (int k = 0; k < kLines; ++k) {
                z[k] = ap[((w - apLen[k]) & (kApSize - 1)) * kLines + static_cast<uint32_t>(k)];
                if (Young) z[k] = apLen[k] > t ? 0.0f : z[k];
            }
            const f4 z0 = load4(z), z1 = load4(z + 4);
            const f4 v0 = r0 + apG * z0, v1 = r1 + apG * z1;
            float* aw = ap + (w & (kApSize - 1)) * kLines;
            store4(aw, v0);
            store4(aw + 4, v1);

            // Damping and loop gain.
            if (Glide) {
                g0 += gs0;
                g1 += gs1;
                p0 += ps0;
                p1 += ps1;
            }
            const f4 o0 = z0 - apG * v0, o1 = z1 - apG * v1;
            lp0 = o0 + p0 * (lp0 - o0);
            lp1 = o1 + p1 * (lp1 - o1);
            const f4 y0 = g0 * lp0, y1 = g1 * lp1;

            // Hadamard: three butterfly stages (the gains hold its 1 / sqrt(8)).
            f4 ha = butterflyPairs(butterflyHalves(y0 + y1));
            const f4 hb = butterflyPairs(butterflyHalves(y0 - y1));
            outL[i] = lane<1>(ha);   // rows + - + - ... and + + - - ...
            outR[i] = lane<2>(ha);

            if (Shimmer) row3[i] = lane<3>(ha);

            // Back into the lines, rotated by one (H alone is its own inverse), with the input:
            // L into the even lines, R into the odd.
            const f4 inj = {inL[i], inR[i], inL[i], inR[i]};
            float wv[kLines];
            store4(wv, ext<3>(hb, ha) + inj);
            store4(wv + 4, ext<3>(ha, hb) + inj);
            for (int k = 0; k < kLines; ++k) {
                const uint32_t j = w & masks[k];
                lines[k][j] = wv[k];
                if (j < kGuard) lines[k][j + masks[k] + 1] = wv[k];
            }
        }
        pos_[0] = pos0;
        pos_[1] = pos1;
        lp_[0] = lp0;
        lp_[1] = lp1;
        if (Glide) {
            g_[0] = g0;
            g_[1] = g1;
            pole_[0] = p0;
            pole_[1] = p1;
        }

        // Shimmer: row 3's component c, part of it pitched, back into line 4 (the one it feeds, with
        // L's input). After the loop rather than in it, where its state would crowd the lines' out
        // of the registers: no line is read sooner than a few hundred samples after it is written,
        // so nothing in this stretch reads what is rewritten here. What the pitched copy holds of c
        // itself (an octave landing on a frozen chord's own harmonics, in phase) comes out first:
        // with only the rest, cos^2 + sin^2 can't add power, whatever the material.
        if (Shimmer) {
            PitchShift shift = shift_;
            ShimmerFilter filter = shimFilter_;
            Ramp keep = shimCos_, pitched = shimSin_;
            double pp = shimPP_, cc = shimCC_;
            const float beta = shimBeta_;
            float* const line = lines[4];
            const uint32_t mask = masks[4];
            for (int i = 0; i < n; ++i) {
                const float c = row3[i], q = shift.tick(filter.tick(c));
                pp += kShimCorr * (static_cast<double>(q) * c - pp);   // in double: no denormals from the quiet
                cc += kShimCorr * (static_cast<double>(c) * c - cc);
                const float v = (keep.next() * c + pitched.next() * (q - beta * c)) + inL[i];
                const uint32_t j = (w0 + static_cast<uint32_t>(i)) & mask;
                line[j] = v;
                if (j < kGuard) line[j + mask + 1] = v;
            }
            shift_ = shift;
            shimFilter_ = filter;
            shimCos_ = keep;
            shimSin_ = pitched;
            shimPP_ = pp;
            shimCC_ = cc;
        }
    }
    w_ = w0 + static_cast<uint32_t>(n);

    // Width (mid / side) and mix. Mix 0 is the dry signal bit for bit (a -0 stays -0).
    Ramp dry = dry_, wet = wet_, width = width_;
    if (wet.value() == 0.0f && wet.target() == 0.0f) {
        for (int i = 0; i < n; ++i) {
            const float dg = dry.next();
            L[i] = sanitize(L[i]) * dg;
            R[i] = sanitize(R[i]) * dg;
        }
        width.jump(width.target());
    } else {
        for (int i = 0; i < n; ++i) {
            const float dg = dry.next(), wg = wet.next(), side = width.next() * 0.5f;
            const float mid = 0.5f * (outL[i] + outR[i]), sd = (outL[i] - outR[i]) * side;
            L[i] = sanitize(L[i]) * dg + (mid + sd) * wg;
            R[i] = sanitize(R[i]) * dg + (mid - sd) * wg;
        }
    }
    dry_ = dry;
    wet_ = wet;
    width_ = width;
}

} // namespace ef
