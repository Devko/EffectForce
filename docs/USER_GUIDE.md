# User guide

- [Getting started](#getting-started)
- [The screen](#the-screen)
- [The chain](#the-chain)
- [The modules](#the-modules)
- [Modulation](#modulation)
- [Performance: scenes and the looper](#performance-scenes-and-the-looper)
- [Presets](#presets)
- [How it behaves in MPC](#how-it-behaves-in-mpc)

---

## Getting started

1. Install it (`make plugin-install`, or the release zip's `install.sh`; both restart MPC).
2. On a track, a pad, or wherever MPC offers insert effects, add **EffectForce** from the insert
   effects.
3. Open its screen. The **CHAIN** page shows the ten modules in their order; everything starts off.
4. Pick a factory preset (**PRESETS** page, or the preset stepper at the top of CHAIN), or tap a
   module's tile, press **ON / OFF**, and set it up on its own page.

## The screen

Five tabs; a tab with several pages shows dots under it, and a tap on it again shows the next page.
Every page has its own Q-Link set (MPC shows its name in the tab strip): the Force's 8 knobs reach the
page's top card, the next bank its bottom card. The PRESETS page has no knobs of its own; there the
Q-Links keep the preset stepper, the macros and the levels.

Every control's name says what it belongs to ("Comp Attack", "EQ Low Gain"), because MPC's Q-Link
overlay shows the name without the page.

| Tab | Pages |
|---|---|
| CHAIN | **CHAIN**: the order, MOVE and ON / OFF, Input, Output, Mix, the macros, the preset stepper. **PERFORM**: the scenes, the crossfader and the FX library. **LOOPER**: REC, the loop and how it plays. **PRESETS**: the browser |
| TONE | **DRIVE+FILTER**, **EQ**, **COMP** |
| MOTION | **CHORUS+PHASE**, **PULSE** |
| SPACE | **DELAY**, **GRAIN**, **REVERB** |
| MOD | **LFO+ENV** (both LFOs, the envelope follower, the macros), **MATRIX** (8 slots) |

The status line at the top of every page: how many modules run, and the CPU EffectForce takes (the
average and the peak of the last half second, in percent of MPC's audio block).

## The chain

The input goes through **Rack Input** gain, then the ten modules in order (the first row of tiles left
to right, then the second), then **Rack Output** gain; **Rack Mix** blends the result with the dry input (100% for an insert; lower to use the
whole chain in parallel).

- **The tiles** show the order. A lit tile is a module that is on; the selected one is in brackets.
- **Tap a tile** to select it, **ON / OFF** to switch it, **< MOVE** / **MOVE >** to swap it with
  its neighbour. A module's own page has the same On switch.
- Switching fades over 10 ms; a module that is off costs no CPU. A module switched on again starts
  fresh (no echo of what played minutes ago).
- Moving a module dips the sound for a few milliseconds while the order changes: no click.

Some orders worth knowing: Drive before Filter is the classic synth path; Filter before Drive makes
the drive react to the filter's resonance. Comp before Delay and Reverb keeps the tails free; Comp
after them pumps the tails with the beat. EQ last shapes everything.

## The modules

Every module has an **On** switch and, where it makes sense, its own **Mix**.

### Drive

Six characters, 2x oversampled (Crush on purpose isn't): **Soft** (round, tape-like), **Tube**
(asymmetric, warm), **Hard** (clipping with a rounded corner), **Fold** and **Sine** (wavefolders:
bright, synthetic), **Crush** (fewer bits and a lower sample rate as Drive goes up).

**Drive Amount** is the gain into the shaper (0..36 dB); the level is compensated, so it changes the
character more than the loudness. **Tone** tilts the result darker or brighter. **Bias** makes it
asymmetric (even harmonics: warmer, thicker). **Out** trims the level.

### Filter

A state-variable filter: **LP 12 / LP 24**, **HP 12 / HP 24**, **BP**, **Notch**. **Filter Cutoff**,
**Filter Res**, **Filter Drive** (saturation before the filter) and **Filter Spread**, which moves
the left and right cutoffs apart (up to an octave each way) for a wide, moving stereo sound.

### EQ

**EQ Low Cut** and **EQ High Cut** (12 dB/oct; at the end of their range they are off), a **low
shelf**, a **bell** (Mid Freq, Gain, Q) and a **high shelf**, ±18 dB. At 0 dB a band is out of the
signal entirely.

### Comp

Two modes; the selector at the top right shows the mode's card:

- **Comp:** Thresh, Ratio, Attack, Release, Knee, **Comp SC Cut** (the detector ignores the lows,
  so a bass doesn't pump everything), Comp Mix (parallel compression).
- **OTT:** three bands (split at 88 Hz and 2.5 kHz), each compressed down when loud and lifted up
  when quiet: dense, bright, in-your-face. **OTT Depth** is the amount, **OTT Time** scales the
  attack and release, **OTT Up** / **OTT Down** the two directions, **OTT Low / Mid / High** each
  band's level. **Comp Makeup** (the OUTPUT card) applies in both modes.

### Chorus

**Chorus** (two voices a side), **Ensemble** (three voices with a slow and a fast wobble, string
machine style) and **Dimension** (Juno-60 style: subtle, wide). **Chorus Delay** sets the voices'
base delay, **Chorus Lo Cut** keeps the lows out of the effect (they stay centred and solid),
**Chorus Width** the stereo spread.

### Phaser

**Phaser 4 / 8 / 12** (more stages: more notches, a deeper sweep) or **Flanger**. **Free** runs at
**Phaser Rate**; **Sync** locks the sweep to MPC's beat (**Phaser Div** from 1/16 to 16 bars, in the
same place on screen; while MPC plays the sweep follows the song position, so it is on the beat every
time). **Phaser Center** is the centre of the sweep (Hz for a phaser, the base delay in ms for the
flanger), **Phaser FB** the resonance (negative values
put the notches elsewhere), **Phaser Stereo** the phase between left and right.

### Pulse

**Tremolo** (the level), **Auto-Pan** (the place in the stereo field, at constant power) or **Gate** (a
rhythmic 16-step pattern). **Sync** runs it on MPC's beat (**Pulse Div**; for the gate, the length of
a step, and the pattern's first step on the bar's downbeat), **Free** at **Pulse Rate**. **Pulse
Depth**, **Pulse Shape** (sine, triangle, square for tremolo and pan; for the gate, hard steps to
swells), **Pulse Stereo** (the tremolo's right side ahead of the left). The **GATE** card: **Gate
Pattern** (1/16, 1/8, 1/4, Offbeat, Off 16ths, Dotted, Tresillo, Gallop, Rev Gallop, three Trance
patterns, Pump, Stutter, Half Bar, Build), **Gate Length** (how long each step stays open) and **Gate
Smooth** (its edges, so it never clicks). The patterns are named for **Pulse Div 1/16**, a bar each
(Offbeat: the eighths between the beats); at 1/8 a pattern takes two bars and plays twice as slow.

### Delay

**Stereo**, **Ping-Pong** (repeats bounce left, right, left) or **Mono**. **Sync** takes the time from
MPC's tempo (**Delay Div**, 1/64 to a bar, with triplets and dotted values), **Free** from **Delay
Time** in ms; the knob or the list shows in the same place, whichever the mode uses. **Delay FB** at
100% never runs away; the repeats hold as long as the cuts let them (with Lo Cut and Hi Cut open,
for ever).
**Delay Spread** offsets the right side's time. The feedback path has **Lo Cut** and **Hi Cut** (each
repeat a little thinner and darker), **Drive** (each repeat a little dirtier) and **Wow** (tape speed
wobble). **Delay Duck** keeps the repeats down while you play and lets them bloom in the gaps.

**Delay Glide** decides what a change of time does (a new tempo, another division, a turn of Delay
Time, modulation): **Tape** glides the time like a tape delay's motor, so repeats in flight bend in
pitch; **Fade** crossfades to the new time over about 50 ms, no pitch bend at all.

### Grain

The last few seconds you played, replayed as grains and slices on MPC's beat (in the spirit of
granular pedals such as the Microcosm). Five modes on the TEXTURE card:

- **Cloud:** many short grains from all around the recent past: a shimmering haze.
- **Stretch:** long grains that crawl through the recording far slower than it played: drones.
- **Mosaic:** slices of the beat replayed in a new order, some reversed, some an octave away.
- **Stutter:** the latest slice repeated, glitch style, then back to what you play.
- **Arp:** grains on the beat stepping through intervals.

**Grain Size** (Free, ms) or **Grain Div** (Sync, a note value) sets the grain or slice length;
**Grain Density** how many grains overlap (Cloud, Stretch) or how much varies (Mosaic, Stutter, Arp);
**Grain Pitch** in semitones; **Grain Reverse** the chance a grain plays backwards; **Grain Spread**
how far back grains reach and how wide they sit; **Grain FB** feeds the output back into the
recording, so textures evolve; **Grain Hold** freezes the recording and keeps playing it.

### Reverb

**Room**, **Hall**, **Plate** and **Space** (very long, slowly moving). **Reverb Size**, **Reverb
Decay** (0.1 to 30 s), **Reverb Pre** (the pre-delay), **Reverb Damp** (above it the tail dies faster, as in a real
room), **Reverb Lo Cut** (keeps the lows out of the tail), **Reverb Mod** (gentle movement in the
tail: lusher, never metallic), **Reverb Width** and **Reverb Freeze** (the tail holds for ever, new
sound stays out). Changing the mode clears the tail, as a hardware reverb's program change does.
**Shimmer** sends the tail through a pitch shifter as it circulates, so it blooms upward with every
pass (**Shimmer Pitch**: +12, +7, +19, or -12 for a sub shimmer). The climbing energy leaves the
audible band in the end, so with a lot of shimmer a long tail ends a little sooner than its Decay
(a very short one rings a little past it: the pitched echoes take a moment to come back). **Freeze
with shimmer** doesn't hold still: the tail keeps climbing and drains away over tens of seconds
(faster at +19 and with more shimmer); set Shimmer to 0 for a freeze that holds for good.

## Modulation

### Sources

- **Macro 1-4**: knobs on the CHAIN and LFO+ENV pages, for the Q-Links. A macro does nothing on its
  own: give it targets in the matrix.
- **LFO 1, LFO 2**: Sine, Triangle, Saw Up, Saw Down, Square, S&H, Smooth (random glides). **Free**
  (Rate, 0.01..20 Hz) or **Sync** (Div, 1/16 to 16 bars, locked to MPC's beat while it plays; Div shows
  where Rate was); **Phase** shifts the cycle against the bar.
- **Envelope**: the input's level (Attack, Release, Gain: more gain reaches the top sooner).

### The matrix

Eight slots of **Source → Target × Amount**. A target is one of 54 knobs: the levels, every
module's main controls and both LFO rates. The amount moves the knob's value by up to its whole range
(±100%), so modulation follows the knob's own curve (a cutoff sweeps evenly through octaves). Slots on
the same target add up. A delay synced to the tempo has no time of its own: **Delay Time** as a target
scales the synced time instead. A rate (Phaser Rate, Pulse Rate, LFO 1 / 2 Rate) moves only while
its owner runs free.

Ideas: Macro 1 → Filter Cutoff + Reverb Mix + Delay FB (a build-up on one knob); Envelope → Filter
Cutoff (an auto wah); Envelope → Reverb Mix with a negative amount (the reverb ducks while you play);
LFO 1 synced to 1/8 → Filter Cutoff (a rhythmic filter).

## Performance: scenes and the looper

The Octatrack's performance-mixer setup in one insert: put EffectForce on the **master** (or on a drum
submix and a melodic submix, one each), and the Force's **crossfader** moves between two **scenes**: a
clean one at A and an effect at B, picked from a library of 64. The **looper** records the next 4 or 8
bars on the grid (or keeps the bar just played) and you crossfade into it, roll it, stop it, reverse it.
Two pages, both in the CHAIN tab: **PERFORM** (scenes, crossfader, the FX library) and **LOOPER**.

### Playing it, the Octatrack way

1. Load a **Perform** preset (say Perform Mixer, or Loop Mixer 4 Bar) and learn the crossfader (below).
2. **Pick an effect:** on PERFORM, tap a tile of the FX library: it goes into scene B. Push the fader:
   the effect fades in; pull back: it fades out, a delay's or reverb's tail ringing on.
3. **Loop:** on LOOPER, set **Length** (4 bars, 8 bars, ...) and press **REC**. It records the next
   4 bars from the next 4-bar line (the line shows "REC 4 BARS IN 3 BEATS", then the bars as they
   record) and keeps them. Push **Loop** (or the fader into a scene that turns Loop up, like the Loop
   Mixer presets' scene 2): the loop plays, in time. Press REC again whenever you like: the next 4 bars
   replace the loop when they are done, without a gap.
4. **Mix:** **Blend Swap** crossfades the live input with the loop; **Layer** keeps the live input and
   plays the loop on top. Rolls, a tape stop, half speed and reverse are effects of the Loop bank: pick
   one into scene B and the fader does it.

### The crossfader

**Crossfader** (Q-Link 1 on the PERFORM page) runs from scene A (0%) to scene B (100%); the bar under it
shows where it is. To play it with the Force's own crossfader, assign it there: MPC can learn any of a
plugin's automatable parameters to the crossfader (hold **ASSIGN A** or **ASSIGN B** and move the
parameter, here Crossfader). One crossfader can drive several EffectForce instances that way (a drum
bus and a melodic bus, each with its own effects). EffectForce glides after the fader over 15 ms, so a
fast flick is smooth and a Q-Link's steps don't click.

### The FX library

64 effects in four banks of 16: **Filter** (sweeps, resonant and band filters, wobbles, flangers, a comb
riser, kills), **Space** (washes, freezes, shimmer, dub and tape echoes, an echo freeze, grain clouds),
**Loop** (rolls from 1/2 to 1/32, loops of a bar or half, tape stop, half and double speed, reverse, beat
repeat, a build roll, stutter, a layered loop) and **Rhythm** (a sidechain-style pump, trance and offbeat
gates, tremolo, auto-pan, bit crush, drive, fold, OTT, destroy).

- **< BANK** and **BANK >** page through the banks. A tap on a tile puts the effect into the scene at
  the B end (or into the scene you are editing), replacing what was there; its tile lights while scene
  B holds it as it came. The scenes' line shows each end's effect by name.
- An effect is a scene: it switches its modules on and sets them, and leaves everything else to the
  knobs. Effects that need an LFO or the matrix use **LFO 2** and **matrix slot 8**, so LFO 1 and slots
  1-7 stay yours. Effects of the Loop bank switch the looper on.
- Change a picked effect like any scene (EDIT B, turn knobs): its name gets a `*`.

### Scenes

There are eight scenes. The two rows of tiles pick the one at each end: **A** (top) and **B** (bottom).
A tile shows how many settings its scene **locks**, in brackets. Changing the scene at the end the fader
is away from changes nothing you hear: line up the next move, then bring the fader over.

- **Making a scene yourself:** tap **EDIT B** (or EDIT A). Every page now shows scene B, and you hear it
  in full. Whatever you move, on any page, is **locked** in the scene: a filter's cutoff, a delay's
  feedback, a module switched on, the looper's Loop, a macro. Tap **EDIT B** again when done: the knobs
  come back as they were. Tapping another tile in the edited row switches the edit to that scene.
- **CLEAR** (while editing) removes every lock of the scene being edited.
- While editing, every change locks, recorded automation playing back too: stop the sequence (or
  edit on a part without automation) while you set up a scene.
- **What the fader does with a lock:** knobs move smoothly between the two ends (along their own curve:
  a cutoff sweeps evenly through octaves), choices like a division or the loop's length step through
  the values in between (a roll speeds up as you push), and other choices (a filter type, a reverb mode,
  freeze) switch at the middle. Settings neither scene locks stay where the knobs are.
- **A module switched on by a scene** fades in with the fader, and when you pull back its **tail rings
  out**: push the fader into a delay or reverb scene and pull back, and the echoes carry on (a dub throw).
  A module at the Off end costs no CPU once its tail has died away.
- Modulation (LFOs, the envelope, the macros) still works on top of whatever the fader sets.
- Scenes (with their names) are saved with the project and in presets. Loading a preset ends an edit.

### The looper

The **LOOPER** page; its line says what it is doing.

- **On** arms it: from then on it records all the time (the first time, it takes 24 MB of memory, only
  in that instance). **Place**: **Pre** loops the input and sends the loop through every module,
  **Post** loops what the rack plays.
- **Length**: 1/16 to 8 bars. **REC** takes a loop of that length and keeps it (Hold turns on):
  **Capture Next** (the default) records the coming one from the grid's next line of that length, as the
  Octatrack does: a 4-bar loop starts on a 4-bar line. A press up to a beat late still takes the bars
  that have just begun (the recorder already has their start). **Capture Last** keeps the bars just
  played, at once. Press REC again while it waits to cancel.
- **Loop** plays the kept loop, in time with the beat (Swap: crossfading the live input; Layer: on top
  of it). Without a kept loop, pushing Loop grabs the last bars on the spot (a quick roll). Back at 0 it
  stops; a kept loop stays for the next push. **Hold** off forgets it.
- **Repeat** (1/2 to 1/32) repeats the part of the loop that was playing, on the grid: a roll. Lock
  Repeat 1/32 in scene B and the fader rolls in 1/2, 1/4, 1/8, 1/16, 1/32 as it moves. A **Length**
  shorter than the kept loop plays that much of it.
- **Speed**: 1.00x as recorded, 0.50x half speed (an octave down), 0 stops it (lock 0 in scene B: the
  fader is a tape stop), below 0 backwards.
- Right after arming, a loop waits until its bars have been recorded. A loop longer than 20 seconds at
  the tempo is halved (8 bars fit from 96 BPM up).

### The Perform presets

Ready-made mixers: scene 1 clean at A, scenes 2 to 8 effects of the library (pick one in the B row).
**Perform Mixer** (a low-pass sweep, a resonant high-pass, an echo throw, a 1/16 roll, a tape stop, a
hall wash, a pump), **Loop Mixer 4 Bar** and **Loop Mixer 8 Bar** (REC records 4 or 8 bars; scene 2 plays
the loop; rolls, half speed, reverse, a build roll, a layered loop), **DJ Mixer** (filters and kills),
**Dub Mixer**, **Build and Drop**, **Glitch Mixer**, **Rhythm Mixer**, **Space Mixer**, **Crush Mixer**.

### More than one EffectForce

Load as many as you like: one on a drum submix and one on a melodic submix with the same crossfader
(the Octatrack template's two buses), one on a synth track for its own sound and one on the master for
the performance. Each has its own scenes and looper; a module that is off costs nothing, and the
looper's memory is only taken where it is armed.

## Presets

- **Factory presets** in eleven categories: Utility, Synth, Pads, Bass, Drums, Lo-Fi, Space, Creative,
  Texture (Grain), Rhythm (Pulse), Perform (scenes and the looper).
  Each is level-matched on reference material at a track's usual level (chords for Synth, Pads,
  Texture and Rhythm, a bass line for Bass, drums for Drums, all of it for the rest): it comes out about as loud as it went
  in, so you compare sounds, not levels. Delay and reverb presets make up for the dry signal their Mix
  takes away. Band-pass sounds (Radio, Auto Wah) stay a little quieter: their Output stops at +9 dB.
- **PRESETS page:** categories on the left (FAVORITES and RECENT first), presets on the right; tap to
  load. **FAV** marks the loaded preset, **RND** loads a random one of the category, **SAVE** writes
  `User NNN.efp` (there is no text entry on the device), **INIT** loads Init (everything off).
- **The preset stepper** (CHAIN page, top right) walks all presets, one per Q-Link detent.
- Preset files live in the plugin's folder under `Presets/` and on the SSD under
  `EffectForce Presets/`; folders become categories. MPC's own preset save (the disk icon in its
  header) works too: it stores the whole state.

## How it behaves in MPC

- **Zero latency.** Nothing looks ahead, so nothing needs compensating.
- **Tails ring on through Stop.** MPC briefly suspends an insert when the transport stops; EffectForce
  keeps its delay lines and reverb through that. Switching the insert's slot **off** for longer
  clears them, so switching it back on never replays old echoes.
- **Projects** save the whole rack (every module, the order, the matrix, the scenes) and the preset it
  came from. The looper's recording isn't saved: it starts again when the project loads.
