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
std::unique_ptr<EffectDevice> make() { return createEffect("dist"); }
int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

// Run `n` frames (multiple of kMaxBlock) of a sine with exactly `cycles` cycles per 4096 samples.
std::vector<float> sine(EffectDevice& d, int cycles, float amp, int n) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int t = 0; t < n; t += kMaxBlock) {
        for (int i = 0; i < kMaxBlock; ++i)
            l[size_t(i)] = r[size_t(i)] = amp * float(std::sin(2.0 * std::numbers::pi * cycles * double(t + i) / 4096.0));
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

std::vector<float> constant(EffectDevice& d, float v, int n) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int t = 0; t < n; t += kMaxBlock) {
        std::fill(l.begin(), l.end(), v); std::fill(r.begin(), r.end(), v);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

double dft(const std::vector<float>& x, size_t start, int cycles) {  // |DFT| over a 4096 window
    double re = 0, im = 0;
    for (size_t i = 0; i < 4096; ++i) {
        const double ph = 2.0 * std::numbers::pi * cycles * double(i) / 4096.0;
        re += double(x[start + i]) * std::cos(ph);
        im -= double(x[start + i]) * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / 4096.0;
}
}  // namespace

TEST_CASE("dist: device contract", "[device][dist]") { checkEffectContract(make); }

TEST_CASE("dist: a sine yields odd harmonics only, and is bounded", "[device][dist]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "amt", 1.0f); set(*d, "mix", 1.0f); d->reset();
    auto y = sine(*d, 32, 0.8f, 4096 * 3);
    const size_t s = 4096 * 2;
    const double h1 = dft(y, s, 32), h2 = dft(y, s, 64), h3 = dft(y, s, 96);
    CHECK(h1 > 0.05);
    CHECK(h3 > 0.005);
    CHECK(h2 < 0.1 * h3);                          // odd-symmetric curve: no even harmonics
    for (float v : y) CHECK(std::abs(v) < 0.5f);   // |f| < 0.35 plus FIR overshoot
}

TEST_CASE("dist: DC settles on the Tone.Distortion curve value", "[device][dist]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "amt", 0.4f); set(*d, "mix", 1.0f); d->reset();
    auto y = constant(*d, 0.5f, kMaxBlock * 20);
    const double k = 40.0, pi = std::numbers::pi;
    const double expect = ((3.0 + k) * 0.5 * pi / 9.0) / (pi + k * 0.5);
    CHECK(double(y.back()) == Catch::Approx(expect).margin(2e-3));

    // Beyond the curve domain the input is clamped: 3.0 saturates at f(1).
    d->reset();
    y = constant(*d, 3.0f, kMaxBlock * 20);
    CHECK(double(y.back()) == Catch::Approx(((3.0 + k) * pi / 9.0) / (pi + k)).margin(2e-3));
}

TEST_CASE("dist: mix 0 passes the dry signal, mix scales equal-power", "[device][dist]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "amt", 1.0f); set(*d, "mix", 0.0f); d->reset();
    auto y = sine(*d, 32, 0.5f, 4096 * 3);
    CHECK(dft(y, 4096 * 2, 32) == Catch::Approx(0.5).margin(0.01));
    CHECK(dft(y, 4096 * 2, 96) < 0.005);           // no distortion product
}

TEST_CASE("dist: drive adds harmonics", "[device][dist]") {
    auto h3 = [](float amt) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "amt", amt); set(*d, "mix", 1.0f); d->reset();
        auto y = sine(*d, 32, 0.5f, 4096 * 3);
        return dft(y, 4096 * 2, 96) / dft(y, 4096 * 2, 32);
    };
    CHECK(h3(0.9f) > 2.0 * h3(0.1f));
}
