#pragma once
// The chain (docs/DESIGN.md "The rack"): input gain, the ten modules in the patch's order, output
// gain and the global dry / wet, one control chunk at a time.
//
//   - A module that is off is not processed at all. Switching fades over 10 ms; a module switched
//     on again starts from cleared state (no stale tail from long ago).
//   - A new order of the modules that sound dips the chain's output to silence over 3 ms, swaps, and
//     fades back over 3 ms; moving modules that are off changes nothing you hear, so no dip.
//   - Everything off with unity gains passes the input through bit for bit.
#include "common.h"
#include "chorus.h"
#include "comp.h"
#include "delay.h"
#include "drive.h"
#include "eq.h"
#include "filter.h"
#include "grain.h"
#include "mod.h"
#include "phaser.h"
#include "pulse.h"
#include "reverb.h"

namespace ef {

// The modules, in their default order (= surface.py MODULES; plugin/rack_map.cpp checks it).
enum RackModule : int { RM_DRIVE, RM_FILTER, RM_EQ, RM_COMP, RM_CHORUS, RM_PHASER, RM_PULSE, RM_GRAIN, RM_DELAY,
                        RM_REVERB, RM_COUNT };

struct RackPatch {
    float inDb = 0.0f, outDb = 0.0f, mix = 1.0f;
    int order[RM_COUNT] = {RM_DRIVE, RM_FILTER, RM_EQ, RM_COMP, RM_CHORUS, RM_PHASER, RM_PULSE, RM_GRAIN, RM_DELAY, RM_REVERB};
    bool on[RM_COUNT] = {};
    Drive::Params drive{};
    Filter::Params filter{};
    Eq::Params eq{};
    Comp::Params comp{};
    Chorus::Params chorus{};
    Phaser::Params phaser{};
    Pulse::Params pulse{};
    Grain::Params grain{};
    Delay::Params delay{};
    Reverb::Params reverb{};
    // The modulation sources' settings: the engine uses them, the rack doesn't.
    LfoParams lfo[2];
    float envAttackS = 0.01f, envReleaseS = 0.2f, envGainDb = 12.0f;
};

// True if order[] names every module once.
bool validOrder(const int* order);

class Rack {
public:
    Rack();   // allocates every module (UI thread)

    void reset();   // clears every module; the next chunk starts from the patch as it is
    // One chunk, in place, 1 <= n <= kChunk.
    void process(const RackPatch& p, const Transport& t, float* L, float* R, int n);

    int tailSamples() const;   // the longest tail among the modules running now
    int running() const;       // modules processed in the last chunk (on, or fading out)

private:
    void runModule(int m, const RackPatch& p, const Transport& t, float* L, float* R, int n);
    void resetModule(int m);

    Drive drive_;
    Filter filter_;
    Eq eq_;
    Comp comp_;
    Chorus chorus_;
    Phaser phaser_;
    Pulse pulse_;
    Grain grain_;
    Delay delay_;
    Reverb reverb_;

    bool fresh_ = true;
    Ramp in_, out_, mix_;
    float fade_[RM_COUNT] = {};    // each module's share of its slot: 0 off .. 1 on
    bool dirty_[RM_COUNT] = {};    // processed since its last reset
    int order_[RM_COUNT] = {RM_DRIVE, RM_FILTER, RM_EQ, RM_COMP, RM_CHORUS, RM_PHASER, RM_PULSE, RM_GRAIN, RM_DELAY, RM_REVERB};
    float dip_ = 1.0f;             // the chain's level while the order changes
    int dipDir_ = 0;               // -1 fading out for a new order, +1 fading back in, 0 none
    int running_ = 0;
    float dryL_[kChunk] = {}, dryR_[kChunk] = {}, inL_[kChunk] = {}, inR_[kChunk] = {};
};

} // namespace ef
