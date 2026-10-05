#pragma once
// Delay (docs/DESIGN.md, "Modules"): the probe's delay (dsp/probe_delay.h) grown up. Two delay
// lines, L and R, each read at its own time and fed back through the cuts, a drive and a limiter;
// the wet ducks under the input.
//
// Time: free (1..2000 ms) or synced (divBeats quarter notes at MPC's tempo), in double precision,
// gliding to new targets (glideTime() below). R's time is L's x (1 + spread), so L stays on the
// grid and spread -0.5..0.5 makes R half as long .. half again as long. A time within a millionth
// of a sample of a whole number is made whole: a 1/16T at 100 BPM is 4410 samples, not
// 4410.000000000001, and reads the line without interpolating. The lines hold 8 s (1 bar at
// 30 BPM) plus the wow's depth; a longer time (R with spread at the slowest tempos) is clamped to
// 8 s, a shorter one to 1 ms (R: 0.5 ms).
//
// Modes. Stereo: each side its own line and feedback. Ping-Pong: the input's mono sum enters L
// only, L's output feeds R and R's feeds L, so the repeats alternate L, R, L... (R's come at L's
// time x (1 + spread) after L's). Mono: both lines get the mono sum, the same time and the same
// wow, so both sides are the same (spread doesn't apply). A mode change crossfades the routing
// over one chunk.
//
// The loop, per side: read -> low-pass -> high-pass -> the wet, and x feedback (+ the input) ->
// drive -> limiter -> write. Repeat n has passed the cuts n times: one-pole filters, 6 dB / octave
// per pass, matched to the analog pole (a = 1 - exp(-2 pi fc / rate)); a high cut of 20 kHz is
// exactly off. The drive is a cubic soft clipper, c - 4/3 c^3 with c = u clamped to +-1/2 (unity
// gain for small signals, flat at +-1/3 from +-1/2 on), blended in by the drive amount: 0 is
// exactly linear. The limiter keeps what is written under 0 dBFS (instant attack, 100 ms release,
// after a clamp at +-8 so a wild input can't hold its gain down for long). No stage has a gain
// over 1, so feedback 1 holds the repeats (the cuts still take their share each pass) and never
// runs away; drive 1 makes them fade slowly as it squares them off.
//
// Wow: a 0.5 Hz wow (+-3 ms at wow 1) and a 6 Hz flutter (+-0.2 ms) move the read times; R's
// wow runs a quarter cycle ahead of L's and its flutter at 6.6 Hz. Under 12 ms the depth shrinks to
// a quarter of the time. Reads are cubic (4-point Hermite): linear interpolation would dull the
// repeats more wherever the wow puts them between samples.
//
// Ducking: an envelope of the input's louder side (5 ms attack, 250 ms release) sets the wet's
// gain to 1 / (1 + 16 duck env): at duck 1, -6 dB with the input at -24 dBFS, -19 dB at -6 dBFS.
// Only the wet ducks, not the loop, so the repeats bloom in the gaps.
//
// reset() is cheap (the rack calls it on the audio thread when the module comes back on): the
// lines aren't cleared. Until they have been written all the way round, each run of samples first
// zeroes the few old samples its taps can reach, so nothing from before ever comes out.
#include "common.h"

#include <vector>

namespace ef {

class Delay {
public:
    enum Mode : int { STEREO, PING_PONG, MONO, kModes };

    struct Params {
        int mode = STEREO;
        bool sync = true;            // time from MPC's tempo
        float timeMs = 375.0f;       // 1..2000, free
        double divBeats = 0.75;      // synced: quarter-note beats (kDelayDivs): 1/8. by default
        float feedback = 0.4f;       // 0..1: 1 holds the repeats
        float spread = 0.0f;         // -0.5..0.5: R's time is L's x (1 + spread)
        float lowCutHz = 100.0f;     // 20..2000: high-pass in the loop (so on the wet too)
        float highCutHz = 8000.0f;   // 500..20000: low-pass in the loop; 20000 = off
        float wow = 0.0f;            // 0..1: wow and flutter
        float drive = 0.0f;          // 0..1: saturation in the loop
        float duck = 0.0f;           // 0..1: the wet ducks under the input
        float mix = 0.3f;            // 0..1 dry / wet
    };

    Delay();   // allocates the lines, 2.8 MB (UI thread)
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return tail_; }

    // The glided delay time of L (side 0) or R (side 1) in samples, without the wow.
    double timeSamples(int side) const { return side ? tR_ : tL_; }

private:
    // The values that move in straight lines across a chunk.
    enum : int { FB, DRIVE, MIX, LP, HP, MONO_IN, CROSS, R_IN, kRamps };

    // How the delay time follows a new target (a tempo or division change, automation): the time
    // one segment (32 samples) on, the samples between in a straight line. The probe's 60 ms
    // one-pole glide, as a tape delay's motor would: echoes already in flight bend in pitch while
    // the time moves, more the further it has to go. The last millionth of a sample snaps, so it
    // lands exactly. In double: in float the step drops under the precision of a time near 22050
    // samples and the glide stalls ~2.6 samples short of its target, for good.
    // TODO(EffectForce): pick the behaviour the real delay keeps (see docs/PROBE.md, "Your part").
    double glideTime(double current, double target) const {
        const double d = target - current;
        return std::fabs(d) < 1e-6 ? target : current + d * glide_;
    }

    void segment();
    void wowNow(float& l, float& r) const;
    template <bool Moving>
    void run(float* L, float* R, int n);

    const double glide_;          // glideTime()'s step per segment
    std::vector<float> lineL_, lineR_;
    int w_ = 0;                   // where the next sample goes, in both lines
    int written_ = 0;             // samples written since reset(), up to the lines' length

    float cur_[kRamps] = {}, tgt_[kRamps] = {}, step_[kRamps] = {};
    float lastLowCut_ = -1.0f, lastHighCut_ = -1.0f;

    double tL_ = 1.0, tR_ = 1.0, tgtL_ = 1.0, tgtR_ = 1.0;   // glided times at the segment's end
    double teL_ = 1.0, teR_ = 1.0;                           // with the wow, this sample
    double teEndL_ = 1.0, teEndR_ = 1.0, teStepL_ = 0.0, teStepR_ = 0.0;
    int segLeft_ = 0;

    float lpL_ = 0.0f, lpR_ = 0.0f, hpL_ = 0.0f, hpR_ = 0.0f;   // the cuts' states
    float gainL_ = 1.0f, gainR_ = 1.0f;                         // the limiter's
    float env_ = 0.0f;                                          // the input's envelope
    float duck_ = 1.0f, duckEnd_ = 1.0f, duckStep_ = 0.0f;      // the wet's gain
    float wowAmt_ = 0.0f, wowTgt_ = 0.0f, duckAmt_ = 0.0f, duckTgt_ = 0.0f, mono_ = 0.0f, monoTgt_ = 0.0f;
    float phWow_ = 0.0f, phFlutL_ = 0.0f, phFlutR_ = 0.0f;      // cycles

    int tail_ = 0;
    float tailFb_ = -1.0f;        // what tail_ was worked out for
    double tailLongest_ = -1.0, repeats_ = 1.0;
    bool fresh_ = true;
};

} // namespace ef
