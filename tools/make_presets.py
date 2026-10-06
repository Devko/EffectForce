#!/usr/bin/env python3
"""Writes EffectForce's factory presets (presets/Factory/NN_Category/NN_Name.efp) from the lists below:
only what differs from the defaults. It rewrites the whole folder, so run `make preset-levels` after
it: that sets every preset's out_gain (level-matching), which this file doesn't know.

    python3 tools/make_presets.py && make preset-levels
"""
import os
import shutil

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "presets", "Factory")

def slot(k, src, dst, amt):
    return {"m%d_src" % k: src, "m%d_dst" % k: dst, "m%d_amt" % k: amt}

P = {}

def preset(cat, name, **kv):
    d = {}
    for k, v in kv.items():
        if isinstance(v, dict):
            d.update(v)
        else:
            d[k] = v
    P.setdefault(cat, []).append((name, d))

def merge(*ds):
    out = {}
    for d in ds:
        out.update(d)
    return out

# --- Utility ---
preset("Utility", "Init")
preset("Utility", "Glue Comp", cmp_on="On", cmp_thr=-20, cmp_ratio=2.5, cmp_att=0.03, cmp_rel=0.2, cmp_knee=8, cmp_makeup=2)
preset("Utility", "Clean Up", eq_on="On", eq_lc=80, eq_mf=400, eq_mg=-3, eq_mq=1.0, eq_hf=8000, eq_hg=2)
preset("Utility", "Wide Stereo", chr_on="On", chr_mode="Dimension", chr_depth=0.35, chr_mix=0.35, chr_lc=200)
preset("Utility", "Parallel Squash", cmp_on="On", cmp_thr=-36, cmp_ratio=10, cmp_att=0.002, cmp_rel=0.08, cmp_knee=3,
       cmp_makeup=12, cmp_mix=0.35)

# --- Synth ---
preset("Synth", "Lead Polish", eq_on="On", eq_lc=120, eq_hf=7000, eq_hg=3, cmp_on="On", cmp_thr=-12, cmp_ratio=3,
       cmp_att=0.01, cmp_rel=0.15, dly_on="On", dly_mode="Ping-Pong", dly_div="1/8.", dly_fb=0.35, dly_hc=6000,
       dly_mix=0.2, rev_on="On", rev_mode="Plate", rev_decay=1.6, rev_mix=0.18)
preset("Synth", "Juno Chorus", chr_on="On", chr_mode="Dimension", chr_depth=0.6, chr_mix=0.5, rev_on="On",
       rev_mode="Room", rev_size=0.4, rev_decay=0.9, rev_mix=0.12)
preset("Synth", "Saturated Lead", drv_on="On", drv_type="Tube", drv_amt=14, drv_tone=0.2, flt_on="On",
       flt_type="LP 24", flt_cut=6000, flt_res=0.1, dly_on="On", dly_div="1/4", dly_fb=0.3, dly_duck=0.5,
       dly_mix=0.2, rev_on="On", rev_decay=2.5, rev_mix=0.15)
preset("Synth", "Synth Stabs", cmp_on="On", cmp_thr=-14, cmp_ratio=4, cmp_att=0.003, cmp_rel=0.1, dly_on="On",
       dly_div="1/8.", dly_fb=0.45, dly_mix=0.25, rev_on="On", rev_mode="Plate", rev_decay=1.2, rev_mix=0.2)
preset("Synth", "Pluck Echo", dly_on="On", dly_mode="Ping-Pong", dly_div="1/8", dly_fb=0.5, dly_lc=300,
       dly_hc=5000, dly_wow=0.15, dly_mix=0.3, rev_on="On", rev_mode="Room", rev_decay=0.8, rev_mix=0.15)

# --- Pads ---
preset("Pads", "Lush Pad", chr_on="On", chr_mode="Ensemble", chr_depth=0.6, chr_mix=0.5, rev_on="On",
       rev_size=0.7, rev_decay=5, rev_pre=0.03, rev_mod=0.5, rev_mix=0.35)
preset("Pads", "Endless", eq_on="On", eq_lc=150, rev_on="On", rev_mode="Space", rev_size=0.8, rev_decay=12,
       rev_mod=0.7, rev_damp=5000, rev_mix=0.45)
preset("Pads", "Slow Phase", phs_on="On", phs_mode="Phaser 8", phs_rate=0.15, phs_depth=0.8, phs_fb=0.6,
       phs_mix=0.5, rev_on="On", rev_decay=4, rev_mix=0.3)
preset("Pads", "Wash Macro", rev_on="On", rev_size=0.75, rev_decay=8, rev_mix=0.25, dly_on="On", dly_div="1/4.",
       dly_fb=0.5, dly_mix=0.15, mac_1=0.0, **merge(slot(1, "Macro 1", "Reverb Mix", 0.5),
                                                    slot(2, "Macro 1", "Delay Mix", 0.3)))

# --- Bass ---
preset("Bass", "Bass Grit", drv_on="On", drv_type="Tube", drv_amt=10, drv_tone=-0.2, drv_mix=0.7, cmp_on="On",
       cmp_thr=-20, cmp_ratio=4, cmp_att=0.01, cmp_rel=0.12, eq_on="On", eq_lc=30, eq_lf=80, eq_lg=2)
preset("Bass", "Acid Squelch", drv_on="On", drv_amt=8, flt_on="On", flt_type="LP 24", flt_cut=600, flt_res=0.75,
       flt_drive=0.4, l1_sync="Sync", l1_div="1/8", l1_wave="Triangle", **slot(1, "LFO 1", "Filter Cutoff", 0.3))
preset("Bass", "Sub Tight", eq_on="On", eq_lc=25, eq_lf=60, eq_lg=3, eq_mf=300, eq_mg=-2, cmp_on="On",
       cmp_thr=-16, cmp_ratio=6, cmp_att=0.005, cmp_rel=0.08, cmp_makeup=3)
preset("Bass", "OTT Bass", cmp_on="On", cmp_mode="OTT", ott_depth=0.5, eq_on="On", eq_lc=30)

# --- Drums ---
preset("Drums", "Drum Bus", cmp_on="On", cmp_thr=-16, cmp_ratio=3, cmp_att=0.02, cmp_rel=0.12, cmp_makeup=2,
       drv_on="On", drv_type="Tube", drv_amt=4, drv_mix=0.5, eq_on="On", eq_lf=80, eq_lg=2, eq_hf=10000, eq_hg=2)
preset("Drums", "Crushed Loop", drv_on="On", drv_type="Crush", drv_amt=20, drv_mix=0.6, flt_on="On",
       flt_type="HP 12", flt_cut=150, flt_res=0.1, cmp_on="On", cmp_thr=-18, cmp_ratio=4)
preset("Drums", "Room Kit", rev_on="On", rev_mode="Room", rev_size=0.35, rev_decay=0.7, rev_mix=0.18, cmp_on="On",
       cmp_thr=-30, cmp_ratio=8, cmp_att=0.002, cmp_rel=0.1, cmp_makeup=10, cmp_mix=0.5)
preset("Drums", "OTT Smash", cmp_on="On", cmp_mode="OTT", ott_depth=0.7, ott_time=0.6, eq_on="On", eq_lc=40)

# --- Lo-Fi ---
preset("Lo-Fi", "Tape Echo", drv_on="On", drv_type="Tube", drv_amt=6, drv_tone=-0.3, dly_on="On", dly_mode="Mono",
       dly_div="1/8.", dly_fb=0.55, dly_wow=0.5, dly_drive=0.4, dly_hc=3500, dly_lc=250, dly_mix=0.3)
preset("Lo-Fi", "Old Sampler", drv_on="On", drv_type="Crush", drv_amt=14, flt_on="On", flt_type="LP 12",
       flt_cut=6000, eq_on="On", eq_lc=80)
preset("Lo-Fi", "Radio", flt_on="On", flt_type="BP", flt_cut=1500, flt_res=0.3, drv_on="On", drv_type="Hard",
       drv_amt=12, drv_mix=0.8, order_1="Filter", order_2="Drive")
preset("Lo-Fi", "Wobble Chorus", chr_on="On", chr_rate=0.3, chr_depth=0.9, chr_mix=0.6, dly_on="On",
       dly_div="1/4", dly_fb=0.3, dly_wow=0.7, dly_mix=0.2)

# --- Space ---
preset("Space", "Big Hall", rev_on="On", rev_size=0.8, rev_decay=4, rev_pre=0.04, rev_mix=0.35)
preset("Space", "Plate", rev_on="On", rev_mode="Plate", rev_decay=2, rev_lc=200, rev_mix=0.3)
preset("Space", "Ping Pong Hall", dly_on="On", dly_mode="Ping-Pong", dly_div="1/4.", dly_fb=0.45, dly_duck=0.4,
       dly_mix=0.25, rev_on="On", rev_decay=3, rev_mix=0.25)
preset("Space", "Infinite Space", rev_on="On", rev_mode="Space", rev_size=1.0, rev_decay=30, rev_mod=0.8,
       rev_damp=4000, rev_lc=300, rev_mix=0.5)
preset("Space", "Dub Delay", dly_on="On", dly_div="1/4.", dly_fb=0.75, dly_hc=2500, dly_lc=400, dly_drive=0.3,
       dly_mix=0.35, rev_on="On", rev_mode="Room", rev_decay=1.0, rev_mix=0.1)

# --- Creative ---
preset("Creative", "Pulse Filter", flt_on="On", flt_type="LP 24", flt_cut=400, flt_res=0.4, l1_wave="Square",
       l1_sync="Sync", l1_div="1/8", **slot(1, "LFO 1", "Filter Cutoff", 0.35))
preset("Creative", "Jet Flanger", phs_on="On", phs_mode="Flanger", phs_rate=0.1, phs_depth=0.9, phs_fb=0.8,
       phs_center=0.4, phs_mix=0.5)
preset("Creative", "Auto Wah", flt_on="On", flt_type="BP", flt_cut=300, flt_res=0.6, flt_mix=0.8, env_gain=6,
       env_att=0.005, env_rel=0.15, **slot(1, "Envelope", "Filter Cutoff", 0.3))
preset("Creative", "Riser Macro", flt_on="On", flt_type="HP 12", flt_cut=20, flt_res=0.3, rev_on="On",
       rev_decay=4, rev_mix=0.15, dly_on="On", dly_div="1/8", dly_fb=0.3, dly_mix=0.15,
       **merge(slot(1, "Macro 1", "Filter Cutoff", 0.8), slot(2, "Macro 1", "Reverb Mix", 0.3),
               slot(3, "Macro 1", "Delay FB", 0.3)))
preset("Creative", "Ducked Wash", dly_on="On", dly_div="1/4", dly_fb=0.6, dly_duck=0.6, dly_mix=0.4, rev_on="On",
       rev_mode="Space", rev_decay=6, rev_mix=0.45, env_gain=12, **slot(1, "Envelope", "Reverb Mix", -0.4))
preset("Creative", "Phase Drive", drv_on="On", drv_type="Fold", drv_amt=10, drv_mix=0.6, phs_on="On",
       phs_mode="Phaser 12", phs_sync="Sync", phs_div="1 bar", phs_depth=0.9, phs_fb=0.7, phs_mix=0.5, rev_on="On",
       rev_decay=2, rev_mix=0.2)

# --- Shimmer ---
preset("Space", "Shimmer Hall", rev_on="On", rev_size=0.75, rev_decay=6, rev_pre=0.03, rev_mod=0.5, rev_shim=0.5,
       rev_mix=0.4)
preset("Pads", "Shimmer Pad", chr_on="On", chr_mode="Ensemble", chr_depth=0.5, chr_mix=0.4, rev_on="On",
       rev_mode="Space", rev_decay=10, rev_mod=0.6, rev_shim=0.6, rev_damp=7000, rev_mix=0.45)
preset("Space", "Fifth Heaven", rev_on="On", rev_mode="Plate", rev_decay=5, rev_shim=0.45, rev_shim_int="+7",
       rev_lc=250, rev_mix=0.35)

# --- Texture: Grain ---
preset("Texture", "Grain Cloud", grn_on="On", grn_mode="Cloud", grn_density=0.7, grn_pitch=12, grn_spread=0.6,
       grn_mix=0.5, rev_on="On", rev_decay=3.5, rev_mix=0.3)
preset("Texture", "Glitter Haze", grn_on="On", grn_mode="Cloud", grn_density=0.85, grn_pitch=12, grn_reverse=0.4,
       grn_spread=0.8, grn_fb=0.3, grn_mix=0.55, rev_on="On", rev_mode="Space", rev_decay=8, rev_shim=0.4,
       rev_mix=0.4)
preset("Texture", "Beat Mosaic", grn_on="On", grn_mode="Mosaic", grn_div="1/16", grn_density=0.6, grn_reverse=0.3,
       grn_mix=0.6)
preset("Texture", "Stutter Glitch", grn_on="On", grn_mode="Stutter", grn_div="1/32", grn_density=0.5,
       grn_mix=0.5)
preset("Texture", "Drone Stretch", grn_on="On", grn_mode="Stretch", grn_sync="Free", grn_size=0.5,
       grn_density=0.8, grn_fb=0.4, grn_mix=0.6, rev_on="On", rev_mode="Space", rev_decay=12, rev_mix=0.4)
preset("Texture", "Grain Arp", grn_on="On", grn_mode="Arp", grn_div="1/16", grn_density=0.5, grn_mix=0.6,
       dly_on="On", dly_div="1/8.", dly_fb=0.35, dly_mix=0.2)
preset("Texture", "Reverse Swells", grn_on="On", grn_mode="Cloud", grn_size=0.4, grn_sync="Free",
       grn_density=0.6, grn_reverse=1.0, grn_spread=0.7, grn_mix=0.5, rev_on="On", rev_decay=4, rev_mix=0.35)

# --- Rhythm: Pulse ---
preset("Rhythm", "Trance Gate", pls_on="On", pls_mode="Gate", pls_div="1/16", pls_pattern="Trance 1",
       pls_length=0.6, pls_smooth=0.003, dly_on="On", dly_div="1/8.", dly_fb=0.3, dly_mix=0.2)
preset("Rhythm", "Vintage Trem", pls_on="On", pls_mode="Tremolo", pls_div="1/8", pls_depth=0.6, pls_shape=0.3)
preset("Rhythm", "Wide Pan", pls_on="On", pls_mode="Auto-Pan", pls_div="1/4", pls_depth=0.8)
preset("Rhythm", "Choppy Pads", pls_on="On", pls_mode="Gate", pls_div="1/16", pls_pattern="Offbeat",
       pls_length=0.7, pls_smooth=0.008, rev_on="On", rev_decay=2.5, rev_mix=0.3)
preset("Rhythm", "Stereo Trem", pls_on="On", pls_mode="Tremolo", pls_div="1/16", pls_depth=0.8,
       pls_stereo=180, chr_on="On", chr_mode="Dimension", chr_mix=0.3)

# --- Perform: the Octatrack's performance-mixer setups (docs/DESIGN.md "Performance: scenes and the looper").
# Put on the master (or a drum and a melodic submix), learn the Force's crossfader to Crossfader: scene 1
# (clean) at the A end, an effect scene at the B end. The knobs stay clean; the scenes are effects of the
# FX library (tools/make_fx.py), named, so the PERFORM page shows which. ---
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_fx   # noqa: E402  (the FX library's lists)


def scene(n, **kv):
    return {"scene%d.%s" % (n, k): v for k, v in kv.items()}


def fx_scene(n, name):
    """Scene n as the library's effect `name`, named after it."""
    return dict(scene(n, name=name), **scene(n, **make_fx.find(name)))


def scenes(*names):
    """Scenes 2.. as these effects (scene 1 stays clean, the A end)."""
    out = {}
    for n, name in enumerate(names, 2):
        out.update(fx_scene(n, name))
    return out


ARMED = dict(lp_on="On")   # the looper records from the start
preset("Perform", "Perform Mixer", **ARMED, scene_b="2",
       s=scenes("LP Sweep", "HP Resonant", "Echo Throw", "Roll 1/16", "Tape Stop", "Hall Wash", "Pump"))
preset("Perform", "Loop Mixer 4 Bar", **ARMED, lp_len="4 bars", lp_capture="Next", scene_b="2",
       s2=scene(2, name="4-Bar Loop", lp_mix=1), s=dict(fx_scene(3, "Roll 1/8"), **fx_scene(4, "Roll 1/32"),
                                                        **fx_scene(5, "Half Speed"), **fx_scene(6, "Reverse"),
                                                        **fx_scene(7, "Build Roll"), **fx_scene(8, "Loop Layer")))
preset("Perform", "Loop Mixer 8 Bar", **ARMED, lp_len="8 bars", lp_capture="Next", scene_b="2",
       s2=scene(2, name="8-Bar Loop", lp_mix=1), s=dict(fx_scene(3, "Roll 1/4"), **fx_scene(4, "Roll 1/16"),
                                                        **fx_scene(5, "Tape Stop"), **fx_scene(6, "Echo Freeze"),
                                                        **fx_scene(7, "Shimmer Wash"), **fx_scene(8, "Loop Layer")))
preset("Perform", "DJ Mixer", scene_b="2",
       s=scenes("LP Sweep", "HP Sweep", "Kill Lows", "Kill Highs", "LP Resonant", "HP Resonant", "Echo Throw"))
preset("Perform", "Dub Mixer", scene_b="2",
       s=scenes("Dub Echo", "Tape Echo", "Ping-Pong 1/4", "Echo Freeze", "Plate Splash", "Space Freeze",
                "Underwater"))
preset("Perform", "Build and Drop", **ARMED, scene_b="2",
       s=scenes("Build Roll", "Comb Riser", "HP Resonant", "Build Gate", "Shimmer Wash", "Tape Stop", "Destroy"))
preset("Perform", "Glitch Mixer", **ARMED, scene_b="2",
       s=scenes("Stutter", "Mosaic", "Beat Repeat", "Stutter Gate", "Reverse", "Double Speed", "Bit Crush"))
preset("Perform", "Rhythm Mixer", scene_b="2",
       s=scenes("Pump", "Trance Gate", "Gate 1/16", "Offbeat Gate", "Tremolo 1/8", "Auto-Pan 1/4", "Wobble 1/8"))
preset("Perform", "Space Mixer", scene_b="2",
       s=scenes("Hall Wash", "Shimmer Wash", "Dark Space", "Grain Cloud", "Drone Stretch", "Reverse Haze",
                "Wide Chorus"))
preset("Perform", "Crush Mixer", scene_b="2",
       s=scenes("Bit Crush", "Lo-Fi Radio", "Overdrive", "Fold", "OTT Squash", "Crush Filter", "Telephone"))

CATS = ["Utility", "Synth", "Pads", "Bass", "Drums", "Lo-Fi", "Space", "Creative", "Texture", "Rhythm", "Perform"]


def fmt(v):
    if isinstance(v, tuple):   # a scene's lock that moves (an effect of the FX library): start>end
        return "%s>%s" % (fmt(v[0]), fmt(v[1]))
    if isinstance(v, str):
        return v
    if isinstance(v, float) and v != int(v):
        return ("%.6g" % v)
    return "%g" % v


def order_lines(d):
    """A preset that names part of the order gets the whole order: the named slots, then the rest in default order."""
    named = {k: v for k, v in d.items() if k.startswith("order_")}
    if not named:
        return d
    modules = ["Drive", "Filter", "EQ", "Comp", "Chorus", "Phaser", "Pulse", "Grain", "Delay", "Reverb"]
    rest = [m for m in modules if m not in named.values()]
    out = {k: v for k, v in d.items() if not k.startswith("order_")}
    for k in range(1, len(modules) + 1):
        out["order_%d" % k] = named.get("order_%d" % k) or rest.pop(0)
    return out


shutil.rmtree(ROOT, ignore_errors=True)
for ci, cat in enumerate(CATS, 1):
    folder = os.path.join(ROOT, "%02d_%s" % (ci, cat))
    os.makedirs(folder)
    for pi, (name, d) in enumerate(P[cat], 1):
        d = order_lines(d)
        text = "effectforce 1\n" + "".join("%s=%s\n" % (k, fmt(v)) for k, v in d.items())
        with open(os.path.join(folder, "%02d_%s.efp" % (pi, name.replace(" ", "_"))), "w", newline="\n") as f:
            f.write(text)
print(sum(len(v) for v in P.values()), "presets")
