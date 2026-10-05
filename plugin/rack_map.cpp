#include "rack_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ef {

// Option lists in surface.py against the engine's enums.
static_assert(int(kNumModules) == int(RM_COUNT) && int(M_DRIVE) == int(RM_DRIVE) && int(M_FILTER) == int(RM_FILTER) &&
                  int(M_EQ) == int(RM_EQ) && int(M_COMP) == int(RM_COMP) && int(M_CHORUS) == int(RM_CHORUS) &&
                  int(M_PHASER) == int(RM_PHASER) && int(M_PULSE) == int(RM_PULSE) && int(M_GRAIN) == int(RM_GRAIN) &&
                  int(M_DELAY) == int(RM_DELAY) && int(M_REVERB) == int(RM_REVERB),
              "surface.py MODULES must match dsp/rack.h RackModule");
static_assert(kNumModSources == MS_COUNT && kNumLfoWaves == LW_COUNT, "surface.py mod lists must match dsp/mod.h");
static_assert(kNumDelayDivisions == kNumDelayDivs && kNumLfoDivisions == kNumLfoDivs,
              "surface.py DELAY_DIVS / LFO_DIVS must match dsp/common.h");
static_assert(PARAM_INFO[P_PLS_PATTERN].nopts == Pulse::kPatterns, "surface.py PULSE_PATTERNS must match dsp/pulse.h");
static_assert(PARAM_INFO[P_L2_WAVE].key[0] == 'l' && P_L2_PHASE - P_L2_WAVE == P_L1_PHASE - P_L1_WAVE,
              "LFO 2's parameters must mirror LFO 1's");

namespace {

// The phaser's centre (0..1) as the module reads it: Hz for a phaser, ms for the flanger.
constexpr float kPhaserLoHz = 50.0f, kPhaserHiHz = 8000.0f, kFlangerLoMs = 0.2f, kFlangerHiMs = 10.0f;
constexpr int kFlangerMode = 3;   // surface.py phs_mode "Flanger"

bool isOff(int id, float v) {   // a cut at the end of its range is no cut
    const ParamSpec& s = PARAM_SPECS[id];
    return s.fmt == Fmt::HzLo ? v <= s.lo * 1.0005f : s.fmt == Fmt::HzHi ? v >= s.hi * 0.9995f : false;
}

} // namespace

float paramValue(int id, float n) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    n = n > 0.0f ? (n < 1.0f ? n : 1.0f) : 0.0f;   // NaN-safe (std::clamp passes NaN through)
    switch (s.curve) {
        case Curve::Lin:  return s.lo + n * (s.hi - s.lo);
        case Curve::Log:  return s.lo * std::pow(s.hi / s.lo, n);
        case Curve::Int:  return std::round(s.lo + n * (s.hi - s.lo));
        case Curve::Enum: return std::round(n * s.hi);   // lo = 0, hi = options - 1
        case Curve::Pow:  return s.hi * n * n * n;
        default:          return 0.0f;
    }
}

float paramNorm(int id, float v) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    float n = 0.0f;
    switch (s.curve) {
        case Curve::Log: n = v > 0.0f ? std::log(v / s.lo) / std::log(s.hi / s.lo) : 0.0f; break;
        case Curve::Pow: n = s.hi > 0.0f && v > 0.0f ? std::cbrt(v / s.hi) : 0.0f; break;
        case Curve::Enum: n = s.hi > 0.0f ? std::round(v) / s.hi : 0.0f; break;
        case Curve::Lin:
        case Curve::Int: n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f; break;
        default: break;
    }
    return std::isfinite(n) ? std::clamp(n, 0.0f, 1.0f) : 0.0f;
}

std::string paramDisplay(int id, float norm, const float* all) {
    if (id < 0 || id >= P_COUNT) return {};
    const float v = paramValue(id, norm);
    const ParamSpec& s = PARAM_SPECS[id];
    char b[48];
    switch (s.fmt) {
        case Fmt::Enum:
        case Fmt::Text: {
            const int i = static_cast<int>(v);
            return i >= 0 && i < PARAM_INFO[id].nopts ? PARAM_INFO[id].opts[i] : "";
        }
        case Fmt::Percent: std::snprintf(b, sizeof b, "%.0f%%", v * 100.0f); break;
        case Fmt::Bipolar:
            std::snprintf(b, sizeof b, std::fabs(v) < 0.005f ? "0%%" : "%+.0f%%", v * 100.0f);
            break;
        case Fmt::HzLo:
        case Fmt::HzHi:
            if (isOff(id, v)) return "Off";
            [[fallthrough]];
        // Unit changes where the rounded text would reach the next unit ("1000 Hz" is "1.00 kHz").
        case Fmt::Hz:
            if (v < 999.5f) std::snprintf(b, sizeof b, "%.0f Hz", v);
            else std::snprintf(b, sizeof b, v < 9995.0f ? "%.2f kHz" : "%.1f kHz", v / 1000.0f);
            break;
        case Fmt::Time:
            if (v < 0.00995f) std::snprintf(b, sizeof b, "%.1f ms", v * 1000.0f);
            else if (v < 0.9995f) std::snprintf(b, sizeof b, "%.0f ms", v * 1000.0f);
            else std::snprintf(b, sizeof b, "%.2f s", v);
            break;
        case Fmt::Db: std::snprintf(b, sizeof b, "%.1f dB", std::fabs(v) < 0.05f ? 0.0f : v); break;   // never "-0.0 dB"
        case Fmt::Ratio: std::snprintf(b, sizeof b, v < 9.95f ? "%.1f:1" : "%.0f:1", v); break;
        case Fmt::Mult: std::snprintf(b, sizeof b, "%.2fx", v); break;
        case Fmt::Q: std::snprintf(b, sizeof b, "Q %.2f", v); break;
        case Fmt::Oct: std::snprintf(b, sizeof b, std::fabs(v) < 0.005f ? "0 oct" : "%+.2f oct", v); break;
        case Fmt::Degrees: std::snprintf(b, sizeof b, "%.0f deg", v); break;
        case Fmt::LfoHz: std::snprintf(b, sizeof b, v < 0.995f ? "%.2f Hz" : (v < 9.95f ? "%.1f Hz" : "%.0f Hz"), v); break;
        case Fmt::Center: {
            const bool flanger = all && static_cast<int>(paramValue(P_PHS_MODE, all[P_PHS_MODE])) == kFlangerMode;
            if (flanger) {
                const float ms = kFlangerLoMs * std::pow(kFlangerHiMs / kFlangerLoMs, v);
                std::snprintf(b, sizeof b, "%.2f ms", ms);
                break;
            }
            const float hz = kPhaserLoHz * std::pow(kPhaserHiHz / kPhaserLoHz, v);
            if (hz < 999.5f) std::snprintf(b, sizeof b, "%.0f Hz", hz);
            else std::snprintf(b, sizeof b, "%.2f kHz", hz / 1000.0f);
            break;
        }
        case Fmt::Count: std::snprintf(b, sizeof b, "%.0f", v); break;
        case Fmt::Semi: std::snprintf(b, sizeof b, v == 0.0f ? "0 st" : "%+.0f st", v); break;
        default: return {};
    }
    return b;
}

void setField(RackPatch& p, int id, float v) {
    const int opt = static_cast<int>(v);
    const bool on = v > 0.5f;
    if (id >= P_ORDER_1 && id < P_ORDER_1 + RM_COUNT) {
        p.order[id - P_ORDER_1] = std::clamp(opt, 0, RM_COUNT - 1);
        return;
    }
    for (int m = 0; m < RM_COUNT; ++m)
        if (id == kModuleOnParam[m]) {
            p.on[m] = on;
            return;
        }
    switch (id) {
        case P_IN_GAIN: p.inDb = v; break;
        case P_OUT_GAIN: p.outDb = v; break;
        case P_MIX: p.mix = v; break;

        case P_DRV_TYPE: p.drive.type = opt; break;
        case P_DRV_AMT: p.drive.driveDb = v; break;
        case P_DRV_TONE: p.drive.tone = v; break;
        case P_DRV_BIAS: p.drive.bias = v; break;
        case P_DRV_OUT: p.drive.outDb = v; break;
        case P_DRV_MIX: p.drive.mix = v; break;

        case P_FLT_TYPE: p.filter.type = opt; break;
        case P_FLT_CUT: p.filter.cutoffHz = v; break;
        case P_FLT_RES: p.filter.res = v; break;
        case P_FLT_DRIVE: p.filter.drive = v; break;
        case P_FLT_SPREAD: p.filter.spread = v; break;
        case P_FLT_MIX: p.filter.mix = v; break;

        case P_EQ_LC: p.eq.lowCutHz = v; break;
        case P_EQ_LF: p.eq.lowFreq = v; break;
        case P_EQ_LG: p.eq.lowGainDb = v; break;
        case P_EQ_MF: p.eq.midFreq = v; break;
        case P_EQ_MG: p.eq.midGainDb = v; break;
        case P_EQ_MQ: p.eq.midQ = v; break;
        case P_EQ_HF: p.eq.highFreq = v; break;
        case P_EQ_HG: p.eq.highGainDb = v; break;
        case P_EQ_HC: p.eq.highCutHz = v; break;

        case P_CMP_MODE: p.comp.mode = opt; break;
        case P_CMP_THR: p.comp.thresholdDb = v; break;
        case P_CMP_RATIO: p.comp.ratio = v; break;
        case P_CMP_ATT: p.comp.attackMs = v * 1000.0f; break;
        case P_CMP_REL: p.comp.releaseMs = v * 1000.0f; break;
        case P_CMP_KNEE: p.comp.kneeDb = v; break;
        case P_CMP_SC: p.comp.scLowCutHz = v; break;
        case P_CMP_MAKEUP: p.comp.makeupDb = v; break;
        case P_CMP_MIX: p.comp.mix = v; break;
        case P_OTT_DEPTH: p.comp.ottDepth = v; break;
        case P_OTT_TIME: p.comp.ottTime = v; break;
        case P_OTT_UP: p.comp.ottUp = v; break;
        case P_OTT_DOWN: p.comp.ottDown = v; break;
        case P_OTT_LOW: p.comp.ottLowDb = v; break;
        case P_OTT_MID: p.comp.ottMidDb = v; break;
        case P_OTT_HIGH: p.comp.ottHighDb = v; break;

        case P_CHR_MODE: p.chorus.mode = opt; break;
        case P_CHR_RATE: p.chorus.rateHz = v; break;
        case P_CHR_DEPTH: p.chorus.depth = v; break;
        case P_CHR_DELAY: p.chorus.delayMs = v * 1000.0f; break;
        case P_CHR_LC: p.chorus.lowCutHz = v; break;
        case P_CHR_WIDTH: p.chorus.width = v; break;
        case P_CHR_MIX: p.chorus.mix = v; break;

        case P_PHS_MODE: p.phaser.mode = opt; break;
        case P_PHS_SYNC: p.phaser.sync = on; break;
        case P_PHS_RATE: p.phaser.rateHz = v; break;
        case P_PHS_DIV: p.phaser.divBeats = kLfoDivs[std::clamp(opt, 0, kNumLfoDivs - 1)].beats; break;
        case P_PHS_DEPTH: p.phaser.depth = v; break;
        case P_PHS_CENTER: p.phaser.center = v; break;
        case P_PHS_FB: p.phaser.feedback = v; break;
        case P_PHS_STEREO: p.phaser.stereo = v; break;
        case P_PHS_MIX: p.phaser.mix = v; break;

        case P_DLY_MODE: p.delay.mode = opt; break;
        case P_DLY_SYNC: p.delay.sync = on; break;
        case P_DLY_TIME: p.delay.timeMs = v * 1000.0f; break;
        case P_DLY_DIV: p.delay.divBeats = kDelayDivs[std::clamp(opt, 0, kNumDelayDivs - 1)].beats; break;
        case P_DLY_FB: p.delay.feedback = v; break;
        case P_DLY_SPREAD: p.delay.spread = v; break;
        case P_DLY_LC: p.delay.lowCutHz = v; break;
        case P_DLY_HC: p.delay.highCutHz = v; break;
        case P_DLY_WOW: p.delay.wow = v; break;
        case P_DLY_DRIVE: p.delay.drive = v; break;
        case P_DLY_DUCK: p.delay.duck = v; break;
        case P_DLY_MIX: p.delay.mix = v; break;
        case P_DLY_GLIDE: p.delay.glide = opt; break;

        case P_REV_MODE: p.reverb.mode = opt; break;
        case P_REV_SIZE: p.reverb.size = v; break;
        case P_REV_DECAY: p.reverb.decayS = v; break;
        case P_REV_PRE: p.reverb.predelayMs = v * 1000.0f; break;
        case P_REV_DAMP: p.reverb.dampHz = v; break;
        case P_REV_LC: p.reverb.lowCutHz = v; break;
        case P_REV_MOD: p.reverb.mod = v; break;
        case P_REV_WIDTH: p.reverb.width = v; break;
        case P_REV_FREEZE: p.reverb.freeze = on; break;
        case P_REV_MIX: p.reverb.mix = v; break;
        case P_REV_SHIM: p.reverb.shimmer = v; break;
        case P_REV_SHIM_INT: p.reverb.shimmerInterval = opt; break;

        case P_PLS_MODE: p.pulse.mode = opt; break;
        case P_PLS_SYNC: p.pulse.sync = on; break;
        case P_PLS_RATE: p.pulse.rateHz = v; break;
        case P_PLS_DIV: p.pulse.divBeats = kLfoDivs[std::clamp(opt, 0, kNumLfoDivs - 1)].beats; break;
        case P_PLS_DEPTH: p.pulse.depth = v; break;
        case P_PLS_SHAPE: p.pulse.shape = v; break;
        case P_PLS_STEREO: p.pulse.stereo = v; break;
        case P_PLS_PATTERN: p.pulse.pattern = opt; break;
        case P_PLS_LENGTH: p.pulse.length = v; break;
        case P_PLS_SMOOTH: p.pulse.smooth = v * 1000.0f; break;
        case P_PLS_MIX: p.pulse.mix = v; break;

        case P_GRN_MODE: p.grain.mode = opt; break;
        case P_GRN_SYNC: p.grain.sync = on; break;
        case P_GRN_SIZE: p.grain.sizeMs = v * 1000.0f; break;
        case P_GRN_DIV: p.grain.sizeBeats = kDelayDivs[std::clamp(opt, 0, kNumDelayDivs - 1)].beats; break;
        case P_GRN_DENSITY: p.grain.density = v; break;
        case P_GRN_PITCH: p.grain.pitch = v; break;
        case P_GRN_REVERSE: p.grain.reverse = v; break;
        case P_GRN_SPREAD: p.grain.spread = v; break;
        case P_GRN_FB: p.grain.feedback = v; break;
        case P_GRN_HOLD: p.grain.hold = on; break;
        case P_GRN_MIX: p.grain.mix = v; break;

        case P_ENV_ATT: p.envAttackS = v; break;
        case P_ENV_REL: p.envReleaseS = v; break;
        case P_ENV_GAIN: p.envGainDb = v; break;
        default: break;
    }
    for (int l = 0; l < 2; ++l) {   // LFO 2's parameters mirror LFO 1's (static_assert above)
        const int k = id - (l ? P_L2_WAVE : P_L1_WAVE);
        LfoParams& f = p.lfo[l];
        if (k == P_L1_WAVE - P_L1_WAVE) f.wave = opt;
        else if (k == P_L1_SYNC - P_L1_WAVE) f.sync = on;
        else if (k == P_L1_RATE - P_L1_WAVE) f.rateHz = v;
        else if (k == P_L1_DIV - P_L1_WAVE) f.divBeats = kLfoDivs[std::clamp(opt, 0, kNumLfoDivs - 1)].beats;
        else if (k == P_L1_PHASE - P_L1_WAVE) f.phase = v / 360.0f;
    }
}

RackPatch patchFromParams(const float* norm) {
    RackPatch p;
    for (int i = 0; i < P_COUNT; ++i)
        if (PARAM_INFO[i].kind == Kind::Synth || PARAM_INFO[i].kind == Kind::Chain) setField(p, i, paramValue(i, norm[i]));
    if (!validOrder(p.order))   // a state that isn't a permutation: the default order
        for (int k = 0; k < RM_COUNT; ++k) p.order[k] = k;
    return p;
}

} // namespace ef
