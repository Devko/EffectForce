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
#include "simd.h"

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

    // Whether the head reads what was written since the restart, at full level.
    bool sounding() const { return age >= static_cast<uint32_t>(d) + static_cast<uint32_t>(kOnset) + 2u; }

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
    // What the windows read is gathered from the ring first, so that four lags' sums run as one
    // vector, each in the same order as on its own: the same sums, the same choice, far fewer
    // instructions (the search is one sample's work, every grain). Out of line: the hot loop
    // calling tick() keeps its registers.
    __attribute__((noinline)) int lag() const {
        const int dNow = static_cast<int>(d);
        int lo = jump - kSearch, hi = jump + kSearch;
        if (!up) hi = std::min(hi, dNow - static_cast<int>(kMinDelay) - 2);
        // Too little heard yet to search (the first splice after a restart): the nominal jump, but
        // going up never past what was written since, which would read silence.
        if (lo < 1 || hi <= lo || age < static_cast<uint32_t>(dNow + hi + kWindow + 8))
            return std::max(1, std::min(up ? std::min(jump, static_cast<int>(age) - dNow - 3) : jump, hi));
        constexpr int kCoarse = kWindow / 4, kFine = kWindow / 2, kNear = 3;
        constexpr int kLags = (2 * kSearch) / 4 + 1, kGroups = (kLags + 3) / 4 * 4;
        // Coarse: lag lo + 4 i against the old head's window. Its sample j reads the ring at
        // dNow +- (lo + 4 i) + 4 j: going up b[i + j], going down b[i - j + kCoarse - 1].
        alignas(16) float a[kCoarse], b[kGroups + kCoarse], sums[kGroups];
        const int lags = (hi - lo) / 4 + 1;
        for (int j = 0; j < kCoarse; ++j) a[j] = at(static_cast<uint32_t>(dNow + 4 * j));
        const int span = (lags + 3) / 4 * 4 + kCoarse - 1;
        for (int m = 0; m < span; ++m)
            b[m] = at(static_cast<uint32_t>(up ? dNow + lo + 4 * m : dNow - lo + 4 * (kCoarse - 1) - 4 * m));
        for (int i = 0; i < lags; i += 4) {
            f4 sum = splat(0.0f);
            const float* const q = up ? b + i : b + i + kCoarse - 1;
            for (int j = 0; j < kCoarse; ++j) sum += splat(a[j]) * load4(up ? q + j : q - j);
            store4(sums + i, sum);
        }
        int best = jump;
        float top = -1e30f;
        for (int i = 0; i < lags; ++i)
            if (sums[i] > top) {
                top = sums[i];
                best = lo + 4 * i;
            }
        // Fine: every lag within kNear of it, every 2nd sample. Lag center - kNear + r, sample j:
        // going up e[r + 2 j], going down e[r - 2 j + kWindow - 2].
        const int center = best, from = std::max(lo, center - kNear), to = std::min(hi, center + kNear);
        alignas(16) float a2[kFine], e[8 + kWindow - 2], near[8];
        for (int j = 0; j < kFine; ++j) a2[j] = at(static_cast<uint32_t>(dNow + 2 * j));
        const int base = center - kNear;
        for (int m = 0; m < 8 + kWindow - 2; ++m)
            e[m] = at(static_cast<uint32_t>(up ? dNow + base + m : dNow - base + kWindow - 2 - m));
        for (int r = 0; r < 8; r += 4) {
            f4 sum = splat(0.0f);
            const float* const q = up ? e + r : e + r + kWindow - 2;
            for (int j = 0; j < kFine; ++j) sum += splat(a2[j]) * load4(up ? q + 2 * j : q - 2 * j);
            store4(near + r, sum);
        }
        top = -1e30f;
        for (int l = from; l <= to; ++l)
            if (near[l - base] > top) {
                top = near[l - base];
                best = l;
            }
        return best;
    }
};

} // namespace ef
