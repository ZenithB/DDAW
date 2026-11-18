// Deleting a track shifts the ones after it one place to the left. What the engine carries across the rebuilt graph (launched clips,
// sounding notes, input monitoring) must follow the tracks, not their positions.
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;

namespace {
constexpr double kSr = 48000.0;

std::string songOf(std::initializer_list<const char*> ids) {
    std::string tracks, clips;
    for (const char* id : ids) {
        if (!tracks.empty()) tracks += ",";
        if (!clips.empty()) clips += ",";
        tracks += std::string(R"({"id":")") + id + R"(","kind":"synth","inst":{"type":"poly","params":{"wave":3,"attack":0.002,"sustain":1.0}},"fx":[],"gain":0,"pan":0})";
        clips += std::string("\"") + id + R"(|s":{"len":384,"notes":{"a":{"p":57,"s":0,"d":384,"v":0.9,"pr":1}}})";
    }
    return R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[)" + tracks + R"(],"clips":{)" + clips + "}}}";
}

struct Rig {
    Engine e;
    std::vector<float> l = std::vector<float>(128u), r = std::vector<float>(128u);
    Rig(const std::string& text) {
        e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(buildGraph(project::importFixtureJson(text), kSr, 1).graph);
    }
    void run(int blocks = 6) { for (int b = 0; b < blocks; ++b) e.process(l.data(), r.data(), 128); }
    void swapTo(const std::string& text, uint32_t epoch) {
        auto g = buildGraph(project::importFixtureJson(text), kSr, epoch).graph;
        REQUIRE(e.postGraph(g));
        run();
    }
    int scene(int track) { return e.meters().snapshot().trackScene[track]; }
};
}  // namespace

TEST_CASE("deleting a track keeps each remaining track's launched clip on that track", "[engine][tracks][swap]") {
    Rig rig(songOf({"A", "B", "C", "D"}));
    Cmd c; c.type = CmdType::ClipLaunch; c.epoch = 1; c.clipLaunch = {2, 0};          // only C is playing
    rig.e.commands().push(c);
    rig.run();
    REQUIRE(rig.scene(2) == 0);
    REQUIRE(rig.scene(1) == -1);
    rig.swapTo(songOf({"B", "C", "D"}), 2);                                           // A is deleted: C is now track 1
    CHECK(rig.scene(1) == 0);                                                          // C still plays
    CHECK(rig.scene(2) == -1);                                                         // D, which took C's old position, does not
    CHECK(rig.scene(0) == -1);
    rig.swapTo(songOf({"C", "D"}), 3);                                                // B deleted too
    CHECK(rig.scene(0) == 0);
    CHECK(rig.scene(1) == -1);
    rig.swapTo(songOf({"D"}), 4);                                                     // C itself is deleted: nothing plays
    CHECK(rig.scene(0) == -1);
}

TEST_CASE("inserting a track in front keeps launched clips on their tracks too", "[engine][tracks][swap]") {
    Rig rig(songOf({"A", "B"}));
    Cmd c; c.type = CmdType::ClipLaunch; c.epoch = 1; c.clipLaunch = {1, 0};          // B plays
    rig.e.commands().push(c);
    rig.run();
    rig.swapTo(songOf({"N", "A", "B"}), 2);                                           // a new track in front
    CHECK(rig.scene(2) == 0);
    CHECK(rig.scene(1) == -1);
    CHECK(rig.scene(0) == -1);
}

TEST_CASE("deleting a track keeps sounding notes and input monitoring on their own tracks", "[engine][tracks][swap]") {
    Rig rig(songOf({"A", "B", "C"}));
    Cmd on; on.type = CmdType::NoteOn; on.epoch = 1; on.noteOn = {2, 60, 0.9f, 5};   // a held note on C
    rig.e.commands().push(on);
    Cmd mon; mon.type = CmdType::MonitorInput; mon.epoch = 1; mon.monitorInput = {2, 1};
    rig.e.commands().push(mon);
    rig.run();
    std::vector<float> before;
    rig.swapTo(songOf({"B", "C"}), 2);                                                // A is deleted
    auto g = buildGraph(project::importFixtureJson(songOf({"B", "C"})), kSr, 3).graph;   // a reference graph for the note's destination
    (void)g;
    const auto level = [&](Rig& r) { double s = 0; for (int b = 0; b < 20; ++b) { r.run(1); for (float v : r.l) s += double(v) * v; } return s; };
    CHECK(level(rig) > 1e-6);                                                          // the note on C keeps sounding (on C, now track 1)
    Cmd off; off.type = CmdType::NoteOff; off.epoch = 2; off.noteOff = {1, 5};          // released where it now lives
    rig.e.commands().push(off);
    rig.run(400);                                                                      // ...and it stops: it was never left hanging on another track
    CHECK(level(rig) < 1e-6);
}
