// wavetable: the generic device gates, then analytic checks - frame spectra and the morph between them, the mip levels (no
// aliasing, steady level across the range), unison and stereo spread, the sub, the filter, the position envelope, both
// audio-rate ports and per-note expression.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "B6Kit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;
using namespace ddaw::testkit::b6;
using Catch::Approx;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("wavetable"); }

std::unique_ptr<InstrumentDevice> clean() {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    set(*d, "bank", 0.0f); set(*d, "pos", 0.0f); set(*d, "posEnv", 0.0f); set(*d, "unison", 1.0f); set(*d, "sub", 0.0f);
    set(*d, "cutoff", 16000.0f); set(*d, "keytrack", 0.0f); set(*d, "res", 0.0f);
    set(*d, "attack", 0.001f); set(*d, "decay", 0.01f); set(*d, "sustain", 1.0f); set(*d, "release", 0.05f);
    set(*d, "level", 1.0f);
    d->reset();
    return d;
}
constexpr size_t kLen = 16384, kFrom = 22050;
constexpr float kFrame(int i) { return float(i) / 7.0f; }
std::vector<float> note(InstrumentDevice& d, int pitch, int blocks = 320) {
    d.noteOn(uint8_t(pitch), 1.0f, 1);
    return render(d, blocks);
}
double partial(const std::vector<float>& x, int pitch, int k) { return bin(x, kFrom, kLen, k * hz(pitch)); }
std::vector<float> frameNote(InstrumentDevice& d, int bank, int frame, int pitch) {
    set(d, "bank", float(bank)); set(d, "pos", kFrame(frame)); d.reset();
    return note(d, pitch);
}
}  // namespace

TEST_CASE("wavetable passes the generic instrument gates", "[wavetable][device]") { checkInstrumentContract(make); }

TEST_CASE("wavetable: the classic bank holds a sine, a triangle, a saw, a square and an impulse train", "[wavetable]") {
    auto d = clean();
    const int p = 45;                                  // 110 Hz: every table has all its harmonics
    auto x = frameNote(*d, 0, 0, p);
    CHECK(db(partial(x, p, 2) / partial(x, p, 1)) < -60.0);
    CHECK(rms(x, kFrom, kLen) == Approx(0.35).epsilon(0.05));           // the tables are normalised to a common loudness
    x = frameNote(*d, 0, 1, p);
    CHECK(partial(x, p, 3) / partial(x, p, 1) == Approx(1.0 / 9.0).epsilon(0.03));
    CHECK(db(partial(x, p, 2) / partial(x, p, 1)) < -60.0);
    x = frameNote(*d, 0, 2, p);
    CHECK(partial(x, p, 2) / partial(x, p, 1) == Approx(0.5).epsilon(0.03));
    CHECK(partial(x, p, 7) / partial(x, p, 1) == Approx(1.0 / 7.0).epsilon(0.03));
    x = frameNote(*d, 0, 4, p);
    CHECK(db(partial(x, p, 2) / partial(x, p, 1)) < -60.0);
    CHECK(partial(x, p, 5) / partial(x, p, 1) == Approx(0.2).epsilon(0.03));
    x = frameNote(*d, 0, 7, p);
    for (int k : {2, 9, 30, 60}) CHECK(partial(x, p, k) / partial(x, p, 1) == Approx(1.0).epsilon(0.04));
    CHECK(db(partial(x, p, 66) / partial(x, p, 1)) < -40.0);             // 64 harmonics is the end of the table
}

TEST_CASE("wavetable: the position morphs linearly between neighbouring frames", "[wavetable]") {
    auto d = clean();
    const int p = 45;
    const auto a = frameNote(*d, 0, 0, p), b = frameNote(*d, 0, 1, p);
    set(*d, "bank", 0.0f); set(*d, "pos", 0.5f / 7.0f); d->reset();
    const auto m = note(*d, p);
    for (int k : {1, 3, 5}) {
        const double expect = 0.5 * (partial(a, p, k) + partial(b, p, k));       // sine and triangle are in phase at every partial
        INFO("partial " << k);
        CHECK(partial(m, p, k) == Approx(expect).epsilon(0.03).margin(1e-4));
    }
}

TEST_CASE("wavetable: every bank and frame is finite, audible and bounded across the keyboard", "[wavetable]") {
    auto d = clean();
    for (int bank = 0; bank < 4; ++bank)
        for (int frame = 0; frame < 8; ++frame)
            for (int p : {24, 60, 96}) {
                INFO("bank " << bank << " frame " << frame << " pitch " << p);
                const auto x = frameNote(*d, bank, frame, p);
                float mx = 0; for (float v : x) mx = std::max(mx, std::abs(v));
                CHECK(mx < 1.8f);
                CHECK(rms(x, kFrom, 8192) > 0.05);
            }
}

TEST_CASE("wavetable: the mip levels keep high notes free of aliasing and the level steady", "[wavetable]") {
    auto d = clean();
    // a square (odd partials) and the impulse train at 2093 Hz: partials past 0.47 of the rate must not fold back
    for (int frame : {4, 7}) {
        const int p = 96;
        const double f = hz(p);
        const auto x = frameNote(*d, 0, frame, p);
        const double a1 = bin(x, kFrom, kLen, f);
        INFO("frame " << frame);
        CHECK(a1 > 0.02);
        for (int k = 9; k <= 40; ++k) {
            double fa = std::fmod(k * f, kSr);
            if (fa > kSr / 2) fa = kSr - fa;
            if (std::abs(fa / f - std::round(fa / f)) < 0.03) continue;
            INFO("partial " << k << " folds to " << fa);
            CHECK(db(bin(x, kFrom, kLen, fa) / a1) < -50.0);
        }
    }
    // the fundamental keeps its amplitude from the lowest to the highest note (the mip levels differ only above it)
    double ref = 0;
    for (int p = 28; p <= 100; p += 8) {
        const auto x = frameNote(*d, 0, 2, p);
        const double a1 = peak(x, kFrom, kLen, hz(p), 1.0);
        if (p == 28) ref = a1;
        INFO("pitch " << p);
        CHECK(db(a1 / ref) == Approx(0.0).margin(1.0));
    }
}

TEST_CASE("wavetable: unison stacks detuned oscillators and spreads them across the stereo field", "[wavetable]") {
    auto d = clean();
    set(*d, "bank", 0.0f); set(*d, "pos", 0.0f); set(*d, "unison", 3.0f); set(*d, "detune", 50.0f); set(*d, "spread", 0.0f); d->reset();
    d->noteOn(81, 1.0f, 1);                              // 880 Hz: 50 cents are 25 Hz, well apart in a 16384-sample window
    auto [l, r] = renderLR(*d, 320);
    const double f = hz(81), lo = f * std::pow(2.0, -50.0 / 1200.0), hi = f * std::pow(2.0, 50.0 / 1200.0);
    for (double fk : {lo, f, hi}) CHECK(peak(l, kFrom, kLen, fk, 1.0) > 0.1);
    CHECK(l == r);                                                       // no spread: dual mono
    set(*d, "spread", 1.0f); d->reset();
    d->noteOn(81, 1.0f, 1);
    auto [l2, r2] = renderLR(*d, 320);
    CHECK(l2 != r2);
    // the lowest oscillator is on the left, the highest on the right
    CHECK(peak(l2, kFrom, kLen, lo, 1.0) > 3.0 * peak(r2, kFrom, kLen, lo, 1.0));
    CHECK(peak(r2, kFrom, kLen, hi, 1.0) > 3.0 * peak(l2, kFrom, kLen, hi, 1.0));
    CHECK(rms(l2, kFrom, kLen) == Approx(rms(r2, kFrom, kLen)).epsilon(0.1));
    // unison keeps the loudness of one oscillator
    CHECK(rms(l2, kFrom, kLen) == Approx(0.35).epsilon(0.2));
}

TEST_CASE("wavetable: the sub adds a sine an octave below", "[wavetable]") {
    auto d = clean();
    set(*d, "sub", 1.0f); d->reset();
    const auto x = note(*d, 57);
    const double f = hz(57);
    CHECK(bin(x, kFrom, kLen, f * 0.5) / bin(x, kFrom, kLen, f) == Approx(1.0).epsilon(0.1));
}

TEST_CASE("wavetable: the low-pass cuts the top and follows the key", "[wavetable]") {
    auto d = clean();
    set(*d, "bank", 0.0f); set(*d, "pos", kFrame(2)); set(*d, "cutoff", 800.0f); d->reset();
    const auto closed = note(*d, 45);
    set(*d, "cutoff", 16000.0f); d->reset();
    const auto open = note(*d, 45);
    CHECK(db((partial(closed, 45, 16) / partial(closed, 45, 1)) / (partial(open, 45, 16) / partial(open, 45, 1))) < -10.0);   // a 12 dB/octave low-pass, 1.1 octaves above its corner
    set(*d, "cutoff", 800.0f);
    auto tilt = [&](float track, int pitch) {
        set(*d, "keytrack", track); d->reset();
        const auto x = note(*d, pitch);
        return partial(x, pitch, 8) / partial(x, pitch, 1);
    };
    const double lo0 = tilt(0.0f, 48), hi0 = tilt(0.0f, 60), lo1 = tilt(1.0f, 48), hi1 = tilt(1.0f, 60);
    CHECK(hi0 < 0.6 * lo0);
    CHECK(hi1 == Approx(lo1).epsilon(0.1));
}

TEST_CASE("wavetable: the position envelope sweeps the frames from the note's start", "[wavetable]") {
    auto d = clean();
    set(*d, "pos", 0.0f); set(*d, "posEnv", 1.0f); set(*d, "posDecay", 0.15f); d->reset();
    const auto x = note(*d, 45);
    // starts on the last frame (the impulse train, bright) and settles on the sine
    CHECK(powerAbove(x, 100, 2048, 800.0) > 0.05);
    CHECK(powerAbove(x, kFrom, kLen, 800.0) < 1e-4);
}

TEST_CASE("wavetable: the audio-rate ports act like moving the parameter", "[wavetable][arate]") {
    {   // position: 0 plus a constant 2/7 from the port is the saw frame
        auto a = clean(), b = clean();
        set(*a, "pos", kFrame(2)); a->reset();
        std::vector<float> buf(kMaxBlock, kFrame(2));
        const float* ptrs[2] = {buf.data(), nullptr};
        ModInputs mod{std::span<const float* const>(ptrs, 2)};
        const auto xa = note(*a, 45);
        b->noteOn(45, 1.0f, 1);
        const auto xb = render(*b, 320, mod);
        double diff = 0; for (size_t i = kFrom; i < kFrom + kLen; ++i) diff = std::max(diff, double(std::abs(xa[i] - xb[i])));
        CHECK(diff < 1e-4);
    }
    {   // pitch: +12 semitones is an octave up
        auto d = clean();
        std::vector<float> buf(kMaxBlock, 12.0f);
        const float* ptrs[2] = {nullptr, buf.data()};
        ModInputs mod{std::span<const float* const>(ptrs, 2)};
        d->noteOn(57, 1.0f, 1);
        const auto x = render(*d, 320, mod);
        CHECK(peak(x, kFrom, kLen, 2 * hz(57), 1.0) > 0.3);
        CHECK(db(bin(x, kFrom, kLen, hz(57)) / peak(x, kFrom, kLen, 2 * hz(57), 1.0)) < -50.0);
    }
    {   // a sine on the position port sweeps the spectrum: sidebands around the fundamental
        auto d = clean();
        set(*d, "pos", 0.3f); d->reset();
        std::vector<float> buf(kMaxBlock);
        const float* ptrs[2] = {buf.data(), nullptr};
        ModInputs mod{std::span<const float* const>(ptrs, 2)};
        d->noteOn(45, 1.0f, 1);
        std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
        double ph = 0;
        for (int b = 0; b < 480; ++b) {
            for (auto& v : buf) { v = 0.25f * float(std::sin(ph)); ph += 2.0 * 3.14159265358979 * 20.0 / kSr; }
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            d->process(l.data(), r.data(), kMaxBlock, ctx(), mod);
            out.insert(out.end(), l.begin(), l.end());
        }
        CHECK(bin(out, kFrom, 32768, 3 * hz(45) + 20.0) > 0.02 * peak(out, kFrom, 32768, 3 * hz(45), 1.0));
    }
}

TEST_CASE("wavetable: the envelope attacks, sustains and releases", "[wavetable]") {
    auto d = clean();
    set(*d, "attack", 0.1f); set(*d, "decay", 0.2f); set(*d, "sustain", 0.5f); set(*d, "release", 0.2f); d->reset();
    d->noteOn(57, 1.0f, 1);
    const auto x = render(*d, 400);
    const double early = rms(x, 0, 441), top = rms(x, 4000, 441), sus = rms(x, 40000, 4410);
    CHECK(early < 0.3 * top);
    CHECK(sus == Approx(0.5 * top).epsilon(0.15));
    d->noteOff(1);
    CHECK(rms(render(*d, 200), 12000, 1024) < 0.02 * sus);
}

TEST_CASE("wavetable: per-note expression bends, moves the position and swells only its note", "[wavetable][mpe]") {
    auto d = clean();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 2, 12.0f);
    auto x = render(*d, 320);
    CHECK(peak(x, kFrom, kLen, 2 * hz(57), 2.0) > 0.3);
    d->reset();
    d->noteOn(57, 1.0f, 1);
    const auto plain = render(*d, 320);
    d->reset();
    d->noteOn(57, 1.0f, 1);
    d->noteExpression(1, 0, 1.0f);
    const auto slid = render(*d, 320);
    CHECK(partial(slid, 57, 2) > 0.05);                                  // moved toward the triangle/saw frames: new partials
    CHECK(partial(plain, 57, 2) < 1e-3);
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

TEST_CASE("wavetable: two instances sound identically (the tables are deterministic)", "[wavetable]") {
    auto a = clean(), b = clean();
    CHECK(frameNote(*a, 1, 3, 52) == frameNote(*b, 1, 3, 52));
}
