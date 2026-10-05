// A3 real-time gate and engine-level features: the allocation rule across the whole new engine surface
// (scheduler, buses, modulation, ducks, swap), the metronome, and the arrangement / loop render scopes.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../AllocGuard.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "engine/OfflineRender.h"
#include "harness/Metrics.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;

Cmd cmd(CmdType t, uint32_t epoch = 0) { Cmd c; c.type = t; c.epoch = epoch; return c; }

// Everything the new engine can do, in one project.
const char* kKitchenSink = R"({"scope":{"kind":"scene","sceneId":"s"},"project":{
  "meta":{"bpm":132,"swing":0.4,"swingSubdivision":"16n","humanize":0.5,"root":9,"scale":"minor","launchQ":1,
          "masterGain":-2,"loopOn":true,"loopStart":0,"loopEnd":768},
  "scenes":[{"id":"s"},{"id":"s2"}],
  "tracks":[
    {"id":"lead","kind":"synth","inst":{"type":"mono","params":{"cutoff":1200}},"fx":[{"type":"delay","on":true,"params":{}},{"type":"comp","on":true,"params":{}}],
     "gain":-3,"pan":-0.2,"sendA":0.3,"sendB":0.2,"sends":{"bx":0.4},
     "midifx":[{"type":"arp","on":true,"params":{"rate":3,"mode":1,"oct":2,"gate":0.6}},{"type":"velo","on":true,"params":{"scale":0.8,"rand":0.2}}],
     "lfos":[{"id":"l1","on":true,"shape":0,"sync":false,"rate":5,"hz":3,"depth":0.4,"phase":0,"targets":[{"dest":"inst","fxId":"","pkey":"cutoff"},{"dest":"mix","fxId":"","pkey":"pan"}]},
             {"id":"l2","on":true,"shape":2,"sync":true,"rate":5,"hz":1,"depth":0.5,"phase":0,"dest":"lfo","fxId":"l1","pkey":"hz"}],
     "macros":[{"name":"m","value":0.3,"targets":[{"dest":"inst","fxId":"","pkey":"res"},{"dest":"mix","fxId":"","pkey":"gain"}]}],
     "auto":{"mix||gain":[{"t":0,"v":0.8},{"t":768,"v":0.9}]}},
    {"id":"pad","kind":"synth","inst":{"type":"poly","params":{}},"fx":[{"type":"duck","on":true,"srcTrack":"drums","params":{"rate":1,"amount":0.8,"curve":0.5}},{"type":"chorus","on":true,"params":{}}],
     "gain":-6,"pan":0.3,"sendA":0.4},
    {"id":"drums","kind":"drum","inst":{"type":"drum","params":{}},"fx":[],"gain":-2,"pan":0},
    {"id":"bx","kind":"bus","output":"by","inst":{"type":"audiobus","params":{}},"fx":[{"type":"filter","on":true,"params":{}}],"gain":0,"pan":0},
    {"id":"by","kind":"bus","output":"bx","inst":{"type":"audiobus","params":{}},"fx":[],"gain":-9,"pan":0},
    {"id":"A","kind":"bus","send":"A","inst":{"type":"audiobus","params":{}},"fx":[{"type":"reverb","on":true,"params":{}}],"gain":0,"pan":0},
    {"id":"F","kind":"bus","send":"F","sends":{"F":0.4},"inst":{"type":"audiobus","params":{}},"fx":[],"gain":0,"pan":0}],
  "clips":{
    "lead|s":{"len":384,"notes":{"a":{"p":57,"s":0,"d":90,"v":0.9,"pr":1},"b":{"p":60,"s":96,"d":90,"v":0.8,"pr":0.7},"c":{"p":64,"s":192,"d":180,"v":1.0,"pr":1}},
              "env":{"inst||cutoff":[{"t":0,"v":0.2},{"t":384,"v":0.9}],"fx|delay|mix":[{"t":0,"v":0.1}]}},
    "pad|s":{"len":768,"notes":{"a":{"p":45,"s":0,"d":700,"v":0.7,"pr":1}}},
    "drums|s":{"len":96,"notes":{"a":{"p":36,"s":0,"d":10,"v":1.0,"pr":1},"b":{"p":38,"s":48,"d":10,"v":0.9,"pr":1}}},
    "lead|s2":{"len":192,"notes":{"a":{"p":52,"s":0,"d":90,"v":1.0,"pr":1}}}},
  "returns":[{"name":"r","fxType":"reverb","params":{},"gain":0}],
  "masterFx":[{"type":"comp","on":true,"params":{"thresh":-12,"ratio":3}}]}})";

BuildResult build(uint32_t epoch) { return buildGraph(project::importFixtureJson(kKitchenSink), kSr, epoch); }

void launchAll(Engine& e, int tracks, int scene, uint32_t epoch) {
    for (int t = 0; t < tracks; ++t) { Cmd c = cmd(CmdType::ClipLaunch, epoch); c.clipLaunch = {uint16_t(t), uint16_t(scene)}; e.commands().push(c); }
}

}  // namespace

TEST_CASE("the kitchen-sink project builds with only the known gaps reported", "[engine][realtime]") {
    auto b = build(1);
    for (auto& u : b.unsupported) INFO(u);
    // a bus feeding itself through a cycle, midi-fx, modulation, ducks, returns... all supported; nothing else is flagged
    CHECK(b.unsupported.empty());
    CHECK(b.graph->trackCount() == 7);
}

TEST_CASE("the whole engine surface performs no allocation on the audio thread", "[engine][realtime]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::ToneCompat, 0});
    auto b1 = build(1);
    ParamAddr gain{}, sendA{};
    REQUIRE(b1.resolver.resolve("lead|mix|gain", gain));
    REQUIRE(b1.resolver.resolve("lead|mix|sendA", sendA));
    e.setInitialGraph(std::move(b1.graph));
    auto next = build(2).graph;                       // the swap target, built on the "builder thread"
    std::vector<float> l(64), r(64);

    test::AllocGuard guard;
    launchAll(e, 3, 0, 1);
    { Cmd c = cmd(CmdType::SetTempo); c.setTempo = {140.0}; e.commands().push(c); }
    { Cmd c = cmd(CmdType::TransportPlay); c.transportPlay = {0, 0.0}; e.commands().push(c); }
    e.commands().push(cmd(CmdType::MetronomeOn));
    { Cmd c = cmd(CmdType::TestTone); c.testTone = {440.0f, -30.0f, 1}; e.commands().push(c); }
    for (int i = 0; i < 6000; ++i) {                   // about 8 s at 64 frames
        if (i % 100 == 0) { Cmd c = cmd(CmdType::SetParam, 1); c.setParam = {gain, float(-(i % 12))}; e.commands().push(c); }
        if (i % 250 == 0) { Cmd c = cmd(CmdType::SetParam, 1); c.setParam = {sendA, 0.5f}; e.commands().push(c); }
        if (i == 800) launchAll(e, 3, 1, 1);           // a relaunch onto scene 2 while playing
        if (i == 1500) e.commands().push(cmd(CmdType::TransportStop));
        if (i == 1600) { Cmd c = cmd(CmdType::TransportPlay); c.transportPlay = {1, 100.0}; e.commands().push(c); }   // arrangement mode, mid-song
        if (i == 3000) { Cmd c = cmd(CmdType::TransportPlay); c.transportPlay = {0, 0.0}; e.commands().push(c); }
        if (i == 3500) e.postGraph(next);              // a graph swap with the launched clips carried over
        if (i == 4000) { Cmd c = cmd(CmdType::NoteOn, 2); c.noteOn = {0, 60, 0.9f, 77}; e.commands().push(c); }
        if (i == 4100) { Cmd c = cmd(CmdType::NoteOff, 2); c.noteOff = {0, 77}; e.commands().push(c); }
        if (i == 4500) e.commands().push(cmd(CmdType::MetronomeOff));
        e.process(l.data(), r.data(), 64);
        REQUIRE(std::isfinite(l[0]));
    }
    CHECK(guard.count() == 0);
    CHECK(e.diagnostics().graphSwaps.load() == 1);
    CHECK(e.takeRetired() != nullptr);
}

// Render-path parity: the same project must produce bit-identical output whatever the host's callback
// size (live at 32 or 64 frames, offline at 128), including while automation, LFOs, sends and smoothed
// gains are moving. The engine renders on a fixed internal grid to guarantee this.
TEST_CASE("output is bit-identical for every callback size, with everything moving", "[engine][determinism]") {
    auto renderWith = [](int block) {
        Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::ToneCompat, 0});
        e.setInitialGraph(build(1).graph);
        launchAll(e, 3, 0, 1);
        { Cmd c = cmd(CmdType::SetTempo); c.setTempo = {137.0}; e.commands().push(c); }
        { Cmd c = cmd(CmdType::TransportPlay); c.transportPlay = {0, 0.0}; e.commands().push(c); }
        e.commands().push(cmd(CmdType::MetronomeOn));
        const size_t total = static_cast<size_t>(3.0 * kSr);
        std::vector<float> l(total), r(total);
        for (size_t pos = 0; pos < total; pos += size_t(block)) e.process(l.data() + pos, r.data() + pos, int(std::min<size_t>(size_t(block), total - pos)));
        l.insert(l.end(), r.begin(), r.end());
        return l;
    };
    const auto ref = renderWith(128);
    REQUIRE(harness::rms(ref) > 0.001);
    for (int block : {1, 17, 32, 64, 100, 127, 129, 333, 1024}) {
        INFO("callback size " << block);
        CHECK(renderWith(block) == ref);              // bit-exact
    }
}

TEST_CASE("the kitchen sink stays within the real-time budget at 64 frames", "[engine][realtime][timing]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::ToneCompat, 0});
    e.setInitialGraph(build(1).graph);
    launchAll(e, 3, 0, 1);
    { Cmd c = cmd(CmdType::TransportPlay); c.transportPlay = {0, 0.0}; e.commands().push(c); }
    std::vector<float> l(64), r(64);
    const int blocks = int(kSr / 64.0 * 10.0);
    const double budgetUs = 64.0 / kSr * 1e6;
    std::vector<double> us; us.reserve(size_t(blocks));
    for (int i = 0; i < blocks; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        e.process(l.data(), r.data(), 64);
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(us.begin(), us.end());
    const double p99 = us[size_t(0.99 * double(us.size()))], mx = us.back();
    std::printf("[realtime] kitchen sink (7 tracks, 4 buses, reverbs, delay, comp, chorus, filter), 64 frames: p99 %.1f us, max %.1f us, budget %.0f us (%.1f%% / %.1f%%)\n",
                p99, mx, budgetUs, 100 * p99 / budgetUs, 100 * mx / budgetUs);
#ifdef NDEBUG
    CHECK(p99 < budgetUs * 0.5);
    CHECK(mx < budgetUs);
#else
    WARN("timing assertions run in Release builds only");
#endif
}

TEST_CASE("metronome: a click every beat, accented on the bar, silent when off", "[engine]") {
    const std::string json = R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120,"tsTop":4,"tsBottom":4},
      "scenes":[{"id":"s"}],"tracks":[{"id":"t1","kind":"synth","inst":{"type":"stubtone","params":{}},"fx":[],"gain":0,"pan":0}],"clips":{}}})";
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(buildGraph(project::importFixtureJson(json), kSr, 1).graph);
    { Cmd c = cmd(CmdType::SetTempo); c.setTempo = {120.0}; e.commands().push(c); }
    e.commands().push(cmd(CmdType::MetronomeOn));
    { Cmd c = cmd(CmdType::TransportPlay); c.transportPlay = {0, 0.0}; e.commands().push(c); }
    std::vector<float> l(size_t(2.2 * kSr)), r(l.size());
    for (size_t pos = 0; pos < l.size(); pos += 64) e.process(l.data() + pos, r.data() + pos, int(std::min<size_t>(64, l.size() - pos)));
    auto peak = [&](double a, double b) { float p = 0; for (size_t i = size_t(a * kSr); i < size_t(b * kSr); ++i) p = std::max(p, std::abs(l[i])); return p; };
    CHECK(peak(0.0, 0.03) > 0.3f);                    // beat 1: the accent (0.5 velocity)
    CHECK(peak(0.5, 0.53) > 0.15f);                   // beat 2 (0.25)
    CHECK(peak(0.5, 0.53) < 0.3f);
    CHECK(peak(0.3, 0.45) < 1e-4f);                   // silent between clicks
    CHECK(peak(2.0, 2.03) > 0.3f);                    // beat 1 of the second bar: accented again
    CHECK(peak(1.5, 1.53) < 0.3f);                    // beat 4: not accented
    // off: no more clicks
    e.commands().push(cmd(CmdType::MetronomeOff));
    std::vector<float> l2(size_t(1.2 * kSr)), r2(l2.size());
    for (size_t pos = 0; pos < l2.size(); pos += 64) e.process(l2.data() + pos, r2.data() + pos, int(std::min<size_t>(64, l2.size() - pos)));
    CHECK(harness::rms(std::span<const float>(l2).subspan(4800)) == 0.0);
}

TEST_CASE("render scopes: arrangement and loop", "[engine][render]") {
    const std::string base = R"({"project":{"meta":{"bpm":120,"loopOn":true,"loopStart":384,"loopEnd":768},"scenes":[{"id":"s"}],
      "tracks":[{"id":"t1","kind":"synth","inst":{"type":"stubtone","params":{"level":0.3}},"fx":[],"gain":0,"pan":0}],"clips":{},
      "arr":{"a1":{"trackId":"t1","start":384,"clip":{"len":384,"notes":{"n":{"p":69,"s":0,"d":300,"v":1.0}}}}}},"scope":{"kind":")";
    auto render = [&](const char* kind) { return renderFixture(project::importFixtureJson(base + kind + "\"}}"), kSr, RenderOptions{}); };

    const auto arr = render("arr");                   // 0 .. end of the last clip (768), plus the tail
    CHECK(arr.l.size() == static_cast<size_t>(std::ceil(768 * (kSr * 60 / 120 / 96) + 1.0 * kSr)));
    CHECK(harness::rms(std::span<const float>(arr.l).subspan(0, size_t(1.9 * kSr))) == 0.0);       // nothing before tick 384 (2 s at 120 bpm)
    CHECK(harness::rms(std::span<const float>(arr.l).subspan(size_t(2.1 * kSr), 4800)) > 0.05);    // the note sounds from tick 384

    const auto lp = render("loop");                   // the loop region 384 .. 768 (2 s), played through once
    CHECK(lp.l.size() == static_cast<size_t>(std::ceil(384 * (kSr * 60 / 120 / 96) + 1.0 * kSr)));
    CHECK(harness::rms(std::span<const float>(lp.l).subspan(size_t(0.1 * kSr), 4800)) > 0.05);      // starts at the loop start

    // an empty arrangement is an error, as in synthyy
    auto empty = project::importFixtureJson(R"({"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
      "tracks":[{"id":"t1","kind":"synth","inst":{"type":"stubtone","params":{}},"fx":[],"gain":0,"pan":0}],"clips":{}},"scope":{"kind":"arr"}})");
    CHECK_THROWS(renderFixture(empty, kSr, RenderOptions{}));
}
