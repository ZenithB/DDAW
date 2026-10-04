#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("opto"); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(findParam(d.params(), key)), v); }

// Run a 220 Hz sine of `amp` for `seconds`; records gainReductionDb() after each block.
std::vector<float> runSine(EffectDevice& d, float amp, double seconds, std::vector<float>* out = nullptr) {
    std::vector<float> grs, l(kMaxBlock), r(kMaxBlock);
    const size_t total = size_t(seconds * kSr) / kMaxBlock * kMaxBlock;
    static thread_local size_t t0 = 0;
    for (size_t t = 0; t < total; t += kMaxBlock) {
        for (int i = 0; i < kMaxBlock; ++i) l[size_t(i)] = r[size_t(i)] = amp * float(std::sin(2.0 * 3.14159265358979 * 220.0 * double(t0 + t + size_t(i)) / kSr));
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        grs.push_back(d.gainReductionDb());
        if (out) out->insert(out->end(), l.begin(), l.end());
    }
    t0 += total;
    return grs;
}
double tailRms(const std::vector<float>& v) { return harness::rms(std::span<const float>(v).subspan(v.size() / 2)); }
}  // namespace

TEST_CASE("opto: device contract", "[device][opto]") { checkEffectContract(make); }

TEST_CASE("opto: reports the 6 ms lookahead as latency", "[device][opto]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    CHECK(d->latencySamples() == static_cast<int>(0.006f * 44100.0f));
}

TEST_CASE("opto: peak reduction deepens gain reduction, and Limit bites harder than Comp", "[device][opto]") {
    auto grFor = [](float reduction, float mode) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "reduction", reduction); set(*d, "mode", mode); d->reset();
        auto grs = runSine(*d, 0.5f, 2.0);
        return grs.back();
    };
    const float low = grFor(0.2f, 0), high = grFor(1.0f, 0), limit = grFor(1.0f, 1);
    CHECK(high < -6.0f);                 // threshold -40 dB, hot sine: real reduction
    CHECK(high < low - 3.0f);            // more peak reduction, more GR
    CHECK(limit < high - 3.0f);          // Limit (ratio 10) is harder than Comp (ratio 3)
    CHECK(low <= 0.0f);
}

TEST_CASE("opto: steady-state output is compressed relative to the input", "[device][opto]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "reduction", 1.0f); set(*d, "mode", 1.0f); set(*d, "gain", 0.0f); d->reset();
    std::vector<float> out;
    runSine(*d, 0.5f, 2.0, &out);
    CHECK(tailRms(out) < 0.8 * (0.5 / std::sqrt(2.0)));   // Limit mode nets a real level drop below the input RMS
}

TEST_CASE("opto: makeup gain is applied below threshold", "[device][opto]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "reduction", 0.0f); set(*d, "gain", 12.0f); d->reset();
    std::vector<float> out;
    runSine(*d, 0.1f, 1.0);                       // settle smoothers and the kernel
    runSine(*d, 0.1f, 0.25, &out);
    const double expect = 0.1 / std::sqrt(2.0) * std::pow(10.0, 12.0 / 20.0);
    CHECK(harness::rms(out) == Catch::Approx(expect).epsilon(0.05));
}

TEST_CASE("opto: release is slower than attack, two-stage and program dependent", "[device][opto]") {
    const double blockS = double(kMaxBlock) / kSr;
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "reduction", 1.0f); d->reset();
    const auto on = runSine(*d, 0.5f, 3.0);
    const float steady = on.back();
    REQUIRE(steady < -6.0f);
    const auto onsetIt = std::find_if(on.begin(), on.end(), [&](float g) { return g <= steady * 0.63f; });
    REQUIRE(onsetIt != on.end());
    const double onsetS = double(onsetIt - on.begin()) * blockS;
    CHECK(onsetS < 0.5);

    const auto off = runSine(*d, 0.001f, 5.0);   // drop far below threshold
    auto depthAt = [&](double t) { return off[size_t(t / blockS)] / steady; };
    const auto recIt = std::find_if(off.begin(), off.end(), [&](float g) { return g >= steady * 0.37f; });
    REQUIRE(recIt != off.end());
    const double recS = double(recIt - off.begin()) * blockS;
    CHECK(recS > 3.0 * std::max(onsetS, blockS));  // 63% recovery much slower than 63% onset
    CHECK(depthAt(0.45) < 0.75);                   // fast stage has acted
    CHECK(depthAt(1.35) > 0.15);                   // slow opto tail remains

    // Program dependence: 0.6 s after the drop, a brief peak has recovered more than sustained program.
    auto leftAfter = [](double hotS) {
        auto f = make(); f->prepare(kSr, kMaxBlock);
        set(*f, "reduction", 1.0f); f->reset();
        const float depth = runSine(*f, 0.5f, hotS).back();
        return runSine(*f, 0.001f, 0.6).back() / depth;
    };
    CHECK(leftAfter(3.0) > leftAfter(0.05) + 0.05);
}
