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
std::unique_ptr<EffectDevice> make() { return createEffect("phaser"); }
int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

std::pair<std::vector<float>, std::vector<float>> render(float rate, float octaves, float mix, const std::vector<float>& in) {
    auto d = make();
    set(*d, "rate", rate); set(*d, "octaves", octaves); set(*d, "mix", mix);
    d->prepare(kSr, kMaxBlock);
    std::vector<float> l = in, r = in;
    for (size_t i = 0; i < in.size(); i += size_t(kMaxBlock)) d->process(l.data() + i, r.data() + i, kMaxBlock, ctx(), {});
    return {l, r};
}

std::vector<float> sine(size_t n, double hz, float amp) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = amp * float(std::sin(2.0 * M_PI * hz * double(i) / kSr));
    return x;
}

// max/min of 5 ms RMS windows over [from, n).
double envelopeRatio(const std::vector<float>& x, size_t from) {
    const size_t win = 220;
    double mn = 1e9, mx = 0;
    for (size_t i = from; i + win <= x.size(); i += win) {
        const double e = harness::rms(std::span<const float>(x).subspan(i, win));
        mn = std::min(mn, e); mx = std::max(mx, e);
    }
    return mx / std::max(mn, 1e-9);
}
}  // namespace

TEST_CASE("phaser: device contract", "[device][phaser]") { checkEffectContract(make); }

TEST_CASE("phaser: the allpass chain alone is magnitude-flat (mix 1)", "[device][phaser]") {
    auto in = noiseBlock(size_t(kSr), 5, 0.3f);
    auto [l, r] = render(0.01f, 3.0f, 1.0f, in);
    const size_t skip = size_t(kSr / 2);
    const double a = harness::rms(std::span<const float>(l).subspan(skip));
    const double b = harness::rms(std::span<const float>(in).subspan(skip));
    CHECK(a / b == Catch::Approx(1.0).margin(0.1));
}

TEST_CASE("phaser: notches sweep through a fixed tone (static LFO vs moving LFO)", "[device][phaser]") {
    // dry+wet sum at mix 0.5: a 1 kHz tone is nulled whenever the sweeping chain's phase there is
    // 180 deg, so its level pumps deeply with the sweep and stays constant with a frozen LFO.
    const auto in = sine(size_t(kSr * 1.5), 1000.0, 0.5f);
    auto [moving, mr] = render(2.0f, 4.0f, 0.5f, in);
    auto [frozen, fr] = render(0.01f, 4.0f, 0.5f, in);
    const size_t from = size_t(kSr / 2);
    CHECK(envelopeRatio(moving, from) > 3.0);
    CHECK(envelopeRatio(frozen, from) < 1.2);
}

TEST_CASE("phaser: octaves sets the sweep range (more octaves, deeper reach)", "[device][phaser]") {
    // A 6 kHz tone is only reached by the sweep with high octave counts (350*2^5 = 11.2 kHz upper edge).
    const auto in = sine(size_t(kSr * 1.5), 6000.0, 0.5f);
    auto [one, a] = render(2.0f, 1.0f, 0.5f, in);                 // sweep 350..700 Hz: far from 6 kHz
    auto [five, b] = render(2.0f, 5.0f, 0.5f, in);
    CHECK(envelopeRatio(one, size_t(kSr / 2)) < envelopeRatio(five, size_t(kSr / 2)));
}

TEST_CASE("phaser: left and right sweep in antiphase", "[device][phaser]") {
    const auto in = noiseBlock(size_t(kSr), 9, 0.3f);
    auto [l, r] = render(1.0f, 4.0f, 0.5f, in);
    double diff = 0; for (size_t i = 0; i < l.size(); ++i) diff += std::abs(double(l[i]) - double(r[i]));
    CHECK(diff / double(l.size()) > 0.01);
}

TEST_CASE("phaser: mix 0 is dry", "[device][phaser]") {
    const auto in = noiseBlock(1024, 3, 0.4f);
    auto [l, r] = render(1.0f, 3.0f, 0.0f, in);
    for (size_t i = 0; i < in.size(); ++i) CHECK(l[i] == Catch::Approx(in[i]).margin(1e-6));
}

namespace {
std::vector<float> renderMode(float mode, float rateSync, float rateHi, float rateLo, double bpm) {
    auto d = make();
    set(*d, "rateMode", mode); set(*d, "rateSync", rateSync); set(*d, "rateHi", rateHi); set(*d, "rate", rateLo);
    set(*d, "octaves", 4.0f); set(*d, "mix", 0.5f);
    d->prepare(kSr, kMaxBlock);
    const auto in = noiseBlock(size_t(kSr / 2), 11, 0.3f);
    std::vector<float> l = in, r = in;
    ProcessContext pc = ctx(); pc.bpm = bpm;
    for (size_t i = 0; i < in.size(); i += size_t(kMaxBlock)) d->process(l.data() + i, r.data() + i, kMaxBlock, pc, {});
    return l;
}
}  // namespace

TEST_CASE("phaser: Sync mode follows bpm (1/4 at 120 bpm sweeps like a 2 Hz Low rate; 240 bpm like 4 Hz)", "[device][phaser]") {
    CHECK(renderMode(0, 5, 440, 9, 120.0) == renderMode(1, 5, 440, 2.0f, 120.0));
    CHECK(renderMode(0, 5, 440, 9, 240.0) == renderMode(1, 5, 440, 4.0f, 120.0));
    CHECK(renderMode(0, 5, 440, 9, 120.0) != renderMode(0, 5, 440, 9, 240.0));
    CHECK(renderMode(0, 3, 440, 9, 120.0) == renderMode(1, 3, 440, 0.5f, 120.0));   // 1 bar
}

TEST_CASE("phaser: High mode uses rateHi, Low mode ignores it", "[device][phaser]") {
    // 440 Hz from rateHi equals the sync rate that works out to 440 Hz (bpm 26400, 1/4)
    CHECK(renderMode(2, 5, 440, 1.0f, 120.0) == renderMode(0, 5, 999, 1.0f, 26400.0));
    CHECK(renderMode(2, 5, 440, 1.0f, 120.0) != renderMode(2, 5, 700, 1.0f, 120.0));
    CHECK(renderMode(1, 5, 440, 1.0f, 120.0) == renderMode(1, 5, 900, 1.0f, 120.0));
}
