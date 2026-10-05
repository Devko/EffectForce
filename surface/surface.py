#!/usr/bin/env python3
"""EffectForce touchscreen surface: the ONE place the plugin's parameters and pages are defined.

Writes, next to this file:
  params.json        ordered VST parameter list (index = position; append-only once released:
                     MPC projects store values by index)
  layout.conf        the skin (shadow_page.conf syntax, see
                     third_party/mpc-vst-plugins/tools/shadow_skin.py); coords are 1280x800
                     Force-Shadow pixels, the plugin area is y = 86..714
  vst.json           plugin identity for the vendored gen_vst.py
  build/param_ids.h  everything the C++ is compiled against: parameter ids, kinds, value
                     curves, names, options, defaults, the modules and the modulation targets
                     (the C++ never reads gen_vst's params.h, so `make test` and the .so build need
                     only Python, not the skin toolchain)
  build/factory_presets.h  the factory presets (presets/Factory/*/*.efp), checked and embedded
  build/skin_style.json  the palette, knob looks, primary buttons and page groups for
                     skin_polish.py, which `make skin` runs after the generator

Continuous parameters are declared to MPC as 0..1: the real range and curve (log Hz, log
seconds, ...) live in param_ids.h, and the plugin formats every value text itself, so the
knob, its label and the DSP can never disagree.

Before writing anything the layout is checked the way shadow_skin.py would (unknown keys,
option counts, when=, Q-Link sets) plus geometry with shadow_skin's own sizes (inside the plugin
area, no overlaps within a page, nothing in a card's title band, open popup lists inside the
plugin area, bitmap-font glyphs) and the parameter names MPC shows (short, unique), so a broken
page fails here instead of on the device. The layout machinery and its checks are PolyForce's,
unchanged.

Run: python3 surface/surface.py   (make surface does this)
"""
import json
import math
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

STEPPER_RANGE = 1023        # the preset stepper's VST range: 0..1023 items (stepItem moves 1 per event)
BROWSER_CATS = 16           # category tiles on the browser page (2 x 8)
BROWSER_ITEMS = 24          # preset tiles (3 x 8)
MOD_SLOTS = 8

VST = {"name": "EffectForce", "vendor": "Devko", "uid": "EfFc", "version": 1000,
       "so": "effectforce.so", "params": "params.json", "layout": "layout.conf", "effect": True}

# The modules, in their default order; the C++ Module enum (build/param_ids.h) follows this list.
# (prefix, name on the CHAIN page and in saved state)
MODULES = [("drv", "Drive"), ("flt", "Filter"), ("eq", "EQ"), ("cmp", "Comp"), ("chr", "Chorus"),
           ("phs", "Phaser"), ("pls", "Pulse"), ("grn", "Grain"), ("dly", "Delay"), ("rev", "Reverb")]
MODULE_NAMES = [m[1] for m in MODULES]


# --- parameters ------------------------------------------------------------------------------
# kind:
#   synth    a sound parameter: saved in the state, automatable; curve lin|log|int|pow|enum
#   chain    a module's place in the chain (order_1..8): saved, part of the sound, but only the
#            plugin moves it (the CHAIN page's MOVE buttons); not automatable
#   ui       a value the surface keeps for itself (the selected chain slot): not saved
#   readout  text the plugin writes (status line, "PAGE 2 / 4"): read only
#   stepper  plugin-owned index into a list (presets), text = the item; moves one item per
#            Q-Link/wheel event; comes with <key>_prev / <key>_next buttons
#   button   momentary: acts on the press, springs back to 0
#   tile     a tile (list widget): lit = 1, text = the item; a tap acts
#   toggle   plugin-owned on/off (lit state follows the plugin), a tap acts
#   popup    the hidden "<key>__open" flag of a popup list (shadow_skin's popup_params)
# fmt: how the plugin prints the value (plugin/rack_map.cpp paramDisplay)
P = []


def _add(key, name, kind, curve, lo, hi, default, fmt, **extra):
    d = dict(key=key, name=name, kind=kind, curve=curve, lo=lo, hi=hi, default=default, fmt=fmt)
    d.update(extra)
    P.append(d)


def readout(key, name):
    _add(key, name, "readout", "readout", 0, 0, 0, "none")


def enum(key, name, options, default, kind="synth"):
    _add(key, name, kind, "enum", 0, len(options) - 1, options.index(default), "enum", options=options)


def num(key, name, curve, lo, hi, default, fmt):
    _add(key, name, "synth", curve, lo, hi, default, fmt)


def stepper(key, name):
    _add(key, name, "stepper", "int", 0, STEPPER_RANGE, 0, "text")
    button(key + "_prev", name + " Prev")
    button(key + "_next", name + " Next")


def button(key, name):
    _add(key, name, "button", "int", 0, 1, 0, "none")


def tile(key, name):
    _add(key, name, "tile", "enum", 0, 1, 0, "text", options=["-", "On"])


def toggle(key, name):
    _add(key, name, "toggle", "enum", 0, 1, 0, "enum", options=["Off", "On"])


def popup_flag(of):
    src = next(p for p in P if p["key"] == of)
    _add(of + "__open", "%s List" % src["name"], "popup", "enum", 0, 1, 0, "none", options=["Closed", "Open"],
         popup_of=of)


ON_OFF = ["Off", "On"]
FREE_SYNC = ["Free", "Sync"]
# dsp/common.h kDelayDivs and kLfoDivs, shortest first
DELAY_DIVS = ["1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T",
              "1/4.", "1/2", "1/2.", "1 bar"]
LFO_DIVS = ["1/16", "1/8T", "1/8", "1/4T", "1/8.", "1/4", "1/4.", "1/2", "1/2.", "1 bar", "2 bars", "4 bars",
            "8 bars", "16 bars"]

readout("status", "Status")            # index 0 must stay a read-only readout: MPC sets it at load
num("in_gain", "Rack Input", "lin", -24, 24, 0, "db")
num("out_gain", "Rack Output", "lin", -24, 24, 0, "db")
num("mix", "Rack Mix", "lin", 0, 1, 1, "pct")

# --- the chain: the order, the CHAIN page's tiles and buttons, the macros ---
for k in range(1, len(MODULES) + 1):
    enum("order_%d" % k, "Order %d" % k, MODULE_NAMES, MODULE_NAMES[k - 1], kind="chain")
_add("chain_sel", "Selected", "ui", "int", 0, len(MODULES) - 1, 0, "count")
for k in range(1, len(MODULES) + 1):
    tile("slot_%d" % k, "Slot %d" % k)
button("move_l", "Move Left")
button("move_r", "Move Right")
button("sel_on", "Slot On/Off")
for k in range(1, 5):
    num("mac_%d" % k, "Macro %d" % k, "lin", 0, 1, 0, "pct")

# --- the modules (dsp/<module>.h; ranges and defaults: docs/DESIGN.md "Modules") ---
enum("drv_on", "Drive On", ON_OFF, "Off")
enum("drv_type", "Drive Type", ["Soft", "Tube", "Hard", "Fold", "Sine", "Crush"], "Soft")
num("drv_amt", "Drive Amount", "lin", 0, 36, 12, "db")
num("drv_tone", "Drive Tone", "lin", -1, 1, 0, "bipct")
num("drv_bias", "Drive Bias", "lin", 0, 1, 0, "pct")
num("drv_out", "Drive Out", "lin", -24, 12, 0, "db")
num("drv_mix", "Drive Mix", "lin", 0, 1, 1, "pct")

enum("flt_on", "Filter On", ON_OFF, "Off")
enum("flt_type", "Filter Type", ["LP 12", "LP 24", "HP 12", "HP 24", "BP", "Notch"], "LP 24")
num("flt_cut", "Filter Cutoff", "log", 20, 20000, 1000, "hz")
num("flt_res", "Filter Res", "lin", 0, 1, 0.2, "pct")
num("flt_drive", "Filter Drive", "lin", 0, 1, 0, "pct")
num("flt_spread", "Filter Spread", "lin", -1, 1, 0, "oct")
num("flt_mix", "Filter Mix", "lin", 0, 1, 1, "pct")

enum("eq_on", "EQ On", ON_OFF, "Off")
num("eq_lc", "EQ Low Cut", "log", 20, 1000, 20, "hzlo")
num("eq_lf", "EQ Low Freq", "log", 30, 500, 100, "hz")
num("eq_lg", "EQ Low Gain", "lin", -18, 18, 0, "db")
num("eq_mf", "EQ Mid Freq", "log", 100, 10000, 1000, "hz")
num("eq_mg", "EQ Mid Gain", "lin", -18, 18, 0, "db")
num("eq_mq", "EQ Mid Q", "log", 0.3, 8, 1, "q")
num("eq_hf", "EQ High Freq", "log", 1000, 16000, 6000, "hz")
num("eq_hg", "EQ High Gain", "lin", -18, 18, 0, "db")
num("eq_hc", "EQ High Cut", "log", 1000, 20000, 20000, "hzhi")

enum("cmp_on", "Comp On", ON_OFF, "Off")
enum("cmp_mode", "Comp Mode", ["Comp", "OTT"], "Comp")
num("cmp_thr", "Comp Thresh", "lin", -60, 0, -18, "db")
num("cmp_ratio", "Comp Ratio", "log", 1, 20, 4, "ratio")
num("cmp_att", "Comp Attack", "log", 0.0001, 0.1, 0.01, "time")
num("cmp_rel", "Comp Release", "log", 0.01, 2, 0.15, "time")
num("cmp_knee", "Comp Knee", "lin", 0, 24, 6, "db")
num("cmp_sc", "Comp SC Cut", "log", 20, 500, 20, "hzlo")
num("cmp_makeup", "Comp Makeup", "lin", -12, 24, 0, "db")
num("cmp_mix", "Comp Mix", "lin", 0, 1, 1, "pct")
num("ott_depth", "OTT Depth", "lin", 0, 1, 0.6, "pct")
num("ott_time", "OTT Time", "log", 0.1, 10, 1, "mult")
num("ott_up", "OTT Up", "lin", 0, 1, 1, "pct")
num("ott_down", "OTT Down", "lin", 0, 1, 1, "pct")
num("ott_low", "OTT Low", "lin", -12, 12, 0, "db")
num("ott_mid", "OTT Mid", "lin", -12, 12, 0, "db")
num("ott_high", "OTT High", "lin", -12, 12, 0, "db")

enum("chr_on", "Chorus On", ON_OFF, "Off")
enum("chr_mode", "Chorus Mode", ["Chorus", "Ensemble", "Dimension"], "Chorus")
num("chr_rate", "Chorus Rate", "log", 0.03, 10, 0.5, "lfohz")
num("chr_depth", "Chorus Depth", "lin", 0, 1, 0.5, "pct")
num("chr_delay", "Chorus Delay", "log", 0.001, 0.04, 0.012, "time")
num("chr_lc", "Chorus Lo Cut", "log", 20, 1000, 120, "hzlo")
num("chr_width", "Chorus Width", "lin", 0, 1, 1, "pct")
num("chr_mix", "Chorus Mix", "lin", 0, 1, 0.5, "pct")

enum("phs_on", "Phaser On", ON_OFF, "Off")
enum("phs_mode", "Phaser Mode", ["Phaser 4", "Phaser 8", "Phaser 12", "Flanger"], "Phaser 4")
enum("phs_sync", "Phaser Sync", FREE_SYNC, "Free")
num("phs_rate", "Phaser Rate", "log", 0.01, 20, 0.3, "lfohz")
enum("phs_div", "Phaser Div", LFO_DIVS, "1 bar")
popup_flag("phs_div")
num("phs_depth", "Phaser Depth", "lin", 0, 1, 0.7, "pct")
num("phs_center", "Phaser Center", "lin", 0, 1, 0.5, "center")   # Hz for a phaser, ms for the flanger
num("phs_fb", "Phaser FB", "lin", -0.95, 0.95, 0.5, "bipct")
num("phs_stereo", "Phaser Stereo", "lin", 0, 180, 90, "deg")
num("phs_mix", "Phaser Mix", "lin", 0, 1, 0.5, "pct")

enum("dly_on", "Delay On", ON_OFF, "Off")
enum("dly_mode", "Delay Mode", ["Stereo", "Ping-Pong", "Mono"], "Stereo")
enum("dly_sync", "Delay Sync", FREE_SYNC, "Sync")
num("dly_time", "Delay Time", "log", 0.001, 2, 0.375, "time")
enum("dly_div", "Delay Div", DELAY_DIVS, "1/8.")
popup_flag("dly_div")
num("dly_fb", "Delay FB", "lin", 0, 1, 0.4, "pct")
num("dly_spread", "Delay Spread", "lin", -0.5, 0.5, 0, "bipct")
num("dly_lc", "Delay Lo Cut", "log", 20, 2000, 100, "hzlo")
num("dly_hc", "Delay Hi Cut", "log", 500, 20000, 8000, "hzhi")
num("dly_wow", "Delay Wow", "lin", 0, 1, 0, "pct")
num("dly_drive", "Delay Drive", "lin", 0, 1, 0, "pct")
num("dly_duck", "Delay Duck", "lin", 0, 1, 0, "pct")
num("dly_mix", "Delay Mix", "lin", 0, 1, 0.3, "pct")
enum("dly_glide", "Delay Glide", ["Tape", "Fade"], "Tape")   # a time change bends the repeats, or crossfades

enum("rev_on", "Reverb On", ON_OFF, "Off")
enum("rev_mode", "Reverb Mode", ["Room", "Hall", "Plate", "Space"], "Hall")
num("rev_size", "Reverb Size", "lin", 0, 1, 0.5, "pct")
num("rev_decay", "Reverb Decay", "log", 0.1, 30, 2.5, "time")
num("rev_pre", "Reverb Pre", "lin", 0, 0.25, 0.02, "time")
num("rev_damp", "Reverb Damp", "log", 1000, 20000, 6000, "hz")
num("rev_lc", "Reverb Lo Cut", "log", 20, 1000, 150, "hzlo")
num("rev_mod", "Reverb Mod", "lin", 0, 1, 0.3, "pct")
num("rev_width", "Reverb Width", "lin", 0, 1, 1, "pct")
enum("rev_freeze", "Reverb Freeze", ON_OFF, "Off")
num("rev_mix", "Reverb Mix", "lin", 0, 1, 0.3, "pct")
num("rev_shim", "Shimmer", "lin", 0, 1, 0, "pct")                         # dsp/reverb.h: the pitched tail
enum("rev_shim_int", "Shimmer Pitch", ["+12", "+7", "+19", "-12"], "+12")

# Pulse (dsp/pulse.h): tremolo, auto-pan, a rhythmic gate. PULSE_PATTERNS = Pulse::kPatternNames.
PULSE_PATTERNS = ["1/16", "1/8", "1/4", "Offbeat", "Off 16ths", "Dotted", "Tresillo", "Gallop", "Rev Gallop",
                  "Trance 1", "Trance 2", "Trance 3", "Pump", "Stutter", "Half Bar", "Build"]
enum("pls_on", "Pulse On", ON_OFF, "Off")
enum("pls_mode", "Pulse Mode", ["Tremolo", "Auto-Pan", "Gate"], "Tremolo")
enum("pls_sync", "Pulse Sync", FREE_SYNC, "Sync")
num("pls_rate", "Pulse Rate", "log", 0.1, 20, 4, "lfohz")
enum("pls_div", "Pulse Div", LFO_DIVS, "1/8")
popup_flag("pls_div")
num("pls_depth", "Pulse Depth", "lin", 0, 1, 1, "pct")
num("pls_shape", "Pulse Shape", "lin", 0, 1, 0, "pct")
num("pls_stereo", "Pulse Stereo", "lin", 0, 180, 0, "deg")
enum("pls_pattern", "Gate Pattern", PULSE_PATTERNS, "1/16")
popup_flag("pls_pattern")
num("pls_length", "Gate Length", "lin", 0.05, 1, 0.5, "pct")
num("pls_smooth", "Gate Smooth", "log", 0.0005, 0.05, 0.003, "time")
num("pls_mix", "Pulse Mix", "lin", 0, 1, 1, "pct")

# Grain (dsp/grain.h): the last seconds of input replayed as grains and slices, on MPC's beat.
enum("grn_on", "Grain On", ON_OFF, "Off")
enum("grn_mode", "Grain Mode", ["Cloud", "Stretch", "Mosaic", "Stutter", "Arp"], "Cloud")
enum("grn_sync", "Grain Sync", FREE_SYNC, "Sync")
num("grn_size", "Grain Size", "log", 0.01, 1, 0.12, "time")
enum("grn_div", "Grain Div", DELAY_DIVS, "1/16")
popup_flag("grn_div")
num("grn_density", "Grain Density", "lin", 0, 1, 0.5, "pct")
num("grn_pitch", "Grain Pitch", "int", -24, 24, 0, "semi")
num("grn_reverse", "Grain Reverse", "lin", 0, 1, 0, "pct")
num("grn_spread", "Grain Spread", "lin", 0, 1, 0.5, "pct")
num("grn_fb", "Grain FB", "lin", 0, 0.95, 0, "pct")
enum("grn_hold", "Grain Hold", ON_OFF, "Off")
num("grn_mix", "Grain Mix", "lin", 0, 1, 0.5, "pct")

# --- modulation (dsp/mod.h): two LFOs, the envelope follower, the matrix ---
LFO_WAVES = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth"]   # dsp/mod.h LfoWave
for l, rate in ((1, 1.0), (2, 0.25)):
    p = "l%d_" % l
    enum(p + "wave", "LFO %d Wave" % l, LFO_WAVES, "Sine")
    popup_flag(p + "wave")
    enum(p + "sync", "LFO %d Sync" % l, FREE_SYNC, "Free")
    num(p + "rate", "LFO %d Rate" % l, "log", 0.01, 20, rate, "lfohz")
    enum(p + "div", "LFO %d Div" % l, LFO_DIVS, "1 bar")
    popup_flag(p + "div")
    num(p + "phase", "LFO %d Phase" % l, "lin", 0, 360, 0, "deg")
num("env_att", "Env Attack", "log", 0.001, 0.5, 0.01, "time")
num("env_rel", "Env Release", "log", 0.01, 3, 0.2, "time")
num("env_gain", "Env Gain", "lin", 0, 36, 12, "db")

MOD_SOURCES = ["Off", "Macro 1", "Macro 2", "Macro 3", "Macro 4", "LFO 1", "LFO 2", "Envelope"]   # dsp/mod.h ModSource
# What the matrix reaches: continuous parameters only, by key; the options show their names.
MOD_TARGET_KEYS = ["in_gain", "out_gain", "mix",
                   "drv_amt", "drv_tone", "drv_bias", "drv_mix",
                   "flt_cut", "flt_res", "flt_drive", "flt_spread", "flt_mix",
                   "eq_lf", "eq_lg", "eq_mf", "eq_mg", "eq_hf", "eq_hg",
                   "cmp_thr", "cmp_makeup", "cmp_mix", "ott_depth", "ott_up", "ott_down",
                   "chr_rate", "chr_depth", "chr_mix",
                   "phs_rate", "phs_depth", "phs_center", "phs_fb", "phs_mix",
                   "dly_time", "dly_fb", "dly_spread", "dly_lc", "dly_hc", "dly_wow", "dly_duck", "dly_mix",
                   "rev_size", "rev_decay", "rev_damp", "rev_mod", "rev_width", "rev_mix", "rev_shim",
                   "pls_rate", "pls_depth", "grn_density", "grn_pitch", "grn_mix",
                   "l1_rate", "l2_rate"]
_names = {p["key"]: p["name"] for p in P}
MOD_TARGETS = ["Off"] + [_names[k] for k in MOD_TARGET_KEYS]
for k in range(1, MOD_SLOTS + 1):
    p = "m%d_" % k
    enum(p + "src", "Mod %d Source" % k, MOD_SOURCES, "Off")
    popup_flag(p + "src")
    enum(p + "dst", "Mod %d Target" % k, MOD_TARGETS, "Off")
    popup_flag(p + "dst")
    num(p + "amt", "Mod %d Amt" % k, "lin", -1, 1, 0, "bipct")

# --- presets ---
stepper("preset", "Preset")
button("pre_save", "Save Preset")
button("pre_init", "Init Preset")

# --- the preset browser ---
for i in range(1, BROWSER_CATS + 1):
    tile("cat_%d" % i, "Category %d" % i)
button("cat_prev", "Categories Prev")
button("cat_next", "Categories Next")
for i in range(1, BROWSER_ITEMS + 1):
    tile("item_%d" % i, "Item %d" % i)
button("item_prev", "Items Prev")
button("item_next", "Items Next")
readout("item_page", "Items Page")
readout("br_now", "Loaded")
toggle("fav", "Favorite")
button("rnd", "Random Pick")


def norm(p):
    """The default as MPC's 0..1 value."""
    lo, hi, d = p["lo"], p["hi"], p["default"]
    if p["curve"] == "log":
        return math.log(d / lo) / math.log(hi / lo)
    if p["curve"] == "pow":
        return (d / hi) ** (1.0 / 3.0) if hi > 0 else 0.0
    return (d - lo) / (hi - lo) if hi > lo else 0.0


def params_json():
    out = []
    for p in P:
        k = p["kind"]
        e = {"key": p["key"], "name": p["name"]}
        if k == "readout":
            e.update(min=0, max=0, display="string", type="readout")
        elif k == "stepper":
            e.update(min=0, max=1, display="string", type="stepper")
        elif k == "button":
            e.update(min=0, max=1, momentary=True, type="trigger")
        elif k == "popup":
            e.update(options=p["options"], default=p["options"][0], popup_of=p["popup_of"], type="enum")
        elif k == "tile":
            e.update(options=p["options"], default=p["options"][0], display="string")
        elif "options" in p:
            e.update(options=p["options"], default=p["options"][p["default"]])
        else:
            e.update(min=0, max=1, default=round(norm(p), 6), display="string")
        out.append(e)
    return {"name": VST["name"], "params": out}


# --- touchscreen pages -------------------------------------------------------------------------
# PolyForce's look (style=td3 rounded cards on one flat colour; bg and box the same, so the opaque image of a
# control never shows a box behind it), in EffectForce's violet. Plugin area 1280x628 at y = 86..714. Every
# page: a header row (the status line from x=24; a page's own choice or the preset stepper right-aligned to
# x=1256), then cards at y=158 and y=440 (h=270) or one full-height card (h=552). Nothing may sit in a card's
# title band (y .. y+44: td3 draws the title rule at y+38). skin_polish.py (run by `make skin` after the
# generator) redraws the knob strips, the trigger buttons and the stepper arrows, and makes the pages of a
# group sub-pages of one tab.
PALETTE = {
    "bg": "15171c", "box": "15171c", "line": "2d3038", "ink": "e9e9f0", "ink_dim": "9a9aa8",
    "ink_faint": "282a31", "accent": "a08cf5", "accent_hi": "cdc2ff", "lcd": "0c0d10",
    "seg_inactive": "212329", "seg_active": "a08cf5", "seg_active_tx": "17112e", "tile_on": "2a2443",
    "btn_bg": "2b2d34", "title": "aaa8b8", "knob_face": "26282e", "knob_ring": "3c3f47", "knob_dot": "a08cf5",
}
FONT_LABEL = "fonts/TitilliumWeb-SemiBold.ttf"   # font_label=: shadow_skin sizes the buttons with it
LIVE_FONT = "fonts/TitilliumWeb-SemiBold.ttf"    # the face MPC draws live text in (names, values)
TITLE_FONT = "fonts/TitilliumWeb-Bold.ttf"       # = SHADOW_TITLE_FONT in the Makefile: card titles, enum
                                                 # labels and segment, popup-option and button text
TITLE_SIZE = 17
THEME = ("style=td3\nfont_label=%s\ntitle_size=%d\n" % (FONT_LABEL, TITLE_SIZE)
         + "".join("theme_%s=%s\n" % kv for kv in PALETTE.items()))
TEXT_INK = PALETTE["ink_dim"]   # free bitmap text (column headers, slot numbers, hints)

S8 = [100, 252, 404, 556, 708, 860, 1012, 1164]   # 8 knob slots across a card = one Q-Link bank
L4, R4 = S8[:4], [724, 876, 1028, 1180]           # 4 slots in the left / right half card
R1, R2 = 158, 440                                 # card rows (h=270), or R1 with h=552

# Knobs: shadow_skin bakes ONE filmstrip per radius, so the radius picks the look. A bipolar knob (its arc
# grows from 12 o'clock) is one pixel smaller than a unipolar knob of the same size. This table is the only
# place that says so: check_layout() holds every knob to it and skin_polish.py draws the strips from it
# (exported as build/skin_style.json).
KNOB_SIZES = {"big": 30, "small": 22}
KNOB_STYLES = {r - b: {"bipolar": bool(b), "track": 4 if r - b >= 28 else 3, "pointer": 3.0 if r - b >= 28 else 2.5}
               for r in KNOB_SIZES.values() for b in (0, 1)}
BIPOLAR_EXTRA = ()                                # every bipolar knob here has a symmetric range
PRIMARY_BUTTONS = ("pre_save",)                   # drawn in the accent colour by skin_polish.py
FRAMES = 128                                      # shadow_skin: every filmstrip has 128 frames
PARAMS = {p["key"]: p for p in P}


def bipolar(key):
    """A knob whose value runs both ways from the middle (tilt, gains, amounts): symmetric range."""
    p = PARAMS[key]
    return p["lo"] == -p["hi"] or key in BIPOLAR_EXTRA


def knob_radius(key, size="big"):
    return KNOB_SIZES[size] - (1 if bipolar(key) else 0)


class Layout:
    """layout.conf lines, one generator [tab] per page. Pages are grouped: a group is one button of MPC's tab strip
    and its pages are that button's sub-pages (the dots under it; a tap on the button again shows the next), each
    with its own screen and its own Q-Link set. skin_polish.py renumbers the generator's tabs into these groups
    (skin_style.json "tab_groups"). mode() tags every widget that follows with when= until the next page or mode()."""

    def __init__(self):
        self.lines, self.when, self.groups = [THEME], None, []

    def group(self, name):
        self.groups.append({"name": name, "pages": []})

    def page(self, name, qlinks):
        """A page of the current group. Its name is also its Q-Link set's title, which MPC shows in the tab strip
        while the page is up."""
        self.lines.append("[tab %s]" % name)
        self.lines.append('qlinks "%s" = %s' % (name, ",".join(qlinks)))
        self.groups[-1]["pages"].append(name)
        self.when = None

    def mode(self, when):
        self.when = when

    def add(self, line):
        self.lines.append(line + (' when="%s"' % self.when if self.when else ""))

    def header(self, modes=None, status_w=None):
        """The status line from x=24 and the page-mode selector right-aligned to x=1256."""
        n = len(PARAMS[modes]["options"]) if modes else 0
        w = status_w or (1232 - n * 124 - 16 if n else 1232)
        self.readout(24 + w // 2, 121, w, "status")
        if n:
            self.hseg(1256 - (n * 124 - 2) // 2, 121, modes, 122)

    def card(self, x, y, w, h, title):
        self.add('frame x=%d y=%d w=%d h=%d title="%s"' % (x, y, w, h, title))

    def knob(self, cx, cy, key, size="big"):
        self.add('knob cx=%d cy=%d r=%d label="%s" key=%s' % (cx, cy, knob_radius(key, size), PARAMS[key]["name"], key))

    def hseg(self, cx, cy, key, sw, label=None):
        self.add('enum_h cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def vseg(self, cx, cy, key, sw=124, label=None):
        self.add('enum_v cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def popup(self, cx, cy, w, key):
        self.add('popup cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

    def stepper(self, cx, cy, w, key):
        self.add('stepper cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

    def readout(self, cx, cy, w, key, h=40):
        self.add('readout cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, h, key))

    def button(self, cx, cy, label, key):
        self.add('button cx=%d cy=%d label="%s" key=%s' % (cx, cy, label, key))

    def text(self, cx, top, label):   # bitmap font; top = the top of the glyphs (render_conf_preview.c)
        self.add('text cx=%d cy=%d label="%s" color=%s' % (cx, top, label, TEXT_INK))

    def slider(self, cx, cy, w, h, cw, key):
        self.add('slider_v cx=%d cy=%d w=%d h=%d cw=%d label="%s" key=%s' % (cx, cy, w, h, cw, PARAMS[key]["name"], key))

    def toggle(self, cx, cy, key):
        self.add('toggle cx=%d cy=%d label="%s" key=%s' % (cx, cy, PARAMS[key]["name"], key))

    def meter(self, cx, cy, w, h, key):   # display only: shadow_skin's filmstrip meter, no look (PolyForce patch)
        self.add('meter cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, h, key))

    def tiles(self, x, y, w, cols, rows, th, gap, key):
        self.add('list x=%d y=%d w=%d cols=%d rows=%d th=%d gap=%d key=%s' % (x, y, w, cols, rows, th, gap, key))


def on_seg(L, cx, top, key):
    """A module's Off / On switch, at the left of its card."""
    L.vseg(cx, top + 160, key, sw=110, label="ON")


def hseg_rows(L, cx, cy, key, sw, rows, label):
    """Segments in `rows` rows, the first centred on cy (shadow_skin enum_h rows=)."""
    L.add('enum_h cx=%d cy=%d sw=%d rows=%d key=%s label="%s"' % (cx, cy, sw, rows, key, label))


def bank(keys, pad=()):
    """A Q-Link bank: the Force's 8 knobs. The first bank is a page's top card, the second its bottom card;
    a top card with fewer than 8 controls is filled from `pad` so the bottom card starts on the second bank."""
    keys = list(keys)
    for k in pad:
        if len(keys) >= 8:
            break
        keys.append(k)
    assert len(keys) <= 8, keys
    return keys


def rate_or_div(L, cx, cy, sync, rate, div):
    """A free rate (or time) knob and its synced division in one place: each shows in its own mode."""
    L.mode("%s:Free" % sync)
    L.knob(cx, cy, rate)
    L.mode("%s:Sync" % sync)
    L.popup(cx, cy, 170, div)
    L.mode(None)


def build_layout():
    """Every page, in groups (see Layout). Each page has its own Q-Link set: the Force's 8 knobs show the first 8
    keys, the next bank the other 8 (shadow_skin qlink_for_slot); a bank is a card (bank())."""
    L = Layout()
    levels = ("in_gain", "out_gain", "mix")

    # CHAIN: the order and the levels; the presets.
    L.group("CHAIN")
    L.page("CHAIN", ["mac_1", "mac_2", "mac_3", "mac_4", "in_gain", "out_gain", "mix", "preset"])
    L.header(status_w=700)
    L.stepper(998, 121, 516, "preset")
    L.card(24, R1, 1232, 270, "CHAIN")
    L.tiles(44, 206, 1192, 5, 2, 52, 8, "slot")   # the order reads left to right, then the second row
    L.button(380, R1 + 220, "< MOVE", "move_l")
    L.button(640, R1 + 220, "ON / OFF", "sel_on")
    L.button(900, R1 + 220, "MOVE >", "move_r")
    L.card(24, R2, 608, 270, "LEVELS")
    for cx, k in zip(L4, levels):
        L.knob(cx, R2 + 126, k)
    L.card(648, R2, 608, 270, "MACROS")
    for cx, k in zip(R4, ("mac_1", "mac_2", "mac_3", "mac_4")):
        L.knob(cx, R2 + 126, k)

    # The browser has no knobs of its own: the Q-Links keep the preset stepper, the macros and the levels.
    L.page("PRESETS", ["preset", "mac_1", "mac_2", "mac_3", "mac_4", "in_gain", "out_gain", "mix"])
    L.header()
    L.card(24, R1, 360, 552, "CATEGORIES")
    L.tiles(44, 206, 320, 2, 8, 48, 8, "cat")
    L.button(124, 676, "< PREV", "cat_prev")
    L.button(304, 676, "NEXT >", "cat_next")
    L.card(400, R1, 856, 474, "PRESETS")
    L.tiles(420, 206, 816, 3, 8, 38, 8, "item")
    L.button(476, 596, "< PREV", "item_prev")
    L.readout(828, 596, 240, "item_page", h=36)
    L.button(1180, 596, "NEXT >", "item_next")
    L.readout(590, 676, 380, "br_now")   # the row: 400..1256, 8 px apart
    L.toggle(848, 668, "fav")
    L.button(968, 676, "RND", "rnd")
    L.button(1083, 676, "SAVE", "pre_save")
    L.button(1197, 676, "INIT", "pre_init")

    # TONE: drive and filter, the EQ, the compressor.
    L.group("TONE")
    drv, flt = ("drv_amt", "drv_tone", "drv_bias", "drv_out", "drv_mix"), ("flt_cut", "flt_res", "flt_drive",
                                                                           "flt_spread", "flt_mix")
    L.page("DRIVE+FILTER", bank(drv + ("drv_type", "drv_on"), pad=("out_gain",)) + list(flt) + ["flt_type", "flt_on"])
    L.header()
    for top, title, p, keys in ((R1, "DRIVE", "drv_", drv), (R2, "FILTER", "flt_", flt)):
        L.card(24, top, 1232, 270, title)
        on_seg(L, S8[0], top, p + "on")
        hseg_rows(L, 330, top + 130, p + "type", 96, 2, "TYPE")
        for cx, k in zip(S8[3:], keys):
            L.knob(cx, top + 126, k)

    eq7 = ("eq_lf", "eq_lg", "eq_mf", "eq_mg", "eq_mq", "eq_hf", "eq_hg")
    L.page("EQ", bank(eq7 + ("eq_on",)) + ["eq_lc", "eq_hc"])
    L.header()
    L.card(24, R1, 1232, 270, "EQ")
    on_seg(L, S8[0], R1, "eq_on")
    for cx, k in zip(S8[1:], eq7):
        L.knob(cx, R1 + 126, k)
    L.card(24, R2, 608, 270, "CUTS")
    for cx, k in zip(L4, ("eq_lc", "eq_hc")):
        L.knob(cx, R2 + 126, k)

    # COMP: the mode picks the card (the other mode's knobs would do nothing); Makeup works in both.
    comp7 = ("cmp_thr", "cmp_ratio", "cmp_att", "cmp_rel", "cmp_knee", "cmp_sc", "cmp_mix")
    ott7 = ("ott_depth", "ott_time", "ott_up", "ott_down", "ott_low", "ott_mid", "ott_high")
    L.page("COMP", bank(comp7 + ("cmp_on",)) + list(ott7) + ["cmp_makeup"])
    L.header("cmp_mode")
    for mode, title, keys in (("Comp", "COMPRESSOR", comp7), ("OTT", "OTT", ott7)):
        L.mode("cmp_mode:%s" % mode)
        L.card(24, R1, 1232, 270, title)
        on_seg(L, S8[0], R1, "cmp_on")
        for cx, k in zip(S8[1:], keys):
            L.knob(cx, R1 + 126, k)
    L.mode(None)
    L.card(24, R2, 608, 270, "OUTPUT")
    L.knob(L4[0], R2 + 126, "cmp_makeup")

    # MOTION: chorus and phaser.
    L.group("MOTION")
    chr6 = ("chr_rate", "chr_depth", "chr_delay", "chr_lc", "chr_width", "chr_mix")
    phs5 = ("phs_depth", "phs_center", "phs_fb", "phs_stereo", "phs_mix")
    L.page("CHORUS+PHASE", bank(chr6 + ("chr_mode", "chr_on"))
           + ["phs_rate", "phs_div", "phs_depth", "phs_center", "phs_fb", "phs_mix", "phs_mode", "phs_on"])
    L.header()
    L.card(24, R1, 1232, 270, "CHORUS")
    on_seg(L, S8[0], R1, "chr_on")
    L.vseg(S8[1], R1 + 170, "chr_mode", sw=124, label="MODE")
    for cx, k in zip(S8[2:], chr6):
        L.knob(cx, R1 + 126, k)
    L.card(24, R2, 1232, 270, "PHASER")
    on_seg(L, S8[0], R2, "phs_on")
    L.vseg(S8[1], R2 + 170, "phs_mode", sw=124, label="MODE")
    L.hseg(S8[2], R2 + 76, "phs_sync", 100)
    rate_or_div(L, S8[2], R2 + 170, "phs_sync", "phs_rate", "phs_div")
    for cx, k in zip(S8[3:], phs5):
        L.knob(cx, R2 + 170, k)

    pls5 = ("pls_depth", "pls_shape", "pls_stereo", "pls_mix")
    L.page("PULSE", bank(("pls_rate", "pls_div") + pls5 + ("pls_sync", "pls_on"))
           + ["pls_pattern", "pls_length", "pls_smooth", "pls_mode"])
    L.header("pls_mode")
    L.card(24, R1, 1232, 270, "PULSE")
    on_seg(L, S8[0], R1, "pls_on")
    L.hseg(S8[1] + 76, R1 + 76, "pls_sync", 100)
    rate_or_div(L, S8[2], R1 + 170, "pls_sync", "pls_rate", "pls_div")
    for cx, k in zip(S8[3:], pls5):
        L.knob(cx, R1 + 170, k)
    L.mode("pls_mode:Gate")
    L.card(24, R2, 1232, 270, "GATE")
    L.popup(S8[1] - 40, R2 + 126, 200, "pls_pattern")
    L.knob(S8[3], R2 + 126, "pls_length")
    L.knob(S8[4], R2 + 126, "pls_smooth")
    L.mode(None)

    # SPACE: delay, grain and reverb.
    L.group("SPACE")
    dly4 = ("dly_fb", "dly_spread", "dly_duck", "dly_mix")
    fbk4 = ("dly_lc", "dly_hc", "dly_drive", "dly_wow")
    L.page("DELAY", bank(("dly_time", "dly_div") + dly4 + ("dly_sync", "dly_on"))
           + list(fbk4) + ["dly_glide", "dly_mode", "out_gain", "mix"])
    L.header()
    L.card(24, R1, 1232, 270, "DELAY")
    on_seg(L, S8[0], R1, "dly_on")
    L.vseg(S8[1], R1 + 170, "dly_mode", sw=124, label="MODE")
    L.hseg(S8[2], R1 + 76, "dly_sync", 100)
    rate_or_div(L, S8[2], R1 + 170, "dly_sync", "dly_time", "dly_div")
    for cx, k in zip(S8[3:], dly4):
        L.knob(cx, R1 + 170, k)
    L.card(24, R2, 1232, 270, "REPEATS")
    for cx, k in zip(S8, fbk4):
        L.knob(cx, R2 + 126, k)
    L.vseg(S8[5], R2 + 160, "dly_glide", sw=124, label="TIME CHANGE")

    grn5 = ("grn_density", "grn_pitch", "grn_reverse", "grn_spread", "grn_mix")
    L.page("GRAIN", bank(("grn_size", "grn_div") + grn5 + ("grn_on",))
           + ["grn_fb", "grn_hold", "grn_sync", "grn_mode"])
    L.header()
    L.card(24, R1, 1232, 270, "GRAIN")
    on_seg(L, S8[0], R1, "grn_on")
    L.hseg(S8[1] + 76, R1 + 76, "grn_sync", 100)
    rate_or_div(L, S8[2], R1 + 170, "grn_sync", "grn_size", "grn_div")
    for cx, k in zip(S8[3:], grn5):
        L.knob(cx, R1 + 170, k)
    L.card(24, R2, 1232, 270, "TEXTURE")
    hseg_rows(L, 330, R2 + 130, "grn_mode", 96, 2, "MODE")
    L.knob(S8[4], R2 + 126, "grn_fb")
    L.vseg(S8[6], R2 + 160, "grn_hold", sw=110, label="HOLD")

    rev5 = ("rev_size", "rev_decay", "rev_pre", "rev_width", "rev_mix")
    rev3 = ("rev_damp", "rev_lc", "rev_mod")
    L.page("REVERB", bank(rev5 + ("rev_freeze", "rev_mode", "rev_on")) + list(rev3) + ["rev_shim", "rev_shim_int"])
    L.header()
    L.card(24, R1, 1232, 270, "REVERB")
    on_seg(L, S8[0], R1, "rev_on")
    L.vseg(S8[1], R1 + 170, "rev_mode", sw=124, label="MODE")
    for cx, k in zip(S8[2:], rev5):
        L.knob(cx, R1 + 126, k)
    L.vseg(S8[7], R1 + 160, "rev_freeze", sw=110, label="FREEZE")
    L.card(24, R2, 1232, 270, "TONE AND SHIMMER")
    for cx, k in zip(S8, rev3):
        L.knob(cx, R2 + 126, k)
    L.knob(S8[4], R2 + 126, "rev_shim")
    L.vseg(S8[5] + 20, R2 + 160, "rev_shim_int", sw=110, label="PITCH")

    # MOD: the LFOs, the envelope follower and the macros; the matrix.
    L.group("MOD")
    L.page("LFO+ENV", ["l1_rate", "l1_div", "l1_phase", "l1_wave", "l2_rate", "l2_div", "l2_phase", "l2_wave",
                       "env_att", "env_rel", "env_gain", "l1_sync", "mac_1", "mac_2", "mac_3", "mac_4"])
    L.header()
    for l, x in ((1, 24), (2, 648)):
        p = "l%d_" % l
        L.card(x, R1, 608, 270, "LFO %d" % l)
        L.popup(x + 130, R1 + 76, 200, p + "wave")
        L.hseg(x + 400, R1 + 76, p + "sync", 100)
        rate_or_div(L, x + 100, R1 + 170, p + "sync", p + "rate", p + "div")
        L.knob(x + 300, R1 + 170, p + "phase")
    L.card(24, R2, 608, 270, "ENVELOPE FOLLOWER")
    for cx, k in zip(L4, ("env_att", "env_rel", "env_gain")):
        L.knob(cx, R2 + 126, k)
    L.card(648, R2, 608, 270, "MACROS")
    for cx, k in zip(R4, ("mac_1", "mac_2", "mac_3", "mac_4")):
        L.knob(cx, R2 + 126, k)

    rows = [268 + 110 * r for r in range(4)]   # 110: a small knob's box is 110 tall
    L.page("MATRIX", ["m%d_amt" % k for k in range(1, MOD_SLOTS + 1)]
           + ["m%d_%s" % (k, s) for k in (1, 2, 3, 4) for s in ("src", "dst")])
    L.header()
    L.card(24, R1, 1232, 552, "MOD SLOTS")
    for x in (24, 648):
        for label, dx in (("SOURCE", 160), ("TARGET", 365), ("AMOUNT", 545)):
            L.text(x + dx, 202, label)
    for k in range(1, MOD_SLOTS + 1):
        x, cy = (24, 648)[(k - 1) // 4], rows[(k - 1) % 4]
        p = "m%d_" % k
        L.text(x + 40, cy - 8, str(k))
        L.popup(x + 160, cy, 150, p + "src")
        L.popup(x + 365, cy, 150, p + "dst")   # 150: the open list of 55 targets fits the screen in 8 columns
        L.knob(x + 545, cy, p + "amt", "small")
    return L


def pages():
    """layout.conf."""
    return "\n".join(build_layout().lines) + "\n"


def skin_style():
    """What skin_polish.py needs (build/skin_style.json): the palette, knob looks and primary buttons to redraw the
    knob strips, buttons and stepper arrows, and the page groups to renumber the generator's tabs into sub-pages."""
    return {"palette": PALETTE, "title_font": TITLE_FONT, "frames": FRAMES,
            "knobs": {str(r): s for r, s in sorted(KNOB_STYLES.items())}, "primary_buttons": list(PRIMARY_BUTTONS),
            "tab_groups": build_layout().groups}


# --- layout check (offline, no skin toolchain) ---------------------------------------------------
# Geometry mirrors third_party/mpc-vst-plugins/tools/shadow_skin.py (component boxes, button_rect,
# seg_rects, popup_layout) and render_conf_preview.c (the bitmap font of `text`).
X0, Y0, X1, Y1 = 0, 86, 1280, 714
TITLE_BAND = 44              # a card's title band: y .. y+44
NAME_MAX = 13                # MPC shows a knob/slider's effGetParamName at ~19.5 px in a 130 px box
MAX_IMAGE_H = 16384          # taller skin images draw wrongly (sd88me/mpc-vst-plugins catalog_check.py warns)
POP_ROW, POP_GAP, POP_PAD, POP_GROUP_ROWS = 40, 2, 6, 8
BITMAP_GLYPHS = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-/_>%+:#"   # font8x8.h font_chars
BITMAP_ADVANCE = {" ": 4, "J": 9, "j": 9, ".": 9, "-": 9, ":": 9}   # font_glyph_width(): last lit column + 2; else 10
INT_KEYS = ("x", "y", "w", "h", "cx", "cy", "r", "sw", "rows", "cols", "th", "gap", "cw")


def _widget(line):
    toks = shlex.split(line)
    w = {"kind": toks[0]}
    for t in toks[1:]:
        k, _, v = t.partition("=")
        w[k] = int(v) if k in INT_KEYS else v
    return w


def bitmap_width(s, scale):
    """render_conf_preview.c text_width()."""
    return int(sum(BITMAP_ADVANCE.get(c, 10) * scale for c in s))


_FONTS = {}


def _ttf_advances(path):
    """(unitsPerEm, {char: advance}) from a TrueType font's cmap (format 4) and hmtx tables."""
    import struct
    d = open(path, "rb").read()
    tabs = {}
    for i in range(struct.unpack(">H", d[4:6])[0]):
        tag, _, off, ln = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        tabs[tag.decode("latin-1")] = off
    upem = struct.unpack(">H", d[tabs["head"] + 18:tabs["head"] + 20])[0]
    nhm = struct.unpack(">H", d[tabs["hhea"] + 34:tabs["hhea"] + 36])[0]
    adv = [struct.unpack(">H", d[tabs["hmtx"] + 4 * i:tabs["hmtx"] + 4 * i + 2])[0] for i in range(nhm)]
    co = tabs["cmap"]
    for i in range(struct.unpack(">H", d[co + 2:co + 4])[0]):
        pid, eid, off = struct.unpack(">HHI", d[co + 4 + 8 * i:co + 12 + 8 * i])
        so = co + off
        if struct.unpack(">H", d[so:so + 2])[0] != 4 or (pid, eid) not in ((3, 1), (0, 3), (0, 4)):
            continue
        n2 = struct.unpack(">H", d[so + 6:so + 8])[0]
        ends = struct.unpack(">%dH" % (n2 // 2), d[so + 14:so + 14 + n2])
        starts = struct.unpack(">%dH" % (n2 // 2), d[so + 16 + n2:so + 16 + 2 * n2])
        deltas = struct.unpack(">%dh" % (n2 // 2), d[so + 16 + 2 * n2:so + 16 + 3 * n2])
        ro = so + 16 + 3 * n2
        ranges = struct.unpack(">%dH" % (n2 // 2), d[ro:ro + n2])
        out = {}
        for s in range(n2 // 2):
            for c in range(starts[s], min(ends[s], 0x7e) + 1):
                if ranges[s]:
                    gi = ro + 2 * s + ranges[s] + 2 * (c - starts[s])
                    g = struct.unpack(">H", d[gi:gi + 2])[0]
                    g = (g + deltas[s]) & 0xFFFF if g else 0
                else:
                    g = (c + deltas[s]) & 0xFFFF
                out[chr(c)] = adv[min(g, nhm - 1)]
        return upem, out
    raise SystemExit("%s: no Unicode cmap" % path)


def ttf_width(font, px, s):
    """Advance width of s in a bundled font at px pixels, measured with Pillow as shadow_skin does. Without
    Pillow (surface.py needs only python3): from the font's own advance table, +1 px (that is within 0.75 px
    of Pillow for Titillium Web, and errs wide)."""
    key = (font, px)
    if key not in _FONTS:
        path = os.path.join(HERE, font)
        try:
            from PIL import ImageFont
            _FONTS[key] = ImageFont.truetype(path, px).getlength
        except ImportError:
            upem, adv = _ttf_advances(path)
            _FONTS[key] = lambda t: sum(adv.get(c, upem) for c in t) * px / upem + 1.0
    return _FONTS[key](s)


def _top_level(text):
    """The style keys before the first [tab] (shadow_skin apply_theme)."""
    top = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("["):
            break
        k, eq, v = line.partition("=")
        if eq and not line.startswith("#"):
            top[k.strip()] = v.strip()
    return top


class Geometry:
    """Where shadow_skin puts each widget, for this layout's style keys."""

    def __init__(self, top):
        self.td3 = top.get("style") == "td3"
        self.font_label = top.get("font_label")
        self.ls = float(top.get("label_scale", 1.15))

    def knob(self, w):   # shadow_skin build(): the filmstrip, the Name and Value labels under it
        r = w["r"]
        s = 2 * r + 10
        cw = max(130, s)
        name_y = s // 2 + r + 2
        ch = name_y + round(20 * self.ls) + 2 + round(26 * self.ls) + 6
        return (w["cx"] - cw // 2, w["cy"] - s // 2, cw, ch)

    def slider(self, w):
        sq = max(w["w"], w["h"])
        cw = w.get("cw", max(130, sq))
        ch = (sq - w["h"]) // 2 + w["h"] + 2 + 20 + 2 + 26 + 6
        return (w["cx"] - cw // 2, w["cy"] - sq // 2, cw, ch)

    def text_width(self, s):   # shadow_skin text_width(): sizes a button
        if self.font_label:
            return int(ttf_width(self.font_label, round(9 * 1.15 * 1.6), s) * 1.2)
        return int(len(s) * 10 * 1.15 - 1.15)

    def button(self, w):   # shadow_skin button_rect()
        bw, bh = self.text_width(w["label"]) + 36, 39
        if self.td3:
            bw, bh = bw + 24 + 4, 48 + 4
        return (w["cx"] - bw // 2, w["cy"] - bh // 2, bw, bh)

    @staticmethod
    def segs(w, n):   # shadow_skin seg_rects()
        if w["kind"] == "enum_v":
            sw = w.get("sw") or 135
            y0 = w["cy"] - (n * 32) // 2
            return [(w["cx"] - sw // 2, y0 + i * 32, sw, 30) for i in range(n)]
        sw, rows = w.get("sw") or 117, w.get("rows", 1)
        per = -(-n // rows)
        out = []
        for i in range(n):
            r, c = divmod(i, per)
            cnt = min(per, n - r * per)
            out.append((w["cx"] - (cnt * sw + (cnt - 1) * 2) // 2 + c * (sw + 2), w["cy"] - 16 + r * 35, sw, 33))
        return out

    @staticmethod
    def enum_label(w, n):   # the TrueType group label shadow_skin draws centred at (gx, gy), 18 px
        gy = w["cy"] - 33 // 2 - 22 if w["kind"] == "enum_h" else w["cy"] - (n * 32) // 2 - 24
        tw = int(ttf_width(TITLE_FONT, 18, w["label"])) + 2
        return (w["cx"] - tw // 2, gy - 10, tw, 20)

    @staticmethod
    def text(w):   # render_conf_preview.c draw_text_c(): cx centres, cy is the TOP of the glyphs
        size = float(w.get("size", 1.5))
        tw = bitmap_width(w["label"], size)
        return (w["cx"] - tw // 2, w["cy"], tw + 1, int(9 * size + 0.5))

    @staticmethod
    def popup_panel(w, n):   # shadow_skin popup_layout(): the open list
        fx, fy, fw, fh = w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"]
        below, above = Y1 - (fy + fh + 4), fy - 4 - Y0
        groups = [(t, int(c)) for t, _, c in (g.rpartition(":") for g in w["groups"].split(","))] \
            if w.get("groups") else None
        if groups:
            rows = min(POP_GROUP_ROWS, max(c for _, c in groups))
            cols = sum(-(-c // rows) for _, c in groups)
            ph = (rows + 1) * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
        else:
            for cols in ([int(w["cols"])] if w.get("cols") else range(1, n + 1)):
                rows = -(-n // cols)
                ph = rows * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
                if ph <= max(below, above):
                    break
        pw = cols * fw + (cols - 1) * POP_GAP + 2 * POP_PAD
        py = fy + fh + 4 if ph <= below else fy - 4 - ph if ph <= above else Y0
        return (max(0, min(fx, X1 - pw)), py, pw, ph)

    def rects(self, w, params):
        """[(x, y, w, h)] of a control, as shadow_skin places it (stepper: arrows and text together)."""
        k = w["kind"]
        if k == "knob":
            return [self.knob(w)]
        if k in ("slider_v", "slider_h"):
            return [self.slider(w)]
        if k == "toggle":
            return [(w["cx"] - 60, w["cy"] - 18, 120, 58)]
        if k == "button":
            return [self.button(w)]
        if k in ("readout", "stepper", "popup", "menu"):
            return [(w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"])]
        if k == "list":
            tw = (w["w"] - (w["cols"] - 1) * w["gap"]) // w["cols"]
            return [(w["x"] + c * (tw + w["gap"]), w["y"] + r * (w["th"] + w["gap"]), tw, w["th"])
                    for r in range(w["rows"]) for c in range(w["cols"])]
        if k in ("enum_h", "enum_v"):
            return self.segs(w, len(params[w["key"]]["options"]))
        if k == "meter":   # shadow_skin: a square of the larger side, centred (transparent padding)
            sq = max(w["w"], w["h"])
            return [(w["cx"] - sq // 2, w["cy"] - sq // 2, sq, sq)]
        return []


def _overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def _inside(r):
    return r[0] >= X0 and r[1] >= Y0 and r[0] + r[2] <= X1 and r[1] + r[3] <= Y1


def _same_screen(m1, m2):
    return m1 is None or m2 is None or m1 == m2


def check_names(layout_tabs, geo, errors):
    """Parameter names: MPC shows them under knobs and sliders and in its Q-Link overlay, without page context."""
    seen = {}
    for p in P:
        if len(p["name"]) > 24:
            errors.append("parameter %s: name %r is longer than 24 characters" % (p["key"], p["name"]))
        if p["name"].lower() in seen:
            errors.append("parameters %s and %s have the same name %r" % (seen[p["name"].lower()], p["key"], p["name"]))
        seen[p["name"].lower()] = p["key"]
    for tab in layout_tabs:
        for w in tab["widgets"]:
            if w["kind"] not in ("knob", "slider_v", "slider_h", "toggle") or w.get("key") not in PARAMS:
                continue
            name = PARAMS[w["key"]]["name"]
            if len(name) > NAME_MAX:
                errors.append("%s: %s %s: name %r is longer than %d characters" % (tab["name"], w["kind"], w["key"], name,
                                                                                    NAME_MAX))
            # the live Name label: knob 17 x label_scale px in max(130, 2r+10); slider 17 px in cw; toggle 15 px in 120
            px, box = ((math.ceil(17 * geo.ls), max(130, 2 * w["r"] + 10)) if w["kind"] == "knob" else
                       (15, 120) if w["kind"] == "toggle" else (17, w.get("cw") or max(130, w["w"], w["h"])))
            if ttf_width(LIVE_FONT, px, name) > box - 4:
                errors.append("%s: %s %s: name %r does not fit its %d px label" % (tab["name"], w["kind"], w["key"], name, box))


def check_layout(text, groups):
    """Raise SystemExit on anything shadow_skin.py would refuse, plus geometry mistakes: outside the plugin
    area, overlaps on one screen (a page mode with everything shown in every mode), controls or text in a
    card's title band, open popup lists that leave the plugin area, unknown bitmap glyphs; and the page groups
    (Layout): every page in one group, in layout order, with exactly one Q-Link set titled like the page."""
    params = PARAMS
    geo = Geometry(_top_level(text))
    errors = []
    tabs = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or (not tabs and "=" in line and not line.startswith("[")):
            continue
        m = re.match(r"\[tab (.+)\]$", line)
        if m:
            tabs.append({"name": m.group(1), "widgets": [], "qlinks": []})
            continue
        if line.startswith("qlinks"):
            m = re.match(r'qlinks\s+"([^"]+)"\s*=\s*(.+)$', line)
            keys = [k.strip() for k in m.group(2).split(",") if k.strip()]
            tabs[-1]["qlinks"].append((m.group(1), keys))
            continue
        tabs[-1]["widgets"].append(_widget(line))
    if len(groups) > 7:
        errors.append("%d page groups: MPC's tab strip shows five plus a pager; keep it to seven" % len(groups))
    errors += ["page group %s has no pages" % g["name"] for g in groups if not g["pages"]]
    grouped, names = [n for g in groups for n in g["pages"]], [t["name"] for t in tabs]
    if grouped != names:
        errors.append("the page groups list %s, the layout has the pages %s" % (grouped, names))
    errors += ["page %r: the name is taken (MPC tells pages apart by it)" % n for n in sorted(set(names))
               if names.count(n) > 1]
    for t in tabs:
        if [title for title, _ in t["qlinks"]] != [t["name"]]:
            errors.append("%s: a page has exactly one Q-Link set, titled like the page" % t["name"])
    check_names(tabs, geo, errors)
    errors += ["PRIMARY_BUTTONS: %r is not a button parameter" % k for k in PRIMARY_BUTTONS
               if PARAMS.get(k, {}).get("kind") != "button"]
    errors += ["BIPOLAR_EXTRA: %r is not a parameter" % k for k in BIPOLAR_EXTRA if k not in PARAMS]
    seg_images = {}   # shadow_skin names enum images sh_seg_<key>_<n> for the whole skin: one size per key
    for tab in tabs:
        T = tab["name"]
        frames, placed = [], []   # (rect, title, mode); (rect, what, mode)
        for w in tab["widgets"]:
            kind, key, mode = w["kind"], w.get("key"), w.get("when")
            if mode:
                mk, _, mo = mode.partition(":")
                opts = [o.lower() for o in params.get(mk, {}).get("options", [])]
                if len(opts) < 2 or mo.lower() not in opts:
                    errors.append("%s: when=%s is not an option of an option parameter" % (T, mode))
            if kind == "frame":
                r = (w["x"], w["y"], w["w"], w["h"])
                if not _inside(r):
                    errors.append("%s: frame %r at %s leaves the plugin area" % (T, w.get("title"), r))
                frames.append((r, w.get("title", ""), mode))
                continue
            if kind == "text":
                lab = w.get("label", "")
                bad = sorted(set(c for c in lab if c not in BITMAP_GLYPHS))
                if not lab or bad:
                    errors.append("%s: text %r: %s" % (T, lab, "the bitmap font has no %r" % "".join(bad) if bad
                                                       else "an empty label fails shadow_art"))
                placed.append((Geometry.text(w), "text %r" % lab, mode))
                continue
            if kind == "art":
                errors.append("%s: art needs the browser renderer" % T)
                continue
            need = ["%s_%d" % (key, i + 1) for i in range(w["cols"] * w["rows"])] if kind == "list" else [key]
            if kind == "stepper":
                need += [key + "_prev", key + "_next"]
            if kind == "popup":
                need.append(key + "__open")
            missing = [k for k in need if k not in params]
            for k in missing:
                errors.append("%s: %s key %r is not a parameter" % (T, kind, k))
            if missing:
                continue
            p = params[need[0]]
            if kind in ("enum_h", "enum_v", "popup") and "options" not in p:
                errors.append("%s: %s %r is not an option parameter" % (T, kind, key))
                continue
            if kind == "list" and p["kind"] != "tile":
                errors.append("%s: list %r tiles must be tile parameters" % (T, key))
            if kind == "stepper" and p["kind"] != "stepper":
                errors.append("%s: stepper %r is not a stepper parameter" % (T, key))
            if kind == "meter" and p["kind"] != "meter":
                errors.append("%s: meter %r is not a meter parameter" % (T, key))
            if kind == "button" and not w.get("label"):
                errors.append("%s: button %r needs a label" % (T, key))
                continue
            strip = {"knob": lambda: 2 * w["r"] + 10, "slider_v": lambda: max(w["w"], w["h"]),
                     "slider_h": lambda: max(w["w"], w["h"]), "meter": lambda: max(w["w"], w["h"])}.get(kind)
            if strip and strip() * FRAMES > MAX_IMAGE_H:   # FRAMES square frames stacked: one tall image
                errors.append("%s: %s %s: its filmstrip is %d px tall; MPC draws images over %d px wrongly" % (
                    T, kind, key, strip() * FRAMES, MAX_IMAGE_H))
            if kind == "knob":
                style = KNOB_STYLES.get(w["r"])
                if not style:
                    errors.append("%s: knob %s: r=%d has no look in KNOB_STYLES" % (T, key, w["r"]))
                elif style["bipolar"] != bipolar(key):
                    errors.append("%s: knob %s: r=%d is a %s look, the parameter is %s" % (
                        T, key, w["r"], "bipolar" if style["bipolar"] else "unipolar",
                        "bipolar" if bipolar(key) else "unipolar"))
            if kind in ("readout", "stepper", "popup") and w["h"] < 36:
                errors.append("%s: %s %s: h=%d clips its 26 px live text (min 36)" % (T, kind, key, w["h"]))
            if kind in ("enum_h", "enum_v"):
                n = len(p["options"])
                size = (kind, w.get("sw"), n)
                if seg_images.setdefault(key, size) != size:
                    errors.append("%s: enum %s is drawn as %s and %s: its segment images are shared" % (
                        T, key, seg_images[key], size))
                if w.get("label"):
                    placed.append((Geometry.enum_label(w, n), "%s label" % key, mode))
            if kind == "popup":
                panel = Geometry.popup_panel(w, len(p["options"]))
                if not _inside(panel):
                    errors.append("%s: popup %s: its open list %s leaves the plugin area" % (T, key, panel))
            for r in geo.rects(w, params):
                placed.append((r, "%s %s" % (kind, key), mode))
        for i, (r, what, mode) in enumerate(placed):
            if not _inside(r):
                errors.append("%s: %s at %s leaves the plugin area" % (T, what, r))
            for o_r, o_what, o_mode in placed[:i]:
                if what.startswith("meter ") and o_what.startswith("meter "):
                    continue   # a row of meters: their padded squares overlap, transparent and untouchable
                if _same_screen(mode, o_mode) and _overlap(r, o_r) and o_what != what:
                    errors.append("%s: %s overlaps %s" % (T, what, o_what))
            for f_r, title, f_mode in frames:
                band = (f_r[0], f_r[1], f_r[2], TITLE_BAND)
                if _same_screen(mode, f_mode) and _overlap(r, band):
                    errors.append("%s: %s at %s is in the title band of card %r" % (T, what, r, title))
        titles = [t for t, _ in tab["qlinks"]]
        for title, keys in tab["qlinks"]:
            if len(keys) > 16:
                errors.append("%s: qlinks %r has %d keys (max 16)" % (T, title, len(keys)))
            if len(title) > 12 or titles.count(title) > 1:
                errors.append("%s: qlinks title %r: keep it unique and at most 12 characters (MPC's tab strip)" % (T, title))
            for k in keys:
                if k not in params:
                    errors.append("%s: qlinks %r key %r is not a parameter" % (T, title, k))
    if errors:
        raise SystemExit("layout check failed:\n  " + "\n  ".join(errors))
    return tabs


# --- C++ header --------------------------------------------------------------------------------
CURVE = {"readout": "Readout", "enum": "Enum", "lin": "Lin", "log": "Log", "int": "Int", "pow": "Pow"}
FMT = {"none": "None", "enum": "Enum", "pct": "Percent", "bipct": "Bipolar", "hz": "Hz", "hzlo": "HzLo",
       "hzhi": "HzHi", "time": "Time", "db": "Db", "ratio": "Ratio", "mult": "Mult", "q": "Q", "oct": "Oct",
       "deg": "Degrees", "lfohz": "LfoHz", "center": "Center", "count": "Count", "text": "Text", "semi": "Semi"}
KIND = {"synth": "Synth", "chain": "Chain", "ui": "Ui", "readout": "Readout", "stepper": "Stepper",
        "button": "Button", "tile": "Tile", "toggle": "Toggle", "popup": "Popup"}


def c_str(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def header():
    index = {p["key"]: i for i, p in enumerate(P)}
    ids = ",\n".join("    P_%s%s" % (p["key"].upper(), " = 0" if i == 0 else "") for i, p in enumerate(P))
    specs = ",\n".join("    {Curve::%s, Fmt::%s, %sf, %sf}  /* %s */" % (
        CURVE[p["curve"]], FMT[p["fmt"]], float(p["lo"]), float(p["hi"]), p["key"]) for p in P)
    opts = []
    for i, p in enumerate(P):
        if "options" in p:
            opts.append("static constexpr const char* OPTS_%d[] = {%s};" % (i, ", ".join(c_str(o) for o in p["options"])))
    info = ",\n".join("    {%s, %s, Kind::%s, %rf, %d, %s, %d}" % (
        c_str(p["key"]), c_str(p["name"]), KIND[p["kind"]], float(round(norm(p), 6)),
        len(p.get("options", [])), "OPTS_%d" % i if "options" in p else "nullptr",
        index[p["popup_of"]] if p["kind"] == "popup" else -1) for i, p in enumerate(P))
    modules = ", ".join("M_%s" % n.upper() for n in MODULE_NAMES)
    on_params = ", ".join("P_%s_ON" % pre.upper() for pre, _ in MODULES)
    targets = ", ".join(["-1"] + ["P_%s" % k.upper() for k in MOD_TARGET_KEYS])
    uid = int.from_bytes(VST["uid"].encode(), "big")
    return """// generated by surface/surface.py: do not edit
#pragma once
#include <cstdint>

namespace ef {

enum ParamId : int {
%s,
    P_COUNT
};

enum class Curve : unsigned char { Readout, Enum, Lin, Log, Int, Pow };
enum class Fmt : unsigned char { None, Enum, Percent, Bipolar, Hz, HzLo, HzHi, Time, Db, Ratio, Mult, Q, Oct, Degrees,
                                 LfoHz, Center, Count, Text, Semi };
// Who owns the value and what a set does: see surface.py "kind".
enum class Kind : unsigned char { Synth, Chain, Ui, Readout, Stepper, Button, Tile, Toggle, Popup };

struct ParamSpec { Curve curve; Fmt fmt; float lo, hi; };
struct ParamInfo {
    const char* key;
    const char* name;
    Kind kind;
    float def;                  // MPC's 0..1 default
    int nopts;
    const char* const* opts;
    int popupOf;                // Kind::Popup: the parameter whose list it opens, else -1
};

static constexpr ParamSpec PARAM_SPECS[P_COUNT] = {
%s
};

%s

static constexpr ParamInfo PARAM_INFO[P_COUNT] = {
%s
};

constexpr const char* kPlugName = %s;
constexpr const char* kPlugVendor = %s;
constexpr int32_t kPlugUid = 0x%08x;   // '%s'
constexpr int32_t kPlugVersion = %d;

// The modules in their default order (surface.py MODULES); OPTS of order_1..8 name them.
enum Module : int { %s, kNumModules };
static constexpr int kModuleOnParam[kNumModules] = {%s};

// The modulation matrix's targets: the parameter each option of m1_dst..m%d_dst reaches (-1: Off).
constexpr int kNumModTargets = %d;
static constexpr int kModTargetParam[kNumModTargets] = {%s};

constexpr int kNumModSlots = %d;
constexpr int kNumModSources = %d;
constexpr int kNumLfoWaves = %d;
constexpr int kNumDelayDivisions = %d;
constexpr int kNumLfoDivisions = %d;
constexpr int kStepperRange = %d;
constexpr int kBrowserCats = %d;
constexpr int kBrowserItems = %d;

} // namespace ef
""" % (ids, specs, "\n".join(opts), info, c_str(VST["name"]), c_str(VST["vendor"]), uid, VST["uid"],
       VST["version"], modules, on_params, MOD_SLOTS, len(MOD_TARGETS), targets, MOD_SLOTS, len(MOD_SOURCES),
       len(LFO_WAVES), len(DELAY_DIVS), len(LFO_DIVS), STEPPER_RANGE, BROWSER_CATS, BROWSER_ITEMS)


# --- factory presets: presets/Factory/<NN_Category>/<NN_Name>.efp, embedded in the .so ---------
PRESET_DIR = os.path.join(HERE, "..", "presets", "Factory")
PRESET_EXT = ".efp"
STATE_EXTRA_KEYS = ("preset",)
PRESET_NAME_MAX = 18      # an item tile on the browser page (816 px / 3 columns)
CATEGORY_NAME_MAX = 12    # a category tile (320 px / 2 columns), shown in capitals


def _shown(entry):
    """"02_Tape_Echo.efp" -> "Tape Echo", "03_Space" -> "Space"."""
    return re.sub(r"^\d+\s+", "", re.sub(r"\.efp$", "", entry).replace("_", " "))


def check_preset_line(params, key, val):
    """None if key=val is a valid sound line, else what is wrong with it. Options go by name (or index)."""
    p = params.get(key)
    if not p or p["kind"] not in ("synth", "chain"):
        return "%r is not a sound parameter" % key
    if "options" in p:
        if val in p["options"] or (val.isdigit() and int(val) < len(p["options"])):
            return None
        return "%r is not an option of %s (%s)" % (val, key, ", ".join(p["options"]))
    try:
        v = float(val)
    except ValueError:
        return "%r is not a number" % val
    if not (min(p["lo"], p["hi"]) - 1e-9 <= v <= max(p["lo"], p["hi"]) + 1e-9):
        return "%s=%s outside %s..%s" % (key, val, p["lo"], p["hi"])
    return None


def factory_presets():
    """[(category, name, text)]: one folder per category, both in file order ("NN_" orders them, "_" shows as a
    space). Every line must be a sound parameter with a value in range (options by name), the order lines all or
    none and then a permutation of the modules, every name unique (keys are "builtin:<name>") and short enough for
    its tile: a typo fails the build, not the device."""
    params = {p["key"]: p for p in P}
    out, errors, seen = [], [], {}
    for d in sorted(os.listdir(PRESET_DIR)):
        folder = os.path.join(PRESET_DIR, d)
        if d.endswith(PRESET_EXT):
            errors.append("%s: put it in a category folder (presets/Factory/NN_Category/)" % d)
            continue
        if not os.path.isdir(folder):
            continue
        category = _shown(d)
        if not category or len(category) > CATEGORY_NAME_MAX:
            errors.append("%s: a category name of 1..%d characters" % (d, CATEGORY_NAME_MAX))
        for f in sorted(os.listdir(folder)):
            if not f.endswith(PRESET_EXT):
                continue
            where = "%s/%s" % (d, f)
            text = open(os.path.join(folder, f), encoding="utf-8").read().replace("\r\n", "\n")
            lines = text.split("\n")
            if not lines[0].startswith("effectforce "):
                errors.append("%s: no 'effectforce N' header" % where)
            order, seen_keys = {}, {}
            for n, line in enumerate(lines[1:], 2):
                if not line.strip():
                    continue
                key, _, val = line.partition("=")
                if key in seen_keys:
                    errors.append("%s:%d: %s is set twice (line %d too)" % (where, n, key, seen_keys[key]))
                seen_keys[key] = n
                if key in STATE_EXTRA_KEYS:
                    continue
                bad = check_preset_line(params, key, val)
                if not bad and key == "out_gain" and abs(float(val)) > 9.0 + 1e-9:
                    bad = "out_gain=%s: a factory preset's Output stays within +-9 dB (tools/levels.cpp)" % val
                if bad:
                    errors.append("%s:%d: %s" % (where, n, bad))
                elif key.startswith("order_"):
                    opts = params[key]["options"]
                    order[key] = opts[int(val)] if val.isdigit() else val
            if order and sorted(order.values()) != sorted(MODULE_NAMES):
                errors.append("%s: order_1..%d must name every module once" % (where, len(MODULES)))
            values = dict(l.partition("=")[::2] for l in lines[1:] if "=" in l)
            for pre, mod in MODULES:
                used = [k for k in values if k.startswith(pre + "_") and k != pre + "_on"
                        or (pre == "cmp" and k.startswith("ott_"))]
                if used and values.get(pre + "_on") != "On":
                    errors.append("%s: sets %s but leaves %s off" % (where, ", ".join(used), mod))
            name = _shown(f)
            if name in seen:
                errors.append("%s: the name %r is taken by %s" % (where, name, seen[name]))
            seen[name] = where
            if len(name) > PRESET_NAME_MAX:
                errors.append("%s: %r is longer than %d characters" % (where, name, PRESET_NAME_MAX))
            out.append((category, name, text))
    if "Init" not in seen:
        errors.append("no Init preset (the INIT button loads builtin:Init)")
    if errors:
        raise SystemExit("factory presets:\n  " + "\n  ".join(errors))
    return out


def presets_header(presets):
    rows = ",\n".join("    {%s, %s, %s}" % (c_str(c), c_str(n), c_str(t).replace("\n", "\\n")) for c, n, t in presets)
    return """// generated by surface/surface.py from presets/Factory/*/*.efp: do not edit
#pragma once

namespace ef {

struct FactoryPreset { const char* category; const char* name; const char* text; };
static const FactoryPreset kFactoryPresets[] = {
%s
};
constexpr int kNumFactoryPresets = %d;

} // namespace ef
""" % (rows, len(presets))


def main():
    keys = [p["key"] for p in P]
    assert len(set(keys)) == len(keys), "duplicate parameter key"
    assert P[0]["kind"] == "readout", "parameter 0 must stay a read-only readout"
    assert len(set(MOD_TARGETS)) == len(MOD_TARGETS), "two modulation targets with the same name"
    for k in MOD_TARGET_KEYS:
        assert PARAMS[k]["kind"] == "synth" and "options" not in PARAMS[k], "%s: not a continuous parameter" % k
    layout = pages()
    check_layout(layout, build_layout().groups)
    presets = factory_presets()   # everything checked before anything is written
    outputs = [
        ("params.json", json.dumps(params_json(), indent=1)),
        ("layout.conf", layout),
        ("vst.json", json.dumps(VST, indent=1)),
        (os.path.join("build", "skin_style.json"), json.dumps(skin_style(), indent=1)),
        (os.path.join("build", "factory_presets.h"), presets_header(presets)),
        (os.path.join("build", "param_ids.h"), header()),   # last: make's target, newer than the rest
    ]
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    for name, text in outputs:
        path = os.path.join(HERE, name)
        with open(path + ".tmp", "w", newline="\n") as f:
            f.write(text)
        os.replace(path + ".tmp", path)
    print("surface: %d parameters, layout ok, %d factory presets" % (len(P), len(presets)))


if __name__ == "__main__":
    sys.exit(main())
