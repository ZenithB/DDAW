#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("shift"); }

// Run `seconds` of a sine through the device (fresh state, given params); returns the left channel.
std::vector<float> runSine(float hz, float amt, float mix, double seconds, float amp = 0.5f) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(0, amt); d->setParam(1, mix); d->reset();
    const size_t total = static_cast<size_t>(seconds * kSr);
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (size_t t = 0; t < total; t += kMaxBlock) {
        for (int i = 0; i < kMaxBlock; ++i) {
            l[size_t(i)] = r[size_t(i)] = amp * float(std::sin(2.0 * std::numbers::pi * hz * double(t + size_t(i)) / kSr));
        }
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

// Goertzel magnitude at `hz`, normalised so a full-scale sine reads ~1.
double goertzel(const float* x, size_t n, double hz) {
    const double w = 2.0 * std::numbers::pi * hz / kSr, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) { const double s0 = double(x[i]) + c * s1 - s2; s2 = s1; s1 = s0; }
    return 2.0 * std::sqrt(std::max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0)) / double(n);
}
}  // namespace

TEST_CASE("shift: device contract", "[device][shift]") { checkEffectContract(make); }

TEST_CASE("shift: reports no latency", "[device][shift]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    CHECK(d->latencySamples() == 0);
}

TEST_CASE("shift: +100 Hz moves a 440 Hz sine to 540 Hz", "[device][shift]") {
    const auto x = runSine(440.0f, 100.0f, 1.0f, 2.0);
    const float* tail = x.data() + x.size() - size_t(kSr);               // last whole second: integer-Hz bins
    const double up = goertzel(tail, size_t(kSr), 540.0);
    CHECK(up > 0.3);
    double peak = 0, peakHz = 0;
    for (double f = 20; f <= 2000; f += 20) { const double g = goertzel(tail, size_t(kSr), f); if (g > peak) { peak = g; peakHz = f; } }
    CHECK(peakHz == Catch::Approx(540.0).margin(1.0));
    CHECK(goertzel(tail, size_t(kSr), 440.0) < 0.1 * up);                // carrier leak
    CHECK(goertzel(tail, size_t(kSr), 340.0) < 0.1 * up);                // rejected sideband
}

TEST_CASE("shift: negative amt shifts down", "[device][shift]") {
    const auto x = runSine(440.0f, -100.0f, 1.0f, 2.0);
    const float* tail = x.data() + x.size() - size_t(kSr);
    const double down = goertzel(tail, size_t(kSr), 340.0);
    CHECK(down > 0.3);
    CHECK(goertzel(tail, size_t(kSr), 540.0) < 0.1 * down);
}

TEST_CASE("shift: mix 0 is exactly dry, mix 1 differs from the input", "[device][shift]") {
    const auto dry = runSine(440.0f, 60.0f, 0.0f, 0.2);
    const auto wet = runSine(440.0f, 60.0f, 1.0f, 0.2);
    double maxErr = 0, diff = 0;
    for (size_t i = 0; i < dry.size(); ++i) {
        const float in = 0.5f * float(std::sin(2.0 * std::numbers::pi * 440.0 * double(i) / kSr));
        maxErr = std::max(maxErr, double(std::abs(dry[i] - in)));
        diff += std::abs(wet[i] - in);
    }
    CHECK(maxErr < 1e-6);
    CHECK(diff > 1.0);
}

TEST_CASE("shift: 0 Hz shift keeps the frequency and level (up to the allpass phase)", "[device][shift]") {
    const auto x = runSine(440.0f, 0.0f, 1.0f, 1.0);
    const float* tail = x.data() + x.size() / 2;
    const size_t n = x.size() - x.size() / 2;
    CHECK(goertzel(tail, n, 440.0) == Catch::Approx(0.5).margin(0.03));  // unity-gain allpass pair
}
