# EffectForce: an effect rack as a VST2 insert effect for MPC OS (Force / MPC standalone).
# Builds on Linux or WSL. Native: g++ (tests, x86 bench). Device: arm-linux-gnueabihf-g++ 11 or newer
# (libstdc++ is linked dynamically; MPC OS has it). Releases come from CI, built against glibc 2.31 so
# they load on MPC OS 2.x and 3.x; a newer distribution's cross toolchain needs a newer glibc (3.x only).
# Your own settings (FORCE, SSH_KEY, PY) go in local.mk, which git ignores. The probe that came first
# builds on its own: make -C probe.
-include local.mk

CXX      ?= g++
# ARM_PREFIX: the device toolchain's prefix (also for strip, readelf and nm); empty for a native ARM
# build (the release CI builds in arm32v7/gcc:11-bullseye, glibc 2.31, see .github/workflows/build.yml).
# ARM_RUN: how ARM programs run here: qemu-user on x86, nothing on ARM.
ARM_PREFIX ?= arm-linux-gnueabihf-
ARM_CXX  ?= $(ARM_PREFIX)g++
ARM_RUN  ?= qemu-arm -L /usr/arm-linux-gnueabihf
BUILD    := build
# FORCE: the device, root@<ip>, for bench-device and plugin-install. SSH_KEY: the private key for
# it (empty: ssh's own defaults). PY: a Python 3 with Pillow, for skin, preview and plugin-package.
FORCE    ?=
SSH_KEY  ?=
PY       ?= python3

ifneq ($(filter bench-device plugin-install plugin-uninstall,$(MAKECMDGOALS)),)
ifeq ($(strip $(FORCE)),)
$(error set FORCE=root@<device-ip> (on the command line or in local.mk))
endif
endif

# -n: ssh otherwise inherits make's stdin and hangs. No host-key checks: the Force's DHCP
# address changes on every restart, so its key is never in known_hosts.
SSH_OPTS := $(if $(strip $(SSH_KEY)),-i $(SSH_KEY) -o IdentitiesOnly=yes) -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
SSH      := ssh -n $(SSH_OPTS)

MV       := third_party/mpc-vst-plugins
SURF     := surface
SURF_OUT := $(SURF)/build
SKIN_DIR := $(SURF_OUT)/skin/Devko - VST - EffectForce
GEN      := $(SURF_OUT)/param_ids.h $(SURF_OUT)/factory_presets.h $(SURF_OUT)/fx_library.h
SKIN     := $(SURF_OUT)/skin.stamp

SRC      := $(wildcard dsp/*.cpp) $(wildcard plugin/*.cpp)
HDR      := $(wildcard dsp/*.h plugin/*.h)
INC      := -I$(SURF_OUT) -Iplugin

# -funsafe-math-optimizations: without it GCC won't use NEON for float vectors on ARMv7
# (NEON flushes denormals, so it isn't IEEE-exact). Not -ffast-math: keep isfinite() honest
# (every module drops NaNs from the host with it).
ARM_OPT  := -O3 -march=armv7-a -mtune=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard \
            -funsafe-math-optimizations -fno-math-errno -fno-tree-loop-distribute-patterns
ARM_SO   := $(BUILD)/arm/effectforce.so
ARM_BENCH := $(BUILD)/arm/efbench

.PHONY: all surface skin preview test test-arm test-arm-pgo test-module test-module-arm bench levels preset-levels arm-plugin arm-bench \
        bench-device rebuild-check \
        plugin-package plugin-install plugin-uninstall clean
# A recipe that fails leaves no half-written target behind for the next make to trust.
.DELETE_ON_ERROR:
all: test arm-plugin

# --- generated --------------------------------------------------------------------------------
# surface.py: params.json, layout.conf, vst.json and build/param_ids.h (the C++ side). Needs only
# python3, so tests and the .so build anywhere. It also checks the layout (keys, options, when=,
# Q-Link sets, geometry, names) and every factory preset before writing anything.
surface: $(GEN)
# The preset folders themselves too: their times change when a preset is deleted.
# The fonts too: the layout check measures labels with them.
$(GEN) &: $(SURF)/surface.py presets/Factory $(wildcard presets/Factory/*) $(wildcard presets/Factory/*/*.efp) \
          presets/FX $(wildcard presets/FX/*) $(wildcard presets/FX/*/*.eff) \
          $(wildcard $(SURF)/fonts/*.ttf)   # one run writes both
	python3 $(SURF)/surface.py

# The skin (TUI.json + PNGs) and the plugin-list entry: sd88me's generator, Pillow and a host gcc.
# Then surface/skin_polish.py redraws the knob strips, trigger buttons and stepper arrows (same names
# and sizes) and makes each group's pages sub-pages of one tab; it checks the skin first and fails the
# build on a mismatch. SHADOW_TITLE_FONT = TITLE_FONT in surface.py.
skin: $(SKIN)
$(SKIN): $(GEN) $(MV)/tools/gen_vst.py $(MV)/tools/shadow_skin.py $(MV)/tools/skin_assets.py $(MV)/tools/shadow_art.c \
         $(MV)/tools/params.py $(wildcard $(MV)/tools/vendor/force-shadow/*/*) $(SURF)/skin_polish.py $(wildcard $(SURF)/fonts/*.ttf)
	mkdir -p $(SURF_OUT)
	gcc -O2 -I$(MV)/tools/vendor/force-shadow/tools -o $(SURF_OUT)/shadow_art $(MV)/tools/shadow_art.c -lm
	cd $(SURF) && SHADOW_TITLE_FONT=fonts/TitilliumWeb-Bold.ttf $(PY) ../$(MV)/tools/gen_vst.py vst.json
	$(PY) $(SURF)/skin_polish.py "$(SKIN_DIR)/Plugin Skins" --layout $(SURF)/layout.conf --style $(SURF_OUT)/skin_style.json
	touch $@

# Skin previews (PNG per page) for checking the layout without a device.
preview: $(SKIN)
	$(PY) $(MV)/tools/studio.py preview "$(SKIN_DIR)/Plugin Skins" -o $(SURF_OUT)/page_%d.png

# --- native -----------------------------------------------------------------------------------
# The whole plugin through its VST2 entry points and every module on its own, under ASan/UBSan;
# any undefined behaviour fails the run (UBSan otherwise reports and carries on).
test: $(BUILD)/plugin_test
	$(BUILD)/plugin_test

TESTS    := $(wildcard test/*_test.cpp)
$(BUILD)/plugin_test: $(TESTS) $(wildcard test/*.h tools/*.h) $(SRC) $(HDR) $(GEN) | $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wextra -pthread \
		$(INC) $(SRC) $(TESTS) -o $@

# The same suite cross-compiled for the Force's CPU and run under qemu-user (no sanitizers):
# catches 32-bit and ARM-only code paths (the FPSCR flush, NEON float code).
test-arm: $(BUILD)/arm/plugin_test
	$(ARM_RUN) $<

$(BUILD)/arm/plugin_test: $(TESTS) $(wildcard test/*.h tools/*.h) $(SRC) $(HDR) $(GEN)
	mkdir -p $(BUILD)/arm
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wall -Wextra -Wno-psabi -pthread $(INC) $(SRC) $(TESTS) -o $@

# One module's tests on their own (docs/DESIGN.md "Modules"): make test-module M=chorus builds
# test/chorus_test.cpp with dsp/chorus.cpp (if there is one), under ASan/UBSan; test-module-arm
# the same for the Force's CPU under qemu-arm.
M ?=
MOD_SRC = test/module_main.cpp test/$(M)_test.cpp $(wildcard dsp/$(M).cpp)
test-module: | $(BUILD)
	@[ -n "$(M)" ] || { echo "usage: make test-module M=<module>"; exit 1; }
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wextra \
		-DMODULE_TESTS=$(M)Tests $(MOD_SRC) -o $(BUILD)/$(M)_test
	$(BUILD)/$(M)_test
test-module-arm:
	@[ -n "$(M)" ] || { echo "usage: make test-module-arm M=<module>"; exit 1; }
	mkdir -p $(BUILD)/arm
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wall -Wextra -Wno-psabi -DMODULE_TESTS=$(M)Tests $(MOD_SRC) -o $(BUILD)/arm/$(M)_test
	$(ARM_RUN) $(BUILD)/arm/$(M)_test

# x86 numbers say nothing about the Force; this only checks the bench and the .so path work.
bench: $(BUILD)/effectforce.so $(BUILD)/efbench
	$(BUILD)/efbench $(BUILD)/effectforce.so -s 1 -c -1

$(BUILD)/effectforce.so: $(SRC) $(HDR) $(GEN) plugin/exports.map | $(BUILD)
	$(CXX) -std=c++17 -O3 -fno-tree-loop-distribute-patterns -fPIC -fvisibility=hidden -fno-gnu-unique -Wall -Wextra -pthread \
		$(INC) -shared -Wl,--no-undefined -Wl,--version-script=plugin/exports.map $(SRC) -o $@

$(BUILD)/efbench: tools/bench.cpp $(HDR) $(GEN) | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(INC) $< -ldl -o $@

# Level-matching (tools/levels.cpp): every factory preset's Output set so a reference mix comes out as
# loud as it went in. `levels` reports, `preset-levels` writes the presets' out_gain and regenerates.
$(BUILD)/eflevels: tools/levels.cpp tools/loudness.h plugin/vst2.h | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $< -ldl -o $@
levels: $(BUILD)/effectforce.so $(BUILD)/eflevels
	$(BUILD)/eflevels $(BUILD)/effectforce.so presets/Factory
preset-levels: $(BUILD)/effectforce.so $(BUILD)/eflevels
	$(BUILD)/eflevels $(BUILD)/effectforce.so presets/Factory --write
	python3 $(SURF)/surface.py

# --- device -----------------------------------------------------------------------------------
# The .so MPC loads: only VSTPluginMain exported (a version script hides the C++ template
# instantiations and typeinfo -fvisibility leaves; -fno-gnu-unique keeps it unloadable);
# --no-undefined because an unresolved symbol otherwise only shows up as MPC crashing on load.
ARM_SO_FLAGS = -std=c++17 $(ARM_OPT) -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -fno-gnu-unique -Wall -Wextra -Wno-psabi -pthread $(INC)
ARM_SO_LINK  = -shared -Wl,--no-undefined -Wl,-soname,effectforce.so -Wl,--version-script=plugin/exports.map

ARM_SO_CMD   = $(ARM_CXX) $(ARM_SO_FLAGS) $(ARM_SO_LINK)

# Profile-guided: on by default when ARM programs can run here (qemu-arm installed, as test-arm
# needs, or a native ARM build); PGO=0 builds without. A copy of the plugin compiled with counters
# is linked into tools/pgo_train.cpp, which plays the reference mix through every module and preset;
# then the .so is compiled from the same sources with the same flags plus that profile, which tells the
# compiler which paths are hot. -fprofile-partial-training keeps functions the trainer never ran
# optimised as usual. Objects keep one path (dir_name.o) in both rounds: GCC names the profile files
# after it. A missing profile fails the build instead of quietly building without. (SubForce's.)
PGO      ?= auto
ARM_RUNS := $(if $(strip $(ARM_RUN)),$(shell command -v $(firstword $(ARM_RUN)) 2>/dev/null),native)
PGO_ON   := $(if $(filter auto,$(PGO)),$(if $(ARM_RUNS),1,0),$(PGO))
PGO_DIR  := $(BUILD)/arm/pgo
PGO_PROF := $(abspath $(PGO_DIR)/profile)
PGO_OBJ  := $(PGO_DIR)/obj
PGO_O    = $(PGO_OBJ)/$$(echo $$f | tr / _ | sed 's/\.cpp$$/.o/')

# The .so is rebuilt when the way it is built changes (PGO on/off, flags), not only its sources.
ARM_SO_STAMP := $(BUILD)/arm/so_flags
$(ARM_SO_STAMP): rebuild-check
	@mkdir -p $(dir $@)
	@echo '$(PGO_ON) $(ARM_SO_FLAGS)' | cmp -s - $@ || echo '$(PGO_ON) $(ARM_SO_FLAGS)' > $@
rebuild-check:

arm-plugin: $(ARM_SO)
$(ARM_SO): $(SRC) $(HDR) $(GEN) tools/pgo_train.cpp tools/loudness.h plugin/exports.map $(ARM_SO_STAMP)
	mkdir -p $(BUILD)/arm
ifeq ($(PGO_ON),1)
	rm -rf $(PGO_DIR) && mkdir -p $(PGO_OBJ) $(PGO_PROF)
	for f in $(SRC); do $(ARM_CXX) $(ARM_SO_FLAGS) -fprofile-generate=$(PGO_PROF) -fprofile-update=prefer-atomic \
		-c $$f -o $(PGO_O) || exit 1; done
	$(ARM_CXX) $(ARM_SO_FLAGS) -fprofile-generate tools/pgo_train.cpp $(PGO_OBJ)/*.o -o $(PGO_DIR)/train
	EF_DATA_DIR=$(PGO_DIR) EF_PRESET_ROOTS=$(PGO_DIR) EF_FIXED_SEED=1 $(ARM_RUN) $(PGO_DIR)/train
	@n=$$(ls $(PGO_PROF)/*.gcda 2>/dev/null | wc -l); [ $$n -eq $(words $(SRC)) ] || \
		{ echo "PGO: $$n of $(words $(SRC)) profiles written (PGO=0 builds without)"; exit 1; }
	for f in $(SRC); do $(ARM_CXX) $(ARM_SO_FLAGS) -fprofile-use=$(PGO_PROF) -fprofile-partial-training -Werror=missing-profile \
		-c $$f -o $(PGO_O) || exit 1; done
	$(ARM_CXX) $(ARM_SO_FLAGS) $(ARM_SO_LINK) $(PGO_OBJ)/*.o -o $@
	@echo "profile-guided build"
else
	$(ARM_SO_CMD) $(SRC) -o $@
	@echo "plain build (PGO=$(PGO): $(firstword $(ARM_RUN)) $(if $(ARM_RUNS),found,not found))"
endif
	$(ARM_PREFIX)strip --strip-unneeded $@
	@$(ARM_PREFIX)readelf -V $@ | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | sed 's/^/needs /'
	@n=$$($(ARM_PREFIX)nm -D --defined-only $@ | wc -l); echo "exported symbols: $$n"; \
		[ $$n -eq 1 ] || { $(ARM_PREFIX)nm -D --defined-only $@; echo "only VSTPluginMain may be exported"; exit 1; }

# The suite against the objects the shipped .so is linked from (profile-guided), under qemu. Here, below
# PGO_ON: make reads ifeq when it reads the line, so above its definition it was always the plain branch.
# EF_PGO_OBJECTS: the checks that pin exact output bits to one compilation (the Delay's Tape hash, the
# Reverb's fingerprints) say so and skip; the profile moves the compiler's fusing of float operations with
# every change of the code, and CI's GCC 11 fuses differently from GCC 13 anyway.
test-arm-pgo: $(ARM_SO)
ifeq ($(PGO_ON),1)
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wno-psabi -pthread -DEF_PGO_OBJECTS=1 $(INC) $(TESTS) $(PGO_OBJ)/*.o -o $(BUILD)/arm/plugin_test_pgo
	$(ARM_RUN) $(BUILD)/arm/plugin_test_pgo
else
	@echo "test-arm-pgo: the .so is a plain build here (PGO=$(PGO)); test-arm covers it"
endif

arm-bench: $(ARM_BENCH)
$(ARM_BENCH): tools/bench.cpp $(HDR) $(GEN)
	mkdir -p $(BUILD)/arm
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wall -Wextra -Wno-psabi $(INC) $< -ldl -o $@

# Bench on the Force: copies the .so and the bench to /tmp, runs pinned to core 1 (MPC's audio
# workers own cores 2-3) while MPC keeps running, then deletes them. Touches nothing else.
#   make bench-device FORCE=root@<ip>
BENCH_ARGS ?= -s 3
bench-device: $(ARM_SO) $(ARM_BENCH)
	scp -q $(SSH_OPTS) $(ARM_SO) $(ARM_BENCH) $(FORCE):/tmp/
	$(SSH) $(FORCE) 'r=0; /tmp/efbench /tmp/effectforce.so $(BENCH_ARGS) -c 1 || r=1; \
		rm -f /tmp/efbench /tmp/effectforce.so; exit $$r'

# Release zip: plugin + skin + sd88me's installer (stops MPC, backs up and edits MPC.settings,
# restarts MPC). The plugin-list entry says Effect with 2 inputs: MPC lists it under insert effects.
PLUGIN_VERSION ?= 0.0.1
PKG      := EffectForce-$(PLUGIN_VERSION)
plugin-package: $(ARM_SO) $(SKIN)
	@# Everything shipped runs under BusyBox on the device: a CR in a script breaks it there.
	@# grep: 1 = no CR found (good); 0 = found one; 2 = it couldn't read the scripts.
	@grep -l "$$(printf '\r')" $(MV)/tools/release/*; r=$$?; [ $$r -eq 1 ] || { echo "error: CRLF in a shipped script, or no scripts"; exit 1; }
	@# A newer toolchain's glibc: fine on the Force (MPC OS 3.x), not for a release (the catalog's limit is 2.32).
	@g=$$(readelf -V $(ARM_SO) 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1); \
		if [ -n "$$g" ] && [ "$$(printf '%s\n2.32\n' "$$g" | sort -V | tail -1)" != 2.32 ]; then \
		echo "warning: this .so needs glibc $$g: it loads on MPC OS 3.x only, and the plugin catalog refuses it."; \
		echo "         Release packages come from CI (glibc 2.31)."; fi
	$(PY) $(MV)/tools/release.py --so $(ARM_SO) --skin "$(SKIN_DIR)" --entry $(SURF_OUT)/pluginlist-entry.xml \
		--version $(PLUGIN_VERSION) --repo Devko/EffectForce --license MIT \
		--about "EffectForce effect rack (preview): drive, filter, EQ, compressor / OTT, chorus, phaser / flanger, tremolo / auto-pan / gate, granular textures, delay and a reverb with shimmer, in any order, with macros, LFOs, an envelope follower and a mod matrix; and an Octatrack-style performance mixer: two scenes on the crossfader, 64 effects that fade in (the sweeps and risers move on the bar), a looper on the grid." \
		--requires "root SSH (MockbaMod)" \
		--user-data Presets --user-data preset_favorites.txt --user-data preset_recent.txt \
		-o dist

# Install on a device: stops MPC, backs up + edits MPC.settings, restarts MPC. Save the MPC
# project first. Usage: make plugin-install FORCE=root@<ip>
PKG_TMP := /tmp/efpkg
plugin-install: plugin-package
	rm -rf $(PKG_TMP) && mkdir -p $(PKG_TMP)
	python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' dist/$(PKG)-mpc-armv7.zip $(PKG_TMP)
	tar -C $(PKG_TMP) -cf - $(PKG) | ssh $(SSH_OPTS) $(FORCE) 'rm -rf /tmp/$(PKG) && tar -C /tmp -xf -'
	$(SSH) $(FORCE) 'sh /tmp/$(PKG)/install.sh -y'

# Remove it again (also stops and restarts MPC); keeps the user's presets.
plugin-uninstall: plugin-package
	rm -rf $(PKG_TMP) && mkdir -p $(PKG_TMP)
	python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' dist/$(PKG)-mpc-armv7.zip $(PKG_TMP)
	tar -C $(PKG_TMP) -cf - $(PKG) | ssh $(SSH_OPTS) $(FORCE) 'rm -rf /tmp/$(PKG) && tar -C /tmp -xf -'
	$(SSH) $(FORCE) 'sh /tmp/$(PKG)/uninstall.sh -y'

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(SURF_OUT)
