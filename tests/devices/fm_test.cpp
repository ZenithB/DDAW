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
std::unique_ptr<InstrumentDevice> make() { return createInstrument("fm"); }

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

std::unique_ptr<InstrumentDevice> steady(float harm, float modIdx) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "harm", harm); set(*d, "modIdx", modIdx); set(*d, "sustain", 1); set(*d, "attack", 0.001f);
    set(*d, "release", 0.05f);
    return d;
}
}  // namespace

TEST_CASE("fm: device contract", "[device][fm]") { checkInstrumentContract(make); }

TEST_CASE("fm: carrier pitch via zero crossings (shallow modulation)", "[device][fm]") {
    auto d = steady(1.0f, 0.5f);
    d->noteOn(69, 1.0f, 1);                                   // A4 = 440 Hz
    render(*d, 120);
    auto x = render(*d, 345);
    int zc = 0; for (size_t i = 1; i < x.size(); ++i) zc += (x[i - 1] >= 0) != (x[i] >= 0);
    CHECK(zc == Catch::Approx(880.0).margin(20));
}

TEST_CASE("fm: sidebands sit at carrier +/- modulator and grow with modIdx", "[device][fm]") {
    // fc = 220 Hz, harm 3 -> fm = 660 Hz: sidebands at 880 (fc+fm) and 440 (|fc-fm|, folded)
    auto spectrum = [](float modIdx) {
        auto d = steady(3.0f, modIdx);
        d->noteOn(57, 1.0f, 1);
        render(*d, 250);                                      // let the modulation envelope reach its sustain
        return render(*d, 345);
    };
    auto deep = spectrum(20.0f), shallow = spectrum(0.5f);
    const double off = bin(deep, 550.0);                      // no carrier/sideband component here
    CHECK(bin(deep, 880.0) > 10.0 * off);
    CHECK(bin(deep, 440.0) > 10.0 * off);
    CHECK(bin(deep, 880.0) > 8.0 * bin(shallow, 880.0));
    CHECK(bin(shallow, 220.0) > 0.1);                         // shallow FM is nearly a pure carrier
}

TEST_CASE("fm: two notes are both audible and the 13th steals the oldest", "[device][fm]") {
    auto d = steady(1.0f, 0.5f);
    d->noteOn(57, 1.0f, 1); d->noteOn(64, 1.0f, 2);
    render(*d, 120);
    auto two = render(*d, 345);
    CHECK(bin(two, hz(57)) > 10.0 * bin(two, 275.0));
    CHECK(bin(two, hz(64)) > 10.0 * bin(two, 275.0));
    d->reset();

    set(*d, "release", 4.0f);
    for (int i = 0; i < 12; ++i) d->noteOn(uint8_t(48 + 2 * i), 0.4f, uint32_t(i + 1));
    render(*d, 120);
    auto before = render(*d, 345);
    const double oldest = bin(before, hz(48));
    CHECK(oldest > 0.02);
    d->noteOn(81, 0.4f, 13);                                  // pool of 12 is full: steals note 1
    render(*d, 120);
    auto after = render(*d, 345);
    CHECK(bin(after, hz(48)) < 0.1 * oldest);
    CHECK(bin(after, hz(50)) > 0.02);
    CHECK(bin(after, hz(81)) > 0.02);
    d->noteOff(1);                                            // stale id: the stealer keeps sounding
    render(*d, 40);
    CHECK(bin(render(*d, 100), hz(81)) > 0.02);
}

TEST_CASE("fm: release ends the voice; a repeated pitch reuses its voice", "[device][fm]") {
    auto d = steady(3.0f, 10.0f);
    d->noteOn(60, 1.0f, 1);
    d->noteOn(60, 1.0f, 2);                                   // same pitch: one voice, now owned by id 2
    render(*d, 20);
    d->noteOff(1);                                            // superseded id: no effect
    CHECK(harness::rms(render(*d, 10)) > 0.05);
    d->noteOff(2);
    render(*d, 60);                                           // > 5x the 50 ms release
    CHECK(harness::rms(render(*d, 10)) == 0.0);
}
