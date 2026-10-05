#include "rack.h"

#include <cstring>

namespace ef {

namespace {
constexpr float kFadeStep = 1.0f / (0.010f * kRate);   // on / off: 10 ms
constexpr float kDipStep = 1.0f / (0.003f * kRate);    // a new order: 3 ms out, 3 ms back

// 0 dB is exactly 1: an empty chain at unity passes the input through bit for bit.
float gainOf(float db) { return db == 0.0f ? 1.0f : dbToGain(db); }
}

bool validOrder(const int* order) {
    bool seen[RM_COUNT] = {};
    for (int k = 0; k < RM_COUNT; ++k) {
        if (order[k] < 0 || order[k] >= RM_COUNT || seen[order[k]]) return false;
        seen[order[k]] = true;
    }
    return true;
}

Rack::Rack() = default;

void Rack::reset() {
    for (int m = 0; m < RM_COUNT; ++m) resetModule(m);
    fresh_ = true;
}

void Rack::resetModule(int m) {
    switch (m) {
        case RM_DRIVE: drive_.reset(); break;
        case RM_FILTER: filter_.reset(); break;
        case RM_EQ: eq_.reset(); break;
        case RM_COMP: comp_.reset(); break;
        case RM_CHORUS: chorus_.reset(); break;
        case RM_PHASER: phaser_.reset(); break;
        case RM_DELAY: delay_.reset(); break;
        case RM_REVERB: reverb_.reset(); break;
        default: break;
    }
    dirty_[m] = false;
}

int Rack::tailSamples() const {
    int t = 0;
    const int tails[RM_COUNT] = {drive_.tailSamples(), filter_.tailSamples(), eq_.tailSamples(), comp_.tailSamples(),
                                 chorus_.tailSamples(), phaser_.tailSamples(), delay_.tailSamples(), reverb_.tailSamples()};
    for (int m = 0; m < RM_COUNT; ++m)
        if (fade_[m] > 0.0f) t = std::max(t, tails[m]);
    return t;
}

int Rack::running() const { return running_; }

void Rack::runModule(int m, const RackPatch& p, const Transport& t, float* L, float* R, int n) {
    const float target = p.on[m] ? 1.0f : 0.0f;
    if (fade_[m] == 0.0f && target == 0.0f) return;   // off: no CPU at all
    if (fade_[m] == 0.0f && dirty_[m]) resetModule(m);   // back on: from cleared state
    dirty_[m] = true;
    ++running_;
    const bool fading = fade_[m] != target;
    if (fading) {
        std::memcpy(inL_, L, sizeof(float) * static_cast<size_t>(n));
        std::memcpy(inR_, R, sizeof(float) * static_cast<size_t>(n));
    }
    switch (m) {
        case RM_DRIVE: drive_.set(p.drive, t); drive_.process(L, R, n); break;
        case RM_FILTER: filter_.set(p.filter, t); filter_.process(L, R, n); break;
        case RM_EQ: eq_.set(p.eq, t); eq_.process(L, R, n); break;
        case RM_COMP: comp_.set(p.comp, t); comp_.process(L, R, n); break;
        case RM_CHORUS: chorus_.set(p.chorus, t); chorus_.process(L, R, n); break;
        case RM_PHASER: phaser_.set(p.phaser, t); phaser_.process(L, R, n); break;
        case RM_DELAY: delay_.set(p.delay, t); delay_.process(L, R, n); break;
        case RM_REVERB: reverb_.set(p.reverb, t); reverb_.process(L, R, n); break;
        default: break;
    }
    if (!fading) return;
    float g = fade_[m];
    const float step = target > g ? kFadeStep : -kFadeStep;
    for (int i = 0; i < n; ++i) {
        g = std::clamp(g + step, 0.0f, 1.0f);
        L[i] = inL_[i] + (L[i] - inL_[i]) * g;
        R[i] = inR_[i] + (R[i] - inR_[i]) * g;
    }
    fade_[m] = g;
}

void Rack::process(const RackPatch& p, const Transport& t, float* L, float* R, int n) {
    const bool orderOk = validOrder(p.order);
    if (fresh_) {
        fresh_ = false;
        in_.jump(gainOf(p.inDb));
        out_.jump(gainOf(p.outDb));
        mix_.jump(std::clamp(p.mix, 0.0f, 1.0f));
        for (int m = 0; m < RM_COUNT; ++m) fade_[m] = p.on[m] ? 1.0f : 0.0f;
        if (orderOk) std::copy(p.order, p.order + RM_COUNT, order_);
        dip_ = 1.0f;
        dipDir_ = 0;
    } else {
        in_.to(gainOf(p.inDb), n);
        out_.to(gainOf(p.outDb), n);
        mix_.to(std::clamp(p.mix, 0.0f, 1.0f), n);
    }
    if (dipDir_ == 0 && orderOk && !std::equal(p.order, p.order + RM_COUNT, order_)) {
        // Modules that are off (and not fading) don't sound: moving them changes nothing you hear, so
        // the new order goes straight in. Only a new sequence of sounding modules dips the output.
        int a[RM_COUNT], b[RM_COUNT], na = 0, nb = 0;
        for (int k = 0; k < RM_COUNT; ++k) {
            if (fade_[order_[k]] > 0.0f || p.on[order_[k]]) a[na++] = order_[k];
            if (fade_[p.order[k]] > 0.0f || p.on[p.order[k]]) b[nb++] = p.order[k];
        }
        if (na == nb && std::equal(a, a + na, b)) std::copy(p.order, p.order + RM_COUNT, order_);
        else dipDir_ = -1;
    }

    std::memcpy(dryL_, L, sizeof(float) * static_cast<size_t>(n));
    std::memcpy(dryR_, R, sizeof(float) * static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float g = in_.next();
        L[i] *= g;
        R[i] *= g;
    }
    running_ = 0;
    for (int k = 0; k < RM_COUNT; ++k) runModule(order_[k], p, t, L, R, n);

    for (int i = 0; i < n; ++i) {
        if (dipDir_ != 0) dip_ = std::clamp(dip_ + (dipDir_ < 0 ? -kDipStep : kDipStep), 0.0f, 1.0f);
        const float o = out_.next() * dip_, w = mix_.next();
        L[i] = dryL_[i] + (L[i] * o - dryL_[i]) * w;
        R[i] = dryR_[i] + (R[i] * o - dryR_[i]) * w;
    }
    if (dipDir_ < 0 && dip_ == 0.0f) {   // silent: the new order goes in, then fades back
        if (orderOk) std::copy(p.order, p.order + RM_COUNT, order_);
        dipDir_ = 1;
    } else if (dipDir_ > 0 && dip_ == 1.0f) {
        dipDir_ = 0;
    }
}

} // namespace ef
