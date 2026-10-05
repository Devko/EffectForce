# The probe

Before EffectForce is designed around MPC's insert slots, the **EffectForce Probe** finds out
what MPC actually does with a VST2 effect on the Force. It is a small tempo delay (gain, delay
time, feedback, mix) that records every call MPC makes and logs it.

- [What it answers](#what-it-answers)
- [Where the answers show](#where-the-answers-show)
- [Running the tests on the Force](#running-the-tests-on-the-force)
- [Results](#results)
- [Your part](#your-part)

---

## What it answers

| # | Question | Why it matters for EffectForce |
|---|---|---|
| 1 | Does MPC load it as an insert, and where: track, pad, program, return, master? | Where the rack can live |
| 2 | Do effects get tempo and transport (`audioMasterGetTime`)? | Synced delays, LFOs, gates |
| 3 | **Tails:** does MPC keep calling `process` while the input is silent, after Stop, after a long pause? | Reverb and delay tails cut off or not |
| 4 | Does MPC compensate a reported latency (`initialDelay`)? | Lookahead compression, linear-phase EQ: allowed or zero-latency only |
| 5 | How does MPC bypass an insert: `effSetBypass`, or it just stops calling? | Click-free bypass, tails while bypassed |
| 6 | Is the effect's state (chunk) saved with the project and restored? | Rack presets and recall |
| 7 | Buffers: in place or separate, block sizes, true stereo input? | The rack's buffer handling |
| 8 | Which thread sets parameters, also during automation playback? | Lock-free parameter handling |
| 9 | CPU per instance; several instances | The budget per rack |

## Where the answers show

**On the page** (PROBE), four readouts, refreshed while MPC processes:

| Readout | Shows |
|---|---|
| Status (top) | Input and output peak, CPU, the echo time in ms |
| Host | Sample rate, block size (a range if it varies), `IN PLACE` / `SEPARATE BUFFERS`, `STEREO IN` / `L = R IN` / `NO INPUT`, process calls per second |
| Timing | Tempo, `PLAYING` / `STOPPED`, beat position, time signature, the flags MPC sets |
| Events | How often MPC called bypass (and its state), asked the tail size, suspended, started / stopped processing, saved and loaded the state; the reported latency |

The readouts only change while MPC calls `process`: if they freeze, that is an answer too (the log
says when the calls stopped).

**In the log**, `/tmp/effectforce.log` on the device (`make -C probe probe-log`): every instance's start
and end, MPC's host name, version and canDo answers, every dispatcher call (once for the getters
MPC calls on every redraw), process starting, stopping (no call for 300 ms) and resuming,
`input silent for 1 / 10 / 60 s, process still called`, transport changes, parameter changes and
which thread made them, and a heartbeat every 10 s. Lines start with the wall-clock time and
`#n`, the instance. `make -C probe probe-clear` empties it.

## Running the tests on the Force

The probe lives in `probe/` and builds on its own. Install (stops and restarts MPC: save the
project first):

```bash
make -C probe plugin-install
```

Then, in MPC:

1. **Insert:** on a track with an instrument (PolyForce, SubForce, a drum program), add
   *EffectForce Probe* as an insert effect and open its screen. Play: echoes on the eighth note.
2. **Tempo:** change MPC's tempo; the echo time follows. Press Play and Stop: the Timing readout
   follows.
3. **Tails:** play one short note and let the echoes ring (feedback around 70%). Then the same
   with the sequencer running, pressing Stop while echoes ring. Then leave the track silent for
   at least 15 s and play again.
4. **Bypass:** switch the insert off and on in its slot, also while echoes ring.
5. **Save and reload:** move the knobs, save the project, load another one, load it again.
6. **Other places:** add the probe on a pad's insert, a return or submix, and the master, if MPC
   offers it there.
7. **Automation:** record a Q-Link move of Mix into a sequence and play it back.
8. **Latency** (needs a shell): `make -C probe latency-on`, then add a *new* probe instance as insert on
   one of two tracks playing the same drum pattern (mix 0). Not compensated: the pattern flams by
   100 ms. Compensated: it stays tight. `make -C probe latency-off` afterwards.

Then `make -C probe probe-log` (or send it over) and fill in the results below.

Uninstall: `make -C probe plugin-uninstall` (also restarts MPC).

## Results

Two rounds on 2026-10-05 on the Force (MPC OS 3.x), one instance on a track insert. Open items are
marked *pending*; none of them blocks the design (docs/DESIGN.md).

| # | Question | Answer | Evidence |
|---|---|---|---|
| 1 | Loads as an insert; where | **Yes**, track insert slot 1, with MPC's own header (preset name from `effGetProgramName`, MPC's preset load/save, the slot's ON button). Pads, returns, master: *pending* | Played by hand; screenshot |
| 2 | Tempo and transport | **Yes.** Flags `0x7FC4` stopped, `0x7FC6` playing: tempo, ppq, bars, cycle position, time signature, nanoseconds and SMPTE all valid; cycle active (the loop). The delay follows MPC's tempo | Log, readout, by ear |
| 3 | Tails | **`process` never stops**: 346-347 calls/s (44100 / 128, real time) through 60 s and more of silence, playing or stopped. **But Stop suspends and resumes the plugin** within 100 ms: `effMainsChanged(0)`, `effStopProcess`, then sample rate, block size, `effMainsChanged(1)`, speaker arrangement, `effStartProcess`. A plugin that clears its buffers on resume loses every tail at Stop. Whether the echoes rang on after Stop (the probe doesn't clear): *pending, by ear* | Log 14:57:46 |
| 4 | Latency compensation | Not tested (the flag test wasn't run), and not needed: EffectForce is zero-latency. MPC answers `acceptIOChanges` with no, so a latency could only be reported at creation | Host canDo |
| 5 | Bypass | **The slot's ON button never calls `effSetBypass`** (after `canDo("bypass")` said no): off suspends the plugin (`effMainsChanged(0)`), on resumes it (`effMainsChanged(1)`, `effStartProcess`). The tail stops when it is switched off, and none comes back when it is switched on | Readout after six toggles: BYPASS 0, SUSPEND 17 (was 5), START 9 (was 3), STOP 1; by ear |
| 6 | Saved state | *Pending* (not tried) | Readout SAVE 0 LOAD 0 |
| 7 | Buffers, blocks, stereo | **In place** (in == out), **always 128 frames**, stereo arrangement (2 in, 2 out) accepted; two audio worker threads take turns. The input so far was L = R (the source's output; a stereo source still to try) | Log, readout |
| 8 | Parameter threads | Touch and Q-Links work, from a non-audio thread (MPC's UI side). Automation playback: *pending* | Log, by hand |
| 9 | CPU | 0.50-0.54% of a block for the probe (delay and bookkeeping) | Readout, heartbeat |

Also learned:

- **The host:** `"MPC 3"`, vendor version 257, VST 2400, process level 0 (unknown) when asked from
  the audio thread. canDo yes: `sendVstEvents`, `sendVstMidiEvent`, `sendVstTimeInfo`,
  `receiveVstEvents`, `receiveVstMidiEvent`, `sizeWindow`, `shellCategory`, `supplyIdle`; no:
  `acceptIOChanges`, `reportConnectionChanges`, `offline`, `openFileSelector`, `startStopProcess`,
  `sendVstMidiEventFlagIsRealtime`.
- **Loading an insert:** `effOpen` ×2, `effSetProgram`, sample rate and block size ×4,
  `effMainsChanged` ×3, `effIdentify` ×2, `effConnectInput` / `effConnectOutput` (1), the pin
  properties, `effSetSpeakerArrangement` ×3, `effGetSpeakerArrangement` ×2, canDo `bypass` and
  `receiveVstMidiEvent`, `effStartProcess` ×2.
- **MPC polls:** with the plugin's screen open it calls `effGetEffectName`, `effGetVendorString`,
  `effGetVendorVersion` and `effGetPlugCategory` about 80 times a second each, and the parameter
  getters several hundred times a second (about 600 calls/s in all; about 5/s before the screen
  was opened). Every getter has to stay cheap and lock-free. (The first log filled its 4 MB with
  these in 25 minutes; the probe now logs them once and reports their rates in the heartbeat, and
  the log rotates.)

## Your part

Decided 2026-10-05: both, as the Delay's **Delay Glide** parameter (Tape: the one-pole glide below;
Fade: a crossfade to the new time). The choice as it was put:

`Delay::glideTime()` in [`dsp/delay.h`](../dsp/delay.h) (the probe's own is `StereoDelay::glideTime()`
in `probe/dsp/probe_delay.h`) decides what happens to the echoes when the delay time changes (a new
tempo, another division, automation). Both use a 60 ms one-pole glide as a placeholder. The Delay
keeps whatever is chosen here:

- **One-pole glide** (the placeholder): tape-like, echoes in flight bend in pitch, a big jump
  sweeps far.
- **Rate-limited glide:** the time moves at most so fast (say 5% speed change), so the pitch bend
  is the same size whatever the jump; a big jump takes longer.
- **Crossfade:** a second read head at the new time, faded in over ~50 ms: no pitch bend at all,
  a short wash while both play. Needs a second read and a fade state.

Keep it in `double`: in `float` a time around 22050 samples stalls a few samples short of its
target (the test suite caught exactly that).
