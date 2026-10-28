#!/bin/sh
# The release installer against scratch folders (MPC_INSTALL_TEST=1: no device checks, MPC isn't touched), for an
# upgrade from the plugin's old name: the user's files move into the new folder, the old folder goes, MPC.settings
# keeps one entry for the uid. `make test-install` runs it on the package it just built:
#   sh test/install_test.sh <package zip> <old name>
set -u
ZIP=$1; OLDNAME=$2
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 -m zipfile -e "$ZIP" "$T/pkg" || exit 1
PKG=$(ls -d "$T"/pkg/*/ | head -n 1)
SKIN=$(ls "$PKG/portable")
SO=$(ls "$PKG/portable/$SKIN" | grep '\.so$')
META="$PKG/portable/$SKIN/plugin-meta.xml"
UID_HEX=$(grep -o ' uid="[^"]*"' "$META" | cut -d'"' -f2)
PREV="${SKIN%% - VST - *} - VST - $OLDNAME"
OLDSO="$(echo "$OLDNAME" | tr 'A-Z' 'a-z').so"
pass=0; fail=0; name=""
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $name: $*"; }

# A Synths folder ($1) with the plugin installed under its old name: the user's files and the .so, and its MPC.settings entry.
old_install() {
    mkdir -p "$1/$PREV/Presets"
    printf 'x 1\nf_cut=500\n' > "$1/$PREV/Presets/Mine.sfp"
    echo "Pads/Warm.sfp" > "$1/$PREV/preset_favorites.txt"
    echo "not a plugin" > "$1/$PREV/$OLDSO"
    sed -e "s|/$SKIN/$SO|/$PREV/$OLDSO|" -e "s| name=\"[^\"]*\"| name=\"$OLDNAME\"|" "$META" > "$1/$PREV/plugin-meta.xml"
    entry=$(sed -e "s|%payload-path%/$SKIN/$SO|$1/$PREV/$OLDSO|" -e "s| name=\"[^\"]*\"| name=\"$OLDNAME\"|" "$META")
    printf '<?xml version="1.0" encoding="UTF-8"?>\n<PROPERTIES>\n  <VALUE name="pluginList-arm">\n    <KNOWNPLUGINS>\n      %s\n    </KNOWNPLUGINS>\n  </VALUE>\n</PROPERTIES>\n' "$entry" > "$T/MPC.settings"
}
fresh() { rm -rf "$T/Synths" "$T/Other" "$T/out.txt"; mkdir -p "$T/Synths"; printf '<PROPERTIES>\n</PROPERTIES>\n' > "$T/MPC.settings"; }
install() {   # install.sh -y -t "$1"; its output in $T/out.txt
    MPC_INSTALL_TEST=1 MPC_SETTINGS="$T/MPC.settings" MPC_LEGACY_ROOT="$T/legacy" sh "$PKG/install.sh" -y -t "$1" > "$T/out.txt" 2>&1
}
moved() {   # the user's files are in the new folder in $1, the old folder is gone, MPC.settings lists the uid once, at the new path
    [ -f "$1/$SKIN/Presets/Mine.sfp" ] && [ "$(cat "$1/$SKIN/Presets/Mine.sfp")" = "$(printf 'x 1\nf_cut=500')" ] && ok || bad "Presets/Mine.sfp not carried over"
    grep -qx "Pads/Warm.sfp" "$1/$SKIN/preset_favorites.txt" 2>/dev/null && ok || bad "preset_favorites.txt not carried over"
    [ ! -e "$1/$PREV" ] && ok || bad "the old folder $PREV is still there"
    [ "$(grep -c " uid=\"$UID_HEX\"" "$T/MPC.settings")" = 1 ] && ok || bad "MPC.settings hasn't exactly one entry for uid $UID_HEX"
    grep -q "file=\"$1/$SKIN/$SO\"" "$T/MPC.settings" && ok || bad "MPC.settings doesn't point at $1/$SKIN/$SO"
}

name=upgrade_moves_user_data; fresh; old_install "$T/Synths"
install "$T/Synths" && ok || bad "install failed: $(cat "$T/out.txt")"
moved "$T/Synths"
grep -q "moved your files from $PREV into $SKIN" "$T/out.txt" && ok || bad "no 'moved your files' line"

name=upgrade_custom_synths_dir; fresh; old_install "$T/Synths"; mkdir -p "$T/Other"; old_install "$T/Other"
install "$T/Other" && ok || bad "install failed: $(cat "$T/out.txt")"
moved "$T/Other"
[ -f "$T/Synths/$PREV/Presets/Mine.sfp" ] && ok || bad "the old folder in the other Synths folder was touched"

name=second_run_is_noop; fresh; old_install "$T/Synths"
install "$T/Synths" && install "$T/Synths" && ok || bad "a second install failed: $(cat "$T/out.txt")"
moved "$T/Synths"
! grep -q "moved your files" "$T/out.txt" && ok || bad "the second run moved files again"

name=interrupted_first_run; fresh; old_install "$T/Synths"
sed -i '/<\/PROPERTIES>/d' "$T/MPC.settings"   # the settings check stops the run after the new folder is in place
install "$T/Synths" && bad "the broken MPC.settings didn't stop the first run" || ok
[ -d "$T/Synths/$SKIN" ] && [ -d "$T/Synths/$PREV" ] && ok || bad "expected both folders after the stopped run"
printf 'y 1\n' > "$T/Synths/$PREV/Presets/Later.sfp"   # saved with the old plugin, still the registered one
echo "</PROPERTIES>" >> "$T/MPC.settings"
install "$T/Synths" && ok || bad "the re-run failed: $(cat "$T/out.txt")"
[ -f "$T/Synths/$SKIN/Presets/Later.sfp" ] && ok || bad "the preset saved between the two runs was lost"
moved "$T/Synths"

name=foreign_old_folder_kept; fresh; old_install "$T/Synths"
sed -i "s| uid=\"$UID_HEX\"| uid=\"00000000\"|" "$T/Synths/$PREV/plugin-meta.xml"   # another plugin's folder by that name
install "$T/Synths" && ok || bad "install failed: $(cat "$T/out.txt")"
[ -f "$T/Synths/$PREV/Presets/Mine.sfp" ] && ok || bad "another plugin's folder was touched"
[ ! -e "$T/Synths/$SKIN/Presets/Mine.sfp" ] && ok || bad "another plugin's presets were copied in"
grep -q "belongs to another plugin" "$T/out.txt" && ok || bad "no warning about the other plugin's folder"

name=fresh_install; fresh
install "$T/Synths" && ok || bad "install failed: $(cat "$T/out.txt")"
[ -f "$T/Synths/$SKIN/$SO" ] && ok || bad "$SO not installed"
! grep -q "moved your files" "$T/out.txt" && ok || bad "a fresh install says it moved files"

name=no_previous_name; fresh; mkdir -p "$T/Synths/Other Plugin"; echo keep > "$T/Synths/Other Plugin/keep.txt"
P2="$T/pkg2"; rm -rf "$P2"; cp -r "$PKG" "$P2"   # a package built without --previous-name: PREV_SKIN is empty
sed -i "s|^PREV_SKIN='[^']*'|PREV_SKIN=''|" "$P2/install.sh"
(cd "$P2" && grep -v '  install.sh$' SHA256SUMS > SHA256SUMS.new && sha256sum install.sh >> SHA256SUMS.new && mv SHA256SUMS.new SHA256SUMS)
MPC_INSTALL_TEST=1 MPC_SETTINGS="$T/MPC.settings" MPC_LEGACY_ROOT="$T/legacy" sh "$P2/install.sh" -y -t "$T/Synths" > "$T/out.txt" 2>&1 &&
    ok || bad "install failed: $(cat "$T/out.txt")"
[ -f "$T/Synths/Other Plugin/keep.txt" ] && [ -f "$T/Synths/$SKIN/$SO" ] && ok || bad "the Synths folder lost files"

if [ $fail = 0 ]; then echo "PASSED: $pass passed, 0 failed"; else echo "FAILED: $pass passed, $fail failed"; exit 1; fi
