# Roadmap

Status: ✅ done · 🔜 next · ⬜ planned · 💤 deferred

## What's next

### Hand-off (2026-10-05, branch `shimmer-grain-pulse`)

Work in progress, moved from the local machine to a cloud session. Shimmer, Pulse and Grain are
integrated on this branch (parameters, pages, rack with ten modules, presets, docs); `main` holds
the last state that passed everything (Delay Glide).

| Item | State |
|---|---|
| Shimmer (dsp/reverb.*, dsp/pitch.h) | Done: 163 module checks pass on x86 and ARM; +27k ARM instructions per block when on |
| Pulse (dsp/pulse.*) | Done: 68 module checks pass on x86 and ARM; ~7.5k instructions per block |
| Grain (dsp/grain.*) | Nearly done: its last run passed all 299 module checks on x86 (`make test-module M=grain`); **the ARM run (`make test-module-arm M=grain`) was never done**, and nobody reviewed it yet. The spec is the Grain row in [Design](DESIGN.md#modules): five modes (Cloud, Stretch, Mosaic, Stutter, Arp), synced to MPC's beat, cheap reset (hide old samples like the Delay), target ≤ 2.5% of a block |
| Integration (surface.py, rack, rack_map, bench, tests, presets) | Written; the full suite compiled and passed everything except Grain's (then unfinished) checks and the preset levels |
| Factory presets | 52 (15 new). `tools/make_presets.py` regenerated all of them, which drops every out_gain: **all 52 need `make preset-levels`** before `make test` passes |

To finish, in order:

1. `make test-module-arm M=grain`; fix what fails.
2. `make preset-levels`, then `make test` and `make test-arm`: all must pass (the last full run before
   the hand-off: 12867 passed, 20 failed, all of them Grain's and the levels).
3. A review of dsp/grain.* (as the other modules had: real-time safety, clicks, transport jumps,
   buffer indexing, NaN), and fix its findings.
4. Merge the branch into `main`; CI must be green.
5. On the device (needs the local machine: the Force is on the home network): `make bench-device`
   with all ten modules (budget: everything on at its heaviest, p99 ≤ 15%; Grain and Shimmer add
   about 2-3 points to 11.1%), then `make plugin-install` (ask first: it restarts MPC) and play
   Shimmer, Pulse and Grain.

Notes: the profile-guided build (PGO, `make arm-plugin` with qemu-arm) gained only ~2% here (the
modules are hand-tuned already). Device access and the WSL toolchain (arm-linux-gnueabihf-g++,
qemu-arm, a Python with Pillow as `local.mk`'s PY) are the local machine's.

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
| Shimmer, granular, pitch | After v0.1, if the CPU budget allows |

## Decisions

- 2026-10-05 — **Rack model:** each module once with its own parameters, reordered (not generic
  slots). Automation, Q-Links and saved projects always mean the same thing.
- 2026-10-05 — **Modules for v0.1:** Drive, Filter, EQ, Comp / OTT, Chorus, Phaser / Flanger, Delay,
  Reverb; 4 macros, 2 LFOs, an envelope follower, an 8-slot matrix.
- 2026-10-05 — **Suspend rule:** under 250 ms keeps every buffer (Stop), longer clears them (ON button).
- 2026-10-05 — **Delay time changes:** both behaviours, as Delay Glide: Tape (repeats bend in pitch)
  or Fade (a crossfade, no bend).
- 2026-10-05 — **Level-matching by category:** Synth and Pads on chords, Bass on a bass line, Drums
  on drums, the rest on the full mix.
