#pragma once
// The rack plus its modulation: two LFOs, the envelope follower, the macros and the 8-slot matrix
// (docs/DESIGN.md "Modulation"). Modulation adds amount x source to a target's 0..1 value, so it
// follows the knob's own curve, and is worked out once per 32-sample chunk from the sources of the
// chunk before (0.7 ms late: nothing hears it). Audio thread only, after construction (but for
// armLooper()).
//
// The scenes (docs/DESIGN.md "Performance: scenes and the looper") come before the modulation: the
// parameters scene A or B locks move with the crossfader, which the engine follows with a 15 ms
// one-pole per chunk; modulation then adds to the moved value. Only the locked parameters are worked
// out per chunk; the list of them is rebuilt when a parameter or a lock changes.
//
// Scene moves (docs/DESIGN.md "Scene moves"): each end of the fader has a clock. A scene's move runs
// while the scene is heard and starts over once it is fully out: the fader leaving A arms B's, which
// starts on the next bar line (on the next beat for a move under a bar) and counts beats at the tempo
// from there. An end plays start + (lock - start) x where its move is; the fader blends the ends as
// before.
#include "param_ids.h"
#include "rack_map.h"
#include "scenes.h"

namespace ef {

class Engine {
public:
    Engine();   // allocates the rack (UI thread)

    void setParams(const float* norm);   // a new snapshot of every parameter (0..1)
    void attachScenes(const Scenes* s);  // the scenes' locks (null: none); before the first render
    bool armLooper();                    // UI thread: the looper's buffers, once (Looper::allocate)
    void setLoopRec(uint32_t presses);   // REC presses so far (the surface counts them); a change captures
    void reset();                        // clears every tail and modulation source
    void seed(uint32_t s);               // the LFOs' random values (S&H, Smooth)
    // In place, any n: chunks of kChunk with the transport advanced for each.
    void render(float* L, float* R, int n, Transport t);

    int tailSamples() const { return rack_.tailSamples(); }
    int running() const { return rack_.running(); }
    const RackPatch& patch() const { return base_; }                          // the knobs as they are
    const RackPatch& current() const { return nSlots_ > 0 ? patch_ : base_; } // as the last chunk played them
    float source(int s) const { return s >= 0 && s < MS_COUNT ? src_[s] : 0.0f; }
    float fader() const { return x_; }              // the crossfader as the engine plays it (smoothed)
    int movingParams() const { return nLocks_; }    // parameters the scenes move
    const Looper& looper() const { return rack_.looper(); }

    // A scene move at one end (0 A, 1 B), for the scenes' line.
    struct MoveStatus {
        int state = kMoveNone;   // MoveState
        double beats = 0.0;      // running: beats since it started; armed: beats until it starts
        double length = 0.0;     // its LENGTH in beats
        int play = kMoveOnce;
    };
    MoveStatus moveStatus(int side) const;

private:
    void modulate();   // patch_ = base_ moved by the matrix (routes() copies base_, this moves its targets)
    void rebuild();    // norm_, base_ and the scenes' list from raw_
    void morph(bool fast = false);   // the scenes' parameters at x_ and the moves into norm_, base_ and patch_
    void routes();     // the matrix's slots and the envelope follower's settings from norm_ / base_; patch_ = base_
    bool tick(double grid, double beats);   // the move clocks for a chunk of `beats` from grid beat `grid`; true: a move went on

    Rack rack_;
    Lfo lfo_[2];
    EnvFollower env_;
    RackPatch base_, patch_;
    float raw_[P_COUNT] = {};    // the knobs as MPC set them
    float norm_[P_COUNT] = {};   // the knobs moved by the scenes (what the modulation adds to)
    bool haveRaw_ = false;

    // The scenes: each parameter A or B locks, its 0..1 value at each end, and where each end's move
    // starts it (a0 = a, b0 = b: that end holds it still).
    struct Lock {
        int param, module;   // module: the rack module a Send switches
        float a, b;
        float a0, b0;
        float logRatio;      // a log-curve parameter: log2(hi / lo), for the fast exp2 while it moves; else 0
        SceneMorph kind;
    };
    const Scenes* scenes_ = nullptr;
    uint32_t sceneGen_ = 0;
    Lock locks_[P_COUNT] = {};
    int nLocks_ = 0;
    float x_ = 0.0f, xTarget_ = 0.0f;
    bool xFresh_ = true;
    // Each end's move clock.
    struct Clock {
        int state = kMoveNone;
        int scene = -1;          // the scene it is the clock of
        uint32_t epoch = 0;      // that scene's moveEpoch() when it was set up
        double length = 0.0;     // beats
        int play = kMoveOnce;
        double wait = 0.0;       // armed: beats until it starts
        double beats = 0.0;      // running: beats since it started
        float at = 0.0f;         // where the end plays: 0 its starts .. 1 its locks
    };
    Clock clock_[2];
    double freeBeat_ = 0.0;      // the beat grid while MPC is stopped: it runs on at the tempo
    uint32_t loopRec_ = 0;
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
