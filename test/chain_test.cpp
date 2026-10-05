// The CHAIN page through the plugin, as a Force sends it: tiles show the order (lit = on, the
// selected one in brackets), a tap selects, MOVE swaps with a neighbour, ON / OFF switches the
// selected module; the order is saved by name and decides the sound.
#include "host.h"

#include <cstdio>

namespace {

using namespace eft;
using namespace ef;

std::string tile(Host& h, int k) { return h.display(P_SLOT_1 + k); }

void tiles() {
    Host h;
    CHECK(tile(h, 0) == "[ DRIVE ]" && tile(h, 1) == "FILTER" && tile(h, 2) == "EQ" && tile(h, 7) == "REVERB");
    for (int k = 0; k < kNumModules; ++k) CHECK(h.get(P_SLOT_1 + k) < 0.5f);   // all off
    // A tap selects.
    h.tap(P_SLOT_1 + 2);
    CHECK(tile(h, 2) == "[ EQ ]" && tile(h, 0) == "DRIVE");
    // ON / OFF switches the selected module, and its tile lights.
    h.press(P_SEL_ON);
    CHECK(h.value(P_EQ_ON) == 1.0f && h.get(P_SLOT_1 + 2) > 0.5f);
    // MOVE > swaps it with its right neighbour; the selection goes with it.
    h.press(P_MOVE_R);
    CHECK(tile(h, 2) == "COMP" && tile(h, 3) == "[ EQ ]");
    CHECK(h.value(P_ORDER_1 + 2) == M_COMP && h.value(P_ORDER_1 + 3) == M_EQ);
    CHECK(h.get(P_SLOT_1 + 3) > 0.5f && h.get(P_SLOT_1 + 2) < 0.5f);   // the light moved with EQ
    h.press(P_MOVE_L);
    h.press(P_MOVE_L);
    h.press(P_MOVE_L);
    CHECK(tile(h, 0) == "[ EQ ]" && tile(h, 1) == "DRIVE");
    h.press(P_MOVE_L);   // the first slot: nowhere to go
    CHECK(tile(h, 0) == "[ EQ ]");
    // A module switched on its own page lights its tile too.
    h.on(P_REV_ON);
    CHECK(h.get(P_SLOT_1 + 7) > 0.5f);
    // The tiles the plugin lit go to MPC (audioMasterAutomate) from the audio thread.
    h.log.automated.clear();
    h.blocks(8);
    CHECK(h.log.automated.count(P_SLOT_1 + 7) == 1);
}

void releaseEcho() {
    // MPC sends a tile tap's release ~0.7 s later: not a second tap.
    Host h;
    h.tap(P_SLOT_1 + 5);
    {
        Turn echo(700);
        h.setN(P_SLOT_1 + 5, h.get(P_SLOT_1 + 5) > 0.5f ? 0.0f : 1.0f);
    }
    CHECK(tile(h, 5) == "[ PHASER ]");
    h.tap(P_SLOT_1 + 6);   // a second later: a real tap
    CHECK(tile(h, 6) == "[ DELAY ]" && tile(h, 5) == "PHASER");
}

void saved() {
    Host a;
    a.tap(P_SLOT_1 + 7);
    a.press(P_MOVE_L);   // Reverb before Delay
    const std::string s = a.chunk();
    CHECK(s.find("order_7=Reverb\n") != std::string::npos && s.find("order_8=Delay\n") != std::string::npos);
    CHECK(s.find("chain_sel") == std::string::npos && s.find("slot_") == std::string::npos);   // the surface's own
    Host b;
    CHECK(b.load(s) == 1);
    CHECK(tile(b, 6) == "REVERB" && tile(b, 7) == "DELAY");
    // Not a permutation (a module twice): the default order.
    Host c;
    CHECK(c.load("effectforce 1\norder_1=Reverb\norder_2=Reverb\n") == 1);
    for (int k = 0; k < kNumModules; ++k) CHECK(c.value(P_ORDER_1 + k) == static_cast<float>(k));
}

void soundFollowsOrder() {
    // A hard clipper before a low-pass, then after it: different sounds, both finite.
    const Buf in = sine(150.0, kBlocksPerSec * kBlock, 0.5f);
    Buf first;
    for (int pass = 0; pass < 2; ++pass) {
        Host h;
        h.on(P_DRV_ON);
        h.option(P_DRV_TYPE, "Hard");
        h.set(P_DRV_AMT, 30.0f);
        h.on(P_FLT_ON);
        h.set(P_FLT_CUT, 400.0f);
        if (pass == 1) {
            h.tap(P_SLOT_1 + 0);
            h.press(P_MOVE_R);   // Filter, then Drive
        }
        h.run(in);
        h.run(in);
        CHECK(h.finite);
        // The clipper's 5th harmonic (750 Hz) survives only when the clipper comes after the low-pass.
        if (pass == 0) first = h.L;
        else CHECK(magnitude(h.L, 750.0, 8192) > 3.0 * magnitude(first, 750.0, 8192));
    }
}

} // namespace

void eft::chainTests() {
    std::printf("== the chain page\n");
    tiles();
    releaseEcho();
    saved();
    soundFollowsOrder();
}
