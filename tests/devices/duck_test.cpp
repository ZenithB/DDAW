#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

// Test-only trigger path, defined (non-static) in src/devices/effects/duck.cpp until A3 supplies a
// real sidechain input.
namespace ddaw::devices {
void duck_test_trigger(EffectDevice&);
void duck_test_set_trigger_mode(EffectDevice&, bool);
}  // namespace ddaw::devices

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("duck"); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(findParam(d.params(), key)), v); }

constexpr double kTicksPerSec = 192.0;  // 120 bpm, PPQ 96

// Feed a constant 1.0 through in kMaxBlock blocks, the transport advancing from startTicks.
std::vector<float> runOnes(EffectDevice& d, double seconds, double startTicks = 0.0, bool playing = true) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    const size_t total = size_t(seconds * kSr);
    for (size_t t = 0; t < total; t += kMaxBlock) {
        std::fill(l.begin(), l.end(), 1.0f); std::fill(r.begin(), r.end(), 1.0f);
        ProcessContext c = ctx();
        c.positionTicks = startTicks + double(t) / kSr * kTicksPerSec;
        c.playing = playing;
        d.process(l.data(), r.data(), kMaxBlock, c, {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}
}  // namespace

TEST_CASE("duck: device contract", "[device][duck]") { checkEffectContract(make); }

TEST_CASE("duck: tempo mode dips at division boundaries and recovers before the next", "[device][duck]") {
    auto d = make();
    set(*d, "rate", 1.0f); set(*d, "amount", 0.9f); set(*d, "curve", 0.5f);
    d->prepare(kSr, kMaxBlock); d->reset();
    const auto x = runOnes(*d, 1.0);                       // two 0.5 s cycles (1/4 note at 120 bpm)
    auto at = [&](double s) { return x[size_t(s * kSr)]; };
    CHECK(at(0.03) < 0.4f);                                // dip after the cycle start (past the 4 ms slew)
    CHECK(at(0.498) > 0.9f);                               // recovered just before the boundary
    CHECK(at(0.53) < 0.4f);                                // dips again after it
    CHECK(*std::min_element(x.begin(), x.end()) >= 0.1f - 1e-3f);   // floor is 1 - amount
    CHECK(*std::max_element(x.begin(), x.end()) <= 1.0f + 1e-6f);    // never boosts
}

TEST_CASE("duck: depth and recovery shape follow amount and curve", "[device][duck]") {
    // Steady state at the boundary (phase ~0): g -> 1 - amount. Mid-cycle phase p: g = (1-a) + a*p^(0.35+1.6c).
    auto d = make();
    set(*d, "rate", 1.0f); set(*d, "amount", 0.6f); set(*d, "curve", 0.5f);
    d->prepare(kSr, kMaxBlock); d->reset();
    const auto x = runOnes(*d, 2.0, 0.0);
    const double phase = 0.5;                              // 0.25 s into a 0.5 s cycle, 1.25 s into the run
    const double expect = 0.4 + 0.6 * std::pow(phase, 0.35 + 0.5 * 1.6);
    CHECK(x[size_t(1.25 * kSr)] == Catch::Approx(expect).margin(0.02));
    // Larger curve recovers later (lower gain at the same phase).
    auto d2 = make();
    set(*d2, "rate", 1.0f); set(*d2, "amount", 0.6f); set(*d2, "curve", 1.0f);
    d2->prepare(kSr, kMaxBlock); d2->reset();
    CHECK(runOnes(*d2, 2.0)[size_t(1.25 * kSr)] < x[size_t(1.25 * kSr)] - 0.05f);
}

TEST_CASE("duck: amount 0 is a passthrough", "[device][duck]") {
    auto d = make();
    set(*d, "amount", 0.0f); d->prepare(kSr, kMaxBlock); d->reset();
    for (float v : runOnes(*d, 0.5)) REQUIRE(v == Catch::Approx(1.0f).margin(1e-5));
}

TEST_CASE("duck: a stopped transport holds the phase within a block", "[device][duck]") {
    auto d = make();
    set(*d, "rate", 1.0f); set(*d, "amount", 0.9f); set(*d, "curve", 0.5f);
    d->prepare(kSr, kMaxBlock); d->reset();
    // Parked at 48 ticks (phase 0.5 of a 96-tick cycle): after the 4 ms slew settles, the gain is constant.
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    ProcessContext c = ctx(); c.positionTicks = 48.0; c.playing = false;
    for (int b = 0; b < 40; ++b) {
        std::fill(l.begin(), l.end(), 1.0f); std::fill(r.begin(), r.end(), 1.0f);
        d->process(l.data(), r.data(), kMaxBlock, c, {});
    }
    const double expect = 0.1 + 0.9 * std::pow(0.5, 0.35 + 0.5 * 1.6);
    CHECK(l.front() == Catch::Approx(expect).margin(0.01));
    CHECK(l.back() == Catch::Approx(l.front()).margin(1e-4));
}

TEST_CASE("duck: trigger mode idles at unity, dips on trigger, recovers over one cycle", "[device][duck]") {
    auto d = make();
    set(*d, "rate", 1.0f); set(*d, "amount", 0.9f);        // 1/4 = 0.5 s at 120 bpm
    d->prepare(kSr, kMaxBlock); d->reset();
    devices::duck_test_set_trigger_mode(*d, true);

    const auto idle = runOnes(*d, 0.2);
    CHECK(idle.back() > 0.95f);                            // no trigger yet: unity

    devices::duck_test_trigger(*d);
    const auto x = runOnes(*d, 0.5, 0.2 * kTicksPerSec);
    auto at = [&](double s) { return x[size_t(s * kSr)]; };
    CHECK(at(0.03) < 0.4f);                                // dipped
    CHECK(at(0.497) > 0.9f);                               // recovered by the cycle end
    CHECK(at(0.03) < at(0.25));                            // monotone recovery
    CHECK(at(0.25) < at(0.45));

    // A second trigger restarts the dip.
    runOnes(*d, 0.3);
    devices::duck_test_trigger(*d);
    const auto y = runOnes(*d, 0.1);
    CHECK(y[size_t(0.05 * kSr)] < 0.4f);
}

TEST_CASE("duck: trigger on a plain tempo-mode device is ignored", "[device][duck]") {
    auto d = make();
    set(*d, "amount", 0.0f); d->prepare(kSr, kMaxBlock); d->reset();
    devices::duck_test_trigger(*d);                        // tempo mode: trigger state unused
    for (float v : runOnes(*d, 0.2)) REQUIRE(v == Catch::Approx(1.0f).margin(1e-5));
}
