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
std::unique_ptr<InstrumentDevice> make() { return createInstrument("keys"); }
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

double goertzel(const std::vector<float>& x, size_t from, double freq) {
    const double w = 2.0 * std::numbers::pi * freq / kSr, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < x.size(); ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return std::sqrt(std::max(p, 0.0)) / double(x.size() - from);
}
}  // namespace

TEST_CASE("keys: device contract", "[device][keys]") { checkInstrumentContract(make); }

TEST_CASE("keys: harm moves the AM sidebands at f0*(1 +- harm)", "[device][keys]") {
    // f0 = 440. harm 0.5 puts sidebands at 220/660 Hz; harm 3 puts one at 1760 Hz.
    auto analyse = [](float harm) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "harm", harm);
        d->prepare(kSr, kMaxBlock);                           // re-snap the smoother
        set(*d, "harm", harm);
        render(*d, 10);                                       // let the smoother settle before the note
        set(*d, "sustain", 1.0f);
        d->noteOn(69, 1.0f, 1);
        auto x = render(*d, 345);                             // ~1 s, past the 0.5 s modulator attack
        const size_t from = size_t(0.8 * kSr);
        return std::array<double, 3>{goertzel(x, from, 440), goertzel(x, from, 660), goertzel(x, from, 1760)};
    };
    const auto a = analyse(0.5f), b = analyse(3.0f);
    CHECK(a[0] > 0.05); CHECK(b[0] > 0.05);                   // carrier present in both
    CHECK(a[1] > 10.0 * std::max(b[1], 1e-6));                // 660 Hz follows harm = 0.5
    CHECK(b[2] > 10.0 * std::max(a[2], 1e-6));                // 1760 Hz follows harm = 3
}

TEST_CASE("keys: release reaches exact silence, stale id is ignored", "[device][keys]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "release", 0.1f);
    d->noteOn(60, 1.0f, 1);
    d->noteOn(60, 1.0f, 2);                                   // same pitch: reuses the voice, id 2 owns it
    render(*d, 20);
    d->noteOff(1);                                            // superseded id
    CHECK(harness::rms(render(*d, 10)) > 1e-3);
    d->noteOff(2);
    render(*d, 120);
    CHECK(harness::rms(render(*d, 10)) == 0.0);
}

TEST_CASE("keys: 12-voice pool steals the oldest voice", "[device][keys]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "sustain", 1.0f);
    // 13 notes into 12 voices: note 40 (the oldest) is stolen by note 52
    for (uint32_t i = 0; i < 13; ++i) { d->noteOn(uint8_t(40 + i), 1.0f, i + 1); render(*d, 1); }
    // releasing the stolen id must be harmless, and releasing the 12 survivors silences everything
    d->noteOff(1);
    for (uint32_t i = 1; i < 13; ++i) d->noteOff(i + 1);
    set(*d, "release", 0.01f);
    render(*d, 200);
    CHECK(harness::rms(render(*d, 10)) == 0.0);
}
