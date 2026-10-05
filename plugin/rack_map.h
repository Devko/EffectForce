#pragma once
// MPC's 0..1 parameter values <-> real values, display text, and the rack's patch.
// Ranges, curves, names and options come from surface/surface.py via build/param_ids.h.
#include "param_ids.h"
#include "../dsp/rack.h"

#include <string>

namespace ef {

float paramValue(int id, float norm);    // real value (Hz, seconds, dB, option index...)
float paramNorm(int id, float value);    // inverse, for state text, tests and the bench
// What the knob's value label shows. `all` (every parameter's 0..1 value) for the one text that
// depends on another parameter: the phaser's centre is Hz for a phaser and ms for the flanger.
std::string paramDisplay(int id, float norm, const float* all = nullptr);

RackPatch patchFromParams(const float* norm);   // norm[P_COUNT]
// One parameter's real value into the patch (the modulation matrix moves single targets).
void setField(RackPatch& p, int id, float value);

} // namespace ef
