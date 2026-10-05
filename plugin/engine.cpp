#include "engine.h"

#include <algorithm>
#include <cmath>

namespace ef {

namespace {
// Each slot's parameters, the same distance apart.
static_assert(P_M2_SRC - P_M1_SRC == P_M8_SRC - P_M7_SRC && P_M1_DST - P_M1_SRC == P_M8_DST - P_M8_SRC &&
                  P_M1_AMT - P_M1_SRC == P_M8_AMT - P_M8_SRC,
              "the matrix slots' parameters must repeat in the same order");
constexpr int kSlotStride = P_M2_SRC - P_M1_SRC;
const float kFaderCoef = 1.0f - std::exp(-static_cast<float>(kChunk) / (0.015f * kRate));   // 15 ms per chunk
float unit(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }                     // NaN: 0

int moduleOf(int param) {
    for (int m = 0; m < kNumModules; ++m)
        if (kModuleOnParam[m] == param) return m;
    return -1;
}
}

Engine::Engine() {
    float def[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) def[i] = PARAM_INFO[i].def;
    setParams(def);
    reset();
}

void Engine::seed(uint32_t s) {
    seed_ = s ? s : 1u;
    lfo_[0].reset(seed_);
    lfo_[1].reset(seed_ * 0x9E3779B9u + 7u);
}

void Engine::reset() {
    rack_.reset();
    env_.reset();
    seed(seed_);
    std::fill(src_, src_ + MS_COUNT, 0.0f);
}

void Engine::attachScenes(const Scenes* s) {
    scenes_ = s;
    if (haveRaw_) rebuild();
}

bool Engine::armLooper() { return rack_.looper().allocate(); }

void Engine::setLoopRec(uint32_t presses) {
    loopRec_ = presses;
    base_.looper.rec = presses;   // patch_ is base_ moved by the matrix: it follows each chunk
}

void Engine::setParams(const float* norm) {
    // The crossfader alone moved (the common case while it is played): no rebuild, the next chunks
    // glide to it.
    xTarget_ = unit(norm[P_XFADE]);
    if (xFresh_) x_ = xTarget_;   // nothing played yet (a project loading): no glide from 0
    bool onlyFader = haveRaw_;
    for (int i = 0; i < P_COUNT && onlyFader; ++i) onlyFader = i == P_XFADE || norm[i] == raw_[i];
    std::copy(norm, norm + P_COUNT, raw_);
    haveRaw_ = true;
    if (!onlyFader) rebuild();
}

void Engine::rebuild() {
    std::copy(raw_, raw_ + P_COUNT, norm_);
    nLocks_ = 0;
    if (scenes_) {
        sceneGen_ = scenes_->generation();   // before the values: a change after it rebuilds again
        if (!scenes_->editing()) {           // while editing, the knobs already show the scene
            const int sa = static_cast<int>(paramValue(P_SCENE_A, raw_[P_SCENE_A]));
            const int sb = static_cast<int>(paramValue(P_SCENE_B, raw_[P_SCENE_B]));
            for (int p = 0; p < P_COUNT; ++p) {
                if (!Scenes::lockable(p)) continue;
                const float la = scenes_->value(sa, p), lb = scenes_->value(sb, p);
                if (la < 0.0f && lb < 0.0f) continue;
                const float a = la >= 0.0f ? la : raw_[p], b = lb >= 0.0f ? lb : raw_[p];
                const SceneMorph kind = kSceneMorph[p];
                const bool moves = kind == SceneMorph::Send ? (a > 0.5f) != (b > 0.5f) : a != b;
                if (!moves) {   // both ends the same (a lock against an equal knob, or both locked alike)
                    norm_[p] = a;
                    continue;
                }
                locks_[nLocks_++] = {p, kind == SceneMorph::Send ? moduleOf(p) : -1, a, b, kind};
                if (kind == SceneMorph::Send) norm_[p] = 1.0f;   // runs while either end has it on
            }
        }
    }
    for (int k = 0; k < nLocks_; ++k)
        if (locks_[k].kind != SceneMorph::Send) norm_[locks_[k].param] = Scenes::morph(locks_[k].param, locks_[k].a, locks_[k].b, x_);
    base_ = patchFromParams(norm_);
    base_.looper.rec = loopRec_;
    for (int k = 0; k < nLocks_; ++k)
        if (locks_[k].kind == SceneMorph::Send && locks_[k].module >= 0)
            base_.send[static_cast<size_t>(locks_[k].module)] = Scenes::send(locks_[k].a, locks_[k].b, x_);
    routes();
}

void Engine::routes() {
    nSlots_ = 0;
    for (int k = 0; k < kNumModSlots; ++k) {
        const int at = P_M1_SRC + k * kSlotStride;
        const int src = static_cast<int>(paramValue(at, norm_[at]));
        const int dst = static_cast<int>(paramValue(at + (P_M1_DST - P_M1_SRC), norm_[at + (P_M1_DST - P_M1_SRC)]));
        const float amount = paramValue(at + (P_M1_AMT - P_M1_SRC), norm_[at + (P_M1_AMT - P_M1_SRC)]);
        if (src <= MS_OFF || src >= MS_COUNT || dst <= 0 || dst >= kNumModTargets || amount == 0.0f) continue;
        slots_[nSlots_++] = {src, kModTargetParam[dst], amount};
    }
    env_.set(base_.envAttackS, base_.envReleaseS, base_.envGainDb);
}

void Engine::morph() {
    bool rerouted = false;
    for (int k = 0; k < nLocks_; ++k) {
        const Lock& l = locks_[k];
        if (l.kind == SceneMorph::Send) {
            if (l.module >= 0) base_.send[static_cast<size_t>(l.module)] = Scenes::send(l.a, l.b, x_);
            continue;
        }
        const float n = Scenes::morph(l.param, l.a, l.b, x_);
        if (n == norm_[l.param]) continue;
        norm_[l.param] = n;
        setField(base_, l.param, paramValue(l.param, n));
        rerouted = rerouted || (l.param >= P_M1_SRC && l.param <= P_M8_AMT) || l.param == P_ENV_ATT ||
                   l.param == P_ENV_REL || l.param == P_ENV_GAIN;
    }
    if (rerouted) routes();   // a scene moves the matrix or the envelope follower
}

void Engine::modulate() {
    patch_ = base_;
    int done[kNumModSlots];
    int nDone = 0;
    for (int s = 0; s < nSlots_; ++s) {
        const int param = slots_[s].param;
        if (std::find(done, done + nDone, param) != done + nDone) continue;
        done[nDone++] = param;
        float sum = 0.0f;   // every slot on this target
        for (int k = s; k < nSlots_; ++k)
            if (slots_[k].param == param) sum += slots_[k].amount * src_[slots_[k].src];
        const float n = std::clamp(norm_[param] + sum, 0.0f, 1.0f);
        const float v = paramValue(param, n);
        setField(patch_, param, v);
        // A synced delay has no time of its own: the knob's move scales the synced time instead.
        if (param == P_DLY_TIME && base_.delay.sync) {
            const float baseV = paramValue(P_DLY_TIME, norm_[P_DLY_TIME]);
            if (baseV > 0.0f) patch_.delay.divBeats = base_.delay.divBeats * v / baseV;
        }
    }
}

void Engine::render(float* L, float* R, int n, Transport t) {
    if (scenes_ && scenes_->generation() != sceneGen_ && haveRaw_) rebuild();   // a lock changed (UI thread)
    xFresh_ = false;
    for (int pos = 0; pos < n; pos += kChunk) {
        const int m = std::min(kChunk, n - pos);
        if (x_ != xTarget_) {   // the crossfader glides; the scenes' parameters follow it
            x_ += (xTarget_ - x_) * kFaderCoef;
            if (std::fabs(xTarget_ - x_) < 1e-4f) x_ = xTarget_;
            morph();
        }
        for (int k = 0; k < 4; ++k) src_[MS_MACRO1 + k] = norm_[P_MAC_1 + k];   // a macro's knob is its value
        const RackPatch* use = &base_;
        if (nSlots_ > 0) {
            modulate();
            use = &patch_;
        }
        // The sources for the next chunk, from this one's input and position.
        src_[MS_ENV] = env_.process(L + pos, R + pos, m);
        src_[MS_LFO1] = lfo_[0].next(use->lfo[0], t, m);
        src_[MS_LFO2] = lfo_[1].next(use->lfo[1], t, m);
        rack_.process(*use, t, L + pos, R + pos, m);
        t.beats += m / static_cast<double>(kRate) * t.bpm / 60.0;
    }
}

} // namespace ef
