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
std::unique_ptr<EffectDevice> make() { return createEffect("vib"); }
int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

void processAll(EffectDevice& d, std::vector<float>& l, std::vector<float>& r) {
    for (size_t i = 0; i < l.size(); i += size_t(kMaxBlock))
        d.process(l.data() + i, r.data() + i, kMaxBlock, ctx(), {});
}

// Ramp through a wet-only vibrato: d(n) = n - y[n]/c.
std::pair<std::vector<double>, std::vector<double>> measureDelays(float rate, float depth, size_t n) {
    auto d = make();
    set(*d, "rate", rate); set(*d, "depth", depth); set(*d, "mix", 1.0f);
    d->prepare(kSr, kMaxBlock);
    const float c = 1e-3f;
    std::vector<float> l(n), r(n);
    for (size_t i = 0; i < n; ++i) l[i] = r[i] = c * float(i);
    processAll(*d, l, r);
    std::vector<double> dl(n), dr(n);
    for (size_t i = 0; i < n; ++i) {
        dl[i] = double(i) - double(l[i]) / c;
        dr[i] = double(i) - double(r[i]) / c;
    }
    return {dl, dr};
}
}  // namespace

TEST_CASE("vib: device contract", "[device][vib]") { checkEffectContract(make); }

TEST_CASE("vib: depth 0 is a static 2.5 ms delay", "[device][vib]") {
    auto d = make();
    set(*d, "depth", 0.0f); set(*d, "mix", 1.0f);
    d->prepare(kSr, kMaxBlock);
    std::vector<float> l(2048, 0.0f), r(2048, 0.0f);
    l[0] = r[0] = 1.0f;
    processAll(*d, l, r);
    const size_t peak = size_t(std::max_element(l.begin(), l.end(), [](float a, float b) { return std::abs(a) < std::abs(b); }) - l.begin());
    CHECK(std::abs(double(peak) - 0.0025 * kSr) <= 2.0);          // 110.25 samples
    CHECK(l[peak] > 0.6f);
}

TEST_CASE("vib: delay follows maxDelay/2*(1+depth*sin(2 pi f t - pi/2)) on both channels", "[device][vib]") {
    const size_t n = size_t(kSr);
    auto [dl, dr] = measureDelays(6.0f, 0.4f, n);
    const double center = 0.0025 * kSr;
    double mn = 1e9, mx = -1e9;
    for (size_t i = 400; i < n; ++i) { mn = std::min(mn, dl[i]); mx = std::max(mx, dl[i]); }
    CHECK(mn == Catch::Approx(center * 0.6).margin(0.5));
    CHECK(mx == Catch::Approx(center * 1.4).margin(0.5));
    for (size_t i = 400; i < n; i += 89) {
        const double expect = center * (1.0 + 0.4 * std::sin(2.0 * M_PI * 6.0 * double(i) / kSr - M_PI / 2.0));
        CHECK(dl[i] == Catch::Approx(expect).margin(0.05));
        CHECK(dr[i] == dl[i]);                                    // shared LFO, no stereo spread
    }
    // starts at the shortest delay (LFO phase -90 deg): first measurable sample sits at the minimum
    CHECK(dl[400] < center);
}

TEST_CASE("vib: modulation rate (6 Hz = 12 centre crossings per second)", "[device][vib]") {
    const size_t n = size_t(kSr);
    auto [dl, dr] = measureDelays(6.0f, 0.6f, n);
    const double center = 0.0025 * kSr;
    int crossings = 0;
    for (size_t i = 401; i < n; ++i) crossings += (dl[i - 1] - center) * (dl[i] - center) < 0;
    CHECK(crossings >= 11);
    CHECK(crossings <= 12);
}

TEST_CASE("vib: mix 0 is dry", "[device][vib]") {
    auto d = make();
    set(*d, "mix", 0.0f);
    d->prepare(kSr, kMaxBlock);
    std::vector<float> l(kMaxBlock, 0.3f), r(kMaxBlock, -0.2f);
    d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
    for (float x : l) CHECK(x == Catch::Approx(0.3f).margin(1e-6));
    for (float x : r) CHECK(x == Catch::Approx(-0.2f).margin(1e-6));
}

namespace {
// Centre-crossings of the measured delay for a ramp through the device in the given rate mode.
int rateModeCrossings(float mode, float rateSync, float rateHi, float rateLo, double bpm, size_t n) {
    auto d = make();
    set(*d, "rateMode", mode); set(*d, "rateSync", rateSync); set(*d, "rateHi", rateHi); set(*d, "rate", rateLo);
    set(*d, "depth", 0.5f); set(*d, "mix", 1.0f);
    d->prepare(kSr, kMaxBlock);
    const float c = 1e-3f;
    std::vector<float> l(n), r(n);
    for (size_t i = 0; i < n; ++i) l[i] = r[i] = c * float(i);
    ProcessContext pc = ctx(); pc.bpm = bpm;
    for (size_t i = 0; i < n; i += size_t(kMaxBlock)) d->process(l.data() + i, r.data() + i, kMaxBlock, pc, {});
    const double center = 0.0025 * kSr;
    int crossings = 0;
    double prev = double(399) - double(l[399]) / c - center;
    for (size_t i = 400; i < n; ++i) {
        const double cur = double(i) - double(l[i]) / c - center;
        crossings += prev * cur < 0;
        prev = cur;
    }
    return crossings;
}
}  // namespace

TEST_CASE("vib: Sync mode follows bpm (1/4 = 2 Hz at 120 bpm, doubles at 240 bpm)", "[device][vib]") {
    const size_t n = size_t(kSr);
    const int a = rateModeCrossings(0, 5, 440, 7, 120.0, n);      // 2 Hz: 4 crossings per second
    const int b = rateModeCrossings(0, 5, 440, 7, 240.0, n);      // 4 Hz
    const int c = rateModeCrossings(0, 6, 440, 7, 120.0, n);      // 1/8 = 4 Hz
    CHECK(a >= 3); CHECK(a <= 4);
    CHECK(b >= 7); CHECK(b <= 8);
    CHECK(c >= 7); CHECK(c <= 8);
}

TEST_CASE("vib: High mode uses rateHi, Low mode ignores it", "[device][vib]") {
    const size_t n = size_t(kSr / 2);
    const int hi = rateModeCrossings(2, 5, 440, 7, 120.0, n);     // 440 Hz over 0.5 s: ~440 crossings
    CHECK(hi >= 430); CHECK(hi <= 441);
    const int lo = rateModeCrossings(1, 5, 800, 4, 120.0, size_t(kSr));   // 4 Hz: 8 crossings
    CHECK(lo >= 7); CHECK(lo <= 8);
}
