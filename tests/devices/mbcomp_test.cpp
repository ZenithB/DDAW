#include <cmath>
#include <numbers>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {

std::unique_ptr<EffectDevice> make() { return createEffect("mbcomp"); }

struct Kv { std::string_view key; float v; };

// Build a prepared device with the given parameters applied and the smoothers snapped to them.
std::unique_ptr<EffectDevice> build(std::initializer_list<Kv> kvs) {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    for (auto& kv : kvs) {
        const int i = findParam(d->params(), kv.key);
        REQUIRE(i >= 0);
        d->setParam(static_cast<uint16_t>(i), kv.v);
    }
    d->reset();
    return d;
}

std::vector<float> sine(size_t n, double hz, float amp) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * double(i) / kSr));
    return x;
}

std::vector<float> run(EffectDevice& d, std::vector<float> l, std::vector<float> r) {
    for (size_t i = 0; i < l.size(); i += kMaxBlock) {
        const int n = static_cast<int>(std::min<size_t>(kMaxBlock, l.size() - i));
        d.process(l.data() + i, r.data() + i, n, ctx(), {});
    }
    return l;
}
std::vector<float> run(EffectDevice& d, const std::vector<float>& x) { return run(d, x, x); }

double tailRms(const std::vector<float>& v) { return harness::rms(std::span<const float>(v).subspan(v.size() / 2)); }
double db(double x) { return 20.0 * std::log10(x); }

// Level (dB) of a steady sine through the device relative to the input.
double gainDb(EffectDevice& d, double hz, float amp, size_t n = 44100) {
    const auto in = sine(n, hz, amp);
    return db(tailRms(run(d, in)) / tailRms(in));
}

}  // namespace

TEST_CASE("mbcomp: device contract", "[device][mbcomp]") { checkEffectContract(make); }

TEST_CASE("mbcomp: reports no latency", "[device][mbcomp]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    CHECK(d->latencySamples() == 0);
}

TEST_CASE("mbcomp: LR4 bands sum to unity with compression disabled", "[device][mbcomp]") {
    // ratio 1 everywhere: no reduction, so the three bands recombine to an allpass of the input.
    auto d = build({{"b0_ratio", 1}, {"b1_ratio", 1}, {"b2_ratio", 1}});
    for (double hz : {60.0, 120.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0, 12000.0}) {
        d->reset();
        INFO("freq " << hz);
        CHECK(std::abs(gainDb(*d, hz, 0.5f)) < 0.1);
    }
    CHECK(d->gainReductionDb() == 0.0f);
}

// Note: the worklet topology (LP_lo + HP_lo * allpass_hi) is only exactly flat for well separated crossovers.
TEST_CASE("mbcomp: bands sum flat for white noise with other crossover settings", "[device][mbcomp]") {
    for (auto [lo, hi] : {std::pair{100.0f, 800.0f}, std::pair{600.0f, 6000.0f}, std::pair{60.0f, 12000.0f}}) {
        auto d = build({{"xlo", lo}, {"xhi", hi}, {"b0_ratio", 1}, {"b1_ratio", 1}, {"b2_ratio", 1}});
        const auto in = noiseBlock(1 << 16, 5, 0.3f);
        const auto out = run(*d, in);
        INFO("xlo " << lo << " xhi " << hi);
        CHECK(std::abs(db(harness::rms(out) / harness::rms(in))) < 0.1);
    }
}

TEST_CASE("mbcomp: a loud tone reduces only its own band", "[device][mbcomp]") {
    // Low band compressed hard, mid/high transparent.
    for (double hz : {100.0, 6000.0}) {
        auto d = build({{"b0_ratio", 20}, {"b1_ratio", 1}, {"b2_ratio", 1}, {"attack", 0.005f}, {"release", 0.1f}});
        const double g = gainDb(*d, hz, 0.9f);
        INFO("freq " << hz);
        if (hz < 250.0) {
            // 0.9 peak is about -0.9 dB, 23 dB over the -24 dB threshold; ratio 20 leaves ~22 dB of reduction.
            CHECK(g < -18.0);
            CHECK(g > -26.0);
            CHECK(d->gainReductionDb() < -15.0f);
        } else {
            CHECK(std::abs(g) < 0.2);
            CHECK(d->gainReductionDb() > -0.5f);
        }
    }
}

TEST_CASE("mbcomp: compression follows threshold and ratio", "[device][mbcomp]") {
    // 1 kHz sits in the mid band. Envelope follows the peak, so the level reads ~A (-6 dBFS for 0.5).
    auto gr = [](float thresh, float ratio) {
        auto d = build({{"b1_thresh", thresh}, {"b1_ratio", ratio}, {"b0_ratio", 1}, {"b2_ratio", 1}, {"attack", 0.002f}});
        return gainDb(*d, 1000.0, 0.5f);
    };
    CHECK(std::abs(gr(-6.0f, 8)) < 1.0);                 // at threshold: nothing yet
    CHECK(gr(-26.0f, 2) == Catch::Approx(-10.0).margin(1.5));   // 20 dB over, 2:1 halves it
    CHECK(gr(-26.0f, 4) == Catch::Approx(-15.0).margin(1.5));   // 20 dB over, 4:1 -> 15 dB
    CHECK(gr(-60.0f, 1) == Catch::Approx(0.0).margin(0.2));     // ratio 1 never compresses
}

TEST_CASE("mbcomp: per-band makeup gain", "[device][mbcomp]") {
    auto d = build({{"b0_ratio", 1}, {"b1_ratio", 1}, {"b2_ratio", 1}, {"b1_gain", 6.0f}});
    CHECK(gainDb(*d, 1000.0, 0.3f) == Catch::Approx(6.0).margin(0.3));
    d->reset();
    CHECK(gainDb(*d, 6000.0, 0.3f) == Catch::Approx(0.0).margin(0.3));
}

TEST_CASE("mbcomp: expand mode attenuates a signal below threshold", "[device][mbcomp]") {
    // 100 Hz at -40 dBFS is 16 dB under the -24 dB threshold: 2:1 downward expansion adds ~16 dB of cut.
    auto d = build({{"mode", 1}, {"b0_ratio", 2}, {"b1_ratio", 1}, {"b2_ratio", 1}});
    const double g = gainDb(*d, 100.0, 0.01f);
    CHECK(g < -8.0);
    CHECK(d->gainReductionDb() < -8.0f);
    // Above threshold the expander leaves the signal alone.
    d->reset();
    CHECK(std::abs(gainDb(*d, 100.0, 0.5f)) < 0.3);
}

TEST_CASE("mbcomp: gain reduction meter reports and reset clears it", "[device][mbcomp]") {
    auto d = build({{"b0_ratio", 8}});
    CHECK(d->gainReductionDb() == 0.0f);
    run(*d, sine(8192, 220.0, 0.9f));
    CHECK(d->gainReductionDb() < -1.0f);
    d->reset();
    CHECK(d->gainReductionDb() == 0.0f);
}
