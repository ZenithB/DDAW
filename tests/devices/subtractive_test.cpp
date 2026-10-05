// subtractive: the generic device gates, then analytic checks - the oscillator spectra, pulse width, detune and sub,
// the filter's slope, resonance, key tracking, envelope and velocity response, the amplitude envelope, the two audio-rate
// ports (equivalent to moving the parameter) and per-note expression.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "B6Kit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;
using namespace ddaw::testkit::b6;
using Catch::Approx;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("subtractive"); }

// One clean oscillator, filter wide open, flat envelopes.
std::unique_ptr<InstrumentDevice> clean() {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    set(*d, "wave1", 0.0f); set(*d, "mix2", 0.0f); set(*d, "sub", 0.0f); set(*d, "noise", 0.0f);
    set(*d, "cutoff", 16000.0f); set(*d, "res", 0.0f); set(*d, "drive", 0.0f); set(*d, "keytrack", 0.0f);
    set(*d, "envAmt", 0.0f); set(*d, "velFilt", 0.0f);
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
}  // namespace

TEST_CASE("subtractive passes the generic instrument gates", "[subtractive][device]") { checkInstrumentContract(make); }

TEST_CASE("subtractive: the oscillators have the spectra of their shapes", "[subtractive]") {
    auto d = clean();
    // saw: 1/k
    auto x = note(*d, 57);
    CHECK(partial(x, 57, 2) / partial(x, 57, 1) == Approx(0.5).epsilon(0.05));
    CHECK(partial(x, 57, 3) / partial(x, 57, 1) == Approx(1.0 / 3.0).epsilon(0.05));
    CHECK(partial(x, 57, 1) > 0.3);
    // square: odd partials only
    set(*d, "wave1", 1.0f); d->reset();
    x = note(*d, 57);
    CHECK(db(partial(x, 57, 2) / partial(x, 57, 1)) < -50.0);
    CHECK(partial(x, 57, 3) / partial(x, 57, 1) == Approx(1.0 / 3.0).epsilon(0.05));
    // a quarter-width pulse has no 4th partial
    set(*d, "pw", 0.25f); d->reset();
    x = note(*d, 57);
    CHECK(db(partial(x, 57, 4) / partial(x, 57, 1)) < -40.0);
    CHECK(partial(x, 57, 2) / partial(x, 57, 1) == Approx(0.7071).epsilon(0.06));
    // triangle: odd partials, 1/k^2
    set(*d, "wave1", 2.0f); d->reset();
    x = note(*d, 57);
    CHECK(partial(x, 57, 3) / partial(x, 57, 1) == Approx(1.0 / 9.0).epsilon(0.08));
    CHECK(db(partial(x, 57, 2) / partial(x, 57, 1)) < -40.0);
    // sine: one partial
    set(*d, "wave1", 3.0f); d->reset();
    x = note(*d, 57);
    CHECK(db(partial(x, 57, 2) / partial(x, 57, 1)) < -60.0);
    CHECK(db(partial(x, 57, 3) / partial(x, 57, 1)) < -40.0);   // the ladder's saturator, at a quarter of full scale
}

TEST_CASE("subtractive: the second oscillator adds a transposed and detuned voice, the sub an octave below", "[subtractive]") {
    auto d = clean();
    set(*d, "wave1", 3.0f); set(*d, "wave2", 3.0f); set(*d, "mix2", 1.0f); set(*d, "semi2", 12.0f); set(*d, "detune2", 0.0f); d->reset();
    auto x = note(*d, 57);
    CHECK(partial(x, 57, 2) / partial(x, 57, 1) == Approx(1.0).epsilon(0.05));       // equal weights at mix2 = 1
    set(*d, "semi2", 0.0f); set(*d, "detune2", 30.0f); d->reset();
    x = note(*d, 57);
    const double f = hz(57), fd = f * std::pow(2.0, 30.0 / 1200.0);
    CHECK(peak(x, kFrom, kLen, fd, 1.0) / peak(x, kFrom, kLen, f, 1.0) == Approx(1.0).epsilon(0.1));
    set(*d, "mix2", 0.0f); set(*d, "detune2", 0.0f); set(*d, "sub", 1.0f); d->reset();
    x = note(*d, 57);
    // a square one octave down at half weight: its fundamental is 4/pi * 0.5 of a unit sine
    CHECK(bin(x, kFrom, kLen, f * 0.5) / bin(x, kFrom, kLen, f) == Approx(0.6366).epsilon(0.1));
}

TEST_CASE("subtractive: noise adds broadband sound that the filter shapes", "[subtractive]") {
    auto d = clean();
    set(*d, "wave1", 3.0f); set(*d, "noise", 1.0f); set(*d, "cutoff", 16000.0f); d->reset();
    const auto bright = note(*d, 57);
    set(*d, "cutoff", 500.0f); d->reset();
    const auto dark = note(*d, 57);
    const auto n1 = minus(bright, [&] { set(*d, "noise", 0.0f); set(*d, "cutoff", 16000.0f); d->reset(); return note(*d, 57); }());
    CHECK(rms(n1, kFrom, kLen) > 0.05);
    CHECK(centroid(bright, kFrom, kLen, 300.0, 18000.0) > 2.0 * centroid(dark, kFrom, kLen, 300.0, 18000.0));
}

TEST_CASE("subtractive: the filter rolls off at 12 or 24 dB per octave and resonates", "[subtractive]") {
    auto d = clean();
    set(*d, "cutoff", 1000.0f); d->reset();
    // a saw's partial 8 (1760 Hz) and 16 (3520 Hz) are an octave apart, 1/8 and 1/16 unfiltered
    auto slope = [&](float s) {
        set(*d, "slope", s); d->reset();
        const auto x = note(*d, 57);
        return db((partial(x, 57, 16) / partial(x, 57, 8)) / 0.5);
    };
    const double s24 = slope(1.0f), s12 = slope(0.0f);
    INFO(s24 << " " << s12);
    CHECK(s24 == Approx(-24.0).margin(5.0));
    CHECK(s12 == Approx(-12.0).margin(4.0));
    // resonance: a peak at the cutoff (a saw at 55 Hz has partial 18 at 990 Hz)
    set(*d, "slope", 1.0f); set(*d, "res", 0.0f); d->reset();
    const auto flat = note(*d, 33);
    set(*d, "res", 0.9f); d->reset();
    const auto reson = note(*d, 33);
    CHECK(partial(reson, 33, 18) > 2.0 * partial(flat, 33, 18));
}

TEST_CASE("subtractive: key tracking carries the filter with the note", "[subtractive]") {
    auto d = clean();
    set(*d, "cutoff", 800.0f); d->reset();
    auto tilt = [&](float track, int pitch) {
        set(*d, "keytrack", track); d->reset();
        const auto x = note(*d, pitch);
        return partial(x, pitch, 8) / partial(x, pitch, 1);           // how much of the 8th survives
    };
    const double lo0 = tilt(0.0f, 48), hi0 = tilt(0.0f, 60), lo1 = tilt(1.0f, 48), hi1 = tilt(1.0f, 60);
    CHECK(hi0 < 0.6 * lo0);                                            // fixed filter: the higher note is duller
    CHECK(hi1 == Approx(lo1).epsilon(0.1));                            // tracking: the same shape an octave up
}

TEST_CASE("subtractive: the filter envelope sweeps the cutoff and velocity opens it", "[subtractive]") {
    auto d = clean();
    set(*d, "cutoff", 300.0f); set(*d, "envAmt", 1.0f); set(*d, "fAttack", 0.001f); set(*d, "fDecay", 0.15f); set(*d, "fSustain", 0.0f); d->reset();
    auto x = note(*d, 45);
    const double early = powerAbove(x, 100, 2048, 400.0), late = powerAbove(x, 22050, kLen, 400.0);
    CHECK(early > 0.01);                  // a saw at 110 Hz has about 14% of its power above 400 Hz when the filter is open
    CHECK(early > 100.0 * late);
    set(*d, "envAmt", 0.0f); set(*d, "velFilt", 1.0f); d->reset();
    const auto loud = note(*d, 45, 1.0f), soft = note(*d, 45, 0.2f);
    // the share of the power above 800 Hz does not depend on the level: the harder note is brighter
    CHECK(powerAbove(loud, kFrom, kLen, 400.0) > 4.0 * powerAbove(soft, kFrom, kLen, 400.0));
}

TEST_CASE("subtractive: the amplitude envelope attacks, sustains and releases", "[subtractive]") {
    auto d = clean();
    set(*d, "wave1", 3.0f); set(*d, "attack", 0.1f); set(*d, "decay", 0.2f); set(*d, "sustain", 0.5f); set(*d, "release", 0.2f); d->reset();
    d->noteOn(57, 1.0f, 1);
    const auto x = render(*d, 400);
    const double early = rms(x, 0, 441), peakLvl = rms(x, 4000, 441), sus = rms(x, 40000, 4410);
    CHECK(early < 0.3 * peakLvl);
    CHECK(sus == Approx(0.5 * peakLvl).epsilon(0.15));
    d->noteOff(1);
    const auto y = render(*d, 200);
    CHECK(rms(y, 12000, 1024) < 0.02 * sus);
}

TEST_CASE("subtractive: the audio-rate ports act like moving the parameter, sample by sample", "[subtractive][arate]") {
    // pitch: a constant +12 semitones is an octave up
    {
        auto d = clean();
        set(*d, "wave1", 3.0f); d->reset();
        std::vector<float> buf(kMaxBlock, 12.0f);
        const float* ptrs[2] = {nullptr, buf.data()};
        ModInputs mod{std::span<const float* const>(ptrs, 2)};
        d->noteOn(57, 1.0f, 1);
        const auto x = render(*d, 320, mod);
        CHECK(peak(x, kFrom, kLen, 2 * hz(57), 1.0) > 0.5);
        CHECK(db(bin(x, kFrom, kLen, hz(57)) / peak(x, kFrom, kLen, 2 * hz(57), 1.0)) < -50.0);
    }
    // cutoff: 500 Hz plus a constant 1500 Hz from the port sounds as the parameter at 2000 Hz
    {
        auto a = clean(), b = clean();
        set(*a, "cutoff", 2000.0f); a->reset();
        set(*b, "cutoff", 500.0f); b->reset();
        std::vector<float> buf(kMaxBlock, 1500.0f);
        const float* ptrs[2] = {buf.data(), nullptr};
        ModInputs mod{std::span<const float* const>(ptrs, 2)};
        const auto xa = note(*a, 45);
        b->noteOn(45, 1.0f, 1);
        const auto xb = render(*b, 320, mod);
        double diff = 0; for (size_t i = kFrom; i < kFrom + kLen; ++i) diff = std::max(diff, double(std::abs(xa[i] - xb[i])));
        CHECK(diff < 2e-3);
        CHECK(rms(xa, kFrom, kLen) > 0.1);
    }
    // a fast sine on the cutoff port is audible modulation: sidebands around the partials
    {
        auto d = clean();
        set(*d, "cutoff", 1500.0f); d->reset();
        std::vector<float> buf(kMaxBlock);
        const float* ptrs[2] = {buf.data(), nullptr};
        ModInputs mod{std::span<const float* const>(ptrs, 2)};
        d->noteOn(45, 1.0f, 1);
        std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
        double ph = 0;
        for (int b = 0; b < 480; ++b) {
            for (auto& v : buf) { v = 900.0f * float(std::sin(ph)); ph += 2.0 * 3.14159265358979 * 40.0 / kSr; }
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            d->process(l.data(), r.data(), kMaxBlock, ctx(), mod);
            out.insert(out.end(), l.begin(), l.end());
        }
        const double f = hz(45) * 12;                                   // partial 12: well inside the sweep
        CHECK(bin(out, kFrom, 32768, f + 40.0) > 0.05 * peak(out, kFrom, 32768, f, 1.0));
    }
}

TEST_CASE("subtractive: high notes do not alias", "[subtractive]") {
    auto d = clean();
    const int pitch = 96;                                               // 2093 Hz
    const double f = hz(pitch);
    const auto x = note(*d, pitch);
    const double a1 = bin(x, kFrom, kLen, f);
    for (int k = 10; k <= 20; ++k) {
        double fa = std::fmod(k * f, kSr);
        if (fa > kSr / 2) fa = kSr - fa;
        if (std::abs(fa / f - std::round(fa / f)) < 0.03) continue;
        INFO("partial " << k << " folds to " << fa);
        CHECK(db(bin(x, kFrom, kLen, fa) / a1) < -35.0);
    }
}

TEST_CASE("subtractive: per-note expression bends, opens the filter and swells only its note", "[subtractive][mpe]") {
    auto d = clean();
    set(*d, "wave1", 3.0f); d->reset();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 2, 12.0f);
    auto x = render(*d, 320);
    CHECK(peak(x, kFrom, kLen, 2 * hz(57), 2.0) > 0.4);
    set(*d, "wave1", 0.0f); set(*d, "cutoff", 400.0f); d->reset();
    d->noteOn(57, 1.0f, 1);
    const auto plain = render(*d, 320);
    d->reset();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 0, 1.0f);
    const auto slid = render(*d, 320);
    CHECK(powerAbove(slid, kFrom, kLen, 400.0) > 4.0 * powerAbove(plain, kFrom, kLen, 400.0));
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
