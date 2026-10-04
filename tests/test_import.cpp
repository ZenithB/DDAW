#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

#include "project/SynthyyImport.h"

using namespace ddaw::project;

TEST_CASE("importer reads a synthyy-shaped fixture", "[import]") {
    auto f = importFixtureFile(std::string(DDAW_FIXTURE_DIR) + "/ddaw/projects/stub-tone-gain.json");
    CHECK(f.name == "stub-tone-gain");
    CHECK(f.scope.kind == "scene");
    CHECK(f.scope.sceneId == "s1");
    REQUIRE(f.project.tracks.size() == 2);
    CHECK(f.project.meta.bpm == 120);
    const auto& t1 = f.project.tracks[0];
    CHECK(t1.inst.type == "stubtone");
    CHECK(t1.inst.params.at("level") == 0.5);
    REQUIRE(t1.fx.size() == 1);
    CHECK(t1.fx[0].type == "stubgain");
    CHECK(t1.gainDb == -6);
    CHECK(t1.pan == 0.5);
    REQUIRE(f.project.clips.count("t1|s1") == 1);
    CHECK(f.project.clips.at("t1|s1").notes.size() == 2);
    CHECK(f.project.clips.at("t1|s1").len == 384);
}

TEST_CASE("importer accepts bare ProjectJSON and rejects garbage", "[import]") {
    auto f = importFixtureJson(R"({"meta":{"bpm":90},"tracks":[{"id":"a","inst":{"type":"x","params":{}},"fx":[]}]})");
    CHECK(f.project.meta.bpm == 90);
    REQUIRE(f.project.tracks.size() == 1);
    CHECK_THROWS(importFixtureJson("{not json"));
    CHECK_THROWS(importFixtureJson(R"({"project":{"meta":{}}})"));  // no tracks array
    CHECK_THROWS(importFixtureFile("/nonexistent/fixture.json"));
}

TEST_CASE("importer keeps features the renderer must flag", "[import]") {
    auto f = importFixtureJson(R"({"project":{"tracks":[{"id":"a","inst":{"type":"mono","params":{}},"fx":[],
        "midifx":[{"type":"arp","on":true,"params":{}}],"sendA":0.6}],"clips":{"a|s":{"len":96,"notes":{},"env":{"inst||cutoff":[]}}},
        "scenes":[{"id":"s"}]}})");
    CHECK(f.project.tracks[0].midifx.size() == 1);
    CHECK(f.project.tracks[0].sendA == 0.6);
}
