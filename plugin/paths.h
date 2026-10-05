#pragma once
// Where EffectForce finds its files on the device, with test overrides.
//
//   pluginDir()   the folder our own .so was loaded from ("/sdcard/Synths/Devko - VST - EffectForce"),
//                 read from /proc/self/maps (the mapping that holds this code). Not dladdr(): built
//                 against glibc >= 2.34 that binds GLIBC_2.34. "" when it can't tell (tests).
//   presetRoots() preset folders, in order: <plugin dir>/Presets ("plugin"), then the SSD
//                 /media/AkaiForce/EffectForce Presets ("ssd"). A missing folder simply contributes
//                 nothing. EF_PRESET_ROOTS (colon-separated) replaces both.
//   dataDir()     the presets' favorites and recent lists: the plugin folder. EF_DATA_DIR
//                 overrides; "" = nothing is persisted. User presets go to the first preset root.
#include <string>
#include <vector>

namespace ef {

struct Root {
    std::string label;   // "plugin", "ssd", then "root3"... for extra test roots
    std::string dir;
};

std::string pluginDir();
std::vector<Root> presetRoots();
std::string dataDir();

// "plugin:User/User 001.efp" + roots -> "<dir of plugin root>/User/User 001.efp"; "" if the label is unknown.
std::string resolveKey(const std::string& key, const std::vector<Root>& roots);

// Writes `text` to `path` via path.new + rename (a crash never leaves half a file). durable: also
// fsync the file and its folder before returning (a preset; the device is often switched off
// hard), not for the favorites and recent lists, written on every preset change. False on error.
bool writeFileAtomic(const std::string& path, const std::string& text, bool durable = true);
// A whole (small) file: presets, lists. False if it is missing or over maxBytes.
bool readFile(const std::string& path, std::string& out, size_t maxBytes = 1u << 20);

} // namespace ef
