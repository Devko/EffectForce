#pragma once
// Looper (docs/DESIGN.md, "Performance: scenes and the looper"): the Octatrack's track recorder and
// flex machine in one stage. While armed it records its input all the time; Loop leaving 0 grabs the
// last complete cell of Length on MPC's beat grid and plays it in phase with the grid, crossfaded
// against the live input by Loop. Repeat (or a Length shorter than the loop) rolls a slice of it on
// the grid; Speed plays it slower, faster, stopped or backwards.
//
//   The grab   end = now - (beats mod length) x samples per beat, start = end - length: Length 1 bar
//              is the bar just played, from its downbeat. At song position b the loop plays frame
//              (b mod length) x samples per beat, so it carries on where the grid is. A cell the
//              recording doesn't hold yet (just armed) waits: the grab happens as soon as it does,
//              and until then Loop has nothing to fade in.
//   Slices     Repeat r (or a Length under the grabbed one): the slice floor(pos / r) r the head is
//              in when it changes plays on the grid (+ b mod r); a shorter one re-slices from there,
//              so 1/4 -> 1/8 -> 1/16 rolls in. Off returns to the whole loop on the grid.
//   Speed      the head moves `speed` frames per sample (4-point Hermite), wrapping in the loop or
//              the slice; under |speed| 1/8 the level fades with it, so a tape stop ends in silence,
//              not a held DC level. At speed 1 a head that is off the grid by more than kDrift frames
//              (after another speed, a locate, the sequence's loop point) crossfades back onto it, as
//              long as the loop is still as long as its Length at the current tempo.
//   Jumps      a wrap at the edge, a new slice, a return to the grid: a 5 ms raised-cosine crossfade,
//              the old head playing on past the edge (the loop buffer keeps kGuard frames of what was
//              before and after the cell).
//   Loop       out = live (1 - m) + loop m, m smoothed over 3 ms. Back at 0 the loop is let go; Hold
//              keeps it for the next time instead of grabbing a new one.
//
// Memory: allocate() (UI thread; the plugin calls it the first time the looper is armed) makes a
// 2^20-frame stereo ring (23.8 s) and a 10 s loop buffer and hands them over through an atomic
// pointer; until then the stage passes its input through untouched. The grabbed cell is copied from
// the ring into the loop buffer at 8x real time, after the frames are recorded; a frame not yet
// copied is read from the ring, which still holds it (the oldest frame a grab needs is two lengths
// back, and the ring is 2.38 of the longest loops long). A Length longer than 10 s at the tempo is
// halved until it fits (4 bars from 96 BPM up).
//
// The grid: MPC's song position while it plays; stopped, it runs on at the tempo. reset() forgets
// the recording and the loop (cheap: nothing is cleared, only counted empty).
#include "common.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace ef {

// Loop lengths and repeat slices in quarter-note beats: the options surface.py shows, same order.
inline constexpr Division kLoopLens[] = {{"1/16", 0.25}, {"1/8", 0.5},     {"1/4", 1.0},    {"1/2", 2.0},
                                         {"1 bar", 4.0}, {"2 bars", 8.0}, {"4 bars", 16.0}};
inline constexpr int kNumLoopLens = static_cast<int>(sizeof kLoopLens / sizeof kLoopLens[0]);
inline constexpr Division kLoopReps[] = {{"Off", 0.0}, {"1/2", 2.0},   {"1/4", 1.0},
                                         {"1/8", 0.5}, {"1/16", 0.25}, {"1/32", 0.125}};
inline constexpr int kNumLoopReps = static_cast<int>(sizeof kLoopReps / sizeof kLoopReps[0]);

class Looper {
public:
    struct Params {
        bool on = false;            // armed: the recorder runs
        float loop = 0.0f;          // 0..1: live <-> loop; leaving 0 grabs a loop
        double lengthBeats = 4.0;   // one of kLoopLens' values
        double repeatBeats = 0.0;   // one of kLoopReps' values; 0 = off
        float speed = 1.0f;         // -1..2: playback rate (0 stops, below 0 backwards)
        bool hold = false;          // leaving 0 plays the last loop again
    };

    static constexpr int kRingFrames = 1 << 20;   // 23.8 s
    static constexpr int kMaxLoop = 441000;       // 10 s
    static constexpr int kGuard = 512;            // frames kept before and after the cell
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

    // What it is doing (tests, the status line).
    bool engaged() const { return engaged_; }       // a loop plays (Loop above 0, or fading out)
    bool hasLoop() const { return have_; }
    int loopFrames() const { return len_; }
    double loopBeats() const { return lenBeats_; }
    double head() const { return pos_; }             // loop frame the head is at
    double sliceStart() const { return regStart_; }
    double sliceFrames() const { return regLen_; }
    bool copyDone() const { return copied_ >= copyTotal_; }

private:
    struct Store {
        std::vector<float> ring;   // kRingFrames interleaved L R frames
        std::vector<float> loop;   // the cell with kGuard frames either side, interleaved
    };

    double gridFrames(double periodBeats) const;   // frames since the grid's last line of that period
    bool grab(double lengthBeats);
    void setSlice(double sliceBeats);
    void jumpTo(double target);
    void read(double pos, float& l, float& r) const;
    void frame(int k, float& l, float& r) const;   // loop frame k (-kGuard .. len + kGuard)

    std::atomic<Store*> store_{nullptr};
    Store* st_ = nullptr;   // the audio thread's copy of store_

    // The recording: frames written since allocation; how many of the last ones the ring holds
    // (written_) and how many of those were recorded without a break (recorded_: a grab needs its
    // whole cell in one take).
    int64_t abs_ = 0;
    int64_t written_ = 0, recorded_ = 0;
    bool armed_ = false;

    // The grid at this chunk's first sample.
    double beat_ = 0.0, freeBeat_ = 0.0, bpm_ = 120.0, spb_ = 22050.0;

    // The loop: loop frame k is recorded frame cellAbs_ + k; the loop buffer holds k + kGuard.
    int64_t cellAbs_ = 0;
    int len_ = 0;
    double lenBeats_ = 0.0;
    int copied_ = 0, copyTotal_ = 0;
    bool have_ = false, engaged_ = false;

    // The heads and the region they wrap in.
    double pos_ = 0.0, old_ = 0.0;
    int fade_ = 0;
    double regStart_ = 0.0, regLen_ = 0.0, slice_ = 0.0;   // slice_: the region's length in beats (0: whole loop)

    float mix_ = 0.0f, mixTarget_ = 0.0f;
    float speed_ = 1.0f, speedTarget_ = 1.0f;
    bool fresh_ = true;
};

} // namespace ef
