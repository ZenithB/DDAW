#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("filter"); }

double gainDb(EffectDevice& d, double hz, int blocks = 300) {
    d.reset();
    double acc = 0, ref = 0; size_t t = 0;
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < kMaxBlock; ++i, ++t) l[size_t(i)] = r[size_t(i)] = float(std::sin(2.0 * M_PI * hz * double(t) / kSr));
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        if (b >= blocks / 2) for (int i = 0; i < kMaxBlock; ++i) { acc += double(l[size_t(i)]) * l[size_t(i)]; ref += 0.5; }
    }
    return 10.0 * std::log10(std::max(acc / ref, 1e-30));
}

std::unique_ptr<EffectDevice> with(float ftype, float freq, float q, float slope) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(0, ftype); d->setParam(1, freq); d->setParam(2, q); d->setParam(3, slope);
    d->reset();
    return d;
}
}  // namespace

TEST_CASE("filter: device contract", "[device][filter]") { checkEffectContract(make); }

TEST_CASE("filter: lowpass corner and resonance (q is in dB, Web Audio style)", "[device][filter]") {
    auto flat = with(0, 1000, 0, 0);
    CHECK(gainDb(*flat, 100) == Catch::Approx(0.0).margin(0.2));
    CHECK(gainDb(*flat, 1000) == Catch::Approx(0.0).margin(0.5));        // Q = 0 dB: unity at the corner
    auto res = with(0, 1000, 6, 0);
    CHECK(gainDb(*res, 1000) == Catch::Approx(6.0).margin(0.5));         // +6 dB peak at the corner
}

TEST_CASE("filter: lowpass stop-band and slope", "[device][filter]") {
    auto s12 = with(0, 500, 0, 0), s24 = with(0, 500, 0, 1), s48 = with(0, 500, 0, 2);
    const double a = gainDb(*s12, 4000), b = gainDb(*s24, 4000), c = gainDb(*s48, 4000);
    CHECK(a < -20.0);                           // 3 octaves at 12 dB/oct
    CHECK(b == Catch::Approx(2.0 * a).epsilon(0.1));
    CHECK(c == Catch::Approx(4.0 * a).epsilon(0.1));
}

TEST_CASE("filter: highpass blocks lows, passes highs", "[device][filter]") {
    auto hp = with(1, 2000, 0, 1);
    CHECK(gainDb(*hp, 100) < -40.0);
    CHECK(gainDb(*hp, 10000) == Catch::Approx(0.0).margin(0.5));
    CHECK(gainDb(*hp, 2000) == Catch::Approx(0.0).margin(0.5));          // Q = 0 dB: unity at the corner
}

TEST_CASE("filter: bandpass is unity at its centre and rejects far bands", "[device][filter]") {
    auto bp = with(2, 1000, 2, 0);
    CHECK(gainDb(*bp, 1000) == Catch::Approx(0.0).margin(0.5));
    CHECK(gainDb(*bp, 12000) < -20.0);
    CHECK(gainDb(*bp, 60) < -20.0);
}

TEST_CASE("filter: switching type mid-stream stays finite and keeps running", "[device][filter]") {
    auto d = with(0, 800, 8, 2);
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < 200; ++b) {
        d->setParam(0, float(b % 3)); d->setParam(3, float((b / 3) % 3));
        for (int i = 0; i < kMaxBlock; ++i) l[size_t(i)] = r[size_t(i)] = 0.5f * float(std::sin(0.05 * double(b * kMaxBlock + i)));
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        REQUIRE(finiteAndBounded(l));
    }
}
