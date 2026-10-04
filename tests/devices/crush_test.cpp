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
std::unique_ptr<EffectDevice> make() { return createEffect("crush"); }
int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

std::vector<float> constant(EffectDevice& d, float v, int n) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int t = 0; t < n; t += kMaxBlock) {
        std::fill(l.begin(), l.end(), v); std::fill(r.begin(), r.end(), v);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

std::vector<float> impulse(EffectDevice& d, float amp, int at, int n) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int t = 0; t < n; t += kMaxBlock) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        if (at >= t && at < t + kMaxBlock) l[size_t(at - t)] = r[size_t(at - t)] = amp;
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}
size_t peakIdx(const std::vector<float>& v) {
    size_t p = 0;
    for (size_t i = 0; i < v.size(); ++i) if (std::abs(v[i]) > std::abs(v[p])) p = i;
    return p;
}
}  // namespace

TEST_CASE("crush: device contract", "[device][crush]") { checkEffectContract(make); }

TEST_CASE("crush: quantises to step 0.5^(bits-1), round-half-up", "[device][crush]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "mix", 1.0f);
    set(*d, "bits", 2.0f); d->reset();                                  // step 0.5
    CHECK(constant(*d, 0.3f, kMaxBlock * 20).back() == Catch::Approx(0.5f).margin(1e-4));    // 0.3/0.5+0.5 -> 1
    d->reset();
    CHECK(constant(*d, 0.2f, kMaxBlock * 20).back() == Catch::Approx(0.0f).margin(1e-4));    // 0.9 -> 0
    d->reset();
    CHECK(constant(*d, -0.3f, kMaxBlock * 20).back() == Catch::Approx(-0.5f).margin(1e-4));  // floor(-0.1) = -1
    set(*d, "bits", 3.0f); d->reset();                                  // step 0.25
    CHECK(constant(*d, 0.3f, kMaxBlock * 20).back() == Catch::Approx(0.25f).margin(1e-4));   // 1.7 -> 1
    set(*d, "bits", 1.0f); d->reset();                                  // step 1.0
    CHECK(constant(*d, 0.4f, kMaxBlock * 20).back() == Catch::Approx(0.0f).margin(1e-4));
    CHECK(constant(*d, 0.6f, kMaxBlock * 20).back() == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("crush: 16 bits is near transparent, 4 bits is not", "[device][crush]") {
    auto err = [](float bits) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "mix", 1.0f); set(*d, "bits", bits); d->reset();
        return std::abs(constant(*d, 0.3137f, kMaxBlock * 20).back() - 0.3137f);
    };
    CHECK(err(16.0f) < 1e-4);
    CHECK(err(4.0f) > 0.01);
}

TEST_CASE("crush: dry path is delayed to match the oversampler (no comb at partial mix)", "[device][crush]") {
    auto peak = [](float mix) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "mix", mix); set(*d, "bits", 16.0f); d->reset();
        return peakIdx(impulse(*d, 0.5f, 20, kMaxBlock * 4));
    };
    // The oversampler's group delay is fractional, so the wet peak may land one sample either side.
    CHECK(std::abs(int(peak(0.0f)) - int(peak(1.0f))) <= 1);
    CHECK(peak(0.0f) > 20);                                             // genuinely delayed
}

TEST_CASE("crush: mix 0 is the dry signal, mix is equal-power", "[device][crush]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "bits", 1.0f); set(*d, "mix", 0.0f); d->reset();
    CHECK(constant(*d, 0.3f, kMaxBlock * 20).back() == Catch::Approx(0.3f).margin(1e-4));
    set(*d, "mix", 0.5f); d->reset();                                   // wet 0.0 (0.3 -> 0), dry 0.3
    CHECK(constant(*d, 0.3f, kMaxBlock * 20).back() == Catch::Approx(0.3f * std::cos(std::numbers::pi_v<float> / 4)).margin(1e-3));
}
