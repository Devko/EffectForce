#include "scenes.h"

namespace ef {

namespace {
bool validScene(int s) { return s >= 0 && s < kNumScenes; }
bool validParam(int p) { return p >= 0 && p < P_COUNT; }
float unit(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }   // NaN: 0
}

Scenes::Scenes() {
    for (int s = 0; s < kNumScenes; ++s) {
        for (auto& v : v_[s]) v.store(kNone, std::memory_order_relaxed);
        for (auto& v : from_[s]) v.store(kNone, std::memory_order_relaxed);
        len_[s].store(kDefaultLength, std::memory_order_relaxed);
        play_[s].store(kMoveOnce, std::memory_order_relaxed);
        epoch_[s].store(0, std::memory_order_relaxed);
    }
}

void Scenes::lock(int scene, int param, float norm) {
    if (!validScene(scene) || !lockable(param)) return;
    from_[scene][param].store(kNone, std::memory_order_relaxed);   // set by hand: it holds still
    v_[scene][param].store(unit(norm), std::memory_order_relaxed);
    changed();
}

void Scenes::lock(int scene, int param, float norm, float start) {
    if (!validScene(scene) || !lockable(param)) return;
    const bool moves = start >= 0.0f && kSceneMorph[param] == SceneMorph::Line;
    from_[scene][param].store(moves ? unit(start) : kNone, std::memory_order_relaxed);
    v_[scene][param].store(unit(norm), std::memory_order_relaxed);
    if (moves) restart(scene);
    changed();
}

void Scenes::unlock(int scene, int param) {
    if (!validScene(scene) || !validParam(param)) return;
    from_[scene][param].store(kNone, std::memory_order_relaxed);
    v_[scene][param].store(kNone, std::memory_order_relaxed);
    changed();
}

void Scenes::clear(int scene) {
    if (!validScene(scene)) return;
    for (auto& v : v_[scene]) v.store(kNone, std::memory_order_relaxed);
    for (auto& v : from_[scene]) v.store(kNone, std::memory_order_relaxed);
    len_[scene].store(kDefaultLength, std::memory_order_relaxed);
    play_[scene].store(kMoveOnce, std::memory_order_relaxed);
    restart(scene);
    changed();
}

void Scenes::clearAll() {
    for (int s = 0; s < kNumScenes; ++s) clear(s);
}

void Scenes::setEditing(bool on) {
    editing_.store(on, std::memory_order_release);
    changed();
}

void Scenes::setStart(int scene, int param, float norm) {
    if (!validScene(scene) || !validParam(param) || kSceneMorph[param] != SceneMorph::Line) return;
    if (!locked(scene, param)) return;   // a start belongs to a lock
    const float v = norm >= 0.0f ? unit(norm) : kNone;
    if (from_[scene][param].load(std::memory_order_relaxed) == v) return;
    from_[scene][param].store(v, std::memory_order_relaxed);
    restart(scene);
    changed();
}

void Scenes::setMove(int scene, int length, int play) {
    if (!validScene(scene)) return;
    length = length < 0 ? 0 : length >= kNumMoveLengths ? kNumMoveLengths - 1 : length;
    play = play < 0 ? 0 : play >= kNumMovePlays ? kNumMovePlays - 1 : play;
    if (len_[scene].load(std::memory_order_relaxed) == length && play_[scene].load(std::memory_order_relaxed) == play) return;
    len_[scene].store(length, std::memory_order_relaxed);
    play_[scene].store(play, std::memory_order_relaxed);
    restart(scene);
    changed();
}

float Scenes::value(int scene, int param) const {
    if (!validScene(scene) || !validParam(param)) return kNone;
    return v_[scene][param].load(std::memory_order_relaxed);
}

float Scenes::start(int scene, int param) const {
    if (!validScene(scene) || !validParam(param)) return kNone;
    return from_[scene][param].load(std::memory_order_relaxed);
}

bool Scenes::moves(int scene) const {
    if (!validScene(scene)) return false;
    for (const auto& v : from_[scene])
        if (v.load(std::memory_order_relaxed) >= 0.0f) return true;
    return false;
}

int Scenes::moveLength(int scene) const {
    return validScene(scene) ? len_[scene].load(std::memory_order_relaxed) : 0;
}

int Scenes::movePlay(int scene) const {
    return validScene(scene) ? play_[scene].load(std::memory_order_relaxed) : kMoveOnce;
}

uint32_t Scenes::moveEpoch(int scene) const {
    return validScene(scene) ? epoch_[scene].load(std::memory_order_acquire) : 0;
}

int Scenes::count(int scene) const {
    if (!validScene(scene)) return 0;
    int n = 0;
    for (const auto& v : v_[scene]) n += v.load(std::memory_order_relaxed) >= 0.0f;
    return n;
}

float Scenes::morph(int param, float a, float b, float x) {
    x = unit(x);
    if (param >= 0 && param < P_COUNT && kSceneMorph[param] == SceneMorph::Switch) return x < 0.5f ? a : b;
    return a + (b - a) * x;
}

float Scenes::send(float aOn, float bOn, float x) {
    const float a = aOn > 0.5f ? 1.0f : 0.0f, b = bOn > 0.5f ? 1.0f : 0.0f;
    return a + (b - a) * unit(x);
}

} // namespace ef
