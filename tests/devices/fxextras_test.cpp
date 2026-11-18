// The controls added to ported effects after the synthyy port (ARCH 27): each is neutral at its default (the golden fixtures never set them)
// and does what it says when turned.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;
using Catch::Approx;

namespace {
void set(EffectDevice& d, const char* key, float v, bool lenient = false) {
    for (const auto& p : d.params()) if (std::string(p.key) == key) { d.setParam(p.index, v); return; }
    if (!lenient) FAIL("no parameter " << key);
}
using Gen = std::function<float(size_t)>;
std::vector<float> run(const std::string& type, const std::vector<std::pair<const char*, float>>& ps, double seconds, const Gen& in, bool lenient = false) {
    auto d = createEffect(type);
    d->prepare(kSr, kMaxBlock);
    for (auto [k, v] : ps) set(*d, k, v, lenient);
    d->reset();
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    const size_t total = size_t(seconds * kSr) / kMaxBlock * kMaxBlock;
    for (size_t t = 0; t < total; t += kMaxBlock) {
        for (int i = 0; i < kMaxBlock; ++i) l[size_t(i)] = r[size_t(i)] = in(t + size_t(i));
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}
double rms(const std::vector<float>& x, size_t from, size_t to) { return harness::rms(std::span<const float>(x).subspan(from, to - from)); }
Gen burst(double hz, double seconds, float amp = 0.5f) {       // a tone for `seconds`, then silence
    return [=](size_t t) { return double(t) / kSr < seconds ? amp * float(std::sin(2.0 * std::numbers::pi * hz * double(t) / kSr)) : 0.0f; };
}
Gen sine(double hz, float amp = 0.5f) { return [=](size_t t) { return amp * float(std::sin(2.0 * std::numbers::pi * hz * double(t) / kSr)); }; }
Gen impulse() { return [](size_t t) { return t == 0 ? 1.0f : 0.0f; }; }
double dftMag(const std::vector<float>& x, size_t from, size_t n, double hz) {
    double re = 0, im = 0;
    const double w = 2.0 * std::numbers::pi * hz / kSr;
    for (size_t i = 0; i < n; ++i) { re += double(x[from + i]) * std::cos(w * double(i)); im += double(x[from + i]) * std::sin(w * double(i)); }
    return std::sqrt(re * re + im * im);
}
}  // namespace

TEST_CASE("fx extras: the new controls are neutral at their defaults", "[device][fxextras]") {
    for (const char* t : {"delay", "pingpong", "reverb", "plate", "chorus", "phaser", "vib", "trem", "autopan", "autofilt"}) {
        INFO(t);
        const auto a = run(t, {}, 0.4, sine(330.0));
        const auto b = run(t, {{"phase", 0.0f}, {"shape", 0.0f}, {"tone", 20000.0f}, {"duck", 0.0f}, {"wobble", 0.0f}, {"lowcut", 20.0f}}, 0.4, sine(330.0), true);      // set explicitly to the defaults where they exist
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
    }
}

TEST_CASE("fx extras: delay and ping pong tone darkens the repeats, duck lowers them while playing, wobble moves the time", "[device][fxextras]") {
    for (const char* t : {"delay", "pingpong"}) {
        INFO(t);
        const std::vector<std::pair<const char*, float>> base = {{"time", 2.0f}, {"fb", 0.7f}, {"mix", 0.5f}};
        auto with = [&](std::vector<std::pair<const char*, float>> extra) { auto p = base; p.insert(p.end(), extra.begin(), extra.end()); return p; };
        // a 5 kHz burst, then only the repeats: the tone control rounds them off
        const auto bright = run(t, base, 3.0, burst(5000.0, 0.1));
        const auto dark = run(t, with({{"tone", 400.0f}}), 3.0, burst(5000.0, 0.1));
        const size_t from = size_t(1.0 * kSr), to = size_t(3.0 * kSr);
        CHECK(rms(dark, from, to) < 0.3 * rms(bright, from, to));
        // while a loud tone plays the echoes sink; the sum of wet and dry is quieter than without ducking
        const auto plain = run(t, with({{"duck", 0.0f}}), 2.0, sine(200.0, 0.6f));
        const auto ducked = run(t, with({{"duck", 1.0f}}), 2.0, sine(200.0, 0.6f));
        const double dry = 0.6 / std::sqrt(2.0) * std::cos(0.5 * std::numbers::pi / 2.0);
        CHECK(rms(ducked, size_t(1.5 * kSr), size_t(2.0 * kSr)) < rms(plain, size_t(1.5 * kSr), size_t(2.0 * kSr)) + 1e-9);
        CHECK(rms(ducked, size_t(1.5 * kSr), size_t(2.0 * kSr)) == Approx(dry).epsilon(0.1));        // the wet is gone, the dry remains
        // wobble: the repeats differ from the steady ones
        const auto steady = run(t, with({{"wobble", 0.0f}}), 1.5, burst(440.0, 0.2));
        const auto drift = run(t, with({{"wobble", 1.0f}}), 1.5, burst(440.0, 0.2));
        double diff = 0;
        for (size_t i = size_t(0.4 * kSr); i < steady.size(); ++i) diff += std::abs(double(steady[i]) - double(drift[i]));
        CHECK(diff > 1.0);
        for (float v : drift) REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("fx extras: reverb pre-delay, tone, low cut and duck; plate low cut and duck", "[device][fxextras]") {
    auto onset = [](const std::vector<float>& x) { for (size_t i = 0; i < x.size(); ++i) if (std::abs(x[i]) > 1e-4f && i > 100) return i; return x.size(); };
    const auto early = run("reverb", {{"mix", 1.0f}, {"predelay", 0.02f}}, 0.6, impulse());
    const auto late = run("reverb", {{"mix", 1.0f}, {"predelay", 0.15f}}, 0.6, impulse());
    CHECK(onset(late) > onset(early) + size_t(0.1 * kSr));                                   // 130 ms later
    const auto full = run("reverb", {{"mix", 1.0f}, {"size", 2.0f}}, 2.0, burst(100.0, 0.5, 0.3f));
    const auto cut = run("reverb", {{"mix", 1.0f}, {"size", 2.0f}, {"lowcut", 600.0f}}, 2.0, burst(100.0, 0.5, 0.3f));
    CHECK(dftMag(cut, size_t(0.6 * kSr), 16384, 100.0) < 0.3 * dftMag(full, size_t(0.6 * kSr), 16384, 100.0));
    const auto bright = run("reverb", {{"mix", 1.0f}, {"size", 2.0f}}, 2.0, burst(8000.0, 0.5, 0.3f));
    const auto dull = run("reverb", {{"mix", 1.0f}, {"size", 2.0f}, {"tone", 800.0f}}, 2.0, burst(8000.0, 0.5, 0.3f));
    CHECK(rms(dull, size_t(0.6 * kSr), size_t(1.5 * kSr)) < 0.3 * rms(bright, size_t(0.6 * kSr), size_t(1.5 * kSr)));
    for (const char* t : {"reverb", "plate"}) {
        INFO(t);
        const auto open = run(t, {{"mix", 1.0f}, {"duck", 0.0f}}, 1.5, sine(220.0, 0.7f));
        const auto ducked = run(t, {{"mix", 1.0f}, {"duck", 1.0f}}, 1.5, sine(220.0, 0.7f));
        CHECK(rms(ducked, size_t(1.0 * kSr), size_t(1.5 * kSr)) < 0.1 * rms(open, size_t(1.0 * kSr), size_t(1.5 * kSr)));
    }
    const auto plateFull = run("plate", {{"mix", 1.0f}}, 2.0, burst(100.0, 0.5, 0.3f));
    const auto plateCut = run("plate", {{"mix", 1.0f}, {"lowcut", 600.0f}}, 2.0, burst(100.0, 0.5, 0.3f));
    CHECK(dftMag(plateCut, size_t(0.6 * kSr), 16384, 100.0) < 0.4 * dftMag(plateFull, size_t(0.6 * kSr), 16384, 100.0));
}

TEST_CASE("fx extras: an LFO's phase moves the waveform and its shape changes it", "[device][fxextras]") {
    // a tremolo at 2 Hz, full depth: moving the phase by half a cycle swaps where the quiet parts fall
    const std::vector<std::pair<const char*, float>> base = {{"rate", 2.0f}, {"depth", 1.0f}, {"mix", 1.0f}};
    auto with = [&](float phase, float shape) { auto p = base; p.push_back({"phase", phase}); p.push_back({"shape", shape}); return run("trem", p, 1.0, sine(300.0)); };
    const auto a = with(0.0f, 0.0f), b = with(180.0f, 0.0f), c = with(0.0f, 4.0f);
    double dab = 0, dac = 0;
    for (size_t i = 0; i < a.size(); ++i) { dab += std::abs(double(a[i]) - double(b[i])); dac += std::abs(double(a[i]) - double(c[i])); }
    CHECK(dab > 100.0);
    CHECK(dac > 100.0);
    // a full turn is the same as none
    const auto d = with(360.0f, 0.0f);
    for (size_t i = 0; i < a.size(); ++i) REQUIRE(std::abs(a[i] - d[i]) < 1e-4f);
    // the other LFO effects respond to phase too
    for (const char* t : {"chorus", "phaser", "vib", "autopan", "autofilt"}) {
        INFO(t);
        const std::vector<std::pair<const char*, float>> p0 = {{"rate", 1.0f}, {"phase", 0.0f}}, p1 = {{"rate", 1.0f}, {"phase", 180.0f}};
        const auto x = run(t, p0, 1.0, sine(300.0)), y = run(t, p1, 1.0, sine(300.0));
        double diff = 0;
        for (size_t i = 0; i < x.size(); ++i) diff += std::abs(double(x[i]) - double(y[i]));
        CHECK(diff > 1.0);
    }
}
