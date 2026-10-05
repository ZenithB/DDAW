// fmop: the generic device gates, then analytic checks - pitch, the Bessel-function sideband levels of FM, alias
// rejection by the 4x oversampling, and the three audio-rate ports (sample-accurate, bypassing the smoothers).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("fmop"); }
void set(InstrumentDevice& d, const char* key, float v) { d.setParam(uint16_t(findParam(d.params(), key)), v); }

// Sustained note, parameters snapped (reset), envelopes flat: sustain 1, index envelope held at 1.
std::unique_ptr<InstrumentDevice> steady(int pitch, int algo) {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    set(*d, "algo", float(algo));
    set(*d, "attack", 0.001f); set(*d, "sustain", 1.0f); set(*d, "iSus", 1.0f); set(*d, "velIdx", 0.0f);
    return d;
}

std::vector<float> render(InstrumentDevice& d, int blocks, const ModInputs& mod = {}) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), mod);
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

// Hann-windowed single-bin amplitude (relative to a unit sine).
double bin(const std::vector<float>& x, size_t from, size_t len, double hz) {
    double re = 0, im = 0, wsum = 0;
    for (size_t i = 0; i < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(len));
        const double ph = 2.0 * std::numbers::pi * hz * double(from + i) / kSr;
        re += w * x[from + i] * std::cos(ph);
        im -= w * x[from + i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}
double midiHz(int p) { return 440.0 * std::pow(2.0, (p - 69) / 12.0); }

// Bessel function of the first kind, integer order, by its power series (small arguments).
double besselJ(int k, double x) {
    double term = 1.0, sum = 0.0;
    for (int i = 1; i <= std::abs(k); ++i) term *= (x / 2.0) / double(i);
    for (int m = 0; m < 60; ++m) {
        sum += term;
        term *= -(x / 2.0) * (x / 2.0) / (double(m + 1) * double(m + 1 + std::abs(k)));
    }
    return sum;
}
}  // namespace

TEST_CASE("fmop passes the generic instrument gates", "[fmop][device]") { checkInstrumentContract(make); }

TEST_CASE("fmop: the parameter table marks exactly index, pitch and amp as audio-rate, in that order", "[fmop]") {
    auto d = make();
    std::vector<std::string> a;
    for (const auto& p : d->params()) if (p.audioRate) a.emplace_back(p.key);
    REQUIRE(a.size() == 3);
    CHECK(a[0] == "index");
    CHECK(a[1] == "pitch");
    CHECK(a[2] == "amp");
}

TEST_CASE("fmop: a single carrier sounds at the note's pitch and reports the decimator's latency", "[fmop]") {
    auto d = steady(69, 4);   // additive: operator 1 alone
    for (const char* k : {"l2", "l3", "l4"}) set(*d, k, 0.0f);
    set(*d, "l1", 1.0f); set(*d, "r1", 1.0f);
    d->reset();
    d->noteOn(69, 1.0f, 1);
    const auto x = render(*d, 200);
    const size_t from = 4096, len = 16384;
    const double a440 = bin(x, from, len, 440.0);
    CHECK(a440 > 0.05);
    CHECK(bin(x, from, len, 330.0) < a440 * 0.01);
    CHECK(bin(x, from, len, 550.0) < a440 * 0.01);
    CHECK(d->latencySamples() == 13);
}

TEST_CASE("fmop: FM sidebands follow the Bessel functions of the modulation index", "[fmop]") {
    // serial wiring with operator 3 silenced: operator 2 (ratio 0.25, so 110 Hz) modulates operator 1 (440 Hz)
    auto d = steady(69, 0);
    set(*d, "r1", 1.0f); set(*d, "r2", 0.25f);
    set(*d, "l1", 1.0f); set(*d, "l2", 1.0f); set(*d, "l3", 0.0f); set(*d, "l4", 0.0f);
    const float beta = 1.5f;
    set(*d, "index", beta);
    d->reset();
    d->noteOn(69, 1.0f, 1);
    const auto x = render(*d, 300);
    const size_t from = 8192, len = 16384;
    const double ref = bin(x, from, len, 440.0 + 110.0);   // |J1|
    const double j1 = std::abs(besselJ(1, beta));
    for (int k : {0, 1, 2, 3}) {
        const double expect = std::abs(besselJ(k, beta)) / j1;
        const double hi = bin(x, from, len, 440.0 + 110.0 * k) / ref;
        const double lo = k == 0 ? hi : bin(x, from, len, 440.0 - 110.0 * k) / ref;
        INFO("sideband order " << k << ": expected " << expect << ", upper " << hi << ", lower " << lo);
        CHECK(hi == Catch::Approx(expect).epsilon(0.04).margin(0.01));
        CHECK(lo == Catch::Approx(expect).epsilon(0.04).margin(0.01));
    }
}

TEST_CASE("fmop: strong FM on a high note does not fold back into the audio band", "[fmop][alias]") {
    // 3322 Hz carrier and modulator, index 8: the 12th sideband sits at 39.9 kHz and would fold to 8.1 kHz. With
    // 4x oversampling and the 47/15 decimator it must be far below what an alias of its true level (J12(8) ~ 0.04
    // of the carrier) would be.
    const int pitch = 104;
    auto d = steady(pitch, 0);
    set(*d, "r1", 1.0f); set(*d, "r2", 1.0f);
    set(*d, "l1", 1.0f); set(*d, "l2", 1.0f); set(*d, "l3", 0.0f); set(*d, "l4", 0.0f);
    set(*d, "index", 8.0f);
    d->reset();
    d->noteOn(uint8_t(pitch), 1.0f, 1);
    const auto x = render(*d, 300);
    const double f = midiHz(pitch);
    const size_t from = 8192, len = 16384;
    double strongest = 0;
    for (int k = 1; k <= 7; ++k) strongest = std::max(strongest, bin(x, from, len, f * k));
    double worstAlias = 0;
    for (int k = 9; k <= 14; ++k) {                      // true frequency above 29.9 kHz -> folds below 18.1 kHz
        double fold = std::fmod(f * k, kSr);
        if (fold > kSr / 2) fold = kSr - fold;
        if (fold > 18000.0 || std::abs(fold / f - std::round(fold / f)) < 0.05) continue;   // skip bins that coincide with a real harmonic
        worstAlias = std::max(worstAlias, bin(x, from, len, fold));
    }
    INFO("strongest harmonic " << strongest << ", worst alias " << worstAlias);
    CHECK(worstAlias < strongest * 0.003);   // better than -50 dB
}

TEST_CASE("fmop: the amp port is amplitude modulation, sample by sample", "[fmop][arate]") {
    auto d = steady(69, 4);
    for (const char* k : {"l2", "l3", "l4"}) set(*d, k, 0.0f);
    d->reset();
    d->noteOn(69, 1.0f, 1);
    std::vector<float> buf(kMaxBlock), seq;
    std::vector<float> out;
    const double fm = 55.0;
    // amp = 1 + 0.5 sin(2 pi fm t)  ->  carrier at 440 with sidebands at 440 +- 55 of 25% each
    for (int b = 0; b < 250; ++b) {
        for (int i = 0; i < kMaxBlock; ++i) buf[size_t(i)] = 0.5f * float(std::sin(2.0 * std::numbers::pi * fm * double(b * kMaxBlock + i) / kSr));
        const float* ptrs[3] = {nullptr, nullptr, buf.data()};
        const auto blk = render(*d, 1, ModInputs{std::span<const float* const>(ptrs, 3)});
        out.insert(out.end(), blk.begin(), blk.end());
    }
    const size_t from = 4096, len = 16384;
    const double c = bin(out, from, len, 440.0);
    CHECK(bin(out, from, len, 440.0 + fm) / c == Catch::Approx(0.25).margin(0.02));
    CHECK(bin(out, from, len, 440.0 - fm) / c == Catch::Approx(0.25).margin(0.02));
}

TEST_CASE("fmop: the pitch port is in semitones, and an unconnected port changes nothing", "[fmop][arate]") {
    auto up = steady(57, 4);
    for (const char* k : {"l2", "l3", "l4"}) set(*up, k, 0.0f);
    up->reset();
    up->noteOn(57, 1.0f, 1);
    std::vector<float> buf(kMaxBlock, 12.0f);                       // +12 semitones: 220 Hz -> 440 Hz
    const float* ptrs[3] = {nullptr, buf.data(), nullptr};
    const auto x = render(*up, 250, ModInputs{std::span<const float* const>(ptrs, 3)});
    CHECK(bin(x, 4096, 16384, 440.0) > 0.05);
    CHECK(bin(x, 4096, 16384, 220.0) < 0.01);

    // a null entry, an empty span and a short span all mean "unmodulated" and give the same sound
    auto a = steady(57, 0), b = steady(57, 0), c = steady(57, 0);
    for (auto* dd : {a.get(), b.get(), c.get()}) { dd->reset(); dd->noteOn(57, 0.8f, 1); }
    const float* none[3] = {nullptr, nullptr, nullptr};
    const float* shortSpan[1] = {nullptr};
    const auto xa = render(*a, 20);
    const auto xb = render(*b, 20, ModInputs{std::span<const float* const>(none, 3)});
    const auto xc = render(*c, 20, ModInputs{std::span<const float* const>(shortSpan, 1)});
    CHECK(xa == xb);
    CHECK(xa == xc);
}

TEST_CASE("fmop: the index port bypasses the smoother - a step lands within the same block", "[fmop][arate]") {
    const auto play = [](bool stepped) {
        auto d = steady(69, 0);
        set(*d, "r2", 1.0f); set(*d, "l2", 1.0f); set(*d, "l3", 0.0f); set(*d, "l4", 0.0f); set(*d, "index", 0.0f);
        d->reset();
        d->noteOn(69, 1.0f, 1);
        std::vector<float> buf(kMaxBlock, 0.0f);
        for (int i = 64; i < kMaxBlock; ++i) buf[size_t(i)] = stepped ? 6.0f : 0.0f;
        const float* ptrs[3] = {buf.data(), nullptr, nullptr};
        return render(*d, 1, ModInputs{std::span<const float* const>(ptrs, 3)});
    };
    const auto flat = play(false), step = play(true);
    // output is delayed by the decimator (13 samples) and its filter looks back, so the change cannot show before ~sample 60
    for (int i = 0; i < 60; ++i) REQUIRE(flat[size_t(i)] == step[size_t(i)]);
    double diff = 0;
    for (int i = 70; i < kMaxBlock; ++i) diff += std::abs(double(flat[size_t(i)]) - step[size_t(i)]);
    CHECK(diff > 1.0);   // a 15 ms smoother could not have moved this far in 58 samples
}
