#include "parameters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace ef {

namespace {

constexpr char kStateMagic[] = "effectforce-probe 1";

bool valid(int id) { return id >= 0 && id < P_COUNT; }

} // namespace

float realOf(int id, float norm) {
    if (!valid(id)) return 0.0f;
    const ParamInfo& p = PARAM_INFO[id];
    const float n = std::clamp(norm, 0.0f, 1.0f);
    if (p.kind == Kind::Enum) return std::round(n * (p.hi - p.lo)) + p.lo;
    return p.lo + n * (p.hi - p.lo);
}

float normOf(int id, float real) {
    if (!valid(id)) return 0.0f;
    const ParamInfo& p = PARAM_INFO[id];
    if (p.hi <= p.lo) return 0.0f;
    return std::clamp((real - p.lo) / (p.hi - p.lo), 0.0f, 1.0f);
}

Params::Params() {
    for (int i = 0; i < P_COUNT; ++i) norm_[i].store(normOf(i, PARAM_INFO[i].def), std::memory_order_relaxed);
}

float Params::get(int id) const {
    if (!valid(id) || PARAM_INFO[id].kind == Kind::Readout) return 0.0f;
    return norm_[id].load(std::memory_order_relaxed);
}

void Params::set(int id, float norm) {
    if (!valid(id) || PARAM_INFO[id].kind == Kind::Readout || !std::isfinite(norm)) return;
    norm_[id].store(std::clamp(norm, 0.0f, 1.0f), std::memory_order_relaxed);
    writes_.fetch_add(1, std::memory_order_release);
}

int Params::option(int id) const { return static_cast<int>(real(id)); }

std::string Params::display(int id) const {
    if (!valid(id)) return "";
    const ParamInfo& p = PARAM_INFO[id];
    const float v = real(id);
    char b[32];
    switch (p.fmt) {
        case Fmt::Db: std::snprintf(b, sizeof b, "%+.1f dB", static_cast<double>(v)); break;
        case Fmt::Percent: std::snprintf(b, sizeof b, "%d%%", static_cast<int>(std::lround(v * 100.0f))); break;
        case Fmt::Enum: return p.opts[std::clamp(static_cast<int>(v), 0, p.nOpts - 1)];
        case Fmt::None: return "";
    }
    return b;
}

std::string Params::save() const {
    std::string s = kStateMagic;
    s += '\n';
    char b[96];
    for (int i = 0; i < P_COUNT; ++i) {
        const ParamInfo& p = PARAM_INFO[i];
        if (p.kind == Kind::Readout) continue;
        if (p.kind == Kind::Enum) std::snprintf(b, sizeof b, "%s=%s\n", p.key, p.opts[option(i)]);
        else std::snprintf(b, sizeof b, "%s=%.6g\n", p.key, static_cast<double>(real(i)));
        s += b;
    }
    return s;
}

bool Params::load(const std::string& state) {
    std::istringstream in(state);
    std::string line;
    if (!std::getline(in, line) || line != kStateMagic) return false;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        for (int i = 0; i < P_COUNT; ++i) {
            const ParamInfo& p = PARAM_INFO[i];
            if (p.kind == Kind::Readout || key != p.key) continue;
            if (p.kind == Kind::Enum) {
                for (int o = 0; o < p.nOpts; ++o)
                    if (value == p.opts[o]) set(i, normOf(i, static_cast<float>(o)));
            } else {
                char* end = nullptr;
                const float v = std::strtof(value.c_str(), &end);
                if (end != value.c_str() && std::isfinite(v)) set(i, normOf(i, v));
            }
        }
    }
    return true;
}

} // namespace ef
