#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("eq7"); }

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

// Param index of band `b`'s "freq" (+0), "gain" (+1) or "q" (+2).
uint16_t at(EffectDevice& d, int band, const char* what) {
    const std::string key = "b" + std::to_string(band) + "_" + what;
    return uint16_t(findParam(d.params(), key));
}

std::unique_ptr<EffectDevice> with(int band, float gain, float freq = 0.0f, float q = 0.0f) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(at(*d, band, "gain"), gain);
    if (freq > 0) d->setParam(at(*d, band, "freq"), freq);
    if (q > 0) d->setParam(at(*d, band, "q"), q);
    d->reset();
    return d;
}
}  // namespace

TEST_CASE("eq7: device contract", "[device][eq7]") { checkEffectContract(make); }

TEST_CASE("eq7: parameter table is laid out band-major (freq, gain, q)", "[device][eq7]") {
    auto d = make();
    CHECK(d->params().size() == 21);
    for (int b = 0; b < 7; ++b) {
        CHECK(at(*d, b, "freq") == 3 * b);
        CHECK(at(*d, b, "gain") == 3 * b + 1);
        CHECK(at(*d, b, "q") == 3 * b + 2);
    }
}

TEST_CASE("eq7: flat settings are exactly transparent", "[device][eq7]") {
    auto d = with(3, 0.0f);
    for (double f : {50.0, 1000.0, 15000.0}) CHECK(gainDb(*d, f) == Catch::Approx(0.0).margin(0.05));
}

TEST_CASE("eq7: peaking band hits its gain at the centre and fades away from it", "[device][eq7]") {
    auto up = with(3, 9.0f);                                   // 1 kHz, Q 1
    CHECK(gainDb(*up, 1000) == Catch::Approx(9.0).margin(0.2));
    CHECK(gainDb(*up, 50) == Catch::Approx(0.0).margin(0.3));
    CHECK(gainDb(*up, 15000) == Catch::Approx(0.0).margin(0.5));
    auto down = with(3, -9.0f);
    CHECK(gainDb(*down, 1000) == Catch::Approx(-9.0).margin(0.2));
    auto narrow = with(3, 9.0f, 0.0f, 8.0f);                   // high Q: one octave away is nearly untouched
    CHECK(gainDb(*narrow, 2000) < 1.5);
    auto wide = with(3, 9.0f, 0.0f, 0.5f);
    CHECK(gainDb(*wide, 2000) > 4.0);
}

TEST_CASE("eq7: peaking frequency moves the centre", "[device][eq7]") {
    auto d = with(3, 9.0f, 3000.0f);
    CHECK(gainDb(*d, 3000) == Catch::Approx(9.0).margin(0.3));
    CHECK(gainDb(*d, 1000) < 3.0);
}

TEST_CASE("eq7: low shelf lifts the bottom, high shelf lifts the top", "[device][eq7]") {
    auto lo = with(0, 9.0f);                                   // shelf at 60 Hz
    CHECK(gainDb(*lo, 20) == Catch::Approx(9.0).margin(0.5));
    CHECK(gainDb(*lo, 60) == Catch::Approx(4.5).margin(0.7));  // half the gain at the corner
    CHECK(gainDb(*lo, 5000) == Catch::Approx(0.0).margin(0.2));
    auto hi = with(6, 6.0f);                                   // shelf at 12 kHz
    CHECK(gainDb(*hi, 20000) == Catch::Approx(6.0).margin(0.7));
    CHECK(gainDb(*hi, 12000) == Catch::Approx(3.0).margin(0.7));
    CHECK(gainDb(*hi, 200) == Catch::Approx(0.0).margin(0.2));
}

TEST_CASE("eq7: bands are in series (gains add where they overlap)", "[device][eq7]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(at(*d, 3, "gain"), 6.0f);
    d->setParam(at(*d, 3, "freq"), 1000.0f);
    d->setParam(at(*d, 4, "gain"), 6.0f);
    d->setParam(at(*d, 4, "freq"), 1000.0f);
    d->reset();
    CHECK(gainDb(*d, 1000) == Catch::Approx(12.0).margin(0.3));
}
