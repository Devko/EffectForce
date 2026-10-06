# Scene moves: design

A scene that moves by itself: an effect in the FX library carries a timed move (LP Sweep closes from
18 kHz to 150 Hz over 8 bars), and it plays once the fader brings that scene in. Today a scene holds
still values and every sweep is the hand on the crossfader; synced LFOs can sweep for up to 16 bars,
but they repeat and are locked to the song's grid, so nothing starts when you push the fader.

Decided with the user on 2026-10-06:

- **Motion lives in the scene** (not an auto-fader that rides from A to B): the fader stays the A/B
  mixer, and an effect brings its own move.
- **Trigger:** the fader leaving A arms the move; it starts on the next bar line.
- **At the end:** each effect says Once (holds), Loop or Ping-pong.
- **Editing:** moves come with the FX library and are kept in scenes and presets. On the Force you set
  a scene's LENGTH and PLAY, but you don't draw moves.
- **Approach:** moving locks (below), not a modulation source or a one-shot LFO 2. A riser can move
  several parameters to exact values, steps walk in order, and the user's matrix and LFOs stay free.

## Behaviour

- **A move** is part of a scene: some of its locks have a **start** value besides the lock (the end),
  and the scene has a **LENGTH** and a **PLAY** mode. What can move: the Line kinds, continuous (on
  the knob's curve, so a cutoff sweeps on its log curve) and ordered steps (Repeat 1/4 → 1/32 walks
  the steps). Switches, modes and a module's On can't move. A module a scene switches still fades in
  with the fader as a send.
- **A scene's move runs while that scene is heard, and resets once it is fully out.**
  - The fader leaving A arms B's move. It starts on the next bar line (on the next beat for a LENGTH
    under a bar). Until then B plays its start values.
  - LENGTH: Off · 1 beat · 2 beats · 1 bar · 2 bars · 4 bars · 8 bars · 16 bars. Off freezes the scene
    at its end values, today's behaviour.
  - PLAY: **Once** holds the end, **Loop** starts again, **Ping-pong** goes there and back (LENGTH
    each way).
  - The fader back at A resets B's move, ready for the next push. Scene A mirrors it: its move arms
    when the fader leaves B. At load with the fader at A, A's move starts on the first bar line.
  - With the fader in the middle you hear half the move: the fader still blends A and B.
- **Timing:** once started, a move counts beats at the current tempo, chunk by chunk: a tempo change
  bends it, and a locate or the sequence's loop point doesn't make it jump. While MPC is stopped it runs
  on at the tempo, as the synced modules and the looper do. Bars are 4 beats.
- **On the Force:** PERFORM gets **LENGTH** and **PLAY**, for the scene at B or the scene being edited
  (the FX tiles' rule). They are not lockable. The scenes' line shows a running move's progress
  (`B: LP Sweep ▸ 3/8`). In EDIT the knobs show, and you hear, the end values. Turning a moving
  parameter there makes it a plain lock (its move goes) and marks the scene `*`.

## Engine and lock store

- **Store** (`plugin/scenes.*`): a second table per scene of start values, atomics like the locks, -1
  where a parameter doesn't move. Per scene, the LENGTH index and the PLAY mode. A plain `lock()`
  clears that parameter's start. New calls: `setStart()`, `start()`, `setMove()`. Every change moves
  the generation counter, as today.
- **Move clocks** (`plugin/engine.*`): one per fader end, *idle → armed → running → done*, plus the
  beats elapsed. Each 32-sample chunk advances them with the tempo and arms or resets them by the rule
  above, read from the fader's target (`xTarget_`), not the smoothed `x_` (its 15 ms tail never quite
  reaches 0). Picking another scene at an end, or loading an FX tile into it, re-arms that end.
- **Values:** `Lock` gains the start at each end. An end plays `start + (end − start) · shape(phase)`,
  a ramp for Once, a sawtooth for Loop and a triangle for Ping-pong. That feeds `Scenes::morph(a, b, x)`
  as it is, so the fader blend, the step walk and the matrix on top don't change.
- `rebuild()` drops a parameter whose two ends are equal (`a != b`). With moves, it must also compare
  the starts: B sweeping from 18k down to a knob already at 150 Hz is equal at the end and still moves.
- `morph()` runs each chunk while the fader glides, as today, and also while either clock is armed or
  running. With nothing moving, the cost is zero. A log-curve parameter goes through the fast `exp2`,
  as `modulate()` does, not `powf`.

## Files and saved state

- A moving lock is `start>end` on its usual line. The scene's timing goes on `move=` and `play=`
  lines, written only for a scene that has a move. Values stay real values and options names:

  ```
  effectforce-fx 1
  name=LP Sweep
  move=8 bars
  play=Once
  flt_on=On
  flt_type=LP 24
  flt_cut=18000>150
  flt_res=0.3
  ```

- Saved state and presets: `scene2.flt_cut=18000>150`, `scene2.move=8 bars`, `scene2.play=Once`.
  Files without arrows load exactly as before (the format version stays 1).
- `surface.py` checks the new form: an arrow only on a Line-kind parameter, both values in range,
  `move` and `play` valid names.
- `tools/make_fx.py`: a move is a tuple, `flt_cut=(18000, 150), move="8 bars", play="Once"`.
- Tapping an FX tile loads the effect's `move`/`play` with its locks. An effect without a move leaves
  the scene's timing alone.

## The library

11 of the 64 effects get a move: the ones already about motion. The colours and the LFO and envelope
effects stay as they are, and every bank keeps 16 tiles.

| Effect | Move |
|---|---|
| LP Sweep | cutoff 18k → 150 Hz, 8 bars, Once |
| HP Sweep | cutoff 20 Hz → 1.5k, 8 bars, Once |
| Comb Riser | flanger centre 0.2 → 0.95, feedback 0.6 → 0.95, 4 bars, Once |
| Build Roll | Repeat 1/4 → 1/32, HP 200 → 2.5k, 4 bars, Once |
| Build Gate | gate 1/8 → 1/32, 4 bars, Once |
| Tape Stop | Speed 1 → 0, 1 beat, Once |
| Hall Wash | mix 0 → 0.6, 4 bars, Once |
| Shimmer Wash | mix 0 → 0.6, shimmer 0 → 0.6, 8 bars, Once |
| Echo Throw | feedback 0.4 → 0.85, 2 bars, Once |
| Notch Sweep | cutoff back and forth, 4 bars, Ping-pong |
| Phaser Sweep | centre back and forth, 4 bars, Ping-pong |

## Testing

- `test/scenes_test.cpp`, through the plugin's entry points:
  - **Store:** start values round-trip, a plain lock clears a start, the generation moves.
  - **Clock:** the fader leaves A at bar 3 beat 2, and the move starts at bar 4.0, not before. It
    ends after exactly LENGTH beats. Once holds, Loop restarts, Ping-pong returns. Back at A resets,
    and a second push starts again on the bar line. A tempo change bends it, a locate doesn't jump
    it, and it runs on while stopped.
  - **Values:** at B, the cutoff's 0..1 value at ¼, ½ and ¾ of the move is on the line. At the
    fader's middle, half of it. Repeat 1/4 → 1/32 visits every step in order. The `rebuild()` case:
    an end equal to the other end, a start that isn't, still moves.
  - **State and tiles:** arrows and `move`/`play` survive save and load. Old files load unchanged. A
    tile brings its timing; an effect without a move keeps the scene's. In EDIT, turning a moving
    knob makes a plain lock and the `*`.
- `test/fx_test.cpp`: every effect with a move plays it whole at B: finite, no clicks (the step check
  the delay tests use), level within bounds.
- `surface.py` rejects an arrow on a switch, a value out of range, an unknown `move` or `play`.
- `make bench-device`: a case "a move running", with a dozen moving locks (log curves among them) at
  B, next to "looper rolling, fader sweeping".
- `make test-arm`, and `make test-arm-pgo` once its Makefile ordering is fixed (it never runs today).
- By ear on the Force:
  - LP Sweep in B: the sweep starts on the bar line and lasts 8 bars.
  - Build Roll: the roll speeds up.
  - Tape Stop: it winds down over a beat.
  - Back at A, then push again: the move starts over.

## Not in this

- An auto-fader that rides the crossfader from A to B over N bars. A different feature, and MPC can
  already record the fader as track automation.
- Drawing moves on the Force (a START / END switch in EDIT), and recording knob gestures.
- Curves other than the straight line in the knob's space.
- Time signatures other than 4/4.
