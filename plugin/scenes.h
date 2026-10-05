#pragma once
// Scenes (docs/DESIGN.md, "Performance: scenes and the looper"): eight sets of parameter locks, a
// scene at each end of the crossfader, and how a locked parameter moves between the two ends.
//
//   Line     a + (b - a) x in the knob's 0..1 space: continuous knobs follow their own curve, and an
//            ordered choice (a sync division, the loop's length) walks its steps in order
//   Switch   a below the middle, b from it (types, modes, sync, freeze, hold, matrix routing)
//   Send     a module's On: the module runs while either end has it on, and the fader moves its
//            input level from 0 at the Off end to 1 at the On end (dsp/rack.h)
//
// Threads: the surface writes locks on MPC's UI thread, the engine reads them on the audio thread.
// Every value is an atomic (0..1, or kNone where nothing is locked); generation() moves on every
// change (and when an edit starts or ends), so the engine rebuilds its list of moving parameters
// only then. While a scene is edited the engine morphs nothing: the knobs already show the scene.
#include "param_ids.h"

#include <atomic>
#include <cstdint>

namespace ef {

class Scenes {
public:
    static constexpr float kNone = -1.0f;

    Scenes();

    // --- UI thread -----------------------------------------------------------------------------
    void lock(int scene, int param, float norm);   // only lockable parameters
    void unlock(int scene, int param);
    void clear(int scene);
    void clearAll();
    void setEditing(bool on);

    // --- any thread ----------------------------------------------------------------------------
    float value(int scene, int param) const;   // kNone where the scene doesn't lock it
    bool locked(int scene, int param) const { return value(scene, param) >= 0.0f; }
    int count(int scene) const;                 // how many parameters the scene locks
    bool editing() const { return editing_.load(std::memory_order_acquire); }
    uint32_t generation() const { return gen_.load(std::memory_order_acquire); }

    static bool lockable(int param) {
        return param >= 0 && param < P_COUNT && kSceneMorph[param] != SceneMorph::None;
    }
    // The 0..1 value a Line or Switch parameter plays at fader position x (0 = A, 1 = B), from its
    // value at each end (its lock there, or the knob).
    static float morph(int param, float a, float b, float x);
    // A module's send at x from its On at each end (0..1 values, On from 0.5).
    static float send(float aOn, float bOn, float x);

private:
    void changed() { gen_.fetch_add(1, std::memory_order_acq_rel); }

    std::atomic<float> v_[kNumScenes][P_COUNT];
    std::atomic<uint32_t> gen_{0};
    std::atomic<bool> editing_{false};
};

} // namespace ef
