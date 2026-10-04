#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("pingpong"); }

int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

void run(EffectDevice& d, std::vector<float>& l, std::vector<float>& r, double bpm = 120.0) {
    auto c = ctx(); c.bpm = bpm;
    for (size_t i = 0; i < l.size(); i += kMaxBlock) {
        const int n = int(std::min<size_t>(kMaxBlock, l.size() - i));
        d.process(l.data() + i, r.data() + i, n, c, {});
    }
}

std::unique_ptr<EffectDevice> configured(float time, float fb, float mix) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "time", time); set(*d, "fb", fb); set(*d, "mix", mix);
    d->reset();
    return d;
}

float windowSum(const std::vector<float>& x, size_t centre, size_t half) {
    float s = 0; for (size_t i = centre - half; i <= centre + half; ++i) s += std::abs(x[i]);
    return s;
}
}  // namespace

TEST_CASE("pingpong: device contract", "[device][pingpong]") { checkEffectContract(make); }

TEST_CASE("pingpong: a left impulse echoes L at dt, R at 2dt, L at 3dt", "[device][pingpong]") {
    // time 2 = 1/8 note at 120 bpm = 0.25 s = 11025 samples
    auto d = configured(2, 0.5f, 1.0f);
    std::vector<float> l(40000, 0.0f), r(40000, 0.0f);
    l[0] = 1.0f;
    run(*d, l, r);
    const size_t dt = 11025;
    CHECK(windowSum(l, dt, 3) == Catch::Approx(1.0f).margin(1e-3));
    CHECK(windowSum(r, dt, 3) < 1e-4f);
    CHECK(windowSum(r, 2 * dt, 3) == Catch::Approx(0.5f).margin(1e-3));
    CHECK(windowSum(l, 2 * dt, 3) < 1e-4f);
    CHECK(windowSum(l, 3 * dt, 3) == Catch::Approx(0.25f).margin(1e-3));
    CHECK(windowSum(r, 3 * dt, 3) < 1e-4f);
}

TEST_CASE("pingpong: the right input takes an extra pre-delay", "[device][pingpong]") {
    // time 0 = 1/32 note = 2756.25 samples; right-only impulse first appears on R at 2dt, bounces to L at 3dt
    auto d = configured(0, 0.5f, 1.0f);
    std::vector<float> l(12000, 0.0f), r(12000, 0.0f);
    r[0] = 1.0f;
    run(*d, l, r);
    const double dt = 2756.25;
    CHECK(windowSum(r, size_t(std::lround(dt)), 4) < 1e-4f);
    CHECK(windowSum(l, size_t(std::lround(dt)), 4) < 1e-4f);
    CHECK(windowSum(r, size_t(std::lround(2 * dt)), 4) == Catch::Approx(1.0f).margin(1e-3));
    CHECK(windowSum(l, size_t(std::lround(3 * dt)), 4) == Catch::Approx(0.5f).margin(1e-3));
}

TEST_CASE("pingpong: tempo change retunes the echo spacing", "[device][pingpong]") {
    auto d = configured(4, 0.0f, 1.0f);
    std::vector<float> zl(88200, 0.0f), zr = zl;
    run(*d, zl, zr, 60.0);
    std::vector<float> l(60000, 0.0f), r(60000, 0.0f);
    l[0] = 1.0f;
    run(*d, l, r, 60.0);                              // 1/4 note at 60 bpm = 1 s = 44100 samples
    const size_t peak = size_t(std::max_element(l.begin(), l.end(), [](float a, float b) { return std::abs(a) < std::abs(b); }) - l.begin());
    CHECK(std::abs(double(peak) - 44100.0) <= 8.0);
}

TEST_CASE("pingpong: mix 0 is bit-exact dry", "[device][pingpong]") {
    auto d = configured(2, 0.4f, 0.0f);
    std::vector<float> l = noiseBlock(2048, 1), r = noiseBlock(2048, 2);
    const auto l0 = l, r0 = r;
    run(*d, l, r);
    CHECK(l == l0);
    CHECK(r == r0);
}
