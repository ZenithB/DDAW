#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("mono"); }

int idx(InstrumentDevice& d, const char* key) { return findParam(d.params(), key); }

std::vector<float> render(InstrumentDevice& d, int blocks) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}
}  // namespace

TEST_CASE("mono: device contract", "[device][mono]") { checkInstrumentContract(make); }

TEST_CASE("mono: pitch via zero crossings (sine, open filter)", "[device][mono]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    for (auto [k, v] : std::vector<std::pair<const char*, float>>{{"wave", 3}, {"cutoff", 8000}, {"envAmt", 0}, {"res", 0}, {"glide", 0}, {"sustain", 1}, {"attack", 0.001f}})
        d->setParam(uint16_t(idx(*d, k)), v);
    d->noteOn(69, 1.0f, 1);                                  // A4 = 440 Hz
    auto x = render(*d, 345);                                // ~1 s
    int zc = 0; for (size_t i = 1; i < x.size(); ++i) zc += (x[i - 1] >= 0) != (x[i] >= 0);
    CHECK(zc == Catch::Approx(880.0).margin(12));
}

TEST_CASE("mono: release ends the voice, and a stale noteOff does not", "[device][mono]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(uint16_t(idx(*d, "sustain")), 0.8f);
    d->setParam(uint16_t(idx(*d, "release")), 0.05f);
    d->noteOn(60, 1.0f, 1);
    d->noteOn(64, 1.0f, 2);                                  // legato retrigger: note 1 is superseded
    render(*d, 20);
    d->noteOff(1);                                           // stale id: the voice must keep sounding
    CHECK(harness::rms(render(*d, 10)) > 0.01);
    d->noteOff(2);                                           // the sounding note's own off releases
    render(*d, 60);                                          // > 5x the 50 ms release
    CHECK(harness::rms(render(*d, 10)) == 0.0);
}

TEST_CASE("mono: slope selects 12/24/48 dB filter rolloff", "[device][mono]") {
    auto level = [](float slope) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        d->setParam(uint16_t(idx(*d, "wave")), 0);           // saw: lots of high harmonics
        d->setParam(uint16_t(idx(*d, "cutoff")), 300);
        d->setParam(uint16_t(idx(*d, "envAmt")), 0);
        d->setParam(uint16_t(idx(*d, "res")), 0);
        d->setParam(uint16_t(idx(*d, "sustain")), 1);
        d->setParam(uint16_t(idx(*d, "slope")), slope);
        d->noteOn(81, 1.0f, 1);                              // A5 = 880 Hz, well above the cutoff
        render(*d, 20);
        return harness::rms(render(*d, 60));
    };
    const double s12 = level(0), s24 = level(1), s48 = level(2);
    CHECK(s12 > s24);
    CHECK(s24 > s48);
}
