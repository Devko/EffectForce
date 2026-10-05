#include "looper.h"

#include <cstring>
#include <new>

namespace ef {

namespace {
constexpr int64_t kMask = Looper::kRingFrames - 1;
const float kMixCoef = smoothCoef(0.003f);   // the live / loop crossfade's 3 ms
constexpr float kSilentMix = 1e-5f;          // under this, a loop that is let go has faded out
constexpr double kEps = 1e-9;

float finiteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }
}

Looper::~Looper() { delete store_.load(std::memory_order_acquire); }

bool Looper::allocate() {
    if (store_.load(std::memory_order_acquire)) return true;
    try {
        Store* s = new Store;
        s->ring.assign(2 * static_cast<size_t>(kRingFrames), 0.0f);
        s->loop.assign(2 * static_cast<size_t>(kMaxLoop + 2 * kGuard), 0.0f);
        store_.store(s, std::memory_order_release);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void Looper::reset() {
    recorded_ = written_ = 0;
    have_ = engaged_ = false;
    len_ = 0;
    lenBeats_ = 0.0;
    copied_ = copyTotal_ = 0;
    pos_ = old_ = 0.0;
    fade_ = 0;
    regStart_ = regLen_ = slice_ = 0.0;
    mix_ = mixTarget_ = 0.0f;
    freeBeat_ = 0.0;
    fresh_ = true;
}

double Looper::gridFrames(double periodBeats) const {
    const double c = beat_ / periodBeats;
    return (c - std::floor(c)) * periodBeats * spb_;
}

bool Looper::grab(double lengthBeats) {
    const int len = static_cast<int>(std::clamp(std::lround(lengthBeats * spb_), 64L, static_cast<long>(kMaxLoop)));
    // Frames since the cell's end (the grid's last line), whole: the head starts on the very frame
    // recorded one length ago, the sample after the one just played.
    int64_t into = std::llround(gridFrames(lengthBeats));
    if (into >= len) into -= len;   // the rounding of a non-whole number of frames
    const int64_t end = abs_ - into;
    const int64_t start = end - len;
    if (start < abs_ - recorded_) return false;   // not recorded (yet)
    cellAbs_ = start;
    len_ = len;
    lenBeats_ = lengthBeats;
    copied_ = 0;
    copyTotal_ = len + 2 * kGuard;
    have_ = true;
    regStart_ = 0.0;
    regLen_ = len;
    slice_ = 0.0;
    pos_ = static_cast<double>(std::max<int64_t>(into, 0));
    fade_ = 0;
    return true;
}

void Looper::jumpTo(double target) {
    // A grid position a rounding error off a whole frame is that frame: the head then wraps on the
    // same sample however the beats were summed (chunk sizes).
    const double whole = std::round(target);
    if (std::fabs(target - whole) < 1e-4) target = whole;
    if (std::fabs(target - pos_) < 0.5) {   // as good as there: no crossfade for a sub-frame
        pos_ = target;
        return;
    }
    old_ = pos_;
    pos_ = target;
    fade_ = kFade;
}

void Looper::setSlice(double sliceBeats) {
    const bool lock = std::fabs(len_ - lenBeats_ * spb_) < 2.0;   // the loop still fits the tempo's grid
    // The head's place in the whole loop (it may be past an edge, playing into a crossfade).
    double at = std::fmod(pos_, static_cast<double>(len_));
    if (at < 0.0) at += len_;
    if (sliceBeats <= 0.0) {
        regStart_ = 0.0;
        regLen_ = len_;
        jumpTo(lock ? std::fmod(gridFrames(lenBeats_), static_cast<double>(len_)) : at);
    } else {
        const double r = std::min(sliceBeats * spb_, static_cast<double>(len_));
        const double s = std::min(std::floor(at / r) * r, len_ - r);
        regStart_ = std::max(s, 0.0);
        regLen_ = r;
        jumpTo(regStart_ + std::fmod(gridFrames(sliceBeats), r));
    }
    slice_ = sliceBeats;
}

void Looper::set(const Params& p, const Transport& t) {
    st_ = store_.load(std::memory_order_acquire);
    bpm_ = std::isfinite(t.bpm) ? std::clamp(t.bpm, 1.0, 1000.0) : 120.0;
    spb_ = kRate * 60.0 / bpm_;
    beat_ = t.valid && t.playing && std::isfinite(t.beats) ? t.beats : freeBeat_;
    armed_ = p.on && st_ != nullptr;

    double lb = p.lengthBeats > 0.0 && std::isfinite(p.lengthBeats) ? p.lengthBeats : 4.0;
    while (lb * spb_ > kMaxLoop && lb > kLoopLens[0].beats) lb *= 0.5;
    speedTarget_ = std::clamp(finiteOr(p.speed, 1.0f), -1.0f, 2.0f);
    const float loop = armed_ ? std::clamp(finiteOr(p.loop, 0.0f), 0.0f, 1.0f) : 0.0f;
    if (fresh_) {
        fresh_ = false;
        speed_ = speedTarget_;
        mix_ = 0.0f;
    }

    // Let go of a loop that has faded out; take one when Loop leaves 0.
    if (engaged_ && loop == 0.0f && mix_ < kSilentMix) {
        engaged_ = false;
        mix_ = 0.0f;
        fade_ = 0;
    }
    if (loop > 0.0f && !engaged_) {
        if (p.hold && have_) {   // the last loop again, from where the grid is
            engaged_ = true;
            if (std::fabs(len_ - lenBeats_ * spb_) < 2.0) pos_ = std::fmod(gridFrames(lenBeats_), static_cast<double>(len_));
            fade_ = 0;
            slice_ = -1.0;   // the region is worked out again below
        } else if (grab(lb)) {
            engaged_ = true;
        }
    }
    mixTarget_ = engaged_ ? loop : 0.0f;
    if (!engaged_) return;

    // The region: a Repeat, or a Length shorter than the grabbed loop, plays a slice of it.
    double slice = 0.0;
    if (lb < lenBeats_ - kEps) slice = lb;
    const double rb = p.repeatBeats;
    if (rb > 0.0 && std::isfinite(rb) && rb < lenBeats_ - kEps) slice = slice > 0.0 ? std::min(slice, rb) : rb;
    if (slice != slice_) {
        setSlice(slice);
        return;
    }
    // At speed 1, back onto the grid if the head has left it.
    const bool steady = std::fabs(speedTarget_ - 1.0f) < 1e-3f && std::fabs(speed_ - 1.0f) < 1e-3f;
    if (!steady || fade_ > 0 || std::fabs(len_ - lenBeats_ * spb_) >= 2.0) return;
    const double target = slice > 0.0 ? regStart_ + std::fmod(gridFrames(slice), regLen_)
                                      : std::fmod(gridFrames(lenBeats_), static_cast<double>(len_));
    double d = target - pos_;
    d -= std::floor(d / regLen_ + 0.5) * regLen_;   // the short way round
    if (std::fabs(d) > kDrift) jumpTo(target);
}

void Looper::frame(int k, float& l, float& r) const {
    const int b = k + kGuard;
    if (b < 0 || b >= copyTotal_) {
        l = r = 0.0f;
        return;
    }
    if (b < copied_) {
        const float* f = st_->loop.data() + 2 * static_cast<size_t>(b);
        l = f[0];
        r = f[1];
        return;
    }
    const int64_t a = cellAbs_ + k;   // not copied yet: the ring still has it, if it was recorded
    if (a >= abs_ || a < abs_ - written_) {
        l = r = 0.0f;
        return;
    }
    const float* f = st_->ring.data() + 2 * static_cast<size_t>(a & kMask);
    l = f[0];
    r = f[1];
}

void Looper::read(double pos, float& l, float& r) const {
    const double fl = std::floor(pos);
    const int k = static_cast<int>(fl);
    const float t = static_cast<float>(pos - fl);
    float l0, r0, l1, r1, l2, r2, l3, r3;
    frame(k - 1, l0, r0);
    frame(k, l1, r1);
    frame(k + 1, l2, r2);
    frame(k + 2, l3, r3);
    l = hermite(l0, l1, l2, l3, t);
    r = hermite(r0, r1, r2, r3, t);
}

void Looper::process(float* L, float* R, int n) {
    if (!st_) {   // nothing allocated: the input passes
        freeBeat_ = beat_ + n / spb_;
        return;
    }
    // Record.
    if (armed_) {
        float* ring = st_->ring.data();
        for (int i = 0; i < n; ++i) {
            float* f = ring + 2 * static_cast<size_t>((abs_ + i) & kMask);
            f[0] = sanitize(L[i]);
            f[1] = sanitize(R[i]);
        }
        abs_ += n;
        recorded_ = std::min<int64_t>(recorded_ + n, kRingFrames);
        written_ = std::min<int64_t>(written_ + n, kRingFrames);
    } else {
        recorded_ = 0;
    }
    // Copy the cell into the loop buffer, as far as it is recorded.
    if (have_ && copied_ < copyTotal_) {
        int budget = kCopyRate * n;
        while (budget > 0 && copied_ < copyTotal_) {
            const int64_t a = cellAbs_ - kGuard + copied_;
            if (a >= abs_) break;
            float* dst = st_->loop.data() + 2 * static_cast<size_t>(copied_);
            if (a < abs_ - written_) {   // never recorded (or overwritten, which these sizes rule out): silence
                dst[0] = dst[1] = 0.0f;
                ++copied_;
                --budget;
                continue;
            }
            const int idx = static_cast<int>(a & kMask);
            const int run = static_cast<int>(std::min<int64_t>(
                {static_cast<int64_t>(budget), static_cast<int64_t>(copyTotal_ - copied_),
                 static_cast<int64_t>(kRingFrames - idx), abs_ - a}));
            std::memcpy(dst, st_->ring.data() + 2 * static_cast<size_t>(idx), sizeof(float) * 2 * static_cast<size_t>(run));
            copied_ += run;
            budget -= run;
        }
    }
    freeBeat_ = beat_ + n / spb_;
    if (!engaged_) return;

    // Play.
    const float s0 = speed_, ds = (speedTarget_ - speed_) / static_cast<float>(n);
    const double end = regStart_ + regLen_;
    for (int i = 0; i < n; ++i) {
        const float speed = s0 + ds * static_cast<float>(i + 1);
        mix_ += (mixTarget_ - mix_) * kMixCoef;
        // Snap the last bit: in float the one-pole stalls ~4e-6 short (its step rounds away).
        if (std::fabs(mixTarget_ - mix_) < 1e-5f) mix_ = mixTarget_;
        float l, r;
        read(pos_, l, r);
        if (fade_ > 0) {
            float ol, orr;
            read(old_, ol, orr);
            const float u = static_cast<float>(fade_) / kFade;   // 1 -> 0 across the fade
            const float wOld = 0.5f - 0.5f * std::cos(3.14159265f * u);
            l += (ol - l) * wOld;
            r += (orr - r) * wOld;
            old_ += speed;
            --fade_;
        }
        pos_ += speed;
        if (pos_ >= end) {   // the edge: wrap, the old head playing on past it
            old_ = pos_;
            pos_ -= regLen_;
            fade_ = kFade;
        } else if (pos_ < regStart_) {
            old_ = pos_;
            pos_ += regLen_;
            fade_ = kFade;
        }
        const float g = std::min(std::fabs(speed) * 8.0f, 1.0f);   // a tape stop fades out
        if (mix_ >= 1.0f) {   // the loop alone (whatever the live input holds, a NaN too)
            L[i] = l * g;
            R[i] = r * g;
        } else {
            L[i] += (l * g - L[i]) * mix_;
            R[i] += (r * g - R[i]) * mix_;
        }
    }
    speed_ = speedTarget_;
}

} // namespace ef
