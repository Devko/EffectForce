#pragma once
// Looper (docs/DESIGN.md, "Performance: scenes and the looper"): the Octatrack's track recorder and
// flex machine in one stage. While armed it records its input all the time. A loop comes two ways:
//
//   REC        Capture Last: the last complete cell of Length on MPC's beat grid, at once.
//              Capture Next: the coming cell, from the grid's next line of Length (a phrase: 4 bars
//              start on a 4-bar line), ready when it ends; a press up to a beat (or a quarter of the
//              cell) late still takes the cell that has just begun, since the recording already
//              has its start. Pressing REC again while waiting cancels. The loop is kept (the surface
//              turns Hold on with REC) and Loop plays it; a new capture while it plays replaces it at
//              once, crossfaded, on the grid.
//   Loop       leaving 0 without a kept loop grabs one as Capture Last would (a fader flick into a
//              roll). Back at 0 a loop that isn't kept is let go.
//
//   The grab   end = now - (beats mod length) x samples per beat, start = end - length: Length 1 bar
//              is the bar just played, from its downbeat. At song position b the loop plays frame
//              (b mod length) x samples per beat, so it carries on where the grid is. A cell the
//              recording doesn't hold yet (just armed) waits.
//   Slices     Repeat r (or a Length under the loop's): the slice floor(pos / r) r the head is in when
//              it changes plays on the grid (+ b mod r); a shorter one re-slices from there, so
//              1/4 -> 1/8 -> 1/16 rolls in. Off returns to the whole loop on the grid.
//   Speed      the head moves `speed` frames per sample (4-point Hermite), wrapping in the loop or
//              the slice; under |speed| 1/8 the level fades with it (a tape stop ends in silence). At
//              speed 1 a head off the grid by more than kDrift frames crossfades back onto it, as long
//              as the loop is still as long as its Length at the current tempo.
//   Jumps      a wrap, a new slice, a return to the grid, a new loop: a 5 ms raised-cosine crossfade,
//              the old head playing on past the edge (the loop buffer keeps kGuard frames either side).
//   Blend      Swap: live (1 - m) + loop m. Layer: live + loop m (the live input stays, the loop on
//              top). m is Loop, smoothed over 3 ms.
//
// Memory: allocate() (UI thread; the plugin calls it the first time the looper is armed) makes a
// 2^21-frame stereo ring (47.5 s) and a 20 s loop buffer (24 MB in all) and hands them over through an
// atomic pointer; until then the stage passes its input through untouched. A loop is copied from the
// ring into the loop buffer at 8x real time; a frame not yet copied is read from the ring, which
// still holds it (the oldest frame a capture needs is two lengths back, the ring 2.38 of the longest
// loops long). A Length longer than 20 s at the tempo is halved until it fits (8 bars from 96 BPM up).
//
// The grid: MPC's song position while it plays; stopped, it runs on at the tempo. Capture Next counts
// samples, not beats, so a sequence looping back meanwhile doesn't lose it. reset() forgets the
// recording and the loop (cheap: nothing is cleared, only counted empty).
#include "common.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace ef {

// Loop lengths and repeat slices in quarter-note beats: the options surface.py shows, same order.
inline constexpr Division kLoopLens[] = {{"1/16", 0.25}, {"1/8", 0.5},     {"1/4", 1.0},     {"1/2", 2.0},
                                         {"1 bar", 4.0}, {"2 bars", 8.0}, {"4 bars", 16.0}, {"8 bars", 32.0}};
inline constexpr int kNumLoopLens = static_cast<int>(sizeof kLoopLens / sizeof kLoopLens[0]);
inline constexpr Division kLoopReps[] = {{"Off", 0.0}, {"1/2", 2.0},   {"1/4", 1.0},
                                         {"1/8", 0.5}, {"1/16", 0.25}, {"1/32", 0.125}};
inline constexpr int kNumLoopReps = static_cast<int>(sizeof kLoopReps / sizeof kLoopReps[0]);

class Looper {
public:
    enum Capture : int { kLast, kNext };

    struct Params {
        bool on = false;            // armed: the recorder runs
        float loop = 0.0f;          // 0..1: how much loop (Swap: and how little live)
        double lengthBeats = 4.0;   // one of kLoopLens' values
        double repeatBeats = 0.0;   // one of kLoopReps' values; 0 = off
        float speed = 1.0f;         // -1..2: playback rate (0 stops, below 0 backwards)
        bool hold = false;          // keep the loop: Loop leaving 0 plays it again instead of grabbing
        int capture = kNext;        // what REC takes: the last cell, or the next one
        uint32_t rec = 0;           // REC presses so far: a change is a press
        bool layer = false;         // Layer: the live input stays at full under the loop
    };

    // What it is doing, for the PERFORM page's line.
    enum State : int { kOff, kListening, kKept, kPlaying };
    struct Status {
        int state = kOff;           // off (not armed), listening (no loop), a loop kept, a loop playing
        double lengthBeats = 0.0;   // the loop's length
        double sliceBeats = 0.0;    // playing: the slice it rolls (0: the whole loop)
        float speed = 1.0f;
        int capture = 0;            // Capture Next: 0 none, 1 waiting for its line, 2 recording its cell
        double captureBeats = 0.0;  // the cell's length
        double captureAt = 0.0;     // beats until it starts (1), or beats of it recorded (2)
    };

    static constexpr int kRingFrames = 1 << 21;   // 47.5 s
    static constexpr int kMaxLoop = 882000;       // 20 s
    // Frames kept before and after the cell: a wrap waits for a crossfade under way to end (one jump at a
    // time: a second one would drop the head still fading out), so the head may run kFade x 2 past an edge
    // and the old head as far again.
    static constexpr int kGuard = 1024;
    static constexpr int kFade = 220;             // a jump's crossfade, 5 ms
    static constexpr int kCopyRate = 8;           // the copy into the loop buffer, x real time
    static constexpr int kDrift = 32;             // frames off the grid before speed 1 snaps back

    Looper() = default;
    ~Looper();
    Looper(const Looper&) = delete;
    Looper& operator=(const Looper&) = delete;

    // The buffers, once (UI thread). False if there is no memory for them: the stage then passes
    // its input through.
    bool allocate();
    bool allocated() const { return store_.load(std::memory_order_acquire) != nullptr; }

    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return 0; }

    Status status() const;
    bool engaged() const { return engaged_; }       // a loop plays (Loop above 0, or fading out)
    bool hasLoop() const { return have_; }
    int loopFrames() const { return cur_.len; }
    double loopBeats() const { return lenBeats_; }
    double head() const { return pos_; }             // loop frame the head is at
    double sliceStart() const { return regStart_; }
    double sliceFrames() const { return regLen_; }
    bool copyDone() const { return cur_.copied >= cur_.total; }
    bool capturing() const { return pendingEnd_ >= 0; }

private:
    struct Store {
        std::vector<float> ring;   // kRingFrames interleaved L R frames
        std::vector<float> loop;   // the cell with kGuard frames either side, interleaved
    };
    // A loop's frames: loop frame k is recorded frame abs + k; the loop buffer holds k + kGuard while
    // k + kGuard < copied (the rest is read from the ring).
    struct Cell {
        int64_t abs = 0;
        int len = 0, copied = 0, total = 0;
    };

    double gridFrames(double periodBeats) const;   // frames since the grid's last line of that period
    double fitLength(double beats) const;          // halved until it fits kMaxLoop
    // A recorded cell becomes the loop, the head at `pos` (frames into it), in the slice of `slice`
    // beats (0: the whole loop); a loop playing crossfades into it.
    bool take(int64_t start, int len, double beats, double pos, double slice);
    bool grab(double lengthBeats, double slice);                // the last complete cell
    void rec(const Params& p, double lengthBeats, double slice);  // a REC press
    void setSlice(double sliceBeats);
    void jumpTo(double target);
    void read(const Cell& c, double pos, float& l, float& r) const;
    void frame(const Cell& c, int k, float& l, float& r) const;

    std::atomic<Store*> store_{nullptr};
    Store* st_ = nullptr;   // the audio thread's copy of store_

    // The recording: frames written since allocation; how many of the last ones the ring holds
    // (written_) and how many of those were recorded without a break (recorded_: a capture needs its
    // whole cell in one take).
    int64_t abs_ = 0;
    int64_t written_ = 0, recorded_ = 0;
    bool armed_ = false;

    // The grid at this chunk's first sample.
    double beat_ = 0.0, freeBeat_ = 0.0, bpm_ = 120.0, spb_ = 22050.0;

    // The loop, and the one the old head reads while a new loop fades in (prevFade_ > 0).
    Cell cur_, prev_;
    double lenBeats_ = 0.0;
    bool have_ = false, engaged_ = false;
    bool oldPrev_ = false;   // the old head reads prev_ (a new loop fading in), not cur_

    // REC: the presses seen; a Capture Next waiting for its cell to end (in recorded frames).
    uint32_t recSeen_ = 0;
    bool recFresh_ = true;
    int64_t pendingEnd_ = -1;
    int pendingLen_ = 0;
    double pendingBeats_ = 0.0;

    // The heads and the region they wrap in.
    double pos_ = 0.0, old_ = 0.0;
    int fade_ = 0;
    double regStart_ = 0.0, regLen_ = 0.0, slice_ = 0.0;   // slice_: the region's length in beats (0: whole loop)

    float mix_ = 0.0f, mixTarget_ = 0.0f;
    float speed_ = 1.0f, speedTarget_ = 1.0f;
    bool layer_ = false, hold_ = false;
    float layerMix_ = 0.0f;   // Swap (0) to Layer (1), glided like the mix: a switch doesn't step the live input
    bool fresh_ = true;
};

} // namespace ef
