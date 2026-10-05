// perc: the generic device gates, then analytic checks of every part - the body's pitch sweep, decay and tracking; the noise
// filter modes and decay; clap strikes; the metal cluster; the click; drive; gate and release; velocity; independence of the
// voices (the sum of two hits is the two hits summed); and per-note expression.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "B6Kit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;
using namespace ddaw::testkit::b6;
using Catch::Approx;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("perc"); }

// Everything off: the tests switch on the part they measure.
std::unique_ptr<InstrumentDevice> silent() {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    set(*d, "body", 0.0f); set(*d, "noise", 0.0f); set(*d, "metal", 0.0f); set(*d, "click", 0.0f); set(*d, "drive", 0.0f);
    set(*d, "clap", 0.0f); set(*d, "gate", 0.0f); set(*d, "level", 1.0f);
    d->reset();
    return d;
}
std::unique_ptr<InstrumentDevice> bodyOnly() {
    auto d = silent();
    set(*d, "body", 1.0f); set(*d, "tune", 100.0f); set(*d, "track", 0.0f); set(*d, "sweep", 0.0f); set(*d, "bodyDecay", 1.0f);
    d->reset();
    return d;
}
std::vector<float> hit(InstrumentDevice& d, int pitch = 60, float vel = 1.0f, int blocks = 200) {
    d.noteOn(uint8_t(pitch), vel, 1);
    return render(d, blocks);
}
// smoothed |x| over 1 ms
std::vector<float> envelope(const std::vector<float>& x) {
    std::vector<float> e(x.size());
    const size_t w = 44;
    double acc = 0;
    for (size_t i = 0; i < x.size(); ++i) { acc += std::abs(x[i]); if (i >= w) acc -= std::abs(x[i - w]); e[i] = float(acc / double(w)); }
    return e;
}
}  // namespace

TEST_CASE("perc passes the generic instrument gates", "[perc][device]") { checkInstrumentContract(make); }

TEST_CASE("perc: the body is a sine at the tune, sweeping down from sweep octaves above it", "[perc]") {
    auto d = bodyOnly();
    auto x = hit(*d);
    CHECK(zeroCrossHz(x, 4000, 8000) == Approx(100.0).epsilon(0.03));
    CHECK(db(bin(x, 4000, 8192, 200.0) / bin(x, 4000, 8192, 100.0)) < -60.0);
    set(*d, "sweep", 2.0f); set(*d, "sweepTime", 0.04f); d->reset();
    x = hit(*d);
    CHECK(zeroCrossHz(x, 0, 441) >= 250.0);                           // 10 ms in: still more than twice the tune (400 Hz falling)
    CHECK(zeroCrossHz(x, 0, 441) < 400.0);
    CHECK(zeroCrossHz(x, 16000, 8000) == Approx(100.0).epsilon(0.05));  // settled
}

TEST_CASE("perc: the body decays by 60 dB in bodyDecay seconds", "[perc]") {
    auto d = bodyOnly();
    set(*d, "bodyDecay", 0.5f); d->reset();
    const auto x = hit(*d, 60, 1.0f, 300);
    const double a = rms(x, 2000, 2205), b = rms(x, 2000 + 11025, 2205);       // a quarter second apart: -30 dB
    CHECK(db(a / b) == Approx(30.0).margin(2.0));
}

TEST_CASE("perc: track makes the body follow the note", "[perc]") {
    auto d = bodyOnly();
    set(*d, "track", 1.0f); d->reset();
    auto x = hit(*d, 60);
    CHECK(zeroCrossHz(x, 4000, 8000) == Approx(100.0).epsilon(0.03));
    d->reset();
    x = hit(*d, 72);
    CHECK(zeroCrossHz(x, 4000, 8000) == Approx(200.0).epsilon(0.03));
    set(*d, "track", 0.0f); d->reset();
    x = hit(*d, 72);
    CHECK(zeroCrossHz(x, 4000, 8000) == Approx(100.0).epsilon(0.03));
    set(*d, "track", 0.5f); d->reset();
    x = hit(*d, 72);
    CHECK(zeroCrossHz(x, 4000, 8000) == Approx(141.4).epsilon(0.03));
}

TEST_CASE("perc: the noise filter modes put the noise where they say, and the noise decays on its own clock", "[perc]") {
    auto d = silent();
    set(*d, "noise", 1.0f); set(*d, "noiseHz", 2000.0f); set(*d, "noiseQ", 0.7071f); set(*d, "noiseDecay", 1.0f); d->reset();
    // velocity 0.5 puts the cutoff at its setting (the filter opens with velocity around 0.5)
    set(*d, "noiseMode", 0.0f); d->reset();
    auto lp = hit(*d, 60, 0.5f, 100);
    set(*d, "noiseMode", 2.0f); d->reset();
    auto hp = hit(*d, 60, 0.5f, 100);
    set(*d, "noiseMode", 1.0f); set(*d, "noiseQ", 6.0f); d->reset();
    auto bp = hit(*d, 60, 0.5f, 100);
    CHECK(powerAbove(lp, 1000, 8192, 4000.0) < 0.05);
    CHECK(powerAbove(hp, 1000, 8192, 1000.0) > 0.8);
    const double c = centroid(bp, 1000, 8192, 100.0, 20000.0);
    CHECK(c == Approx(2000.0).epsilon(0.15));
    // decay: 60 dB in noiseDecay
    set(*d, "noiseMode", 0.0f); set(*d, "noiseHz", 8000.0f); set(*d, "noiseDecay", 0.4f); d->reset();
    const auto x = hit(*d, 60, 0.5f, 300);
    CHECK(db(rms(x, 1000, 2205) / rms(x, 1000 + 8820, 2205)) == Approx(30.0).margin(2.5));    // 0.2 s apart: half the T60
}

TEST_CASE("perc: clap re-strikes the noise with the nine-millisecond spacing", "[perc]") {
    auto d = silent();
    set(*d, "noise", 1.0f); set(*d, "noiseMode", 1.0f); set(*d, "noiseHz", 1500.0f); set(*d, "noiseQ", 1.5f); set(*d, "noiseDecay", 0.3f); d->reset();
    // strikes show as jumps in the 1 ms RMS: a new strike lands on a level that has fallen 50 dB, so it jumps by far more than 4x
    auto strikes = [&](float claps) {
        set(*d, "clap", claps); d->reset();
        const auto x = hit(*d, 60, 1.0f, 40);
        std::vector<size_t> at;
        double prev = 0;
        for (size_t j = 0; j < 40; ++j) {
            const double h = rms(x, j * 44, 44);
            if (j == 0 || h > 4.0 * prev) at.push_back(j);
            prev = h;
        }
        return at;
    };
    CHECK(strikes(0.0f).size() == 1);
    const auto s3 = strikes(3.0f);
    REQUIRE(s3.size() == 4);
    for (size_t i = 1; i < s3.size(); ++i) CHECK(double(s3[i] - s3[i - 1]) == Approx(9.0).margin(1.5));          // 9 ms in 1 ms steps
}

TEST_CASE("perc: the metal cluster is a bright, short, inharmonic clang", "[perc]") {
    auto d = silent();
    set(*d, "metal", 1.0f); set(*d, "metalDecay", 0.2f); d->reset();
    const auto x = hit(*d, 60, 1.0f, 100);
    CHECK(rms(x, 200, 2048) > 0.05);
    CHECK(powerAbove(x, 200, 8192, 5000.0) > 0.7);                     // a 6 kHz high-pass over square-wave partials
    CHECK(db(rms(x, 200, 2048) / rms(x, 200 + 8820, 2048)) > 20.0);   // gone fast: T60 0.2 s
    set(*d, "metalTune", 2.0f); d->reset();
    const auto hi = hit(*d, 60, 1.0f, 100);
    CHECK(hi != x);                                                     // a different cluster ...
    CHECK(rms(hi, 200, 2048) > 0.05);                                    // ... as loud, behind the same high-pass
    CHECK(powerAbove(hi, 200, 8192, 5000.0) > 0.7);
}

TEST_CASE("perc: the click is a two-millisecond tick", "[perc]") {
    auto d = silent();
    set(*d, "click", 1.0f); d->reset();
    const auto x = hit(*d, 60, 1.0f, 40);
    const double early = rms(x, 0, 150) * std::sqrt(150.0), late = rms(x, 441, 441) * std::sqrt(441.0);
    CHECK(early > 0.2);
    CHECK(late < 0.05 * early);
}

TEST_CASE("perc: drive adds harmonics to the body and keeps it bounded", "[perc]") {
    auto d = bodyOnly();
    d->reset();
    auto x = hit(*d);
    CHECK(db(bin(x, 4000, 8192, 300.0) / bin(x, 4000, 8192, 100.0)) < -60.0);
    set(*d, "drive", 1.0f); d->reset();
    x = hit(*d);
    CHECK(db(bin(x, 4000, 8192, 300.0) / bin(x, 4000, 8192, 100.0)) > -20.0);
    float mx = 0; for (float v : x) mx = std::max(mx, std::abs(v));
    CHECK(mx <= 1.1f);
}

TEST_CASE("perc: with gate off a hit rings out, with gate on a note-off damps it", "[perc]") {
    auto a = bodyOnly(), b = bodyOnly();
    set(*a, "bodyDecay", 2.0f); set(*b, "bodyDecay", 2.0f); a->reset(); b->reset();
    a->noteOn(60, 1.0f, 1); b->noteOn(60, 1.0f, 1);
    auto xa = render(*a, 40), xb = render(*b, 40);
    b->noteOff(1);
    xa = render(*a, 100); xb = render(*b, 100);
    CHECK(xa == xb);                                                   // note-off ignored
    set(*b, "gate", 1.0f); set(*b, "release", 0.05f);
    a->reset(); b->reset();
    a->noteOn(60, 1.0f, 1); b->noteOn(60, 1.0f, 1);
    render(*a, 40); render(*b, 40);
    b->noteOff(1);
    const auto ya = render(*a, 100), yb = render(*b, 100);
    CHECK(rms(ya, 8000, 1024) > 0.05);
    CHECK(rms(yb, 8000, 1024) < 0.001);
}

TEST_CASE("perc: velocity scales the level and opens the noise", "[perc]") {
    auto d = bodyOnly();
    const auto loud = hit(*d, 60, 1.0f), soft = [&] { d->reset(); return hit(*d, 60, 0.5f); }();
    CHECK(rms(soft, 2000, 4096) / rms(loud, 2000, 4096) == Approx(0.5).epsilon(0.05));
    auto n = silent();
    set(*n, "noise", 1.0f); set(*n, "noiseMode", 0.0f); set(*n, "noiseHz", 3000.0f); set(*n, "noiseDecay", 1.0f); n->reset();
    const auto hard = hit(*n, 60, 1.0f, 100);
    n->reset();
    const auto gentle = hit(*n, 60, 0.2f, 100);
    CHECK(centroid(hard, 1000, 8192, 100.0, 20000.0) > 1.5 * centroid(gentle, 1000, 8192, 100.0, 20000.0));
}

TEST_CASE("perc: voices are independent and a finished hit costs nothing", "[perc]") {
    auto d = silent();
    set(*d, "body", 1.0f); set(*d, "noise", 0.5f); set(*d, "noiseDecay", 0.2f); set(*d, "click", 0.4f); set(*d, "metal", 0.3f); set(*d, "bodyDecay", 0.3f); d->reset();
    d->noteOn(48, 1.0f, 1);
    const auto a = render(*d, 100);
    d->reset();
    d->noteOn(60, 0.7f, 2);
    const auto b = render(*d, 100);
    d->reset();
    d->noteOn(48, 1.0f, 1);
    d->noteOn(60, 0.7f, 2);
    const auto ab = render(*d, 100);
    for (size_t i = 0; i < ab.size(); ++i) REQUIRE(ab[i] == Approx(a[i] + b[i]).margin(2e-6));
    // everything dies: after the tails, exact silence
    d->reset();
    d->noteOn(48, 1.0f, 1);
    const auto tail = render(*d, 800);
    for (size_t i = tail.size() - 1024; i < tail.size(); ++i) REQUIRE(tail[i] == 0.0f);
}

TEST_CASE("perc: per-note expression bends the body, opens the noise and swells only its note", "[perc][mpe]") {
    auto d = bodyOnly();
    set(*d, "track", 1.0f); d->reset();
    d->noteOn(60, 1.0f, 1);
    d->noteExpression(1, 2, 12.0f);
    const auto up = render(*d, 200);
    CHECK(zeroCrossHz(up, 4000, 8000) == Approx(200.0).epsilon(0.03));
    d->reset();
    const auto plain = hit(*d);
    d->reset();
    d->noteOn(60, 1.0f, 1);
    d->noteExpression(1, 1, 1.0f);
    CHECK(rms(render(*d, 200), 2000, 4096) / rms(plain, 2000, 4096) == Approx(1.5).epsilon(0.05));
    d->reset();
    d->noteOn(60, 1.0f, 1);
    d->noteExpression(99, 2, 12.0f);
    d->noteExpression(1, 2, 0.0f); d->noteExpression(1, 1, 0.0f); d->noteExpression(1, 0, 0.0f);
    CHECK(render(*d, 200) == plain);
    auto n = silent();
    set(*n, "noise", 1.0f); set(*n, "noiseMode", 0.0f); set(*n, "noiseHz", 1500.0f); set(*n, "noiseDecay", 1.0f); n->reset();
    const auto closed = hit(*n, 60, 0.5f, 100);
    n->reset();
    n->noteOn(60, 0.5f, 1);
    n->noteExpression(1, 0, 1.0f);
    const auto open = render(*n, 100);
    CHECK(centroid(open, 1000, 8192, 100.0, 20000.0) > 1.4 * centroid(closed, 1000, 8192, 100.0, 20000.0));
}
