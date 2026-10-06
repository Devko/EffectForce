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

// A scene is heard once the fader is this far from the other end (and out again below it): half a
// Q-Link detent (1/128), so one detent brings it in while a fader resting a hair off its end doesn't.
constexpr float kHeard = 0.005f;

// What an end plays of a lock: its start, the lock, or on the line between (exactly the lock once a
// move is done, as a lock that holds still).
float along(float from, float to, float at) { return at >= 1.0f ? to : from + (to - from) * at; }

int moduleOf(int param) {
    for (int m = 0; m < kNumModules; ++m)
        if (kModuleOnParam[m] == param) return m;
    return -1;
}

// Where a running move plays, 0 (its starts) .. 1 (its locks), `beats` after it started: Once a
// straight line, then held; Loop the line again and again; Ping-pong there and back, `length` each way.
float moveAt(double beats, double length, int play) {
    const double u = beats / length;
    switch (play) {
        case kMoveLoop: return static_cast<float>(u - std::floor(u));
        case kMovePingPong: {
            const double v = u - 2.0 * std::floor(u * 0.5);
            return static_cast<float>(v <= 1.0 ? v : 2.0 - v);
        }
        default: return static_cast<float>(std::min(u, 1.0));
    }
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
    base_.looper.rec = patch_.looper.rec = presses;
}

void Engine::setParams(const float* norm) {
    // The crossfader alone moved (the common case while it is played): no rebuild, the next chunks
    // glide to it.
    xTarget_ = unit(norm[P_XFADE]);
    const bool jumped = xFresh_ && x_ != xTarget_;
    if (xFresh_) x_ = xTarget_;   // nothing played yet (a project loading): no glide from 0
    bool onlyFader = haveRaw_;
    for (int i = 0; i < P_COUNT && onlyFader; ++i) onlyFader = i == P_XFADE || norm[i] == raw_[i];
    std::copy(norm, norm + P_COUNT, raw_);
    haveRaw_ = true;
    if (!onlyFader) rebuild();
    else if (jumped) morph();   // no glide will follow it: the scenes' parameters there at once
}

void Engine::rebuild() {
    std::copy(raw_, raw_ + P_COUNT, norm_);
    nLocks_ = 0;
    if (scenes_) {
        sceneGen_ = scenes_->generation();   // before the values: a change after it rebuilds again
        if (!scenes_->editing()) {           // while editing, the knobs already show the scene
            const int sa = static_cast<int>(paramValue(P_SCENE_A, raw_[P_SCENE_A]));
            const int sb = static_cast<int>(paramValue(P_SCENE_B, raw_[P_SCENE_B]));
            // Each end's clock: another scene there, or its move changed, starts it over.
            const int sides[2] = {sa, sb};
            for (int side = 0; side < 2; ++side) {
                Clock& c = clock_[side];
                const int sc = sides[side];
                const uint32_t epoch = scenes_->moveEpoch(sc);
                const double length = kMoveBeats[scenes_->moveLength(sc)];
                const bool has = length > 0.0 && scenes_->moves(sc);
                if (sc == c.scene && epoch == c.epoch && has == (c.state != kMoveNone)) continue;
                c = Clock{};
                c.scene = sc;
                c.epoch = epoch;
                c.length = length;
                c.play = scenes_->movePlay(sc);
                c.state = has ? kMoveIdle : kMoveNone;
            }
            for (int p = 0; p < P_COUNT; ++p) {
                if (!Scenes::lockable(p)) continue;
                const float la = scenes_->value(sa, p), lb = scenes_->value(sb, p);
                if (la < 0.0f && lb < 0.0f) continue;
                const float a = la >= 0.0f ? la : raw_[p], b = lb >= 0.0f ? lb : raw_[p];
                // Where each end's move starts it: its start, while that end's scene has a move.
                const float fa = la >= 0.0f && clock_[0].state != kMoveNone ? scenes_->start(sa, p) : -1.0f;
                const float fb = lb >= 0.0f && clock_[1].state != kMoveNone ? scenes_->start(sb, p) : -1.0f;
                const float a0 = fa >= 0.0f ? fa : a, b0 = fb >= 0.0f ? fb : b;
                const SceneMorph kind = kSceneMorph[p];
                // Not only the ends' locks: an end that moves it may meet the other end only at its lock.
                const bool moves = kind == SceneMorph::Send ? (a > 0.5f) != (b > 0.5f) : a != b || a0 != a || b0 != b;
                if (!moves) {   // both ends the same (a lock against an equal knob, or both locked alike)
                    norm_[p] = a;
                    continue;
                }
                const ParamSpec& spec = PARAM_SPECS[p];
                const float logRatio = spec.curve == Curve::Log ? static_cast<float>(std::log2(static_cast<double>(spec.hi) / spec.lo)) : 0.0f;
                locks_[nLocks_++] = {p, kind == SceneMorph::Send ? moduleOf(p) : -1, a, b, a0, b0, logRatio, kind};
                if (kind == SceneMorph::Send) norm_[p] = 1.0f;   // runs while either end has it on
            }
        }
    }
    for (int k = 0; k < nLocks_; ++k) {
        const Lock& l = locks_[k];
        if (l.kind != SceneMorph::Send)
            norm_[l.param] = Scenes::morph(l.param, along(l.a0, l.a, clock_[0].at), along(l.b0, l.b, clock_[1].at), x_);
    }
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
        const int param = kModTargetParam[dst];
        const ParamSpec& spec = PARAM_SPECS[param];
        const float logRatio = spec.curve == Curve::Log ? static_cast<float>(std::log2(static_cast<double>(spec.hi) / spec.lo)) : 0.0f;
        slots_[nSlots_++] = {src, param, amount, logRatio};
    }
    env_.set(base_.envAttackS, base_.envReleaseS, base_.envGainDb);
    patch_ = base_;   // modulate() rewrites the fields the matrix reaches, every chunk
}

void Engine::morph(bool fast) {
    bool rerouted = false;
    const float atA = clock_[0].at, atB = clock_[1].at;
    for (int k = 0; k < nLocks_; ++k) {
        const Lock& l = locks_[k];
        // patch_ too: it is base_ as of the last routes(), the matrix only rewrites its own targets.
        if (l.kind == SceneMorph::Send) {
            if (l.module >= 0)
                base_.send[static_cast<size_t>(l.module)] = patch_.send[static_cast<size_t>(l.module)] =
                    Scenes::send(l.a, l.b, x_);
            continue;
        }
        const float n = Scenes::morph(l.param, along(l.a0, l.a, atA), along(l.b0, l.b, atB), x_);
        if (n == norm_[l.param]) continue;
        norm_[l.param] = n;
        // While a move runs, a log curve (a cutoff sweeping) by the fast exp2, as modulate() does; at
        // rest the exact value.
        const float v = fast && l.logRatio != 0.0f ? PARAM_SPECS[l.param].lo * exp2Fast(n * l.logRatio) : paramValue(l.param, n);
        setField(base_, l.param, v);
        setField(patch_, l.param, v);
        rerouted = rerouted || (l.param >= P_M1_SRC && l.param <= P_M8_AMT) || l.param == P_ENV_ATT ||
                   l.param == P_ENV_REL || l.param == P_ENV_GAIN;
    }
    if (rerouted) routes();   // a scene moves the matrix or the envelope follower
}

void Engine::modulate() {
    int done[kNumModSlots];
    int nDone = 0;
    for (int s = 0; s < nSlots_; ++s) {
        const int param = slots_[s].param;
        if (std::find(done, done + nDone, param) != done + nDone) continue;
        done[nDone++] = param;
        float sum = 0.0f;   // every slot on this target
        for (int k = s; k < nSlots_; ++k)
            if (slots_[k].param == param) sum += slots_[k].amount * src_[slots_[k].src];
        // A log curve (frequencies, times) by the fast exp2 (paramValue's powf to float rounding:
        // a libm call per target and chunk otherwise).
        const float logRatio = slots_[s].logRatio;
        const auto value = [param, logRatio](float x) {
            return logRatio != 0.0f ? PARAM_SPECS[param].lo * exp2Fast(x * logRatio) : paramValue(param, x);
        };
        const float n = std::clamp(norm_[param] + sum, 0.0f, 1.0f);
        const float v = value(n);
        setField(patch_, param, v);
        // A synced delay has no time of its own: the knob's move scales the synced time instead.
        if (param == P_DLY_TIME && base_.delay.sync) {
            const float baseV = value(norm_[P_DLY_TIME]);
            patch_.delay.divBeats = baseV > 0.0f ? base_.delay.divBeats * v / baseV : base_.delay.divBeats;
        }
    }
}

bool Engine::tick(double grid, double beats) {
    bool moved = false;
    for (int side = 0; side < 2; ++side) {
        Clock& c = clock_[side];
        if (c.state == kMoveNone) continue;
        // The fader's target leaving the other end brings this end in; it is out once the target and the
        // fader as played are both back there.
        const float heard = side ? xTarget_ : 1.0f - xTarget_;
        const float played = side ? x_ : 1.0f - x_;
        const float was = c.at;
        if (c.state == kMoveIdle) {
            if (heard >= kHeard) {
                // On the grid's next line: a bar, or a beat for a move under a bar. A push up to a 16th
                // of the line late counts from that line.
                const double q = c.length < 4.0 ? 1.0 : 4.0;
                const double since = grid - std::floor(grid / q) * q;
                if (since < q / 16.0) {
                    c.state = kMoveRunning;
                    c.beats = since;
                } else {
                    c.state = kMoveArmed;
                    c.wait = q - since;
                }
            }
        } else if (heard < kHeard && played < kHeard) {
            c.state = kMoveIdle;   // fully out: ready for the next push
            c.wait = c.beats = 0.0;
        }
        if (c.state == kMoveArmed && c.wait <= 0.0) {
            c.state = kMoveRunning;
            c.beats = -c.wait;
        }
        if (c.state == kMoveRunning && c.play == kMoveOnce && c.beats >= c.length) c.state = kMoveDone;
        c.at = c.state == kMoveRunning ? moveAt(c.beats, c.length, c.play) : c.state == kMoveDone ? 1.0f : 0.0f;
        // On to the next chunk: the counts go by beats played, so a locate or a loop point doesn't move them.
        if (c.state == kMoveArmed) c.wait -= beats;
        else if (c.state == kMoveRunning) c.beats += beats;
        moved = moved || c.at != was;
    }
    return moved;
}

Engine::MoveStatus Engine::moveStatus(int side) const {
    MoveStatus s;
    if (side < 0 || side > 1) return s;
    const Clock& c = clock_[side];
    s.state = c.state;
    s.beats = c.state == kMoveArmed ? c.wait : c.beats;
    s.length = c.length;
    s.play = c.play;
    return s;
}

void Engine::render(float* L, float* R, int n, Transport t) {
    // A lock changed (UI thread); not halfway through a load (held), which counts as one change at its end.
    if (scenes_ && scenes_->generation() != sceneGen_ && haveRaw_ && !scenes_->held()) rebuild();
    xFresh_ = false;
    for (int pos = 0; pos < n; pos += kChunk) {
        const int m = std::min(kChunk, n - pos);
        const double beats = m / static_cast<double>(kRate) * t.bpm / 60.0;
        // The grid the moves start on: MPC's song position while it plays; stopped, it runs on at the tempo.
        const double grid = t.valid && t.playing && std::isfinite(t.beats) ? t.beats : freeBeat_;
        freeBeat_ = grid + beats;
        const bool glide = x_ != xTarget_;
        if (glide) {   // the crossfader glides; the scenes' parameters follow it
            x_ += (xTarget_ - x_) * kFaderCoef;
            if (std::fabs(xTarget_ - x_) < 1e-4f) x_ = xTarget_;
        }
        const bool moved = tick(grid, beats);
        if (glide || moved) morph(clock_[0].state == kMoveRunning || clock_[1].state == kMoveRunning);
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
        t.beats += beats;
    }
}

} // namespace ef
