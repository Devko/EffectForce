// Drive (dsp/drive.h): the compensation table, set() and the chunk loop.
#include "drive.h"

#include <algorithm>
#include <cmath>

namespace ef {

namespace {

constexpr float kRef = 0.25f;               // -12 dBFS: the level the compensation keeps
constexpr float kBiasScale = 0.5f;          // bias 1: half the knee, or half a driven sine's swing
constexpr float kMaxDriveDb = 36.0f;
constexpr float kInMax = 64.0f;             // the wet path clamps its input to +36 dBFS: x Drive stays finite
constexpr float kDcPole = 0.998576256f;     // exp(-2 pi 10 Hz / 44.1 kHz): the DC blocker
constexpr float kSplit = 0.0666057803f;     // g / (1 + g), g = tan(pi 1 kHz / 44.1 kHz): the tilt's TPT one-pole
constexpr float kTiltDb = 6.0f;             // per side: 12 dB between the extremes' lows and highs
constexpr float kHoldOctaves = 3.46270675f;  // log2(44.1 kHz / 4 kHz): the crusher's rate at full drive

float param(float x, float lo, float hi) { return std::isnan(x) ? lo : clampf(x, lo, hi); }

float biasOffset(float bias, float drive) {
    const float swing = drive * kRef;
    return bias * kBiasScale * std::sqrt(1.0f + swing * swing);
}

// The compensation in dB per shaper, bias (tenths) and drive (dB): the gain that brings a -12 dBFS
// sine's RMS after the shaper (DC removed, as the DC blocker will) back to its RMS before.
constexpr int kShapers = 5, kBiasSteps = 11, kDriveSteps = 37;

struct CompTable {
    float db[kShapers][kBiasSteps][kDriveSteps];

    CompTable() {
        constexpr int kPoints = 256;   // per cycle: the RMS of harmonics up to the 127th
        const double sineRms = kRef / std::sqrt(2.0);
        float sine[kPoints];
        for (int k = 0; k < kPoints; ++k) sine[k] = kRef * static_cast<float>(std::sin(2.0 * 3.14159265358979 * k / kPoints));
        for (int t = 0; t < kShapers; ++t)
            for (int j = 0; j < kBiasSteps; ++j)
                for (int i = 0; i < kDriveSteps; ++i) {
                    const float g = std::pow(10.0f, static_cast<float>(i) / 20.0f);
                    const float o = biasOffset(0.1f * static_cast<float>(j), g);
                    const float rest = drv::shape(t, o);
                    double sum = 0.0, sum2 = 0.0;
                    for (int k = 0; k < kPoints; ++k) {
                        const double v = drv::shape(t, sine[k] * g + o) - rest;
                        sum += v;
                        sum2 += v * v;
                    }
                    const double mean = sum / kPoints, var = std::max(sum2 / kPoints - mean * mean, 1e-12);
                    db[t][j][i] = static_cast<float>(std::clamp(20.0 * std::log10(sineRms / std::sqrt(var)), -40.0, 24.0));
                }
    }
};

const CompTable& compTable() {
    static const CompTable table;   // built by the first Drive (UI thread), read-only after
    return table;
}

float compDb(const float* table, int type, float driveDb, float bias) {
    const float d = driveDb, b = bias * static_cast<float>(kBiasSteps - 1);
    const int i = std::min(static_cast<int>(d), kDriveSteps - 2), j = std::min(static_cast<int>(b), kBiasSteps - 2);
    const float fd = d - static_cast<float>(i), fb = b - static_cast<float>(j);
    const float* r0 = table + (type * kBiasSteps + j) * kDriveSteps + i;
    const float* r1 = r0 + kDriveSteps;
    const float v0 = r0[0] + (r0[1] - r0[0]) * fd, v1 = r1[0] + (r1[1] - r1[0]) * fd;
    return v0 + (v1 - v0) * fb;
}

// The shaper over a chunk at the high rate, in place: comp x (shaper(u) - shaper(offset)), four
// high-rate samples (two per channel) for each low-rate one. One loop per shaper, so each inlines
// and the four go through NEON together.
template <class F>
void shapeLoop(float* __restrict high, const float* __restrict comp, const float* __restrict rest, int n, F f) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < 4; ++j) high[4 * i + j] = comp[i] * (f(high[4 * i + j]) - rest[i]);
}

void shapeHigh(int type, float* high, const float* comp, const float* rest, int n) {
    switch (type) {
    case Drive::Soft: shapeLoop(high, comp, rest, n, [](float u) { return drv::soft(u); }); break;
    case Drive::Tube: shapeLoop(high, comp, rest, n, [](float u) { return drv::tube(u); }); break;
    case Drive::Hard: shapeLoop(high, comp, rest, n, [](float u) { return drv::hard(u); }); break;
    case Drive::Fold: shapeLoop(high, comp, rest, n, [](float u) { return drv::fold(u); }); break;
    default: shapeLoop(high, comp, rest, n, [](float u) { return drv::sine(u); }); break;
    }
}

// A Ramp's next n values. One at its target is a constant: filled without stepping it.
void fill(Ramp& r, float* out, int n) {
    if (r.value() == r.target()) std::fill(out, out + n, r.value());
    else
        for (int i = 0; i < n; ++i) out[i] = r.next();
}

} // namespace

Drive::Drive() : table_(&compTable().db[0][0][0]) { reset(); }

void Drive::reset() {
    resetChannels();
    fresh_ = true;
    fading_ = false;
}

void Drive::resetChannels() {
    for (Channel& c : ch_) c = Channel();
    up_.reset();
    down_.reset();
    dryUp_.reset();
    dryDown_.reset();
    dryRunning_ = false;
    for (float* h : history_) std::fill(h, h + kHistory, 0.0f);
    historyPos_ = 0;
}

void Drive::set(const Params& p, const Transport&) {
    const int type = std::clamp(p.type, 0, kNumTypes - 1);
    const float driveDb = param(p.driveDb, 0.0f, kMaxDriveDb);
    const float tone = param(p.tone, -1.0f, 1.0f);
    const float bias = param(p.bias, 0.0f, 1.0f);
    const float outDb = param(p.outDb, -24.0f, 12.0f);
    const float mix = param(p.mix, 0.0f, 1.0f);

    const bool crush = type == Crush;
    drive_ = dbToGain(driveDb);
    comp_ = crush ? 1.0f : dbToGain(compDb(table_, type, driveDb, bias));
    const float offset = biasOffset(bias, drive_);
    const float rest = crush ? 0.0f : drv::shape(type, offset);

    step_ = std::exp2(driveDb * (1.0f / 3.0f) - 15.0f);
    invStep_ = 1.0f / step_;
    crushOffset_ = 0.5f * bias * step_;
    holdRate_ = std::exp2(-driveDb * (kHoldOctaves / kMaxDriveDb));

    const float out = dbToGain(outDb), high = dbToGain(kTiltDb * tone), low = dbToGain(-kTiltDb * tone);
    const float direct = out * high, lowExtra = out * (low - high);
    const float filtered = mix > 0.0f && !crush ? 1.0f : 0.0f;   // mix 0: the raw input; Crush: no halfbands to match

    if (fresh_) {
        fresh_ = false;
        type_ = type;
        driveDb_.jump(driveDb);
        bias_.jump(bias);
        lastComp_ = comp_;
        offset_.jump(offset);
        rest_.jump(rest);
        direct_.jump(direct);
        low_.jump(lowExtra);
        mix_.jump(mix);
        filtered_.jump(filtered);
        return;
    }
    if (type != type_) {
        // The old shaper fades out over the next chunk at the compensation and rest it had; the
        // new one starts at its own. A path that sat idle starts from cleared state.
        fading_ = true;
        oldType_ = type_;
        oldComp_ = lastComp_;
        oldRest_ = rest_.value();
        rest_.jump(rest);
        if (crush) {
            for (Channel& c : ch_) {
                c.held = 0.0f;
                c.phase = 1.0f;
            }
        } else if (oldType_ == Crush) {
            up_.reset();
            down_.reset();
        }
        type_ = type;
    } else {
        rest_.to(rest, kChunk);
    }
    driveDb_.to(driveDb, kChunk);
    bias_.to(bias, kChunk);
    offset_.to(offset, kChunk);
    direct_.to(direct, kChunk);
    low_.to(lowExtra, kChunk);
    mix_.to(mix, kChunk);
    filtered_.to(filtered, kChunk);
}

void Drive::process(float* L, float* R, int n) {
    for (int pos = 0; pos < n; pos += kChunk) processChunk(L + pos, R + pos, std::min(kChunk, n - pos));
}

void Drive::processChunk(float* L, float* R, int n) {
    Controls k;
    if (driveDb_.value() != driveDb_.target() || bias_.value() != bias_.target()) {
        // The compensation follows the drive (drive.h): both exact at every 4th sample and the
        // last, straight lines between (a 36 dB jump moves 4.5 dB a stretch).
        auto compAt = [&](float d, float b) { return type_ == Crush ? 1.0f : dbToGain(compDb(table_, type_, d, b)); };
        float d[kChunk], b[kChunk];
        float g0 = dbToGain(driveDb_.value()), c0 = compAt(driveDb_.value(), bias_.value());
        for (int i = 0; i < n; ++i) {
            d[i] = driveDb_.next();
            b[i] = bias_.next();
        }
        for (int from = 0; from < n; from += 4) {
            const int to = std::min(from + 4, n);
            const float g1 = dbToGain(d[to - 1]), c1 = compAt(d[to - 1], b[to - 1]);
            const float step = 1.0f / static_cast<float>(to - from);
            for (int i = from; i < to; ++i) {
                const float t = static_cast<float>(i - from + 1) * step;
                k.drive[i] = g0 + (g1 - g0) * t;
                k.comp[i] = c0 + (c1 - c0) * t;
            }
            g0 = g1;
            c0 = c1;
        }
    } else {
        std::fill(k.drive, k.drive + n, drive_);
        std::fill(k.comp, k.comp + n, comp_);
    }
    // Whether the dry needs the halfbands this chunk: not while only the wet is heard, nor while
    // the dry is the raw input.
    const bool wetOnly = mix_.value() == 1.0f && mix_.target() == 1.0f;
    const bool rawDry = filtered_.value() == 0.0f && filtered_.target() == 0.0f;
    const bool allFiltered = filtered_.value() == 1.0f && filtered_.target() == 1.0f;
    fill(offset_, k.offset, n);
    fill(rest_, k.rest, n);
    fill(direct_, k.direct, n);
    fill(low_, k.low, n);
    fill(mix_, k.mix, n);
    fill(filtered_, k.filtered, n);
    if (fading_)
        for (int i = 0; i < n; ++i) {
            k.fade[i] = static_cast<float>(i + 1) / static_cast<float>(n);
            k.oldComp[i] = oldComp_;
            k.oldRest[i] = oldRest_;
        }
    float dry[2][kChunk], wet[2][kChunk];
    for (int i = 0; i < n; ++i) {   // a NaN never reaches a filter
        dry[0][i] = sanitize(L[i]);
        dry[1][i] = sanitize(R[i]);
    }
    if (type_ == Crush) {
        crush(dry[0], wet[0], ch_[0], n);
        crush(dry[1], wet[1], ch_[1], n);
    } else {
        oversampled(dry, wet, k, n, false);
    }
    if (fading_ && (type_ == Crush) != (oldType_ == Crush)) {   // to or from Crush: fade at the low rate
        float old[2][kChunk];
        if (oldType_ == Crush) {
            crush(dry[0], old[0], ch_[0], n);
            crush(dry[1], old[1], ch_[1], n);
        } else {
            oversampled(dry, old, k, n, true);
        }
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) wet[c][i] = old[c][i] + (wet[c][i] - old[c][i]) * k.fade[i];
    }
    float filtered[2][kChunk];
    const float (*mixDry)[kChunk] = dry;
    if (!wetOnly && !rawDry) {
        if (!dryRunning_) warmUp();
        dryRunning_ = true;
        filteredDry(dry, filtered, n);
        if (!allFiltered)
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) filtered[c][i] = dry[c][i] + k.filtered[i] * (filtered[c][i] - dry[c][i]);
        mixDry = filtered;
    } else {
        dryRunning_ = false;
    }
    remember(dry, n);
    finish(L, ch_[0], mixDry[0], wet[0], k, n);
    finish(R, ch_[1], mixDry[1], wet[1], k, n);
    lastComp_ = k.comp[n - 1];
    fading_ = false;
}

// Both channels at once, through the stereo halfbands. `outgoing`: the type being faded out (to
// Crush), at its old compensation and rest.
void Drive::oversampled(const float (*dry)[kChunk], float (*out)[kChunk], const Controls& k, int n, bool outgoing) {
    float x[2][kChunk];             // the driven input
    float high[4 * kChunk];         // per low-rate sample: left early, left late, right early, right late
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) x[c][i] = clampf(dry[c][i], -kInMax, kInMax) * k.drive[i] + k.offset[i];
    // The halfbands run on local copies: the compiler can't tell their state from the buffers
    // otherwise, and would store it back every sample.
    StereoInterpolator up = up_;
    for (int i = 0; i < n; ++i) store4(high + 4 * i, up.process(x[0][i], x[1][i]));
    up_ = up;
    if (outgoing) {
        shapeHigh(oldType_, high, k.oldComp, k.oldRest, n);
    } else if (fading_ && oldType_ != Crush) {   // from one shaper to another: fade at the high rate
        float old[4 * kChunk];
        std::copy(high, high + 4 * n, old);
        shapeHigh(oldType_, old, k.oldComp, k.oldRest, n);
        shapeHigh(type_, high, k.comp, k.rest, n);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < 4; ++j) high[4 * i + j] = old[4 * i + j] + (high[4 * i + j] - old[4 * i + j]) * k.fade[i];
    } else {
        shapeHigh(type_, high, k.comp, k.rest, n);
    }
    StereoDecimator down = down_;
    for (int i = 0; i < n; ++i) down.process(load4(high + 4 * i), out[0][i], out[1][i]);
    down_ = down;
}

// The DC blocker, the tilt with Out, and the mix with the dry signal.
void Drive::finish(float* io, Channel& c, const float* dry, const float* wet, const Controls& k, int n) {
    float dcIn = c.dcIn, dcOut = c.dcOut, split = c.split;
    for (int i = 0; i < n; ++i) {
        const float h = wet[i] - dcIn + kDcPole * dcOut;
        dcIn = wet[i];
        dcOut = h;
        const float v = (h - split) * kSplit, low = v + split;
        split = low + v;
        const float y = k.direct[i] * h + k.low[i] * low;
        io[i] = dry[i] + k.mix[i] * (y - dry[i]);
    }
    // Flushed by hand: the tests run without the device's flush-to-zero, and the DC blocker
    // takes seconds to decay through the denormals.
    c.dcIn = dcIn;
    c.dcOut = std::fabs(dcOut) < 1e-20f ? 0.0f : dcOut;
    c.split = std::fabs(split) < 1e-20f ? 0.0f : split;
}

// The dry through a halfband pair like the wet's, with nothing between: the same phase.
void Drive::filteredDry(const float (*dry)[kChunk], float (*out)[kChunk], int n) {
    float x[2][kChunk];
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) x[c][i] = clampf(dry[c][i], -kInMax, kInMax);   // as the wet's input
    StereoInterpolator up = dryUp_;
    StereoDecimator down = dryDown_;
    for (int i = 0; i < n; ++i) down.process(up.process(x[0][i], x[1][i]), out[0][i], out[1][i]);
    dryUp_ = up;
    dryDown_ = down;
}

// The dry halfbands start again from the last kHistory input samples, so they're in step.
void Drive::warmUp() {
    StereoInterpolator up;
    StereoDecimator down;
    float l, r;
    for (int j = 0; j < kHistory; ++j) {
        const int at = (historyPos_ + j) & (kHistory - 1);
        down.process(up.process(clampf(history_[0][at], -kInMax, kInMax), clampf(history_[1][at], -kInMax, kInMax)), l, r);
    }
    dryUp_ = up;
    dryDown_ = down;
}

// The input into the history ring: two straight copies around its end.
void Drive::remember(const float (*dry)[kChunk], int n) {
    const int first = std::min(n, kHistory - historyPos_);
    for (int c = 0; c < 2; ++c) {
        std::copy(dry[c], dry[c] + first, history_[c] + historyPos_);
        std::copy(dry[c] + first, dry[c] + n, history_[c]);
    }
    historyPos_ = (historyPos_ + n) & (kHistory - 1);
}

void Drive::crush(const float* dry, float* out, Channel& c, int n) const {
    float held = c.held, phase = c.phase;
    for (int i = 0; i < n; ++i) {
        phase += holdRate_;
        if (phase >= 1.0f) {
            phase -= 1.0f;
            held = drv::quantize(dry[i] + crushOffset_, step_, invStep_) - crushOffset_;
        }
        out[i] = held;
    }
    c.held = held;
    c.phase = phase;
}

} // namespace ef
