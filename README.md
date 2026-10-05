# EffectForce

**An effect rack that runs natively inside MPC on the Akai Force.**

EffectForce is a VST2 insert effect for MPC OS's built-in plugin host, with its own touchscreen pages
and Q-Link sets: ten modules in one insert slot, in any order, with macros, LFOs and a modulation
matrix across all of them. It is built on the same groundwork as its siblings
[PolyForce](https://github.com/Devko/PolyForce) and [SubForce](https://github.com/Devko/SubForce).

> [!NOTE]
> **Preview.** The plugin ID (`EfFc`), the file name (`effectforce.so`) and the parameter list may
> still change before v0.1.

## Highlights

- **Ten modules, any order:** Drive (2x oversampled: soft, tube, hard, fold, sine, crush), Filter
  (12/24 dB state-variable: low, high, band, notch; stereo spread), EQ (low and high cut, two shelves,
  a bell), Comp (a compressor with sidechain low cut, or OTT-style 3-band upward and downward
  compression), Chorus (chorus, ensemble, Juno-style dimension), Phaser (4, 8 or 12 stages, or a
  flanger; synced to the beat), Pulse (tremolo, auto-pan, a 16-step rhythmic gate), Grain (granular
  textures on the beat: cloud, stretch, mosaic, stutter, arp; hold and feedback), Delay (stereo,
  ping-pong or mono; tape wow, feedback filters and drive, ducking; tape glide or crossfade) and
  Reverb (room, hall, plate, space; an 8-line feedback delay network with modulation, freeze and
  shimmer)
- **The CHAIN page:** the order as tiles; move a module left or right, switch it on or off. Switching
  fades, reordering never clicks, and a module that is off costs no CPU
- **Modulation:** 4 macros, 2 LFOs (free or locked to MPC's beat), an envelope follower, and an
  8-slot matrix reaching 48 parameters across the chain
- **A performance mixer, the Octatrack way:** eight scenes of parameter locks, scene A and B at the
  ends of the Force's crossfader; modules a scene switches fade in with the fader and their tails ring
  out when it comes back (dub throws, washes); a looper that grabs the bar just played and plays it on
  the beat, rolls it (1/2 to 1/32), slows, stops (tape stop) or reverses it. On the master it turns the
  whole mix into a performance ([how](docs/USER_GUIDE.md#performance-scenes-and-the-looper))
- **Light on the CPU:** everything on at its heaviest, p99 11.3% of a block on the Force
- **Built for MPC:** zero latency, tails that ring on through Stop, every getter cheap (MPC polls them
  hundreds of times a second), all measured on the device first ([the probe](docs/PROBE.md))
- **62 factory presets** in 11 categories (Perform: ready-made performance-mixer setups),
  level-matched on reference material so a preset changes the sound more than the level; user presets,
  favorites, a browser

## Documentation

| Document | What's in it |
|---|---|
| [User guide](docs/USER_GUIDE.md) | The pages, the chain, every module, modulation, the scenes and the looper, presets |
| [Design](docs/DESIGN.md) | The rack, the modules, modulation, pages, budget: what v0.1 is and why |
| [Architecture](docs/ARCHITECTURE.md) | Source layout, the signal path, threads and real-time rules, saved state |
| [Performance](docs/PERFORMANCE.md) | The CPU budget and what each module costs on the Force |
| [Probe](docs/PROBE.md) | What MPC does with an insert effect, measured on the Force before the design |
| [Roadmap](docs/ROADMAP.md) | What's done, what's next, decisions |

## Building

Linux or WSL, as for PolyForce ([its build docs](https://github.com/Devko/PolyForce/blob/main/docs/BUILDING.md)
list the packages):

```sh
make test                  # every module and the plugin under ASan/UBSan
make test-arm              # the same suite built for the Force, under qemu-arm
make test-module M=reverb  # one module's tests on their own
make arm-plugin            # build/arm/effectforce.so
make skin preview          # the skin, and every page as surface/build/page_*.png
make plugin-package        # dist/EffectForce-<version>-mpc-armv7.zip
make plugin-install        # onto the Force (FORCE=root@<ip> in local.mk); restarts MPC
make bench-device          # CPU per module and all at once, on the Force
make levels                # the factory presets' levels (make preset-levels sets them)
```

`surface/surface.py` is the single source of the parameter list, the pages and the factory presets'
checks; it writes `params.json`, `layout.conf`, `vst.json` and the C++ headers.

## License

MIT. The vendored generator and installer: MIT, Copyright (c) 2026 sd88me
(`third_party/mpc-vst-plugins/LICENSE`).
