#!/bin/sh
# release.py refuses a --previous-name that isn't a plain plugin name: it becomes the name of a folder the installer
# deletes (local patch 7). `make test-install` runs it on the build's own files:
#   sh test/release_test.sh <python> <release.py> <.so> <skin folder> <pluginlist entry>
set -u
PY=$1; REL=$2; SO=$3; SKIN=$4; ENTRY=$5
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
NAME=$(grep -o ' name="[^"]*"' "$ENTRY" | head -n 1 | cut -d'"' -f2)
pass=0; fail=0
release() { "$PY" "$REL" --so "$SO" --skin "$SKIN" --entry "$ENTRY" --version 0.0.0 --previous-name "$1" -o "$T/out" > "$T/log" 2>&1; }
refuses() {
    if release "$1"; then fail=$((fail + 1)); echo "FAIL: --previous-name '$1' was accepted"; else pass=$((pass + 1)); fi
}
refuses "Old/../x"
refuses ".."
refuses "it's"
refuses " Old"
refuses "Old "
refuses "$NAME"
if release "Old Name-2"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: a plain old name was refused: $(cat "$T/log")"; fi

if [ $fail = 0 ]; then echo "PASSED: $pass passed, 0 failed"; else echo "FAILED: $pass passed, $fail failed"; exit 1; fi
