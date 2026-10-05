# Performance

- [The budget](#the-budget)
- [Device measurements](#device-measurements)
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
points to spare. A profile-guided build (as PolyForce and SubForce ship) is the next lever if it is
ever needed.

## What keeps it cheap

- **A module that is off costs nothing:** the rack skips it entirely.
- **Control rate:** parameters, modulation and coefficients are worked out once per 32-sample chunk;
  every module glides across the chunk.
- **NEON where the work is parallel:** left and right as two lanes (Filter, EQ, Comp, Delay), the
  halfband filters of both sides in one vector (Drive), four chorus voices per load, the reverb's
  eight lines as two vectors.
- **No libm per sample:** SubForce's fast math (`dsp/fastmath.h`) and per-chunk coefficients.
- **Cheap resets:** a module switched back on doesn't clear megabytes of delay line on the audio
  thread; the Delay and the Reverb count what they have written since and read older samples as
  silence.
- **Cheap getters:** MPC polls names and texts hundreds of times a second; they read caches.
