#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("autotune"); }

// Params in schema order: amount, speed, mix, mode.
std::vector<float> runSine(float hz, float amount, float speed, float mix, float mode, double seconds, float amp = 0.5f) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(0, amount); d->setParam(1, speed); d->setParam(2, mix); d->setParam(3, mode); d->reset();
    const size_t total = static_cast<size_t>(seconds * kSr);
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (size_t t = 0; t < total; t += kMaxBlock) {
        for (int i = 0; i < kMaxBlock; ++i)
            l[size_t(i)] = r[size_t(i)] = amp * float(std::sin(2.0 * std::numbers::pi * double(hz) * double(t + size_t(i)) / kSr));
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

double dftMag(const float* x, size_t n, double hz) {
    double re = 0, im = 0;
    const double w = 2.0 * std::numbers::pi * hz / kSr;
    for (size_t i = 0; i < n; ++i) { re += double(x[i]) * std::cos(w * double(i)); im += double(x[i]) * std::sin(w * double(i)); }
    return std::sqrt(re * re + im * im);
}

double peakHz(const std::vector<float>& x, size_t n, double lo, double hi, double step) {
    const float* tail = x.data() + x.size() - n;
    double best = lo, bestMag = -1;
    for (double f = lo; f <= hi; f += step) { const double m = dftMag(tail, n, f); if (m > bestMag) { bestMag = m; best = f; } }
    return best;
}
}  // namespace

TEST_CASE("autotune: device contract", "[device][autotune]") { checkEffectContract(make); }

TEST_CASE("autotune: wet path is audible", "[device][autotune]") {
    const auto x = runSine(220.0f, 1.0f, 20.0f, 1.0f, 0.0f, 2.0);
    for (float v : x) REQUIRE(std::isfinite(v));
    CHECK(harness::rms(std::span<const float>(x).subspan(x.size() / 2)) > 0.05);
}

TEST_CASE("autotune: mix 0 is dry passthrough", "[device][autotune]") {
    const auto x = runSine(445.0f, 1.0f, 20.0f, 0.0f, 0.0f, 0.5);
    double maxErr = 0;
    for (size_t i = 0; i < x.size(); ++i)
        maxErr = std::max(maxErr, double(std::abs(x[i] - 0.5f * float(std::sin(2.0 * std::numbers::pi * 445.0 * double(i) / kSr)))));
    CHECK(maxErr < 1e-4);
}

TEST_CASE("autotune: chromatic mode pulls a detuned 445 Hz sine toward 440 Hz", "[device][autotune]") {
    const auto x = runSine(445.0f, 1.0f, 20.0f, 1.0f, 1.0f, 2.0);
    const double p = peakHz(x, 22050, 420.0, 460.0, 0.5);
    CHECK(std::abs(p - 440.0) < std::abs(445.0 - 440.0));
}

TEST_CASE("autotune: key mode snaps an out-of-scale note to the default A-minor scale", "[device][autotune]") {
    // F#4 (369.99 Hz) is not in A minor; the nearest degrees are F4 (349.23) and G4 (392.0), and the
    // downward candidate wins, so the output settles near F4 rather than staying at 370 Hz.
    const double fs4 = 440.0 * std::pow(2.0, -3.0 / 12.0);
    const auto x = runSine(float(fs4), 1.0f, 20.0f, 1.0f, 0.0f, 2.5);
    const double p = peakHz(x, 22050, 330.0, 400.0, 0.5);
    CHECK(p == Catch::Approx(349.23).margin(4.0));
}

TEST_CASE("autotune: an in-scale note passes at (almost) the same pitch", "[device][autotune]") {
    const auto x = runSine(440.0f, 1.0f, 20.0f, 1.0f, 0.0f, 2.0);   // A4: the root
    CHECK(peakHz(x, 22050, 420.0, 460.0, 0.5) == Catch::Approx(440.0).margin(4.0));
}

TEST_CASE("autotune: amount 0 applies no correction", "[device][autotune]") {
    const auto x = runSine(445.0f, 0.0f, 20.0f, 1.0f, 1.0f, 2.0);
    CHECK(peakHz(x, 22050, 420.0, 460.0, 0.5) == Catch::Approx(445.0).margin(1.0));
}
