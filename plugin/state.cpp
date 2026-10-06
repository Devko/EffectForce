#include "state.h"

#include "rack_map.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace ef {
namespace {
constexpr const char* kMagic = "effectforce ";

bool saved(int i) { return PARAM_INFO[i].kind == Kind::Synth || PARAM_INFO[i].kind == Kind::Chain; }

// Numbers in the C locale whatever the process's is (a "0,5" would misread every preset).
std::string number(float v) {
    char b[32];
    const auto r = std::to_chars(b, b + sizeof b, v, std::chars_format::general, 6);   // 6 digits: 333 Hz, not 332.9999
    return std::string(b, r.ptr);
}

bool parse(const std::string& s, float& out) {
    size_t a = 0, e = s.size();
    while (a < e && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (e > a && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    if (a < e && s[a] == '+') ++a;
    float v = 0.0f;
    const auto r = std::from_chars(s.data() + a, s.data() + e, v);
    if (r.ec != std::errc() || r.ptr != s.data() + e || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// A parameter's value as a state line writes it: the real value, or the option's name.
std::string valueText(int i, float norm) {
    const float v = paramValue(i, norm);
    return PARAM_INFO[i].nopts > 0 ? std::string(PARAM_INFO[i].opts[static_cast<int>(v)]) : number(v);
}

// An option by its name ("Ping-Pong", "Reverb") or its index; -1 if it is neither.
int option(int i, const std::string& val) {
    for (int o = 0; o < PARAM_INFO[i].nopts; ++o)
        if (val == PARAM_INFO[i].opts[o]) return o;
    float v = 0.0f;
    if (parse(val, v) && v == std::floor(v) && v >= 0.0f && v < static_cast<float>(PARAM_INFO[i].nopts))
        return static_cast<int>(v);
    return -1;
}

// A line's value as parameter i's 0..1, or -1 if it isn't one.
float normOf(int i, const std::string& val) {
    if (PARAM_INFO[i].nopts > 0) {
        const int o = option(i, val);
        return o >= 0 ? paramNorm(i, static_cast<float>(o)) : -1.0f;
    }
    float v = 0.0f;
    return parse(val, v) ? paramNorm(i, v) : -1.0f;
}

int savedParam(const std::string& key) {
    for (int i = 0; i < P_COUNT; ++i)
        if (saved(i) && key == PARAM_INFO[i].key) return i;
    return -1;
}

// "scene3.flt_cut" -> scene 2 (0-based) and what follows the dot; false if it isn't sceneN.something.
bool sceneOf(const std::string& key, int& scene, std::string& rest) {
    if (key.compare(0, 5, "scene") != 0) return false;
    const size_t dot = key.find('.');
    if (dot == std::string::npos || dot == 5) return false;
    int n = 0;
    const auto r = std::from_chars(key.data() + 5, key.data() + dot, n);
    if (r.ec != std::errc() || r.ptr != key.data() + dot || n < 1 || n > kNumScenes) return false;
    scene = n - 1;
    rest = key.substr(dot + 1);
    return true;
}

// A scene's lock: the scene and the parameter; false if the key isn't one.
bool sceneKey(const std::string& key, int& scene, int& param) {
    std::string rest;
    if (!sceneOf(key, scene, rest)) return false;
    param = savedParam(rest);
    return param >= 0 && Scenes::lockable(param);
}

bool sceneNameKey(const std::string& key, int& scene) {
    std::string rest;
    return sceneOf(key, scene, rest) && rest == "name";
}

// A scene's move= or play= line (rest: what follows "sceneN."; an effect's line: the key itself).
bool moveKey(const std::string& rest) { return rest == "move" || rest == "play"; }

// A lock's value: "end", or "start>end" for a lock that moves. Each as parameter i's 0..1 (start -1
// where it doesn't move); false if either isn't one, or a start on a parameter that can't move.
bool lockOf(int i, const std::string& val, float& start, float& end) {
    const size_t arrow = val.find('>');
    start = -1.0f;
    if (arrow == std::string::npos) {
        end = normOf(i, val);
        return end >= 0.0f;
    }
    if (kSceneMorph[i] != SceneMorph::Line) return false;
    start = normOf(i, val.substr(0, arrow));
    end = normOf(i, val.substr(arrow + 1));
    return start >= 0.0f && end >= 0.0f;
}

// A move= or play= value into the scene's timing (the other half as it is); false if it isn't one.
bool setTiming(Scenes& sc, int scene, const std::string& what, const std::string& val) {
    const int p = what == "move" ? P_MV_LEN : P_MV_PLAY;
    const int o = option(p, val);
    if (o < 0) return false;
    if (what == "move") sc.setMove(scene, o, sc.movePlay(scene));
    else sc.setMove(scene, sc.moveLength(scene), o);
    return true;
}

// A lock as a state line writes it: "end", or "start>end".
std::string lockText(const Scenes& sc, int scene, int i) {
    const float start = sc.start(scene, i);
    const std::string end = valueText(i, sc.value(scene, i));
    return start >= 0.0f ? valueText(i, start) + ">" + end : end;
}
} // namespace

bool isStateText(const std::string& text) {
    size_t at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;   // a UTF-8 byte-order mark (a text editor's)
    if (text.compare(at, std::strlen(kMagic), kMagic) != 0) return false;
    at += std::strlen(kMagic);
    int version = 0;
    const auto r = std::from_chars(text.data() + at, text.data() + text.size(), version);
    return r.ec == std::errc() && version >= 1;
}

std::string saveState(const Surface& s, bool asPreset) {
    std::string out = std::string(kMagic) + std::to_string(kStateVersion) + "\n";
    for (int i = 0; i < P_COUNT; ++i) {
        if (!saved(i)) continue;
        out += PARAM_INFO[i].key;
        out += '=';
        out += valueText(i, s.stateValue(i));   // while a scene is edited: the knobs, not the scene
        out += '\n';
    }
    // The scenes: each one's name (the effect it was made from), its move's timing if it has one, then
    // its locks, sceneN.key=value (start>end for a lock that moves).
    for (int sc = 0; sc < kNumScenes; ++sc) {
        const std::string pre = "scene" + std::to_string(sc + 1) + ".";
        if (!s.sceneName(sc).empty()) out += pre + "name=" + s.sceneName(sc) + "\n";
        if (s.scenes().moves(sc)) {
            out += pre + "move=" + PARAM_INFO[P_MV_LEN].opts[s.scenes().moveLength(sc)] + "\n";
            out += pre + "play=" + PARAM_INFO[P_MV_PLAY].opts[s.scenes().movePlay(sc)] + "\n";
        }
        for (int i = 0; i < P_COUNT; ++i)
            if (s.scenes().locked(sc, i)) out += pre + PARAM_INFO[i].key + "=" + lockText(s.scenes(), sc, i) + "\n";
    }
    if (!asPreset && !s.presetKey().empty()) out += "preset=" + s.presetKey() + "\n";
    return out;
}

bool loadState(Surface& s, const std::string& textIn, bool asPreset) {
    if (!isStateText(textIn)) return false;
    const std::string text = textIn.compare(0, 3, "\xEF\xBB\xBF") == 0 ? textIn.substr(3) : textIn;
    s.endEdit();               // a scene being edited: its knobs go back first
    Surface::Batch batch(s);   // the audio thread never plays a half-loaded sound (knobs and scenes)
    s.scenes().clearAll();     // the state's scenes replace the ones there were (none listed: none)
    for (int sc = 0; sc < kNumScenes; ++sc) s.setSceneName(sc, "");
    if (asPreset)
        for (int i = 0; i < P_COUNT; ++i)
            if (saved(i)) s.setValue(i, PARAM_INFO[i].def);
    // A preset is complete (what it doesn't name is the default); a project changes only what it lists.
    std::string preset;
    bool named[kNumModules] = {};   // the order's slots the text sets
    size_t at = text.find('\n');
    while (at != std::string::npos && at + 1 < text.size()) {
        const size_t end = text.find('\n', at + 1);
        std::string line = text.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
        at = end;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "preset") {
            preset = val;
            continue;
        }
        int scene = 0, param = -1;
        if (sceneNameKey(key, scene)) {
            s.setSceneName(scene, val);
            continue;
        }
        std::string rest;
        if (sceneOf(key, scene, rest) && moveKey(rest)) {
            setTiming(s.scenes(), scene, rest, val);
            continue;
        }
        if (sceneKey(key, scene, param)) {
            float start = 0.0f, end = 0.0f;
            if (lockOf(param, val, start, end)) s.scenes().lock(scene, param, end, start);
            continue;
        }
        const int i = savedParam(key);
        if (i < 0) continue;
        const float n = normOf(i, val);
        if (n < 0.0f) continue;
        s.setValue(i, n);
        if (i >= P_ORDER_1 && i < P_ORDER_1 + kNumModules) named[i - P_ORDER_1] = true;
    }
    // The order must name every module once. One saved before modules were added (the builds before Pulse and Grain had eight
    // slots) names some of them: the slots it doesn't name get the modules it doesn't, in the
    // default order (they were off in it: it sounds the same). Anything else (a module twice, a
    // hand-edited state) is the default.
    int order[kNumModules];
    for (int k = 0; k < kNumModules; ++k) order[k] = static_cast<int>(paramValue(P_ORDER_1 + k, s.get(P_ORDER_1 + k)));
    bool used[kNumModules] = {}, some = false, all = true, twice = false;
    for (int k = 0; k < kNumModules; ++k) {
        if (!named[k]) {
            all = false;
            continue;
        }
        some = true;
        if (order[k] >= 0 && order[k] < kNumModules) {
            twice = twice || used[order[k]];
            used[order[k]] = true;
        }
    }
    if (some && !all && !twice)
        for (int k = 0, next = 0; k < kNumModules; ++k) {
            if (named[k]) continue;
            while (used[next]) ++next;
            used[next] = true;
            order[k] = next;
            s.setValue(P_ORDER_1 + k, paramNorm(P_ORDER_1 + k, static_cast<float>(next)));
        }
    if (!validOrder(order))
        for (int k = 0; k < kNumModules; ++k) s.setValue(P_ORDER_1 + k, PARAM_INFO[P_ORDER_1 + k].def);
    if (!asPreset) s.setPresetKey(preset);   // a project without one came from no preset
    s.refresh();
    return true;
}

std::string loadSceneText(Scenes& sc, int scene, const std::string& text, bool* looper) {
    bool named[P_COUNT] = {};
    std::string name;
    if (looper) *looper = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "name") {
            name = val;
            continue;
        }
        if (moveKey(key)) {   // the move's timing; an effect without one leaves the scene's as it is
            setTiming(sc, scene, key, val);
            continue;
        }
        const int i = savedParam(key);
        if (i < 0 || !Scenes::lockable(i)) continue;
        float from = 0.0f, to = 0.0f;
        if (!lockOf(i, val, from, to)) continue;
        sc.lock(scene, i, to, from);
        named[i] = true;
        if (looper && key.compare(0, 3, "lp_") == 0) *looper = true;
    }
    for (int i = 0; i < P_COUNT; ++i)
        if (!named[i] && sc.locked(scene, i)) sc.unlock(scene, i);
    return name;
}

} // namespace ef
