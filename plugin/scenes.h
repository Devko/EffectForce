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
// A move (docs/DESIGN.md "Scene moves"): a Line lock may have a start value too; the scene then plays
// from the start to the lock over its LENGTH (kMoveBeats), Once (holding the lock), Loop or
// Ping-pong, from the first bar line after the fader brings the scene in (the engine's clocks).
//
// Threads: the surface writes locks on MPC's UI thread, the engine reads them on the audio thread.
// Every value is an atomic (0..1, or kNone where nothing is locked); generation() moves on every
// change (and when an edit starts or ends), so the engine rebuilds its list of moving parameters
// only then. While a scene is edited the engine morphs nothing: the knobs already show the scene.
// moveEpoch() moves when a scene's move changes as a whole (a start set or cleared, its timing, a
// clear): the engine starts that scene's move again on the next bar line.
#include "param_ids.h"

#include <atomic>
#include <cstdint>

namespace ef {

// A move's LENGTH in beats, mv_len's options in order (surface.py MOVE_LENGTHS); 0: Off, the scene
// holds its locks (no move).
inline constexpr double kMoveBeats[] = {0.0, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0};
static_assert(sizeof kMoveBeats / sizeof kMoveBeats[0] == kNumMoveLengths, "kMoveBeats must match surface.py MOVE_LENGTHS");
enum MovePlay : int { kMoveOnce, kMoveLoop, kMovePingPong };   // mv_play's options (surface.py MOVE_PLAYS)
static_assert(kMovePingPong + 1 == kNumMovePlays, "MovePlay must match surface.py MOVE_PLAYS");
// Where a move is at one end of the fader (plugin/engine.h's clocks): None, that end's scene has no move
// (or its LENGTH is Off); Idle, out of the mix; Armed, waiting for the grid's next line; Running; Done, a
// Once move holding its locks.
enum MoveState : int { kMoveNone, kMoveIdle, kMoveArmed, kMoveRunning, kMoveDone };

class Scenes {
public:
    static constexpr float kNone = -1.0f;
    static constexpr int kDefaultLength = 5;   // 4 bars (mv_len's default)

    Scenes();

    // --- UI thread -----------------------------------------------------------------------------
    void lock(int scene, int param, float norm);   // only lockable parameters; a plain lock: no start
    void unlock(int scene, int param);
    void clear(int scene);                         // its locks, starts and timing
    void clearAll();
    void setEditing(bool on);
    // A lock's start (a Line parameter the scene locks; kNone: it doesn't move), and a scene's timing.
    void setStart(int scene, int param, float norm);
    void setMove(int scene, int length, int play);

    // --- any thread ----------------------------------------------------------------------------
    float value(int scene, int param) const;   // kNone where the scene doesn't lock it
    bool locked(int scene, int param) const { return value(scene, param) >= 0.0f; }
    float start(int scene, int param) const;   // kNone where the lock doesn't move
    bool moves(int scene) const;               // a lock with a start (whatever the length)
    int moveLength(int scene) const;           // index into kMoveBeats
    int movePlay(int scene) const;             // MovePlay
    int count(int scene) const;                 // how many parameters the scene locks
    bool editing() const { return editing_.load(std::memory_order_acquire); }
    uint32_t generation() const { return gen_.load(std::memory_order_acquire); }
    uint32_t moveEpoch(int scene) const;

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
    void restart(int scene) { epoch_[scene].fetch_add(1, std::memory_order_relaxed); }

    std::atomic<float> v_[kNumScenes][P_COUNT];
    std::atomic<float> from_[kNumScenes][P_COUNT];   // the starts
    std::atomic<int> len_[kNumScenes], play_[kNumScenes];
    std::atomic<uint32_t> epoch_[kNumScenes];
    std::atomic<uint32_t> gen_{0};
    std::atomic<bool> editing_{false};
};

} // namespace ef
