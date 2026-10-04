#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("autofilt"); }

std::unique_ptr<EffectDevice> with(float rate, float depth, float base, float mix) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(0, rate); d->setParam(1, depth); d->setParam(2, base); d->setParam(3, mix);
    d->reset();
    return d;
}

// Run `seconds` of a sine at `hz` and return the output (left channel).
std::vector<float> run(EffectDevice& d, double hz, double seconds, double bpm = 120.0) {
    const size_t total = size_t(seconds * kSr) / kMaxBlock * kMaxBlock;
    std::vector<float> out; out.reserve(total);
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    ProcessContext c = ctx(); c.bpm = bpm;
    for (size_t t = 0; t < total; t += kMaxBlock) {
        for (int i = 0; i < kMaxBlock; ++i) l[size_t(i)] = r[size_t(i)] = 0.5f * float(std::sin(2.0 * M_PI * hz * double(t + size_t(i)) / kSr));
        d.process(l.data(), r.data(), kMaxBlock, c, {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

// RMS over [t0, t0 + 0.02 s].
double rmsAt(const std::vector<float>& x, double t0) {
    const size_t a = size_t(t0 * kSr), n = size_t(0.02 * kSr);
    double acc = 0; for (size_t i = a; i < a + n && i < x.size(); ++i) acc += double(x[i]) * x[i];
    return std::sqrt(acc / double(n));
}
}  // namespace

TEST_CASE("autofilt: device contract", "[device][autofilt]") { checkEffectContract(make); }

TEST_CASE("autofilt: the LFO sweeps the cutoff between base and base*2^3.5", "[device][autofilt]") {
    // rate 1 Hz, depth 1, base 200 Hz: cutoff = 200 at sin = -1 (t = 0.75 s), 2263 Hz at sin = +1 (t = 0.25 s).
    auto d = with(1.0f, 1.0f, 200.0f, 1.0f);
    auto x = run(*d, 1000.0, 1.5);
    const double open = rmsAt(x, 0.24), closed = rmsAt(x, 0.74);
    CHECK(open > 0.3);                    // 1 kHz sine (rms 0.35) passes a 2.3 kHz lowpass almost untouched
    CHECK(closed < 0.05 * open);          // 200 Hz lowpass: 1 kHz is >2 octaves above, 24+ dB down
}

TEST_CASE("autofilt: depth 0 holds the cutoff steady", "[device][autofilt]") {
    // cutoff = base + span/2 with no modulation: the level of a 1 kHz sine must not vary over time.
    auto d = with(1.0f, 0.0f, 200.0f, 1.0f);
    auto x = run(*d, 1000.0, 1.5);
    const double a = rmsAt(x, 0.24), b = rmsAt(x, 0.74);
    CHECK(a == Catch::Approx(b).epsilon(0.01));
    CHECK(a > 0.05);
}

TEST_CASE("autofilt: rate sets the LFO speed", "[device][autofilt]") {
    // At 4 Hz the cutoff minimum (sin = -1) falls at t = 0.1875 s and the maximum at 0.0625 s.
    auto d = with(4.0f, 1.0f, 200.0f, 1.0f);
    auto x = run(*d, 1000.0, 0.5);
    CHECK(rmsAt(x, 0.185) < 0.08 * rmsAt(x, 0.06));   // the sweep moves during the 20 ms window, so not the full 1 Hz depth
}

TEST_CASE("autofilt: mix 0 is a bit-exact dry pass, mix 1 is fully filtered", "[device][autofilt]") {
    auto dry = with(1.0f, 1.0f, 200.0f, 0.0f);
    std::vector<float> l(kMaxBlock), r(kMaxBlock), ref(kMaxBlock);
    for (int i = 0; i < kMaxBlock; ++i) ref[size_t(i)] = l[size_t(i)] = r[size_t(i)] = 0.4f * float(std::sin(0.3 * i));
    dry->process(l.data(), r.data(), kMaxBlock, ctx(), {});
    CHECK(l == ref);
    auto wet = with(1.0f, 1.0f, 200.0f, 1.0f);
    auto x = run(*wet, 5000.0, 0.8);
    CHECK(rmsAt(x, 0.6) < 0.02);          // 5 kHz is far above any cutoff the sweep reaches (max 2.3 kHz)
}

TEST_CASE("autofilt: the filter is stereo-linked (identical L and R in, identical out)", "[device][autofilt]") {
    auto d = with(2.0f, 0.8f, 300.0f, 0.7f);
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < 50; ++b) {
        for (int i = 0; i < kMaxBlock; ++i) l[size_t(i)] = r[size_t(i)] = 0.4f * float(std::sin(0.11 * (b * kMaxBlock + i)));
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        REQUIRE(l == r);
    }
}

namespace {
// Count LFO cycles from the cutoff sweep: amplitude of a 1 kHz sine through base 200 Hz, depth 1 rises and
// falls once per LFO cycle. Count upward crossings of the mid-level of the 2 ms-smoothed RMS envelope.
int lfoCycles(EffectDevice& d, double bpm, double seconds) {
    auto x = run(d, 1000.0, seconds, bpm);
    std::vector<double> env;
    const size_t w = size_t(0.004 * kSr);
    for (size_t i = 0; i + w < x.size(); i += w / 2) {
        double a = 0; for (size_t j = 0; j < w; ++j) a += double(x[i + j]) * x[i + j];
        env.push_back(std::sqrt(a / double(w)));
    }
    double lo = 1e9, hi = 0;
    for (size_t i = env.size() / 8; i < env.size(); ++i) { lo = std::min(lo, env[i]); hi = std::max(hi, env[i]); }
    const double mid = 0.5 * (lo + hi);
    int n = 0;
    for (size_t i = env.size() / 8 + 1; i < env.size(); ++i) n += env[i - 1] < mid && env[i] >= mid;
    return n;
}
std::unique_ptr<EffectDevice> withRate(float mode, float sync, float hi, float rate = 1.0f) {
    auto d = with(rate, 1.0f, 200.0f, 1.0f);
    d->setParam(4, mode); d->setParam(5, sync); d->setParam(6, hi);
    d->reset();
    return d;
}
}  // namespace

TEST_CASE("autofilt: Sync mode follows ProcessContext.bpm", "[device][autofilt]") {
    // rateSync 5 = quarter note (96 ticks): 2 Hz at 120 bpm, 4 Hz at 240 bpm.
    CHECK(lfoCycles(*withRate(0, 5, 440), 120.0, 3.0) == Catch::Approx(5).margin(1));   // ~2 Hz * (3 s - 1/8 skipped)
    CHECK(lfoCycles(*withRate(0, 5, 440), 240.0, 3.0) == Catch::Approx(10).margin(1));
    // rateSync 3 = 384 ticks = one bar: 0.5 Hz at 120 bpm
    CHECK(lfoCycles(*withRate(0, 3, 440), 120.0, 6.0) == Catch::Approx(2).margin(1));
}

TEST_CASE("autofilt: Sync mode retunes mid-stream when the tempo changes", "[device][autofilt]") {
    auto d = withRate(0, 5, 440);
    std::vector<float> l(kMaxBlock, 0.0f), r(kMaxBlock, 0.0f);
    ProcessContext c = ctx();
    for (int b = 0; b < 20; ++b) { c.bpm = b < 10 ? 120.0 : 60.0; d->process(l.data(), r.data(), kMaxBlock, c, {}); }
    for (float v : l) REQUIRE(std::isfinite(v));
}

TEST_CASE("autofilt: Low mode ignores bpm, rateSync and rateHi", "[device][autofilt]") {
    const int a = lfoCycles(*withRate(1, 5, 440, 2.0f), 120.0, 3.0);
    const int b = lfoCycles(*withRate(1, 0, 900, 2.0f), 240.0, 3.0);
    CHECK(a == b);
    CHECK(a == Catch::Approx(5).margin(1));
}

TEST_CASE("autofilt: High mode runs the LFO at rateHi (audio-rate, 301..1000 Hz)", "[device][autofilt]") {
    // A 1 kHz sine through a cutoff swept at f_m gains amplitude-modulation sidebands at 1000 +/- f_m.
    auto goertzel = [](const std::vector<float>& x, double hz) {
        const double w = 2.0 * M_PI * hz / kSr;
        double re = 0, im = 0;
        for (size_t i = 0; i < x.size(); ++i) { re += double(x[i]) * std::cos(w * double(i)); im += double(x[i]) * std::sin(w * double(i)); }
        return std::sqrt(re * re + im * im) / double(x.size());
    };
    auto render = [&](float hi) {
        auto d = withRate(2, 5, hi);
        auto x = run(*d, 1000.0, 0.6);
        x.erase(x.begin(), x.begin() + long(0.1 * kSr));
        return x;
    };
    const auto a = render(400.0f), b = render(900.0f);
    CHECK(goertzel(a, 1400.0) > 3.0 * goertzel(a, 1900.0));    // rateHi 400: sideband at 1400, none at 1900
    CHECK(goertzel(b, 1900.0) > 3.0 * goertzel(b, 1400.0));    // rateHi 900: the opposite
}
