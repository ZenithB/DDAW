#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "AllocGuard.h"
#include "dsp/Limiter.h"
#include "dsp/Smoother.h"

using namespace ddaw::dsp;

namespace {
constexpr double kSr = 48000.0;
std::vector<float> sine(size_t n, double hz, double amp) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = float(amp * std::sin(2.0 * std::numbers::pi * hz * double(i) / kSr));
    return x;
}
void run(Limiter& lim, std::vector<float>& l, std::vector<float>& r, int block = 128) {
    for (size_t pos = 0; pos < l.size(); pos += size_t(block))
        lim.process(l.data() + pos, r.data() + pos, int(std::min<size_t>(size_t(block), l.size() - pos)));
}
}  // namespace

TEST_CASE("limiter is transparent below the ceiling (bit-exact, delayed by its latency)", "[limiter]") {
    Limiter lim; lim.prepare(kSr, -1.0f);
    auto src = sine(20000, 440, 0.5);   // -6 dBFS, under the -1 dBFS ceiling
    auto l = src, r = src;
    run(lim, l, r);
    const size_t L = size_t(lim.latencySamples());
    REQUIRE(L > 0);
    for (size_t i = 0; i < L; ++i) REQUIRE(l[i] == 0.0f);
    for (size_t i = L; i < l.size(); ++i) REQUIRE(l[i] == src[i - L]);
    CHECK(lim.gainReductionDb() == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("limiter never exceeds the ceiling, whatever the input", "[limiter]") {
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    Limiter lim; lim.prepare(kSr, -1.0f);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const size_t n = 200000;
    std::vector<float> l(n), r(n);
    for (size_t i = 0; i < n; ++i) { l[i] = 0.3f * u(rng); r[i] = 0.3f * u(rng); }
    auto hot = sine(n, 997, 4.0);                         // +12 dBFS sine in sections
    for (size_t i = 20000; i < 60000; ++i) l[i] = hot[i];
    for (size_t i = 100000; i < n; i += 5000) { l[i] = 30.0f; r[i] = -25.0f; }   // isolated spikes, +30 dBFS
    l[0] = 50.0f;                                         // spike in the very first sample
    run(lim, l, r, 64);
    float mx = 0;
    for (size_t i = 0; i < n; ++i) { REQUIRE(std::isfinite(l[i])); REQUIRE(std::isfinite(r[i])); mx = std::max({mx, std::abs(l[i]), std::abs(r[i])}); }
    CHECK(mx <= ceiling * 1.000001f);
    CHECK(mx > ceiling * 0.95f);  // and it is actually limiting to the ceiling, not crushing
}

TEST_CASE("limiter is stereo-linked and releases", "[limiter]") {
    Limiter lim; lim.prepare(kSr, -1.0f, 1.5f, 50.0f);
    const size_t n = 96000;
    std::vector<float> l(n, 0.0f), r(n, 0.2f);
    for (size_t i = 0; i < 4800; ++i) l[i] = 3.0f;           // 100 ms of loud left channel only
    for (size_t i = 4800; i < n; ++i) l[i] = 0.2f;
    run(lim, l, r);
    // while limiting, the quiet right channel is reduced by the same gain (linked): 0.2 -> well below 0.2
    CHECK(r[3000] < 0.2f * 0.5f);
    // after > 5 release times the gain is back to unity
    CHECK(r[n - 1] == Catch::Approx(0.2f).margin(1e-4));
}

TEST_CASE("limiter does not allocate", "[limiter][realtime]") {
    Limiter lim; lim.prepare(kSr, -1.0f);
    std::vector<float> l(128, 2.0f), r(128, -2.0f);
    ddaw::test::AllocGuard g;
    for (int i = 0; i < 1000; ++i) lim.process(l.data(), r.data(), 128);
    CHECK(g.count() == 0);
}

TEST_CASE("smoother converges and snaps", "[dsp]") {
    Smoother s; s.prepare(kSr, 15.0f);
    s.snap(0.0f); s.setTarget(1.0f);
    float v = 0; for (int i = 0; i < int(kSr * 0.015); ++i) v = s.next();
    CHECK(v == Catch::Approx(1.0 - std::exp(-1.0)).margin(0.01));  // one time constant
    for (int i = 0; i < int(kSr); ++i) v = s.next();
    CHECK(v == Catch::Approx(1.0f).margin(1e-4));
    s.snap(0.25f);
    CHECK(s.next() == 0.25f);
}
