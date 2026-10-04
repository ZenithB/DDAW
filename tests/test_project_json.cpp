// Project <-> JSON: every synthyy fixture must survive import -> write -> parse unchanged, UIDs are
// assigned once and stay stable, and a hand-built project with every feature round-trips.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "project/ProjectJson.h"
#include "project/SynthyyImport.h"

using namespace ddaw::project;
namespace fs = std::filesystem;

TEST_CASE("every synthyy fixture round-trips through the project JSON", "[projectjson]") {
    const fs::path dir = fs::path(DDAW_FIXTURE_DIR) / "synthyy" / "projects";
    if (!fs::exists(dir)) { SKIP("synthyy fixtures not synced"); }
    int n = 0;
    for (auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".json") continue;
        INFO(e.path().filename().string());
        const Fixture f = importFixtureFile(e.path().string());
        const auto j1 = projectToJson(f.project);
        const Project back = projectFromJson(j1);
        const auto j2 = projectToJson(back);
        REQUIRE(j1 == j2);                                    // the writer is the parser's inverse
        // and the semantics survive, not just the text
        REQUIRE(back.tracks.size() == f.project.tracks.size());
        REQUIRE(back.clips.size() == f.project.clips.size());
        ++n;
    }
    CHECK(n == 37);
}

TEST_CASE("uids are assigned once, preserved through JSON, and resume the counter", "[projectjson]") {
    Fixture f = importFixtureJson(R"({"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
      "tracks":[{"id":"a","inst":{"type":"mono","params":{}},"fx":[{"type":"comp","on":true,"params":{}}],"midifx":[{"type":"arp","on":true,"params":{}}]}],
      "clips":{"a|s":{"len":96,"notes":{"x":{"p":60,"s":0,"d":10,"v":1},"y":{"p":62,"s":10,"d":10,"v":1}}}},
      "masterFx":[{"type":"comp","on":true,"params":{}}]}})");
    CHECK(maxUid(f.project) == 0);       // a synthyy project carries no uids
    Uid next = 1;
    // track, instrument, effect, MIDI effect, two notes, master effect
    CHECK(assignUids(f.project, next) == 7);
    CHECK(next == 8);
    CHECK(maxUid(f.project) == 7);
    CHECK(assignUids(f.project, next) == 0);    // idempotent: nothing is reassigned

    // uids survive a trip through JSON, and the counter can resume from them
    Project back = projectFromJson(projectToJson(f.project));
    CHECK(projectToJson(back) == projectToJson(f.project));
    CHECK(back.tracks[0].uid == f.project.tracks[0].uid);
    CHECK(back.tracks[0].fx[0].uid == f.project.tracks[0].fx[0].uid);
    CHECK(back.clips.at("a|s").notes[1].uid == f.project.clips.at("a|s").notes[1].uid);
    Uid resumed = maxUid(back) + 1;
    CHECK(resumed == 8);
    back.tracks[0].fx.push_back(DeviceSpec{});
    CHECK(assignUids(back, resumed) == 1);
    CHECK(back.tracks[0].fx.back().uid == 8);
}

TEST_CASE("a project using every feature round-trips", "[projectjson]") {
    const char* json = R"({"project":{
      "meta":{"title":"t","bpm":97,"swing":0.2,"swingSubdivision":"8n","humanize":0.1,"root":3,"scale":"dorian","launchQ":2,"masterGain":-3,
              "loopOn":true,"loopStart":384,"loopEnd":1152,"tsTop":6,"tsBottom":8},
      "scenes":[{"id":"s1"},{"id":"s2"}],
      "tracks":[
        {"id":"lead","name":"Lead","kind":"synth","inst":{"type":"sampler","params":{"tune":2},"sampleId":"smp1","sampleName":"kick.wav","out":-2},
         "fx":[{"id":"d1","type":"delay","on":false,"params":{"time":0.3},"out":1.5},{"type":"duck","on":true,"srcTrack":"drums","srcPitch":36,"params":{}}],
         "midifx":[{"id":"m1","type":"arp","on":true,"params":{"rate":2}}],"gain":-3,"pan":0.4,"mute":true,"solo":false,"sendA":0.2,"sendB":0.1,
         "sends":{"bus1":0.5},"output":"bus1","send":"A",
         "lfos":[{"id":"l1","on":true,"shape":2,"sync":true,"rate":4,"hz":2,"depth":0.3,"phase":0.25,"dest":"inst","fxId":"","pkey":"tune",
                  "targets":[{"dest":"mix","fxId":"","pkey":"pan"}]}],
         "macros":[{"name":"m","value":0.4,"targets":[{"dest":"inst","fxId":"","pkey":"tune"}]}],
         "auto":{"mix||gain":[{"t":0,"v":0.2},{"t":96,"v":0.9}]}},
        {"id":"drums","name":"Drums","kind":"drum","inst":{"type":"drum","params":{},"padSamples":{"0":"smp2"},"padNames":{"0":"snare"}},"fx":[],"gain":0,"pan":0,"mute":false,"solo":true}],
      "clips":{"lead|s1":{"len":192,"notes":{"a":{"p":60,"s":0,"d":48,"v":0.8,"pr":0.5}},"env":{"inst||tune":[{"t":0,"v":0.5}]}},
               "drums|s2":{"len":96,"notes":{},"audio":{"sampleId":"loop","sampleName":"loop.wav","gainDb":-1,"pitch":2,"rev":1,"loop":1,"fadeIn":0.01,"fadeOut":0.02,"offset":0.5,"dur":2,"cents":10,"xfade":0.05}}},
      "arr":{"a1":{"trackId":"lead","start":384,"clip":{"len":96,"notes":{"n":{"p":64,"s":0,"d":24,"v":1}}}}},
      "returns":[{"id":"r1","name":"Verb","fxType":"reverb","params":{"size":0.7},"gain":-6}],
      "masterFx":[{"type":"comp","on":true,"params":{"thresh":-18}}],"masterAuto":{"mix||gain":[{"t":0,"v":0.5}]}}})";
    const Project p = importFixtureJson(json).project;
    const auto j1 = projectToJson(p);
    CHECK(projectToJson(projectFromJson(j1)) == j1);
    const Project b = projectFromJson(j1);
    CHECK(b.meta.swingSubdivision == "8n");
    CHECK(b.meta.tsTop == 6);
    CHECK(b.tracks[0].inst.sampleId == "smp1");
    CHECK(b.tracks[0].fx[0].outDb.value() == 1.5);
    CHECK_FALSE(b.tracks[0].fx[0].on);
    CHECK(b.tracks[0].fx[1].srcPitch.value() == 36);
    CHECK(b.tracks[0].send == SendBus::A);
    CHECK(b.tracks[0].lfos[0].targets.size() == 1);
    CHECK(b.tracks[1].inst.padSamples.at("0") == "smp2");
    REQUIRE(b.clips.at("drums|s2").audio.has_value());
    CHECK(b.clips.at("drums|s2").audio->xfade.value() == 0.05);
    CHECK(b.returns.size() == 1);
    CHECK(b.masterAuto.count("mix||gain") == 1);
}
