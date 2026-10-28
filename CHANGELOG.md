# Changelog

Releases are built by CI from a `vX.Y.Z` tag (see [Releases](README.md#releases)); the section for the
tag's version becomes the release's notes. While the version is 0.x the parameter list may still change
between releases.

## Unreleased: EffectForce is now EffectWerk <!-- werk-rename: keep -->

**A new name, the same effect rack.** "Force" is the name of Akai's device, so it no longer belongs in this
plugin's name. Nothing about the sound changes.

- MPC's plugin list shows **EffectWerk**, and the plugin lives in `/sdcard/Synths/Devko - VST - EffectWerk`.
- Your presets, favorites and recents move over by themselves: the installer carries them out of the old `Devko - VST - EffectForce` folder and removes it, and the first start renames `/media/AkaiForce/EffectForce Presets` to `EffectWerk Presets`. <!-- werk-rename: keep -->
- Projects saved with EffectForce open with EffectWerk and their settings: the plugin keeps EffectForce's plugin ID, and it reads presets and projects saved under the old name (the same way checked with SubWerk on a Force). <!-- werk-rename: keep -->

## 0.0.4

A third full review of the code, and a lighter audio thread (no crash, memory or audio-thread problem
was found; each fix below has a test that fails without it).

- **Q-Links and automation cost a fraction of the CPU they did:** every knob MPC wrote (a Q-Link
  turned, automation playing back) made EffectWerk work out every parameter again, and every write,
  the crossfader's too, made it look through all of them for values to show MPC; with every module
  on, automation playing cost 12% more than the same sound standing still. Now a knob costs about
  what it changes. On the Force, every module on at its heaviest with automation playing went past the
  CPU budget in 0.0.3 (p99 15.9% of a block) and stays within it now (13.9%); every module on alone
  14.5% -> 13.7%, the looper rolling under the fader 7.5% -> 6.7%, the reverb a fifth lighter. The
  sound is the same, bit for bit, but for the looper's float rounding (-116 dB).
- **Grain:** after a jump to shorter or denser grains (a preset, a scene, a Q-Link) the cloud could
  fall silent for up to 1.3 s, waiting out the old grain interval.
- **Presets:** a preset file that won't load (empty, or half-copied onto the card) no longer stops
  PREV / NEXT and the stepper at it: they step over it.
- **Filter:** its tail was reckoned far too short at very high cutoffs, so a resonant filter a scene
  switches out could be stopped while it still rang.
- **The crossfader's bar** is redrawn when the fader goes back to where a loaded project had left it.

## 0.0.3

**Melodic**, a new preset category (72 presets in all): ten chains for melodic techno and house, made for
SubWerk's melodic set and fine after any synth — Melodic Bass, Lead Delay Plate, Lead Throw, Pluck
Space, Arp Dotted, Stab Echo, Sequence Pump, Pad Wash, Drone Space, Riser Wash. They follow what that
genre's synth presets do with their effects, measured over several hundred of them: the delay before
the reverb, darkened repeats, plates and halls with no pre-delay at low mixes, a low cut on everything
but the bass. Level-matched like the rest.

**Q-Links on track edit → automation:** MPC rebuilds its parameter screen whenever a plugin asks it to
redraw its texts, and a Q-Link turned meanwhile can lose its value (found by a port author on the
plugin catalog's wrapper, 2026-10-21). EffectWerk never asked when a sound parameter changed; the
texts that do change (the status line's CPU figures, the fader's bar, the scenes' and the looper's
lines) now ask at most once per 50 ms, instead of up to every 12 ms. And the build stops if a parameter
ever changes its index (`surface/params.lock`): projects, automation and Q-Link maps store them by index.

## 0.0.2

Fixes from a full review of the code (no crash, memory or audio-thread problem was found; each fix
below has a test that fails without it).

- **Looper, no more clicks on a re-capture:** a REC press, a Repeat change or a capture landing while
  a 5 ms crossfade was still under way cut it short. Most audibly: REC (Last) just after a bar line,
  or a Capture Next of the same length as the loop playing. Jumps now wait for the crossfade
  (at most 5 ms).
- **Looper:** switching Blend (Swap / Layer) glides instead of stepping; the looper's line says LOOP
  KEPT only while Hold keeps it.
- **Grain:** a held Mosaic started mid-slice played off the beat grid.
- **Scenes:** CHAIN's ON / OFF while editing a scene is locked in it (it was undone when the edit
  ended); REC's Hold stays on after an edit; loading a preset or an FX tile never plays half of it.
- **Effects a scene fades in** wake from cleared state after a rest: a compressor without its old
  gain reduction, a synced phaser without a stutter.
- **Smaller:** OTT lets its crossovers ring out; the EQ's cuts switch fully off at the ends of their
  ranges; a hand-edited value too big for its range reads as the maximum; a category tile tapped after
  a rescan opens its own category; SAVE and the preset scan wait less on the SD card.

## 0.0.1

The first public preview: an effect rack and an Octatrack-style performance mixer in one insert slot.

- **Ten modules, in any order:** Drive (2x oversampled: soft, tube, hard, fold, sine, crush), Filter
  (12/24 dB state-variable; low, high, band, notch; stereo spread), EQ (cuts, shelves, a bell), Comp
  (a compressor, or OTT-style 3-band upward and downward compression), Chorus (chorus, ensemble,
  dimension), Phaser (4, 8 or 12 stages, or a flanger; synced), Pulse (tremolo, auto-pan, a 16-step
  gate), Grain (cloud, stretch, mosaic, stutter, arp on the beat), Delay (stereo, ping-pong, mono;
  tape wow, drive, ducking; Tape or Fade when its time changes) and Reverb (room, hall, plate,
  space; freeze, shimmer). The CHAIN page moves them and switches them; switching fades, reordering
  never clicks, a module that is off costs no CPU.
- **Modulation:** 4 macros, 2 LFOs (free or locked to MPC's beat), an envelope follower and an
  8-slot matrix over 54 parameters.
- **The performance mixer:** two of eight scenes at the ends of a crossfader (learn the Force's
  own to it); a module a scene switches in fades with the fader and its tail rings out when you pull
  back. An FX library of 64 effects in four banks, a tap puts one in scene B; 11 of them move by
  themselves from the next bar line once the fader brings them in (an 8-bar filter sweep, a build
  roll, a tape stop), Once, Loop or Ping-pong, their LENGTH and PLAY on the PERFORM page. A looper
  that records the next bars on the grid (or keeps the last), and plays, layers, rolls (1/2 to
  1/32), slows, stops or reverses them.
- **62 factory presets** in 11 categories, level-matched, ten of them ready-made performance
  mixers; user presets, favorites, a browser.
- **Fourteen touchscreen pages** in five tabs, a Q-Link set each.
- **On the Force:** all ten modules on at their heaviest p99 14.9% of a block, a Perform preset
  6-8% (measured on the device, profile-guided); zero latency, tails ring on through Stop.
- **Builds:** armhf against glibc 2.31, profile-guided, the test suite run against the shipped
  objects, checked with the plugin catalog's `catalog_check.py`. Tested on an Akai Force with MPC OS
  3.9; MPC OS 2.x is untested (it doesn't draw a plugin's pages).
