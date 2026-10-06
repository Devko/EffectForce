# Changelog

Releases are built by CI from a `vX.Y.Z` tag (see [Releases](README.md#releases)); the section for the
tag's version becomes the release's notes. While the version is 0.x the parameter list may still change
between releases.

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
