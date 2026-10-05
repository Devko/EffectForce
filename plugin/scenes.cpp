#include "scenes.h"

namespace ef {

namespace {
bool validScene(int s) { return s >= 0 && s < kNumScenes; }
float unit(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }   // NaN: 0
}

Scenes::Scenes() {
    for (auto& scene : v_)
        for (auto& v : scene) v.store(kNone, std::memory_order_relaxed);
}

void Scenes::lock(int scene, int param, float norm) {
    if (!validScene(scene) || !lockable(param)) return;
    v_[scene][param].store(unit(norm), std::memory_order_relaxed);
    changed();
}

void Scenes::unlock(int scene, int param) {
    if (!validScene(scene) || param < 0 || param >= P_COUNT) return;
    v_[scene][param].store(kNone, std::memory_order_relaxed);
    changed();
}

void Scenes::clear(int scene) {
    if (!validScene(scene)) return;
    for (auto& v : v_[scene]) v.store(kNone, std::memory_order_relaxed);
    changed();
}

void Scenes::clearAll() {
    for (int s = 0; s < kNumScenes; ++s)
        for (auto& v : v_[s]) v.store(kNone, std::memory_order_relaxed);
    changed();
}

void Scenes::setEditing(bool on) {
    editing_.store(on, std::memory_order_release);
    changed();
}

float Scenes::value(int scene, int param) const {
    if (!validScene(scene) || param < 0 || param >= P_COUNT) return kNone;
    return v_[scene][param].load(std::memory_order_relaxed);
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
