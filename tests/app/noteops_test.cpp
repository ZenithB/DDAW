// The note operations behind the piano roll's tools: quantize, legato, chop, slice, flips, transpose, stretch, humanize.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/model/NoteOps.h"

using namespace ddaw;
using namespace ddaw::app::noteops;
using Catch::Approx;

namespace {
Note note(int pitch, double start, double dur, double vel = 0.8) { Note n; n.uid = 1000 + uint64_t(pitch) * 10 + uint64_t(start); n.pitch = pitch; n.startTicks = start; n.durTicks = dur; n.velocity = vel; return n; }
}  // namespace

TEST_CASE("note ops: quantize moves starts onto the grid, by a strength, optionally with the ends", "[noteops]") {
    const std::vector<Note> in = {note(60, 10, 40), note(62, 50, 40), note(64, 100, 30)};
    const auto q = quantize(in, 24.0);
    CHECK(q[0].startTicks == 0.0);
    CHECK(q[1].startTicks == 48.0);
    CHECK(q[2].startTicks == 96.0);
    CHECK(q[0].durTicks == 40.0);                                         // a start-only quantize keeps the length
    const auto half = quantize(in, 24.0, 0.5);
    CHECK(half[0].startTicks == Approx(5.0));                              // half way from 10 to 0
    CHECK(half[2].startTicks == Approx(98.0));
    const auto both = quantize(in, 24.0, 1.0, QuantizeMode::StartsAndEnds);
    CHECK(both[0].startTicks == 0.0);
    CHECK(both[0].startTicks + both[0].durTicks == 48.0);                  // the end 50 snaps to 48
    for (const auto& n : both) CHECK(n.durTicks >= kMinDur);
    CHECK(quantize(in, 0.0)[0].startTicks == 10.0);                        // no grid: unchanged
}

TEST_CASE("note ops: legato stretches each note to the next start; the last keeps its length", "[noteops]") {
    const std::vector<Note> all = {note(60, 0, 10), note(64, 48, 10), note(67, 48, 10), note(62, 96, 10)};
    const auto l = legato(all, all);
    CHECK(l[0].durTicks == 48.0);
    CHECK(l[1].durTicks == 48.0);                                          // 48 -> 96
    CHECK(l[2].durTicks == 48.0);                                          // a note that starts with another is not cut by it
    CHECK(l[3].durTicks == 10.0);                                          // nothing after it
    const auto only = legato({all[0]}, all);                               // targets and context are separate
    CHECK(only[0].durTicks == 48.0);
}

TEST_CASE("note ops: chop and slice split notes", "[noteops]") {
    const auto c = chop({note(60, 0, 96)}, 4);
    REQUIRE(c.edited.size() == 1);
    REQUIRE(c.added.size() == 3);
    CHECK(c.edited[0].durTicks == 24.0);
    CHECK(c.added[2].startTicks == 72.0);
    CHECK(c.added[0].uid == 0);
    CHECK(chop({note(60, 0, 12)}, 8).added.empty());                       // pieces would be shorter than a 64th: left alone
    const auto s = slice({note(60, 0, 96), note(72, 0, 96), note(60, 96, 96)}, 36.0, 55, 65);
    REQUIRE(s.edited.size() == 1);                                         // only the note in the row range that spans the cut
    CHECK(s.edited[0].durTicks == 36.0);
    CHECK(s.added[0].startTicks == 36.0);
    CHECK(s.added[0].durTicks == 60.0);
    CHECK(slice({note(60, 0, 96)}, 2.0, 0, 127).edited.empty());           // too close to the start: no sliver
}

TEST_CASE("note ops: flips, transpose and stretch", "[noteops]") {
    const std::vector<Note> in = {note(60, 0, 24), note(64, 24, 24), note(67, 48, 48)};
    const auto f = flipTime(in);
    CHECK(f[0].startTicks == 72.0);                                        // the first note now ends where the last began
    CHECK(f[2].startTicks == 0.0);
    CHECK(f[1].startTicks == 48.0);
    const auto p = flipPitch(in);
    CHECK(p[0].pitch == 67);
    CHECK(p[2].pitch == 60);
    CHECK(p[1].pitch == 63);
    CHECK(transpose(in, 12)[0].pitch == 72);
    CHECK(transpose(in, 100)[2].pitch == 127);                             // clamped
    CHECK(transpose(in, -100)[0].pitch == 0);
    CHECK(transpose(in, 5, 7)[0].pitch == 7);                              // a drum clip has 8 rows
    const auto st = stretch(in, 2.0);
    CHECK(st[1].startTicks == 48.0);
    CHECK(st[2].durTicks == 96.0);
    CHECK(stretch(in, 0.5)[2].startTicks == 24.0);
    CHECK(flipTime({}).empty());
}

TEST_CASE("note ops: humanize is repeatable and bounded, velocities scale and set", "[noteops]") {
    std::vector<Note> in;
    for (int i = 0; i < 20; ++i) in.push_back(note(60, 96.0 * i + 10.0, 24, 0.5));
    const auto a = humanize(in, 6.0, 0.2, 7), b = humanize(in, 6.0, 0.2, 7), c = humanize(in, 6.0, 0.2, 8);
    bool differs = false;
    for (size_t i = 0; i < in.size(); ++i) {
        CHECK(a[i].startTicks == b[i].startTicks);
        CHECK(std::abs(a[i].startTicks - in[i].startTicks) <= 6.0 + 1e-9);
        CHECK(std::abs(a[i].velocity - 0.5) <= 0.2 + 1e-9);
        if (a[i].startTicks != c[i].startTicks) differs = true;
    }
    CHECK(differs);
    CHECK(scaleVelocity(in, 3.0)[0].velocity == 1.0);
    CHECK(scaleVelocity(in, 0.0)[0].velocity == Approx(0.02));
    CHECK(setVelocity(in, 0.9)[5].velocity == 0.9);
}

TEST_CASE("note ops: fitting to a clip length and the chord shapes", "[noteops]") {
    std::vector<Note> in = {note(60, 0, 100), note(62, 90, 100), note(64, 200, 10)};
    std::vector<Note> drop;
    fitToLength(in, 120.0, drop);
    REQUIRE(in.size() == 2);
    CHECK(in[1].durTicks == 30.0);
    CHECK(drop.size() == 1);
    for (const auto& c : chordShapes()) { CHECK(c.ivs.front() == 0); CHECK(c.ivs.size() >= 2); }
    CHECK(chordShapes()[0].ivs == std::vector<int>{0, 4, 7});
}
