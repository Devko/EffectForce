// dsp/common.h and the fast math the modules rely on.
#include "signal.h"

namespace {

using namespace eft;
using namespace ef;

void levels() {
    CHECK(std::fabs(dbToGain(0.0f) - 1.0f) < 1e-6f);
    CHECK(std::fabs(dbToGain(-6.0206f) - 0.5f) < 1e-5f);
    CHECK(std::fabs(dbToGain(24.0f) - 15.8489f) < 1e-3f);
    CHECK(std::fabs(gainToDb(0.5f) + 6.0206f) < 1e-4f);
    CHECK(gainToDb(0.0f) == -200.0f);
    CHECK(sanitize(std::nanf("")) == 0.0f && sanitize(INFINITY) == 0.0f && sanitize(0.25f) == 0.25f);
}

void fastMath() {
    float worst = 0.0f;
    for (float x = -20.0f; x < 20.0f; x += 0.01f) worst = std::max(worst, std::fabs(exp2Fast(x) / std::exp2(x) - 1.0f));
    CHECK(worst < 2e-7f);
    worst = 0.0f;
    for (float x = 1e-6f; x < 1e6f; x *= 1.01f) worst = std::max(worst, std::fabs(log2Fast(x) - std::log2(x)));
    CHECK(worst < 5e-6f);
    worst = 0.0f;   // the filters' range: up to 0.45 of the rate
    for (float w = 1e-4f; w < 0.45f * 3.14159265f; w += 1e-3f) worst = std::max(worst, std::fabs(tanFast(w) / std::tan(w) - 1.0f));
    CHECK(worst < 2e-6f);
}

void ramp() {
    Ramp r;
    r.jump(1.0f);
    CHECK(r.next() == 1.0f);
    r.to(2.0f, 4);
    CHECK(std::fabs(r.next() - 1.25f) < 1e-6f);
    r.next();
    r.next();
    CHECK(r.next() == 2.0f);   // lands exactly
    CHECK(r.next() == 2.0f);   // and stays
    r.to(0.0f, 0);             // n < 1 counts as 1
    CHECK(r.next() == 0.0f);
}

void delayLine() {
    DelayLine d(100);
    CHECK(d.capacity() >= 100);
    for (int i = 0; i < 200; ++i) d.write(static_cast<float>(i));
    CHECK(d.at(0) == 199.0f && d.at(1) == 198.0f);
    CHECK(d.readLinear(10.0f) == 189.0f);
    CHECK(std::fabs(d.readLinear(10.25f) - 188.75f) < 1e-4f);
    CHECK(std::fabs(d.readCubic(10.0f) - 189.0f) < 1e-4f);
    CHECK(std::fabs(d.readCubic(10.5f) - 188.5f) < 1e-3f);   // a ramp: cubic is exact on it
    d.clear();
    CHECK(d.at(5) == 0.0f);
}

void transport() {
    Transport t;
    t.beats = 5.0;
    CHECK(std::fabs(lockedPhase(t, 4.0) - 0.25) < 1e-12);
    CHECK(std::fabs(lockedPhase(t, 4.0, 0.8) - 0.05) < 1e-12);
    t.beats = -1.0;   // a pre-roll: still 0..1
    CHECK(lockedPhase(t, 4.0) >= 0.0 && lockedPhase(t, 4.0) < 1.0);
    CHECK(kNumDelayDivs == 16 && kNumLfoDivs == 14);
    CHECK(std::fabs(divSeconds(kDelayDivs[9].beats, 120.0) - 0.375) < 1e-12);   // 1/8. at 120 BPM
    for (int i = 1; i < kNumDelayDivs; ++i) CHECK(kDelayDivs[i].beats > kDelayDivs[i - 1].beats);
    for (int i = 1; i < kNumLfoDivs; ++i) CHECK(kLfoDivs[i].beats > kLfoDivs[i - 1].beats);
}

void measurement() {   // the test helpers themselves
    const Buf s = sine(1000.0, 16384, 0.5f);
    CHECK(std::fabs(magnitude(s, 1000.0) - 0.5) < 1e-3);
    CHECK(magnitude(s, 3000.0) < 1e-4);
    CHECK(std::fabs(rms(s) - 0.5 / std::sqrt(2.0)) < 1e-3);
}

} // namespace

void dspTests() {
    levels();
    fastMath();
    ramp();
    delayLine();
    transport();
    measurement();
}
