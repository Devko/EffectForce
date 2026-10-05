#pragma once
// Presets as files, plus the factory set built in.
//
//   *.efp under <plugin dir>/Presets and /media/AkaiForce/EffectForce Presets (EF_PRESET_ROOTS),
//   plus the factory presets (presets/Factory/<category>/, embedded at build time: one category
//   per folder, keys "builtin:<name>"). A preset file is the plugin's own state text
//   ("effectforce 1" + key=value lines), so a preset and a project restore the same way. Saving
//   writes <first root>/User/User NNN.efp: there is no text entry on the device, so names are
//   numbered.
#include "library.h"

#include <string>

namespace ef {

bool presetText(const std::string& key, std::string& out);   // factory or file
// Claims the next user preset file (created empty: highest number + 1) and its key. "" if
// there is no preset root. The caller writes it, or removes it on failure.
std::string nextUserPreset(std::string* key);

} // namespace ef
