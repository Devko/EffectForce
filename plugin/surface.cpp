#include "surface.h"

#include "fx_library.h"
#include "rack_map.h"
#include "presets.h"
#include "state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

namespace ef {

int Surface::kFine = 48;
long long (*Surface::clock)() = nullptr;

namespace {

constexpr int kMaxAutomatePerBlock = 48;   // spread big refreshes over a few blocks
constexpr int kTextEveryBlocks     = 4;    // at most one UpdateDisplay per ~12 ms
constexpr float kQuant             = 0.0015f;   // MPC rounds values to 1/1000
constexpr long long kGestureMs     = 300;   // sends closer than this belong to one gesture
constexpr float kFirstMoveMax      = 0.16f; // a gesture's first event is a turn, not a jump

// A stepper's 0..1 range: one item per 1/1023, or per 1/(items-1) for longer lists.
int stepperRange(int items) { return std::max(kStepperRange, items - 1); }

long long steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
long long nowMs() { return Surface::clock ? Surface::clock() : steadyMs(); }

// NaN from the host becomes 0: kept, it would reach the engine's smoothers and never leave.
float clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int stepsOf(int i) {   // whole steps of a stepped parameter, 0 = continuous
    const ParamSpec& s = PARAM_SPECS[i];
    if (s.curve == Curve::Enum) return PARAM_INFO[i].nopts - 1;
    if (s.curve == Curve::Int) return static_cast<int>(std::lround(s.hi - s.lo));
    return 0;
}

bool exactOption(float n, int count) {   // a tap on an option (allowing MPC's 1/1000 rounding)
    if (count < 1) return true;
    return std::fabs(n - std::round(n * count) / count) <= kQuant;
}

int popupFlagOf(int param) {
    for (int j = 0; j < P_COUNT; ++j)
        if (PARAM_INFO[j].popupOf == param) return j;
    return -1;
}

const int kCatTiles[] = {
#define T(n) P_CAT_##n
    T(1), T(2), T(3), T(4), T(5), T(6), T(7), T(8), T(9), T(10), T(11), T(12), T(13), T(14), T(15), T(16)
#undef T
};
const int kItemTiles[] = {
#define T(n) P_ITEM_##n
    T(1), T(2), T(3), T(4), T(5), T(6), T(7), T(8), T(9), T(10), T(11), T(12),
    T(13), T(14), T(15), T(16), T(17), T(18), T(19), T(20), T(21), T(22), T(23), T(24)
#undef T
};
static_assert(sizeof kCatTiles / sizeof kCatTiles[0] == kBrowserCats, "category tiles");
static_assert(sizeof kItemTiles / sizeof kItemTiles[0] == kBrowserItems, "item tiles");

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool isModuleOn(int i) {
    for (int m = 0; m < kNumModules; ++m)
        if (i == kModuleOnParam[m]) return true;
    return false;
}

int optionOf(float n, int i) {
    return clampi(static_cast<int>(std::lround(n * static_cast<float>(PARAM_INFO[i].nopts - 1))), 0, PARAM_INFO[i].nopts - 1);
}

} // namespace

Surface::Surface() : texts_(P_COUNT) {
    for (int i = 0; i < P_COUNT; ++i) {
        want_[i].store(PARAM_INFO[i].def);
        shown_[i].store(PARAM_INFO[i].def);
        release_[i].store(false);
        lastN_[i] = -1.0f;
    }
    presetLibrary().rescan();   // a new instance sees the preset files as they are now
    refresh();
}

// --- UI thread ----------------------------------------------------------------------------

float Surface::get(int i) const { return i >= 0 && i < P_COUNT ? want_[i].load(std::memory_order_relaxed) : 0.0f; }

bool Surface::automatable(int i) const { return i >= 0 && i < P_COUNT && PARAM_INFO[i].kind == Kind::Synth; }

void Surface::set(int i, float n) {
    if (i < 0 || i >= P_COUNT) return;
    const Kind k = PARAM_INFO[i].kind;
    if (k == Kind::Readout) return;   // MPC sets param 0 right after loading: ignore
    if (k == Kind::Chain) return;     // only MOVE moves a module (MPC has no control for the order)
    n = clamp01(n);
    auto shown = [this, i, n] {   // what MPC shows now
        shown_[i].store(n, std::memory_order_relaxed);
        changes_.fetch_add(1, std::memory_order_release);
    };
    if (k == Kind::Button) {
        // A tap toggles the value MPC last read back, and a button always reads back 0 (it springs
        // back), so every tap arrives as a 1 with no release before the next: each 1 is a press.
        // (Waiting for a release, as RackForce's rising-edge rule did, left a button dead after its
        // first press on the Force: PolyForce's first device run.) A 0, a release or our own
        // spring-back, does nothing.
        if (n > 0.5f) {
            release_[i] = true;
            changes_.fetch_add(1, std::memory_order_release);   // after the flag: notify must see it
            apply(i, n);
            refresh();
        }
        return;
    }
    // Act first, then record MPC's value: recorded before, the audio thread could push the old
    // value back while a preset loads (a tile would flicker, MPC's next delta start from it).
    apply(i, n);
    shown();
    // A sound parameter's text is computed when MPC asks; a module's on / off also lights its chain tile.
    bool texts = k != Kind::Synth || isModuleOn(i);
    if (editSide_ >= 0 && Scenes::lockable(i)) {   // editing a scene: what MPC sets is locked in it
        scenes_.lock(editScene_, i, want_[i].load());
        std::string& name = sceneName_[editScene_];
        if (!name.empty() && name.back() != '*') name += '*';   // no longer the effect as it came
        texts = true;
    }
    if (i == P_SCENE_A || i == P_SCENE_B) {   // another scene at an end (a Q-Link): the edit follows it
        if (editSide_ == (i == P_SCENE_A ? 0 : 1) && sceneOf(editSide_) != editScene_) loadEdited();
        texts = true;
    }
    if (i == P_XFADE) {   // the fader's bar: MPC redraws texts only when told, so tell it when the bar moves
        const int bar = static_cast<int>(std::lround(want_[P_XFADE].load() * kFaderBarWidth));
        if (bar != shownBar_) {
            shownBar_ = bar;
            textGen_.fetch_add(1, std::memory_order_release);
        }
    }
    if (texts) refresh();
}

void Surface::beginBatch() {
    if (batchDepth_.fetch_add(1) == 0) {
        batchSeq_.fetch_add(1, std::memory_order_relaxed);   // odd: writing
        std::atomic_thread_fence(std::memory_order_release);
    }
}

void Surface::endBatch() {
    if (batchDepth_.fetch_sub(1) == 1) {
        batchSeq_.fetch_add(1, std::memory_order_release);   // even: done
        // Many values at once (a preset, a move in the chain): MPC only re-reads texts when told.
        textGen_.fetch_add(1, std::memory_order_release);
    }
}

int Surface::stepperCur(int i, const Listing& L, const std::string& key) const {
    const int items = static_cast<int>(L.items.size());
    const int at = L.find(key);
    if (at >= 0) return at;
    // Not listed (a deleted or renamed file): where the stepper stands now, so a turn
    // moves from there instead of jumping to the first item.
    return clampi(static_cast<int>(std::lround(want_[i].load() * stepperRange(items))), 0, std::max(items - 1, 0));
}

void Surface::apply(int i, float n) {
    const ParamInfo& info = PARAM_INFO[i];
    switch (info.kind) {
        case Kind::Synth:
        case Kind::Ui: {
            const int steps = stepsOf(i);
            if (steps > 0 && steps < kFine) {
                const int cur = static_cast<int>(std::lround(want_[i].load() * steps));
                const int pick = stepIndex(i, n, steps, cur);
                put(i, static_cast<float>(pick) / static_cast<float>(steps));
                if (exactOption(n, steps)) {   // a tap on a list row closes its popup (a Q-Link nudge doesn't)
                    const int flag = popupFlagOf(i);
                    if (flag >= 0) put(flag, 0.0f);
                }
                if (info.kind == Kind::Ui && pick != cur)   // another page: an open list belongs to the old one
                    for (int j = 0; j < P_COUNT; ++j)
                        if (PARAM_INFO[j].kind == Kind::Popup) put(j, 0.0f);
            } else {
                put(i, n);
            }
            break;
        }
        case Kind::Popup: put(i, n > 0.5f ? 1.0f : 0.0f); break;
        case Kind::Chain: break;
        case Kind::Stepper: {
            if (i == P_PRESET) {
                const auto L = presetLibrary().listing();
                const int items = static_cast<int>(L->items.size());
                const int cur = stepperCur(i, *L, presetKey());
                const int pick = stepItem(n, stepperRange(items), items, cur);
                if (pick != cur && pick < items) loadPreset(L->items[static_cast<size_t>(pick)].key);
            }
            break;
        }
        case Kind::Button:
            if (i == P_PRESET_PREV || i == P_PRESET_NEXT) {   // from where the stepper stands, like a turn
                const auto L = presetLibrary().listing();
                const int items = static_cast<int>(L->items.size());
                const int cur = stepperCur(P_PRESET, *L, presetKey());
                const int pick = clampi(cur + (i == P_PRESET_NEXT ? 1 : -1), 0, std::max(items - 1, 0));
                if (pick != cur) loadPreset(L->items[static_cast<size_t>(pick)].key);   // the ends: nothing to load
            }
            if (i == P_PRE_INIT) loadPreset("builtin:Init");
            if (i == P_PRE_SAVE) savePreset();
            if (i == P_CAT_PREV || i == P_CAT_NEXT || i == P_ITEM_PREV || i == P_ITEM_NEXT || i == P_RND) browserAction(i);
            if (i == P_MOVE_L || i == P_MOVE_R || i == P_SEL_ON) chainAction(i);
            if (i == P_SCN_CLEAR) sceneAction(i);
            if (i == P_FX_PREV || i == P_FX_NEXT) fxAction(i);
            if (i == P_LP_REC) {   // a loop to keep: Hold on, and the engine captures on the count's change
                put(P_LP_HOLD, 1.0f);
                loopRecs_.fetch_add(1, std::memory_order_acq_rel);
            }
            break;
        case Kind::Tile:
        case Kind::Toggle: {
            const bool on = n > 0.5f;
            const bool lit = want_[i].load() > 0.5f;
            if (on == lit || toggleBounce(i, on)) break;   // nothing new, or the release echo
            if (i >= P_SLOT_1 && i < P_SLOT_1 + kNumModules) chainAction(i);
            else if (i == P_EDIT_A || i == P_EDIT_B || (i >= P_SCA_1 && i < P_SCA_1 + kNumScenes) ||
                     (i >= P_SCB_1 && i < P_SCB_1 + kNumScenes))
                sceneAction(i);
            else if (i >= P_FX_1 && i < P_FX_1 + kFxPerBank) fxAction(i);
            else browserAction(i);
            break;
        }
        case Kind::Readout: break;
    }
}

int Surface::stepIndex(int i, float n, int count, int cur) {
    if (count < 1) return 0;
    cur = clampi(cur, 0, count);
    const long long now = nowMs();
    const bool gesture = lastSentMs_[i] > 0 && now - lastSentMs_[i] < kGestureMs && lastN_[i] >= 0.0f;
    const float mpcPrev = lastN_[i];   // MPC's own previous value (never our pushes)
    lastSentMs_[i] = now;
    lastN_[i] = n;
    const float ours = static_cast<float>(cur) / count;
    const float r = std::round(n * count);

    if (count >= kFine) {
        // Follow MPC's value; a gesture that starts far from ours means MPC's idea of the
        // value was stale (a bump, not a jump: move at most kFirstMoveMax of the range).
        if (!gesture && std::fabs(n - ours) > kFirstMoveMax)
            return clampi(static_cast<int>(std::lround((ours + (n > ours ? kFirstMoveMax : -kFirstMoveMax)) * count)),
                          0, count);
        // A single slow detent (1/128) can be under half a step: it still moves one.
        if (!gesture && static_cast<int>(r) == cur && std::fabs(n - ours) > kQuant)
            return clampi(cur + (n > ours ? 1 : -1), 0, count);
        return clampi(static_cast<int>(r), 0, count);
    }
    // Coarse: a value that lands on a step is a tap (or our own value back); a toggle's
    // exact 0/1 is always a tap.
    const bool onStep = std::fabs(n - r / count) <= kQuant;
    if (onStep && (count == 1 || !gesture)) return clampi(static_cast<int>(r), 0, count);
    float delta = n - (gesture ? mpcPrev : ours);
    if (std::fabs(delta) <= kQuant) return cur;
    if (!gesture && std::fabs(delta) > kFirstMoveMax)   // MPC's idea of the value was stale
        delta = delta > 0 ? kFirstMoveMax : -kFirstMoveMax;
    const float steps = delta * count;
    int move = static_cast<int>(std::lround(steps));
    if (move == 0) move = steps > 0 ? 1 : -1;   // every detent moves at least one step
    return clampi(cur + move, 0, count);
}

int Surface::stepItem(float n, int normRange, int items, int cur) {
    if (items < 1 || normRange < 1) return 0;
    cur = clampi(cur, 0, items - 1);
    // The Force sends the value it last read back (ours) plus its step, within one turn too
    // (sd88me/mpc-vst-plugins docs/NOTES.md, "Input probe"). So the direction is n against ours.
    // Against MPC's previous value, each detent after the first differed by the item the last one
    // moved (1/1023, under kQuant), and a turn stalled after one item (PolyForce's device run).
    const float delta = n - static_cast<float>(cur) / normRange;
    if (std::fabs(delta) <= kQuant) return cur;   // our own value back
    // One item per event, whatever the size of MPC's step (Q-Link detent 1/128, wheel click
    // 0.01, a drag ~0.04, a fast spin 1-3 detents): a long list must never jump.
    return clampi(cur + (delta > 0 ? 1 : -1), 0, items - 1);
}

bool Surface::toggleBounce(int i, bool on) {
    const long long now = nowMs();
    if (toggleMs_[i] > 0 && now - toggleMs_[i] < 1000 && on != toggleOn_[i]) return true;   // the release echo
    toggleMs_[i] = now;
    toggleOn_[i] = on;
    return false;
}

std::vector<Surface::Category> Surface::categories(const Listing& L) const {
    FileLibrary& lib = presetLibrary();
    std::vector<Category> out;
    std::vector<std::string> fav, rec;
    for (const std::string& k : lib.favorites()) if (L.find(k) >= 0) fav.push_back(k);
    for (const std::string& k : lib.recent()) if (L.find(k) >= 0) rec.push_back(k);
    out.push_back({"FAVORITES", std::move(fav)});
    out.push_back({"RECENT", std::move(rec)});
    for (size_t c = 0; c < L.categories.size(); ++c) {
        Category cat{L.categories[c], {}};
        for (int m : L.members[c]) cat.keys.push_back(L.items[static_cast<size_t>(m)].key);
        out.push_back(std::move(cat));
    }
    return out;
}

void Surface::browserAction(int i) {
    // Browsing looks at the folders again: presets copied (or an SSD plugged in) since show up.
    for (int t = 0; t < kBrowserCats; ++t)
        if (i == kCatTiles[t]) presetLibrary().rescan();
    if (i == P_CAT_PREV || i == P_CAT_NEXT || i == P_ITEM_PREV || i == P_ITEM_NEXT) presetLibrary().rescan();
    std::string load;   // a preset to load: done after the lock (loading refreshes the surface)
    std::string fav;    // a favorite to toggle: also after the lock (it writes a file)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        FileLibrary& lib = presetLibrary();
        const auto L = lib.listing();
        const std::vector<Category> cats = categories(*L);
        const int ncat = static_cast<int>(cats.size());
        const std::string cur = presetKey_;
        for (int t = 0; t < kBrowserCats; ++t)
            if (i == kCatTiles[t] && t < static_cast<int>(catTiles_.size()) && catTiles_[static_cast<size_t>(t)] >= 0) {
                brCat_ = catTiles_[static_cast<size_t>(t)];
                itemPage_ = 0;
                followed_ = cur;   // the next refresh must not jump back to its category
            }
        for (int t = 0; t < kBrowserItems; ++t)
            if (i == kItemTiles[t] && t < static_cast<int>(tileKeys_.size()) && !tileKeys_[static_cast<size_t>(t)].empty()) {
                load = tileKeys_[static_cast<size_t>(t)];
                followed_ = load;   // picked here: stay on this category and page
            }
        const int catPages = std::max(1, (ncat + kBrowserCats - 1) / kBrowserCats);
        if (i == P_CAT_PREV) catPage_ = clampi(catPage_ - 1, 0, catPages - 1);
        if (i == P_CAT_NEXT) catPage_ = clampi(catPage_ + 1, 0, catPages - 1);
        const int nitems = brCat_ < ncat ? static_cast<int>(cats[static_cast<size_t>(brCat_)].keys.size()) : 0;
        const int itemPages = std::max(1, (nitems + kBrowserItems - 1) / kBrowserItems);
        if (i == P_ITEM_PREV) itemPage_ = clampi(itemPage_ - 1, 0, itemPages - 1);
        if (i == P_ITEM_NEXT) itemPage_ = clampi(itemPage_ + 1, 0, itemPages - 1);

        if (i == P_FAV) fav = cur;
        if (i == P_RND && brCat_ < ncat) {   // a random preset of this category, never the current one
            std::vector<std::string> pool;
            for (const std::string& k : cats[static_cast<size_t>(brCat_)].keys)
                if (k != cur) pool.push_back(k);
            if (!pool.empty()) {
                rng_ ^= rng_ << 13;
                rng_ ^= rng_ >> 17;
                rng_ ^= rng_ << 5;
                load = pool[rng_ % pool.size()];
                followed_ = load;
            }
        }
    }
    if (!fav.empty()) presetLibrary().setFavorite(fav, !presetLibrary().isFavorite(fav));
    if (!load.empty()) loadPreset(load);
}

// --- the chain ----------------------------------------------------------------------------

int Surface::selected() const {   // a number (0..7), not an option
    return clampi(static_cast<int>(paramValue(P_CHAIN_SEL, want_[P_CHAIN_SEL].load())), 0, kNumModules - 1);
}

void Surface::chainAction(int i) {
    const int sel = selected();
    const float span = static_cast<float>(kNumModules - 1);
    for (int k = 0; k < kNumModules; ++k)
        if (i == P_SLOT_1 + k) put(P_CHAIN_SEL, static_cast<float>(k) / span);   // a tap selects
    if (i == P_MOVE_L || i == P_MOVE_R) {
        const int to = sel + (i == P_MOVE_R ? 1 : -1);
        if (to >= 0 && to < kNumModules) {
            Batch batch(*this);   // both slots at once: the audio thread never sees a module twice
            const float a = want_[P_ORDER_1 + sel].load(), b = want_[P_ORDER_1 + to].load();
            put(P_ORDER_1 + sel, b);
            put(P_ORDER_1 + to, a);
            put(P_CHAIN_SEL, static_cast<float>(to) / span);   // the selection moves with the module
        }
    }
    if (i == P_SEL_ON) {
        const int on = kModuleOnParam[optionOf(want_[P_ORDER_1 + sel].load(), P_ORDER_1 + sel)];
        put(on, want_[on].load() > 0.5f ? 0.0f : 1.0f);
    }
}

// --- the scenes -----------------------------------------------------------------------------

static_assert(P_SCA_8 - P_SCA_1 == kNumScenes - 1 && P_SCB_8 - P_SCB_1 == kNumScenes - 1,
              "the scene tiles must be in order");

int Surface::sceneOf(int side) const {
    const int id = side ? P_SCENE_B : P_SCENE_A;
    return clampi(static_cast<int>(paramValue(id, want_[id].load())), 0, kNumScenes - 1);
}

float Surface::stateValue(int i) const {
    if (i < 0 || i >= P_COUNT) return 0.0f;
    return editSide_ >= 0 && Scenes::lockable(i) ? editBase_[i] : want_[i].load();
}

void Surface::startEdit(int side) {
    scenes_.setEditing(true);   // first: the engine stops moving parameters before the knobs change
    {
        Batch batch(*this);
        for (int p = 0; p < P_COUNT; ++p) {
            if (!Scenes::lockable(p)) continue;
            if (editSide_ < 0) editBase_[p] = want_[p].load();   // from the knobs
            else put(p, editBase_[p]);                           // from the other end's scene: the knobs first
        }
        editSide_ = side;
        editScene_ = sceneOf(side);
        for (int p = 0; p < P_COUNT; ++p) {
            const float v = scenes_.value(editScene_, p);
            if (v >= 0.0f) put(p, v);
        }
    }
    refresh();
}

void Surface::loadEdited() {
    Batch batch(*this);
    for (int p = 0; p < P_COUNT; ++p)
        if (Scenes::lockable(p)) put(p, editBase_[p]);
    editScene_ = sceneOf(editSide_);
    for (int p = 0; p < P_COUNT; ++p) {
        const float v = scenes_.value(editScene_, p);
        if (v >= 0.0f) put(p, v);
    }
}

void Surface::endEdit() {
    if (editSide_ < 0) return;
    {
        Batch batch(*this);
        for (int p = 0; p < P_COUNT; ++p)
            if (Scenes::lockable(p)) put(p, editBase_[p]);
        editSide_ = -1;
    }
    scenes_.setEditing(false);   // last: the knobs are back before the engine moves parameters again
    refresh();
}

void Surface::sceneAction(int i) {
    if (i == P_EDIT_A || i == P_EDIT_B) {
        const int side = i == P_EDIT_A ? 0 : 1;
        if (editSide_ == side) endEdit();
        else startEdit(side);
        return;
    }
    for (int side = 0; side < 2; ++side)
        for (int k = 0; k < kNumScenes; ++k)
            if (i == (side ? P_SCB_1 : P_SCA_1) + k) {   // a tap picks the scene at that end
                put(side ? P_SCENE_B : P_SCENE_A, static_cast<float>(k) / static_cast<float>(kNumScenes - 1));
                if (editSide_ == side) loadEdited();
                return;
            }
    if (i == P_SCN_CLEAR && editSide_ >= 0) {   // the edited scene's locks go; its knobs show the base
        Batch batch(*this);
        for (int p = 0; p < P_COUNT; ++p)
            if (scenes_.locked(editScene_, p)) put(p, editBase_[p]);
        scenes_.clear(editScene_);
        sceneName_[editScene_].clear();
    }
}

const std::string& Surface::sceneName(int scene) const {
    static const std::string none;
    return scene >= 0 && scene < kNumScenes ? sceneName_[scene] : none;
}

void Surface::setSceneName(int scene, const std::string& name) {
    if (scene >= 0 && scene < kNumScenes) sceneName_[scene] = name.substr(0, 32);
}

static_assert(P_FX_16 - P_FX_1 == kFxPerBank - 1, "the FX tiles must be in order, one per effect of a bank");

namespace {
int fxBanks() {
    int banks = 0;
    for (int k = 0; k < kNumFx; ++k)
        if (k == 0 || std::strcmp(kFxLibrary[k].bank, kFxLibrary[k - 1].bank) != 0) ++banks;
    return banks;
}
// The library's effect at tile `tile` of bank `bank`, or -1.
int fxAt(int bank, int tile) {
    int b = -1, t = 0;
    for (int k = 0; k < kNumFx; ++k) {
        if (k == 0 || std::strcmp(kFxLibrary[k].bank, kFxLibrary[k - 1].bank) != 0) {
            ++b;
            t = 0;
        }
        if (b == bank && t == tile) return k;
        ++t;
    }
    return -1;
}
}

void Surface::fxAction(int i) {
    const int banks = fxBanks();
    if (i == P_FX_PREV || i == P_FX_NEXT) {   // the banks go round
        if (banks > 0) fxBank_ = (fxBank_ + (i == P_FX_NEXT ? 1 : banks - 1)) % banks;
        return;
    }
    const int k = fxAt(fxBank_, i - P_FX_1);
    if (k < 0) return;
    // Into the scene being edited, or else the one at the fader's B end.
    const int target = editSide_ >= 0 ? editScene_ : sceneOf(1);
    bool looper = false;
    sceneName_[target] = loadSceneText(scenes_, target, kFxLibrary[k].text, &looper);
    if (looper) put(P_LP_ON, 1.0f);   // an effect of the looper arms it (the plugin makes its buffers)
    if (editSide_ >= 0) loadEdited();   // the knobs show the scene as it is now
}

std::string Surface::sceneInfo() const {
    char b[96];
    if (editSide_ >= 0) {
        const std::string name = sceneName_[editScene_].empty() ? "" : " " + upper(sceneName_[editScene_]);
        std::snprintf(b, sizeof b, "EDIT %c: SCENE %d%s, %d LOCKS. WHAT YOU MOVE IS LOCKED", 'A' + editSide_,
                      editScene_ + 1, name.c_str(), scenes_.count(editScene_));
    } else {
        const int a = sceneOf(0), c = sceneOf(1);
        // A scene by its name (the effect it came from), or by how many settings it locks.
        const auto said = [this](int sc) {
            std::string t = "SCENE " + std::to_string(sc + 1);
            if (!sceneName_[sc].empty()) return t + " " + upper(sceneName_[sc]);
            const int n = scenes_.count(sc);
            return n ? t + ", " + std::to_string(n) + " LOCKS" : t + ", CLEAN";
        };
        return "A: " + said(a) + "     B: " + said(c);
    }
    return b;
}

std::string Surface::faderBar() const {
    const int at = clampi(static_cast<int>(std::lround(want_[P_XFADE].load() * kFaderBarWidth)), 0, kFaderBarWidth);
    std::string s = "A  ";
    s.append(static_cast<size_t>(at), '=');
    s += '|';
    s.append(static_cast<size_t>(kFaderBarWidth - at), '-');
    return s + "  B";
}

// --- presets ------------------------------------------------------------------------------

void Surface::loadPreset(const std::string& key) {
    std::string text;
    if (!presetText(key, text)) {   // renamed or deleted since the listing: list again, so steps go past it
        presetLibrary().rescan();
        refresh();
        return;
    }
    // The key first: the state's own refresh then sees the new preset, and a browser that
    // picked it (from FAVORITES, say) stays where it is instead of following the old one.
    const std::string old = presetKey();
    setPresetKey(key);
    if (!loadState(*this, text, true)) {
        setPresetKey(old);
        refresh();
        return;
    }
    presetLibrary().touchRecent(key);
    refresh();
}

void Surface::savePreset() {
    std::string key;
    const std::string path = nextUserPreset(&key);
    if (path.empty()) return;
    if (!writeFileAtomic(path, saveState(*this, true))) {
        std::remove(path.c_str());
        return;
    }
    presetLibrary().rescan();
    setPresetKey(key);
    refresh();
}

std::string Surface::display(int i) const {
    if (i < 0 || i >= P_COUNT) return {};
    if (i == P_XF_BAR) return faderBar();   // follows the fader without a refresh
    switch (PARAM_INFO[i].kind) {
        case Kind::Synth:
        case Kind::Chain:
        case Kind::Ui: {
            if (PARAM_SPECS[i].fmt != Fmt::Center) return paramDisplay(i, want_[i].load());
            float ctx[P_COUNT] = {};   // the phaser's centre reads its mode
            ctx[P_PHS_MODE] = want_[P_PHS_MODE].load();
            return paramDisplay(i, want_[i].load(), ctx);
        }
        case Kind::Stepper:
        case Kind::Tile:
        case Kind::Readout: {
            std::lock_guard<std::mutex> lk(mtx_);
            return texts_[static_cast<size_t>(i)];
        }
        case Kind::Toggle: return want_[i].load() > 0.5f ? "On" : "Off";
        case Kind::Button:
        case Kind::Popup: return {};   // a picture, no text
    }
    return {};
}

// --- texts and the browser ----------------------------------------------------------------

void Surface::refresh() {
    std::lock_guard<std::mutex> lk(mtx_);
    FileLibrary& lib = presetLibrary();
    const auto L = lib.listing();
    std::vector<std::string> t(P_COUNT);

    // The preset stepper: position in the flat list, text = the preset.
    const int idx = L->find(presetKey_);
    if (idx >= 0) put(P_PRESET, std::min(1.0f, static_cast<float>(idx) / static_cast<float>(stepperRange(static_cast<int>(L->items.size())))));
    t[P_PRESET] = presetKey_.empty() ? "PRESET  -" : "PRESET  " + L->label(presetKey_);

    // Browser: follow the preset when it changed from outside the browser.
    const std::string key = presetKey_;
    const std::vector<Category> cats = categories(*L);
    const int ncat = static_cast<int>(cats.size());
    brCat_ = clampi(brCat_, 0, ncat - 1);
    auto pos = [&cats](int c, const std::string& k) {
        const auto& keys = cats[static_cast<size_t>(c)].keys;
        const auto it = std::find(keys.begin(), keys.end(), k);
        return it == keys.end() ? -1 : static_cast<int>(it - keys.begin());
    };
    if (key != followed_) {
        followed_ = key;
        if (!key.empty() && pos(brCat_, key) < 0) {   // elsewhere: show its category (no preset: stay put)
            const int c = L->categoryOf(L->find(key));
            brCat_ = clampi(c >= 0 ? c + 2 : 2, 0, ncat - 1);   // past FAVORITES and RECENT
            itemPage_ = 0;
        }
        const int p = pos(brCat_, key);
        if (p >= 0) itemPage_ = p / kBrowserItems;
        catPage_ = brCat_ / kBrowserCats;
    }
    const int catPages = std::max(1, (ncat + kBrowserCats - 1) / kBrowserCats);
    catPage_ = clampi(catPage_, 0, catPages - 1);
    catTiles_.assign(kBrowserCats, -1);
    for (int k = 0; k < kBrowserCats; ++k) {
        const int c = catPage_ * kBrowserCats + k;
        const bool has = c < ncat;
        catTiles_[static_cast<size_t>(k)] = has ? c : -1;
        put(kCatTiles[k], has && c == brCat_ ? 1.0f : 0.0f);
        t[static_cast<size_t>(kCatTiles[k])] = has ? upper(cats[static_cast<size_t>(c)].name) : "";
    }
    const auto& keys = cats[static_cast<size_t>(brCat_)].keys;
    const int nitems = static_cast<int>(keys.size());
    const int itemPages = std::max(1, (nitems + kBrowserItems - 1) / kBrowserItems);
    itemPage_ = clampi(itemPage_, 0, itemPages - 1);
    tileKeys_.assign(kBrowserItems, std::string());
    for (int k = 0; k < kBrowserItems; ++k) {
        const int j = itemPage_ * kBrowserItems + k;
        const bool has = j < nitems;
        const std::string& tk = has ? keys[static_cast<size_t>(j)] : std::string();
        tileKeys_[static_cast<size_t>(k)] = tk;
        put(kItemTiles[k], has && tk == key ? 1.0f : 0.0f);
        if (has) {
            const int at = L->find(tk);
            t[static_cast<size_t>(kItemTiles[k])] = at >= 0 ? L->items[static_cast<size_t>(at)].name : L->label(tk);
        }
    }
    // The CHAIN page: each slot's module, lit while it is on, the selected one in brackets.
    const int sel = selected();
    for (int k = 0; k < kNumModules; ++k) {
        const int m = optionOf(want_[P_ORDER_1 + k].load(), P_ORDER_1 + k);
        put(P_SLOT_1 + k, want_[kModuleOnParam[m]].load() > 0.5f ? 1.0f : 0.0f);
        const std::string name = upper(PARAM_INFO[P_ORDER_1].opts[m]);
        t[static_cast<size_t>(P_SLOT_1 + k)] = k == sel ? "[ " + name + " ]" : name;
    }

    // The PERFORM page: each end's scene tiles (lit = the scene there, its lock count), the edit
    // switches, the scenes' line and the fader.
    for (int side = 0; side < 2; ++side) {
        const int cur = sceneOf(side);
        for (int k = 0; k < kNumScenes; ++k) {
            const int id = (side ? P_SCB_1 : P_SCA_1) + k;
            put(id, k == cur ? 1.0f : 0.0f);
            const int locks = scenes_.count(k);
            t[static_cast<size_t>(id)] = std::to_string(k + 1) + (locks ? "  (" + std::to_string(locks) + ")" : "");
        }
    }
    put(P_EDIT_A, editSide_ == 0 ? 1.0f : 0.0f);
    put(P_EDIT_B, editSide_ == 1 ? 1.0f : 0.0f);
    t[P_SCN_INFO] = sceneInfo();
    t[P_XF_BAR] = faderBar();
    // The FX tiles: the bank's effects, lit = the one in scene B as it came.
    const int banks = fxBanks();
    fxBank_ = banks > 0 ? clampi(fxBank_, 0, banks - 1) : 0;
    const std::string& inB = sceneName_[sceneOf(1)];
    for (int k = 0; k < kFxPerBank; ++k) {
        const int fx = fxAt(fxBank_, k);
        put(P_FX_1 + k, fx >= 0 && inB == kFxLibrary[fx].name ? 1.0f : 0.0f);
        t[static_cast<size_t>(P_FX_1 + k)] = fx >= 0 ? upper(kFxLibrary[fx].name) : "";
    }
    const int first = fxAt(fxBank_, 0);
    const int into = editSide_ >= 0 ? editScene_ : sceneOf(1);
    t[P_FX_BANK] = first >= 0 ? "BANK " + std::to_string(fxBank_ + 1) + " / " + std::to_string(banks) + ": " +
                                    upper(kFxLibrary[first].bank) + "   A TAP PUTS IT IN SCENE " + std::to_string(into + 1)
                              : "";

    char b[64];
    std::snprintf(b, sizeof b, "PAGE %d / %d", itemPage_ + 1, itemPages);
    t[P_ITEM_PAGE] = b;
    t[P_BR_NOW] = t[P_PRESET];
    put(P_FAV, !key.empty() && lib.isFavorite(key) ? 1.0f : 0.0f);

    size_t h = 0;
    std::hash<std::string> hs;
    for (int i = 0; i < P_COUNT; ++i) h = h * 1000003u ^ hs(t[static_cast<size_t>(i)]);
    texts_.swap(t);
    if (h != textHash_) {
        textHash_ = h;
        textGen_.fetch_add(1, std::memory_order_release);
    }
}

// --- state --------------------------------------------------------------------------------

void Surface::setValue(int i, float n) {
    if (i < 0 || i >= P_COUNT) return;
    const int steps = stepsOf(i);
    n = clamp01(n);
    if (steps > 0) n = std::round(n * steps) / steps;
    put(i, n);
}

std::string Surface::presetKey() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return presetKey_;
}

void Surface::setPresetKey(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    presetKey_ = key;
}

// --- audio thread -------------------------------------------------------------------------

bool Surface::snapshot(float* out) const {
    const uint32_t before = batchSeq_.load(std::memory_order_acquire);
    if (before & 1u) return false;   // a preset is half written
    // Sound parameters and the order only; not what the surface keeps for itself (tiles, the
    // stepper, the selected slot, texts): the plugin rebuilds the patch whenever this changes.
    float tmp[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) {
        const Kind k = PARAM_INFO[i].kind;
        tmp[i] = k == Kind::Synth || k == Kind::Chain ? want_[i].load(std::memory_order_relaxed) : 0.0f;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (batchSeq_.load(std::memory_order_relaxed) != before) return false;   // one started meanwhile
    std::copy(tmp, tmp + P_COUNT, out);
    return true;
}

void Surface::notify(AutomateFn automate, UpdateFn update, void* ctx) {
    // Nothing changed since the last full pass: no need to look at every value every block.
    const uint32_t changes = changes_.load(std::memory_order_acquire);
    if (changes != scanned_ || scanPending_) {
        int pushed = 0, n = 0;
        for (; n < P_COUNT && pushed < kMaxAutomatePerBlock; ++n) {
            const int i = cursor_;
            cursor_ = (cursor_ + 1) % P_COUNT;
            if (PARAM_INFO[i].kind == Kind::Button) {
                if (release_[i].exchange(false, std::memory_order_acq_rel)) {
                    automate(ctx, i, 0.0f);
                    shown_[i].store(0.0f, std::memory_order_relaxed);
                    ++pushed;
                }
                continue;
            }
            if (PARAM_INFO[i].kind == Kind::Readout) continue;
            const float w = want_[i].load(std::memory_order_relaxed);
            if (std::fabs(w - shown_[i].load(std::memory_order_relaxed)) > 1e-4f) {
                automate(ctx, i, w);
                shown_[i].store(w, std::memory_order_relaxed);
                ++pushed;
            }
        }
        scanPending_ = n < P_COUNT;   // stopped at the per-block cap: go on next block
        if (!scanPending_) scanned_ = changes;
    }
    if (sinceText_ < kTextEveryBlocks) ++sinceText_;   // saturates: no overflow in a long session
    if (sinceText_ >= kTextEveryBlocks) {
        const uint32_t g = textGen_.load(std::memory_order_acquire);
        if (g != textSeen_) {
            textSeen_ = g;
            sinceText_ = 0;
            update(ctx);
        }
    }
}

} // namespace ef
