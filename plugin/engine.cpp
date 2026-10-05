#include "engine.h"

#include <algorithm>
#include <cmath>

namespace ef {

namespace {
// Each slot's parameters, the same distance apart.
static_assert(P_M2_SRC - P_M1_SRC == P_M8_SRC - P_M7_SRC && P_M1_DST - P_M1_SRC == P_M8_DST - P_M8_SRC &&
                  P_M1_AMT - P_M1_SRC == P_M8_AMT - P_M8_SRC,
              "the matrix slots' parameters must repeat in the same order");
constexpr int kSlotStride = P_M2_SRC - P_M1_SRC;
}

Engine::Engine() {
    float def[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) def[i] = PARAM_INFO[i].def;
    setParams(def);
    reset();
}

void Engine::seed(uint32_t s) {
    seed_ = s ? s : 1u;
    lfo_[0].reset(seed_);
    lfo_[1].reset(seed_ * 0x9E3779B9u + 7u);
}

void Engine::reset() {
    rack_.reset();
    env_.reset();
    seed(seed_);
    std::fill(src_, src_ + MS_COUNT, 0.0f);
}

void Engine::setParams(const float* norm) {
    std::copy(norm, norm + P_COUNT, norm_);
    base_ = patchFromParams(norm_);
    nSlots_ = 0;
    for (int k = 0; k < kNumModSlots; ++k) {
        const int at = P_M1_SRC + k * kSlotStride;
        const int src = static_cast<int>(paramValue(at, norm_[at]));
        const int dst = static_cast<int>(paramValue(at + (P_M1_DST - P_M1_SRC), norm_[at + (P_M1_DST - P_M1_SRC)]));
        const float amount = paramValue(at + (P_M1_AMT - P_M1_SRC), norm_[at + (P_M1_AMT - P_M1_SRC)]);
        if (src <= MS_OFF || src >= MS_COUNT || dst <= 0 || dst >= kNumModTargets || amount == 0.0f) continue;
        slots_[nSlots_++] = {src, kModTargetParam[dst], amount};
    }
    env_.set(base_.envAttackS, base_.envReleaseS, base_.envGainDb);
    patch_ = base_;   // modulate() rewrites the fields the matrix reaches, every chunk
}

void Engine::modulate() {
    int done[kNumModSlots];
    int nDone = 0;
    for (int s = 0; s < nSlots_; ++s) {
        const int param = slots_[s].param;
        if (std::find(done, done + nDone, param) != done + nDone) continue;
        done[nDone++] = param;
        float sum = 0.0f;   // every slot on this target
        for (int k = s; k < nSlots_; ++k)
            if (slots_[k].param == param) sum += slots_[k].amount * src_[slots_[k].src];
        const float n = std::clamp(norm_[param] + sum, 0.0f, 1.0f);
        const float v = paramValue(param, n);
        setField(patch_, param, v);
        // A synced delay has no time of its own: the knob's move scales the synced time instead.
        if (param == P_DLY_TIME && base_.delay.sync) {
            const float baseV = paramValue(P_DLY_TIME, norm_[P_DLY_TIME]);
            patch_.delay.divBeats = baseV > 0.0f ? base_.delay.divBeats * v / baseV : base_.delay.divBeats;
        }
    }
}

void Engine::render(float* L, float* R, int n, Transport t) {
    for (int pos = 0; pos < n; pos += kChunk) {
        const int m = std::min(kChunk, n - pos);
        for (int k = 0; k < 4; ++k) src_[MS_MACRO1 + k] = norm_[P_MAC_1 + k];   // a macro's knob is its value
        const RackPatch* use = &base_;
        if (nSlots_ > 0) {
            modulate();
            use = &patch_;
        }
        // The sources for the next chunk, from this one's input and position.
        src_[MS_ENV] = env_.process(L + pos, R + pos, m);
        src_[MS_LFO1] = lfo_[0].next(use->lfo[0], t, m);
        src_[MS_LFO2] = lfo_[1].next(use->lfo[1], t, m);
        rack_.process(*use, t, L + pos, R + pos, m);
        t.beats += m / static_cast<double>(kRate) * t.bpm / 60.0;
    }
}

} // namespace ef
