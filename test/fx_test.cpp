// The Octatrack-style workflow through the plugin, as a Force drives it: the FX library (banks of
// effects as tiles; a tap puts one in scene B, or in the scene being edited; names; an edit marks it;
// an effect of the looper arms it; nothing of the effect before stays), every effect played through
// the fader, and the looper's REC (Capture Next on the grid, the loop kept, its line on the page).
#include "host.h"
#include "fx_library.h"
#include "../plugin/scenes.h"

#include <cstdio>
#include <string>

namespace {

using namespace eft;
using namespace ef;

int findFx(const char* name) {
    for (int k = 0; k < kNumFx; ++k)
        if (!std::strcmp(kFxLibrary[k].name, name)) return k;
    return -1;
}

// Taps through the banks until `name`'s bank shows, then its tile.
void pickFx(Host& h, const char* name) {
    const int k = findFx(name);
    for (int guard = 0; guard < 8; ++guard) {
        for (int t = 0; t < kFxPerBank; ++t)
            if (h.display(P_FX_1 + t) == [&] {
                    std::string u = kFxLibrary[k].name;
                    for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    return u;
                }()) {
                h.tap(P_FX_1 + t);
                return;
            }
        h.press(P_FX_NEXT);
    }
    CHECK(false);   // not found
}

void library() {
    std::printf("== fx: the library, its banks and tiles\n");
    CHECK(kNumFx == 64 && kFxPerBank == 16);
    Host h;
    CHECK(h.display(P_FX_1) == "LP SWEEP" && h.display(P_FX_1 + 15) == "KILL HIGHS");
    CHECK(h.display(P_FX_BANK).find("BANK 1 / 4: FILTER") == 0);
    h.press(P_FX_NEXT);
    CHECK(h.display(P_FX_1) == "HALL WASH" && h.display(P_FX_BANK).find("BANK 2 / 4: SPACE") == 0);
    h.press(P_FX_PREV);
    h.press(P_FX_PREV);   // round to the last bank
    CHECK(h.display(P_FX_BANK).find("BANK 4 / 4: RHYTHM") == 0 && h.display(P_FX_1) == "PUMP");
    for (int t = 0; t < kFxPerBank; ++t) CHECK(h.get(P_FX_1 + t) < 0.5f);   // nothing in scene B yet
}

void pickIntoSceneB() {
    std::printf("== fx: a tap puts the effect in scene B; another replaces it whole\n");
    Host h;
    pickFx(h, "Dub Echo");
    std::string s = h.chunk();
    CHECK(s.find("\nscene2.name=Dub Echo\n") != std::string::npos && s.find("\nscene2.dly_on=On\n") != std::string::npos &&
          s.find("\nscene2.dly_fb=0.75\n") != std::string::npos);
    CHECK(h.display(P_SCN_INFO).find("B: SCENE 2 DUB ECHO") != std::string::npos);
    // Its tile is lit; the knobs are untouched (the scene holds the effect).
    bool lit = false;
    for (int t = 0; t < kFxPerBank; ++t) lit = lit || (h.get(P_FX_1 + t) > 0.5f && h.display(P_FX_1 + t) == "DUB ECHO");
    CHECK(lit && h.value(P_DLY_ON) == 0.0f);
    // Another effect replaces it: nothing of the delay stays locked.
    pickFx(h, "Hall Wash");
    s = h.chunk();
    CHECK(s.find("scene2.dly_") == std::string::npos && s.find("\nscene2.rev_on=On\n") != std::string::npos &&
          s.find("\nscene2.name=Hall Wash\n") != std::string::npos);
    // Scene B picked elsewhere: the effect goes there.
    h.tap(P_SCB_1 + 4);
    pickFx(h, "Bit Crush");
    s = h.chunk();
    CHECK(s.find("\nscene5.name=Bit Crush\n") != std::string::npos && s.find("\nscene2.name=Hall Wash\n") != std::string::npos);
    // Saved and loaded with their names.
    Host b;
    CHECK(b.load(s) == 1 && b.chunk() == s);
    b.tap(P_SCB_1 + 1);
    CHECK(b.display(P_SCN_INFO).find("B: SCENE 2 HALL WASH") != std::string::npos);
}

void editMarksAndEditTarget() {
    std::printf("== fx: an edit marks the effect; while editing, a tap fills the edited scene\n");
    Host h;
    pickFx(h, "LP Sweep");
    h.tap(P_EDIT_B);
    CHECK(std::fabs(h.value(P_FLT_CUT) - 150.0f) < 1.0f);   // the knobs show scene B
    h.set(P_FLT_RES, 0.9f);
    CHECK(h.chunk().find("\nscene2.name=LP Sweep*\n") != std::string::npos);
    bool anyLit = false;
    for (int t = 0; t < kFxPerBank; ++t) anyLit = anyLit || h.get(P_FX_1 + t) > 0.5f;
    CHECK(!anyLit);   // no longer the effect as it came
    // While editing A (scene 1), a tap fills scene 1, and the knobs show it at once.
    h.tap(P_EDIT_A);
    pickFx(h, "HP Sweep");
    CHECK(h.chunk().find("\nscene1.name=HP Sweep\n") != std::string::npos);
    CHECK(h.value(P_FLT_TYPE) == 3.0f && std::fabs(h.value(P_FLT_CUT) - 1500.0f) < 2.0f);
    h.tap(P_EDIT_A);
    CHECK(h.value(P_FLT_TYPE) == 1.0f);   // the knobs back: LP 24
    // CLEAR takes the name too.
    h.tap(P_EDIT_A);
    h.press(P_SCN_CLEAR);
    h.tap(P_EDIT_A);
    CHECK(h.chunk().find("scene1.") == std::string::npos);
}

void looperEffectsArm() {
    std::printf("== fx: an effect of the looper arms it\n");
    Host h;
    CHECK(h.value(P_LP_ON) == 0.0f);
    pickFx(h, "Roll 1/16");
    CHECK(h.value(P_LP_ON) == 1.0f);
}

void everyEffectPlays() {
    std::printf("== fx: every effect, the fader swept into it and back\n");
    // Two bars at A first (the looper has a bar to play), 0.15 s into B, 0.5 s at B, back in 0.1 s.
    constexpr int kA = 700, kIn = 50, kAtB = 175, kBack = 35, kAll = kA + kIn + kAtB + kBack;
    const Buf in = whiteNoise(kBlock * kAll, 0.25f, 31);
    int quiet = 0;
    for (int k = 0; k < kNumFx; ++k) {
        Host h;
        h.play(true);
        pickFx(h, kFxLibrary[k].name);
        Buf out;
        for (int b = 0; b < kAll; ++b) {
            if (b >= kA && b < kA + kIn) h.set(P_XFADE, static_cast<float>(b - kA + 1) / kIn);
            if (b >= kA + kIn + kAtB) h.set(P_XFADE, std::max(0.0f, 1.0f - static_cast<float>(b - kA - kIn - kAtB + 1) / kBack));
            h.run(Buf(in.begin() + b * kBlock, in.begin() + (b + 1) * kBlock));
            out.insert(out.end(), h.L.begin(), h.L.end());
        }
        const double atB = rms(out, (kA + kIn) * kBlock, (kA + kIn + kAtB) * kBlock);
        const float pk = peak(out);
        if (db(atB) < -40.0) {
            ++quiet;
            std::printf("  %-14s at B %6.1f dB (quiet)\n", kFxLibrary[k].name, db(atB));
        }
        CHECK(h.finite && pk <= 8.0f);
        if (!h.finite || pk > 8.0f) std::printf("  %s: finite %d peak %.2f\n", kFxLibrary[k].name, h.finite, pk);
    }
    CHECK(quiet <= 1);   // a tape stop ends in silence; the rest all sound at B
}

// An effect's move: its LENGTH in beats (0: none) and PLAY, from its text.
double moveBeats(const char* text, std::string* play = nullptr) {
    const std::string t = text;
    const size_t at = t.find("\nmove=");
    if (at == std::string::npos) return 0.0;
    const std::string len = t.substr(at + 6, t.find('\n', at + 1) - at - 6);
    if (play) {
        const size_t p = t.find("\nplay=");
        *play = t.substr(p + 6, t.find('\n', p + 1) - p - 6);
    }
    for (int o = 0; o < PARAM_INFO[P_MV_LEN].nopts; ++o)
        if (len == PARAM_INFO[P_MV_LEN].opts[o]) return kMoveBeats[o];
    return -1.0;
}

void everyMoveRunsWhole() {
    std::printf("== fx: every effect that moves, its whole move at B (240 bpm)\n");
    int moving = 0;
    for (int k = 0; k < kNumFx; ++k) {
        std::string play;
        const double len = moveBeats(kFxLibrary[k].text, &play);
        CHECK(len >= 0.0);
        if (len <= 0.0) continue;
        ++moving;
        Host h;
        h.log.time.tempo = 240.0;
        h.play(true);
        pickFx(h, kFxLibrary[k].name);
        CHECK(h.display(P_MV_LEN) != "Off");
        const Buf in = whiteNoise(kBlock * 64, 0.25f, 37 + k);
        const auto run = [&](double beats, float* pk) {
            const int blocks = static_cast<int>(beats * 11025.0 / kBlock) + 1;   // 240 bpm: 11025 samples a beat
            for (int b = 0; b < blocks; ++b) {
                h.run(Buf(in.begin() + (b % 64) * kBlock, in.begin() + (b % 64 + 1) * kBlock));
                *pk = std::max(*pk, peak(h.L));
            }
        };
        float pk = 0.0f;
        run(8.0, &pk);   // two bars at A: the looper has them
        h.set(P_XFADE, 1.0f);
        run(4.0 + len + 1.0, &pk);   // up to a bar's wait, the move, a beat more
        const std::string line = h.display(P_SCN_INFO);
        const bool where = play == "Once" ? line.find(", HELD") != std::string::npos : line.find(" OF ") != std::string::npos;
        CHECK(h.finite && pk <= 8.0f && where);
        if (!h.finite || pk > 8.0f || !where)
            std::printf("  %s: finite %d peak %.2f, line \"%s\"\n", kFxLibrary[k].name, h.finite, pk, line.c_str());
    }
    std::printf("  %d effects move\n", moving);
    CHECK(moving == 11);

    // No clicks: a low sine through LP Sweep's close and Tape Stop's wind-down, once the fader is at B. A
    // sine's largest step is its level times its rate: never more than that of the loudest it gets (the
    // sweep's resonance lifts it as the cutoff passes).
    for (const char* name : {"LP Sweep", "Tape Stop"}) {
        Host h;
        h.log.time.tempo = 240.0;
        h.play(true);
        pickFx(h, name);
        const double len = moveBeats(kFxLibrary[findFx(name)].text);
        const int blocks = static_cast<int>((8.0 + 4.0 + len + 1.0) * 11025.0 / kBlock);
        const Buf in = sine(200.0, static_cast<size_t>(blocks) * kBlock, 0.5f);
        Buf out;
        for (int b = 0; b < blocks; ++b) {
            if (b == static_cast<int>(8.0 * 11025.0 / kBlock)) h.set(P_XFADE, 1.0f);
            h.run(Buf(in.begin() + b * kBlock, in.begin() + (b + 1) * kBlock));
            out.insert(out.end(), h.L.begin(), h.L.end());
        }
        const size_t from = static_cast<size_t>(8.5 * 11025.0);   // the fader settled; the move to come
        const Buf moved(out.begin() + static_cast<long>(from), out.end());
        const float natural = peak(moved) * 2.0f * 3.14159265f * 200.0f / 44100.0f;
        std::printf("  %-9s largest step %.4f (a sine at its peak, %.3f: %.4f)\n", name, maxStep(moved), peak(moved), natural);
        CHECK(h.finite && maxStep(moved) < 1.2f * natural);
    }
}

void recThroughThePlugin() {
    std::printf("== looper: REC through the plugin: Capture Next on the grid, kept, its line\n");
    Host h;
    h.play(true);
    h.on(P_LP_ON);
    h.option(P_LP_LEN, "1 bar");
    CHECK(h.display(P_LP_INFO).find("LISTENING") == 0 || h.display(P_LP_INFO).find("LOOPER OFF") == 0);
    h.blocks(4);
    CHECK(h.display(P_LP_INFO).find("LISTENING") == 0);
    // At beat 2.5: REC. Capture Next: the cell is bar 1 (beats 4..8), kept at beat 8.
    h.silence(static_cast<int>(2.5 * 22050) / kBlock * kBlock);
    h.press(P_LP_REC);
    CHECK(h.value(P_LP_HOLD) == 1.0f);
    h.blocks(2);
    CHECK(h.display(P_LP_INFO).find("REC 1 BAR IN ") != std::string::npos);
    h.silence(2 * 22050);   // beat ~4.5: recording its cell
    CHECK(h.display(P_LP_INFO).find("REC 1 BAR: ") != std::string::npos);
    h.silence(4 * 22050);   // past beat 8
    CHECK(h.display(P_LP_INFO).find("LOOP 1 BAR KEPT") == 0);
    h.set(P_LP_MIX, 1.0f);
    h.blocks(4);
    CHECK(h.display(P_LP_INFO).find("PLAYING 1 BAR") == 0);
    h.option(P_LP_REP, "1/16");
    h.blocks(4);
    CHECK(h.display(P_LP_INFO).find("PLAYING 1 BAR, ROLL 1/16") == 0);
    h.option(P_LP_ON, "Off");
    h.blocks(4);
    CHECK(h.display(P_LP_INFO).find("LOOPER OFF") == 0);
}

} // namespace

void eft::fxTests() {
    library();
    pickIntoSceneB();
    editMarksAndEditTarget();
    looperEffectsArm();
    everyEffectPlays();
    everyMoveRunsWhole();
    recThroughThePlugin();
}
