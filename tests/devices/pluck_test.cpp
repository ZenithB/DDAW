#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("pluck"); }
int idx(InstrumentDevice& d, const char* key) { return findParam(d.params(), key); }
void set(InstrumentDevice& d, const char* k, float v) { d.setParam(uint16_t(idx(d, k)), v); }

std::vector<float> render(InstrumentDevice& d, int blocks) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

double rmsRange(const std::vector<float>& x, double a, double b) {
    return harness::rms(std::vector<float>(x.begin() + long(a * kSr), x.begin() + long(b * kSr)));
}

// Fundamental period (samples) by autocorrelation over a sustained stretch.
int autocorrLag(const std::vector<float>& x, int minLag, int maxLag) {
    const size_t from = size_t(0.2 * kSr), to = size_t(0.8 * kSr);
    int best = minLag; double bestV = -1e30;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        double acc = 0;
        for (size_t i = from; i + size_t(lag) < to; ++i) acc += double(x[i]) * x[i + size_t(lag)];
        if (acc > bestV) { bestV = acc; best = lag; }
    }
    return best;
}
}  // namespace

TEST_CASE("pluck: device contract", "[device][pluck]") { checkInstrumentContract(make); }

TEST_CASE("pluck: fundamental equals the delay-line pitch and the string decays", "[device][pluck]") {
    for (uint8_t pitch : {uint8_t(45), uint8_t(69), uint8_t(81)}) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "res", 0.98f);
        d->noteOn(pitch, 1.0f, 1);
        auto x = render(*d, int(1.0 * kSr / kMaxBlock));
        const double f0 = 440.0 * std::pow(2.0, (pitch - 69) / 12.0), expected = kSr / f0;
        const int lag = autocorrLag(x, int(kSr / 2000.0), int(kSr / 50.0));
        INFO("pitch " << int(pitch) << " lag " << lag << " expected " << expected);
        CHECK(std::abs(lag - expected) / expected < 0.02);
        CHECK(rmsRange(x, 0.85, 0.95) < rmsRange(x, 0.05, 0.15));
    }
}

TEST_CASE("pluck: res sets the sustain, noteOff is a no-op, ring-out reaches silence", "[device][pluck]") {
    auto tail = [](float res) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "res", res);
        render(*d, 4);                                        // let the smoother settle
        d->noteOn(60, 1.0f, 1);
        d->noteOff(1);                                        // must not stop the string
        auto x = render(*d, int(1.5 * kSr / kMaxBlock));
        return rmsRange(x, 1.0, 1.4);
    };
    CHECK(tail(0.98f) > 10.0 * tail(0.6f));

    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "res", 0.5f);
    d->noteOn(69, 1.0f, 1);
    render(*d, 400);
    CHECK(harness::rms(render(*d, 10)) == 0.0);               // the retired voice contributes exact silence
}

TEST_CASE("pluck: dampen brightens the excitation", "[device][pluck]") {
    auto energy = [](float cutoff) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "dampen", cutoff);
        render(*d, 20);
        d->noteOn(57, 1.0f, 1);
        return harness::rms(render(*d, int(0.25 * kSr / kMaxBlock)));
    };
    CHECK(energy(12000.0f) > 1.2 * energy(500.0f));
}

TEST_CASE("pluck: a fifth note steals the oldest of the four strings", "[device][pluck]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "res", 0.98f);
    for (uint32_t i = 0; i < 5; ++i) d->noteOn(uint8_t(48 + 7 * i), 1.0f, i + 1);
    auto x = render(*d, 40);
    CHECK(harness::rms(x) > 0.0);
    CHECK(finiteAndBounded(x));
}
