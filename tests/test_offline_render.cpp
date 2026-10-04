#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "engine/OfflineRender.h"
#include "harness/Metrics.h"
#include "project/SynthyyImport.h"

using namespace ddaw;

namespace {
project::Fixture load(const char* json) { return project::importFixtureJson(json); }

const char* kOneNote = R"({"name":"t","scope":{"kind":"scene","sceneId":"s"},"project":{
  "meta":{"bpm":120},"scenes":[{"id":"s"}],
  "tracks":[{"id":"a","inst":{"type":"stubtone","params":{"level":0.5}},"fx":[],"gain":0,"pan":0}],
  "clips":{"a|s":{"len":384,"notes":{"n":{"p":69,"s":0,"d":96,"v":1.0}}}}}})";
}

TEST_CASE("render length follows synthyy scope rules", "[render]") {
    auto r = engine::renderFixture(load(kOneNote), 44100.0);
    // 384 ticks * 2 at 120 bpm = 4 s, plus 1 s tail.
    CHECK(r.l.size() == 220500);
    CHECK(r.complete());
}

TEST_CASE("note timing and pitch are sample accurate", "[render]") {
    auto r = engine::renderFixture(load(kOneNote), 44100.0);
    // First note: A4 (440 Hz), starts at frame 0, 96 ticks = 0.5 s = 22050 frames, then 64-sample release.
    CHECK(harness::rms(std::span<const float>(r.l).subspan(100, 2000)) > 0.25);
    CHECK(harness::rms(std::span<const float>(r.l).subspan(22050 + 100, 4000)) == 0.0);  // silent after release
    // sample 0 is sin(0)=0; sample 1 must equal sin(2*pi*440/44100).
    CHECK(r.l[1] == Catch::Approx(0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 / 44100.0)).margin(1e-5));  // level 0.5: under the limiter ceiling
    // Clip loops every 384 ticks = 2 s: second pass starts at frame 88200.
    CHECK(r.l[88200 + 1] == Catch::Approx(r.l[1]).margin(1e-6));
}

TEST_CASE("unknown devices and unhandled features are reported, never dropped", "[render]") {
    auto fx = load(R"({"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
      "tracks":[{"id":"a","inst":{"type":"nosuchinst","params":{}},"fx":[{"type":"nosuchfx","on":true,"params":{"bogus":1}}]},
                {"id":"b","kind":"audio","inst":{"type":"audiobus","params":{}},"fx":[]}],
      "clips":{"a|s":{"len":96,"notes":{}}}},"scope":{"kind":"scene","sceneId":"s"}})");
    auto r = engine::renderFixture(fx, 44100.0);
    REQUIRE_FALSE(r.complete());
    auto has = [&](const std::string& s) { return std::find(r.unsupported.begin(), r.unsupported.end(), s) != r.unsupported.end(); };
    CHECK(has("instrument: nosuchinst"));
    CHECK(has("effect: nosuchfx"));
}

TEST_CASE("mute, solo, and bad input", "[render]") {
    auto fx = load(kOneNote);
    fx.project.tracks[0].mute = true;
    auto r = engine::renderFixture(fx, 44100.0);
    CHECK(harness::rms(r.l) == 0.0);
    fx.scope.sceneId = "missing";
    CHECK_THROWS(engine::renderFixture(fx, 44100.0));
}
