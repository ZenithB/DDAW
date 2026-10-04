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
std::unique_ptr<EffectDevice> make() { return createEffect("chorus"); }
int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

void processAll(EffectDevice& d, std::vector<float>& l, std::vector<float>& r) {
    for (size_t i = 0; i < l.size(); i += size_t(kMaxBlock))
        d.process(l.data() + i, r.data() + i, kMaxBlock, ctx(), {});
}

// A linear ramp x[n] = c*n through a wet-only device reads back the delay directly:
// y[n] = x[n - d(n)]  =>  d(n) = n - y[n]/c.
std::pair<std::vector<double>, std::vector<double>> measureDelays(float rate, float depth, size_t n) {
    auto d = make();
    set(*d, "rate", rate); set(*d, "depth", depth); set(*d, "mix", 1.0f);
    d->prepare(kSr, kMaxBlock);                       // prepare snaps the smoothers to the targets
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

TEST_CASE("chorus: device contract", "[device][chorus]") { checkEffectContract(make); }

TEST_CASE("chorus: depth 0 is a static 3 ms delay", "[device][chorus]") {
    auto d = make();
    set(*d, "depth", 0.0f); set(*d, "mix", 1.0f);
    d->prepare(kSr, kMaxBlock);
    std::vector<float> l(4096, 0.0f), r(4096, 0.0f);
    l[0] = r[0] = 1.0f;
    processAll(*d, l, r);
    const size_t peak = size_t(std::max_element(l.begin(), l.end(), [](float a, float b) { return std::abs(a) < std::abs(b); }) - l.begin());
    const double expect = 0.003 * kSr;                           // 132.3 samples
    CHECK(std::abs(double(peak) - expect) <= 2.0);
    CHECK(l[peak] > 0.6f);
    CHECK(l == r);                                                // no modulation: channels identical
}

TEST_CASE("chorus: delay sweeps centre +- centre*depth at the LFO rate", "[device][chorus]") {
    const size_t n = size_t(kSr);                                 // 1 s, 2 Hz
    auto [dl, dr] = measureDelays(2.0f, 0.5f, n);
    const double center = 0.003 * kSr;
    double mn = 1e9, mx = -1e9;
    for (size_t i = 400; i < n; ++i) { mn = std::min(mn, dl[i]); mx = std::max(mx, dl[i]); }
    CHECK(mn == Catch::Approx(center * 0.5).margin(0.5));
    CHECK(mx == Catch::Approx(center * 1.5).margin(0.5));
    // exact LFO law: d(n) = centre + centre*depth*sin(2 pi f n / sr)
    for (size_t i = 400; i < n; i += 97)
        CHECK(dl[i] == Catch::Approx(center + center * 0.5 * std::sin(2.0 * M_PI * 2.0 * double(i) / kSr)).margin(0.05));
    // two sweeps per second: 4 crossings of the centre delay
    int crossings = 0;
    for (size_t i = 401; i < n; ++i) crossings += (dl[i - 1] - center) * (dl[i] - center) < 0;
    CHECK(crossings >= 3);
    CHECK(crossings <= 4);
}

TEST_CASE("chorus: right channel modulates in antiphase", "[device][chorus]") {
    auto [dl, dr] = measureDelays(3.0f, 0.8f, size_t(kSr / 2));
    const double center = 0.003 * kSr;
    for (size_t i = 400; i < dl.size(); i += 53) CHECK((dl[i] + dr[i]) / 2.0 == Catch::Approx(center).margin(0.1));
    CHECK(std::abs(dl[5000] - dr[5000]) > 5.0);
}

TEST_CASE("chorus: mix 0 is dry", "[device][chorus]") {
    auto d = make();
    set(*d, "mix", 0.0f);
    d->prepare(kSr, kMaxBlock);
    std::vector<float> l(kMaxBlock, 0.4f), r(kMaxBlock, -0.4f);
    d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
    for (float x : l) CHECK(x == Catch::Approx(0.4f).margin(1e-6));
    for (float x : r) CHECK(x == Catch::Approx(-0.4f).margin(1e-6));
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
    const double center = 0.003 * kSr;
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

TEST_CASE("chorus: Sync mode follows bpm (1/4 = 2 Hz at 120 bpm, doubles at 240 bpm)", "[device][chorus]") {
    const size_t n = size_t(kSr);
    const int a = rateModeCrossings(0, 5, 440, 7, 120.0, n);      // 2 Hz: 4 crossings per second
    const int b = rateModeCrossings(0, 5, 440, 7, 240.0, n);      // 4 Hz
    const int c = rateModeCrossings(0, 6, 440, 7, 120.0, n);      // 1/8 = 4 Hz
    CHECK(a >= 3); CHECK(a <= 4);
    CHECK(b >= 7); CHECK(b <= 8);
    CHECK(c >= 7); CHECK(c <= 8);
}

TEST_CASE("chorus: High mode uses rateHi, Low mode ignores it", "[device][chorus]") {
    const size_t n = size_t(kSr / 2);
    const int hi = rateModeCrossings(2, 5, 440, 7, 120.0, n);     // 440 Hz over 0.5 s: ~440 crossings
    CHECK(hi >= 430); CHECK(hi <= 441);
    const int lo = rateModeCrossings(1, 5, 800, 4, 120.0, size_t(kSr));   // 4 Hz: 8 crossings
    CHECK(lo >= 7); CHECK(lo <= 8);
}
