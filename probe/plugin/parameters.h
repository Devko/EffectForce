#pragma once
// The parameter values as MPC sees them (0..1), their display text and the saved state.
// MPC's UI side writes, the audio thread reads: every value is an atomic float; `writes` counts
// changes so the audio thread looks only after one.
#include "param_ids.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace ef {

float realOf(int id, float norm);   // 0..1 -> lo..hi (an enum: its option index)
float normOf(int id, float real);

class Params {
public:
    Params();

    float get(int id) const;           // 0..1; a readout reads 0
    void set(int id, float norm);      // clamped; a readout, a bad index or a NaN is ignored
    float real(int id) const { return realOf(id, get(id)); }
    int option(int id) const;          // an enum's option index
    uint32_t writes() const { return writes_.load(std::memory_order_acquire); }

    std::string display(int id) const;  // a knob's or enum's text ("+3.0 dB", "1/8."); "" for a readout

    std::string save() const;              // "effectforce-probe 1" and key=value lines, real values
    bool load(const std::string& state);   // unknown keys skipped, missing ones keep their value

private:
    std::atomic<float> norm_[P_COUNT];
    std::atomic<uint32_t> writes_{0};
};

} // namespace ef
