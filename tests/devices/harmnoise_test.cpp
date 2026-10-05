// harmnoise: the generic device gates, then analytic checks of every control - partial levels from tilt, odd/even, partial
// count and formant; the inharmonic stretch; exact Nyquist masking; the noise band's level and colour; chiff, vibrato, the
// envelope and the per-note expression.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "B6Kit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;
using namespace ddaw::testkit::b6;
using Catch::Approx;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("harmnoise"); }

// A steady, clean note: no noise, no vibrato, flat envelope.
std::unique_ptr<InstrumentDevice> steady() {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    set(*d, "noise", 0.0f); set(*d, "vibAmt", 0.0f); set(*d, "chiff", 0.0f);
    set(*d, "attack", 0.001f); set(*d, "decay", 0.01f); set(*d, "sustain", 1.0f); set(*d, "release", 0.05f);
    set(*d, "level", 1.0f);
    d->reset();
    return d;
}
constexpr size_t kLen = 16384, kFrom = 22050;   // analysis window (after 0.5 s); 0.37 s long
std::vector<float> note(InstrumentDevice& d, int pitch, float vel = 1.0f, int blocks = 320) {
    d.noteOn(uint8_t(pitch), vel, 1);
    return render(d, blocks);
}
}  // namespace

TEST_CASE("harmnoise passes the generic instrument gates", "[harmnoise][device]") { checkInstrumentContract(make); }

TEST_CASE("harmnoise: the partials sit at multiples of the note and follow the spectral tilt", "[harmnoise]") {
    auto d = steady();
    set(*d, "tilt", -6.0f); set(*d, "harmonics", 24.0f);
    d->reset();
    const auto x = note(*d, 57);                    // 220 Hz
    const double f = hz(57);
    const double a1 = bin(x, kFrom, kLen, f), a2 = bin(x, kFrom, kLen, 2 * f), a4 = bin(x, kFrom, kLen, 4 * f), a8 = bin(x, kFrom, kLen, 8 * f);
    INFO(a1 << " " << a2 << " " << a4 << " " << a8);
    CHECK(a1 > 0.05);
    CHECK(a2 / a1 == Approx(0.5).epsilon(0.08));    // -6 dB per octave: amplitude 1/k
    CHECK(a4 / a1 == Approx(0.25).epsilon(0.08));
    CHECK(a8 / a1 == Approx(0.125).epsilon(0.08));
    CHECK(bin(x, kFrom, kLen, 1.5 * f) < 0.01 * a1);           // nothing between the partials
    CHECK(bin(x, kFrom, kLen, 2.5 * f) < 0.01 * a1);
    // a flat tilt gives equal partials, a steep one a dull sound
    set(*d, "tilt", 0.0f); d->reset();
    const auto flat = note(*d, 57);
    CHECK(bin(flat, kFrom, kLen, 4 * f) / bin(flat, kFrom, kLen, f) == Approx(1.0).epsilon(0.08));
    set(*d, "tilt", -18.0f); d->reset();
    const auto dull = note(*d, 57);
    CHECK(bin(dull, kFrom, kLen, 4 * f) / bin(dull, kFrom, kLen, f) == Approx(std::pow(4.0, -3.0)).epsilon(0.1));
}

TEST_CASE("harmnoise: the loudness stays put while the tilt moves the timbre", "[harmnoise]") {
    auto d = steady();
    double r[3]; int i = 0;
    for (float tilt : {-18.0f, -6.0f, 0.0f}) { set(*d, "tilt", tilt); d->reset(); r[i++] = rms(note(*d, 57), kFrom, kLen); }
    CHECK(r[0] == Approx(r[1]).epsilon(0.1));
    CHECK(r[2] == Approx(r[1]).epsilon(0.1));
    CHECK(r[1] > 0.1);
}

TEST_CASE("harmnoise: odd/even balance hollows the sound out in both directions", "[harmnoise]") {
    auto d = steady();
    const double f = hz(57);
    set(*d, "oddEven", 1.0f); d->reset();
    auto x = note(*d, 57);
    CHECK(db(bin(x, kFrom, kLen, 2 * f) / bin(x, kFrom, kLen, f)) < -60.0);       // even partials gone
    CHECK(bin(x, kFrom, kLen, 3 * f) / bin(x, kFrom, kLen, f) == Approx(1.0 / 3.0).epsilon(0.08));
    set(*d, "oddEven", -1.0f); d->reset();
    x = note(*d, 57);
    CHECK(db(bin(x, kFrom, kLen, f) / bin(x, kFrom, kLen, 2 * f)) < -60.0);       // the fundamental gone: an octave up
    CHECK(db(bin(x, kFrom, kLen, 3 * f) / bin(x, kFrom, kLen, 2 * f)) < -60.0);
    set(*d, "oddEven", 0.5f); d->reset();
    x = note(*d, 57);
    CHECK(bin(x, kFrom, kLen, 2 * f) / bin(x, kFrom, kLen, f) == Approx(0.25).epsilon(0.1));   // 1/2 amplitude from tilt, halved again
}

TEST_CASE("harmnoise: the partial count cuts the series, with a soft edge for fractions", "[harmnoise]") {
    auto d = steady();
    const double f = hz(57);
    set(*d, "harmonics", 5.0f); d->reset();
    auto x = note(*d, 57);
    const double a5 = bin(x, kFrom, kLen, 5 * f), a6 = bin(x, kFrom, kLen, 6 * f);
    CHECK(a5 > 0.01);
    CHECK(db(a6 / a5) < -60.0);
    set(*d, "harmonics", 4.5f); d->reset();
    x = note(*d, 57);
    const double b4 = bin(x, kFrom, kLen, 4 * f), b5 = bin(x, kFrom, kLen, 5 * f);
    // the fifth partial is at half its level; the tilt alone would put it at 4/5 of the fourth
    CHECK(b5 / b4 == Approx(0.5 * 4.0 / 5.0).epsilon(0.1));
    set(*d, "harmonics", 1.0f); d->reset();
    x = note(*d, 57);
    CHECK(db(bin(x, kFrom, kLen, 2 * f) / bin(x, kFrom, kLen, f)) < -60.0);
    CHECK(rms(x, kFrom, kLen) > 0.1);                                             // a lone sine is as loud as any other setting
}

TEST_CASE("harmnoise: stretch moves the partials to k f sqrt(1 + B k^2)", "[harmnoise]") {
    auto d = steady();
    const double f = hz(45);                         // 110 Hz
    set(*d, "stretch", 0.01f); set(*d, "tilt", 0.0f); d->reset();
    const auto x = note(*d, 45);
    for (int k : {2, 5, 9}) {
        const double fk = k * f * std::sqrt(1.0 + 0.01 * k * k);
        INFO("partial " << k << " at " << fk);
        CHECK(peak(x, kFrom, kLen, fk, 1.0) > 0.05);
        CHECK(bin(x, kFrom, kLen, k * f) < 0.5 * peak(x, kFrom, kLen, fk, 1.0));
    }
}

TEST_CASE("harmnoise: the formant bump lifts the partials around its centre by its gain", "[harmnoise]") {
    auto d = steady();
    const double f = hz(57);                         // partial 6 is 1320 Hz
    set(*d, "tilt", 0.0f); set(*d, "formant", 1320.0f); set(*d, "fWidth", 0.25f); d->reset();
    auto x = note(*d, 57);
    const double base = bin(x, kFrom, kLen, 6 * f) / bin(x, kFrom, kLen, 2 * f);
    set(*d, "fGain", 18.0f); d->reset();
    x = note(*d, 57);
    const double lifted = bin(x, kFrom, kLen, 6 * f) / bin(x, kFrom, kLen, 2 * f);
    CHECK(db(lifted / base) == Approx(18.0).margin(1.0));
    // the bump is local: a partial two octaves away is not touched
    CHECK(bin(x, kFrom, kLen, 20 * f) / bin(x, kFrom, kLen, 2 * f) == Approx(1.0).epsilon(0.05));
}

TEST_CASE("harmnoise: no partial passes the Nyquist, so nothing folds back", "[harmnoise]") {
    auto d = steady();
    set(*d, "tilt", 0.0f); set(*d, "harmonics", 48.0f); d->reset();
    const int pitch = 100;                           // 2637 Hz: 7 partials fit under 0.47 of the sample rate
    const double f = hz(pitch);
    const auto x = note(*d, pitch);
    const double a1 = bin(x, kFrom, kLen, f);
    CHECK(a1 > 0.02);
    for (int k = 9; k <= 40; ++k) {
        double fa = std::fmod(k * f, kSr);
        if (fa > kSr / 2) fa = kSr - fa;             // where partial k would fold to
        INFO("partial " << k << " would alias at " << fa);
        if (std::abs(fa / f - std::round(fa / f)) < 0.02) continue;   // lands on a real partial: not an alias
        CHECK(db(bin(x, kFrom, kLen, fa) / a1) < -70.0);
    }
    // and the sound is not quieter than a low note's
    set(*d, "harmonics", 24.0f); d->reset();
    CHECK(rms(x, kFrom, kLen) > 0.05);
}

TEST_CASE("harmnoise: the noise has a fixed loudness at any colour and sits in its band", "[harmnoise]") {
    auto d = steady();
    auto quiet = [&] { set(*d, "noise", 0.0f); d->reset(); return note(*d, 57); };
    const auto base = quiet();
    double r[4]; int i = 0;
    std::vector<float> noisy[2];
    for (auto [hzv, q] : {std::pair{800.0f, 1.0f}, std::pair{3000.0f, 1.0f}, std::pair{3000.0f, 6.0f}, std::pair{9000.0f, 2.0f}}) {
        set(*d, "noise", 1.0f); set(*d, "noiseHz", hzv); set(*d, "noiseQ", q); d->reset();
        const auto n = minus(note(*d, 57), base);   // the noise alone: everything else is the same, sample for sample
        r[i] = rms(n, kFrom, kLen);
        INFO("noise at " << hzv << " Hz, Q " << q << " rms " << r[i]);
        CHECK(r[i] == Approx(0.35).epsilon(0.25));
        if (i == 1) noisy[0] = n;
        if (i == 2) noisy[1] = n;
        ++i;
    }
    // the band sits at its centre: a narrow one (Q 6) has its power-weighted centre there, and holds more of its power near
    // the centre than the wide one (Q 1)
    CHECK(centroid(noisy[1], kFrom, kLen, 100.0, 20000.0) == Approx(3000.0).epsilon(0.1));
    const auto share = [&](const std::vector<float>& n) { return powerAbove(n, kFrom, kLen, 2200.0) - powerAbove(n, kFrom, kLen, 4000.0); };
    CHECK(share(noisy[1]) > 1.5 * share(noisy[0]));
}

TEST_CASE("harmnoise: noiseTrack carries the noise band with the pitch", "[harmnoise]") {
    auto d = steady();
    set(*d, "noise", 1.0f); set(*d, "noiseHz", 2000.0f); set(*d, "noiseQ", 4.0f);
    auto bandOf = [&](float track, int pitch) {
        set(*d, "noiseTrack", track); set(*d, "noise", 0.0f); d->reset();
        const auto base = note(*d, pitch);
        set(*d, "noise", 1.0f); d->reset();
        return centroid(minus(note(*d, pitch), base), kFrom, kLen, 200.0, 18000.0);
    };
    const double fixedLow = bandOf(0.0f, 45), fixedHigh = bandOf(0.0f, 69);
    const double trackLow = bandOf(1.0f, 45), trackHigh = bandOf(1.0f, 69);
    CHECK(fixedHigh == Approx(fixedLow).epsilon(0.1));
    CHECK(trackHigh / trackLow == Approx(4.0).epsilon(0.25));       // two octaves
}

TEST_CASE("harmnoise: chiff is a short burst of noise at the start of the note", "[harmnoise]") {
    auto d = steady();
    set(*d, "chiff", 1.0f); set(*d, "noise", 0.0f); d->reset();
    const auto x = note(*d, 57);
    set(*d, "chiff", 0.0f); d->reset();
    const auto y = note(*d, 57);
    const auto n = minus(x, y);
    const double early = rms(n, 0, 1200), late = rms(n, 22050, 4096);
    CHECK(early > 0.15);
    CHECK(late < 0.01 * early);
}

TEST_CASE("harmnoise: vibrato puts sidebands around each partial", "[harmnoise]") {
    auto d = steady();
    const double f = hz(57);
    set(*d, "vibAmt", 40.0f); set(*d, "vibRate", 5.0f); d->reset();
    const auto x = note(*d, 57, 1.0f, 720);
    const size_t from = 22050, len = 65536;
    const double side = bin(x, from, len, f + 5.0), carrier = peak(x, from, len, f, 1.0);
    // 40 cents at 5 Hz: modulation index = 220 * 0.0234 / 5 = 1.03, so the first sideband over the carrier is J1/J0 = 0.44 / 0.77
    CHECK(side / carrier == Approx(0.573).margin(0.06));
    set(*d, "vibAmt", 0.0f); d->reset();
    const auto y = note(*d, 57, 1.0f, 720);
    CHECK(bin(y, from, len, f + 5.0) < 0.003 * peak(y, from, len, f, 1.0));
}

TEST_CASE("harmnoise: the envelope attacks, sustains and releases", "[harmnoise]") {
    auto d = steady();
    set(*d, "attack", 0.1f); set(*d, "release", 0.2f); d->reset();
    d->noteOn(57, 1.0f, 1);
    auto x = render(*d, 300);
    CHECK(rms(x, 0, 441) < 0.4 * rms(x, 30000, 2048));                  // still rising in the first 10 ms
    CHECK(rms(x, 8000, 2048) > 0.9 * rms(x, 30000, 2048));              // up by 0.2 s
    d->noteOff(1);
    auto y = render(*d, 170);
    CHECK(rms(y, 8000, 1024) < 0.1 * rms(x, 30000, 2048));              // 0.2 s release: well down after 0.2 s
    CHECK(rms(y, 20000, 1024) < 1e-4);
}

TEST_CASE("harmnoise: per-note expression bends, brightens and swells only its own note", "[harmnoise][mpe]") {
    auto d = steady();
    const double f = hz(57);
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 2, 12.0f);
    auto x = render(*d, 320);
    CHECK(peak(x, kFrom, kLen, 2 * f, 2.0) > 0.05);                      // an octave up
    CHECK(db(peak(x, kFrom, kLen, f, 2.0) / peak(x, kFrom, kLen, 2 * f, 2.0)) < -50.0);
    d->reset();
    d->noteOn(57, 1.0f, 1);
    const auto plain = render(*d, 320);
    d->reset();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 1, 1.0f);
    const auto pressed = render(*d, 320);
    CHECK(rms(pressed, kFrom, kLen) / rms(plain, kFrom, kLen) == Approx(1.5).epsilon(0.05));
    d->reset();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 0, 1.0f);
    const auto slid = render(*d, 320);
    CHECK(bin(slid, kFrom, kLen, 4 * f) / bin(slid, kFrom, kLen, f) > 2.0 * bin(plain, kFrom, kLen, 4 * f) / bin(plain, kFrom, kLen, f));
    // another note's expression is ignored, and neutral values change nothing
    d->reset();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(99, 2, 12.0f);
    d->noteExpression(1, 2, 0.0f);
    d->noteExpression(1, 1, 0.0f);
    d->noteExpression(1, 0, 0.0f);
    CHECK(render(*d, 320) == plain);
}
