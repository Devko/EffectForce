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

// The splice's crossfade: its new head's weight with `left` samples to go (1..kPitchFade), the
// expression tick() had, worked out once.
inline constexpr int kPitchFade = 384;
struct PitchFadeTable {
    float wNew[kPitchFade + 1] = {};
    PitchFadeTable() {
        for (int left = 1; left <= kPitchFade; ++left) {
            float c = 0.5f * static_cast<float>(left) * (1.0f / kPitchFade) - 0.25f;   // cos(pi (1 - left / kFade)) = sin(2 pi c)
            if (c < 0.0f) c += 1.0f;
            wNew[left] = 0.5f - 0.5f * sinCycle(c);
        }
    }
};
inline const PitchFadeTable kPitchFadeTable;

struct PitchShift {
    static constexpr float kMinDelay = 4.0f;   // the head's closest approach to the write position
    static constexpr int kFade = kPitchFade;   // the splice's crossfade: 8.7 ms
    static constexpr uint32_t kGuard = 4;      // the ring's first samples again after its end: a read never wraps
    static constexpr int kSearch = 330;        // the jump moves up to this far: in phase down to 134 Hz
    static constexpr int kWindow = 256;        // the stretch compared at a splice
    static constexpr float kOnset = 256.0f;    // after a restart, the head's reads fade in over this

    // The buffer a range of ratios and a grain duration need, in floats: a power of two (the
    // ring) and kGuard.
    static uint32_t bufferSize(float maxRatio, float minRatio, int grainSamples) {
        const float up = maxRatio - 1.0f, down = 1.0f - minRatio;
        const float reachUp = kMinDelay + up * kFade + up * grainSamples + kSearch + kWindow;
        const float reachDown = kMinDelay + kSearch + down * grainSamples + down * kFade + kWindow;
        uint32_t n = 1;
        while (static_cast<float>(n) < std::max(reachUp, reachDown) + 8.0f) n <<= 1;
        return n + kGuard;
    }

    void attach(float* buffer, uint32_t size) {   // size: bufferSize()'s, a power of two and kGuard
        buf = buffer;
        mask = size - kGuard - 1;
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
        for (uint32_t k = 0; k < 4; ++k) put((w - k) & mask, 0.0f);
        age = 0;
        left = 0;
        d = up ? splice + static_cast<float>(jump) : low;
    }

    // Whether the head reads what was written since the restart, at full level.
    bool sounding() const { return age >= static_cast<uint32_t>(d) + static_cast<uint32_t>(kOnset) + 2u; }

    EF_INLINE float tick(float x) {
        w = (w + 1) & mask;
        put(w, x);
        if (age < kAgeMax) ++age;
        d += step;
        float y = read(d);
        if (left > 0) {   // crossfading from the old head
            dOld += step;
            const float wNew = kPitchFadeTable.wNew[left], old = read(dOld);
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
        const float* const q = buf + ((w - i - 2) & mask);   // the oldest of the four, then newer (the guard after the end)
#if EF_NEON
        const float32x4_t x = vld1q_f32(q);
        const float v = hermite(vgetq_lane_f32(x, 3), vgetq_lane_f32(x, 2), vgetq_lane_f32(x, 1), vgetq_lane_f32(x, 0), t);
#else
        const float v = hermite(q[3], q[2], q[1], q[0], t);
#endif
        if (age >= i + static_cast<uint32_t>(kOnset)) return v;   // long since the restart: the usual case
        const float fresh = static_cast<float>(static_cast<int32_t>(age) - static_cast<int32_t>(i)) * (1.0f / kOnset);
        return fresh > 0.0f ? v * fresh : 0.0f;
    }

    float at(uint32_t delay) const { return buf[(w - delay) & mask]; }
    void put(uint32_t j, float x) {
        buf[j] = x;
        if (j < kGuard) buf[j + mask + 1] = x;
    }

    // The jump for this splice: the nominal one, moved to where the kWindow samples behind the
    // new head look most like those behind the old (correlation, every 4th lag and sample first,
    // then every lag nearby). Going down the new head must stay kMinDelay behind the writes.
    // The ring is unwrapped first (x behind the old head, y what the new heads' windows read), so
    // that sixteen lags' sums run as four vectors: each in the same order as on its own (the same
    // sums, the same choice) and four chains of multiply-adds instead of one. Out of line: the hot
    // loop calling tick() keeps its registers.
    __attribute__((noinline)) int lag() const {
        const int dNow = static_cast<int>(d);
        int lo = jump - kSearch, hi = jump + kSearch;
        if (!up) hi = std::min(hi, dNow - static_cast<int>(kMinDelay) - 2);
        // Too little heard yet to search (the first splice after a restart): the nominal jump, but
        // going up never past what was written since, which would read silence.
        if (lo < 1 || hi <= lo || age < static_cast<uint32_t>(dNow + hi + kWindow + 8))
            return std::max(1, std::min(up ? std::min(jump, static_cast<int>(age) - dNow - 3) : jump, hi));
        // x[k] = at(dNow + k), y[j] = at(base + j): lag l compares x with y from o(l) on, o = l - lo
        // going up, hi - l going down.
        const int range = hi - lo;   // <= 2 kSearch
        alignas(16) float x[kWindow], y[2 * kSearch + kWindow + 64];
        for (int k = 0; k < kWindow; ++k) x[k] = at(static_cast<uint32_t>(dNow + k));
        const uint32_t base = static_cast<uint32_t>(up ? dNow + lo : dNow - hi);
        for (int j = 0; j < range + kWindow; ++j) y[j] = at(base + static_cast<uint32_t>(j));
        for (int j = range + kWindow; j < range + kWindow + 64; ++j) y[j] = 0.0f;
        // Coarse: every 4th lag and sample. o = r + 4 c with p[i] = y[r + 4 i]: the sum for c is
        // x[4 m] p[c + m] over m, sixteen c a pass.
        const int q = range / 4, r = up ? 0 : range - 4 * q;
        alignas(16) float p[(2 * kSearch) / 4 + kWindow / 4 + 20], sums[(2 * kSearch) / 4 + 20];
        for (int i = 0; i < q + 16 + kWindow / 4; ++i) p[i] = y[r + 4 * i];
        for (int c = 0; c <= q; c += 16) {
            f4 a0 = splat(0.0f), a1 = a0, a2 = a0, a3 = a0;
            for (int m = 0; m < kWindow / 4; ++m) {
                const f4 xm = splat(x[4 * m]);
                a0 = a0 + xm * load4(p + c + m);
                a1 = a1 + xm * load4(p + c + m + 4);
                a2 = a2 + xm * load4(p + c + m + 8);
                a3 = a3 + xm * load4(p + c + m + 12);
            }
            store4(sums + c, a0);
            store4(sums + c + 4, a1);
            store4(sums + c + 8, a2);
            store4(sums + c + 12, a3);
        }
        int best = jump;
        float top = -1e30f;
        for (int l = lo, c = 0; l <= hi; l += 4, ++c) {
            const float v = sums[up ? c : q - c];
            if (v > top) {
                top = v;
                best = l;
            }
        }
        // Fine: every lag within 3 of it, every 2nd sample, eight offsets as two vectors.
        const int from = std::max(lo, best - 3), to = std::min(hi, best + 3);
        const int omin = up ? from - lo : hi - to;
        f4 b0 = splat(0.0f), b1 = b0;
        for (int m = 0; m < kWindow / 2; ++m) {
            const f4 xm = splat(x[2 * m]);
            b0 = b0 + xm * load4(y + omin + 2 * m);
            b1 = b1 + xm * load4(y + omin + 2 * m + 4);
        }
        alignas(16) float near[8];
        store4(near, b0);
        store4(near + 4, b1);
        top = -1e30f;
        for (int l = from; l <= to; ++l) {
            const float v = near[(up ? l - lo : hi - l) - omin];
            if (v > top) {
                top = v;
                best = l;
            }
        }
        return best;
    }
};

} // namespace ef
