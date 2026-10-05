// Decimator4 and fastSin2Pi: the building blocks of the oversampled FM/AM operators.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/Decimator4.h"
#include "dsp/FastSin.h"

using namespace ddaw::dsp;
using Catch::Approx;

namespace {
// Run `n` base-rate output samples of a 4x-rate signal given as f(highRateIndex).
template <class F> std::vector<float> run(Decimator4& d, int n, F&& f) {
    const size_t count = size_t(n);
    std::vector<float> out(count);
    for (int i = 0; i < n; ++i) out[size_t(i)] = d.push(f(4 * i), f(4 * i + 1), f(4 * i + 2), f(4 * i + 3));
    return out;
}
double rmsOf(const std::vector<float>& v, size_t from) {
    double s = 0;
    for (size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i];
    return std::sqrt(s / double(v.size() - from));
}
}  // namespace

TEST_CASE("Decimator4: unity gain at DC and in the passband", "[decimator]") {
    for (const auto taps : {std::pair{23, 11}, std::pair{47, 15}}) {
        Decimator4 d(taps.first, taps.second);
        const auto dc = run(d, 400, [](int) { return 1.0f; });
        CHECK(dc.back() == Approx(1.0f).margin(1e-3));
        d.reset();
        const auto tone = run(d, 2000, [](int k) { return float(std::sin(2.0 * std::numbers::pi * 1000.0 * k / 192000.0)); });   // 1 kHz at 48 kHz
        CHECK(rmsOf(tone, 200) == Approx(std::sqrt(0.5)).margin(0.01));
    }
}

TEST_CASE("Decimator4: what lies above the base-rate Nyquist is removed instead of folding back", "[decimator]") {
    // 30 kHz at a 192 kHz rate would fold to 18 kHz at 48 kHz. The FM synth's 47/15-tap pair removes it by > 70 dB;
    // the 23/11 pair of Oversampler4 (used by the distortion effects) by 25 dB there and > 50 dB from 34 kHz.
    const auto level = [](Decimator4& d, double hz) {
        return rmsOf(run(d, 4000, [hz](int k) { return float(std::sin(2.0 * std::numbers::pi * hz * k / 192000.0)); }), 500) / std::sqrt(0.5);
    };
    Decimator4 sharp(47, 15), relaxed;
    CHECK(level(sharp, 30000.0) < 3e-4);
    CHECK(level(sharp, 40000.0) < 3e-4);
    CHECK(level(relaxed, 34000.0) < 3e-3);
}

TEST_CASE("Decimator4: the impulse response peaks at the advertised latency", "[decimator]") {
    for (const auto taps : {std::pair{23, 11}, std::pair{47, 15}}) {
        Decimator4 d(taps.first, taps.second);
        const auto h = run(d, 64, [](int k) { return k == 0 ? 1.0f : 0.0f; });
        size_t peak = 0;
        for (size_t i = 1; i < h.size(); ++i) if (std::abs(h[i]) > std::abs(h[peak])) peak = i;
        CHECK(int(peak) == d.latency());
    }
}

TEST_CASE("fastSin2Pi: matches sin to the table's interpolation error, for any phase", "[decimator]") {
    double worst = 0;
    for (int i = -50000; i <= 50000; ++i) {
        const double x = double(i) * 0.000137;
        worst = std::max(worst, std::abs(double(fastSin2Pi(float(x))) - std::sin(2.0 * std::numbers::pi * double(float(x)))));
    }
    CHECK(worst < 5e-6);
}
