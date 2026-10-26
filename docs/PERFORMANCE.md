# Performance

- [The budget](#the-budget)
- [Device measurements](#device-measurements)
- [Instruction counts](#instruction-counts)
- [What keeps it cheap](#what-keeps-it-cheap)

---

## The budget

MPC renders 128-frame blocks: **2902 µs per block**, per plugin instance. A plugin passes at
**p99 ≤ 15%** of the block (warns up to 35%). EffectForce's ceiling is **every module on** at its
heaviest settings with every matrix slot busy: what the worst preset could reach.

`make bench-device` copies the `.so` and `efbench` (tools/bench.cpp) to the Force, runs them pinned
to core 1 while MPC keeps running, and deletes them. Each case feeds noise in place through the
plugin's own entry points and times every block with the thread's CPU clock.

## Device measurements

2026-10-26, the Force, 0.0.3 and 0.0.4 side by side: the same bench (with the automation cases), the
same hour, the profile-guided local builds (GCC 13; glibc 2.38), percent of the 2902 us block:

| Case | 0.0.3 (avg / p99) | 0.0.4 (avg / p99) |
|---|---|---|
| Everything off | 0.27 / 0.52 | 0.27 / 0.47 |
| Reverb alone (Hall, full modulation) | 2.41 / 3.05 | 1.92 / 2.60 |
| **Everything on, heaviest**, 8 mod slots | **12.20 / 14.54** | **11.55 / 13.67** |
| Looper rolling, fader sweeping | 6.07 / 7.47 | 5.16 / 6.72 |
| That, everything on heaviest | 13.64 / 16.31 (WARN) | 12.58 / 15.12 (WARN) |
| A scene holding 14 locks still | 4.45 / 5.57 | 3.92 / 5.05 |
| The same scene moving all 14 every chunk | 5.37 / 6.67 | 4.84 / 6.20 |
| That, everything on heaviest | 13.48 / 15.88 (WARN) | 12.73 / 15.32 (WARN) |
| **Automation: 2 knobs every block, everything on** | **13.39 / 15.86 (WARN)** | **11.79 / 13.90** |
| That, the looper, the fader sweeping | 14.37 / 17.03 (WARN) | 13.41 / 16.15 (WARN) |

Automation playing over everything on went past the budget in 0.0.3 (the bench never wrote a knob,
so it never showed); 0.0.4 keeps it within, and everything on alone has 1.3 points to spare. The
reverb gains four times on the device what its instruction count says (-20%, -5% in instructions):
the lines' positions in NEON also took eight VFP-to-core transfers a sample out, and those stall on
the A17. Every module alone is otherwise as it was (within the device's run-to-run 0.5 at p99).

2026-10-06, the Force, the ten modules and the performance layer, the profile-guided build (GCC 13,
local; glibc 2.38), percent of the 2902 µs block:

| Case | avg | p99 | max |
|---|---|---|---|
| Everything off | 0.27 | 0.58 | 0.87 |
| Drive / Filter / EQ / Comp alone | 1.37 / 0.76 / 0.68 / 0.58 | 1.79 / 1.08 / 1.08 / 0.89 | 2.13 / 1.42 / 1.31 / 1.20 |
| Chorus / Phaser / Pulse alone | 0.89 / 0.86 / 0.42 | 1.30 / 1.21 / 0.77 | 1.66 / 1.79 / 1.00 |
| Grain / Delay / Reverb alone | 2.09 / 0.94 / 2.42 | 2.73 / 1.42 / 3.20 | 2.94 / 1.68 / 3.60 |
| **Everything on, heaviest**, 8 mod slots | **11.97** | **14.90** | 16.37 |
| Looper rolling, fader sweeping (scene 2: four sends, a dozen locks) | 6.01 | 7.70 | 8.27 |
| That, everything on heaviest | 13.40 | 16.94 (WARN) | 18.98 |
| A scene holding 14 locks still (six modules on) | 4.50 | 5.78 | 6.64 |
| **The same scene moving all 14 every chunk** (a 1-beat Loop, 7 on a log curve) | **5.43** | **7.08** | 8.30 |
| That, everything on heaviest | 13.32 | 16.56 (WARN) | 18.63 |

Everything on at its heaviest passes with nothing to spare (p99 14.9 of 15; the first eight modules
had 11.3, below), and the performance layer on top of it goes past (16.6-16.9): a warning, not a
failure, and the worst a patch can reach, not what Perform presets do (6-8). A scene move costs what
any moving parameter costs, mostly the modules working out their coefficients again: 14 locks moving
every chunk add 0.9 points on average and 1.3 at p99 to the same scene held still. An effect of the
FX library moves one to three over bars. Run to run, the device's p99 moves by about 0.5.

2026-10-05, the Force (Cortex-A17), a local build (GCC 13, not profile-guided), percent of the
2902 µs block. "Alone" includes the plugin's own overhead (the first row).

| Case | avg | p99 | max | Design target (the module's share) |
|---|---|---|---|---|
| Everything off | 0.33 | 0.62 | 0.89 | |
| Drive alone (Tube, 24 dB, 80% mix: both halfband pairs) | 1.44 | 1.85 | 2.19 | ≤ 1.5 |
| Filter alone (LP 24, resonant, drive, spread) | 0.72 | 1.07 | 1.25 | ≤ 0.8 |
| EQ alone (every band) | 0.71 | 1.01 | 1.34 | ≤ 0.8 |
| Comp alone (compressor, sidechain cut) | 0.66 | 1.00 | 1.27 | ≤ 0.8 |
| Chorus alone (Ensemble) | 0.95 | 1.30 | 1.53 | ≤ 0.8 |
| Phaser alone (Phaser 8) | 0.94 | 1.32 | 1.51 | ≤ 0.8 |
| Delay alone (ping-pong, wow, drive, ducking) | 0.98 | 1.40 | 1.73 | ≤ 0.8 |
| Reverb alone (Hall, 6 s, full modulation) | 2.58 | 3.30 | 4.46 | ≤ 4 |
| **Everything on, heaviest** (Drive Fold, OTT, Phaser 12, Space reverb), 8 mod slots | **9.10** | **11.31** | 13.32 | p99 ≤ 15 |

Every case passes. Net of the overhead, every module is within (or, for Chorus and Delay, about
0.1-0.3 point above) its share of the design budget; the total, which is what MPC sees, has 3.7
points to spare. These are the first eight modules: Pulse, Grain and the shimmer came after, and
their device run is still to do ([Roadmap](ROADMAP.md)); until then, the instruction counts below.
The release build is profile-guided (as PolyForce's and SubForce's: `make arm-plugin` with qemu-arm,
and CI), worth about 2% here: the modules are hand-tuned already.

## Instruction counts

Without the device, ARM instructions per 128-frame block are the measure: the bench's cases
(tools/bench.cpp) through the plain `.so` (GCC 13, the device flags) under a plugin-enabled
qemu-arm, per `processReplacing` call. On the first eight modules' everything-on case, 328k
instructions were 9.1% of the block on the Force, Hall alone 98k net for 2.25%: about 36k-44k
instructions to a point of the block. They miss what the Force adds (cache misses, VFP divisions and
flag transfers that stall), which the device's p99 over its average (1.24) shows.

`make bench-count` counts them: it builds the plain `.so` (unstripped, with `-g`), the ARM bench and
tools/efcount.c, a qemu plugin, and runs the bench with `-m` under a qemu-arm built with plugins
(`QEMU_PLUGINS`, and `QEMU_PLUGIN_INCLUDE` for the folder with its `qemu-plugin.h`, the 8.2 API; both
can go in local.mk). Every case prints its thousands of instructions per block (average, p99 and max
over 2 s, 689 blocks) and two hashes of its output, the warm-up's and the whole case's: a change meant
to keep the sound bit for bit keeps every hash, built with the same compiler. The counts move by up to
0.1k from run to run: the plugin times itself for its CPU meter, under qemu on the host's clock, so a
few blocks update the meter's text in one run and not in the next. `CASES=11,17` runs only those,
numbered as it lists them. `PROF=<file>` also writes every translated block that ran in the
measured calls (`vaddr insns execs`); the log, build/arm/count/log.txt, has the bench's memory map:
an address less `BASE` is the `.so`'s, for `arm-linux-gnueabihf-addr2line`.

2026-10-05, the ten modules, thousands of instructions per block:

| Case | At the hand-off (avg / p99) | Now (avg / p99) |
|---|---|---|
| Everything off (the plugin's own work) | 17.8 / – | 9.8 / – |
| Pulse alone (Gate) | 25.1 / 25.3 | 17.1 / 17.3 |
| Grain alone (Cloud, densest, +12, feedback) | 97.2 / 108.1 | 72.1 / 79.8 |
| Reverb alone (Space, full modulation, shimmer 0.6) | 140.7 / 284.6 | 120.0 / 146.1 |
| **Everything on, heaviest**, 8 mod slots | **446.5 / 592.7** | **402.5 / 431.3** |

Everything on is about 9-11% of the block on average by that measure. At the hand-off its p99 was
the shimmer's splice search (a correlation of a few thousand multiply-adds in one sample, every 80 ms:
one block in 28, a p99 of over 16% before the device's own jitter); now the search sums sixteen lags
at a time in vector lanes, and Grain, the rack's per-sample overhead and the control paths are
leaner too (the commits of 2026-10-05 say what and by how much). Most of the changes leave the
output bit for bit as it was; four move it by float rounding, the compiler fusing multiply-adds
differently: the shimmer's shifter reads and its path after the network loop (-130 dB), Grain's
read segments aligned to time since reset (-100 dB) and the modulation of log-curve targets by the
fast exp2 (1e-7 of the value).

2026-10-26, round three (a full review's performance findings), thousands of instructions per
block. Counted exactly (`make bench-count`): a qemu TCG plugin counts every instruction executed
between two marker syscalls the bench makes around each `processReplacing` call (the plain `.so`, GCC
13, the device flags), and every case's output is hashed, so a change that should leave the sound
bit for bit is checked to. Two new cases: automation playing (two knobs written every block; a
Q-Link turned is the same), which the bench didn't cover and which cost the most:

| Case | Before (avg / p99) | Now (avg / p99) |
|---|---|---|
| Drive alone (Tube) | 47.4 / 47.5 | 45.8 / 45.9 |
| Comp alone | 21.4 / 21.4 | 20.2 / 20.2 |
| Reverb alone (Hall, full modulation) | 106.3 / 113.8 | 101.1 / 109.1 |
| **Everything on, heaviest**, 8 mod slots | **407.2 / 435.1** | **399.8 / 425.0** |
| Looper rolling, fader sweeping | 269.3 / 296.2 | 237.2 / 252.1 |
| That, everything on heaviest | 483.3 / 516.1 | 447.7 / 478.7 |
| Scene moving 14 locks, everything on heaviest | 450.0 / 479.5 | 436.7 / 463.3 |
| **Automation: 2 knobs every block, everything on** | **455.8 / 482.4** | **406.8 / 430.1** |
| That, with the looper and the fader | 515.2 / 548.0 | 486.0 / 517.6 |

- **A knob written cost a rebuild of every parameter** (~45k instructions, a point of the block, in
  every block a Q-Link turns or automation plays): one no scene locks now goes straight into the patch,
  as a rebuild would set it (test/scenes_test.cpp: sample for sample the rebuilt engine's).
- **Every write made the surface look at all 318 parameters** for values to push back to MPC (12.7k
  a block while the fader moves), and the plugin compared and copied the whole snapshot twice: a
  bitmap of what was written, one compare.
- **The looper** called libm's `floor` and `cos` per sample and read its four Hermite frames through
  four bounds-checked calls: the fast floor, a table for the jump's crossfade, one run of frames when
  all four are copied.
- **The reverb** converted each line's position to an index and a fraction one line at a time (VFP,
  and a transfer to the core per line): eight at once in NEON; the shimmer's splice search gathered
  three times the samples it reads (its spike sets the p99); the tail length with shimmer, solved again
  whenever Shimmer, Decay or Size move (a scene moving Shimmer: every block), takes Newton's four or
  five steps in logs instead of a 40-step bisection.
- **OTT and the compressor** stepped ramps that stood still, every sample; **Drive** worked out its
  controls (two `exp2` and the compensation) every chunk whether they changed or not.

All of it is bit for bit as before but the looper's frames, which the compiler fuses differently
(-130 dB, -116 dB after the reverb). On the device: [Device measurements](#device-measurements).

## What keeps it cheap

- **A module that is off costs nothing:** the rack skips it entirely.
- **Control rate:** parameters, modulation and coefficients are worked out once per 32-sample chunk;
  every module glides across the chunk.
- **NEON where the work is parallel:** left and right as two lanes (Filter, EQ, Comp, Delay), the
  halfband filters of both sides in one vector (Drive), four chorus voices per load, the reverb's
  eight lines as two vectors, two of a grain's samples (left and right each) per vector, the
  shimmer's splice search sixteen lags at a time.
- **No VFP compares per sample where NEON lanes do:** VFP has no min or max, and moving its flags
  to the core stalls; limiters, envelope followers and gates keep their state in a NEON lane.
- **No libm per sample:** SubForce's fast math (`dsp/fastmath.h`) and per-chunk coefficients; what
  needs libm (a tail length, a pitch's speed, a loop gain) is worked out again only when what it
  depends on changes.
- **Settled is free:** a gain or mix that isn't moving runs as a constant, four samples at a time
  (and a unity gain not at all).
- **Cheap resets:** a module switched back on doesn't clear megabytes of delay line on the audio
  thread; the Delay and the Reverb count what they have written since and read older samples as
  silence.
- **Cheap getters:** MPC polls names and texts hundreds of times a second; they read caches.
- **A write costs what it changes:** a knob turned (a Q-Link, automation) is written into the patch on
  its own, not by rebuilding every parameter; what MPC should read back is looked at only for the
  parameters written since the last block.
