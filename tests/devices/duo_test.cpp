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
std::unique_ptr<InstrumentDevice> make() { return createInstrument("duo"); }

int idx(InstrumentDevice& d, const char* key) { return findParam(d.params(), key); }
void set(InstrumentDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

std::vector<float> render(InstrumentDevice& d, int blocks) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

double bin(const std::vector<float>& x, double hz) {
    double re = 0, im = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double w = 2.0 * std::numbers::pi * hz * double(i) / kSr;
        re += x[i] * std::cos(w);
        im -= x[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / double(x.size());
}
double hz(int pitch) { return 440.0 * std::pow(2.0, (pitch - 69) / 12.0); }

// Steady-state device: no vibrato, sustain 1 so the amp envelope is flat after the attack.
std::unique_ptr<InstrumentDevice> steady(float harm) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "harm", harm); set(*d, "vibAmt", 0); set(*d, "sustain", 1); set(*d, "attack", 0.001f);
    set(*d, "release", 0.05f);
    return d;
}
}  // namespace

TEST_CASE("duo: device contract", "[device][duo]") { checkInstrumentContract(make); }

TEST_CASE("duo: second layer sits at harm x the base pitch", "[device][duo]") {
    auto d = steady(1.5f);
    d->noteOn(57, 1.0f, 1);                                   // 220 Hz; layers at 220 and 330 Hz
    render(*d, 60);
    auto x = render(*d, 345);
    const double off = bin(x, 275.0);                         // between the layers, no harmonic of either
    CHECK(bin(x, 220.0) > 10.0 * off);
    CHECK(bin(x, 330.0) > 10.0 * off);
}

TEST_CASE("duo: vibrato adds sidebands at vibRate, none when vibAmt is 0", "[device][duo]") {
    auto sideband = [](float amt) {
        auto d = steady(1.0f);
        set(*d, "vibAmt", amt); set(*d, "vibRate", 5.0f);
        d->noteOn(57, 1.0f, 1);
        render(*d, 120);                                      // settle past the filter envelope and smoothers
        auto x = render(*d, 690);                             // ~2 s: integer vibrato cycles, 0.5 Hz bins
        return bin(x, 225.0) / bin(x, 220.0);                 // first upper sideband / carrier
    };
    CHECK(sideband(0.0f) < 0.02);
    CHECK(sideband(0.6f) > 0.2);
}

TEST_CASE("duo: two notes are both audible and the 7th steals the oldest", "[device][duo]") {
    auto d = steady(1.0f);
    d->noteOn(57, 1.0f, 1); d->noteOn(64, 1.0f, 2);
    render(*d, 60);
    auto two = render(*d, 345);
    CHECK(bin(two, hz(57)) > 10.0 * bin(two, 275.0));
    CHECK(bin(two, hz(64)) > 10.0 * bin(two, 275.0));
    d->reset();

    set(*d, "release", 4.0f);
    d->noteOn(80, 0.5f, 1);                                   // oldest, 831 Hz
    for (int i = 0; i < 5; ++i) d->noteOn(uint8_t(45 + 4 * i + (i > 2)), 0.5f, uint32_t(i + 2));
    render(*d, 60);
    auto before = render(*d, 345);
    const double top = bin(before, hz(80));
    CHECK(top > 0.02);
    d->noteOn(66, 0.5f, 7);                                   // pool of 6 is full: steals note 1
    render(*d, 60);
    auto after = render(*d, 345);
    CHECK(bin(after, hz(80)) < 0.1 * top);
    CHECK(bin(after, hz(66)) > 0.02);
    d->noteOff(1);                                            // stale id: the stealer keeps sounding
    render(*d, 40);
    CHECK(bin(render(*d, 100), hz(66)) > 0.02);
}

TEST_CASE("duo: release ends the voice; a repeated pitch reuses its voice", "[device][duo]") {
    auto d = steady(1.5f);
    d->noteOn(60, 1.0f, 1);
    d->noteOn(60, 1.0f, 2);                                   // same pitch: one voice, now owned by id 2
    render(*d, 20);
    d->noteOff(1);                                            // superseded id: no effect
    CHECK(harness::rms(render(*d, 10)) > 0.05);
    d->noteOff(2);
    render(*d, 60);                                           // > 5x the 50 ms release
    CHECK(harness::rms(render(*d, 10)) == 0.0);
}
