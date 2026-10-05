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

// An option by its name ("Ping-Pong", "Reverb") or its index; -1 if it is neither.
int option(int i, const std::string& val) {
    for (int o = 0; o < PARAM_INFO[i].nopts; ++o)
        if (val == PARAM_INFO[i].opts[o]) return o;
    float v = 0.0f;
    if (parse(val, v) && v == std::floor(v) && v >= 0.0f && v < static_cast<float>(PARAM_INFO[i].nopts))
        return static_cast<int>(v);
    return -1;
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
        const float v = paramValue(i, s.get(i));
        out += PARAM_INFO[i].nopts > 0 ? std::string(PARAM_INFO[i].opts[static_cast<int>(v)]) : number(v);
        out += '\n';
    }
    if (!asPreset && !s.presetKey().empty()) out += "preset=" + s.presetKey() + "\n";
    return out;
}

bool loadState(Surface& s, const std::string& textIn, bool asPreset) {
    if (!isStateText(textIn)) return false;
    const std::string text = textIn.compare(0, 3, "\xEF\xBB\xBF") == 0 ? textIn.substr(3) : textIn;
    Surface::Batch batch(s);   // the audio thread never plays a half-loaded sound
    if (asPreset)
        for (int i = 0; i < P_COUNT; ++i)
            if (saved(i)) s.setValue(i, PARAM_INFO[i].def);
    // A preset is complete (what it doesn't name is the default); a project changes only what it lists.
    std::string preset;
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
        for (int i = 0; i < P_COUNT; ++i)
            if (saved(i) && key == PARAM_INFO[i].key) {
                if (PARAM_INFO[i].nopts > 0) {
                    const int o = option(i, val);
                    if (o >= 0) s.setValue(i, paramNorm(i, static_cast<float>(o)));
                } else {
                    float v = 0.0f;
                    if (parse(val, v)) s.setValue(i, paramNorm(i, v));
                }
                break;
            }
    }
    // The order must name every module once; anything else (an old or hand-edited state) is the default.
    int order[kNumModules];
    for (int k = 0; k < kNumModules; ++k) order[k] = static_cast<int>(paramValue(P_ORDER_1 + k, s.get(P_ORDER_1 + k)));
    if (!validOrder(order))
        for (int k = 0; k < kNumModules; ++k) s.setValue(P_ORDER_1 + k, PARAM_INFO[P_ORDER_1 + k].def);
    if (!asPreset) s.setPresetKey(preset);   // a project without one came from no preset
    s.refresh();
    return true;
}

} // namespace ef
