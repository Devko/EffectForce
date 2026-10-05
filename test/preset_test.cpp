// Saved state, presets and the surface: chunk round trips, the preset stepper and buttons, user
// presets, the browser, favorites, stepping, pushing values back to MPC, and every factory preset
// playing (and level-matched).
#include "host.h"
#include "../plugin/presets.h"
#include "factory_presets.h"
#include "../tools/loudness.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace eft {
namespace {

using namespace ef;

void testState() {
    std::printf("== saved state\n");
    Host a;
    a.set(P_FLT_CUT, 333.0f);
    a.option(P_DLY_MODE, "Ping-Pong");
    a.option(P_DLY_DIV, "1/4T");
    a.set(P_REV_DECAY, 7.5f);
    a.on(P_CMP_ON);
    a.option(P_M4_DST, "Reverb Mix");
    const std::string s = a.chunk();
    CHECK(s.compare(0, 14, "effectforce 1\n") == 0);
    CHECK(s.find("flt_cut=333\n") != std::string::npos && s.find("rev_decay=7.5\n") != std::string::npos);
    CHECK(s.find("dly_mode=Ping-Pong\n") != std::string::npos && s.find("m4_dst=Reverb Mix\n") != std::string::npos);
    Host b;
    CHECK(b.load(s) == 1);
    for (int i = 0; i < P_COUNT; ++i) {
        const Kind k = PARAM_INFO[i].kind;
        if ((k == Kind::Synth || k == Kind::Chain) && std::fabs(a.get(i) - b.get(i)) > 1e-5f) {
            std::printf("  %s differs after a round trip\n", PARAM_INFO[i].key);
            CHECK(false);
        }
    }
    // A project's state changes only what it lists; unknown keys, bad numbers and options are skipped.
    Host c;
    c.set(P_FLT_RES, 0.5f);
    c.set(P_DLY_FB, 0.7f);
    CHECK(c.load("effectforce 1\nflt_cut=100\nnot_a_param=3\ndly_fb=abc\ndly_mode=Sideways\nrev_mode=2\n") == 1);
    CHECK(std::fabs(c.value(P_FLT_CUT) - 100.0f) < 0.1f && std::fabs(c.value(P_FLT_RES) - 0.5f) < 1e-4f);
    CHECK(std::fabs(c.value(P_DLY_FB) - 0.7f) < 1e-4f && c.value(P_DLY_MODE) == 0.0f);   // left as they were
    CHECK(c.value(P_REV_MODE) == 2.0f);                                                   // an option by index
    // Out of range values clamp; BOM and CRLF are fine; other text is refused.
    CHECK(c.load("\xEF\xBB\xBF" "effectforce 1\r\nflt_cut=99999\r\n") == 1 && std::fabs(c.value(P_FLT_CUT) - 20000.0f) < 1.0f);
    CHECK(c.load("subforce 1\nvolume=0\n") == 0 && c.load("") == 0 && c.load("effectforce x\n") == 0);
    CHECK(c.op(vst::effSetChunk, 0, 0, nullptr) == 0);
    // The selected chain slot and the browser are the surface's, not the sound's.
    CHECK(s.find("chain_sel") == std::string::npos && s.find("cat_") == std::string::npos);
}

void testPresets() {
    std::printf("== presets\n");
    const std::string root = fixtureDir() + "/presets";
    Host h;
    // INIT loads the Init preset (everything off, unity).
    h.on(P_REV_ON);
    h.set(P_FLT_CUT, 50.0f);
    h.press(P_PRE_INIT);
    CHECK(h.value(P_REV_ON) == 0.0f && std::fabs(h.get(P_FLT_CUT) - PARAM_INFO[P_FLT_CUT].def) < 1e-5f);
    CHECK(h.display(P_PRESET) == "PRESET  Utility / Init");
    // SAVE writes User 001.efp, then User 002; the stepper shows it.
    h.set(P_FLT_CUT, 444.0f);
    h.press(P_PRE_SAVE);
    CHECK(std::filesystem::exists(root + "/User/User 001.efp"));
    CHECK(h.display(P_PRESET) == "PRESET  User / User 001");
    std::string text;
    CHECK(presetText("plugin:User/User 001.efp", text) && text.find("flt_cut=444\n") != std::string::npos &&
          text.find("preset=") == std::string::npos);
    h.press(P_PRE_SAVE);
    CHECK(std::filesystem::exists(root + "/User/User 002.efp"));
    // Next / previous walk the flat list; the stepper turns one preset per event.
    h.press(P_PRE_INIT);
    const auto L = presetLibrary().listing();
    const int at = L->find("builtin:Init");
    CHECK(at >= 0);
    h.press(P_PRESET_NEXT);
    CHECK(h.display(P_PRESET) == "PRESET  " + L->label(L->items[static_cast<size_t>(at + 1)].key));
    h.press(P_PRESET_PREV);
    CHECK(h.display(P_PRESET) == "PRESET  Utility / Init");
    h.setN(P_PRESET, h.get(P_PRESET) + 1.0f / 128.0f);   // one Q-Link detent
    CHECK(h.display(P_PRESET) == "PRESET  " + L->label(L->items[static_cast<size_t>(at + 1)].key));
    // A project remembers its preset; a preset file doesn't name one.
    CHECK(h.chunk().find("\npreset=" + L->items[static_cast<size_t>(at + 1)].key + "\n") != std::string::npos);
    // User numbers are never reused, even after the newest file is deleted.
    std::filesystem::remove(root + "/User/User 002.efp");
    h.press(P_PRE_SAVE);
    CHECK(std::filesystem::exists(root + "/User/User 003.efp") && !std::filesystem::exists(root + "/User/User 002.efp"));
    // A preset file added while MPC runs shows up in a new instance.
    std::filesystem::create_directories(root + "/Mine");
    std::ofstream(root + "/Mine/Wide.efp") << "effectforce 1\nchr_on=On\n";
    Host fresh;
    CHECK(presetLibrary().listing()->find("plugin:Mine/Wide.efp") >= 0);
}

void testBrowser() {
    std::printf("== preset browser\n");
    Host h;
    h.press(P_PRE_INIT);
    h.blocks(8);
    // Categories: FAVORITES, RECENT, then the factory folders.
    CHECK(h.display(P_CAT_1) == "FAVORITES" && h.display(P_CAT_2) == "RECENT");
    bool utility = false;
    for (int t = 0; t < kBrowserCats; ++t) utility = utility || (h.display(P_CAT_1 + t) == "UTILITY" && h.get(P_CAT_1 + t) > 0.5f);
    CHECK(utility);   // Init's category is lit
    // Favorite: toggled, kept in the data folder, listed under FAVORITES.
    h.setN(P_FAV, 1.0f);
    CHECK(h.get(P_FAV) > 0.5f);
    CHECK(std::filesystem::exists(fixtureDir() + "/data/preset_favorites.txt"));
    h.setN(P_CAT_1, 1.0f);
    CHECK(h.get(P_CAT_1) > 0.5f && h.display(P_ITEM_1) == "Init");
    // A tap on a preset tile loads it.
    h.setN(P_ITEM_1, 1.0f);
    CHECK(h.display(P_BR_NOW) == "PRESET  Utility / Init");
}

void testStepping() {
    std::printf("== stepping (Q-Link, data wheel, taps)\n");
    Host h;
    // An option moves one step per Q-Link detent, whatever the size of the detent.
    h.option(P_REV_MODE, "Plate");
    h.setN(P_REV_MODE, h.get(P_REV_MODE) - 1.0f / 128.0f);
    CHECK(h.display(P_REV_MODE) == "Hall");
    // A tap on an option (its exact value) lands on it.
    h.setN(P_REV_MODE, 0.0f);
    CHECK(h.display(P_REV_MODE) == "Room");
    // The snapped value goes back to MPC.
    h.setN(P_CHR_MODE, 0.5f + 0.01f);   // off an option, far from Chorus: one step up, not a jump
    h.log.automated.clear();
    h.blocks(4);
    CHECK(h.log.automated.count(P_CHR_MODE) == 1 && h.log.automated[P_CHR_MODE] == 0.5f);
    // A popup's list closes when an option is picked.
    h.setN(P_M1_SRC__OPEN, 1.0f);
    CHECK(h.get(P_M1_SRC__OPEN) > 0.5f);
    h.option(P_M1_SRC, "LFO 2");
    CHECK(h.get(P_M1_SRC__OPEN) == 0.0f && h.display(P_M1_SRC) == "LFO 2");
    // Continuous knobs follow MPC as they are.
    h.setN(P_FLT_RES, 0.37f);
    CHECK(h.get(P_FLT_RES) == 0.37f);
    // Value texts.
    h.set(P_EQ_LC, 20.0f);
    CHECK(h.display(P_EQ_LC) == "Off");
    h.set(P_EQ_LC, 80.0f);
    CHECK(h.display(P_EQ_LC) == "80 Hz");
    h.set(P_CMP_RATIO, 4.0f);
    CHECK(h.display(P_CMP_RATIO) == "4.0:1");
    h.option(P_PHS_MODE, "Flanger");
    h.setN(P_PHS_CENTER, 0.0f);
    CHECK(h.display(P_PHS_CENTER) == "0.20 ms");
    h.option(P_PHS_MODE, "Phaser 4");
    CHECK(h.display(P_PHS_CENTER) == "50 Hz");
    h.set(P_DLY_TIME, 0.375f);
    CHECK(h.display(P_DLY_TIME) == "375 ms");
}

void testFactory() {
    std::printf("== factory presets\n");
    CHECK(kNumFactoryPresets >= 1);
    // Every preset plays finite and stays level-matched: the reference mix (tools/loudness.h) comes
    // out as loud as it went in, as `make preset-levels` left it. A DSP change that moves a preset's
    // level fails here: run make preset-levels again.
    int bad = 0;
    for (int i = 0; i < kNumFactoryPresets; ++i) {
        Buf inL, inR;
        efl::referenceMix(inL, inR, efl::partsFor(kFactoryPresets[i].category));
        const double ref = efl::lufs(inL, inR);
        Host h;
        h.play(true);
        std::string text;
        CHECK(presetText(std::string("builtin:") + kFactoryPresets[i].name, text));
        CHECK(h.load(text) == 1);
        h.run(inL, inR);
        const double gain = efl::lufs(h.L, h.R) - ref;
        const bool capped = std::fabs(h.value(P_OUT_GAIN)) >= 8.95f;   // as matched as the +-9 dB cap allows
        if (!h.finite || (std::fabs(gain) > 0.5 && !capped) || peak(h.L) > 2.0f) {
            ++bad;
            std::printf("  %s: %+.1f dB, peak %.2f%s\n", kFactoryPresets[i].name, gain, static_cast<double>(peak(h.L)),
                        h.finite ? "" : ", NOT FINITE");
        }
    }
    CHECK(bad == 0);
}

} // namespace

void presetTests() {
    testState();
    testPresets();
    testBrowser();
    testStepping();
    testFactory();
}

} // namespace eft
