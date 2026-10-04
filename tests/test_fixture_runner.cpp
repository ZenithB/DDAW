// M0 exit criterion, via the real path: import synthyy-format fixture JSON ->
// render through the engine -> compare to a golden WAV produced by an
// independent Python reference (tests/fixtures/make_stub_golden.py).
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

#include "harness/FixtureRunner.h"

using namespace ddaw::harness;

TEST_CASE("stub fixture passes the parity runner at the tight tier", "[runner]") {
    RunOptions opt;
    opt.fixturesDir = std::string(DDAW_FIXTURE_DIR) + "/ddaw";
    opt.tierFor = [](const std::string&) { return Tier::Tight; };
    auto reports = runFixtures(opt);
    REQUIRE(reports.size() == 1);
    const auto& r = reports[0];
    INFO(formatTable(reports));
    CHECK(r.name == "stub-tone-gain");
    CHECK(r.status == Status::Pass);
    CHECK(r.rmsNullDb <= -60.0);
    CHECK(r.similarity >= 0.98);
    CHECK(r.levelDb > -40.0);  // audible
}

TEST_CASE("runner reports a failing comparison", "[runner]") {
    RunOptions opt;
    opt.fixturesDir = std::string(DDAW_FIXTURE_DIR) + "/ddaw";
    opt.goldenLeadSec = 0.05;  // wrong alignment on purpose
    opt.tierFor = [](const std::string&) { return Tier::Tight; };
    auto reports = runFixtures(opt);
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].status == Status::Fail);
}

TEST_CASE("runner reports a missing golden and a bad fixture", "[runner]") {
    RunOptions opt;
    auto r1 = runFixture(std::string(DDAW_FIXTURE_DIR) + "/ddaw/projects/stub-tone-gain.json", "/nonexistent.wav", opt);
    CHECK(r1.status == Status::NoGolden);
    auto r2 = runFixture("/nonexistent.json", "/nonexistent.wav", opt);
    CHECK(r2.status == Status::Error);
}

// Every synthyy fixture, once copied in: not-yet-ported devices must surface as
// 'unported' with reasons, and nothing may crash or go non-finite.
TEST_CASE("synthyy fixtures all load and are classified", "[runner][.synthyy]") {
    RunOptions opt;
    opt.fixturesDir = std::string(DDAW_FIXTURE_DIR) + "/synthyy";
    auto reports = runFixtures(opt);
    REQUIRE(reports.size() == 37);
    for (auto& r : reports) {
        INFO(r.name);
        CHECK(r.status != Status::Error);
        CHECK(r.status != Status::Fail);
    }
}
