// Pressing play starts every launched clip at once, whenever each of them was launched (a clip launched while the transport ran was
// queued for a bar line: that stale anchor must not hold it back when the transport starts again from the top).
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;

namespace {
constexpr double kSr = 48000.0;
const char* kSong = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120,"launchQ":1},"scenes":[{"id":"s"}],"tracks":[
  {"id":"A","kind":"synth","inst":{"type":"poly","params":{"wave":3,"attack":0.002,"sustain":1.0,"release":0.05}},"fx":[],"gain":0,"pan":0},
  {"id":"B","kind":"synth","inst":{"type":"poly","params":{"wave":3,"attack":0.002,"sustain":1.0,"release":0.05}},"fx":[],"gain":0,"pan":0}],
  "clips":{"A|s":{"len":384,"notes":{"a":{"p":57,"s":0,"d":300,"v":0.9,"pr":1}}},"B|s":{"len":384,"notes":{"a":{"p":64,"s":0,"d":300,"v":0.9,"pr":1}}}}}})";

struct Rig {
    Engine e;
    std::vector<float> l = std::vector<float>(128u), r = std::vector<float>(128u);
    Rig() { e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0}); e.setInitialGraph(buildGraph(project::importFixtureJson(kSong), kSr, 1).graph); }
    void run(int blocks) { for (int b = 0; b < blocks; ++b) e.process(l.data(), r.data(), 128); }
    void push(Cmd c) { c.epoch = 1; e.commands().push(c); }
    void launch(int track) { Cmd c; c.type = CmdType::ClipLaunch; c.clipLaunch = {uint16_t(track), 0}; push(c); }
    void play() { Cmd c; c.type = CmdType::TransportPlay; c.transportPlay = {0, 0.0}; push(c); }
    void stop() { Cmd c; c.type = CmdType::TransportStop; push(c); }
    // the loudest block of a track in the next `blocks` blocks
    float peakOf(int track, int blocks) {
        float pk = 0;
        for (int b = 0; b < blocks; ++b) { run(1); pk = std::max(pk, e.meters().snapshot().trackPeak[track]); }
        return pk;
    }
};
}  // namespace

TEST_CASE("global play starts every launched clip at once, however late each was launched", "[engine][transport][launch]") {
    Rig rig;
    rig.launch(0);                                  // launched while stopped: anchored at the top
    rig.play();
    rig.run(300);                                   // a little over half a bar (128 frames: 2.7 ms each)
    rig.launch(1);                                  // launched while playing: queued for the next bar line
    rig.run(40);
    rig.stop();
    rig.run(400);                                   // the meters die away
    // press play again: both are in play mode and both must sound within the first part of a bar, not after the bar line
    rig.play();
    float pa = 0, pb = 0;
    for (int blk = 0; blk < 120; ++blk) { rig.run(1); const auto s = rig.e.meters().snapshot(); pa = std::max(pa, s.trackPeak[0]); pb = std::max(pb, s.trackPeak[1]); }
    CHECK(pa > 0.01f);
    CHECK(pb > 0.01f);
    // and again, from a stop in the middle of a bar
    rig.run(200);
    rig.stop();
    rig.run(400);
    rig.play();
    pa = pb = 0;
    for (int blk = 0; blk < 120; ++blk) { rig.run(1); const auto s = rig.e.meters().snapshot(); pa = std::max(pa, s.trackPeak[0]); pb = std::max(pb, s.trackPeak[1]); }
    CHECK(pa > 0.01f);
    CHECK(pb > 0.01f);
}

TEST_CASE("a clip launched while the transport runs still waits for its bar line", "[engine][transport][launch]") {
    Rig rig;
    rig.launch(0);
    rig.play();
    rig.run(100);                                   // a bar is 750 blocks: about an eighth of the way in
    rig.launch(1);
    float early = 0;
    for (int blk = 0; blk < 60; ++blk) { rig.run(1); early = std::max(early, rig.e.meters().snapshot().trackPeak[1]); }
    CHECK(early < 0.001f);                          // not yet: quantised launches are untouched
    float later = 0;
    for (int blk = 0; blk < 800; ++blk) { rig.run(1); later = std::max(later, rig.e.meters().snapshot().trackPeak[1]); }
    CHECK(later > 0.01f);                           // after the bar line it sounds
}
