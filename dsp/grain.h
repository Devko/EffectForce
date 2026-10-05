#pragma once
// Grain (docs/DESIGN.md, "Modules"): granular textures after Hologram's Microcosm. It records the
// last 8 seconds of its input and plays them back as grains and slices, locked to MPC's beat.
//
//   Cloud    Hann-windowed grains of `size`, 1..8 overlapping (density), from random points
//            20..50 ms plus up to spread^2 x 8 s back, at `pitch` with up to +-6 cents x spread
//            of random detune, panned at random up to `spread`, each backwards with probability
//            `reverse`. A haze.
//   Stretch  A play head crawls through the recent past at 1/8 of real time; 2..8 overlapping
//            Hann grains (density) read just behind it (scattered 5 + 60 x spread ms) at `pitch`:
//            time stretched 8x, pitch untouched, a drone. Recording, the head falls behind and
//            starts again from the present once it is 1 + 5 x spread s back; held, it loops
//            through the frozen buffer.
//   Mosaic   Slices of `size` on the beat grid. Each slice period plays one of the last
//            1 + 7 x spread grid-aligned slices, backwards with probability `reverse`, an octave
//            up or down with probability 0.7 x density, panned up to 0.8 x spread; 5 ms
//            crossfades at the edges.
//   Stutter  At each slice boundary, with probability `density`, the slice just played repeats
//            1..1 + 7 x density times (each repeat at `pitch`, backwards with probability
//            `reverse`, sides alternating by spread; as many as the 8 s reach back: fewer for
//            slices over a second); then the live input comes back. Held, it repeats for good the
//            last slice on the grid before the recording stopped. A slice whose end is too young
//            to play through (Hold pressed within ~16 ms of a grid line, or a short slice pitched
//            up, which would catch up with the input) gives way to the nearest earlier one.
//   Arp      A note per slice on the grid: a grain of the step before (up to 1 + spread steps
//            back) at `pitch` plus a step of an interval pattern density picks (octaves; fifth and
//            octave; adding the twelfth; a leaping six-step; up and down over two octaves; steps
//            over +24 fold down an octave), sides alternating by spread. Its step is the slice's
//            number in the song, so the same place always plays the same step. With feedback the
//            steps stack.
//
// Sync: Size is a division of MPC's tempo (halved while over 2 s, doubled while under 10 ms, so it
// stays on the grid). While MPC plays, a slice boundary is where the song position is a whole
// number of slices; a locate or a loop (the song position more than a host block away from where
// it should be, even within one slice: a loop shorter than a slice starts one at every wrap)
// starts a new slice at once, partway through as the grid has it there, crossfaded like any
// other, so the slices follow t.beats without a click. A jump by whole slices (a loop on the grid)
// only renumbers the slice under way. Stopped, the grid runs on at the tempo. Cloud and Stretch
// take their grain length from the tempo but place their grains freely.
//
// The buffer holds 8.5 s of stereo three times over: level 0 at the full rate, levels 1 and 2 at
// a half and a quarter of it, each through the halfband decimator of dsp/halfband.h. A grain
// pitched up more than 4 semitones reads level 1, more than 13.6 level 2, so what it would shift
// past 22 kHz was filtered out first (85 dB down) instead of folding back as aliases. Reads are
// cubic (4-point Hermite) on the level a grain uses. A grain starts only where all its taps hold
// samples written since reset() and not yet overwritten (never those of the chunk being
// rendered: they are written after the grains), for the whole of its planned life; one whose path
// would leave that region (a hold, a tempo change, a slice that runs long) fades out before it
// does.
//
// Level: overlapping grains sum at 1 / sqrt(3/8 overlap) (3/8 is a Hann window's mean square),
// so uncorrelated grains keep a steady input's loudness at any density. Slices and notes play at
// unity. Mix is a straight dry / wet blend.
//
// Feedback: the wet (not Stutter's live input) through a 20 Hz high-pass and a 12 kHz low-pass
// (each pass a little darker), times feedback, into the buffer with the input; a limiter keeps
// what is written under 0 dBFS (instant attack, 100 ms release), so the loop never runs away.
//
// Hold freezes the buffer: nothing is written, the grains play on what is there. Engaging it fades
// the last 5 ms recorded to silence and releasing it fades the recording back in, so the seam
// between old and new material is a dip, never a step. Held slices stay on the recording's grid.
//
// Changes: pitch, size, density, spread and reverse take effect with the next grain or slice, so
// they never click; a mode change fades every grain out over 20 ms while the new mode's fade in.
// reset() is cheap: the buffer isn't cleared, only counted empty, and no grain reaches past what
// was written since. Random choices come from a xorshift that reset() seeds: the same input and
// settings always give the same output.
#include "common.h"
#include "halfband.h"

#include <cstdint>
#include <vector>

namespace ef {

class Grain {
public:
    enum Mode : int { kCloud, kStretch, kMosaic, kStutter, kArp, kModes };

    struct Params {
        int mode = kCloud;        // 0 Cloud, 1 Stretch, 2 Mosaic, 3 Stutter, 4 Arp
        bool sync = true;         // grain / slice length from MPC's tempo
        float sizeMs = 120.0f;    // 10..1000: grain / slice length when free
        double sizeBeats = 0.25;  // when synced: length in quarter-note beats (one of kDelayDivs' values)
        float density = 0.5f;     // 0..1: how many grains overlap (Cloud, Stretch) / how much variation (Mosaic, Stutter, Arp)
        float pitch = 0.0f;       // -24..+24 semitones
        float reverse = 0.0f;     // 0..1: chance a grain / slice plays backwards
        float spread = 0.5f;      // 0..1: how far back grains reach at random, and their stereo spread
        float feedback = 0.0f;    // 0..0.95: the output written back into the buffer (evolving textures); never runs away
        bool hold = false;        // freeze the buffer: stop recording, keep playing what's in it
        float mix = 0.5f;         // 0..1 dry/wet
    };

    static constexpr int kVoices = 16;   // grains sounding at once, at most

    Grain();   // allocates the buffer, 5.3 MB (UI thread)
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return tail_; }

private:
    static constexpr int kLevels = 3;

    // A grain, a slice or a note. Its envelope's ramp a(t) = min(t inSlope, (end - t) outSlope,
    // (relEnd - t) relSlope, 1), clamped to 0..1, makes the gain sin^2(pi / 2 a): a Hann window
    // when it rises for half its length and falls for the other half. A release adds the third
    // term, starting from where the envelope is; one partway into a call keeps the line it
    // replaces for that call (the `prior` ones), which is the lower one before it starts.
    struct Voice {
        double start = 0.0;   // its level's ring position at its own time 0 (frames)
        double rate = 0.0;    // its level's frames per sample (< 0: backwards)
        int level = 0;
        int t = 0;            // its own time at the next call's first sample (< 0: starts later)
        int end = 0;          // its own time when the natural envelope reaches 0
        int relEnd = 0;       // and when a release does
        float inSlope = 0.0f, outSlope = 0.0f, relSlope = 1.0f;
        int priors = 0;       // release lines replaced during this call, still in force before theirs
        int priorEnd[2] = {};
        float priorSlope[2] = {};
        float gd[2] = {}, gx[2] = {};   // L and R: the gains of its own side and of the other one
        bool on = false;
        bool slice = false;   // a Mosaic slice or a Stutter repeat: released at the next boundary
    };

    float uniform();
    double ageAt(double p0, int k) const;
    bool room(double rate, double span, int k, double& lo, double& hi) const;
    bool spawn(int k, double want, double rate, double span, int length, int fadeIn, int fadeOut, float pan, float amp,
               bool slice, bool exact);
    float ramp(const Voice& v, int t) const;
    void release(Voice& v, int k, int fade);
    void releaseAll(int k, int fade, bool slicesOnly);
    void guard(int n);
    int boundary(int n, double& phase);
    void grains(int n);
    void mosaic(int k, double phase);
    void stutter(int k, double phase);
    void arp(int k, double phase);
    void freeze();
    void render(Voice& v, int n);
    template <bool Cross>
    void readVoice(const Voice& v, const float* buf, int k0, int count);
    template <int Level>
    int decimate(StereoDecimator& down, float (&early)[2], const float* in, int first, int count, float* out);

    std::vector<float> buf_[kLevels];   // interleaved L R frames, then kGuard frames again
    Voice voice_[kVoices];
    alignas(16) float acc_[2 * kChunk];   // the wet of this call, L R interleaved
    // A voice's reads, staged: per sample its first float, then its fraction and its envelope, each
    // twice (L and R lanes), so two samples' worth load as one vector.
    alignas(16) int idx_[kChunk + 4];
    alignas(16) float frac_[2 * (kChunk + 4)], env_[2 * (kChunk + 4)];

    int w0_ = -1;          // level 0's newest frame
    int written_ = 0;      // level-0 frames written since reset(), up to what stays readable
    double now_ = 0.0;     // samples since reset() (a whole number: exact in double for millennia)
    int seg_ = 0;          // and that modulo 32: where a call starts in render()'s segments
    uint32_t rng_ = 1;

    // Settings, clamped.
    int mode_ = kCloud;
    float density_ = 0.5f, pitch_ = 0.0f, reverse_ = 0.0f, spread_ = 0.5f;
    bool hold_ = false;
    double slice_ = 5292.0;    // slice / grain length, samples
    double sliceBeats_ = 0.25; // and in beats, synced
    double bpm_ = 120.0, beats_ = 0.0;
    bool locked_ = false;      // the grid follows the song position

    // The grid: the last boundary (samples since reset), the slice the song position is in.
    double lastBoundary_ = 0.0, sliceIndex_ = 0.0;
    double expectBeats_ = 0.0; // locked: where the song position should be at the next call
    bool indexValid_ = false;
    bool enter_ = false;       // a slice mode starts a slice at once (mode change, hold in Stutter)
    bool dropAll_ = false;     // a mode change: every grain fades out

    double nextSpawn_ = 0.0;   // Cloud, Stretch: when the next grain starts
    double head_ = 0.0;        // Stretch: the play head (level-0 ring position) at sample headTime_
    double headTime_ = 0.0;
    double cap_ = 0.0;         // Stutter: where the repeated slice starts (level-0 ring position)
    double holdLag_ = 0.0;     // held: how far past the grid line the recording stopped (samples)
    int left_ = 0, repeat_ = 0;
    bool stuttering_ = false;
    int64_t step_ = 0;         // Arp, free: the pattern's step

    // Stutter's live input: its envelope's ramp, the target before and after sample liveK_.
    float live_ = 0.0f, liveFrom_ = 0.0f, liveTo_ = 0.0f;
    int liveK_ = 0;

    // Mix and feedback: two per-chunk glides, then straight lines across the call.
    float mixGlide_[2] = {}, fbGlide_[2] = {};
    float mix_ = 0.0f, mixEnd_ = 0.0f, fb_ = 0.0f, fbEnd_ = 0.0f;

    // The feedback path's filters and limiter, the recording's fade-in after a hold.
    float lpL_ = 0.0f, lpR_ = 0.0f, hpL_ = 0.0f, hpR_ = 0.0f, limit_ = 1.0f, rec_ = 1.0f;
    StereoDecimator down1_, down2_;
    float early0_[2] = {}, early1_[2] = {};   // the first of a pair waiting for its second

    int tail_ = 0;
    bool fresh_ = true;

    // What the tail and Cloud's speed were worked out for (libm calls: only when these change).
    struct TailKey {
        int mode = -1;
        float pitch = 0.0f, spread = 0.0f, density = 0.0f, fb = 0.0f;
        double slice = 0.0;
        bool hold = false;
        bool operator==(const TailKey& o) const {
            return mode == o.mode && pitch == o.pitch && spread == o.spread && density == o.density && fb == o.fb &&
                   slice == o.slice && hold == o.hold;
        }
    } tailKey_;
    float speedPitch_ = 0.0f;
    double speed_ = 1.0;
};

} // namespace ef
