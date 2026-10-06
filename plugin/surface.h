#pragma once
// Surface: what every plugin parameter does when MPC sets it, what it reads back, and what
// its value text says. SubForce's plugin/surface.{h,cpp} (PolyForce's, itself RackForce's,
// device-proven), plus the CHAIN page: the order as tiles, the MOVE and ON / OFF buttons.
//
// Threads:
//   UI thread      get / set / display (MPC polls these freely; they read caches)
//   UI thread      refresh(): recompute the plugin-owned values (stepper, tiles) and texts;
//                  called after every set
//   audio thread   notify(): tell MPC what changed underneath it (audioMasterAutomate for
//                  values, audioMasterUpdateDisplay for text) and snapshot() the sound values.
//                  No allocation, no locks.
//
// MPC sends values rounded to 1/1000 as "what it last read back + a delta": a Q-Link detent
// is 1/128 of the range, a data-wheel click 0.01, a touch drag about 0.04, and a tile tap
// sends a release echo ~0.7 s later. So every stepped parameter (choices, small whole
// numbers, the preset stepper) moves exactly one step per event in MPC's direction, and the
// plugin pushes the snapped value back (stepIndex / stepItem below, RackForce's rules). The
// read-back is the base of every value, within a turn too: a stepper measures each detent from
// its own value, never from MPC's previous one. A button tap toggles its read-back, and a button
// always reads back 0 (it springs back): every 1 is a press, and no release ever follows.
#include "library.h"
#include "param_ids.h"
#include "scenes.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace ef {

class Surface {
public:
    using AutomateFn = void (*)(void* ctx, int index, float value);
    using UpdateFn   = void (*)(void* ctx);

    Surface();

    // --- UI thread ---------------------------------------------------------------
    float       get(int i) const;
    void        set(int i, float n);
    std::string display(int i) const;
    bool        automatable(int i) const;

    // State load: values as saved (no stepping), pushed to MPC by the next blocks.
    void        setValue(int i, float n);
    std::string presetKey() const;         // the preset this sound came from ("" = none)
    void        setPresetKey(const std::string& key);
    void        refresh();

    // Many values changed as one (a preset, a project, a move in the chain): the audio thread keeps the
    // previous sound until the batch is complete. Nests; UI thread.
    void        beginBatch();
    void        endBatch();
    struct Batch {
        explicit Batch(Surface& s) : s_(s) { s_.beginBatch(); }
        ~Batch() { s_.endBatch(); }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        Surface& s_;
    };

    // --- audio thread ------------------------------------------------------------
    void        notify(AutomateFn automate, UpdateFn update, void* ctx);
    // Every parameter's current 0..1 value. False (and `out` untouched) while a batch is
    // being written: keep using the previous snapshot.
    bool        snapshot(float* out) const;
    // Moves on every value write: unchanged since a snapshot, the snapshot is still current.
    uint32_t    writes() const { return changes_.load(std::memory_order_acquire); }

    void        seed(uint32_t s) { rng_ = s ? s : 1u; }   // RND's random numbers

    // --- the scenes (docs/DESIGN.md "Performance: scenes and the looper") ------------------
    // EDIT A / B: every page shows the scene picked at that end; what MPC sets while editing is
    // locked in it. UI thread, but for scenes(), which the engine reads.
    Scenes&       scenes() { return scenes_; }
    const Scenes& scenes() const { return scenes_; }
    int         editSide() const { return editSide_; }   // -1 none, 0 A, 1 B
    void        endEdit();                               // the knobs back as they were
    // What the state saves for a parameter: while a scene is edited, the knob, not the scene.
    float       stateValue(int i) const;

    // Each scene's name: the effect of the FX library it was made from, "*" added once it is edited.
    const std::string& sceneName(int scene) const;
    void        setSceneName(int scene, const std::string& name);

    // A scene move as the scenes' line shows it (docs/DESIGN.md "Scene moves"), per end (0 A, 1 B): the
    // plugin publishes the engine's clocks on the audio thread, the line reads them. state: Engine's
    // MoveState; at: the bar (or beat) the move is in, 1-based, of `of`. True if it changed.
    struct MoveShown {
        int state = 0, at = 0, of = 0;
        bool bars = true;
    };
    bool        setMoveShown(int side, const MoveShown& m);

    // The looper's REC presses so far (the engine captures on a change); any thread.
    uint32_t    loopRecs() const { return loopRecs_.load(std::memory_order_acquire); }
    // The texts changed underneath (the looper's line, from the audio thread): MPC redraws them.
    void        textsChanged() { textGen_.fetch_add(1, std::memory_order_release); }

    static int  kFine;   // ranges with this many steps or more follow MPC's value
    // Milliseconds for telling gestures apart (null: the steady clock). Tests set one that only
    // moves when they say, so stepping doesn't depend on how fast the machine is.
    static long long (*clock)();

private:
    struct Category {
        std::string name;
        std::vector<std::string> keys;
    };
    void apply(int i, float n);
    int  stepIndex(int i, float n, int count, int cur);
    int  stepItem(float n, int normRange, int items, int cur);   // one item per event
    bool toggleBounce(int i, bool on);
    void browserAction(int i);
    void chainAction(int i);   // a slot tile, MOVE < / >, ON / OFF
    void sceneAction(int i);   // EDIT A / B, a scene tile, CLEAR
    void fxAction(int i);      // an FX tile (into scene B, or the scene being edited), the bank arrows
    void startEdit(int side);
    void loadEdited();         // the edited scene's locks onto the knobs (the knobs hold the base)
    int  sceneOf(int side) const;
    std::string sceneInfo() const;
    std::string faderBar() const;
    int  selected() const;     // the selected chain slot
    // FAVORITES, RECENT, then the library's categories (from L, the listing in use).
    std::vector<Category> categories(const Listing& L) const;
    void loadPreset(const std::string& key);
    void savePreset();
    int  stepperCur(int i, const Listing& L, const std::string& key) const;   // where a stepper stands

    // UI-thread-only stepping state (RackForce's).
    long long lastSentMs_[P_COUNT] = {};
    float     lastN_[P_COUNT] = {};
    long long toggleMs_[P_COUNT] = {};
    bool      toggleOn_[P_COUNT] = {};

    // What each parameter should read vs what MPC last saw.
    std::atomic<float>    want_[P_COUNT];
    std::atomic<float>    shown_[P_COUNT];
    std::atomic<bool>     release_[P_COUNT];
    std::atomic<uint32_t> textGen_{0};
    std::atomic<uint32_t> batchSeq_{0};    // odd while a batch is being written (a seqlock)
    std::atomic<uint32_t> changes_{0};     // bumped by every write to want_ / shown_ (notify's cue)
    std::atomic<int>      batchDepth_{0};

    // Browser state + text cache (refresh writes, the UI thread reads).
    mutable std::mutex       mtx_;
    std::vector<std::string> texts_;
    size_t                   textHash_ = 0;
    int                      brCat_ = 2;       // index into categories(): starts on the first factory category
    int                      catPage_ = 0, itemPage_ = 0;
    std::string              followed_;        // the preset key the browser last followed
    std::string              presetKey_;
    std::vector<std::string> tileKeys_;        // what each preset tile holds now
    std::vector<int>         catTiles_;        // which category each category tile holds
    uint32_t                 rng_ = 0x2545F491u;

    // The scenes; the edit (UI thread): which end, which scene, the knobs as they were before it.
    Scenes                   scenes_;
    int                      editSide_ = -1, editScene_ = 0;
    float                    editBase_[P_COUNT] = {};
    static constexpr int     kFaderBarWidth = 24;   // the fader's bar: this many steps from A to B
    int                      shownBar_ = 0;         // where it was when MPC last redrew it
    std::atomic<uint32_t>    loopRecs_{0};
    std::string              sceneName_[kNumScenes];
    int                      fxBank_ = 0;           // the FX library's bank on the tiles
    std::atomic<uint32_t>    moveShown_[2] = {};    // MoveShown, packed (state, at, of, bars)
    std::string moveText(int side) const;           // ", BAR 3 OF 8" and the like; "" if nothing moves
    int  moveTarget() const;                        // the scene LENGTH and PLAY set: B's, or the edited one

    // Every write of a value MPC should see goes through here.
    void put(int i, float v) {
        want_[i].store(v, std::memory_order_relaxed);
        changes_.fetch_add(1, std::memory_order_release);
    }

    // Audio-thread state.
    uint32_t scanned_ = 0xffffffffu;   // changes_ at the last complete notify pass
    bool     scanPending_ = true;
    uint32_t textSeen_ = 0;
    int      cursor_ = 0;
    int      sinceText_ = 0;
};

} // namespace ef
