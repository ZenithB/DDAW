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
std::unique_ptr<EffectDevice> make() { return createEffect("autopan"); }
int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

void run(float rate, float depth, std::vector<float>& l, std::vector<float>& r) {
    auto d = make();
    set(*d, "rate", rate); set(*d, "depth", depth);
    d->prepare(kSr, kMaxBlock);
    for (size_t i = 0; i < l.size(); i += size_t(kMaxBlock)) d->process(l.data() + i, r.data() + i, kMaxBlock, ctx(), {});
}
}  // namespace

TEST_CASE("autopan: device contract", "[device][autopan]") { checkEffectContract(make); }

TEST_CASE("autopan: depth 0 is passthrough (no mix knob, always wet)", "[device][autopan]") {
    std::vector<float> l(1024, 0.25f), r(1024, -0.5f);
    run(3.0f, 0.0f, l, r);
    // pan 0 goes through cos(pi/2) in float (~-4e-8), as in the Rust port: passthrough to ~1e-6
    for (float x : l) CHECK(x == Catch::Approx(0.25f).margin(1e-6));
    for (float x : r) CHECK(x == Catch::Approx(-0.5f).margin(1e-6));
}

TEST_CASE("autopan: Web Audio stereo pan law follows depth*sin(2 pi f t)", "[device][autopan]") {
    const size_t n = size_t(kSr / 2);
    std::vector<float> l(n, 0.25f), r(n, 0.5f);
    run(4.0f, 0.7f, l, r);
    for (size_t i = 0; i < n; i += 53) {
        const double pan = 0.7 * std::sin(2.0 * M_PI * 4.0 * double(i) / kSr);
        double el, er;
        if (pan <= 0.0) { const double x = (pan + 1.0) * M_PI / 2.0; el = 0.25 + 0.5 * std::cos(x); er = 0.5 * std::sin(x); }
        else { const double x = pan * M_PI / 2.0; el = 0.25 * std::cos(x); er = 0.5 + 0.25 * std::sin(x); }
        CHECK(l[i] == Catch::Approx(el).margin(1e-4));
        CHECK(r[i] == Catch::Approx(er).margin(1e-4));
    }
}

TEST_CASE("autopan: a left-only signal alternates sides at the LFO rate", "[device][autopan]") {
    // 2 Hz, full depth: pan > 0 in the first half cycle (R receives the signal), pan <= 0 in the second
    // half (everything stays left, R is silent).
    const size_t n = size_t(kSr);
    std::vector<float> l(n, 1.0f), r(n, 0.0f);
    run(2.0f, 1.0f, l, r);
    const size_t quarter = size_t(kSr / 8.0);                     // phase 0.25: pan = +1
    CHECK(l[quarter] < 0.001f);
    CHECK(r[quarter] == Catch::Approx(1.0f).margin(0.001));
    const size_t threeQ = size_t(kSr / 8.0 * 3.0);                // phase 0.75: pan = -1
    CHECK(l[threeQ] == Catch::Approx(1.0f).margin(0.001));
    CHECK(r[threeQ] < 0.001f);
    int swaps = 0;                                                // R active <-> silent: 2 per cycle, 4 cycles... 2 Hz = 2 cycles
    for (size_t i = 1; i < n; ++i) swaps += (r[i - 1] > 1e-6f) != (r[i] > 1e-6f);
    CHECK(swaps >= 3);
    CHECK(swaps <= 5);
}

TEST_CASE("autopan: identical channels give anti-correlated envelopes", "[device][autopan]") {
    const size_t n = size_t(kSr * 2);
    std::vector<float> l(n), r(n);
    for (size_t i = 0; i < n; ++i) l[i] = r[i] = 0.5f * float(std::sin(2.0 * M_PI * 1000.0 * double(i) / kSr));
    run(2.0f, 1.0f, l, r);
    const size_t win = 220;
    std::vector<double> el, er;
    for (size_t i = size_t(kSr / 2); i + win <= n; i += win) {
        el.push_back(harness::rms(std::span<const float>(l).subspan(i, win)));
        er.push_back(harness::rms(std::span<const float>(r).subspan(i, win)));
    }
    double ma = 0, mb = 0; for (size_t i = 0; i < el.size(); ++i) { ma += el[i]; mb += er[i]; }
    ma /= double(el.size()); mb /= double(el.size());
    double num = 0, da = 0, db = 0;
    for (size_t i = 0; i < el.size(); ++i) { num += (el[i] - ma) * (er[i] - mb); da += (el[i] - ma) * (el[i] - ma); db += (er[i] - mb) * (er[i] - mb); }
    CHECK(num / std::sqrt(da * db) < -0.8);
}

namespace {
// Number of R active <-> silent switches of a left-only DC signal (2 per LFO cycle).
int sideSwitches(float mode, float rateSync, float rateHi, float rateLo, double bpm, size_t n) {
    auto d = make();
    set(*d, "rateMode", mode); set(*d, "rateSync", rateSync); set(*d, "rateHi", rateHi); set(*d, "rate", rateLo);
    set(*d, "depth", 1.0f);
    d->prepare(kSr, kMaxBlock);
    std::vector<float> l(n, 1.0f), r(n, 0.0f);
    ProcessContext pc = ctx(); pc.bpm = bpm;
    for (size_t i = 0; i < n; i += size_t(kMaxBlock)) d->process(l.data() + i, r.data() + i, kMaxBlock, pc, {});
    int c = 0;
    for (size_t i = 1; i < n; ++i) c += (r[i - 1] > 1e-6f) != (r[i] > 1e-6f);
    return c;
}
}  // namespace

TEST_CASE("autopan: Sync mode follows bpm (1/4 = 2 Hz at 120 bpm, doubles at 240 bpm)", "[device][autopan]") {
    const size_t n = size_t(kSr);
    const int a = sideSwitches(0, 5, 440, 9, 120.0, n);           // 2 Hz: 4 switches per second
    const int b = sideSwitches(0, 5, 440, 9, 240.0, n);           // 4 Hz
    CHECK(a >= 3); CHECK(a <= 5);
    CHECK(b >= 7); CHECK(b <= 9);
}

TEST_CASE("autopan: High mode uses rateHi, Low mode ignores it", "[device][autopan]") {
    const int hi = sideSwitches(2, 5, 440, 3, 120.0, size_t(kSr / 4));        // 440 Hz over 0.25 s: ~220
    CHECK(hi >= 215); CHECK(hi <= 221);
    const int lo = sideSwitches(1, 5, 800, 4, 120.0, size_t(kSr));
    CHECK(lo >= 7); CHECK(lo <= 9);
}
