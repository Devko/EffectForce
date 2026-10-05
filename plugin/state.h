#pragma once
// The plugin's state text, shared by projects (effGetChunk / effSetChunk) and preset files.
//
// "effectforce 1": key=value lines for every sound parameter and the chain's order: REAL values
// (Hz, seconds, dB) and options by name ("dly_mode=Ping-Pong", "order_3=Comp"; an index is read
// too), plus, in a project, the preset it came from. Survives parameters being added or reordered
// AND ranges changing (a 0..1 value would silently move when a range does).
#include "surface.h"

#include <string>

namespace ef {

constexpr int kStateVersion = 1;

std::string saveState(const Surface& s, bool asPreset);
bool isStateText(const std::string& text);   // "effectforce <version >= 1>" (a UTF-8 BOM allowed)
// A preset starts from the defaults (what it doesn't say is the default); a project's state
// only overrides what it lists. False if it isn't EffectForce state.
bool loadState(Surface& s, const std::string& text, bool asPreset);

// An effect of the FX library (or any scene's key=value lines) into one scene: it locks what the text
// names and nothing else (what the scene locked before and the text doesn't name is unlocked, after
// the new locks are in, so the engine never plays the scene half empty). Returns the text's name=
// ("" if none); `looper` says whether it uses the looper (lp_ keys).
std::string loadSceneText(Scenes& sc, int scene, const std::string& text, bool* looper = nullptr);

} // namespace ef
