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

std::unique_ptr<EffectDevice> make() { return createEffect("gate"); }

struct Kv { std::string_view key; float v; };

void set(EffectDevice& d, std::initializer_list<Kv> kvs) {
    for (auto& kv : kvs) {
        const int i = findParam(d.params(), kv.key);
        REQUIRE(i >= 0);
        d.setParam(static_cast<uint16_t>(i), kv.v);
    }
}

std::unique_ptr<EffectDevice> build(std::initializer_list<Kv> kvs = {}) {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    set(*d, kvs);
    d->reset();
    return d;
}

std::vector<float> sine(size_t n, double hz, float amp) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * double(i) / kSr));
    return x;
}

std::vector<float> run(EffectDevice& d, std::vector<float> l, std::vector<float> r, int block = kMaxBlock) {
    for (size_t i = 0; i < l.size(); i += size_t(block)) {
        const int n = static_cast<int>(std::min<size_t>(size_t(block), l.size() - i));
        d.process(l.data() + i, r.data() + i, n, ctx(), {});
    }
    return l;
}
std::vector<float> run(EffectDevice& d, const std::vector<float>& x) { return run(d, x, x); }

double tailRms(const std::vector<float>& v) { return harness::rms(std::span<const float>(v).subspan(v.size() / 2)); }
double db(double x) { return 20.0 * std::log10(x); }
size_t samples(double sec) { return static_cast<size_t>(sec * kSr); }

// Steady-state gain (dB) of a sine, and the meter afterwards.
double gainDb(EffectDevice& d, double hz, float amp, size_t n = 44100) {
    const auto in = sine(n, hz, amp);
    return db(tailRms(run(d, in)) / tailRms(in));
}

}  // namespace

TEST_CASE("gate: device contract", "[device][gate]") { checkEffectContract(make); }

TEST_CASE("gate: loud passes at unity, quiet is cut to the range", "[device][gate]") {
    auto d = build({{"thresh", -40}});
    CHECK(std::abs(gainDb(*d, 220, 0.5f)) < 0.01);
    CHECK(d->gainReductionDb() > -0.01f);

    d = build({{"thresh", -40}});
    CHECK(gainDb(*d, 220, 0.002f) == Catch::Approx(-60.0).margin(0.5));
    CHECK(d->gainReductionDb() < -55.0f);
}

TEST_CASE("gate: opens within a dB of the threshold", "[device][gate]") {
    // Peak detector reads the crest of the sine, so thresh -40 / up 0 opens for peaks above -40 dBFS.
    auto closed = build({{"thresh", -40}});
    gainDb(*closed, 220, 0.0079f);                       // -42 dBFS
    CHECK(closed->gainReductionDb() < -55.0f);
    auto open = build({{"thresh", -40}});
    gainDb(*open, 220, 0.0126f);                         // -38 dBFS
    CHECK(open->gainReductionDb() > -0.01f);
}

TEST_CASE("gate: up offset raises the opening level", "[device][gate]") {
    auto d = build({{"thresh", -40}, {"up", 10}});       // opens above -30 dBFS
    gainDb(*d, 220, 0.0126f);                            // -38: still closed
    CHECK(d->gainReductionDb() < -55.0f);
    d = build({{"thresh", -40}, {"up", 10}});
    gainDb(*d, 220, 0.05f);                              // -26: open
    CHECK(d->gainReductionDb() > -0.01f);
}

TEST_CASE("gate: the hysteresis window latches the state", "[device][gate]") {
    // thresh -40, up 0, dn -12: opens above -40, closes below -52.
    const std::initializer_list<Kv> params = {{"thresh", -40}, {"up", 0}, {"dn", -12}, {"hold", 0.001f}};
    auto d = build(params);
    run(*d, sine(samples(0.5), 220, 0.5f));
    run(*d, sine(samples(0.5), 220, 0.005f));            // -46 dBFS: inside the window after opening
    CHECK(d->gainReductionDb() > -0.01f);

    d = build(params);
    run(*d, sine(samples(0.5), 220, 0.005f));            // same level from a closed start
    CHECK(d->gainReductionDb() < -55.0f);

    d = build(params);                                    // below the close level it does shut
    run(*d, sine(samples(0.5), 220, 0.5f));
    run(*d, sine(samples(0.5), 220, 0.001f));            // -60 dBFS
    CHECK(d->gainReductionDb() < -55.0f);
}

TEST_CASE("gate: hold bridges a short gap and a long gap closes", "[device][gate]") {
    auto gapGr = [](double gapSec) {
        auto d = build({{"thresh", -40}, {"hold", 0.1f}, {"release", 0.005f}});
        run(*d, sine(samples(0.3), 220, 0.5f));
        run(*d, std::vector<float>(samples(gapSec), 0.0f));
        return d->gainReductionDb();
    };
    CHECK(gapGr(0.05) > -0.01f);   // 50 ms gap < 100 ms hold (plus detector fall time)
    CHECK(gapGr(0.3) < -55.0f);    // 300 ms gap > hold
}

TEST_CASE("gate: attack ramp is linear in dB over the range", "[device][gate]") {
    // Close the gate, then feed a loud tone one sample at a time: the gate opens immediately (instant
    // peak attack) and the gain climbs 60 dB over `attack` seconds.
    auto d = build({{"thresh", -40}, {"attack", 0.01f}});
    run(*d, std::vector<float>(samples(0.5), 0.0f));
    REQUIRE(d->gainReductionDb() < -59.9f);
    const auto in = sine(samples(0.02), 220, 0.5f);
    std::vector<float> l = in, r = in;
    std::vector<double> gr;
    for (size_t i = 0; i < l.size(); ++i) { d->process(&l[i], &r[i], 1, ctx(), {}); gr.push_back(d->gainReductionDb()); }
    const size_t half = samples(0.005);                   // halfway through the 10 ms attack
    CHECK(gr[half] == Catch::Approx(-30.0).margin(1.5));
    CHECK(gr[samples(0.0025)] == Catch::Approx(-45.0).margin(1.5));
    CHECK(gr[samples(0.0095)] > -4.0);
    CHECK(gr.back() == 0.0);
}

TEST_CASE("gate: release ramp is linear in dB and starts after the hold", "[device][gate]") {
    // hold 1 ms, release 0.2 s => 0.3 dB per ms once the gate has closed.
    auto d = build({{"thresh", -40}, {"hold", 0.001f}, {"release", 0.2f}});
    run(*d, sine(samples(0.3), 220, 0.5f));
    REQUIRE(d->gainReductionDb() > -0.01f);
    std::vector<double> gr;
    std::vector<float> z(1, 0.0f), z2(1, 0.0f);
    for (size_t i = 0; i < samples(0.3); ++i) { z[0] = z2[0] = 0.0f; d->process(z.data(), z2.data(), 1, ctx(), {}); gr.push_back(d->gainReductionDb()); }
    CHECK(gr[samples(0.005)] > -0.01);                    // detector still falling toward the close level
    const double a = gr[samples(0.15)], b = gr[samples(0.17)];
    CHECK(a - b == Catch::Approx(6.0).margin(0.4));      // 20 ms at 0.3 dB/ms
    CHECK(gr.back() < -50.0);
}

TEST_CASE("gate: inverse flips which side passes", "[device][gate]") {
    auto d = build({{"thresh", -40}, {"inverse", 1}});
    gainDb(*d, 220, 0.5f);
    CHECK(d->gainReductionDb() < -55.0f);
    d = build({{"thresh", -40}, {"inverse", 1}});
    gainDb(*d, 220, 0.002f);
    CHECK(d->gainReductionDb() > -0.01f);
}

TEST_CASE("gate: key filter scopes the detection band", "[device][gate]") {
    auto d = build({{"thresh", -40}});
    gainDb(*d, 80, 0.5f);
    CHECK(d->gainReductionDb() > -0.01f);
    d = build({{"thresh", -40}, {"hpf", 2000}});
    gainDb(*d, 80, 0.5f);
    CHECK(d->gainReductionDb() < -55.0f);
    d = build({{"thresh", -40}, {"lpf", 200}});           // lowpassed key ignores a 6 kHz tone
    gainDb(*d, 6000, 0.5f);
    CHECK(d->gainReductionDb() < -55.0f);
}

TEST_CASE("gate: key source selects the detected channel", "[device][gate]") {
    const auto loud = sine(samples(0.5), 220, 0.5f);
    const std::vector<float> silent(loud.size(), 0.0f);
    auto gr = [&](float key, const std::vector<float>& l, const std::vector<float>& r) {
        auto d = build({{"thresh", -40}, {"key", key}});
        run(*d, l, r);
        return d->gainReductionDb();
    };
    CHECK(gr(0, loud, silent) > -0.01f);   // stereo: either channel opens
    CHECK(gr(1, loud, silent) > -0.01f);   // left only
    CHECK(gr(2, loud, silent) < -55.0f);   // right only: silent
    CHECK(gr(2, silent, loud) > -0.01f);
    CHECK(gr(3, loud, silent) > -0.01f);   // mid = (L+R)/2 = 0.25 peak, still > -40 dB
    CHECK(gr(4, loud, loud) < -55.0f);     // side of identical channels is silent
    CHECK(gr(4, loud, silent) > -0.01f);
}

TEST_CASE("gate: RMS detector reads 3 dB below the peak detector for a sine", "[device][gate]") {
    // -38.5 dBFS peak: the peak detector opens (> -40) but the RMS level (-41.5) stays closed.
    const float amp = std::pow(10.0f, -38.5f / 20.0f);
    auto peak = build({{"thresh", -40}, {"det", 0}});
    gainDb(*peak, 220, amp);
    CHECK(peak->gainReductionDb() > -0.01f);
    auto rms = build({{"thresh", -40}, {"det", 1}});
    gainDb(*rms, 220, amp);
    CHECK(rms->gainReductionDb() < -55.0f);
}

TEST_CASE("gate: finite ratio expands proportionally", "[device][gate]") {
    auto red = [](float ratio) {
        auto d = build({{"thresh", -40}, {"ratio", ratio}, {"knee", 0}, {"range", -100}, {"up", 0}, {"dn", 0}});
        gainDb(*d, 220, 0.0025f);                          // -52 dBFS: 12 dB below
        return -d->gainReductionDb();
    };
    const float r2 = red(2), r4 = red(4);
    CHECK(r2 > 6.0f);
    CHECK(r2 < 18.0f);
    CHECK(r4 > 2.5f * r2);
}

TEST_CASE("gate: range floors the reduction", "[device][gate]") {
    for (float range : {-6.0f, -24.0f, -60.0f}) {
        auto d = build({{"thresh", -20}, {"range", range}});
        gainDb(*d, 220, 0.0005f);
        INFO("range " << range);
        CHECK(d->gainReductionDb() >= range - 0.01f);
        CHECK(d->gainReductionDb() < range + 0.5f);
    }
}

TEST_CASE("gate: lookahead delays the audio by the requested time", "[device][gate]") {
    auto d = build({{"look", 5.0f}, {"range", 0}});
    std::vector<float> l(4096, 0.0f);
    l[0] = 0.5f;
    const auto out = run(*d, l, l);
    const size_t expect = static_cast<size_t>((5.0f / 1000.0f) * 44100.0f);
    size_t peak = 0;
    for (size_t i = 0; i < out.size(); ++i) if (std::abs(out[i]) > std::abs(out[peak])) peak = i;
    CHECK(peak == expect);
    CHECK(out[peak] == Catch::Approx(0.5f));
}

TEST_CASE("gate: key listen outputs the filtered key", "[device][gate]") {
    const auto src = sine(samples(1.0), 80, 0.5f);
    auto d = build({{"listen", 1}, {"hpf", 2000}});
    CHECK(tailRms(run(*d, src)) < 0.05 * harness::rms(src));
    d = build({{"listen", 1}});
    CHECK(tailRms(run(*d, src)) > 0.8 * harness::rms(src));
}

TEST_CASE("gate: mix blends against the dry path and gain scales the output", "[device][gate]") {
    const auto src = sine(samples(1.0), 220, 0.002f);
    auto d = build({{"thresh", -40}, {"range", -100}, {"mix", 0.5f}});
    CHECK(tailRms(run(*d, src)) / tailRms(src) == Catch::Approx(0.5).margin(0.02));
    d = build({{"gain", 6.0f}});
    CHECK(gainDb(*d, 220, 0.5f) == Catch::Approx(6.0).margin(0.1));
}

TEST_CASE("gate: reset restores the open state", "[device][gate]") {
    auto d = build({{"thresh", -40}});
    run(*d, std::vector<float>(samples(1.0), 0.0f));
    CHECK(d->gainReductionDb() < -55.0f);
    d->reset();
    CHECK(d->gainReductionDb() == 0.0f);
}

TEST_CASE("gate: non-finite parameter values are ignored", "[device][gate]") {
    auto d = build({{"thresh", -40}});
    d->setParam(0, std::nanf(""));
    auto out = run(*d, sine(samples(0.2), 220, 0.5f));
    for (float x : out) REQUIRE(std::isfinite(x));
}
