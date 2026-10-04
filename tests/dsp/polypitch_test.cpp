// Polyphonic pitch estimation: chords of harmonic notes, closely spaced and octave-related notes, noise, and the
// note tracker's hysteresis.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <set>

#include "dsp/PolyPitch.h"

using namespace ddaw::dsp;

namespace {

constexpr double kRate = 16000.0;
double hzOf(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }

// A chord of notes, each with `partials` harmonics falling off as 1/h, at a given level per note.
std::vector<float> chord(const std::vector<int>& notes, int n, int partials = 8, const std::vector<double>& levels = {}, double vibratoCents = 0) {
    std::vector<float> x(size_t(n), 0.0f);
    for (size_t k = 0; k < notes.size(); ++k) {
        const double lvl = k < levels.size() ? levels[k] : 0.3;
        double ph = 0.4 * double(k);
        for (int i = 0; i < n; ++i) {
            const double f = hzOf(notes[k]) * std::pow(2.0, vibratoCents / 1200.0 * std::sin(2.0 * std::numbers::pi * 5.0 * i / kRate));
            ph += 2.0 * std::numbers::pi * f / kRate;
            double s = 0;
            for (int h = 1; h <= partials; ++h) if (f * h < kRate * 0.45) s += std::sin(double(h) * ph) / double(h);
            x[size_t(i)] += float(lvl * s);
        }
    }
    return x;
}

std::set<int> found(PolyPitch& pp, const std::vector<float>& x, size_t at = 0) {
    std::set<int> s;
    for (const auto& c : pp.estimate(&x[at])) s.insert(int(std::lround(c.midi)));
    return s;
}

}  // namespace

TEST_CASE("poly pitch: triads and seventh chords are found note for note", "[polypitch]") {
    PolyPitch pp;
    pp.prepare();
    CHECK(found(pp, chord({60}, 4096)) == std::set<int>{60});
    CHECK(found(pp, chord({60, 64, 67}, 4096)) == std::set<int>{60, 64, 67});                 // C major
    CHECK(found(pp, chord({57, 60, 64}, 4096)) == std::set<int>{57, 60, 64});                 // A minor
    CHECK(found(pp, chord({55, 59, 62, 65}, 4096)) == std::set<int>{55, 59, 62, 65});         // G7
    CHECK(found(pp, chord({62, 66, 69, 73, 76}, 4096)) == std::set<int>{62, 66, 69, 73, 76}); // D major 9-ish, five voices
    CHECK(found(pp, chord({72, 76, 79}, 4096)) == std::set<int>{72, 76, 79});                 // an octave up
}

TEST_CASE("poly pitch: pitch accuracy within 30 cents, with vibrato too", "[polypitch]") {
    PolyPitch pp;
    pp.prepare();
    for (double vib : {0.0, 20.0}) {
        const auto r = pp.estimate(chord({60, 67}, 4096, 8, {}, vib).data());
        REQUIRE(r.size() == 2);
        for (const auto& c : r) {
            const double nearest = std::round(c.midi);
            CHECK(std::abs(double(c.midi) - nearest) < 0.30);
            CHECK((int(nearest) == 60 || int(nearest) == 67));
        }
    }
}

TEST_CASE("poly pitch: an octave pair is two notes, a weak upper note is still found", "[polypitch]") {
    PolyPitch pp;
    pp.prepare();
    CHECK(found(pp, chord({48, 60}, 4096, 8, {0.3, 0.3})) == std::set<int>{48, 60});
    CHECK(found(pp, chord({55, 62}, 4096, 8, {0.4, 0.15})) == std::set<int>{55, 62});          // a fifth, the upper one quiet
}

TEST_CASE("poly pitch: no phantom notes from noise or silence; low chords need the longer window", "[polypitch]") {
    PolyPitch pp;
    pp.prepare();
    std::mt19937 g(9);
    std::normal_distribution<float> nd(0.0f, 0.2f);
    std::vector<float> noise(4096);
    for (auto& v : noise) v = nd(g);
    CHECK(pp.estimate(noise.data()).empty());                  // white noise has no harmonic structure
    CHECK(pp.estimate(std::vector<float>(4096, 0.0f).data()).empty());

    // C3 E3 G3 are 34 Hz and 31 Hz apart: closer than a 128 ms window can separate; a 256 ms window can
    const auto low = chord({48, 52, 55}, 8192);
    PolyPitchConfig c4; c4.window = 4096;
    PolyPitch longer;
    longer.prepare(c4);
    CHECK(found(longer, low) == std::set<int>{48, 52, 55});
}

TEST_CASE("poly note tracker: confirms before sounding, holds through a dropout, releases after silence", "[polypitch]") {
    PolyNoteTracker t;
    t.configure(2, 3);
    std::vector<PolyNoteEvent> ev;
    PolyCandidate c60{60.0f, 261.6f, 1.0f, 0.3f}, c64{64.02f, 329.6f, 0.8f, 0.2f};
    t.update({c60}, ev);
    CHECK(ev.empty());                                         // seen once: not yet
    t.update({c60, c64}, ev);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].pitch == 60); CHECK(ev[0].on);
    t.update({c60, c64}, ev);
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].pitch == 64); CHECK(ev[1].on);
    t.update({c64}, ev); t.update({c64}, ev);                  // 60 drops out for two frames: still held
    CHECK(t.active(60));
    t.update({c60, c64}, ev);                                  // and comes back without a retrigger
    CHECK(ev.size() == 2);
    t.update({}, ev); t.update({}, ev);
    CHECK(ev.size() == 2);
    t.update({}, ev);                                          // three missing frames: both release
    CHECK(ev.size() == 4);
    CHECK_FALSE(ev[2].on); CHECK_FALSE(ev[3].on);
    CHECK_FALSE(t.active(60));
    // a candidate between two semitones (out of tune by more than a quarter tone) is ignored
    PolyCandidate between{61.5f, 277.0f, 0.5f, 0.1f};
    ev.clear(); t.reset();
    t.update({between}, ev); t.update({between}, ev);
    CHECK(ev.empty());
}
