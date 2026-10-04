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
std::unique_ptr<InstrumentDevice> make() { return createInstrument("poly"); }

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

// Single-bin DFT magnitude over the whole buffer, normalised to a sine amplitude.
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

std::unique_ptr<InstrumentDevice> sine() {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "wave", 3); set(*d, "cutoff", 14000); set(*d, "res", 0); set(*d, "sustain", 1);
    set(*d, "attack", 0.001f);
    return d;
}
}  // namespace

TEST_CASE("poly: device contract", "[device][poly]") { checkInstrumentContract(make); }

TEST_CASE("poly: pitch via zero crossings (sine, open filter)", "[device][poly]") {
    auto d = sine();
    d->noteOn(69, 1.0f, 1);                                  // A4 = 440 Hz
    render(*d, 40);
    auto x = render(*d, 345);                                // ~1 s, settled
    int zc = 0; for (size_t i = 1; i < x.size(); ++i) zc += (x[i - 1] >= 0) != (x[i] >= 0);
    CHECK(zc == Catch::Approx(880.0).margin(12));
}

TEST_CASE("poly: a chord has three audible pitches", "[device][poly]") {
    auto d = sine();
    d->noteOn(60, 1.0f, 1); d->noteOn(64, 1.0f, 2); d->noteOn(67, 1.0f, 3);
    render(*d, 40);
    auto x = render(*d, 345);
    const double off = bin(x, 470.0);
    for (int p : {60, 64, 67}) CHECK(bin(x, hz(p)) > 20.0 * off);
}

TEST_CASE("poly: release ends the voice; a stale or unknown id does not", "[device][poly]") {
    auto d = sine();
    set(*d, "release", 0.05f);
    d->noteOn(60, 1.0f, 1);
    d->noteOn(64, 1.0f, 2);
    render(*d, 20);
    d->noteOff(1);                                           // releases only note 1
    render(*d, 40);
    auto mid = render(*d, 20);
    CHECK(bin(mid, hz(60)) < 0.1 * bin(mid, hz(64)));
    CHECK(bin(mid, hz(64)) > 0.5);                           // note 2 still sounding
    d->noteOff(999);                                         // unknown id is harmless
    CHECK(harness::rms(render(*d, 10)) > 0.1);
    d->noteOff(2);
    render(*d, 60);                                          // > 5x the 50 ms release
    CHECK(harness::rms(render(*d, 10)) == 0.0);
}

TEST_CASE("poly: 17th note steals the oldest voice", "[device][poly]") {
    auto d = sine();
    set(*d, "release", 4.0f);
    for (int i = 0; i < 16; ++i) d->noteOn(uint8_t(48 + 2 * i), 0.4f, uint32_t(i + 1));
    render(*d, 80);
    auto before = render(*d, 345);
    const double oldest = bin(before, hz(48));
    CHECK(oldest > 0.05);
    CHECK(bin(before, hz(50)) > 0.05);
    d->noteOn(81, 0.4f, 17);                                  // pool full: steals note 1 (pitch 48)
    render(*d, 80);
    auto after = render(*d, 345);
    CHECK(bin(after, hz(48)) < 0.1 * oldest);                 // oldest voice is gone
    CHECK(bin(after, hz(50)) > 0.05);                         // the others survive
    CHECK(bin(after, hz(81)) > 0.05);                         // the new note sounds
    d->noteOff(1);                                            // stale id must not release the stealer
    render(*d, 40);
    CHECK(bin(render(*d, 100), hz(81)) > 0.05);
}

TEST_CASE("poly: fat waves spread the unison by +-spread/2 cents", "[device][poly]") {
    auto level = [](float spread) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "wave", 4); set(*d, "cutoff", 14000); set(*d, "res", 0); set(*d, "sustain", 1);
        set(*d, "spread", spread);
        d->noteOn(57, 1.0f, 1);                              // 220 Hz
        render(*d, 40);
        auto x = render(*d, 690);                            // ~2 s: 0.5 Hz resolution
        return bin(x, 220.0 * std::pow(2.0, 30.0 / 1200.0));  // the upper unison oscillator at spread 60
    };
    const double flat = level(0), wide = level(60);
    CHECK(wide > 0.05);
    CHECK(wide > 5.0 * flat);
}

TEST_CASE("poly: slope selects 12/24/48 dB filter rolloff", "[device][poly]") {
    auto level = [](float slope) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "wave", 0); set(*d, "cutoff", 300); set(*d, "res", 0); set(*d, "sustain", 1);
        set(*d, "slope", slope);
        d->noteOn(81, 1.0f, 1);                              // 880 Hz, above the cutoff
        render(*d, 80);                                      // let the 30 ms cutoff smoother settle
        return harness::rms(render(*d, 60));
    };
    const double s12 = level(0), s24 = level(1), s48 = level(2);
    CHECK(s12 > s24);
    CHECK(s24 > s48);
}
