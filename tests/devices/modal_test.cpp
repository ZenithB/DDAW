// modal: the generic device gates, then analytic checks - the mode frequencies of every model, the decay time of a mode and
// its frequency dependence, strike-position nulls, the exciters, release, inharmonic stretch, mode count, the Nyquist cut, the
// strike being fixed while a note sounds, stereo spread, polyphony and per-note expression.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "B6Kit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;
using namespace ddaw::testkit::b6;
using Catch::Approx;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("modal"); }

// A string struck at a place that leaves every mode in (pos is not a rational with a small denominator), long decay.
std::unique_ptr<InstrumentDevice> base() {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    set(*d, "model", 0.0f); set(*d, "modes", 24.0f); set(*d, "inharm", 0.0f); set(*d, "decay", 3.0f); set(*d, "damp", 0.0f);
    set(*d, "pos", 0.137f); set(*d, "tone", 0.0f); set(*d, "exciter", 0.0f); set(*d, "spread", 0.0f); set(*d, "level", 1.0f);
    d->reset();
    return d;
}
std::vector<float> strike(InstrumentDevice& d, int pitch, float vel = 1.0f, int blocks = 200) {
    d.noteOn(uint8_t(pitch), vel, 1);
    return render(d, blocks);
}
constexpr size_t kW = 8192;
double mode(const std::vector<float>& x, size_t from, double f) { return peak(x, from, kW, f, 1.0); }
}  // namespace

TEST_CASE("modal passes the generic instrument gates", "[modal][device]") { checkInstrumentContract(make); }

TEST_CASE("modal: every model rings at its own mode frequencies", "[modal]") {
    auto d = base();
    set(*d, "decay", 4.0f);
    struct Case { int model; std::vector<double> ratios; };
    const std::vector<Case> cases = {
        {0, {1, 2, 3, 7}},
        {1, {1, 2.7565, 5.4039, 8.9330}},                // free bar
        {2, {1, 4, 10}},                                  // marimba
        {3, {1, 1.593, 2.135, 2.653}},                    // membrane
        {5, {1, 2, 2.4, 3}},                              // bell
        {6, {1, 3, 5, 7}},                                // closed tube
    };
    for (const auto& c : cases) {
        set(*d, "model", float(c.model)); d->reset();
        const auto x = strike(*d, 45);                    // 110 Hz
        const double ref = mode(x, 2000, 110.0);
        for (double r : c.ratios) {
            INFO("model " << c.model << " ratio " << r);
            CHECK(mode(x, 2000, 110.0 * r) > 0.02 * ref);
        }
        // and nothing at a frequency that is not a mode
        const double off = c.model == 0 ? 2.5 : c.model == 6 ? 2.0 : 1.3;
        CHECK(mode(x, 2000, 110.0 * off) < 0.02 * ref);
    }
}

TEST_CASE("modal: a mode rings for its own decay time, and damping shortens the high ones", "[modal]") {
    auto d = base();
    auto t60 = [&](const std::vector<float>& x, double f, double t0, double span) {
        const size_t a = size_t(t0 * kSr), b = a + size_t(span * kSr);
        const double drop = db(bin(x, a, 1024, f) / bin(x, b, 1024, f));
        return 60.0 * span / drop;
    };
    set(*d, "decay", 1.0f); set(*d, "damp", 0.0f); d->reset();
    auto x = strike(*d, 45, 1.0f, 400);
    CHECK(t60(x, 110.0, 0.1, 0.5) == Approx(1.0).epsilon(0.1));
    CHECK(t60(x, 440.0, 0.1, 0.5) == Approx(1.0).epsilon(0.1));
    set(*d, "damp", 1.0f); d->reset();
    x = strike(*d, 45, 1.0f, 400);
    const double t1 = t60(x, 110.0, 0.05, 0.5), t4 = t60(x, 440.0, 0.02, 0.03);
    CHECK(t1 == Approx(1.0).epsilon(0.1));
    CHECK(t4 == Approx(1.0 / 8.0).epsilon(0.15));        // decay * r^-1.5 = 1/8 at the 4th mode
}

TEST_CASE("modal: the strike position nulls the modes it touches at a node", "[modal]") {
    auto d = base();
    set(*d, "pos", 0.5f); d->reset();
    auto x = strike(*d, 45);
    const double a1 = mode(x, 2000, 110.0);
    CHECK(a1 > 0.01);
    CHECK(db(mode(x, 2000, 220.0) / a1) < -60.0);       // struck in the middle: no even modes
    CHECK(mode(x, 2000, 330.0) > 0.1 * a1);
    set(*d, "pos", 0.25f); d->reset();
    x = strike(*d, 45);
    CHECK(db(mode(x, 2000, 440.0) / mode(x, 2000, 110.0)) < -60.0);
    CHECK(mode(x, 2000, 220.0) > 0.1 * mode(x, 2000, 110.0));
}

TEST_CASE("modal: a harder strike is brighter at the same overall level", "[modal]") {
    auto d = base();
    set(*d, "exciter", 2.0f); set(*d, "hard", 0.0f); d->reset();
    const auto soft = strike(*d, 45);
    set(*d, "hard", 1.0f); d->reset();
    const auto hard = strike(*d, 45);
    set(*d, "exciter", 0.0f); d->reset();
    const auto impulse = strike(*d, 45);
    const double s = powerAbove(soft, 400, 8192, 1500.0), h = powerAbove(hard, 400, 8192, 1500.0), im = powerAbove(impulse, 400, 8192, 1500.0);
    INFO(s << " " << h << " " << im);
    CHECK(h > 3.0 * s);
    CHECK(im >= 0.9 * h);
    // the fundamental is much the same whatever the strike
    CHECK(mode(soft, 2000, 110.0) > 0.4 * mode(hard, 2000, 110.0));       // an 8 ms mallet takes a little off even 110 Hz
    // the noise burst excites everything too
    set(*d, "exciter", 1.0f); set(*d, "hard", 0.5f); d->reset();
    const auto burst = strike(*d, 45);
    CHECK(mode(burst, 2000, 110.0) > 0.02);
    CHECK(rms(burst, 400, 4096) > 0.01);
    // velocity: a soft touch is quieter
    d->reset();
    const auto touch = strike(*d, 45, 0.25f);
    CHECK(rms(touch, 400, 4096) < 0.4 * rms(burst, 400, 4096));
}

TEST_CASE("modal: the release damps a held ring, and a note left alone keeps ringing", "[modal]") {
    auto d = base();
    set(*d, "decay", 6.0f); set(*d, "release", 0.05f); d->reset();
    d->noteOn(45, 1.0f, 1);
    auto x = render(*d, 100);
    const double before = rms(x, 10000, 1024);
    auto y = render(*d, 100);
    CHECK(rms(y, 8000, 1024) > 0.6 * before);                          // still ringing
    d->noteOff(1);
    const auto z = render(*d, 100);
    CHECK(rms(z, 6000, 1024) < 0.01 * before);                         // 50 ms release: gone well within 0.15 s
}

TEST_CASE("modal: inharmonicity stretches the series and the mode count cuts it", "[modal]") {
    auto d = base();
    set(*d, "inharm", 0.2f); d->reset();
    auto x = strike(*d, 45);
    CHECK(mode(x, 2000, 110.0 * std::pow(5.0, 1.2)) > 0.1 * mode(x, 2000, 110.0));
    CHECK(mode(x, 2000, 550.0) < 0.02 * mode(x, 2000, 110.0));
    set(*d, "inharm", 0.0f); set(*d, "modes", 4.0f); d->reset();
    x = strike(*d, 45);
    CHECK(mode(x, 2000, 440.0) > 0.05 * mode(x, 2000, 110.0));
    CHECK(mode(x, 2000, 550.0) < 0.01 * mode(x, 2000, 110.0));
}

TEST_CASE("modal: tone tilts the levels of the modes", "[modal]") {
    auto d = base();
    set(*d, "tone", -1.0f); d->reset();
    const auto dull = strike(*d, 45);
    set(*d, "tone", 1.0f); d->reset();
    const auto bright = strike(*d, 45);
    CHECK(mode(bright, 2000, 1100.0) / mode(bright, 2000, 110.0) > 8.0 * mode(dull, 2000, 1100.0) / mode(dull, 2000, 110.0));
}

TEST_CASE("modal: modes above the Nyquist are left out and nothing folds back", "[modal]") {
    auto d = base();
    set(*d, "model", 1.0f); set(*d, "decay", 1.0f); d->reset();
    const auto x = strike(*d, 100);                                    // 2637 Hz: the bar's 3rd mode is 14.2 kHz, the 4th would be 23.6 kHz
    float mx = 0; for (float v : x) mx = std::max(mx, std::abs(v));
    CHECK(mx < 2.0f);
    CHECK(mode(x, 2000, 2637.0) > 0.01);
    CHECK(mode(x, 2000, 44100.0 - 23550.0) < 1e-4);                    // where the 4th mode would have folded
    CHECK(powerAbove(x, 2000, 8192, 20700.0) < 1e-6);
}

TEST_CASE("modal: a sounding note keeps the strike it started with", "[modal]") {
    auto a = base(), b = base();
    a->noteOn(45, 1.0f, 1); b->noteOn(45, 1.0f, 1);
    auto xa = render(*a, 30), xb = render(*b, 30);
    set(*a, "decay", 0.1f); set(*a, "model", 3.0f); set(*a, "pos", 0.3f); set(*a, "hard", 0.0f);   // changed while it rings
    xa = render(*a, 100); xb = render(*b, 100);
    CHECK(xa == xb);
    // the next strike uses the new settings
    a->noteOn(60, 1.0f, 2); b->noteOn(60, 1.0f, 2);
    CHECK(render(*a, 50) != render(*b, 50));
}

TEST_CASE("modal: stereo spread alternates the modes between the channels", "[modal]") {
    auto d = base();
    d->noteOn(45, 1.0f, 1);
    auto [l, r] = renderLR(*d, 200);
    CHECK(l == r);
    set(*d, "spread", 1.0f); d->reset();
    d->noteOn(45, 1.0f, 1);
    auto [l2, r2] = renderLR(*d, 200);
    CHECK(mode(l2, 2000, 110.0) > 10.0 * mode(r2, 2000, 110.0));      // the odd modes on the left ...
    CHECK(mode(r2, 2000, 220.0) > 10.0 * mode(l2, 2000, 220.0));      // ... the even ones on the right
}

TEST_CASE("modal: eight notes ring together and the ninth takes the oldest voice", "[modal]") {
    auto d = base();
    set(*d, "decay", 4.0f); d->reset();
    for (int i = 0; i < 8; ++i) d->noteOn(uint8_t(40 + 3 * i), 0.8f, uint32_t(i + 1));
    auto x = render(*d, 100);
    for (int i = 0; i < 8; ++i) CHECK(mode(x, 2000, hz(40 + 3 * i)) > 0.01);
    d->noteOn(90, 0.8f, 9);
    x = render(*d, 100);
    CHECK(mode(x, 2000, hz(90)) > 0.01);
    CHECK(mode(x, 2000, hz(40)) < 0.05 * mode(x, 2000, hz(43)));     // the oldest (pitch 40) was taken
}

TEST_CASE("modal: per-note expression bends the modes and swells only its note", "[modal][mpe]") {
    auto d = base();
    d->noteOn(45, 1.0f, 1);
    d->noteExpression(1, 2, 12.0f);
    auto x = render(*d, 200);
    CHECK(mode(x, 2000, 220.0) > 0.02);
    CHECK(mode(x, 2000, 110.0) < 0.05 * mode(x, 2000, 220.0));
    d->reset();
    const auto plain = strike(*d, 45);
    d->reset();
    d->noteOn(45, 1.0f, 1);
    d->noteExpression(1, 1, 1.0f);
    CHECK(rms(render(*d, 200), 2000, 4096) / rms(plain, 2000, 4096) == Approx(1.5).epsilon(0.05));
    d->reset();
    d->noteOn(45, 1.0f, 1);
    d->noteExpression(99, 2, 12.0f);
    d->noteExpression(1, 2, 0.0f); d->noteExpression(1, 1, 0.0f); d->noteExpression(1, 0, 0.0f);
    CHECK(render(*d, 200) == plain);
}
