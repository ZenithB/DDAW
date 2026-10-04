#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("eq"); }

// Gain in dB of a steady sine at `hz` (RMS over the second half, after smoothing and filter settle).
double gainDb(EffectDevice& d, double hz, int blocks = 400) {
    d.reset();
    double acc = 0, ref = 0; size_t cnt = 0, t = 0;
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < kMaxBlock; ++i, ++t) l[size_t(i)] = r[size_t(i)] = float(std::sin(2.0 * M_PI * hz * double(t) / kSr));
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        if (b >= blocks / 2) for (int i = 0; i < kMaxBlock; ++i) { acc += double(l[size_t(i)]) * l[size_t(i)]; ref += 0.5; ++cnt; }
    }
    return 10.0 * std::log10(acc / ref);
}

std::unique_ptr<EffectDevice> with(float low, float mid, float high) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(0, low); d->setParam(1, mid); d->setParam(2, high);
    d->reset();
    return d;
}
}  // namespace

TEST_CASE("eq: device contract", "[device][eq]") { checkEffectContract(make); }

TEST_CASE("eq: flat settings are near unity (Tone.EQ3 crossover colouration only)", "[device][eq]") {
    auto d = with(0, 0, 0);
    for (double f : {100.0, 1000.0, 8000.0}) CHECK(std::abs(gainDb(*d, f)) < 3.0);
}

TEST_CASE("eq: each band boosts its own region by about its gain", "[device][eq]") {
    auto flat = with(0, 0, 0);
    const double f100 = gainDb(*flat, 100), f1k = gainDb(*flat, 1000), f10k = gainDb(*flat, 10000);
    auto lo = with(12, 0, 0), mi = with(0, 12, 0), hi = with(0, 0, 12);
    CHECK(gainDb(*lo, 100) - f100 == Catch::Approx(12.0).margin(1.5));
    CHECK(gainDb(*mi, 1000) - f1k == Catch::Approx(12.0).margin(3.0));   // the flat mid sits ~2 dB down (crossover overlap)
    CHECK(gainDb(*hi, 10000) - f10k == Catch::Approx(12.0).margin(1.5));
}

TEST_CASE("eq: cutting a band leaves the far-away bands alone (stop-band)", "[device][eq]") {
    auto flat = with(0, 0, 0);
    auto lowCut = with(-12, 0, 0);
    CHECK(gainDb(*lowCut, 100) - gainDb(*flat, 100) < -9.0);
    CHECK(std::abs(gainDb(*lowCut, 8000) - gainDb(*flat, 8000)) < 1.0);   // low cut does not leak into the highs
    auto highCut = with(0, 0, -12);
    CHECK(std::abs(gainDb(*highCut, 100) - gainDb(*flat, 100)) < 1.0);    // and vice versa
    CHECK(gainDb(*highCut, 10000) - gainDb(*flat, 10000) < -9.0);
}

TEST_CASE("eq: gain changes are smoothed, not stepped", "[device][eq]") {
    auto d = with(0, 0, 0);
    std::vector<float> l(kMaxBlock, 0.5f), r(kMaxBlock, 0.5f);
    for (int b = 0; b < 100; ++b) d->process(l.data(), r.data(), kMaxBlock, ctx(), {});   // DC settles
    const float before = l[kMaxBlock - 1];
    d->setParam(0, -12.0f);
    std::fill(l.begin(), l.end(), 0.5f); std::fill(r.begin(), r.end(), 0.5f);
    d->process(l.data(), r.data(), 8, ctx(), {});
    CHECK(std::abs(l[0] - before) < 0.05f);   // first sample after the change has barely moved
}
