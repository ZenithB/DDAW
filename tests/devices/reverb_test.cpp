#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("reverb"); }

int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

std::unique_ptr<EffectDevice> configured(float size, float mix) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "size", size); set(*d, "mix", mix);
    d->reset();
    return d;
}

// Stereo impulse response of `secs` seconds.
std::pair<std::vector<float>, std::vector<float>> impulse(EffectDevice& d, float secs) {
    const size_t n = size_t(secs * float(kSr));
    std::vector<float> l(n, 0.0f), r(n, 0.0f);
    l[0] = r[0] = 1.0f;
    for (size_t i = 0; i < n; i += kMaxBlock)
        d.process(l.data() + i, r.data() + i, int(std::min<size_t>(kMaxBlock, n - i)), ctx(), {});
    return {l, r};
}

double energy(const std::vector<float>& x, double t0, double t1) {
    double e = 0;
    for (size_t i = size_t(t0 * kSr); i < std::min(x.size(), size_t(t1 * kSr)); ++i) e += double(x[i]) * x[i];
    return e;
}
}  // namespace

TEST_CASE("reverb: device contract", "[device][reverb]") { checkEffectContract(make); }

TEST_CASE("reverb: tail energy decays", "[device][reverb]") {
    auto d = configured(1.0f, 1.0f);
    auto [l, r] = impulse(*d, 3.0f);
    const double e1 = energy(l, 0.2, 0.7), e2 = energy(l, 0.7, 1.2), e3 = energy(l, 1.2, 1.7), e4 = energy(l, 2.2, 2.7);
    CHECK(e1 > 1e-9);
    CHECK(e1 > e2);
    CHECK(e2 > e3);
    CHECK(e3 > 10.0 * e4);
    // ~60 dB over `size` seconds: 0.5 s windows 1 s apart differ by roughly 60 dB in energy
    const double dropDb = 10.0 * std::log10(energy(l, 0.1, 0.35) / energy(l, 1.1, 1.35));
    CHECK(dropDb > 40.0);
    CHECK(dropDb < 80.0);
}

TEST_CASE("reverb: a larger size rings longer", "[device][reverb]") {
    auto tail = [](float size) {
        auto d = configured(size, 1.0f);
        auto [l, r] = impulse(*d, 2.5f);
        return energy(l, 1.5, 2.5);
    };
    CHECK(tail(8.0f) > 10.0 * tail(0.5f));
}

TEST_CASE("reverb: wet impulse energy follows the convolver normalisation law", "[device][reverb]") {
    // per-channel energy = (0.00125*44100/sr)^2 * size * sr (reverb.rs, golden-path calibration)
    for (float size : {1.0f, 4.0f}) {
        auto d = configured(size, 1.0f);
        auto [l, r] = impulse(*d, size * 2.0f + 0.5f);
        const double target = 0.00125 * 0.00125 * double(size) * kSr;
        const double db = 10.0 * std::log10(energy(l, 0, 100) / target);
        CHECK(std::abs(db) < 2.0);
    }
}

TEST_CASE("reverb: pre-delay holds the wet path back and mix 0 is bit-exact dry", "[device][reverb]") {
    {
        auto d = configured(2.0f, 1.0f);
        auto [l, r] = impulse(*d, 0.2f);
        size_t first = 0; while (first < l.size() && std::abs(l[first]) < 1e-9f) ++first;
        CHECK(first >= size_t(0.02 * kSr));
    }
    {
        auto d = configured(2.2f, 0.0f);
        std::vector<float> l = noiseBlock(1024, 3), r = noiseBlock(1024, 4);
        const auto l0 = l, r0 = r;
        d->process(l.data(), r.data(), 128, ctx(), {});
        CHECK(std::equal(l.begin(), l.begin() + 128, l0.begin()));
        CHECK(std::equal(r.begin(), r.begin() + 128, r0.begin()));
    }
}

TEST_CASE("reverb: stereo tails are decorrelated", "[device][reverb]") {
    auto d = configured(2.0f, 1.0f);
    auto [l, r] = impulse(*d, 1.0f);
    double diff = 0;
    for (size_t i = 4410; i < l.size(); ++i) diff += double(l[i] - r[i]) * (l[i] - r[i]);
    CHECK(diff > 1e-9);
}
