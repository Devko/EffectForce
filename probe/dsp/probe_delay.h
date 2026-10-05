#pragma once
// The probe's sound (the probe build only; the Delay module is dsp/delay.h): input gain into a
// tempo-synced stereo delay with feedback. Simple on purpose:
// it only has to make MPC's behaviour audible (tempo, tails, bypass, latency), not sound finished.
//
// Every control glides per sample, so knob moves, tempo changes and automation never click.
// Safe in place (in == out): each sample is read before it is written.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ef {

class StereoDelay {
public:
    static constexpr int kSize = 1 << 18;   // 5.9 s at 44.1 kHz: 1 bar down to 41 BPM; longer is clamped

    explicit StereoDelay(float sampleRate)
        : sr_(sampleRate),
          smooth_(1.0f - std::exp(-1.0f / (0.010f * sampleRate))),   // gain, feedback, mix: 10 ms
          glide_(1.0 - std::exp(-1.0 / (0.060 * sampleRate))),       // delay time: see glideTime()
          bufL_(kSize, 0.0f),
          bufR_(kSize, 0.0f) {}

    // Targets for the next block. The first call after construction or clear() jumps there.
    void set(float gainLin, double delaySamples, float feedback, float mix) {
        tGain_ = gainLin;
        tTime_ = std::clamp(delaySamples, 1.0, static_cast<double>(kSize - 4));
        tFb_ = std::clamp(feedback, 0.0f, 0.99f);
        tMix_ = std::clamp(mix, 0.0f, 1.0f);
        if (fresh_) {
            gain_ = tGain_;
            time_ = tTime_;
            fb_ = tFb_;
            mix_ = tMix_;
            fresh_ = false;
        }
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int n) {
        for (int i = 0; i < n; ++i) {
            gain_ += (tGain_ - gain_) * smooth_;
            fb_ += (tFb_ - fb_) * smooth_;
            mix_ += (tMix_ - mix_) * smooth_;
            time_ = glideTime(time_, tTime_);

            // A NaN or infinity from the host would circulate in the feedback loop for good.
            const float xl = std::isfinite(inL[i]) ? inL[i] * gain_ : 0.0f;
            const float xr = std::isfinite(inR[i]) ? inR[i] * gain_ : 0.0f;
            const float wl = read(bufL_, time_), wr = read(bufR_, time_);
            bufL_[static_cast<size_t>(write_)] = xl + wl * fb_;
            bufR_[static_cast<size_t>(write_)] = xr + wr * fb_;
            write_ = (write_ + 1) & (kSize - 1);
            outL[i] = xl + (wl - xl) * mix_;
            outR[i] = xr + (wr - xr) * mix_;
        }
    }

    void clear() {
        std::fill(bufL_.begin(), bufL_.end(), 0.0f);
        std::fill(bufR_.begin(), bufR_.end(), 0.0f);
        fresh_ = true;
    }

    // How long the echoes ring after the input stops, down to -60 dB, in samples (effGetTailSize).
    int64_t tailSamples() const {
        const double t = tTime_;
        if (tFb_ < 1e-3f) return static_cast<int64_t>(t) + 1;
        const double repeats = std::ceil(std::log(1e-3) / std::log(static_cast<double>(tFb_)));
        return static_cast<int64_t>(t * (repeats + 1.0)) + 1;
    }

    float sampleRate() const { return sr_; }

private:
    // How the delay time follows a new target (a tempo or division change), one sample at a time.
    // Placeholder: a 60 ms one-pole glide, as a tape delay's motor would: echoes already in
    // flight bend in pitch while the time moves, more the further it has to go.
    // In double: in float the step drops under the precision of a time near 22050 samples and the
    // glide stalls ~2.6 samples short of its target, for good.
    // TODO(EffectForce): pick the behaviour the real delay keeps (see docs/PROBE.md, "Your part").
    double glideTime(double current, double target) const {
        return current + (target - current) * glide_;
    }

    // Linear interpolation `delay` samples behind the write position.
    float read(const std::vector<float>& buf, double delay) const {
        const double pos = static_cast<double>(write_) - delay;
        const double fl = std::floor(pos);
        const float frac = static_cast<float>(pos - fl);
        const int i0 = static_cast<int>(fl) & (kSize - 1);
        const int i1 = (i0 + 1) & (kSize - 1);
        return buf[static_cast<size_t>(i0)] + (buf[static_cast<size_t>(i1)] - buf[static_cast<size_t>(i0)]) * frac;
    }

    float sr_, smooth_;
    double glide_;
    std::vector<float> bufL_, bufR_;
    int write_ = 0;
    bool fresh_ = true;
    float gain_ = 1.0f, fb_ = 0.0f, mix_ = 0.0f;
    float tGain_ = 1.0f, tFb_ = 0.0f, tMix_ = 0.0f;
    double time_ = 1.0, tTime_ = 1.0;   // samples
};

// A fixed delay on the output, for the latency test: the plugin reports it as initialDelay, and
// whether MPC shifts the other tracks to match is what the test hears.
class LatencyLine {
public:
    explicit LatencyLine(int samples) : n_(std::max(samples, 0)), bufL_(static_cast<size_t>(n_), 0.0f), bufR_(bufL_) {}

    int samples() const { return n_; }

    void process(float* L, float* R, int n) {
        if (n_ == 0) return;
        for (int i = 0; i < n; ++i) {
            std::swap(L[i], bufL_[static_cast<size_t>(pos_)]);
            std::swap(R[i], bufR_[static_cast<size_t>(pos_)]);
            if (++pos_ == n_) pos_ = 0;
        }
    }

    void clear() {
        std::fill(bufL_.begin(), bufL_.end(), 0.0f);
        std::fill(bufR_.begin(), bufR_.end(), 0.0f);
    }

private:
    int n_;
    std::vector<float> bufL_, bufR_;
    int pos_ = 0;
};

} // namespace ef
