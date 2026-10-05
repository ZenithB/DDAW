// waveshaper: the generic device gates, then analytic checks of each transfer function against closed forms (soft clip,
// the Bessel spectrum of the sine fold, exact Chebyshev harmonics, the drawn curve), the level held as the drive falls, the
// bias and DC blocker, the drive envelope and velocity, aliasing under hard clipping, the three audio-rate ports and
// per-note expression.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "B6Kit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;
using namespace ddaw::testkit::b6;
using Catch::Approx;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("waveshaper"); }

// A steady sine through the soft clipper, drive envelope flat.
std::unique_ptr<InstrumentDevice> steady() {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    set(*d, "src", 0.0f); set(*d, "shape", 0.0f); set(*d, "drive", 1.0f); set(*d, "bias", 0.0f);
    set(*d, "dSus", 1.0f); set(*d, "velDrive", 0.0f);
    set(*d, "attack", 0.001f); set(*d, "decay", 0.01f); set(*d, "sustain", 1.0f); set(*d, "release", 0.05f);
    set(*d, "level", 1.0f);
    d->reset();
    return d;
}
constexpr size_t kLen = 16384, kFrom = 22050;
std::vector<float> note(InstrumentDevice& d, int pitch, float vel = 1.0f, int blocks = 320) {
    d.noteOn(uint8_t(pitch), vel, 1);
    return render(d, blocks);
}
double partial(const std::vector<float>& x, int pitch, int k) { return bin(x, kFrom, kLen, k * hz(pitch)); }
double mean(const std::vector<float>& x, size_t from, size_t len) { double s = 0; for (size_t i = from; i < from + len; ++i) s += x[i]; return s / double(len); }

double besselJ(int k, double x) {
    double term = 1.0, sum = 0.0;
    for (int i = 1; i <= std::abs(k); ++i) term *= (x / 2.0) / double(i);
    for (int m = 0; m < 80; ++m) { sum += term; term *= -(x / 2.0) * (x / 2.0) / (double(m + 1) * double(m + 1 + std::abs(k))); }
    return sum;
}
}  // namespace

TEST_CASE("waveshaper passes the generic instrument gates", "[waveshaper][device]") { checkInstrumentContract(make); }

TEST_CASE("waveshaper: the soft clip keeps its level while the drive rises and adds only odd harmonics", "[waveshaper]") {
    auto d = steady();
    double a1[3]; int i = 0;
    for (float drive : {0.05f, 1.0f, 8.0f}) {
        set(*d, "drive", drive); d->reset();
        const auto x = note(*d, 45);
        a1[i] = partial(x, 45, 1);
        INFO("drive " << drive);
        CHECK(db(partial(x, 45, 2) / a1[i]) < -60.0);                      // symmetric: no even harmonics
        float mx = 0; for (size_t j = kFrom; j < kFrom + kLen; ++j) mx = std::max(mx, std::abs(x[j]));
        CHECK(mx <= 0.6f);                                                 // half scale, plus the halfband filter's ringing
        if (i == 0) CHECK(db(partial(x, 45, 3) / a1[i]) < -45.0);          // nearly a clean sine
        if (i == 2) CHECK(db(partial(x, 45, 3) / a1[i]) > -14.0);          // heavily driven: a rounded square
        ++i;
    }
    CHECK(a1[0] == Approx(0.5).epsilon(0.05));
    CHECK(a1[1] == Approx(0.5).epsilon(0.15));
}

TEST_CASE("waveshaper: bias makes even harmonics and the DC it creates is blocked", "[waveshaper]") {
    auto d = steady();
    set(*d, "drive", 2.0f); d->reset();
    auto x = note(*d, 45);
    CHECK(db(partial(x, 45, 2) / partial(x, 45, 1)) < -60.0);
    set(*d, "bias", 0.5f); d->reset();
    x = note(*d, 45);
    CHECK(db(partial(x, 45, 2) / partial(x, 45, 1)) > -25.0);
    CHECK(std::abs(mean(x, kFrom, kLen)) < 2e-3);
}

TEST_CASE("waveshaper: the sine fold has the Bessel spectrum sin(b sin t) = 2 sum J_(2m+1)(b) sin((2m+1)t)", "[waveshaper]") {
    auto d = steady();
    set(*d, "shape", 1.0f); set(*d, "drive", 3.0f); d->reset();
    const auto x = note(*d, 45);
    const double b = 3.0 * std::numbers::pi / 2.0;
    const double j1 = std::abs(besselJ(1, b)), j3 = std::abs(besselJ(3, b)), j5 = std::abs(besselJ(5, b));
    CHECK(partial(x, 45, 1) == Approx(0.5 * 2.0 * j1).epsilon(0.03));
    CHECK(partial(x, 45, 3) / partial(x, 45, 1) == Approx(j3 / j1).epsilon(0.04));
    CHECK(partial(x, 45, 5) / partial(x, 45, 1) == Approx(j5 / j1).epsilon(0.05));
    CHECK(db(partial(x, 45, 2) / partial(x, 45, 1)) < -60.0);
}

TEST_CASE("waveshaper: Chebyshev weights put exactly their share into each harmonic", "[waveshaper]") {
    auto d = steady();
    set(*d, "shape", 2.0f); set(*d, "drive", 1.0f);
    set(*d, "h2", 0.5f); set(*d, "h3", 0.3f); set(*d, "h4", 0.2f); set(*d, "h5", 0.1f); d->reset();
    const auto x = note(*d, 45);
    const double sum = 1.0 + 0.5 + 0.3 + 0.2 + 0.1;
    CHECK(partial(x, 45, 1) == Approx(0.5 / sum).epsilon(0.03));
    CHECK(partial(x, 45, 2) / partial(x, 45, 1) == Approx(0.5).epsilon(0.03));
    CHECK(partial(x, 45, 3) / partial(x, 45, 1) == Approx(0.3).epsilon(0.03));
    CHECK(partial(x, 45, 4) / partial(x, 45, 1) == Approx(0.2).epsilon(0.04));
    CHECK(partial(x, 45, 5) / partial(x, 45, 1) == Approx(0.1).epsilon(0.05));
    CHECK(db(partial(x, 45, 6) / partial(x, 45, 1)) < -55.0);
    for (const char* k : {"h2", "h3", "h4", "h5"}) set(*d, k, 0.0f);
    d->reset();
    const auto pure = note(*d, 45);
    CHECK(db(partial(pure, 45, 2) / partial(pure, 45, 1)) < -60.0);
    CHECK(db(partial(pure, 45, 3) / partial(pure, 45, 1)) < -60.0);
}

TEST_CASE("waveshaper: the diode is asymmetric, the hard clip is square", "[waveshaper]") {
    auto d = steady();
    set(*d, "shape", 3.0f); set(*d, "drive", 2.0f); d->reset();
    auto x = note(*d, 45);
    CHECK(db(partial(x, 45, 2) / partial(x, 45, 1)) > -25.0);
    CHECK(std::abs(mean(x, kFrom, kLen)) < 2e-3);
    set(*d, "shape", 4.0f); set(*d, "drive", 8.0f); d->reset();
    x = note(*d, 45);
    CHECK(db(partial(x, 45, 2) / partial(x, 45, 1)) < -60.0);
    CHECK(partial(x, 45, 3) / partial(x, 45, 1) == Approx(1.0 / 3.0).epsilon(0.1));    // a square wave
    float mx = 0; for (size_t j = kFrom; j < kFrom + kLen; ++j) mx = std::max(mx, std::abs(x[j]));
    CHECK(mx <= 0.6f);
}

TEST_CASE("waveshaper: the drawn curve is the transfer function", "[waveshaper]") {
    auto d = steady();
    set(*d, "shape", 5.0f); set(*d, "drive", 1.0f);
    for (auto [k, v] : {std::pair{"k1", -1.0f}, {"k2", -0.5f}, {"k3", 0.0f}, {"k4", 0.5f}, {"k5", 1.0f}}) set(*d, k, v);
    d->reset();
    auto x = note(*d, 45);
    CHECK(db(partial(x, 45, 3) / partial(x, 45, 1)) < -50.0);                           // a straight line: a clean sine
    CHECK(partial(x, 45, 1) == Approx(0.5).epsilon(0.03));
    for (auto [k, v] : {std::pair{"k1", -1.0f}, {"k2", -1.0f}, {"k3", 0.0f}, {"k4", 1.0f}, {"k5", 1.0f}}) set(*d, k, v);
    d->reset();
    x = note(*d, 45);
    CHECK(db(partial(x, 45, 3) / partial(x, 45, 1)) > -25.0);                           // an S-curve distorts, symmetrically
    CHECK(db(partial(x, 45, 2) / partial(x, 45, 1)) < -60.0);
}

TEST_CASE("waveshaper: the drive falls over each note and harder notes drive harder", "[waveshaper]") {
    auto d = steady();
    set(*d, "drive", 6.0f); set(*d, "dDecay", 0.4f); set(*d, "dSus", 0.0f); d->reset();
    const auto x = note(*d, 45);
    const double early = powerAbove(x, 100, 2048, 330.0), late = powerAbove(x, kFrom, kLen, 330.0);
    CHECK(early > 0.05);
    CHECK(late < 1e-3);                                   // settled on a plain sine
    set(*d, "drive", 3.0f); set(*d, "dSus", 1.0f); set(*d, "velDrive", 1.0f); d->reset();
    const auto hard = note(*d, 45, 1.0f), soft = note(*d, 45, 0.3f);
    CHECK(partial(hard, 45, 3) / partial(hard, 45, 1) > 3.0 * partial(soft, 45, 3) / partial(soft, 45, 1));
}

TEST_CASE("waveshaper: the sources are a sine, a triangle and a band-limited saw", "[waveshaper]") {
    auto d = steady();
    set(*d, "shape", 4.0f); set(*d, "drive", 0.05f); d->reset();               // hard clip at a tiny drive: the source, normalised
    auto x = note(*d, 45);
    CHECK(db(partial(x, 45, 2) / partial(x, 45, 1)) < -60.0);
    set(*d, "src", 1.0f); d->reset();
    x = note(*d, 45);
    CHECK(partial(x, 45, 3) / partial(x, 45, 1) == Approx(1.0 / 9.0).epsilon(0.05));
    set(*d, "src", 2.0f); d->reset();
    x = note(*d, 45);
    CHECK(partial(x, 45, 2) / partial(x, 45, 1) == Approx(0.5).epsilon(0.05));
}

TEST_CASE("waveshaper: a hard-clipped high note does not alias", "[waveshaper]") {
    auto d = steady();
    set(*d, "shape", 4.0f); set(*d, "drive", 10.0f); d->reset();
    const int p = 90;                                       // 1480 Hz: a square wave's partials run far past the Nyquist
    const double f = hz(p);
    const auto x = note(*d, p);
    const double a1 = bin(x, kFrom, kLen, f);
    for (int k = 15; k <= 45; ++k) {
        if (k * f < 26000.0) continue;                      // just past the Nyquist is the oversampler's transition band
        double fa = std::fmod(k * f, kSr);
        if (fa > kSr / 2) fa = kSr - fa;
        if (std::abs(fa / f - std::round(fa / f)) < 0.03) continue;
        INFO("partial " << k << " folds to " << fa);
        CHECK(db(bin(x, kFrom, kLen, fa) / a1) < -45.0);
    }
}

TEST_CASE("waveshaper: the audio-rate ports act like moving the parameter", "[waveshaper][arate]") {
    auto check = [&](const char* key, float base, float add, size_t slot, float pitchSemi = 0.0f) {
        auto a = steady(), b = steady();
        set(*a, "drive", 1.0f); set(*b, "drive", 1.0f);
        set(*a, "shape", 0.0f); set(*b, "shape", 0.0f);
        (void)pitchSemi;
        set(*a, key, base + add); a->reset();
        set(*b, key, base); b->reset();
        std::vector<float> buf(kMaxBlock, add);
        const float* ptrs[3] = {nullptr, nullptr, nullptr};
        ptrs[slot] = buf.data();
        ModInputs mod{std::span<const float* const>(ptrs, 3)};
        const auto xa = note(*a, 45);
        b->noteOn(45, 1.0f, 1);
        const auto xb = render(*b, 320, mod);
        double diff = 0; for (size_t i = kFrom; i < kFrom + kLen; ++i) diff = std::max(diff, double(std::abs(xa[i] - xb[i])));
        INFO(key);
        CHECK(diff < 2e-4);
        CHECK(rms(xa, kFrom, kLen) > 0.1);
    };
    check("drive", 0.5f, 3.0f, 0);
    check("bias", 0.0f, 0.4f, 1);
    {   // pitch: +12 semitones is an octave up
        auto d = steady();
        std::vector<float> buf(kMaxBlock, 12.0f);
        const float* ptrs[3] = {nullptr, nullptr, buf.data()};
        ModInputs mod{std::span<const float* const>(ptrs, 3)};
        d->noteOn(57, 1.0f, 1);
        const auto x = render(*d, 320, mod);
        CHECK(peak(x, kFrom, kLen, 2 * hz(57), 1.0) > 0.3);
        CHECK(db(bin(x, kFrom, kLen, hz(57)) / peak(x, kFrom, kLen, 2 * hz(57), 1.0)) < -50.0);
    }
}

TEST_CASE("waveshaper: the envelope attacks, sustains and releases, and the oversampler's delay is reported", "[waveshaper]") {
    auto d = steady();
    CHECK(d->latencySamples() == 13);
    set(*d, "drive", 0.05f); set(*d, "attack", 0.1f); set(*d, "decay", 0.2f); set(*d, "sustain", 0.5f); set(*d, "release", 0.2f); d->reset();
    d->noteOn(57, 1.0f, 1);
    const auto x = render(*d, 400);
    const double early = rms(x, 0, 441), top = rms(x, 4000, 441), sus = rms(x, 40000, 4410);
    CHECK(early < 0.3 * top);
    CHECK(sus == Approx(0.5 * top).epsilon(0.15));
    d->noteOff(1);
    CHECK(rms(render(*d, 200), 12000, 1024) < 0.02 * sus);
}

TEST_CASE("waveshaper: per-note expression bends, drives harder and swells only its note", "[waveshaper][mpe]") {
    auto d = steady();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 2, 12.0f);
    auto x = render(*d, 320);
    CHECK(peak(x, kFrom, kLen, 2 * hz(57), 2.0) > 0.2);
    d->reset();
    d->noteOn(57, 1.0f, 1);
    const auto plain = render(*d, 320);
    d->reset();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 0, 1.0f);
    const auto slid = render(*d, 320);
    CHECK(partial(slid, 57, 3) / partial(slid, 57, 1) > 3.0 * partial(plain, 57, 3) / partial(plain, 57, 1));
    d->reset();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 1, 1.0f);
    CHECK(rms(render(*d, 320), kFrom, kLen) / rms(plain, kFrom, kLen) == Approx(1.5).epsilon(0.05));
    d->reset();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(99, 2, 12.0f);
    d->noteExpression(1, 2, 0.0f); d->noteExpression(1, 1, 0.0f); d->noteExpression(1, 0, 0.0f);
    CHECK(render(*d, 320) == plain);
}
