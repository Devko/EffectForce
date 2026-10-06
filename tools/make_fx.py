#!/usr/bin/env python3
"""Writes EffectForce's performance effects (presets/FX/NN_Bank/NN_Name.eff) from the lists below: the
FX library the PERFORM page picks from (docs/DESIGN.md "Performance: scenes and the looper").

An effect is a scene: the parameters it locks, nothing else. Tapping it on the PERFORM page puts it in
the scene at the crossfader's B end; the fader then fades it in from the clean A end. So every effect
switches its modules on itself (a module a scene switches is a send: delay and reverb tails ring out
when the fader comes back), sets what makes it sound, and leaves the rest to the knobs. Effects that
need an LFO or the matrix use LFO 2 and matrix slot 8, so slots 1-7 and LFO 1 stay the user's.
Effects that use the looper (lp_*) arm it when they are picked.

An effect can move (docs/DESIGN.md "Scene moves"): a lock given as (start, end) plays from start to end
over move= (a LENGTH: "1 beat" .. "16 bars") once the fader brings the scene in, from the next bar line;
play= says what then: "Once" holds the end, "Loop" starts again, "Ping-pong" goes back. The sweeps and
risers move; the colours, and the effects an LFO or the envelope already moves, hold still.

Four banks of sixteen, one per page of tiles. Names fit a tile (14 characters).

    python3 tools/make_fx.py      (surface.py checks and embeds them)
"""
import os
import shutil

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "presets", "FX")
NAME_MAX = 14

BANKS = []   # [(bank, [(name, {key: value})])]


def bank(name):
    BANKS.append((name, []))


def fx(name, move=None, play="Once", **kv):
    assert len(name) <= NAME_MAX, name
    assert (move is None) == (not any(isinstance(v, tuple) for v in kv.values())), name   # a move needs both
    if move:   # the timing first, then the locks
        kv = dict(move=move, play=play, **kv)
    BANKS[-1][1].append((name, kv))


def lfo2(div, wave="Sine", sync="Sync"):
    return dict(l2_sync=sync, l2_div=div, l2_wave=wave)


def slot8(src, dst, amt):
    return dict(m8_src=src, m8_dst=dst, m8_amt=amt)


# --- FILTER: sweeps and colours; the fader is the knob ---
bank("Filter")
fx("LP Sweep", move="8 bars", flt_on="On", flt_type="LP 24", flt_cut=(18000, 150), flt_res=0.3)
fx("LP Resonant", flt_on="On", flt_type="LP 24", flt_cut=250, flt_res=0.75, flt_drive=0.3)
fx("HP Sweep", move="8 bars", flt_on="On", flt_type="HP 24", flt_cut=(20, 1500), flt_res=0.25)
fx("HP Resonant", flt_on="On", flt_type="HP 24", flt_cut=2500, flt_res=0.7)
fx("Band Pass", flt_on="On", flt_type="BP", flt_cut=1200, flt_res=0.5)
fx("Telephone", flt_on="On", flt_type="BP", flt_cut=1800, flt_res=0.45, drv_on="On", drv_type="Soft", drv_amt=14)
fx("Underwater", flt_on="On", flt_type="LP 12", flt_cut=350, flt_res=0.4, chr_on="On", chr_mode="Ensemble",
   chr_depth=0.7, chr_mix=0.5)
fx("Notch Sweep", move="4 bars", play="Ping-pong", flt_on="On", flt_type="Notch", flt_cut=(250, 2500), flt_res=0.6,
   flt_spread=0.5)
fx("Acid Squelch", flt_on="On", flt_type="LP 24", flt_cut=700, flt_res=0.85, flt_drive=0.6,
   **lfo2("1/8"), **slot8("LFO 2", "Filter Cutoff", 0.3))
fx("Wobble 1/8", flt_on="On", flt_type="LP 24", flt_cut=500, flt_res=0.5, **lfo2("1/8", "Triangle"),
   **slot8("LFO 2", "Filter Cutoff", 0.45))
fx("Auto Wah", flt_on="On", flt_type="BP", flt_cut=400, flt_res=0.6, **slot8("Envelope", "Filter Cutoff", 0.6))
fx("Phaser Sweep", move="4 bars", play="Ping-pong", phs_on="On", phs_mode="Phaser 8", phs_sync="Sync", phs_div="1 bar",
   phs_depth=0.9, phs_center=(0.2, 0.8), phs_fb=0.7, phs_mix=0.6)
fx("Jet Flanger", phs_on="On", phs_mode="Flanger", phs_sync="Free", phs_rate=0.2, phs_depth=0.9, phs_fb=0.85,
   phs_mix=0.6)
fx("Comb Riser", move="4 bars", phs_on="On", phs_mode="Flanger", phs_depth=0.0, phs_center=(0.95, 0.15),
   phs_fb=(0.6, 0.95), phs_mix=0.7)
fx("Kill Lows", eq_on="On", eq_lc=300)
fx("Kill Highs", eq_on="On", eq_hc=1500)

# --- SPACE: reverbs, echoes, textures ---
bank("Space")
fx("Hall Wash", move="4 bars", rev_on="On", rev_mode="Hall", rev_size=0.8, rev_decay=6, rev_mix=(0, 0.6))
fx("Plate Splash", rev_on="On", rev_mode="Plate", rev_decay=2.5, rev_pre=0.03, rev_mix=0.5)
fx("Space Freeze", rev_on="On", rev_mode="Space", rev_size=0.9, rev_decay=12, rev_freeze="On", rev_mix=0.8)
fx("Shimmer Wash", move="8 bars", rev_on="On", rev_mode="Space", rev_decay=10, rev_shim=(0, 0.6), rev_mix=(0.3, 0.6))
fx("Dark Space", rev_on="On", rev_mode="Space", rev_decay=12, rev_damp=2500, rev_mix=0.6, flt_on="On",
   flt_type="LP 12", flt_cut=2000)
fx("Dub Echo", dly_on="On", dly_sync="Sync", dly_div="1/8.", dly_fb=0.75, dly_lc=300, dly_hc=3000, dly_wow=0.3,
   dly_drive=0.3, dly_mix=0.5)
fx("Ping-Pong 1/4", dly_on="On", dly_mode="Ping-Pong", dly_sync="Sync", dly_div="1/4", dly_fb=0.6, dly_mix=0.45)
fx("Echo Throw", move="2 bars", dly_on="On", dly_sync="Sync", dly_div="1/8", dly_fb=(0.4, 0.8), dly_mix=0.5)
fx("Tape Echo", dly_on="On", dly_sync="Sync", dly_div="1/4.", dly_fb=0.6, dly_wow=0.6, dly_drive=0.5, dly_hc=4000,
   dly_mix=0.45)
fx("Echo Freeze", dly_on="On", dly_sync="Sync", dly_div="1/16", dly_fb=1, dly_mix=0.6)
fx("Slapback", dly_on="On", dly_sync="Free", dly_time=0.09, dly_fb=0.1, dly_mix=0.4)
fx("Grain Cloud", grn_on="On", grn_mode="Cloud", grn_density=0.8, grn_spread=0.7, grn_mix=0.6)
fx("Reverse Haze", grn_on="On", grn_mode="Cloud", grn_reverse=1, grn_density=0.6, grn_mix=0.6, rev_on="On",
   rev_decay=4, rev_mix=0.35)
fx("Drone Stretch", grn_on="On", grn_mode="Stretch", grn_density=0.7, grn_mix=0.7, rev_on="On", rev_mix=0.3)
fx("Wide Chorus", chr_on="On", chr_mode="Dimension", chr_depth=0.7, chr_width=1, chr_mix=0.5)
fx("Ducked Wash", rev_on="On", rev_mode="Hall", rev_decay=5, rev_mix=0.5, dly_on="On", dly_div="1/8.",
   dly_fb=0.5, dly_duck=0.8, dly_mix=0.4)

# --- LOOP: the looper (armed when picked) ---
bank("Loop")
fx("Roll 1/2", lp_mix=1, lp_rep="1/2")
fx("Roll 1/4", lp_mix=1, lp_rep="1/4")
fx("Roll 1/8", lp_mix=1, lp_rep="1/8")
fx("Roll 1/16", lp_mix=1, lp_rep="1/16")
fx("Roll 1/32", lp_mix=1, lp_rep="1/32")
fx("Loop 1 Bar", lp_mix=1, lp_len="1 bar")
fx("Loop 1/2 Bar", lp_mix=1, lp_len="1/2")
fx("Tape Stop", move="1 beat", lp_mix=1, lp_speed=(1, 0))
fx("Half Speed", lp_mix=1, lp_len="2 bars", lp_speed=0.5)
fx("Double Speed", lp_mix=1, lp_speed=2)
fx("Reverse", lp_mix=1, lp_speed=-1)
fx("Beat Repeat", lp_mix=1, lp_rep="1/4", flt_on="On", flt_type="HP 12", flt_cut=250)
fx("Build Roll", move="4 bars", lp_mix=1, lp_rep=("1/4", "1/32"), flt_on="On", flt_type="HP 24", flt_cut=(200, 2500),
   rev_on="On", rev_mix=0.4)
fx("Stutter", grn_on="On", grn_mode="Stutter", grn_density=0.8, grn_mix=1)
fx("Mosaic", grn_on="On", grn_mode="Mosaic", grn_density=0.6, grn_spread=0.7, grn_mix=0.8)
fx("Loop Layer", lp_mix=1, lp_blend="Layer", lp_len="2 bars")

# --- RHYTHM / CRUSH: gates, pumps, dirt ---
bank("Rhythm")
fx("Pump", pls_on="On", pls_mode="Gate", pls_div="1/4", pls_pattern="Pump", pls_length=0.7, pls_smooth=0.02)
fx("Trance Gate", pls_on="On", pls_mode="Gate", pls_div="1/16", pls_pattern="Trance 1", pls_length=0.6)
fx("Gate 1/16", pls_on="On", pls_mode="Gate", pls_div="1/16", pls_pattern="1/16", pls_length=0.5)
fx("Offbeat Gate", pls_on="On", pls_mode="Gate", pls_div="1/8", pls_pattern="Offbeat", pls_length=0.7)
fx("Tremolo 1/8", pls_on="On", pls_mode="Tremolo", pls_sync="Sync", pls_div="1/8", pls_depth=0.8)
fx("Auto-Pan 1/4", pls_on="On", pls_mode="Auto-Pan", pls_sync="Sync", pls_div="1/4", pls_depth=0.9)
fx("Stutter Gate", pls_on="On", pls_mode="Gate", pls_div="1/16", pls_pattern="Stutter", pls_length=0.5)
fx("Build Gate", move="4 bars", pls_on="On", pls_mode="Gate", pls_div="1/16", pls_pattern="Build", pls_length=(0.9, 0.2))
fx("Bit Crush", drv_on="On", drv_type="Crush", drv_amt=24, drv_mix=1)
fx("Lo-Fi Radio", drv_on="On", drv_type="Crush", drv_amt=14, flt_on="On", flt_type="BP", flt_cut=1500,
   flt_res=0.35)
fx("Overdrive", drv_on="On", drv_type="Tube", drv_amt=24, drv_tone=0.2)
fx("Fold", drv_on="On", drv_type="Fold", drv_amt=20, drv_mix=0.7)
fx("OTT Squash", cmp_on="On", cmp_mode="OTT", ott_depth=1, cmp_makeup=4)
fx("Crush Filter", drv_on="On", drv_type="Crush", drv_amt=20, flt_on="On", flt_type="LP 24", flt_cut=800,
   flt_res=0.5)
fx("Punch Comp", cmp_on="On", cmp_mode="Comp", cmp_thr=-30, cmp_ratio=8, cmp_att=0.003, cmp_rel=0.08,
   cmp_makeup=10)
fx("Destroy", drv_on="On", drv_type="Hard", drv_amt=36, flt_on="On", flt_type="HP 12", flt_cut=200, cmp_on="On",
   cmp_mode="OTT", ott_depth=0.8)


def fmt(v):
    if isinstance(v, tuple):   # a lock that moves: start>end
        return "%s>%s" % (fmt(v[0]), fmt(v[1]))
    if isinstance(v, str):
        return v
    if isinstance(v, float) and v != int(v):
        return "%.6g" % v
    return "%g" % v


def text(name, kv):
    return "effectforce-fx 1\nname=%s\n" % name + "".join("%s=%s\n" % (k, fmt(v)) for k, v in kv.items())


def find(name):
    """An effect's locks by name (tools/make_presets.py builds the Perform presets from them)."""
    for _, effects in BANKS:
        for n, kv in effects:
            if n == name:
                return kv
    raise KeyError(name)


def main():
    shutil.rmtree(ROOT, ignore_errors=True)
    for bi, (b, effects) in enumerate(BANKS, 1):
        folder = os.path.join(ROOT, "%d_%s" % (bi, b))
        os.makedirs(folder)
        for i, (name, kv) in enumerate(effects, 1):
            with open(os.path.join(folder, "%02d_%s.eff" % (i, name.replace(" ", "_").replace("/", "-"))), "w",
                      newline="\n") as f:
                f.write(text(name, kv))
    print(sum(len(e) for _, e in BANKS), "effects in", len(BANKS), "banks")


if __name__ == "__main__":
    main()
