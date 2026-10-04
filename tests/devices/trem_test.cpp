#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("trem"); }
int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

// DC input of 1 with mix 1: the output is the wet gain curve itself.
std::pair<std::vector<float>, std::vector<float>> gainCurve(float rate, float depth, size_t n, float mix = 1.0f) {
    auto d = make();
    set(*d, "rate", rate); set(*d, "depth", depth); set(*d, "mix", mix);
    d->prepare(kSr, kMaxBlock);
    std::vector<float> l(n, 1.0f), r(n, 1.0f);
    for (size_t i = 0; i < n; i += size_t(kMaxBlock)) d->process(l.data() + i, r.data() + i, kMaxBlock, ctx(), {});
    return {l, r};
}
}  // namespace

TEST_CASE("trem: device contract", "[device][trem]") { checkEffectContract(make); }

TEST_CASE("trem: gain swings between (1-depth)/2 and (1+depth)/2", "[device][trem]") {
    auto [l, r] = gainCurve(4.0f, 0.6f, size_t(kSr));
    CHECK(*std::min_element(l.begin(), l.end()) == Catch::Approx(0.2f).margin(0.002));
    CHECK(*std::max_element(l.begin(), l.end()) == Catch::Approx(0.8f).margin(0.002));
    auto [l1, r1] = gainCurve(4.0f, 1.0f, size_t(kSr));
    CHECK(*std::min_element(l1.begin(), l1.end()) < 0.005f);
    CHECK(*std::max_element(l1.begin(), l1.end()) > 0.995f);
}

TEST_CASE("trem: gain follows (1 - depth*sin(2 pi f t + phi))/2 with spread 60 (L 60 deg, R 120 deg)", "[device][trem]") {
    auto [l, r] = gainCurve(5.0f, 0.8f, size_t(kSr / 2));
    for (size_t i = 200; i < l.size(); i += 71) {
        const double t = double(i) / kSr;
        CHECK(l[i] == Catch::Approx(0.5 * (1.0 - 0.8 * std::sin(2.0 * M_PI * (5.0 * t + 60.0 / 360.0)))).margin(1e-4));
        CHECK(r[i] == Catch::Approx(0.5 * (1.0 - 0.8 * std::sin(2.0 * M_PI * (5.0 * t + 120.0 / 360.0)))).margin(1e-4));
    }
}

TEST_CASE("trem: modulation rate (8 Hz = 16 midpoint crossings per second)", "[device][trem]") {
    auto [l, r] = gainCurve(8.0f, 1.0f, size_t(kSr));
    int crossings = 0;
    for (size_t i = 1; i < l.size(); ++i) crossings += (l[i - 1] - 0.5f) * (l[i] - 0.5f) < 0;
    CHECK(crossings >= 15);
    CHECK(crossings <= 16);
}

TEST_CASE("trem: depth 0 pins the wet gain at 0.5; mix 0 is exactly dry", "[device][trem]") {
    auto [l, r] = gainCurve(5.0f, 0.0f, 2048);
    for (size_t i = 0; i < l.size(); ++i) { CHECK(l[i] == Catch::Approx(0.5f).margin(1e-5)); CHECK(r[i] == Catch::Approx(0.5f).margin(1e-5)); }
    auto [l0, r0] = gainCurve(5.0f, 1.0f, 2048, 0.0f);
    for (size_t i = 0; i < l0.size(); ++i) CHECK(l0[i] == Catch::Approx(1.0f).margin(1e-6));
}

namespace {
int midpointCrossings(float mode, float rateSync, float rateHi, float rateLo, double bpm, size_t n) {
    auto d = make();
    set(*d, "rateMode", mode); set(*d, "rateSync", rateSync); set(*d, "rateHi", rateHi); set(*d, "rate", rateLo);
    set(*d, "depth", 1.0f); set(*d, "mix", 1.0f);
    d->prepare(kSr, kMaxBlock);
    std::vector<float> l(n, 1.0f), r(n, 1.0f);
    ProcessContext pc = ctx(); pc.bpm = bpm;
    for (size_t i = 0; i < n; i += size_t(kMaxBlock)) d->process(l.data() + i, r.data() + i, kMaxBlock, pc, {});
    int c = 0;
    for (size_t i = 1; i < n; ++i) c += (l[i - 1] - 0.5f) * (l[i] - 0.5f) < 0;
    return c;
}
}  // namespace

TEST_CASE("trem: Sync mode follows bpm (1/4 = 2 Hz at 120 bpm, doubles at 240 bpm)", "[device][trem]") {
    const size_t n = size_t(kSr);
    const int a = midpointCrossings(0, 5, 440, 9, 120.0, n);      // 2 Hz: 4 midpoint crossings per second
    const int b = midpointCrossings(0, 5, 440, 9, 240.0, n);      // 4 Hz
    const int c = midpointCrossings(0, 3, 440, 9, 120.0, 4 * n);  // 1 bar = 0.5 Hz over 4 s: 4 crossings
    CHECK(a >= 3); CHECK(a <= 4);
    CHECK(b >= 7); CHECK(b <= 8);
    CHECK(c >= 3); CHECK(c <= 4);
}

TEST_CASE("trem: High mode uses rateHi, Low mode ignores it", "[device][trem]") {
    const int hi = midpointCrossings(2, 5, 440, 3, 120.0, size_t(kSr / 2));   // 440 Hz over 0.5 s
    CHECK(hi >= 438); CHECK(hi <= 441);
    const int lo = midpointCrossings(1, 5, 800, 4, 120.0, size_t(kSr));        // 4 Hz
    CHECK(lo >= 7); CHECK(lo <= 8);
}
