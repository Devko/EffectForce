#pragma once
// Phaser and flanger (docs/DESIGN.md, "Modules") on one LFO.
//
//   Phaser 4 / 8 / 12  A cascade of first-order allpasses; their break frequency sweeps
//                      exponentially around the centre (50 Hz..8 kHz, log), up to +-3 octaves at
//                      depth 1, held within 20 Hz..18.5 kHz. Sine LFO; the coefficients follow it
//                      sample by sample.
//   Flanger            A short delay per side (cubic interpolation), swept exponentially around the
//                      base delay (0.2..10 ms, log), up to +-2 octaves at depth 1, never under two
//                      samples. Triangle LFO.
//
// Feedback: the phaser's last stage into its first one sample later, the flanger's delayed signal
// into its line; both through a gentle saturator (1% compression at full scale, a ceiling of 4), so
// the loop's level stays bounded whatever the modulation does to its gain. Mix 0.5: dry and wet
// equal, the classic notches.
//
// Sync: while MPC plays, each chunk takes the LFO's phase from the song position (lockedPhase), so
// it stays on the beat through loops and jumps; stopped, or free, it runs on at its rate (with
// sync, the division's at MPC's tempo). The right side's LFO runs `stereo` degrees later.
// A mode change fades the wet out over 8 chunks, switches (the allpasses from rest; the flanger's
// lines, fed in every mode, hold the input's recent past), runs the new path unheard for 4 chunks
// with its feedback gliding in from 0, and fades back in.
#include "common.h"

#include <cstdint>
#include <vector>

namespace ef {

class Phaser {
public:
    enum Mode { kPhaser4, kPhaser8, kPhaser12, kFlanger };

    struct Params {
        int mode = kPhaser4;       // 0 Phaser 4, 1 Phaser 8, 2 Phaser 12, 3 Flanger
        bool sync = false;         // LFO synced to MPC's tempo
        float rateHz = 0.3f;       // 0.01..20 when free
        double divBeats = 4.0;     // when synced: the LFO period in quarter-note beats (kLfoDivs)
        float depth = 0.7f;        // 0..1
        float center = 0.5f;       // 0..1: phaser centre 50 Hz..8 kHz (log); flanger base delay 0.2..10 ms (log)
        float feedback = 0.5f;     // -0.95..0.95
        float stereo = 90.0f;      // 0..180 degrees of LFO phase between left and right
        float mix = 0.5f;          // 0..1 dry / wet (0.5 = classic notches)
    };

    static constexpr float kPhaserOctaves = 3.0f, kFlangerOctaves = 2.0f;   // the sweep at depth 1, +-
    static constexpr float kCenterLowHz = 50.0f, kCenterHighHz = 8000.0f;
    static constexpr float kSweepLowHz = 20.0f, kSweepHighHz = 18500.0f;   // where a phaser sweep stops
    static constexpr float kDelayLowMs = 0.2f, kDelayHighMs = 10.0f;
    static constexpr float kFlangerMin = 2.0f, kFlangerMax = 1800.0f;     // where a flanger sweep stops, samples
    static constexpr int kMaxStages = 12;

    Phaser();
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const;

private:
    struct Lin {   // a value moving in a straight line to its target over a chunk
        float cur = 0.0f, step = 0.0f, target = 0.0f;
    };

    static constexpr int kLine = 2048, kStride = kLine + 4;   // a flanger line, then its first three samples again

    template <int N>
    void runPhaser(float* L, float* R, int n);
    void runFlanger(float* L, float* R, int n);
    void sweep(int n);
    void clearPaths();

    std::vector<float> line_;          // the flanger's: left line, then right
    int w_ = 0;
    float ap_[kMaxStages][2] = {};     // allpass states, [stage][side]
    float yl_ = 0.0f, yr_ = 0.0f;      // the phaser's last output per side: its feedback

    // Per sample of the chunk: L and R allpass coefficient (or delay in samples), and feedback, mix.
    alignas(16) float ctl_[2 * kChunk] = {};
    alignas(16) float fbMix_[2 * kChunk] = {};

    // LFO phase in 32-bit fixed point: exact, so blocks of any size see the same sweep.
    uint32_t acc_ = 0, inc_ = 0;
    // The sweep's centre and span (log2 of the coefficient's frequency or of the delay), the right
    // side's phase offset (cycles), feedback and mix: each glides per chunk first, then follows in
    // a straight line across the chunk.
    Lin cen_, span_, off_, fb_, mix_;
    float cenGlide_ = 0.0f, spanGlide_ = 0.0f, offGlide_ = 0.0f, fbGlide_[2] = {}, mixGlide_[2] = {};
    float gate_ = 1.0f;   // the wet's share while a mode change fades it out and back in (< 0: not yet)

    int left_ = 0;   // samples until the ramps reach their targets
    int tail_ = 0;
    int mode_ = kPhaser4;
    bool fresh_ = true;
};

} // namespace ef
