// MIDI-fx expander tests: port of the midifx.rs unit tests plus exact-sequence checks for every device.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine/MidiFx.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

project::Note note(int p, double s, double d, double v = 0.8, double pr = 1.0) {
    project::Note n;
    n.pitch = p;
    n.startTicks = s;
    n.durTicks = d;
    n.velocity = v;
    n.probability = pr;
    return n;
}

project::DeviceSpec fx(const std::string& type, std::map<std::string, double> params = {}, bool on = true) {
    project::DeviceSpec d;
    d.type = type;
    d.on = on;
    d.params = std::move(params);
    return d;
}

MidiFxContext makeCtx(bool drum = false, const std::string& id = "t1", double root = 0,
                      const std::string& scale = "major") {
    MidiFxContext c;
    c.trackId = id;
    c.isDrum = drum;
    c.root = root;
    c.scale = scale;
    c.loopLen = 384;
    return c;
}

std::vector<NoteEv> run(const std::vector<project::DeviceSpec>& chain, const std::vector<project::Note>& notes,
                        bool drum = false, const std::string& id = "t1", double root = 0,
                        const std::string& scale = "major") {
    return expandMidi(chain, notes, makeCtx(drum, id, root, scale));
}

std::vector<int> pitches(const std::vector<NoteEv>& v, size_t n = SIZE_MAX) {
    std::vector<int> o;
    for (size_t i = 0; i < v.size() && i < n; ++i) o.push_back(v[i].pitch);
    return o;
}

std::vector<double> ticks(const std::vector<NoteEv>& v, size_t n = SIZE_MAX) {
    std::vector<double> o;
    for (size_t i = 0; i < v.size() && i < n; ++i) o.push_back(v[i].tick);
    return o;
}

// The same FNV-1a seed the expander uses, so tests can replay its rolls exactly.
uint64_t fnv(const std::string& id) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char b : id) {
        h ^= b;
        h *= 0x00000100000001b3ull;
    }
    return h;
}

using Ints = std::vector<int>;
using Dbls = std::vector<double>;

}  // namespace

// ---------------------------------------------------------------- ported Rust tests

TEST_CASE("midifx: arp expands to more events than input", "[midifx]") {
    // one whole-bar note at 1/16 rate -> 384/24 = 16 steps
    auto out = run({fx("arp", {{"rate", 3}, {"mode", 0}, {"pat", 0}, {"oct", 2}, {"gate", 0.6}})},
                   {note(60, 0, 384, 0.8)});
    REQUIRE(out.size() == 16);
    CHECK(out[0].pitch == 60);  // Octave pattern over 2 octaves alternates 60/72
    CHECK(out[1].pitch == 72);
    CHECK(out[2].pitch == 60);
    CHECK(out[0].durTicks == Approx(14.4).margin(1e-9));  // gate: 24 * 0.6
}

TEST_CASE("midifx: arp up-down folds the sequence", "[midifx]") {
    auto out = run({fx("arp", {{"rate", 3}, {"mode", 2}, {"pat", 0}, {"oct", 3}, {"gate", 1.0}})},
                   {note(60, 0, 384, 0.8)});
    CHECK(pitches(out, 4) == Ints{60, 72, 84, 72});  // seq [60,72,84] -> up-down [60,72,84,72]
}

TEST_CASE("midifx: chord stacks intervals", "[midifx]") {
    auto out = run({fx("chord", {{"i1", 3}, {"i2", 7}, {"i3", 0}})}, {note(60, 0, 96, 0.8)});
    REQUIRE(out.size() == 3);  // root + two non-zero intervals
    CHECK(pitches(out) == Ints{60, 63, 67});
    CHECK(out[0].vel == Approx(0.8).margin(1e-6));
    CHECK(out[1].vel == Approx(0.68).margin(1e-6));  // stacked notes at 85%
}

TEST_CASE("midifx: velo scales velocities", "[midifx]") {
    auto out = run({fx("velo", {{"scale", 0.5}, {"rand", 0}})}, {note(60, 0, 96, 0.8)});
    CHECK(out[0].vel == Approx(0.4).margin(1e-6));
    auto quiet = run({fx("velo", {{"scale", 0.0}, {"rand", 0}})}, {note(60, 0, 96, 0.8)});
    CHECK(quiet[0].vel == 0.05f);  // floor at 0.05
}

TEST_CASE("midifx: scale snaps off-scale pitch down on a tie", "[midifx]") {
    auto out = run({fx("scale")}, {note(61, 0, 96)});
    CHECK(out[0].pitch == 60);  // C# in C major: 60 and 62 tie -> down
    auto kept = run({fx("scale")}, {note(64, 0, 96)});
    CHECK(kept[0].pitch == 64);  // in-scale pitches untouched
}

TEST_CASE("midifx: drum tracks skip pitch devices but not velo", "[midifx]") {
    auto out = run({fx("scale"), fx("chord", {{"i1", 4}, {"i2", 7}, {"i3", 0}}),
                    fx("arp", {{"rate", 3}, {"mode", 0}, {"pat", 0}, {"oct", 1}, {"gate", 0.8}}),
                    fx("velo", {{"scale", 0.5}, {"rand", 0}})},
                   {note(61, 0, 96)}, /*drum*/ true);
    REQUIRE(out.size() == 1);
    CHECK(out[0].pitch == 61);
    CHECK(out[0].vel == Approx(0.4).margin(1e-6));
}

TEST_CASE("midifx: disabled and unknown devices pass through", "[midifx]") {
    auto out = run({fx("chord", {{"i1", 4}, {"i2", 7}, {"i3", 0}}, /*on*/ false), fx("wobble")}, {note(60, 0, 96)});
    REQUIRE(out.size() == 1);
    CHECK(out[0].pitch == 60);
}

TEST_CASE("midifx: rand chance 0 drops everything and is deterministic", "[midifx]") {
    auto none = run({fx("rand", {{"chance", 0}, {"octave", 0}})}, {note(60, 0, 96), note(62, 96, 96)});
    CHECK(none.empty());

    auto roll = [](const std::string& id) {
        std::vector<project::Note> notes;
        for (int i = 0; i < 32; ++i) notes.push_back(note(60, i * 12.0, 12));
        auto out = run({fx("rand", {{"chance", 0.5}, {"octave", 0.5}})}, notes, false, id);
        std::vector<std::pair<int, double>> r;
        for (auto& e : out) r.push_back({e.pitch, e.tick});
        return r;
    };
    CHECK(roll("a") == roll("a"));
    CHECK(roll("a") != roll("b"));
}

TEST_CASE("midifx: expand sorts and applies the track chain", "[midifx]") {
    auto evs = run({fx("chord", {{"i1", 12}, {"i2", 0}, {"i3", 0}})}, {note(60, 96, 48), note(48, 0, 48)});
    REQUIRE(evs.size() == 4);
    for (size_t i = 1; i < evs.size(); ++i) CHECK(evs[i - 1].tick <= evs[i].tick);
    CHECK(evs[0].pitch == 48);
    CHECK(evs[1].pitch == 60);
}

// ---------------------------------------------------------------- empty chain, ordering

TEST_CASE("midifx: empty chain is the identity (sorted, stable, clamped)", "[midifx]") {
    std::vector<project::Note> notes = {note(64, 96, 48, 0.5, 0.25), note(60, 0, 24, 0.9), note(67, 96, 10, 0.7)};
    auto out = run({}, notes);
    REQUIRE(out.size() == 3);
    CHECK(ticks(out) == Dbls{0, 96, 96});
    CHECK(pitches(out) == Ints{60, 64, 67});  // equal ticks keep input order
    CHECK(out[0].durTicks == 24.0);
    CHECK(out[0].vel == Approx(0.9f));
    CHECK(out[1].vel == Approx(0.5f));
    CHECK(out[1].pr == Approx(0.25f));
    CHECK(run({}, {}).empty());
    // out-of-range pitch / negative duration are clamped like the Rust u8 cast / max(0)
    auto edge = run({}, {note(200, 0, -5), note(-3, 1, 5)});
    CHECK(edge[0].pitch == 127);
    CHECK(edge[0].durTicks == 0.0);
    CHECK(edge[1].pitch == 0);
}

TEST_CASE("midifx: devices apply in chain order", "[midifx]") {
    // scale then chord: 61 -> 60, then +4 -> 60, 64
    CHECK(pitches(run({fx("scale"), fx("chord", {{"i1", 4}})}, {note(61, 0, 96)})) == Ints{60, 64});
    // chord then scale: 61, 65 -> 60, 65
    CHECK(pitches(run({fx("chord", {{"i1", 4}}), fx("scale")}, {note(61, 0, 96)})) == Ints{60, 65});
}

// ---------------------------------------------------------------- scale

TEST_CASE("midifx: scale ties resolve downward in every direction", "[midifx]") {
    // C major (root 0): 61, 66, 70 and 68 sit between scale tones
    CHECK(pitches(run({fx("scale")}, {note(61, 0, 1), note(66, 1, 1), note(70, 2, 1), note(68, 3, 1)})) ==
          Ints{60, 65, 69, 67});
    // D major (root 2): 63 is equidistant from 62 and 64 -> 62
    CHECK(run({fx("scale")}, {note(63, 0, 1)}, false, "t", 2)[0].pitch == 62);
}

TEST_CASE("midifx: scale ids, fallback and root wrap", "[midifx]") {
    // unknown id falls back to major
    CHECK(run({fx("scale")}, {note(61, 0, 1)}, false, "t", 0, "wobble")[0].pitch == 60);
    // pentatonic minor on C: 62 is not in {0,3,5,7,10}; 61 down is 0? 62-1=61 no, 62+1=63 yes
    CHECK(run({fx("scale")}, {note(62, 0, 1)}, false, "t", 0, "pentMin")[0].pitch == 63);
    // blues has the b5
    CHECK(run({fx("scale")}, {note(66, 0, 1)}, false, "t", 0, "blues")[0].pitch == 66);
    // harmonic minor has the major 7th
    CHECK(run({fx("scale")}, {note(71, 0, 1)}, false, "t", 0, "harmMin")[0].pitch == 71);
    // A minor (root 9): 61 snaps to 60; pitch below the root wraps (rem_euclid): 1 -> 0
    CHECK(run({fx("scale")}, {note(61, 0, 1)}, false, "t", 9, "minor")[0].pitch == 60);
    CHECK(run({fx("scale")}, {note(1, 0, 1)}, false, "t", 9, "minor")[0].pitch == 0);
    // dorian vs mixolydian differ on the 3rd/6th/7th: E (64) is out of C dorian, snaps to Eb (63)
    CHECK(run({fx("scale")}, {note(64, 0, 1)}, false, "t", 0, "dorian")[0].pitch == 63);
    CHECK(run({fx("scale")}, {note(64, 0, 1)}, false, "t", 0, "mixo")[0].pitch == 64);
    CHECK(run({fx("scale")}, {note(63, 0, 1)}, false, "t", 0, "mixo")[0].pitch == 62);
    CHECK(run({fx("scale")}, {note(66, 0, 1)}, false, "t", 0, "pentMaj")[0].pitch == 67);
}

// ---------------------------------------------------------------- chord

TEST_CASE("midifx: chord zero means off, defaults are zero, stacked notes at 85%", "[midifx]") {
    // no params at all: Rust defaults are 0, so only the root survives
    CHECK(pitches(run({fx("chord")}, {note(60, 0, 96)})) == Ints{60});
    // i3 non-zero participates; order is root, i1, i2, i3
    auto out = run({fx("chord", {{"i1", 4}, {"i2", 7}, {"i3", 11}})}, {note(60, 0, 96, 1.0)});
    CHECK(pitches(out) == Ints{60, 64, 67, 71});
    CHECK(out[0].vel == 1.0f);
    CHECK(out[3].vel == Approx(0.85f));
    // i1 = 0 and i2 = 0 and i3 = 0 -> identity
    CHECK(pitches(run({fx("chord", {{"i1", 0}, {"i2", 0}, {"i3", 0}})}, {note(60, 0, 96)})) == Ints{60});
    // negative intervals, clamped to 0..127; fields other than pitch/vel are copied
    auto neg = run({fx("chord", {{"i1", -12}, {"i2", 24}})}, {note(5, 10, 33, 0.8, 0.5), note(120, 20, 5)});
    CHECK(pitches(neg) == Ints{5, 0, 29, 120, 108, 127});
    CHECK(neg[1].tick == 10.0);
    CHECK(neg[1].durTicks == 33.0);
    CHECK(neg[1].pr == 0.5f);
}

TEST_CASE("midifx: chord multiplies every note", "[midifx]") {
    auto out = run({fx("chord", {{"i1", 4}, {"i2", 7}})}, {note(60, 0, 96), note(62, 96, 96)});
    CHECK(pitches(out) == Ints{60, 64, 67, 62, 66, 69});
    CHECK(ticks(out) == Dbls{0, 0, 0, 96, 96, 96});
}

// ---------------------------------------------------------------- arp

TEST_CASE("midifx: arp rate indexes, clamping and defaults", "[midifx]") {
    const std::vector<std::pair<double, double>> steps = {{0, 96}, {1, 48}, {2, 32}, {3, 24}, {4, 16}, {5, 12}};
    for (auto [rate, step] : steps) {
        INFO("rate " << rate);
        auto out = run({fx("arp", {{"rate", rate}})}, {note(60, 0, 96)});
        REQUIRE(out.size() == size_t(96.0 / step));
        for (size_t i = 0; i < out.size(); ++i) CHECK(out[i].tick == Approx(double(i) * step));
    }
    CHECK(run({fx("arp", {{"rate", 9}})}, {note(60, 0, 96)})[1].tick == 12.0);    // clamps to 5
    CHECK(run({fx("arp", {{"rate", -3}})}, {note(60, 0, 192)})[1].tick == 96.0);  // clamps to 0
    CHECK(run({fx("arp", {{"rate", 3.9}})}, {note(60, 0, 96)})[1].tick == 24.0);  // x | 0 truncates
    // no params: Rust defaults rate 0 (96 ticks), gate 0.8
    auto def = run({fx("arp")}, {note(60, 0, 192)});
    REQUIRE(def.size() == 2);
    CHECK(ticks(def) == Dbls{0, 96});
    CHECK(def[0].durTicks == Approx(76.8));
}

TEST_CASE("midifx: arp directions on a held chord", "[midifx]") {
    std::vector<project::Note> chord = {note(67, 0, 192), note(60, 0, 192), note(64, 0, 192)};  // unsorted
    auto arp = [&](double mode) { return run({fx("arp", {{"rate", 3}, {"mode", mode}})}, chord); };
    auto up = arp(0);
    REQUIRE(up.size() == 8);  // 192 / 24
    CHECK(pitches(up) == Ints{60, 64, 67, 60, 64, 67, 60, 64});
    CHECK(ticks(up) == Dbls{0, 24, 48, 72, 96, 120, 144, 168});
    CHECK(pitches(arp(1)) == Ints{67, 64, 60, 67, 64, 60, 67, 64});
    CHECK(pitches(arp(2)) == Ints{60, 64, 67, 64, 60, 64, 67, 64});  // seq [60 64 67 64]
    // two-element sequences are not folded
    auto two = run({fx("arp", {{"rate", 3}, {"mode", 2}})}, {note(60, 0, 96), note(64, 0, 96)});
    CHECK(pitches(two) == Ints{60, 64, 60, 64});
}

TEST_CASE("midifx: arp random mode is seeded, in-set and replayable", "[midifx]") {
    auto f = fx("arp", {{"rate", 5}, {"mode", 3}});
    std::vector<project::Note> chord = {note(60, 0, 192), note(64, 0, 192), note(67, 0, 192)};
    auto a = run({f}, chord, false, "a");
    REQUIRE(a.size() == 16);
    // replay with the documented seed
    XorShift rng(fnv("a"));
    const int seq[3] = {60, 64, 67};
    for (auto& e : a) CHECK(e.pitch == seq[size_t(rng.nextF64() * 3.0) % 3]);
    CHECK(pitches(a) == pitches(run({f}, chord, false, "a")));
    CHECK(pitches(a) != pitches(run({f}, chord, false, "b")));
}

TEST_CASE("midifx: arp patterns (octave, 5ths, scale walk, select)", "[midifx]") {
    auto go = [](std::map<std::string, double> p, int pitch = 60, double dur = 192, double root = 0) {
        p["rate"] = 3;
        return pitches(run({fx("arp", p)}, {note(pitch, 0, dur)}, false, "t", root));
    };
    // octave: oct 3 -> 60 72 84 repeating
    CHECK(go({{"pat", 0}, {"oct", 3}}) == Ints{60, 72, 84, 60, 72, 84, 60, 72});
    // oct below 1 is raised to 1
    CHECK(go({{"pat", 0}, {"oct", 0}}) == Ints(8, 60));
    // 5ths: [60, 67]; with 2 octaves [60, 67, 72, 79]
    CHECK(go({{"pat", 1}, {"oct", 1}}) == Ints{60, 67, 60, 67, 60, 67, 60, 67});
    CHECK(go({{"pat", 1}, {"oct", 2}}) == Ints{60, 67, 72, 79, 60, 67, 72, 79});
    // scale walk from C in C major
    CHECK(go({{"pat", 2}, {"oct", 1}}) == Ints{60, 62, 64, 65, 67, 69, 71, 60});
    // from D, wrapping into the next octave
    CHECK(go({{"pat", 2}, {"oct", 1}}, 62, 168) == Ints{62, 64, 65, 67, 69, 71, 72});
    // off-scale held note snaps first (61 -> 60), two octaves
    auto two = go({{"pat", 2}, {"oct", 2}}, 61, 336);
    REQUIRE(two.size() == 14);
    CHECK(two.front() == 60);
    CHECK(two.back() == 83);
    // scale walk in A major (root 9, major scale) from A (57)
    CHECK(go({{"pat", 2}, {"oct", 1}}, 57, 168, 9) == Ints{57, 59, 61, 62, 64, 66, 68});
    // ... and in A minor from A (57)
    auto am = run({fx("arp", {{"rate", 3}, {"pat", 2}})}, {note(57, 0, 168)}, false, "t", 9, "minor");
    CHECK(pitches(am) == Ints{57, 59, 60, 62, 64, 65, 67});
    // select bitmask: bits 0 and 2 -> offsets 0, 2
    CHECK(go({{"pat", 3}, {"oct", 1}, {"sel", 5}}, 60, 96) == Ints{60, 62, 60, 62});
    CHECK(go({{"pat", 3}, {"oct", 1}, {"sel", 0}}, 60, 96) == Ints{60, 60, 60, 60});  // nothing on: the note
    CHECK(go({{"pat", 3}, {"oct", 1}}, 60, 96) == Ints{60, 60, 60, 60});               // default sel = 1
    CHECK(go({{"pat", 3}, {"oct", 2}, {"sel", 3}}, 60, 96) == Ints{60, 61, 72, 73});
    // overlapping sets are deduplicated: held 60,61 with offsets 0,1 -> {60,61,62}
    auto dd = run({fx("arp", {{"rate", 3}, {"pat", 3}, {"sel", 3}})}, {note(60, 0, 72), note(61, 0, 72)});
    CHECK(pitches(dd) == Ints{60, 61, 62});
}

TEST_CASE("midifx: arp gate, velocity, probability and pitch clamp", "[midifx]") {
    auto out = run({fx("arp", {{"rate", 5}, {"gate", 0.1}})}, {note(60, 0, 36, 0.33, 0.5), note(64, 0, 12, 0.9)});
    REQUIRE(out.size() == 3);
    CHECK(out[0].durTicks == 6.0);  // 12 * 0.1 = 1.2 -> floor at 6
    // velocity/probability come from the first note of the group (input order)
    CHECK(out[0].vel == Approx(0.33f));
    CHECK(out[0].pr == 0.5f);
    auto g = run({fx("arp", {{"rate", 0}, {"gate", 1.0}})}, {note(60, 0, 96)});
    CHECK(g[0].durTicks == 96.0);
    auto hi = run({fx("arp", {{"rate", 3}, {"oct", 3}})}, {note(120, 0, 72)});
    CHECK(pitches(hi) == Ints{120, 127, 127});  // 132 and 144 clamp to 127
}

TEST_CASE("midifx: arp groups by start tick, ends per group, output sorted", "[midifx]") {
    // group A at tick 0 (len 48), group B at tick 24 (len 48): rate 1 = 48-tick steps
    auto out = run({fx("arp", {{"rate", 1}})}, {note(60, 0, 48), note(72, 24, 48), note(64, 0, 96)});
    // group A: notes 60,64 end = 96 -> steps at 0, 48 ; seq [60,64] -> 60, 64
    // group B: note 72, end 72 -> steps at 24, 72? 24 < 72 yes, 72 < 72 no -> only tick 24
    CHECK(ticks(out) == Dbls{0, 24, 48});
    CHECK(pitches(out) == Ints{60, 72, 64});
    // an empty clip stays empty
    CHECK(run({fx("arp")}, {}).empty());
    // zero-length notes produce nothing
    CHECK(run({fx("arp")}, {note(60, 0, 0)}).empty());
}

TEST_CASE("midifx: arp then velo chain", "[midifx]") {
    auto out = run({fx("arp", {{"rate", 3}}), fx("velo", {{"scale", 0.5}})}, {note(60, 0, 48, 0.8)});
    REQUIRE(out.size() == 2);
    for (auto& e : out) CHECK(e.vel == Approx(0.4f));
}

// ---------------------------------------------------------------- velo / rand rolls

TEST_CASE("midifx: velo clamps and defaults", "[midifx]") {
    CHECK(run({fx("velo", {{"scale", 2.0}})}, {note(60, 0, 1, 0.8)})[0].vel == 1.0f);   // ceiling
    CHECK(run({fx("velo")}, {note(60, 0, 1, 0.8)})[0].vel == Approx(0.8f));             // scale 1, rand 0
    CHECK(run({fx("velo", {{"scale", 0.5}})}, {note(60, 0, 1, 0.0)})[0].vel == 0.05f);  // floor
}

TEST_CASE("midifx: velo rand replays the seeded roll exactly", "[midifx]") {
    std::vector<project::Note> notes;
    for (int i = 0; i < 20; ++i) notes.push_back(note(60, i * 10.0, 5, 0.6));
    auto out = run({fx("velo", {{"scale", 1.0}, {"rand", 0.3}})}, notes, false, "lead");
    XorShift rng(fnv("lead"));
    std::set<float> distinct;
    for (size_t i = 0; i < out.size(); ++i) {
        double e = std::min(std::max(0.6 * 1.0 + (rng.nextF64() * 2.0 - 1.0) * 0.3, 0.05), 1.0);
        CHECK(out[i].vel == static_cast<float>(e));
        CHECK(out[i].vel >= 0.3f - 1e-6f);
        CHECK(out[i].vel <= 0.9f + 1e-6f);
        distinct.insert(out[i].vel);
    }
    CHECK(distinct.size() > 10);
    // same track id -> identical; different id -> different
    auto again = run({fx("velo", {{"rand", 0.3}})}, notes, false, "lead");
    auto other = run({fx("velo", {{"rand", 0.3}})}, notes, false, "bass");
    bool same = true, diff = false;
    for (size_t i = 0; i < out.size(); ++i) {
        same = same && out[i].vel == again[i].vel;
        diff = diff || out[i].vel != other[i].vel;
    }
    CHECK(same);
    CHECK(diff);
}

TEST_CASE("midifx: rand device replays the seeded roll exactly", "[midifx]") {
    std::vector<project::Note> notes;
    for (int i = 0; i < 64; ++i) notes.push_back(note(60, i * 6.0, 6));
    auto out = run({fx("rand", {{"chance", 0.6}, {"octave", 0.4}})}, notes, false, "drums2");

    XorShift rng(fnv("drums2"));
    std::vector<std::pair<double, int>> expect;
    for (auto& n : notes) {
        if (rng.nextF64() > 0.6) continue;
        double p = n.pitch;
        if (rng.nextF64() < 0.4) p += rng.nextF64() < 0.5 ? 12 : -12;
        expect.push_back({n.startTicks, int(p)});
    }
    REQUIRE(out.size() == expect.size());
    for (size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i].tick == expect[i].first);
        CHECK(out[i].pitch == expect[i].second);
    }
    CHECK(out.size() > 20);
    CHECK(out.size() < 64);
}

TEST_CASE("midifx: rand octave 1 always shifts by +-12 and clamps", "[midifx]") {
    std::vector<project::Note> notes;
    for (int i = 0; i < 64; ++i) notes.push_back(note(60, i * 6.0, 6));
    auto out = run({fx("rand", {{"chance", 1.0}, {"octave", 1.0}})}, notes);
    REQUIRE(out.size() == 64);
    bool up = false, down = false;
    for (auto& e : out) {
        CHECK((e.pitch == 72 || e.pitch == 48));
        up = up || e.pitch == 72;
        down = down || e.pitch == 48;
    }
    CHECK(up);
    CHECK(down);
    for (auto& e : run({fx("rand", {{"octave", 1.0}})}, {note(5, 0, 1), note(125, 1, 1)})) {
        CHECK((e.pitch == 0 || e.pitch == 17 || e.pitch == 113 || e.pitch == 127));
    }
    // defaults (chance 1, octave 0): identity
    auto id = run({fx("rand")}, notes);
    CHECK(pitches(id) == Ints(64, 60));
}

// ---------------------------------------------------------------- drums, determinism

TEST_CASE("midifx: drum tracks still get velo and rand", "[midifx]") {
    CHECK(run({fx("rand", {{"chance", 0.0}})}, {note(36, 0, 6), note(38, 6, 6)}, true).empty());
    auto out = run({fx("velo", {{"scale", 0.5}}), fx("scale"), fx("chord", {{"i1", 4}}), fx("arp")},
                   {note(61, 0, 96, 0.8)}, true);
    REQUIRE(out.size() == 1);
    CHECK(out[0].pitch == 61);
    CHECK(out[0].vel == Approx(0.4f));
    // the same chain on a pitched track does transform
    CHECK(run({fx("scale")}, {note(61, 0, 96)}, false)[0].pitch == 60);
}

TEST_CASE("midifx: expansion is deterministic and seeded per track", "[midifx]") {
    std::vector<project::Note> notes;
    for (int i = 0; i < 32; ++i) notes.push_back(note(48 + i % 12, i * 12.0, 12, 0.7));
    std::vector<project::DeviceSpec> chain = {fx("velo", {{"rand", 0.4}}), fx("rand", {{"chance", 0.7}, {"octave", 0.3}}),
                                              fx("arp", {{"rate", 4}, {"mode", 3}})};
    auto a = run(chain, notes, false, "track-1");
    auto b = run(chain, notes, false, "track-1");
    auto c = run(chain, notes, false, "track-2");
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].tick == b[i].tick);
        CHECK(a[i].pitch == b[i].pitch);
        CHECK(a[i].vel == b[i].vel);
        CHECK(a[i].durTicks == b[i].durTicks);
    }
    bool differs = a.size() != c.size();
    for (size_t i = 0; !differs && i < a.size(); ++i)
        differs = a[i].pitch != c[i].pitch || a[i].vel != c[i].vel || a[i].tick != c[i].tick;
    CHECK(differs);
}
