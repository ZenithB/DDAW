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
std::unique_ptr<EffectDevice> make() { return createEffect("cheby"); }
int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

std::vector<float> sine(EffectDevice& d, int cycles, float amp, int n) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int t = 0; t < n; t += kMaxBlock) {
        for (int i = 0; i < kMaxBlock; ++i)
            l[size_t(i)] = r[size_t(i)] = amp * float(std::sin(2.0 * std::numbers::pi * cycles * double(t + i) / 4096.0));
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}
std::vector<float> constant(EffectDevice& d, float v, int n) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int t = 0; t < n; t += kMaxBlock) {
        std::fill(l.begin(), l.end(), v); std::fill(r.begin(), r.end(), v);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}
double dft(const std::vector<float>& x, size_t start, int cycles) {
    double re = 0, im = 0;
    for (size_t i = 0; i < 4096; ++i) {
        const double ph = 2.0 * std::numbers::pi * cycles * double(i) / 4096.0;
        re += double(x[start + i]) * std::cos(ph);
        im -= double(x[start + i]) * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / 4096.0;
}
}  // namespace

TEST_CASE("cheby: device contract", "[device][cheby]") { checkEffectContract(make); }

TEST_CASE("cheby: DC settles on the Tone recurrence (T0 = 0, Cn = x*U(n-1))", "[device][cheby]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "mix", 1.0f);
    const double x = 0.8;
    struct Case { float order; double expect; };
    // C2 = 2x^2, C3 = 4x^3 - x, C4 = 2x*C3 - C2.
    for (auto c : {Case{2, 2 * x * x}, Case{3, 4 * x * x * x - x}, Case{4, 2 * x * (4 * x * x * x - x) - 2 * x * x}}) {
        set(*d, "order", c.order); d->reset();
        CHECK(double(constant(*d, float(x), kMaxBlock * 20).back()) == Catch::Approx(c.expect).margin(2e-3));
    }
    set(*d, "order", 3.0f); d->reset();
    CHECK(double(constant(*d, 0.0f, kMaxBlock * 4).back()) == Catch::Approx(0.0).margin(1e-6));   // silence in, silence out
}

TEST_CASE("cheby: full-scale sine emits harmonics N, N-2, ... and nothing above N", "[device][cheby]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "mix", 1.0f); set(*d, "order", 3.0f); d->reset();
    auto y = sine(*d, 32, 1.0f, 4096 * 3);
    const size_t s = 4096 * 2;
    CHECK(dft(y, s, 96) == Catch::Approx(1.0).margin(0.15));    // 3rd at amplitude 1
    CHECK(dft(y, s, 32) == Catch::Approx(2.0).margin(0.15));    // 1st at amplitude 2
    CHECK(dft(y, s, 64) < 0.05);                                // odd order: no even harmonics
    CHECK(dft(y, s, 160) < 0.05);                               // nothing above the order
}

TEST_CASE("cheby: order is truncated and floored at 1", "[device][cheby]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "mix", 1.0f);
    set(*d, "order", 3.9f); d->reset();                         // truncates to 3
    const double o3 = constant(*d, 0.8f, kMaxBlock * 20).back();
    CHECK(o3 == Catch::Approx(4 * 0.512 - 0.8).margin(2e-3));
    set(*d, "order", 0.0f); d->reset();                         // floor 1: identity
    CHECK(constant(*d, 0.8f, kMaxBlock * 20).back() == Catch::Approx(0.8).margin(2e-3));
}

TEST_CASE("cheby: mix 0 passes the dry signal", "[device][cheby]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "mix", 0.0f); set(*d, "order", 7.0f); d->reset();
    auto y = sine(*d, 32, 0.5f, 4096 * 3);
    CHECK(dft(y, 4096 * 2, 32) == Catch::Approx(0.5).margin(0.01));
    CHECK(dft(y, 4096 * 2, 96) < 0.005);
}
