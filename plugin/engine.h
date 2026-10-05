#pragma once
// The rack plus its modulation: two LFOs, the envelope follower, the macros and the 8-slot matrix
// (docs/DESIGN.md "Modulation"). Modulation adds amount x source to a target's 0..1 value, so it
// follows the knob's own curve, and is worked out once per 32-sample chunk from the sources of the
// chunk before (0.7 ms late: nothing hears it). Audio thread only, after construction.
#include "param_ids.h"
#include "rack_map.h"

namespace ef {

class Engine {
public:
    Engine();   // allocates the rack (UI thread)

    void setParams(const float* norm);   // a new snapshot of every parameter (0..1)
    void reset();                        // clears every tail and modulation source
    void seed(uint32_t s);               // the LFOs' random values (S&H, Smooth)
    // In place, any n: chunks of kChunk with the transport advanced for each.
    void render(float* L, float* R, int n, Transport t);

    int tailSamples() const { return rack_.tailSamples(); }
    int running() const { return rack_.running(); }
    const RackPatch& patch() const { return base_; }                          // the knobs as they are
    const RackPatch& current() const { return nSlots_ > 0 ? patch_ : base_; } // as the last chunk played them
    float source(int s) const { return s >= 0 && s < MS_COUNT ? src_[s] : 0.0f; }

private:
    void modulate();   // patch_ = base_ moved by the matrix (setParams() copies base_, this moves its targets)

    Rack rack_;
    Lfo lfo_[2];
    EnvFollower env_;
    RackPatch base_, patch_;
    float norm_[P_COUNT] = {};
    float src_[MS_COUNT] = {};   // each source's value from the chunk before
    struct Slot {
        int src, param;
        float amount;
        float logRatio;   // a log-curve target: log2(hi / lo), so its value is lo 2^(n logRatio); else 0
    };
    Slot slots_[kNumModSlots] = {};
    int nSlots_ = 0;
    uint32_t seed_ = 1u;
};

} // namespace ef
