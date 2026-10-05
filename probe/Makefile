# The EffectForce Probe (docs/PROBE.md): a small tempo delay that records what MPC does with an insert
# effect. Kept for the host questions still open; it builds on its own: make -C probe <target>.
# Builds on Linux or WSL. Native: g++ (tests). Device: arm-linux-gnueabihf-g++ 11 or newer (libstdc++
# is linked dynamically; MPC OS has it). Your own settings (FORCE, SSH_KEY, PY) go in local.mk, which git
# ignores.
-include ../local.mk

CXX      ?= g++
ARM_PREFIX ?= arm-linux-gnueabihf-
ARM_CXX  ?= $(ARM_PREFIX)g++
ARM_RUN  ?= qemu-arm -L /usr/arm-linux-gnueabihf
BUILD    := build
# FORCE: the device, root@<ip>. SSH_KEY: the private key for it (empty: ssh's own defaults).
# PY: a Python 3 with Pillow, for skin, preview and plugin-package.
FORCE    ?=
SSH_KEY  ?=
PY       ?= python3

DEVICE_GOALS := plugin-install plugin-uninstall probe-log probe-clear latency-on latency-off
ifneq ($(filter $(DEVICE_GOALS),$(MAKECMDGOALS)),)
ifeq ($(strip $(FORCE)),)
$(error set FORCE=root@<device-ip> (on the command line or in local.mk))
endif
endif

# -n: ssh otherwise inherits make's stdin and hangs. No host-key checks: the Force's DHCP
# address changes on every restart, so its key is never in known_hosts.
SSH_OPTS := $(if $(strip $(SSH_KEY)),-i $(SSH_KEY) -o IdentitiesOnly=yes) -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
SSH      := ssh -n $(SSH_OPTS)

MV       := ../third_party/mpc-vst-plugins
SURF     := surface
SURF_OUT := $(SURF)/build
SKIN_DIR := $(SURF_OUT)/skin/Devko - VST - EffectForce Probe
GEN      := $(SURF_OUT)/param_ids.h $(SURF)/params.json $(SURF)/layout.conf $(SURF)/vst.json
SKIN     := $(SURF_OUT)/skin.stamp

SRC      := $(wildcard plugin/*.cpp)
HDR      := $(wildcard dsp/*.h plugin/*.h)
INC      := -I$(SURF_OUT) -Iplugin

# -funsafe-math-optimizations: without it GCC won't use NEON for float vectors on ARMv7.
# Not -ffast-math: keep isfinite() honest (the delay drops NaNs from the host with it).
ARM_OPT  := -O3 -march=armv7-a -mtune=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard \
            -funsafe-math-optimizations -fno-math-errno -fno-tree-loop-distribute-patterns
ARM_SO   := $(BUILD)/arm/effectforce_probe.so

.PHONY: all surface skin preview test test-arm arm-plugin plugin-package plugin-install plugin-uninstall \
        probe-log probe-clear latency-on latency-off clean
.DELETE_ON_ERROR:
all: test arm-plugin

# --- generated --------------------------------------------------------------------------------
# surface.py: params.json, layout.conf, vst.json and build/param_ids.h, after checking names and keys.
# It rewrites a file only when its text changes, so make may run it again without rebuilding anything.
surface: $(GEN)
$(GEN) &: $(SURF)/surface.py
	python3 $(SURF)/surface.py

# The skin (TUI.json + PNGs) and the plugin-list entry: sd88me's generator, Pillow and a host gcc.
skin: $(SKIN)
$(SKIN): $(GEN) $(MV)/tools/gen_vst.py $(MV)/tools/shadow_skin.py $(MV)/tools/skin_assets.py $(MV)/tools/shadow_art.c \
         $(MV)/tools/params.py $(wildcard $(MV)/tools/vendor/force-shadow/*/*) $(wildcard ../surface/fonts/*.ttf)
	mkdir -p $(SURF_OUT)
	gcc -O2 -I$(MV)/tools/vendor/force-shadow/tools -o $(SURF_OUT)/shadow_art $(MV)/tools/shadow_art.c -lm
	cd $(SURF) && SHADOW_TITLE_FONT=../../surface/fonts/TitilliumWeb-Bold.ttf $(PY) ../$(MV)/tools/gen_vst.py vst.json
	touch $@

# The page as surface/build/page_1.png, for checking the layout without a device.
preview: $(SKIN)
	$(PY) $(MV)/tools/studio.py preview "$(SKIN_DIR)/Plugin Skins" -o $(SURF_OUT)/page_%d.png

# --- native -----------------------------------------------------------------------------------
# The plugin through its VST2 entry points, under ASan/UBSan; any undefined behaviour fails the run.
TESTS    := test/probe_test.cpp
test: $(BUILD)/probe_test
	$(BUILD)/probe_test

$(BUILD)/probe_test: $(TESTS) $(wildcard test/*.h) $(SRC) $(HDR) $(GEN) | $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wextra -pthread \
		$(INC) $(SRC) $(TESTS) -o $@

# The same suite cross-compiled for the Force's CPU and run under qemu-user (no sanitizers).
test-arm: $(BUILD)/arm/probe_test
	$(ARM_RUN) $<

$(BUILD)/arm/probe_test: $(TESTS) $(wildcard test/*.h) $(SRC) $(HDR) $(GEN)
	mkdir -p $(BUILD)/arm
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wall -Wextra -Wno-psabi -pthread $(INC) $(SRC) $(TESTS) -o $@

# --- device -----------------------------------------------------------------------------------
# The .so MPC loads: only VSTPluginMain exported; --no-undefined because an unresolved symbol
# otherwise only shows up as MPC crashing on load. A plain build: the probe isn't about speed.
ARM_SO_FLAGS = -std=c++17 $(ARM_OPT) -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -fno-gnu-unique -Wall -Wextra -Wno-psabi -pthread $(INC)
ARM_SO_LINK  = -shared -Wl,--no-undefined -Wl,-soname,effectforce_probe.so -Wl,--version-script=plugin/exports.map

arm-plugin: $(ARM_SO)
$(ARM_SO): $(SRC) $(HDR) $(GEN) plugin/exports.map
	mkdir -p $(BUILD)/arm
	$(ARM_CXX) $(ARM_SO_FLAGS) $(ARM_SO_LINK) $(SRC) -o $@
	$(ARM_PREFIX)strip --strip-unneeded $@
	@$(ARM_PREFIX)readelf -V $@ | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | sed 's/^/needs /'
	@n=$$($(ARM_PREFIX)nm -D --defined-only $@ | wc -l); echo "exported symbols: $$n"; \
		[ $$n -eq 1 ] || { $(ARM_PREFIX)nm -D --defined-only $@; echo "only VSTPluginMain may be exported"; exit 1; }

# Package: plugin + skin + sd88me's installer (stops MPC, backs up and edits MPC.settings,
# restarts MPC). The plugin-list entry says Effect, 2 inputs: MPC lists it under insert effects.
PLUGIN_VERSION ?= 0.0.1
PKG      := EffectForce-Probe-$(PLUGIN_VERSION)
plugin-package: $(ARM_SO) $(SKIN)
	@grep -l "$$(printf '\r')" $(MV)/tools/release/*; r=$$?; [ $$r -eq 1 ] || { echo "error: CRLF in a shipped script, or no scripts"; exit 1; }
	$(PY) $(MV)/tools/release.py --so $(ARM_SO) --skin "$(SKIN_DIR)" --entry $(SURF_OUT)/pluginlist-entry.xml \
		--version $(PLUGIN_VERSION) --license MIT \
		--about "EffectForce Probe: a tempo delay that logs what MPC does with an insert effect (tempo, tails, bypass, latency) to /tmp/effectforce.log." \
		--requires "root SSH (MockbaMod)" -o dist

# Install on a device: stops MPC, backs up + edits MPC.settings, restarts MPC. Save the MPC
# project first. Usage: make plugin-install FORCE=root@<ip>
PKG_TMP := /tmp/efpkg
plugin-install: plugin-package
	rm -rf $(PKG_TMP) && mkdir -p $(PKG_TMP)
	python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' dist/$(PKG)-mpc-armv7.zip $(PKG_TMP)
	tar -C $(PKG_TMP) -cf - $(PKG) | ssh $(SSH_OPTS) $(FORCE) 'rm -rf /tmp/$(PKG) && tar -C /tmp -xf -'
	$(SSH) $(FORCE) 'sh /tmp/$(PKG)/install.sh -y'

# Remove it again (also stops and restarts MPC).
plugin-uninstall: plugin-package
	rm -rf $(PKG_TMP) && mkdir -p $(PKG_TMP)
	python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' dist/$(PKG)-mpc-armv7.zip $(PKG_TMP)
	tar -C $(PKG_TMP) -cf - $(PKG) | ssh $(SSH_OPTS) $(FORCE) 'rm -rf /tmp/$(PKG) && tar -C /tmp -xf -'
	$(SSH) $(FORCE) 'sh /tmp/$(PKG)/uninstall.sh -y'

# The probe's log on the device, and the latency test's flag (read when an instance is created).
probe-log:
	$(SSH) $(FORCE) 'cat /tmp/effectforce.log 2>/dev/null || echo "(no log yet)"'
# Emptied, not deleted: running instances keep the file open and would go on writing to a deleted one.
probe-clear:
	$(SSH) $(FORCE) ': > /tmp/effectforce.log'
latency-on:
	$(SSH) $(FORCE) 'touch /tmp/effectforce.latency'
latency-off:
	$(SSH) $(FORCE) 'rm -f /tmp/effectforce.latency'

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(SURF_OUT)
