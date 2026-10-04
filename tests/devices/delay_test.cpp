#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("delay"); }

int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

ProcessContext ctxBpm(double bpm) { auto c = ctx(); c.bpm = bpm; return c; }

// Process `l`/`r` in place in kMaxBlock chunks.
void run(EffectDevice& d, std::vector<float>& l, std::vector<float>& r, double bpm = 120.0) {
    for (size_t i = 0; i < l.size(); i += kMaxBlock) {
        const int n = int(std::min<size_t>(kMaxBlock, l.size() - i));
        d.process(l.data() + i, r.data() + i, n, ctxBpm(bpm), {});
    }
}

std::unique_ptr<EffectDevice> configured(float time, float fb, float mix) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "time", time); set(*d, "fb", fb); set(*d, "mix", mix);
    d->reset();
    return d;
}

// Sum of |x| over a window: preserved by linear-interpolated (fractional) delays.
float windowSum(const std::vector<float>& x, size_t centre, size_t half) {
    float s = 0; for (size_t i = centre - half; i <= centre + half; ++i) s += std::abs(x[i]);
    return s;
}
}  // namespace

TEST_CASE("delay: device contract", "[device][delay]") { checkEffectContract(make); }

TEST_CASE("delay: echo spacing equals the tempo-synced delay time", "[device][delay]") {
    // time 2 = 1/8 of a whole note; whole = 240/120 = 2 s -> 0.25 s = 11025 samples at 44.1 kHz.
    auto d = configured(2, 0.0f, 1.0f);
    std::vector<float> l(22050, 0.0f), r(22050, 0.0f);
    l[0] = r[0] = 1.0f;
    run(*d, l, r);
    const size_t peak = size_t(std::max_element(l.begin(), l.end(), [](float a, float b) { return std::abs(a) < std::abs(b); }) - l.begin());
    CHECK(peak == 11025);
    CHECK(std::abs(l[peak]) > 0.99f);
    CHECK(l == r);
}

TEST_CASE("delay: tempo change retunes the delay time", "[device][delay]") {
    auto d = configured(4, 0.0f, 1.0f);              // 1/4 of a whole note
    std::vector<float> zl(88200, 0.0f), zr = zl;     // 2 s at 60 bpm lets the 50 ms glide settle
    run(*d, zl, zr, 60.0);
    std::vector<float> l(60000, 0.0f), r(60000, 0.0f);
    l[0] = r[0] = 1.0f;
    run(*d, l, r, 60.0);                             // whole = 4 s -> 1 s = 44100 samples
    const size_t peak = size_t(std::max_element(l.begin(), l.end(), [](float a, float b) { return std::abs(a) < std::abs(b); }) - l.begin());
    CHECK(std::abs(double(peak) - 44100.0) <= 8.0);
}

TEST_CASE("delay: feedback gives a geometric decay train", "[device][delay]") {
    // time 0 = 1/32 note = 62.5 ms = 2756.25 samples (fractional: compare windowed sums)
    auto d = configured(0, 0.5f, 1.0f);
    std::vector<float> l(14000, 0.0f), r(14000, 0.0f);
    l[0] = r[0] = 1.0f;
    run(*d, l, r);
    const double dt = 2756.25;
    float prev = 0;
    for (int k = 1; k <= 4; ++k) {
        const float s = windowSum(l, size_t(std::lround(dt * k)), 4);
        CHECK(s == Catch::Approx(std::pow(0.5f, float(k - 1))).margin(0.03));
        if (k > 1) CHECK(s / prev == Catch::Approx(0.5f).margin(0.03));
        prev = s;
    }
}

TEST_CASE("delay: wet/dry mix is equal power", "[device][delay]") {
    {   // mix 0: bit-exact dry
        auto d = configured(2, 0.35f, 0.0f);
        std::vector<float> l = noiseBlock(2048, 5), r = noiseBlock(2048, 6);
        const auto l0 = l, r0 = r;
        run(*d, l, r);
        CHECK(l == l0);
        CHECK(r == r0);
    }
    {   // mix 1: no dry at the impulse's own sample
        auto d = configured(2, 0.0f, 1.0f);
        std::vector<float> l(64, 0.0f), r(64, 0.0f);
        l[0] = 1.0f;
        run(*d, l, r);
        CHECK(std::abs(l[0]) < 1e-6f);
    }
    {   // mix 0.5: dry gain cos(pi/4); echo gain sin(pi/4)
        auto d = configured(2, 0.0f, 0.5f);
        std::vector<float> l(12000, 0.0f), r = l;
        l[0] = 1.0f;
        run(*d, l, r);
        CHECK(l[0] == Catch::Approx(std::cos(0.25 * 3.14159265358979)).margin(1e-5));
        CHECK(l[11025] == Catch::Approx(std::sin(0.25 * 3.14159265358979)).margin(1e-5));
    }
}

TEST_CASE("delay: changing the division glides instead of clicking", "[device][delay]") {
    auto d = configured(4, 0.0f, 1.0f);
    std::vector<float> l(kMaxBlock * 400), r;
    for (size_t i = 0; i < l.size(); ++i) l[i] = 0.5f * float(std::sin(2.0 * 3.14159265358979 * 220.0 * double(i) / kSr));
    r = l;
    float maxJump = 0;
    float prev = 0;
    for (size_t b = 0; b < 400; ++b) {
        if (b == 200) set(*d, "time", 0);                      // 11025 -> 2756 samples mid-stream
        d->process(l.data() + b * kMaxBlock, r.data() + b * kMaxBlock, kMaxBlock, ctx(), {});
        if (b > 100) for (size_t i = 0; i < size_t(kMaxBlock); ++i) { maxJump = std::max(maxJump, std::abs(l[b * kMaxBlock + i] - prev)); prev = l[b * kMaxBlock + i]; }
        else prev = l[b * kMaxBlock + kMaxBlock - 1];
    }
    CHECK(maxJump < 0.25f);   // a hard switch of the read position would jump by up to ~1.0
}
