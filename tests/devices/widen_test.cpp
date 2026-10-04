#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("widen"); }

// One settled block of constant l/r through the device at the given width.
std::pair<float, float> run(float width, float l0, float r0) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(0, width); d->reset();
    std::vector<float> l(kMaxBlock, l0), r(kMaxBlock, r0);
    d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
    return {l.back(), r.back()};
}
}  // namespace

TEST_CASE("widen: device contract", "[device][widen]") { checkEffectContract(make); }

TEST_CASE("widen: width 0.5 is unity", "[device][widen]") {
    auto [l, r] = run(0.5f, 0.7f, -0.2f);
    CHECK(l == Catch::Approx(0.7f).margin(1e-6));
    CHECK(r == Catch::Approx(-0.2f).margin(1e-6));
}

TEST_CASE("widen: width 0 keeps only the mid (doubled), width 1 only the side", "[device][widen]") {
    auto [l0, r0] = run(0.0f, 0.3f, 0.1f);          // mid * 2: L = R = l + r
    CHECK(l0 == Catch::Approx(0.4f).margin(1e-6));
    CHECK(r0 == Catch::Approx(0.4f).margin(1e-6));
    auto [l1, r1] = run(1.0f, 0.3f, 0.1f);          // side * 2: L = l - r, R = -(l - r)
    CHECK(l1 == Catch::Approx(0.2f).margin(1e-6));
    CHECK(r1 == Catch::Approx(-0.2f).margin(1e-6));
}

TEST_CASE("widen: mono input vanishes at full width and is doubled at zero width", "[device][widen]") {
    auto [l1, r1] = run(1.0f, 0.5f, 0.5f);          // the fx-widen golden is silent for this reason
    CHECK(l1 == 0.0f);
    CHECK(r1 == 0.0f);
    auto [l0, r0] = run(0.0f, 0.5f, 0.5f);
    CHECK(l0 == Catch::Approx(1.0f).margin(1e-6));
    CHECK(r0 == Catch::Approx(1.0f).margin(1e-6));
}

TEST_CASE("widen: general mid/side maths for an arbitrary width", "[device][widen]") {
    const float w = 0.8f, l = 0.6f, r = -0.1f;
    const float mid = (l + r) * 0.5f * 2.0f * (1.0f - w), side = (l - r) * 0.5f * 2.0f * w;
    auto [lo, ro] = run(w, l, r);
    CHECK(lo == Catch::Approx(mid + side).margin(1e-6));
    CHECK(ro == Catch::Approx(mid - side).margin(1e-6));
}

TEST_CASE("widen: width changes are smoothed, not stepped", "[device][widen]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(0, 0.0f); d->reset();
    d->setParam(0, 1.0f);
    std::vector<float> l(kMaxBlock, 0.5f), r(kMaxBlock, 0.5f);
    d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
    CHECK(l.front() > 0.9f);                         // still near the old (doubled-mono) value
    CHECK(l.back() < l.front());                     // gliding toward silence
    CHECK(l.back() > 0.0f);
}
