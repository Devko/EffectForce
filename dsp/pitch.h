#pragma once
// A pitch shifter for feedback paths (the Reverb's shimmer; a Grain module can reuse it): a read
// head moves through a delay line at `ratio` times the speed it is written, so the pitch is exact.
// When it runs out of room (every `grain` samples or so) it jumps back by about the length it
// covered and crossfades (raised cosine, kFade samples) from where it was. The jump is the nominal
// one moved by up to kSearch samples to where the signal best repeats (autocorrelation, as SOLA
// does): a steady tone splices in phase, so it comes out a clean tone at exactly ratio x its pitch,
// without the flutter (and spectral lines half the grain rate either side) of fixed jumps; dense
// material just gets slightly irregular splices, which keeps it from sounding periodic.
//
// The crossfade's weights sum to exactly 1, so it never adds energy: in phase it passes the
// level, out of phase (noise) it keeps 3/4 of the power for the fade's few milliseconds (97% on
// average with the Reverb's 80 ms grains). A loop through it is as stable as one without.
//
// 4-point Hermite reads. No band-limiting here: a shift up folds what lands above Nyquist, so the
// caller low-passes the input under rate / 2 / ratio.
//
// The state is small and copyable, so a hot loop runs a local copy (in registers) and writes it
// back; the buffer belongs to the caller (attach()). restart() forgets the buffer without clearing
// it: a read reaching back before the restart is a zero, and the output fades in (kOnset samples)
// as the head reaches what was written since, rather than stepping in.
#include "common.h"

#include <cstdint>

namespace ef {

struct PitchShift {
    static constexpr float kMinDelay = 4.0f;   // the head's closest approach to the write position
    static constexpr int kFade = 384;          // the splice's crossfade: 8.7 ms
    static constexpr int kSearch = 330;        // the jump moves up to this far: in phase down to 134 Hz
    static constexpr int kWindow = 256;        // the stretch compared at a splice
    static constexpr float kOnset = 256.0f;    // after a restart, the head's reads fade in over this

    // The buffer a range of ratios and a grain duration need (a power of two, in floats).
    static uint32_t bufferSize(float maxRatio, float minRatio, int grainSamples) {
        const float up = maxRatio - 1.0f, down = 1.0f - minRatio;
        const float reachUp = kMinDelay + up * kFade + up * grainSamples + kSearch + kWindow;
        const float reachDown = kMinDelay + kSearch + down * grainSamples + down * kFade + kWindow;
        uint32_t n = 1;
        while (static_cast<float>(n) < std::max(reachUp, reachDown) + 8.0f) n <<= 1;
        return n;
    }

    void attach(float* buffer, uint32_t size) {   // size: a power of two
        buf = buffer;
        mask = size - 1;
        restart();
    }

    // A new ratio (> 0, not 1). A change while sounding moves the head; the caller fades around
    // it and restarts.
    void setRatio(float ratio, int grainSamples) {
        up = ratio > 1.0f;
        step = 1.0f - ratio;   // the head's delay per sample: falls for a higher pitch
        const float speed = up ? ratio - 1.0f : 1.0f - ratio;
        jump = static_cast<int>(speed * static_cast<float>(grainSamples) + 0.5f);
        // Where a splice starts: going up, early enough that the old head still has room for the
        // crossfade; going down, a sweep after the lowest delay.
        low = up ? kMinDelay + speed * kFade : kMinDelay + kSearch;
        splice = up ? low : low + static_cast<float>(jump);
    }

    // Everything written so far is forgotten: reads before this point come out as zeros (the 4
    // samples a read can straddle are cleared, the rest is never read), the head starts over.
    void restart() {
        for (uint32_t k = 0; k < 4; ++k) buf[(w - k) & mask] = 0.0f;
        age = 0;
        left = 0;
        d = up ? splice + static_cast<float>(jump) : low;
    }

    EF_INLINE float tick(float x) {
        w = (w + 1) & mask;
        buf[w] = x;
        if (age < kAgeMax) ++age;
        d += step;
        float y = read(d);
        if (left > 0) {   // crossfading from the old head
            dOld += step;
            float c = 0.5f * static_cast<float>(left) * (1.0f / kFade) - 0.25f;   // cos(pi (1 - left / kFade)) = sin(2 pi c)
            if (c < 0.0f) c += 1.0f;
            const float wNew = 0.5f - 0.5f * sinCycle(c), old = read(dOld);
            y = old + (y - old) * wNew;
            --left;
        } else if (up ? d <= splice : d >= splice) {
            dOld = d;
            const int l = lag();
            d += up ? static_cast<float>(l) : -static_cast<float>(l);
            left = kFade;
        }
        return y;
    }

    float* buf = nullptr;
    uint32_t mask = 0, w = 0, age = 0;   // age: samples written since restart (up to kAgeMax)
    float d = 0.0f, dOld = 0.0f;          // the head's delay, and the old head's while crossfading
    float step = 0.0f, low = 0.0f, splice = 0.0f;
    int jump = 0, left = 0;               // the nominal jump; samples of crossfade still to go
    bool up = true;

private:
    static constexpr uint32_t kAgeMax = 1u << 30;

    // Hermite at `delay` samples (delay 0: the sample just written); from before the restart a
    // zero, and fading in over kOnset samples after it.
    EF_INLINE float read(float delay) const {
        const uint32_t i = static_cast<uint32_t>(delay);
        const float t = delay - static_cast<float>(i);
        const float v = hermite(buf[(w - i + 1) & mask], buf[(w - i) & mask], buf[(w - i - 1) & mask], buf[(w - i - 2) & mask], t);
        if (age >= i + static_cast<uint32_t>(kOnset)) return v;   // long since the restart: the usual case
        const float fresh = static_cast<float>(static_cast<int32_t>(age) - static_cast<int32_t>(i)) * (1.0f / kOnset);
        return fresh > 0.0f ? v * fresh : 0.0f;
    }

    float at(uint32_t delay) const { return buf[(w - delay) & mask]; }

    // The jump for this splice: the nominal one, moved to where the kWindow samples behind the
    // new head look most like those behind the old (correlation, every 4th lag and sample first,
    // then every lag nearby). Going down the new head must stay kMinDelay behind the writes.
    int lag() const {
        const int dNow = static_cast<int>(d);
        int lo = jump - kSearch, hi = jump + kSearch;
        if (!up) hi = std::min(hi, dNow - static_cast<int>(kMinDelay) - 2);
        if (lo < 1 || hi <= lo || age < static_cast<uint32_t>(dNow + hi + kWindow + 8)) return std::max(1, std::min(jump, hi));
        auto corr = [&](int l, int stride) {
            float s = 0.0f;
            const uint32_t other = static_cast<uint32_t>(up ? dNow + l : dNow - l);
            for (int k = 0; k < kWindow; k += stride) s += at(static_cast<uint32_t>(dNow + k)) * at(other + static_cast<uint32_t>(k));
            return s;
        };
        int best = jump;
        float top = -1e30f;
        for (int l = lo; l <= hi; l += 4) {
            const float c = corr(l, 4);
            if (c > top) {
                top = c;
                best = l;
            }
        }
        const int center = best;
        top = -1e30f;
        for (int l = std::max(lo, center - 3); l <= std::min(hi, center + 3); ++l) {
            const float c = corr(l, 2);
            if (c > top) {
                top = c;
                best = l;
            }
        }
        return best;
    }
};

} // namespace ef
