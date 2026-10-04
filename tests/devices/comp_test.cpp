#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("comp"); }
double tailRms(const std::vector<float>& v) { return harness::rms(std::span<const float>(v).subspan(v.size() / 2)); }
}  // namespace

TEST_CASE("comp: device contract", "[device][comp]") { checkEffectContract(make); }

TEST_CASE("comp: reports the node's 6 ms lookahead as latency", "[device][comp]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    CHECK(d->latencySamples() == static_cast<int>(0.006f * 44100.0f));
}

TEST_CASE("comp: heavy settings reduce a loud signal, light settings do not", "[device][comp]") {
    auto level = [](float thresh, float ratio, float amp) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        d->setParam(0, thresh); d->setParam(1, ratio); d->setParam(2, 0.003f); d->setParam(3, 0.1f);
        d->reset();
        return tailRms(runEffect(*d, 200, kSr, amp).first);
    };
    const double heavy = level(-40, 20, 0.8f), light = level(0, 1, 0.8f);
    CHECK(heavy < 0.6 * light);                      // clearly compressed
    CHECK(light > 0.2);                              // threshold 0 dB, ratio 1: roughly passes through (+makeup)
}

TEST_CASE("comp: gain reduction meter tracks the compression", "[device][comp]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setParam(0, -40.0f); d->setParam(1, 12.0f); d->reset();
    CHECK(d->gainReductionDb() == 0.0f);
    runEffect(*d, 100, kSr, 0.8f);
    CHECK(d->gainReductionDb() < -3.0f);
    CHECK(d->gainReductionDb() <= 0.0f);
}
