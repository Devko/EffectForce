// The performance layer (docs/DESIGN.md "Performance: scenes and the looper"): which parameters a
// scene can lock and how each kind moves with the fader; the engine following the fader (its glide,
// a module's send, a tail ringing out, a module resting); then through the plugin as MPC drives it:
// EDIT A / B, the scene tiles, CLEAR, the state with its scene lines, the sound at each end, the
// fader's bar, and the looper armed, played and moved by a scene.
#include "host.h"
#include "../plugin/engine.h"

#include <cstdio>
#include <memory>

namespace {

using namespace eft;
using namespace ef;

float normOf(int id, float value) { return paramNorm(id, value); }
float optionNorm(int id, const char* name) {
    for (int o = 0; o < PARAM_INFO[id].nopts; ++o)
        if (!std::strcmp(PARAM_INFO[id].opts[o], name)) return paramNorm(id, static_cast<float>(o));
    return -1.0f;
}

struct Knobs {
    float v[P_COUNT];
    Knobs() {
        for (int i = 0; i < P_COUNT; ++i) v[i] = PARAM_INFO[i].def;
    }
};

int blockAt(const Buf& b, size_t pos) { return static_cast<int>(std::min<size_t>(kBlock, b.size() - pos)); }

// Renders `seconds` of silence through the engine.
void play(Engine& e, double seconds) {
    Buf L(static_cast<size_t>(seconds * kRate), 0.0f), R = L;
    Transport t;
    for (size_t pos = 0; pos < L.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, L.size() - pos));
        e.render(&L[pos], &R[pos], n, t);
    }
}

// A song for the moves: MPC's transport, rendered in its blocks up to a beat (silence in).
struct Song {
    Engine& e;
    Transport t;
    explicit Song(Engine& eng) : e(eng) {
        t.bpm = 120.0;
        t.valid = t.playing = true;
    }
    double beat() const { return t.beats; }
    // On to beat b (while stopped, the same time at the tempo), one block at a time.
    void to(double b) {
        float L[kBlock] = {}, R[kBlock] = {};
        double at = t.beats;
        while (at < b - 1e-9) {
            const int n = static_cast<int>(std::min<double>(kBlock, std::ceil((b - at) * 60.0 / t.bpm * kRate - 1e-6)));
            e.render(L, R, std::max(n, 1), t);
            const double d = std::max(n, 1) / static_cast<double>(kRate) * t.bpm / 60.0;
            at += d;
            if (t.playing) t.beats += d;
        }
        if (!t.playing) t.beats = b;   // stopped: the beat only as our clock (MPC's position doesn't move)
    }
    void wait(double beats) { to(t.beats + beats); }
};

// The cutoff the engine plays now, as the knob's 0..1.
float cutNorm(const Engine& e) { return paramNorm(P_FLT_CUT, e.patch().filter.cutoffHz); }

void classification() {
    std::printf("== scenes: what a scene can lock, and how it moves\n");
    CHECK(kSceneMorph[P_XFADE] == SceneMorph::None && kSceneMorph[P_SCENE_A] == SceneMorph::None &&
          kSceneMorph[P_SCENE_B] == SceneMorph::None && kSceneMorph[P_LP_ON] == SceneMorph::None);
    CHECK(kSceneMorph[P_ORDER_1] == SceneMorph::None && kSceneMorph[P_EDIT_A] == SceneMorph::None &&
          kSceneMorph[P_PRESET] == SceneMorph::None && kSceneMorph[P_STATUS] == SceneMorph::None);
    for (int m = 0; m < kNumModules; ++m) CHECK(kSceneMorph[kModuleOnParam[m]] == SceneMorph::Send);
    CHECK(kSceneMorph[P_FLT_CUT] == SceneMorph::Line && kSceneMorph[P_MAC_1] == SceneMorph::Line &&
          kSceneMorph[P_LP_MIX] == SceneMorph::Line && kSceneMorph[P_LP_SPEED] == SceneMorph::Line);
    CHECK(kSceneMorph[P_DLY_DIV] == SceneMorph::Line && kSceneMorph[P_LP_LEN] == SceneMorph::Line &&
          kSceneMorph[P_LP_REP] == SceneMorph::Line);   // ordered steps walk in order
    CHECK(kSceneMorph[P_FLT_TYPE] == SceneMorph::Switch && kSceneMorph[P_REV_FREEZE] == SceneMorph::Switch &&
          kSceneMorph[P_DLY_SYNC] == SceneMorph::Switch && kSceneMorph[P_M1_DST] == SceneMorph::Switch);

    CHECK(std::fabs(Scenes::morph(P_FLT_CUT, 0.2f, 0.8f, 0.25f) - 0.35f) < 1e-6f);
    CHECK(Scenes::morph(P_FLT_TYPE, 0.2f, 0.8f, 0.49f) == 0.2f && Scenes::morph(P_FLT_TYPE, 0.2f, 0.8f, 0.5f) == 0.8f);
    CHECK(Scenes::send(0.0f, 1.0f, 0.3f) == 0.3f && Scenes::send(1.0f, 0.0f, 0.3f) == 0.7f);
    CHECK(Scenes::morph(P_FLT_CUT, 0.2f, 0.8f, std::nanf("")) == 0.2f);   // a NaN fader is A

    // The steps of an ordered choice come one after the other as the fader moves.
    const int last = PARAM_INFO[P_LP_REP].nopts - 1;
    int prev = 0;
    bool walks = true;
    for (int k = 0; k <= 100; ++k) {
        const int step = static_cast<int>(paramValue(P_LP_REP, Scenes::morph(P_LP_REP, 0.0f, 1.0f, k / 100.0f)));
        walks = walks && (step == prev || step == prev + 1);
        prev = step;
    }
    CHECK(walks && prev == last);

    Scenes s;
    s.lock(0, P_XFADE, 0.5f);   // not lockable: ignored
    s.lock(9, P_FLT_CUT, 0.5f);  // no such scene
    s.lock(2, P_FLT_CUT, 1.7f);  // clamped
    CHECK(s.count(0) == 0 && s.value(2, P_FLT_CUT) == 1.0f && s.count(2) == 1);
    const uint32_t g = s.generation();
    s.unlock(2, P_FLT_CUT);
    CHECK(s.count(2) == 0 && s.generation() != g);
}

void engineFollowsTheFader() {
    std::printf("== scenes: the engine follows the fader\n");
    Scenes sc;
    sc.lock(1, P_FLT_CUT, 0.2f);                                    // scene 2 (B by default): a low cutoff
    sc.lock(1, P_FLT_TYPE, optionNorm(P_FLT_TYPE, "HP 24"));
    Engine e;
    e.attachScenes(&sc);
    Knobs k;
    k.v[P_FLT_ON] = 1.0f;
    e.setParams(k.v);
    play(e, 0.05);
    CHECK(std::fabs(e.patch().filter.cutoffHz - 1000.0f) < 0.5f && e.patch().filter.type == 1);   // at A: the knobs
    CHECK(e.movingParams() == 2);
    // The fader to B: it glides (15 ms), then plays B.
    k.v[P_XFADE] = 1.0f;
    e.setParams(k.v);
    play(e, 128.0 / kRate);
    const float early = e.fader();
    CHECK(early > 0.1f && early < 0.3f);   // 1 - exp(-128 / (0.015 x 44100)) = 0.18
    play(e, 0.3);
    CHECK(e.fader() == 1.0f);
    CHECK(std::fabs(e.patch().filter.cutoffHz - paramValue(P_FLT_CUT, 0.2f)) < 0.01f && e.patch().filter.type == 3);
    // Halfway: the cutoff halfway in the knob's (log) space; the type already B's.
    k.v[P_XFADE] = 0.5f;
    e.setParams(k.v);
    play(e, 0.3);
    const float mid = paramValue(P_FLT_CUT, 0.5f * (PARAM_INFO[P_FLT_CUT].def + 0.2f));
    CHECK(std::fabs(e.patch().filter.cutoffHz - mid) < 0.5f && e.patch().filter.type == 3);
    // Scene B picked elsewhere (scene 3, no locks): nothing moves.
    k.v[P_SCENE_B] = 2.0f / 7.0f;
    e.setParams(k.v);
    play(e, 0.05);
    CHECK(e.movingParams() == 0 && std::fabs(e.patch().filter.cutoffHz - 1000.0f) < 0.5f);
    // Back to scene 2, and a lock changes on the UI thread: the next block has it.
    k.v[P_SCENE_B] = 1.0f / 7.0f;
    k.v[P_XFADE] = 1.0f;
    e.setParams(k.v);
    play(e, 0.3);
    sc.lock(1, P_FLT_CUT, 0.6f);
    play(e, 128.0 / kRate);
    CHECK(std::fabs(e.patch().filter.cutoffHz - paramValue(P_FLT_CUT, 0.6f)) < 0.01f);
    // While a scene is edited the engine moves nothing (the knobs show the scene).
    sc.setEditing(true);
    play(e, 128.0 / kRate);
    CHECK(e.movingParams() == 0 && std::fabs(e.patch().filter.cutoffHz - 1000.0f) < 0.5f);
    sc.setEditing(false);
    play(e, 128.0 / kRate);
    CHECK(e.movingParams() == 2);
    // Modulation adds to the moved value: Macro 1 -> cutoff, +50%.
    k.v[P_M1_SRC] = optionNorm(P_M1_SRC, "Macro 1");
    k.v[P_M1_DST] = optionNorm(P_M1_DST, "Filter Cutoff");
    k.v[P_M1_AMT] = normOf(P_M1_AMT, 0.2f);
    k.v[P_MAC_1] = 1.0f;
    e.setParams(k.v);
    play(e, 0.05);
    CHECK(std::fabs(e.current().filter.cutoffHz - paramValue(P_FLT_CUT, 0.8f)) < 0.05f);
    // The fader moving while the matrix is busy elsewhere: what the scene moves still follows it.
    k.v[P_M1_DST] = optionNorm(P_M1_DST, "Reverb Mix");
    k.v[P_XFADE] = 0.0f;
    e.setParams(k.v);
    play(e, 0.3);
    CHECK(std::fabs(e.current().filter.cutoffHz - 1000.0f) < 0.5f);
    k.v[P_XFADE] = 1.0f;
    e.setParams(k.v);   // the fader alone: no rebuild, the chunks glide
    play(e, 0.3);
    CHECK(std::fabs(e.current().filter.cutoffHz - paramValue(P_FLT_CUT, 0.6f)) < 0.01f && e.current().filter.type == 3);
    k.v[P_M1_DST] = optionNorm(P_M1_DST, "Filter Cutoff");
    e.setParams(k.v);
    play(e, 0.05);
    // A scene moving the matrix: B locks the amount to 0.
    sc.lock(1, P_M1_AMT, normOf(P_M1_AMT, 0.0f));
    play(e, 0.05);
    CHECK(std::fabs(e.current().filter.cutoffHz - paramValue(P_FLT_CUT, 0.6f)) < 0.05f);
}

// A delay switched on by scene B: x = 0 passes the input bit for bit, x = 1 is the delay, and the
// tail rings out when the fader goes back.
void sends() {
    std::printf("== scenes: a module switched by the fader is a send\n");
    Scenes sc;
    sc.lock(1, P_DLY_ON, 1.0f);
    Knobs k;
    k.v[P_DLY_FB] = normOf(P_DLY_FB, 0.6f);
    k.v[P_DLY_MIX] = normOf(P_DLY_MIX, 0.5f);
    const Buf in = whiteNoise(44100, 0.3f, 5);
    {   // at A: the input, bit for bit (the delay rests after its first silent tail)
        Engine e;
        e.attachScenes(&sc);
        e.setParams(k.v);
        Buf L = in, R = in;
        Transport t;
        for (size_t pos = 0; pos < L.size(); pos += kBlock) e.render(&L[pos], &R[pos], blockAt(L, pos), t);
        bool same = true;
        for (size_t i = 0; i < L.size(); ++i) same = same && L[i] == in[i];
        CHECK(same);
    }
    {   // at B (from the start): the same as the delay switched on by its knob
        Engine e, ref;
        e.attachScenes(&sc);
        Knobs kb = k;
        kb.v[P_XFADE] = 1.0f;
        e.setParams(kb.v);
        Knobs kr = k;
        kr.v[P_DLY_ON] = 1.0f;
        ref.setParams(kr.v);
        Buf L = in, R = in, A = in, B = in;
        Transport t;
        for (size_t pos = 0; pos < L.size(); pos += kBlock) {
            e.render(&L[pos], &R[pos], blockAt(L, pos), t);
            ref.render(&A[pos], &B[pos], blockAt(L, pos), t);
        }
        double worst = 0.0;
        for (size_t i = 0; i < L.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(L[i] - A[i])));
        std::printf("  at B vs the knob: worst difference %.2e\n", worst);
        CHECK(worst < 1e-5);
    }
    {   // a burst at B, then the fader to A: the echoes ring on; the knob's Off (no scenes) cuts them
        auto run = [&](bool byScene) {
            Engine e;
            if (byScene) e.attachScenes(&sc);
            Knobs kk = k;
            if (byScene) kk.v[P_XFADE] = 1.0f;
            else kk.v[P_DLY_ON] = 1.0f;
            e.setParams(kk.v);
            Buf L(static_cast<size_t>(kRate * 2), 0.0f), R = L;
            for (int i = 0; i < 2205; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = in[static_cast<size_t>(i)];
            Transport t;
            for (size_t pos = 0; pos < L.size(); pos += kBlock) {
                if (pos == 35 * kBlock) {   // 0.1 s: back to A / the knob off
                    if (byScene) kk.v[P_XFADE] = 0.0f;
                    else kk.v[P_DLY_ON] = 0.0f;
                    e.setParams(kk.v);
                }
                e.render(&L[pos], &R[pos], blockAt(L, pos), t);
            }
            return rms(L, static_cast<size_t>(kRate * 0.5), static_cast<size_t>(kRate * 1.5));
        };
        const double scene = run(true), knob = run(false);
        std::printf("  echoes 0.5..1.5 s after: by the fader %.1f dB, by the knob %.1f dB\n", db(scene), db(knob));
        CHECK(db(scene) > -60.0 && knob < 1e-6);
    }
    {   // at A for long: the delay rests (no CPU)
        Engine e;
        e.attachScenes(&sc);
        e.setParams(k.v);
        play(e, 0.2);
        CHECK(e.running() == 1);   // its tail time isn't over yet: it runs on silence
        play(e, 12.0);
        CHECK(e.running() == 0);
        // The fader up again: it runs.
        Knobs kb = k;
        kb.v[P_XFADE] = 0.4f;
        e.setParams(kb.v);
        play(e, 0.05);
        CHECK(e.running() == 1);
    }
}

void editThroughThePlugin() {
    std::printf("== scenes: EDIT A / B, the tiles and CLEAR, as the Force sends them\n");
    Host h;
    CHECK(h.display(P_SCA_1) == "1" && h.get(P_SCA_1) > 0.5f && h.get(P_SCB_1 + 1) > 0.5f && h.get(P_SCB_1) < 0.5f);
    CHECK(h.display(P_SCN_INFO).find("A: SCENE 1, CLEAN") == 0);
    h.on(P_FLT_ON);
    // EDIT B: the knobs show scene 2 (nothing locked: the knobs); what is set is locked in it.
    h.tap(P_EDIT_B);
    CHECK(h.get(P_EDIT_B) > 0.5f && h.get(P_EDIT_A) < 0.5f && h.display(P_SCN_INFO).find("EDIT B: SCENE 2") == 0);
    h.set(P_FLT_CUT, 300.0f);
    h.option(P_FLT_TYPE, "BP");
    CHECK(std::fabs(h.value(P_FLT_CUT) - 300.0f) < 1.0f);
    CHECK(h.display(P_SCB_1 + 1) == "2  (2)" && h.display(P_SCA_1 + 1) == "2  (2)");
    // Saved meanwhile: the knobs, not the scene, plus the scene's lines.
    std::string s = h.chunk();
    CHECK(s.find("\nflt_cut=999.998\n") != std::string::npos && s.find("\nflt_type=LP 24\n") != std::string::npos);
    CHECK(s.find("\nscene2.flt_cut=300\n") != std::string::npos && s.find("\nscene2.flt_type=BP\n") != std::string::npos);
    // EDIT B again: done; the knobs are back.
    h.tap(P_EDIT_B);
    CHECK(h.get(P_EDIT_B) < 0.5f && std::fabs(h.value(P_FLT_CUT) - 1000.0f) < 1.0f && h.value(P_FLT_TYPE) == 1.0f);
    // MPC hears about it: the knobs' values go back to it.
    h.log.automated.clear();
    h.blocks(12);
    CHECK(h.log.automated.count(P_FLT_CUT) == 1);

    // EDIT A, then a tap on B's scene 3 tile: B moves, the edit stays on A.
    h.tap(P_EDIT_A);
    h.set(P_REV_MIX, 0.6f);
    h.tap(P_SCB_1 + 2);
    CHECK(h.value(P_SCENE_B) == 2.0f && h.get(P_EDIT_A) > 0.5f && std::fabs(h.value(P_REV_MIX) - 0.6f) < 1e-3f);
    // A tap on A's scene 4 tile: the edit follows to scene 4 (no locks: the knobs).
    h.tap(P_SCA_1 + 3);
    CHECK(h.value(P_SCENE_A) == 3.0f && std::fabs(h.value(P_REV_MIX) - 0.3f) < 1e-3f);
    CHECK(h.display(P_SCA_1) == "1  (1)");   // scene 1 kept its lock
    h.set(P_DLY_FB, 0.8f);
    // From A to B straight away: scene 3 (B now) shows; scene 4 kept its lock.
    h.tap(P_EDIT_B);
    CHECK(h.get(P_EDIT_A) < 0.5f && h.get(P_EDIT_B) > 0.5f && std::fabs(h.value(P_DLY_FB) - 0.4f) < 1e-3f);
    CHECK(h.display(P_SCA_1 + 3) == "4  (1)");
    h.set(P_DLY_MIX, 0.9f);
    h.press(P_SCN_CLEAR);   // scene 3's locks go
    CHECK(h.display(P_SCB_1 + 2) == "3" && std::fabs(h.value(P_DLY_MIX) - 0.3f) < 1e-3f);
    h.tap(P_EDIT_B);
    CHECK(std::fabs(h.value(P_DLY_FB) - 0.4f) < 1e-3f && std::fabs(h.value(P_REV_MIX) - 0.3f) < 1e-3f);
    // CLEAR outside an edit does nothing.
    h.press(P_SCN_CLEAR);
    CHECK(h.display(P_SCA_1 + 3) == "4  (1)");

    // A scene picked by a Q-Link while it is edited: the edit follows.
    h.tap(P_EDIT_A);   // scene 4
    CHECK(std::fabs(h.value(P_DLY_FB) - 0.8f) < 1e-3f);
    {
        Turn turn;
        h.detent(P_SCENE_A, -1);   // to scene 3
    }
    CHECK(h.value(P_SCENE_A) == 2.0f && std::fabs(h.value(P_DLY_FB) - 0.4f) < 1e-3f);
    // A preset ends the edit and brings its own scenes (Init: none).
    h.press(P_PRE_INIT);
    CHECK(h.get(P_EDIT_A) < 0.5f && h.display(P_SCA_1 + 3) == "4" && h.display(P_SCB_1 + 1) == "2");
}

void stateLines() {
    std::printf("== scenes: saved and loaded as sceneN.key=value lines\n");
    Host a;
    CHECK(a.load("effectforce 1\nflt_on=On\nxfade=0.25\nscene_b=5\nscene5.flt_cut=200\nscene5.dly_on=On\n"
                 "scene1.lp_len=1/4\nscene9.flt_cut=300\nscene2.xfade=1\nscene2.nonsense=4\nscene2.flt_cut=abc\n") == 1);
    CHECK(a.display(P_SCB_1 + 4) == "5  (2)" && a.display(P_SCA_1) == "1  (1)");
    CHECK(a.get(P_SCB_1 + 4) > 0.5f && std::fabs(a.value(P_XFADE) - 0.25f) < 1e-4f);
    const std::string s = a.chunk();
    CHECK(s.find("\nscene5.flt_cut=200\n") != std::string::npos && s.find("\nscene5.dly_on=On\n") != std::string::npos &&
          s.find("\nscene1.lp_len=1/4\n") != std::string::npos);
    CHECK(s.find("scene9") == std::string::npos && s.find("scene2.") == std::string::npos);
    CHECK(s.find("edit_") == std::string::npos && s.find("sca_") == std::string::npos);   // the surface's own
    Host b;
    CHECK(b.load(s) == 1 && b.chunk() == s);
    // A project without scene lines has none.
    CHECK(b.load("effectforce 1\nflt_on=On\n") == 1 && b.display(P_SCB_1 + 4) == "5");
}

void soundAtEachEnd() {
    std::printf("== scenes: the sound at each end of the fader\n");
    Host h;
    h.on(P_FLT_ON);
    h.set(P_FLT_CUT, 20000.0f);   // A: open
    h.tap(P_EDIT_B);
    h.set(P_FLT_CUT, 150.0f);   // B: a low-pass at 150 Hz
    h.tap(P_EDIT_B);
    const Buf in = sine(2000.0, kBlocksPerSec * kBlock / 2, 0.5f);
    h.run(in);
    h.run(in);
    const double atA = magnitude(h.L, 2000.0, 4096);
    h.set(P_XFADE, 1.0f);
    h.run(in);
    h.run(in);
    const double atB = magnitude(h.L, 2000.0, 4096);
    std::printf("  2 kHz at A %.1f dB, at B %.1f dB\n", db(atA), db(atB));
    CHECK(db(atA) > -7.0 && db(atB) < -50.0 && h.finite);
    // The fader swept by a Q-Link (1/128 detents, as a Force sends them): no clicks on a low sine.
    Host q;
    q.on(P_FLT_ON);
    q.tap(P_EDIT_B);
    q.set(P_FLT_CUT, 100.0f);
    q.tap(P_EDIT_B);
    const Buf low = sine(80.0, kBlock * 260, 0.5f);
    Buf out;
    for (int k = 0; k < 260; ++k) {
        if (k >= 2 && k < 130) {
            Turn turn;
            q.detent(P_XFADE, 1);
        }
        q.run(Buf(low.begin() + k * kBlock, low.begin() + (k + 1) * kBlock));
        out.insert(out.end(), q.L.begin(), q.L.end());
    }
    CHECK(maxStep(out) < 1.5f * maxStep(low) && q.finite);
    CHECK(q.value(P_XFADE) > 0.99f);
}

void faderBar() {
    std::printf("== scenes: the fader's bar\n");
    Host h;
    CHECK(h.display(P_XF_BAR) == "A  |------------------------  B");
    h.set(P_XFADE, 0.5f);
    CHECK(h.display(P_XF_BAR) == "A  ============|------------  B");
    h.set(P_XFADE, 1.0f);
    CHECK(h.display(P_XF_BAR) == "A  ========================|  B");
    // A move of the fader asks MPC to redraw the texts.
    h.blocks(8);
    const int before = h.log.updates;
    h.set(P_XFADE, 0.3f);
    h.blocks(8);
    CHECK(h.log.updates > before);
}

// The ramp's sample index (x 1e-6), as looper_test.cpp uses it.
Buf ramp(size_t n, size_t from = 0) {
    Buf x(n);
    for (size_t i = 0; i < n; ++i) x[i] = static_cast<float>((from + i) * 1e-6);
    return x;
}

void looperInThePlugin() {
    std::printf("== looper: armed, played and rolled by a scene, through the plugin\n");
    Host h;
    h.play(true);
    h.on(P_LP_ON);   // arms it: the buffers are made now
    h.option(P_LP_LEN, "1 bar");
    // Scene B: the loop up, rolling 1/4.
    h.tap(P_EDIT_B);
    h.set(P_LP_MIX, 1.0f);
    h.option(P_LP_REP, "1/4");
    h.tap(P_EDIT_B);
    const size_t n = 44100 * 6;
    const Buf in = ramp(n);
    Buf out;
    for (size_t pos = 0; pos < n; pos += 44100) {
        if (pos == 44100 * 3) h.set(P_XFADE, 1.0f);   // beat 6: the fader to B
        h.run(Buf(in.begin() + static_cast<long>(pos), in.begin() + static_cast<long>(pos + 44100)));
        out.insert(out.end(), h.L.begin(), h.L.end());
    }
    CHECK(h.finite);
    // Before: the input. After: the beat the head was in (beat 3) of bar 1 (the last whole bar
    // before the fader, frames 0..88200), over and over.
    bool live = true;
    for (size_t i = 0; i < 44100 * 3; i += 7) live = live && out[i] == in[i];
    CHECK(live);
    double lo = 1e9, hi = -1e9;
    for (size_t i = 44100 * 4; i < n; ++i) {
        lo = std::min(lo, out[i] / 1e-6);
        hi = std::max(hi, out[i] / 1e-6);
    }
    std::printf("  rolled: frames %.0f .. %.0f\n", lo, hi);
    CHECK(lo >= 44100.0 - 1.0 && hi <= 66150.0 + 1.0);
    // Back to A: live again.
    h.set(P_XFADE, 0.0f);
    h.run(ramp(44100, n));
    bool back = true;
    for (size_t i = 13230; i < 44100; i += 7) back = back && h.L[i] == static_cast<float>((n + i) * 1e-6);   // after the glide
    CHECK(back);
    // Off: the input passes untouched, the fader or not.
    h.option(P_LP_ON, "Off");
    h.set(P_XFADE, 1.0f);
    h.run(ramp(44100, n));
    h.run(ramp(44100, n + 44100));
    bool passes = true;
    for (size_t i = 0; i < 44100; i += 7) passes = passes && h.L[i] == static_cast<float>((n + 44100 + i) * 1e-6);
    CHECK(passes);
}

// --- scene moves (docs/DESIGN.md "Scene moves") ------------------------------------------------

void moveStore() {
    std::printf("== moves: a lock's start, a scene's timing\n");
    Scenes s;
    s.lock(1, P_FLT_CUT, 0.2f);
    const uint32_t g = s.generation(), ep = s.moveEpoch(1);
    s.setStart(1, P_FLT_CUT, 0.8f);
    CHECK(s.start(1, P_FLT_CUT) == 0.8f && s.value(1, P_FLT_CUT) == 0.2f && s.moves(1));
    CHECK(s.generation() != g && s.moveEpoch(1) != ep && s.moveEpoch(2) == 0);
    s.setStart(1, P_FLT_TYPE, 0.5f);   // a switch can't move
    s.setStart(1, P_FLT_RES, 0.5f);    // nor a parameter the scene doesn't lock
    s.setStart(1, P_FLT_ON, 0.5f);     // nor a module's On
    CHECK(s.start(1, P_FLT_TYPE) < 0.0f && s.start(1, P_FLT_RES) < 0.0f && s.start(1, P_FLT_ON) < 0.0f);
    // Locked again by hand: it holds still (and the scene's other moves go on: no new epoch).
    s.lock(1, P_FLT_RES, 0.5f);
    s.setStart(1, P_FLT_RES, 0.9f);
    const uint32_t ep2 = s.moveEpoch(1);
    s.lock(1, P_FLT_CUT, 0.3f);
    CHECK(s.start(1, P_FLT_CUT) < 0.0f && s.moves(1) && s.moveEpoch(1) == ep2);
    // The timing: defaults, clamped, a change starts the move over.
    CHECK(s.moveLength(0) == Scenes::kDefaultLength && kMoveBeats[Scenes::kDefaultLength] == 16.0 && s.movePlay(0) == kMoveOnce);
    s.setMove(1, 99, -3);
    CHECK(s.moveLength(1) == kNumMoveLengths - 1 && s.movePlay(1) == kMoveOnce && s.moveEpoch(1) != ep2);
    // Unlocked or cleared: no start, and the timing back to its default.
    s.unlock(1, P_FLT_RES);
    CHECK(!s.moves(1) && s.start(1, P_FLT_RES) < 0.0f);
    s.setStart(1, P_FLT_CUT, 0.7f);
    s.clear(1);
    CHECK(!s.moves(1) && s.moveLength(1) == Scenes::kDefaultLength && s.count(1) == 0);
}

// Scene 2 (B) closes the cutoff from 0.8 to 0.2 (the knob's 0..1) over a bar.
struct MoveRig {
    Scenes sc;
    Engine e;
    Knobs k;
    explicit MoveRig(int length = 3, int play = kMoveOnce) {
        sc.lock(1, P_FLT_ON, 1.0f);
        sc.lock(1, P_FLT_CUT, 0.2f);
        sc.setStart(1, P_FLT_CUT, 0.8f);
        sc.setMove(1, length, play);
        e.attachScenes(&sc);
        e.setParams(k.v);
    }
    void fader(float x) {
        k.v[P_XFADE] = x;
        e.setParams(k.v);
    }
};

void moveClock() {
    std::printf("== moves: armed by the fader, on the bar line, for LENGTH\n");
    MoveRig r;   // 1 bar, Once
    Song s(r.e);
    s.to(9.0);   // bar 3, beat 2
    CHECK(r.e.moveStatus(1).state == kMoveIdle && r.e.moveStatus(0).state == kMoveNone);
    r.fader(1.0f);
    s.wait(0.5);   // the fader's glide over
    CHECK(r.e.moveStatus(1).state == kMoveArmed && std::fabs(cutNorm(r.e) - 0.8f) < 1e-3f);   // B's start
    s.to(11.98);
    CHECK(r.e.moveStatus(1).state == kMoveArmed && std::fabs(cutNorm(r.e) - 0.8f) < 1e-3f);   // not before bar 4
    // From beat 12: 0.8 down to 0.2 in a straight line over 4 beats.
    for (double at : {13.0, 14.0, 15.0}) {
        s.to(at);
        const float want = 0.8f - 0.6f * static_cast<float>((at - 12.0) / 4.0);
        CHECK(std::fabs(cutNorm(r.e) - want) < 2e-3f);
        if (std::fabs(cutNorm(r.e) - want) >= 2e-3f) std::printf("  beat %.1f: %.4f, want %.4f\n", at, cutNorm(r.e), want);
    }
    CHECK(r.e.moveStatus(1).state == kMoveRunning && std::fabs(r.e.moveStatus(1).beats - 3.0) < 0.01);
    s.to(16.02);
    CHECK(r.e.moveStatus(1).state == kMoveDone);   // held: the lock, exactly as a lock that holds still
    CHECK(std::fabs(r.e.patch().filter.cutoffHz - paramValue(P_FLT_CUT, 0.2f)) < 0.01f);
    s.to(20.0);
    CHECK(r.e.moveStatus(1).state == kMoveDone);
    // Back at A: it resets once the fader is there (after its glide); out again, it waits for the bar.
    r.fader(0.0f);
    s.wait(0.01);
    CHECK(r.e.moveStatus(1).state == kMoveDone);   // still gliding out
    s.wait(0.5);
    CHECK(r.e.moveStatus(1).state == kMoveIdle);
    s.to(21.0);
    r.fader(1.0f);
    s.to(23.9);
    CHECK(r.e.moveStatus(1).state == kMoveArmed);
    s.to(25.0);
    CHECK(std::fabs(cutNorm(r.e) - 0.65f) < 2e-3f);
    // A push a hair after the line counts from it.
    r.fader(0.0f);
    s.to(28.1);
    r.fader(1.0f);
    s.wait(0.01);
    CHECK(r.e.moveStatus(1).state == kMoveRunning);
    s.to(29.0);
    CHECK(std::fabs(cutNorm(r.e) - 0.65f) < 2e-3f);
    // A fader resting a hair off A doesn't arm it; one Q-Link detent does.
    r.fader(0.0f);
    s.wait(1.0);
    r.fader(0.003f);
    s.wait(1.0);
    CHECK(r.e.moveStatus(1).state == kMoveIdle);
    r.fader(1.0f / 128.0f);
    s.wait(0.01);
    CHECK(r.e.moveStatus(1).state == kMoveArmed || r.e.moveStatus(1).state == kMoveRunning);

    std::printf("== moves: Loop, Ping-pong, a beat's move, a new scene\n");
    {
        MoveRig x(3, kMoveLoop);
        Song sx(x.e);
        x.fader(1.0f);   // at beat 0: on the line, so it runs from 0
        sx.wait(0.01);
        CHECK(x.e.moveStatus(1).state == kMoveRunning);
        sx.to(4.5);   // the second pass, an eighth in
        CHECK(std::fabs(cutNorm(x.e) - (0.8f - 0.6f * 0.125f)) < 2e-3f && x.e.moveStatus(1).state == kMoveRunning);
    }
    {
        MoveRig x(3, kMovePingPong);
        Song sx(x.e);
        x.fader(1.0f);
        sx.to(6.0);   // back half way
        CHECK(std::fabs(cutNorm(x.e) - 0.5f) < 2e-3f);
        sx.to(8.0);   // back at the start
        CHECK(std::fabs(cutNorm(x.e) - 0.8f) < 2e-3f);
        sx.to(10.0);
        CHECK(std::fabs(cutNorm(x.e) - 0.5f) < 2e-3f);
    }
    {
        MoveRig x(1, kMoveOnce);   // 1 beat: on the next beat
        Song sx(x.e);
        sx.to(2.3);
        x.fader(1.0f);
        sx.to(2.99);
        CHECK(x.e.moveStatus(1).state == kMoveArmed);
        sx.to(3.5);
        CHECK(std::fabs(cutNorm(x.e) - 0.5f) < 3e-3f);
        sx.to(4.1);
        CHECK(x.e.moveStatus(1).state == kMoveDone);
        // Another scene at B: its clock (scene 3 has no move); back to scene 2: armed again.
        x.k.v[P_SCENE_B] = 2.0f / 7.0f;
        x.e.setParams(x.k.v);
        sx.wait(0.1);
        CHECK(x.e.moveStatus(1).state == kMoveNone);
        x.k.v[P_SCENE_B] = 1.0f / 7.0f;
        x.e.setParams(x.k.v);
        sx.wait(0.01);
        CHECK(x.e.moveStatus(1).state == kMoveArmed && std::fabs(cutNorm(x.e) - 0.8f) < 1e-3f);
        // The move changed on the UI thread (a new LENGTH): it starts over, on the bar this time.
        sx.to(6.0);
        x.sc.setMove(1, 3, kMoveOnce);
        sx.wait(0.01);
        CHECK(x.e.moveStatus(1).state == kMoveArmed && std::fabs(x.e.moveStatus(1).beats - 2.0) < 0.02);
        // LENGTH Off: the scene holds its lock, no clock.
        x.sc.setMove(1, 0, kMoveOnce);
        sx.wait(0.01);
        CHECK(x.e.moveStatus(1).state == kMoveNone && std::fabs(cutNorm(x.e) - 0.2f) < 1e-4f);
    }

    std::printf("== moves: the tempo bends a move, a locate doesn't, stopped it runs on\n");
    {
        MoveRig x;
        Song sx(x.e);
        x.fader(1.0f);
        sx.to(2.0);   // half way
        sx.t.bpm = 60.0;
        sx.to(3.0);   // a beat at 60: a second
        CHECK(std::fabs(cutNorm(x.e) - 0.35f) < 2e-3f);
        sx.t.beats = 0.0;   // the sequence loops back to its start
        sx.to(0.5);
        CHECK(std::fabs(cutNorm(x.e) - (0.8f - 0.6f * 3.5f / 4.0f)) < 2e-3f);
    }
    {
        MoveRig x;
        Song sx(x.e);
        sx.t.playing = false;   // MPC stopped: the grid runs on from where the engine began
        sx.to(1.0);
        x.fader(1.0f);
        sx.to(3.9);
        CHECK(x.e.moveStatus(1).state == kMoveArmed);
        sx.to(5.0);
        CHECK(std::fabs(cutNorm(x.e) - 0.65f) < 2e-3f);
    }
    {
        // Scene A moves too: at A from the start, its move runs from the first line; B's waits for the fader.
        MoveRig x;
        x.sc.lock(0, P_FLT_CUT, 0.9f);
        x.sc.setStart(0, P_FLT_CUT, 0.5f);
        x.sc.setMove(0, 3, kMoveOnce);
        Song sx(x.e);
        sx.to(2.0);
        CHECK(x.e.moveStatus(0).state == kMoveRunning && x.e.moveStatus(1).state == kMoveIdle);
        CHECK(std::fabs(cutNorm(x.e) - 0.7f) < 2e-3f);
        // Out to B: A resets once it is fully out.
        x.fader(1.0f);
        sx.wait(0.5);
        CHECK(x.e.moveStatus(0).state == kMoveIdle && x.e.moveStatus(1).state != kMoveIdle);
    }
}

void moveValues() {
    std::printf("== moves: half way on the fader, steps in order, a lock equal to the other end\n");
    {
        // A fresh engine given only the fader (a project saved at B): B's values before anything plays.
        Scenes sc;
        sc.lock(1, P_FLT_CUT, 0.2f);
        Engine e;
        e.attachScenes(&sc);
        Knobs k;
        k.v[P_XFADE] = 1.0f;
        e.setParams(k.v);
        CHECK(std::fabs(e.patch().filter.cutoffHz - paramValue(P_FLT_CUT, 0.2f)) < 0.01f);
    }
    {
        MoveRig x;
        Song sx(x.e);
        x.fader(0.5f);
        sx.to(2.0);   // B's move half way (0.5); A plays the knob
        const float knob = PARAM_INFO[P_FLT_CUT].def;
        CHECK(std::fabs(cutNorm(x.e) - (knob + (0.5f - knob) * 0.5f)) < 2e-3f);
    }
    {
        // Repeat 1/4 -> 1/32 over a bar: every step in order, none skipped.
        Scenes sc;
        sc.lock(1, P_LP_REP, optionNorm(P_LP_REP, "1/32"));
        sc.setStart(1, P_LP_REP, optionNorm(P_LP_REP, "1/4"));
        sc.setMove(1, 3, kMoveOnce);
        Engine e;
        e.attachScenes(&sc);
        Knobs k;
        k.v[P_XFADE] = 1.0f;
        e.setParams(k.v);
        Song s(e);
        int prev = -1, seen = 0;
        bool order = true;
        for (double b = 0.01; b < 4.5; b += 0.05) {   // from the first block on (the patch before it is A's)
            s.to(b);
            const double rep = e.patch().looper.repeatBeats;   // 1/4 is a beat, 1/32 an eighth of one
            const int idx = rep >= 1.0 - 1e-6 ? 2 : rep >= 0.5 - 1e-6 ? 3 : rep >= 0.25 - 1e-6 ? 4 : 5;
            if (idx != prev) {
                order = order && (prev < 0 || idx == prev + 1);
                prev = idx;
                ++seen;
            }
        }
        CHECK(order && seen == 4 && prev == 5);
    }
    {
        // B's lock equals the knob (A); only its start differs: it still moves.
        Scenes sc;
        Knobs k;
        k.v[P_FLT_ON] = 1.0f;
        k.v[P_FLT_CUT] = 0.2f;
        k.v[P_XFADE] = 1.0f;
        sc.lock(1, P_FLT_CUT, 0.2f);
        sc.setStart(1, P_FLT_CUT, 0.8f);
        sc.setMove(1, 3, kMoveOnce);
        Engine e;
        e.attachScenes(&sc);
        e.setParams(k.v);
        CHECK(e.movingParams() == 1);
        Song s(e);
        s.to(2.0);
        CHECK(std::fabs(cutNorm(e) - 0.5f) < 2e-3f);
    }
    {
        // A move never steps further per chunk than its line (no jumps), at any tempo.
        MoveRig x(3, kMovePingPong);
        Song sx(x.e);
        sx.t.bpm = 177.0;
        x.fader(1.0f);
        float L[kChunk] = {}, R[kChunk] = {};
        const auto chunk = [&] {   // exactly one chunk
            x.e.render(L, R, kChunk, sx.t);
            sx.t.beats += kChunk / static_cast<double>(kRate) * sx.t.bpm / 60.0;
        };
        chunk();   // the first chunk plays B
        float prev = cutNorm(x.e), worst = 0.0f;
        for (int k = 0; k < 4000; ++k) {
            chunk();
            const float n = cutNorm(x.e);
            worst = std::max(worst, std::fabs(n - prev));
            prev = n;
        }
        const float line = 0.6f / 4.0f * static_cast<float>(kChunk / static_cast<double>(kRate) * 177.0 / 60.0);
        CHECK(worst < 1.5f * line + 1e-5f);
    }
}

void movesThroughThePlugin() {
    std::printf("== moves: saved and loaded, LENGTH and PLAY, an edit, the scenes' line\n");
    Host h;
    h.play(true);
    CHECK(h.load("effectforce 1\nscene_b=2\nscene2.name=Sweep\nscene2.move=1 bar\nscene2.play=Ping-pong\nscene2.flt_on=On\n"
                 "scene2.flt_cut=18000>150\nscene2.flt_type=LP 24>BP\nscene3.move=7 bars\nscene3.flt_res=0.1>0.2>0.3\n"
                 "scene4.flt_res=x>0.5\n") == 1);
    std::string s = h.chunk();
    CHECK(s.find("\nscene2.move=1 bar\nscene2.play=Ping-pong\n") != std::string::npos);
    CHECK(s.find("\nscene2.flt_cut=18000>150\n") != std::string::npos && s.find("scene2.flt_type") == std::string::npos);
    CHECK(s.find("scene3.") == std::string::npos && s.find("scene4.") == std::string::npos);   // nothing valid there
    CHECK(s.find("mv_len") == std::string::npos);   // the surface's own, not a knob
    Host b;
    CHECK(b.load(s) == 1 && b.chunk() == s);
    // LENGTH and PLAY show and set scene B's.
    CHECK(h.display(P_MV_LEN) == "1 bar" && h.display(P_MV_PLAY) == "Ping-pong");
    h.option(P_MV_LEN, "2 bars");
    h.option(P_MV_PLAY, "Once");
    s = h.chunk();
    CHECK(s.find("\nscene2.move=2 bars\nscene2.play=Once\n") != std::string::npos && s.find("scene2.name=Sweep*") != std::string::npos);
    h.tap(P_SCB_1 + 2);   // scene 3 at B: its own (the default)
    CHECK(h.display(P_MV_LEN) == "4 bars" && h.display(P_MV_PLAY) == "Once");
    h.tap(P_SCB_1 + 1);
    CHECK(h.display(P_MV_LEN) == "2 bars");
    // The scenes' line follows the move: armed, its bars, held.
    h.blocks(4);
    CHECK(h.display(P_SCN_INFO).find("B: SCENE 2 SWEEP*") != std::string::npos);
    h.silence(static_cast<int>(1.25 * 22050) / kBlock * kBlock);   // beat 1.25
    h.set(P_XFADE, 1.0f);
    h.blocks(4);
    CHECK(h.display(P_SCN_INFO).find("SWEEP*, MOVES ON THE BAR") != std::string::npos);
    const int before = h.log.updates;
    h.silence(3 * 22050);   // beat ~4.3: bar 1 of 2
    CHECK(h.display(P_SCN_INFO).find("SWEEP*, BAR 1 OF 2") != std::string::npos && h.log.updates > before);
    h.silence(4 * 22050);   // beat ~8.3
    CHECK(h.display(P_SCN_INFO).find("SWEEP*, BAR 2 OF 2") != std::string::npos);
    h.silence(4 * 22050);   // beat ~12.3: done
    CHECK(h.display(P_SCN_INFO).find("SWEEP*, HELD") != std::string::npos);
    h.set(P_XFADE, 0.0f);
    h.blocks(40);
    CHECK(h.display(P_SCN_INFO).find("SWEEP*     ") == std::string::npos && h.display(P_SCN_INFO).find(", HELD") == std::string::npos);
    // EDIT B: the knob shows the lock (the end); turned, it holds still (no move left: no timing lines).
    h.tap(P_EDIT_B);
    CHECK(std::fabs(h.value(P_FLT_CUT) - 150.0f) < 1.0f);
    CHECK(h.display(P_MV_LEN) == "2 bars");   // the edited scene's
    h.set(P_FLT_CUT, 300.0f);
    h.tap(P_EDIT_B);
    s = h.chunk();
    CHECK(s.find("\nscene2.flt_cut=300\n") != std::string::npos && s.find("scene2.move") == std::string::npos);
}

} // namespace

void eft::scenesTests() {
    classification();
    engineFollowsTheFader();
    sends();
    editThroughThePlugin();
    stateLines();
    soundAtEachEnd();
    faderBar();
    looperInThePlugin();
    moveStore();
    moveClock();
    moveValues();
    movesThroughThePlugin();
}
