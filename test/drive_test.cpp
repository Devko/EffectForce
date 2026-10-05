// Drive (dsp/drive.h): the halfband pair, the shapers against their definitions, aliasing,
// harmonics and bias, the level compensation, tone, Crush, mix, and what every module owes
// (extremes, random jumps, NaN input, block sizes, reset, tail, no clicks).
#include "signal.h"
#include "../dsp/drive.h"

#include <cstdio>

namespace {

using namespace eft;
using namespace ef;
using P = Drive::Params;

constexpr float kRefAmp = 0.25f;   // -12 dBFS

P params(int type, float driveDb, float tone = 0.0f, float bias = 0.0f, float outDb = 0.0f, float mix = 1.0f) {
    P p;
    p.type = type;
    p.driveDb = driveDb;
    p.tone = tone;
    p.bias = bias;
    p.outDb = outDb;
    p.mix = mix;
    return p;
}

// A fresh Drive over `in` on both channels, constant params; the left output.
Buf render(const P& p, const Buf& in, int chunk = kChunk) {
    Drive d;
    Buf L = in, R = in;
    run(d, p, L, R, {}, chunk);
    return L;
}

bool same(const Buf& a, const Buf& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

float maxDiff(const Buf& a, const Buf& b) {
    float m = 0.0f;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

// The least-squares fit of DC + a sine at hz over x[from, to), for a window of whole cycles
// (there sine, cosine and DC are orthogonal): returns the residual's RMS, and amplitude / phase.
struct Fit {
    double amp, phase, residual;
};
Fit fitSine(const Buf& x, double hz, size_t from, size_t to) {
    double s = 0.0, c = 0.0, m = 0.0;
    const double n = static_cast<double>(to - from);
    for (size_t i = from; i < to; ++i) {
        const double w = 2.0 * eft::kPi * hz * static_cast<double>(i) / kRate;
        s += x[i] * std::sin(w);
        c += x[i] * std::cos(w);
        m += x[i];
    }
    s *= 2.0 / n;
    c *= 2.0 / n;
    m /= n;
    double r = 0.0;
    for (size_t i = from; i < to; ++i) {
        const double w = 2.0 * eft::kPi * hz * static_cast<double>(i) / kRate;
        const double e = x[i] - (m + s * std::sin(w) + c * std::cos(w));
        r += e * e;
    }
    return {std::sqrt(s * s + c * c), std::atan2(c, s), std::sqrt(r / n)};
}

double mean(const Buf& x, size_t from, size_t to) {
    double s = 0.0;
    for (size_t i = from; i < to; ++i) s += x[i];
    return s / static_cast<double>(to - from);
}

// --- the halfband pair ---------------------------------------------------------------------

void halfband() {
    // A sine through the interpolator: at 88.2 kHz the sine at gain 1, and its image at
    // 44.1 kHz - f far down. magnitude() assumes 44.1 kHz, so at the high rate every frequency
    // is given as half.
    const int n = 20480;
    double pass = 0.0, image = -300.0;
    for (double hz : {100.0, 1000.0, 5000.0, 10000.0, 15000.0, 19000.0, 20000.0}) {
        Interpolator up;
        const Buf x = sine(hz, n, 0.5f);
        Buf hi(static_cast<size_t>(2 * n));
        for (int i = 0; i < n; ++i) up.process(x[static_cast<size_t>(i)], hi[static_cast<size_t>(2 * i)], hi[static_cast<size_t>(2 * i + 1)]);
        pass = std::max(pass, std::fabs(db(magnitude(hi, hz / 2.0, 8192) / 0.5)));
        image = std::max(image, db(magnitude(hi, (kRate - hz) / 2.0, 8192) / 0.5));
    }
    std::printf("  interpolator: passband to 20 kHz within %.4f dB, images at %.1f dB\n", pass, image);
    CHECK(pass < 0.01);
    CHECK(image < -80.0);

    // Up and down again: flat, and the documented delay (2.7 samples at low frequencies).
    double flat = 0.0;
    for (double hz : {441.0, 4410.0, 11025.0, 19845.0}) {
        Interpolator up;
        Decimator down;
        const Buf x = sine(hz, n, 0.5f);
        Buf y(x.size());
        for (size_t i = 0; i < x.size(); ++i) {
            float e, l;
            up.process(x[i], e, l);
            y[i] = down.process(e, l);
        }
        const Fit fx = fitSine(x, hz, 4000, 4000 + 16000), fy = fitSine(y, hz, 4000, 4000 + 16000);
        flat = std::max(flat, std::fabs(db(fy.amp / fx.amp)));
        if (hz == 441.0) {
            double dp = fx.phase - fy.phase;
            while (dp < 0.0) dp += 2.0 * eft::kPi;
            const double delay = dp / (2.0 * eft::kPi * hz / kRate);
            CHECK(delay > 2.6 && delay < 2.8);
        }
    }
    CHECK(flat < 0.01);

    // The stereo versions (what Drive runs) are the scalar ones, lane by lane.
    Interpolator upL, upR;
    Decimator downL, downR;
    StereoInterpolator up2;
    StereoDecimator down2;
    const Buf a = whiteNoise(4096, 0.8f, 61), b = sine(3000.0, 4096, 0.6f);
    float lanes = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        float h1[4], h2[4], y1[2], y2[2];
        upL.process(a[i], h1[0], h1[1]);
        upR.process(b[i], h1[2], h1[3]);
        store4(h2, up2.process(a[i], b[i]));
        y1[0] = downL.process(h1[0], h1[1]);
        y1[1] = downR.process(h1[2], h1[3]);
        down2.process(load4(h2), y2[0], y2[1]);
        for (int j = 0; j < 4; ++j) lanes = std::max(lanes, std::fabs(h1[j] - h2[j]));
        for (int j = 0; j < 2; ++j) lanes = std::max(lanes, std::fabs(y1[j] - y2[j]));
    }
    CHECK(lanes < 1e-6f);
}

// --- the shapers -----------------------------------------------------------------------------

float shaper(int t, float u) { return drv::shape(t, u); }

void shapers() {
    // Slope 1 at the origin: quiet signals at 0 dB drive pass unchanged.
    for (int t = 0; t < 5; ++t) CHECK(std::fabs((shaper(t, 1e-3f) - shaper(t, -1e-3f)) / 2e-3f - 1.0f) < 2e-3f);

    // Continuous: no step anywhere in -8..8 bigger than the steepest slope allows (Tube's 1.5).
    for (int t = 0; t < 5; ++t) {
        float worst = 0.0f;
        for (float u = -8.0f; u < 8.0f; u += 1e-3f) worst = std::max(worst, std::fabs(shaper(t, u + 1e-3f) - shaper(t, u)) / 1e-3f);
        CHECK(worst < (t == Drive::Tube ? 1.52f : 1.02f));
    }

    // Finite and bounded for any input.
    for (float u : {1e3f, 65536.0f, 1e6f, 1e30f, 3.4e38f, INFINITY})
        for (float s : {1.0f, -1.0f})
            for (int t = 0; t < 5; ++t) {
                const float y = shaper(t, s * u);
                CHECK(std::isfinite(y) && std::fabs(y) <= 1.25f);
            }

    // Odd symmetry (no even harmonics without bias), except Tube.
    float odd = 0.0f;
    for (int t : {Drive::Soft, Drive::Hard, Drive::Fold, Drive::Sine})
        for (float u = 0.0f; u < 10.0f; u += 0.01f) odd = std::max(odd, std::fabs(shaper(t, u) + shaper(t, -u)));
    CHECK(odd < 1e-6f);

    // Each against its definition.
    float soft = 0.0f;   // slope (1 - (u / edge)^2)^5, flat at 1 from the edge on
    for (float u = -2.6f; u < 2.6f; u += 0.05f) {
        const float slope = (drv::soft(u + 1e-3f) - drv::soft(u - 1e-3f)) / 2e-3f;
        soft = std::max(soft, std::fabs(slope - std::pow(1.0f - (u / drv::kSoftEdge) * (u / drv::kSoftEdge), 5.0f)));
    }
    CHECK(soft < 2e-3f);
    CHECK(std::fabs(drv::soft(drv::kSoftEdge) - 1.0f) < 1e-6f && drv::soft(4.0f) == drv::soft(drv::kSoftEdge));
    CHECK(std::fabs(drv::tube(10.0f) - 1.25f) < 1e-6f && std::fabs(drv::tube(-10.0f) + 0.75f) < 1e-6f);
    CHECK(drv::hard(0.8f) == 0.8f && drv::hard(-0.5f) == -0.5f && drv::hard(1.2f) == 1.0f && drv::hard(-7.0f) == -1.0f);
    CHECK(std::fabs(drv::hard(1.0f) - 0.95f) < 1e-6f);   // halfway into the knee: 1 - 1.25 * 0.2^2
    CHECK(drv::fold(0.7f) == 0.7f && drv::fold(-1.0f) == -1.0f);
    CHECK(std::fabs(drv::fold(2.0f)) < 1e-6f && std::fabs(drv::fold(3.0f) + 1.0f) < 1e-6f && std::fabs(drv::fold(5.5f) - 0.5f) < 1e-6f);
    float sine = 0.0f;
    for (float u = -20.0f; u < 20.0f; u += 0.01f) sine = std::max(sine, std::fabs(drv::sine(u) - std::sin(u)));
    CHECK(sine < 1e-5f);
}

// --- aliasing, harmonics, bias ---------------------------------------------------------------

// Where the harmonics of f0 that the 2x path can't hold land in the output: folded at 88.2 kHz,
// then (what the decimator lets through above 22.05 kHz) at 44.1 kHz. Harmonics themselves and
// DC are left out.
std::vector<double> aliases(double f0) {
    std::vector<double> out;
    for (int k = 1; k <= 80; ++k) {
        double f = std::fmod(k * f0, 2.0 * kRate);
        if (f > kRate) f = 2.0 * kRate - f;
        if (f > kRate / 2.0) f = kRate - f;
        const double r = std::fmod(f, f0);
        if (f < 50.0 || std::min(r, f0 - r) < 50.0) continue;
        out.push_back(f);
    }
    return out;
}

double worstAliasDb(const Buf& y, double f0, size_t from) {
    const double fund = magnitude(y, f0, from);
    double worst = 0.0;
    for (double f : aliases(f0)) worst = std::max(worst, magnitude(y, f, from));
    return db(worst / fund);
}

void aliasing() {
    const Buf x = sine(5000.0, 4096 + 16384, kRefAmp);
    const double soft = worstAliasDb(render(params(Drive::Soft, 24.0f), x), 5000.0, 4096);
    const double tube = worstAliasDb(render(params(Drive::Tube, 24.0f), x), 5000.0, 4096);
    const double hard = worstAliasDb(render(params(Drive::Hard, 24.0f), x), 5000.0, 4096);
    std::printf("  5 kHz at -12 dBFS, drive 24 dB, strongest alias: Soft %.1f dB, Tube %.1f, Hard %.1f\n", soft, tube, hard);
    // At this level Soft's curve is still smooth enough; 1 dB hotter it's 63 dB, 2 dB 52: past
    // that every curve clips flat and 2x can't hold the harmonics (drive.h).
    CHECK(soft < -60.0);
    // And the measurement itself: at 0 dB drive Soft is all but linear, so no alias shows.
    CHECK(worstAliasDb(render(params(Drive::Soft, 0.0f), x), 5000.0, 4096) < -90.0);
}

double harmonicDb(const P& p, int h, double f0 = 1000.0) {
    const Buf y = render(p, sine(f0, 8192 + 16384, kRefAmp));
    return db(magnitude(y, h * f0, 8192) / magnitude(y, f0, 8192));
}

void harmonics() {
    // Without bias the symmetric shapers make no even harmonics at all.
    for (int t : {Drive::Soft, Drive::Hard, Drive::Fold, Drive::Sine}) CHECK(harmonicDb(params(t, 12.0f), 2) < -80.0);
    CHECK(harmonicDb(params(Drive::Soft, 36.0f), 2) < -80.0);
    // Tube has them anyway, and the odd ones grow with drive.
    CHECK(harmonicDb(params(Drive::Tube, 0.0f), 2) > -40.0);
    CHECK(harmonicDb(params(Drive::Soft, 24.0f), 3) > harmonicDb(params(Drive::Soft, 6.0f), 3) + 6.0);

    // Bias 0.5 brings the 2nd harmonic up at every drive where the curve bends (Hard and Fold
    // are straight below 0.8 and 1: a -12 dBFS sine reaches their bends from drive 12 dB on).
    double lowest = 0.0;
    for (int t : {Drive::Soft, Drive::Tube, Drive::Hard, Drive::Fold, Drive::Sine})
        for (float d : {12.0f, 24.0f, 36.0f}) {
            const double h2 = harmonicDb(params(t, d, 0.0f, 0.5f), 2);
            lowest = std::min(lowest, h2);
            CHECK(h2 > -40.0);
        }
    CHECK(harmonicDb(params(Drive::Soft, 0.0f, 0.0f, 0.5f), 2) > -40.0);   // even at 0 dB, where Soft bends
    std::printf("  bias 0.5: 2nd harmonic at least %.1f dB (drive 12..36 dB, every shaper)\n", lowest);

    // The DC blocker leaves no DC, however asymmetric (441 Hz: whole cycles in the window).
    for (int t : {Drive::Soft, Drive::Tube, Drive::Hard, Drive::Fold, Drive::Crush}) {
        const Buf y = render(params(t, 24.0f, 0.0f, 1.0f), sine(441.0, 44000, kRefAmp));
        CHECK(std::fabs(mean(y, 22000, 44000)) < 1e-3 * rms(y, 22000, 44000));
    }
}

// --- level compensation ----------------------------------------------------------------------

void levels() {
    const size_t settle = 11025;
    const Buf x = sine(1000.0, static_cast<int>(settle) + 16384, kRefAmp);
    const double inRms = rms(x, settle), inFund = magnitude(x, 1000.0, settle);
    double worstRms = 0.0, worstFund = 0.0, worstOther = 0.0;
    for (int t = 0; t < Drive::kNumTypes; ++t)
        for (float bias : {0.0f, 0.5f})
            for (float d = 0.0f; d <= 36.0f; d += 3.0f) {
                const Buf y = render(params(t, d, 0.0f, bias), x);
                const double r = db(rms(y, settle) / inRms), f = db(magnitude(y, 1000.0, settle) / inFund);
                if (t <= Drive::Hard) {
                    worstRms = std::max(worstRms, std::fabs(r));
                    worstFund = std::max(worstFund, std::fabs(f));
                    CHECK(std::fabs(r) < 3.0 && std::fabs(f) < 3.0);
                } else {
                    worstOther = std::max(worstOther, std::fabs(r));
                    CHECK(std::fabs(r) < 6.0);
                }
            }
    std::printf("  -12 dBFS sine, drive 0..36 dB: Soft/Tube/Hard RMS within %.2f dB, fundamental %.2f dB; "
                "Fold/Sine/Crush RMS %.2f dB\n", worstRms, worstFund, worstOther);
    // Out trims on top.
    const Buf y = render(params(Drive::Soft, 12.0f, 0.0f, 0.0f, -12.0f), x);
    const Buf y0 = render(params(Drive::Soft, 12.0f), x);
    CHECK(std::fabs(db(rms(y, settle) / rms(y0, settle)) + 12.0) < 0.05);
}

// --- tone ------------------------------------------------------------------------------------

void tone() {
    Drive d;
    auto tilt = [&](float tone) {
        const P p = params(Drive::Soft, 0.0f, tone);
        return gainAt(d, p, 8000.0, 8192, 16384) - gainAt(d, p, 100.0, 8192, 16384);
    };
    const double bright = tilt(1.0f), dark = tilt(-1.0f), flat = tilt(0.0f);
    std::printf("  tilt 8 kHz vs 100 Hz: %+.2f dB at +1, %+.2f at -1, %+.3f at 0\n", bright, dark, flat);
    CHECK(bright > 10.5 && bright < 12.5);
    CHECK(dark < -10.5 && dark > -12.5);
    CHECK(std::fabs(flat) < 0.08);   // all of it the DC blocker's 0.04 dB at 100 Hz
    // Half way: about half the tilt (6 dB per side scale in dB with tone).
    const double half = tilt(0.5f);
    CHECK(half > 5.0 && half < 6.5);
}

// --- Crush -----------------------------------------------------------------------------------

void crush() {
    // A 441 Hz sine (100 samples a cycle): at 0 dB 16 bits and the full rate (the noise of 16
    // bits under a -12 dBFS sine: 86 dB), at 36 dB 4 bits and 4 kHz.
    const Buf x = sine(441.0, 8000 + 16000, kRefAmp);
    auto snr = [&](float d) {
        const Fit f = fitSine(render(params(Drive::Crush, d), x), 441.0, 8000, 8000 + 16000);
        return db(f.amp / std::sqrt(2.0) / f.residual);
    };
    const double clean = snr(0.0f), mid = snr(18.0f), crushed = snr(36.0f);
    std::printf("  Crush SNR of a -12 dBFS sine: %.1f dB at drive 0, %.1f at 18, %.1f at 36\n", clean, mid, crushed);
    CHECK(clean > 75.0);
    CHECK(crushed < 20.0);
    CHECK(clean > mid && mid > crushed);

    // 4 bits: a triangle at +-0.3 lands on five levels, the multiples of 1/8 (the DC blocker
    // bends them slightly); at 16 bits anywhere.
    Buf tri(44100);
    for (size_t i = 0; i < tri.size(); ++i) {
        const double ph = std::fmod(200.0 * static_cast<double>(i) / kRate, 1.0);
        tri[i] = static_cast<float>(0.3 * (ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph));
    }
    auto levels = [](Buf y, float tol) {
        std::sort(y.begin(), y.end());
        int n = 1;
        for (size_t i = 1; i < y.size(); ++i) n += y[i] - y[i - 1] > tol ? 1 : 0;
        return n;
    };
    auto onGrid = [](const Buf& y) {   // the share of samples within 0.02 of a multiple of 1/8
        int n = 0;
        for (float v : y) n += std::fabs(v * 8.0f - std::round(v * 8.0f)) < 0.16f ? 1 : 0;
        return static_cast<double>(n) / static_cast<double>(y.size());
    };
    const Buf full4 = render(params(Drive::Crush, 36.0f), tri), full16 = render(params(Drive::Crush, 0.0f), tri);
    const Buf y4(full4.begin() + 4410, full4.end()), y16(full16.begin() + 4410, full16.end());
    const int few = levels(y4, 0.03f);
    const double grid4 = onGrid(y4), grid16 = onGrid(y16);
    std::printf("  Crush: a +-0.3 triangle takes %d levels at drive 36 dB; on the 1/8 grid: %.0f%%, at 0 dB %.0f%%\n",
                few, 100.0 * grid4, 100.0 * grid16);
    CHECK(few == 5);
    CHECK(grid4 > 0.99 && grid16 < 0.5);

    // The sample-and-hold: noise changes level at most at the hold rate (4 kHz at 36 dB), at
    // nearly every sample at 0 dB.
    const Buf noise = whiteNoise(44100, 0.5f, 7);
    auto changes = [&](float d) {
        const Buf y = render(params(Drive::Crush, d), noise);
        int n = 0;
        for (size_t i = 1; i < y.size(); ++i) n += std::fabs(y[i] - y[i - 1]) > 0.01f ? 1 : 0;
        return n;
    };
    const int slow = changes(36.0f), fast = changes(0.0f);
    CHECK(slow > 2500 && slow <= 4100);
    CHECK(fast > 40000);

    // Bias moves the grid by up to half a step: at 1, a quiet sine (under half a step) buzzes
    // instead of gating.
    const Buf quiet = sine(441.0, 8000 + 16000, 0.05f);
    CHECK(rms(render(params(Drive::Crush, 36.0f, 0.0f, 0.0f), quiet), 8000) < 1e-6);
    CHECK(rms(render(params(Drive::Crush, 36.0f, 0.0f, 1.0f), quiet), 8000) > 0.03);
}

// --- mix -------------------------------------------------------------------------------------

void mix() {
    // Mix 0: the input back bit for bit, whatever else is set (and after a ramp down to 0).
    const Buf noise = whiteNoise(8192, 0.9f, 3);
    for (int t = 0; t < Drive::kNumTypes; ++t) CHECK(same(render(params(t, 36.0f, 1.0f, 1.0f, 12.0f, 0.0f), noise), noise));
    Drive d;
    Buf L = noise, R = noise;
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        d.set(params(Drive::Fold, 30.0f, -1.0f, 0.3f, 6.0f, pos < 4096 ? 1.0f : 0.0f), {});
        d.process(&L[pos], &R[pos], kChunk);
    }
    CHECK(same(Buf(L.begin() + 4096 + kChunk, L.end()), Buf(noise.begin() + 4096 + kChunk, noise.end())));

    // Partial mixes don't comb: the dry runs through halfbands like the wet's, so quiet tones (where
    // the shaper is all but straight) come out at one level from 1 to 18 kHz. A raw dry against the
    // wet's 2.7 samples of delay would notch 42 dB at 7.7 kHz. Crush (not oversampled, here at 0 dB:
    // 16 bits, full rate) blends with the raw dry.
    const P partial[] = {params(Drive::Soft, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f), params(Drive::Soft, 24.0f, 0.0f, 0.0f, 0.0f, 0.5f),
                         params(Drive::Soft, 24.0f, 0.0f, 0.0f, 0.0f, 0.8f), params(Drive::Tube, 24.0f, 0.0f, 0.0f, 0.0f, 0.5f),
                         params(Drive::Crush, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f)};
    double spread = 0.0;
    for (const P& p : partial) {
        double lo = 1e9, hi = -1e9;
        for (double hz : {1000.0, 2000.0, 4000.0, 6000.0, 7700.0, 10000.0, 12000.0, 14000.0, 16000.0, 18000.0}) {
            const Buf x = sine(hz, 4096 + 8192, 0.01f);   // -40 dBFS
            const double g = db(magnitude(render(p, x), hz, 4096) / magnitude(x, hz, 4096));
            lo = std::min(lo, g);
            hi = std::max(hi, g);
        }
        spread = std::max(spread, hi - lo);
        CHECK(hi - lo < 1.0);
    }
    std::printf("  partial mixes, quiet tones 1..18 kHz: level within %.2f dB\n", spread);

    // Mix 0 -> 0.3 -> 0: the dry crosses between raw and filtered over a chunk, no click (a
    // switch would jump up to 0.27 on this 3 kHz sine, against its own 0.11 a sample). At four
    // phases, so neither switch can land where raw and filtered happen to agree.
    for (double phase : {0.0, 0.8, 1.6, 2.4}) {
        const Buf s = sine(3000.0, 32 * 300, kRefAmp, phase);
        Buf A = s, B = s;
        Drive m;
        auto mixAt = [](size_t chunk) { return chunk < 100 ? 0.0f : chunk < 200 ? 0.3f : 0.0f; };
        for (size_t pos = 0; pos < A.size(); pos += kChunk) {
            m.set(params(Drive::Soft, 0.0f, 0.0f, 0.0f, 0.0f, mixAt(pos / kChunk)), {});
            m.process(&A[pos], &B[pos], kChunk);
        }
        const float steady = std::max(maxStep(A, 32 * 50, 32 * 99), maxStep(A, 32 * 150, 32 * 199));
        CHECK(maxStep(A, 32 * 99, 32 * 102) < 1.05f * steady);
        CHECK(maxStep(A, 32 * 199, 32 * 202) < 1.05f * steady);
        CHECK(same(Buf(A.begin() + 32 * 201, A.end()), Buf(s.begin() + 32 * 201, s.end())));   // and exact again
    }

    // Out of mix 1, where the dry halfbands rest: re-run on the remembered input, they pick up as
    // if they had run all along (against a mix of 0.999, where they do).
    const Buf noise2 = whiteNoise(32 * 200, 0.5f, 71);
    auto resume = [&](float before) {
        Drive r;
        Buf l = noise2, rr = noise2;
        for (size_t pos = 0; pos < l.size(); pos += kChunk) {
            r.set(params(Drive::Soft, 12.0f, 0.0f, 0.0f, 0.0f, pos < 32 * 100 ? before : 0.5f), {});
            r.process(&l[pos], &rr[pos], kChunk);
        }
        return Buf(l.begin() + 32 * 101, l.end());
    };
    CHECK(maxDiff(resume(1.0f), resume(0.999f)) < 1e-4f);
}

// --- what every module owes ------------------------------------------------------------------

void extremes() {
    // Loud noise at every corner, a second each (four for the hardest): finite and bounded.
    const Buf noise = whiteNoise(44100, 1.0f, 11);
    float worst = 0.0f;
    bool finite = true;
    for (int t = 0; t < Drive::kNumTypes; ++t)
        for (float d : {0.0f, 36.0f})
            for (float bias : {0.0f, 1.0f})
                for (float tone : {-1.0f, 1.0f}) {
                    const Buf y = render(params(t, d, tone, bias, 12.0f), noise);
                    finite = finite && allFinite(y);
                    worst = std::max(worst, peak(y));
                }
    const Buf longNoise = whiteNoise(4 * 44100, 1.0f, 12);
    const Buf y = render(params(Drive::Tube, 36.0f, 1.0f, 1.0f, 12.0f), longNoise);
    finite = finite && allFinite(y);
    worst = std::max(worst, peak(y));
    std::printf("  loud noise at the extremes (Out +12 dB): peak %.1f\n", worst);
    CHECK(finite);
    CHECK(worst < 64.0f);
}

void randomJumps() {
    // New parameters every chunk, in range and far out of it (NaN too), for 20 seconds.
    Drive d;
    uint32_t s = 99;
    auto r = [&](float lo, float hi) { return lo + (hi - lo) * 0.5f * (randBipolar(s) + 1.0f); };
    const Buf noise = whiteNoise(20 * 44100, 1.0f, 5);
    Buf L = noise, R = whiteNoise(20 * 44100, 1.0f, 6);
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        P p = params(static_cast<int>(r(0.0f, 6.0f)), r(0.0f, 36.0f), r(-1.0f, 1.0f), r(0.0f, 1.0f), r(-24.0f, 12.0f), r(0.0f, 1.0f));
        if (xorshift(s) % 16 == 0) p = params(static_cast<int>(xorshift(s) % 20) - 10, r(-1e9f, 1e9f), r(-50.0f, 50.0f), r(-5.0f, 5.0f), r(-1e3f, 1e3f), r(-3.0f, 3.0f));
        if (xorshift(s) % 64 == 0) p.driveDb = p.tone = p.bias = p.outDb = p.mix = std::nanf("");
        d.set(p, {});
        d.process(&L[pos], &R[pos], static_cast<int>(std::min<size_t>(kChunk, L.size() - pos)));
    }
    CHECK(allFinite(L) && allFinite(R));
    std::printf("  random jumps every chunk: peak %.1f\n", std::max(peak(L), peak(R)));
    CHECK(peak(L) < 64.0f && peak(R) < 64.0f);
}

void badInput() {
    // A NaN or infinity in the input acts as a zero there: the output is the same as with zeros.
    Buf clean = whiteNoise(8192, 0.5f, 21), bad = clean;
    for (size_t i : {1000u, 2000u, 3000u}) clean[i] = 0.0f;
    bad[1000] = std::nanf("");
    bad[2000] = INFINITY;
    bad[3000] = -INFINITY;
    for (int t = 0; t < Drive::kNumTypes; ++t) {
        const P p = params(t, 24.0f, 0.5f, 0.5f, 0.0f, 0.8f);
        const Buf y = render(p, bad);
        CHECK(allFinite(y));
        CHECK(same(y, render(p, clean)));
    }
}

void blockSizes() {
    // The same up to float rounding: on the device a whole chunk's shaper runs in NEON and a
    // shorter one partly in VFP, which fuses multiply-adds differently (Tube: 1.5e-7).
    const Buf x = whiteNoise(6000, 0.7f, 31);
    for (int t = 0; t < Drive::kNumTypes; ++t) {
        const P p = params(t, 20.0f, 0.3f, 0.5f, -3.0f, 0.7f);
        const Buf y32 = render(p, x, 32);
        CHECK(maxDiff(render(p, x, 1), y32) < 1e-6f);
        CHECK(maxDiff(render(p, x, 7), y32) < 1e-6f);
    }
    // One process() call longer than a chunk is cut into chunks.
    Drive d;
    Buf L = x, R = x;
    d.set(params(Drive::Tube, 20.0f), {});
    d.process(L.data(), R.data(), 100);
    Buf ref = x;
    ref.resize(100);
    CHECK(maxDiff(Buf(L.begin(), L.begin() + 100), render(params(Drive::Tube, 20.0f), ref, 32)) < 1e-6f);
}

void resetClears() {
    const Buf a = whiteNoise(5000, 0.8f, 41), b = whiteNoise(5000, 0.8f, 42);
    for (int t = 0; t < Drive::kNumTypes; ++t) {
        const P pa = params(Drive::Tube, 30.0f, -0.5f, 1.0f), pb = params(t, 18.0f, 0.5f, 0.2f);
        Drive d;
        Buf L = a, R = a;
        run(d, pa, L, R);
        d.reset();
        L = b;
        R = b;
        run(d, pb, L, R);
        CHECK(same(L, render(pb, b)));
    }
}

void tail() {
    // The longest tail: lots of DC from an asymmetric curve, cut off. The DC blocker's step
    // falls 60 dB within tailSamples(); at mix 0 there's nothing after the input.
    Drive d;
    Buf L = whiteNoise(44100, 0.8f, 51);
    L.resize(44100 + 12000, 0.0f);
    Buf R = L;
    run(d, params(Drive::Tube, 36.0f, 0.0f, 1.0f), L, R);
    const double start = peak(L, 44100, 44100 + 300);
    int last = 0;
    for (size_t i = 44100; i < L.size(); ++i)
        if (std::fabs(L[i]) > 1e-3 * start) last = static_cast<int>(i - 44100);
    std::printf("  tail: under -60 dB after %d samples (tailSamples %d)\n", last, d.tailSamples());
    CHECK(last <= d.tailSamples());
    CHECK(last > d.tailSamples() / 2);   // the DC blocker's decay really is the tail

    Drive dry;
    L = whiteNoise(4410, 0.8f, 52);
    L.resize(8820, 0.0f);
    R = L;
    run(dry, params(Drive::Fold, 36.0f, 0.0f, 1.0f, 0.0f, 0.0f), L, R);
    CHECK(dry.tailSamples() == 0 && peak(L, 4410) == 0.0f);
}

void noClicks() {
    // Out jumps 36 dB, the type steps through every shaper, drive jumps 36 dB: on a 100 Hz sine
    // no step between neighbouring samples is much bigger than the sine's own.
    const Buf x = sine(100.0, 32 * 300, kRefAmp);
    const float own = maxStep(x);   // 0.0036
    auto step = [&](auto paramsAt) {
        Drive d;
        Buf L = x, R = x;
        for (size_t pos = 0; pos < L.size(); pos += kChunk) {
            d.set(paramsAt(pos / kChunk), {});
            d.process(&L[pos], &R[pos], kChunk);
        }
        return maxStep(L, 4096);
    };
    const float out = step([](size_t c) { return params(Drive::Soft, 0.0f, 0.0f, 0.0f, c < 150 ? -24.0f : 12.0f); });
    CHECK(out < 0.06f);   // the ramp's share: 0.25 x (3.98 - 0.06) / 32 a sample; at once it would be ~1
    const float type = step([](size_t c) { return params(static_cast<int>(c / 20 % Drive::kNumTypes), 6.0f); });
    CHECK(type < 4.0f * own);
    // Drive 0 -> 36 dB at once: the compensation follows in dB, no level jump on the way.
    Drive d;
    const Buf s = sine(1000.0, 32 * 400, kRefAmp);
    Buf L = s, R = s;
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        d.set(params(Drive::Soft, pos < 32 * 200 ? 0.0f : 36.0f), {});
        d.process(&L[pos], &R[pos], kChunk);
    }
    const float before = peak(L, 32 * 100, 32 * 200), after = peak(L, 32 * 300, 32 * 400), during = peak(L, 32 * 199, 32 * 203);
    CHECK(during < 1.3f * std::max(before, after));
}

} // namespace

void driveTests() {
    std::printf("== drive\n");
    halfband();
    shapers();
    aliasing();
    harmonics();
    levels();
    tone();
    crush();
    mix();
    extremes();
    randomJumps();
    badInput();
    blockSizes();
    resetClears();
    tail();
    noClicks();
}
