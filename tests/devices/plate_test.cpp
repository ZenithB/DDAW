#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<EffectDevice> make() { return createEffect("plate"); }

int idx(EffectDevice& d, const char* key) { return findParam(d.params(), key); }
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(idx(d, key)), v); }

std::unique_ptr<EffectDevice> configured(float decay, float predelay, float damp, float mix) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "decay", decay); set(*d, "predelay", predelay); set(*d, "damp", damp); set(*d, "mix", mix);
    d->reset();
    return d;
}

std::pair<std::vector<float>, std::vector<float>> impulse(EffectDevice& d, float secs) {
    const size_t n = size_t(secs * float(kSr));
    std::vector<float> l(n, 0.0f), r(n, 0.0f);
    l[0] = r[0] = 1.0f;
    for (size_t i = 0; i < n; i += kMaxBlock)
        d.process(l.data() + i, r.data() + i, int(std::min<size_t>(kMaxBlock, n - i)), ctx(), {});
    return {l, r};
}

double rmsWin(const std::vector<float>& x, double t0, double t1) {
    const size_t a = size_t(t0 * kSr), b = std::min(x.size(), size_t(t1 * kSr));
    double e = 0; for (size_t i = a; i < b; ++i) e += double(x[i]) * x[i];
    return std::sqrt(e / double(b - a));
}
double db(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }

// RMS spectral frequency via Parseval (derivative energy over energy): a brightness measure.
double rmsFreq(const std::vector<float>& x) {
    double e = 0, de = 0;
    for (size_t i = 1; i < x.size(); ++i) { const double dd = double(x[i]) - x[i - 1]; de += dd * dd; e += double(x[i]) * x[i]; }
    return kSr / (2.0 * 3.14159265358979) * std::sqrt(de / std::max(e, 1e-30));
}
}  // namespace

TEST_CASE("plate: device contract", "[device][plate]") { checkEffectContract(make); }

TEST_CASE("plate: dense wet onset and a ringing, decorrelated tail", "[device][plate]") {
    auto d = configured(1.8f, 0.02f, 6000.0f, 0.5f);
    auto [l, r] = impulse(*d, 2.0f);
    CHECK(rmsWin(l, 0.025, 0.06) > 1e-6);
    CHECK(rmsWin(l, 0.3, 0.8) > 1e-7);
    CHECK(rmsWin(r, 0.3, 0.8) > 1e-7);
    double diff = 0;
    for (size_t i = size_t(0.1 * kSr); i < size_t(0.8 * kSr); ++i) diff += double(l[i] - r[i]) * (l[i] - r[i]);
    CHECK(diff > 1e-8);
}

TEST_CASE("plate: tail decay tracks the decay parameter", "[device][plate]") {
    auto dropOf = [](float decay) {
        auto d = configured(decay, 0.0f, 16000.0f, 1.0f);
        auto [l, r] = impulse(*d, 2.0f);
        return std::pair{db(rmsWin(l, 0.05, 0.25)) - db(rmsWin(l, 1.2, 1.5)), rmsWin(l, 1.2, 1.5)};
    };
    const auto [dropShort, lateShort] = dropOf(0.5f);
    const auto [dropLong, lateLong] = dropOf(4.0f);
    CHECK(dropShort > dropLong + 30.0);
    CHECK(dropLong > 5.0);
    CHECK(lateLong > 10.0 * lateShort);
}

TEST_CASE("plate: damp darkens the tail", "[device][plate]") {
    auto bright = [](float damp) {
        auto d = configured(1.8f, 0.0f, damp, 1.0f);
        auto [l, r] = impulse(*d, 1.0f);
        return rmsFreq(l);
    };
    CHECK(bright(16000.0f) > 1.3 * bright(500.0f));
}

TEST_CASE("plate: pre-delay shifts the onset", "[device][plate]") {
    auto onset = [](float pre) {
        auto d = configured(1.8f, pre, 16000.0f, 1.0f);
        auto [l, r] = impulse(*d, 0.5f);
        size_t i = 0; while (i < l.size() && std::abs(l[i]) <= 1e-4f) ++i;
        return i;
    };
    const double shift = double(onset(0.15f) - onset(0.0f)) / kSr;
    CHECK(shift == Catch::Approx(0.15).margin(0.01));
}

TEST_CASE("plate: wet = mix, dry = 1 - mix", "[device][plate]") {
    {
        auto d = configured(1.8f, 0.02f, 6000.0f, 0.0f);
        std::vector<float> l(128, 0.5f), r(128, -0.5f);
        d->process(l.data(), r.data(), 128, ctx(), {});
        for (float x : l) CHECK(std::abs(x - 0.5f) < 1e-6f);
        for (float x : r) CHECK(std::abs(x + 0.5f) < 1e-6f);
    }
    {
        auto d = configured(1.8f, 0.02f, 6000.0f, 1.0f);
        auto [l, r] = impulse(*d, 0.05f);
        CHECK(std::abs(l[0]) < 1e-6f);   // no dry at mix 1
    }
}
