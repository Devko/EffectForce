# Roadmap

Status: ✅ done · 🔜 next · ⬜ planned · 💤 deferred

## What's next

### Hand-off (2026-10-05, branch `shimmer-grain-pulse`), finished in a cloud session

Shimmer, Pulse and Grain were integrated on `shimmer-grain-pulse` and handed over unfinished; the
cloud session's branch carries it on.

| Item | State |
|---|---|
| Grain on ARM | ✅ its 299 module checks passed under qemu-arm as they had on x86 |
| Preset levels | ✅ `make preset-levels`: the 37 older presets came back exactly as on `main`, the 15 new ones set |
| Full suites | ✅ x86 (ASan/UBSan) and ARM (qemu), 12913 checks after the fixes below; CI green |
| Review: Grain | ✅ memory and real-time safety held (no stale or unwritten frame read in 50M+ instrumented reads). Fixed: held Stutter silent when Hold came near a grid line; short pitched-up Stutter never repeating; the grid missing loops shorter than a slice (and loops wrapping on a block edge) and firing twice on a mid-block wrap; held slices off the grid; a second release lifting the samples before it; a stalled slice cut dead; a backwards read's margin. 16 new checks |
| Review: Pulse, Shimmer | ✅ Pulse clean. Shimmer: the shifter's first splice after a restart read silence (+19 dropped out), the path faded in while the shifter was still silent (a dip), the tail length ignored the shifter's delay; a frozen shimmer drains (now documented). 9 new checks |
| Review: integration | ✅ an order saved by 0.0.1 (eight slots) fell back to the default (now kept), Choppy Pads' gate on 1/8 steps (now 1/16, the patterns' unit), option lists checked against the enums, stale docs |
| Performance | ✅ everything on: avg -9%, p99 -27% in ARM instructions; the p99 had been the shimmer's splice search ([Performance](PERFORMANCE.md#instruction-counts)) |

Still to do, in order:

1. Merge the branch into `main` (CI green on it).
2. On the device (needs the local machine): `make bench-device` with all ten modules (budget:
   everything on at its heaviest, p99 ≤ 15%), then `make plugin-install` (ask first: it restarts
   MPC) and play Shimmer, Pulse and Grain; Grain's Sync with MPC looping (the slice grid's locate
   tolerance is a host block: whether MPC's song position ever jitters more than that is untested).

### Before the hand-off

- ✅ **Device bench** (2026-10-05): everything on at its heaviest with 8 mod slots, p99 11.3% of the
  block ([Performance](PERFORMANCE.md#device-measurements)).
- ✅ **On the device** (2026-10-05, 0.0.1): installed and played; everything worked and the pages
  open fast (MPC loads it next to SubForce without a complaint in its log).
- 🔜 **Still to try on the device:** save and reload a project, two instances, Delay Glide's Fade.
- 🔜 **Probe items still open** ([Probe](PROBE.md#results)): pads, returns and master as insert
  places; automation playback.
- ⬜ **v0.1**, the first release: parameter list frozen (append-only from then on), catalog-style
  package from CI.

## Milestones

### Phase 0 — probe ✅ (2026-10-05)

What MPC does with a VST2 insert effect, measured on the Force: [Probe](PROBE.md).

### v0.1 — the rack ✅ (built, 2026-10-05; device checks next)

Design record: [Design](DESIGN.md).

- ✅ Eight modules in any order: Drive (2x oversampled, six shapers), Filter, EQ, Comp / OTT,
  Chorus (three modes), Phaser / Flanger (synced), Delay (stereo, ping-pong, mono; tape character),
  Reverb (four modes, 8-line FDN, freeze)
- ✅ Then (2026-10-05, after the first device run): Pulse (tremolo, auto-pan, a 16-step gate), Grain
  (cloud, stretch, mosaic, stutter, arp on MPC's beat; hold, feedback), the Reverb's shimmer; ten
  modules, 52 presets in 10 categories, 54 modulation targets
- ✅ The rack: order, 10 ms on / off fades, a 3 ms dip for a new order, no CPU for modules that are
  off, bit-exact pass-through with everything off
- ✅ Modulation: 4 macros, 2 LFOs (free or beat-locked), an envelope follower, an 8-slot matrix
  over 48 targets
- ✅ Ten pages in five tabs, each with its own Q-Link set; the CHAIN page (tiles, MOVE, ON / OFF)
- ✅ Presets: 37 factory presets in 8 categories, level-matched on reference material
  (`make preset-levels`); user presets, favorites, the browser
- ✅ What the probe found: tails survive Stop, a long suspend clears them, zero latency, cheap getters
- ✅ The output guard: never NaN, never past +18 dBFS
- ✅ Tests: every module against theory, the rack, modulation, the plugin through a fake MPC, the
  presets' levels; x86 under ASan/UBSan and ARM under qemu

## Deferred and not planned

| Feature | Why not (now) |
|---|---|
| Two instances of a module in one rack | The fixed parameter list (automation and saved projects stay valid) has one of each; two EffectForce inserts do it |
| Sidechain input | MPC's VST2 host gives an insert 2 inputs |
| Lookahead limiting, linear-phase EQ | MPC can't compensate latency (`acceptIOChanges`: no) |
| Drawn curves (EQ, compressor) | MPC skins can't draw dynamic graphics |
| A pitch shifter module | The shimmer's shifter (dsp/pitch.h) is built for feedback paths; a module of its own would want formants and lower latency |

## Decisions

- 2026-10-05 — **Rack model:** each module once with its own parameters, reordered (not generic
  slots). Automation, Q-Links and saved projects always mean the same thing.
- 2026-10-05 — **Modules for v0.1:** Drive, Filter, EQ, Comp / OTT, Chorus, Phaser / Flanger, Delay,
  Reverb; 4 macros, 2 LFOs, an envelope follower, an 8-slot matrix.
- 2026-10-05 — **Suspend rule:** under 250 ms keeps every buffer (Stop), longer clears them (ON button).
- 2026-10-05 — **Delay time changes:** both behaviours, as Delay Glide: Tape (repeats bend in pitch)
  or Fade (a crossfade, no bend).
- 2026-10-05 — **Level-matching by category:** Synth and Pads on chords, Bass on a bass line, Drums
  on drums, the rest on the full mix (Texture and Rhythm on chords too).
- 2026-10-05 — **Shimmer, Pulse, Grain before v0.1:** the CPU budget allowed them.
- 2026-10-05 — **Frozen shimmer drains** rather than holding: what climbs out of the band leaves, the
  loop stays bounded. Shimmer 0 freezes for good.
- 2026-10-05 — **Grain's grid follows locates and loops** by the song position's distance from where
  it should be (more than a host block: a jump), not only by a new slice number.
- 2026-10-05 — **An order saved with fewer modules** keeps its slots; the new modules (off in it) fill
  the rest in the default order.
