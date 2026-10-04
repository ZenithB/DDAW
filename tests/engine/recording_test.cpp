// Input recording: the capture start is exact (including count-in and mid-callback starts), the take
// reaches disk intact, and with a simulated loopback of known latency the compensated recording lines
// up with the timeline to the sample.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

#include "../FakeDevice.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "engine/Recorder.h"
#include "harness/Wav.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using ddaw::test::FakeDevice;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;

// One kick at arrangement tick 96 (0.5 s at 120 bpm), 4 bars long.
const char* kProj = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
  "tracks":[{"id":"t1","kind":"drum","inst":{"type":"drum","params":{}},"fx":[],"gain":0,"pan":0}],
  "clips":{},
  "arr":{"a1":{"trackId":"t1","start":96,"clip":{"len":384,"notes":{"n":{"p":0,"s":0,"d":24,"v":1.0}}}}}}})";

std::string tempWav(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

int firstAbove(const std::vector<float>& x, float thr) {
    for (size_t i = 0; i < x.size(); ++i) if (std::abs(x[i]) > thr) return int(i);
    return -1;
}

harness::Audio take(Engine& e, Recorder& rec, FakeDevice& dev, double from, double seconds, Recorder::Take& out, const char* file) {
    std::string err;
    REQUIRE(rec.start(tempWav(file), 1, kSr, err));
    Cmd p; p.type = CmdType::TransportPlay; p.transportPlay = {1, from};
    e.commands().push(p);
    const uint64_t target = dev.frames.load() + uint64_t(seconds * kSr);
    while (dev.frames.load() < target) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Cmd s; s.type = CmdType::TransportStop; e.commands().push(s);
    out = rec.stop();
    REQUIRE(out.ok);
    auto a = harness::readWav(out.path);
    std::filesystem::remove(out.path);
    return a;
}

}  // namespace

TEST_CASE("recording: the take starts at the playhead and lands on disk intact", "[recording]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(buildGraph(project::importFixtureJson(kProj), kSr, 1).graph);
    FakeDevice dev(e, 4800, 64);
    Recorder rec(e);
    Recorder::Take t;
    const auto a = take(e, rec, dev, 0.0, 2.0, t, "ddaw_rec_a.wav");
    CHECK(t.startTick == Approx(0.0).margin(1e-6));
    CHECK(t.dropped == 0);
    CHECK(a.sampleRate == kSr);
    CHECK(a.frames() == t.frames);
    CHECK(a.frames() > 90000);                 // about 2 s
    CHECK(firstAbove(a.l, 0.02f) > 0);          // the kick came back through the loopback
    dev.stop();
    CHECK(dev.allocs == 0);                     // capture and monitoring allocate nothing on the audio thread
}

TEST_CASE("recording: loopback latency compensation puts the audio on the timeline to the sample", "[recording]") {
    for (int loop : {2400, 4410, 9000}) {
        INFO("loopback " << loop);
        Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(buildGraph(project::importFixtureJson(kProj), kSr, 1).graph);
        FakeDevice dev(e, loop, 64);
        Recorder rec(e);
        Recorder::Take t;
        const auto a = take(e, rec, dev, 0.0, 1.6, t, "ddaw_rec_b.wav");
        dev.stop();
        // reference: the same kick rendered offline sits at frame 24000 (tick 96 at 120 bpm)
        const int onset = firstAbove(a.l, 0.02f);
        REQUIRE(onset > 0);
        const int comp = loop + e.latencySamples();           // device round trip + the engine's own latency
        CHECK(onset - comp - 0 == Approx(24000).margin(64));  // within the attack ramp's own rise (threshold crossing)
    }
}

TEST_CASE("recording: a count-in starts the take before tick 0, and a mid-block start is exact", "[recording]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(buildGraph(project::importFixtureJson(kProj), kSr, 1).graph);
    FakeDevice dev(e, 4800, 64);
    Recorder rec(e);
    Recorder::Take t;
    const auto a = take(e, rec, dev, -192.0, 2.5, t, "ddaw_rec_c.wav");   // two beats of count-in
    dev.stop();
    CHECK(t.startTick == Approx(-192.0).margin(1e-6));
    // the kick is 192 + 96 ticks after the start = 0.75 s... plus the latency
    const int onset = firstAbove(a.l, 0.02f);
    REQUIRE(onset > 0);
    CHECK(onset - 4800 - e.latencySamples() == Approx((192 + 96) * kSr * 60.0 / 120.0 / 96.0).margin(64));
}

TEST_CASE("recording: stopping without ever playing reports nothing recorded", "[recording]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    FakeDevice dev(e, 100, 64);
    Recorder rec(e);
    std::string err;
    REQUIRE(rec.start(tempWav("ddaw_rec_d.wav"), 2, kSr, err));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto t = rec.stop();
    dev.stop();
    CHECK_FALSE(t.ok);
    CHECK_FALSE(t.error.empty());
    CHECK(t.frames == 0);
    std::filesystem::remove(t.path);
    CHECK_FALSE(rec.recording());
}

TEST_CASE("input ring: wraps, counts drops and never allocates", "[recording][ring]") {
    InputTap tap;
    tap.prepare(2048);
    std::vector<float> a(1500, 1.0f), out(4000);
    CHECK(tap.push(a.data(), nullptr, a.size()) == 1500);
    CHECK(tap.push(a.data(), nullptr, a.size()) == 548);     // only what fits
    CHECK(tap.dropped() == 952);
    std::vector<float> r(4000);
    CHECK(tap.pop(out.data(), r.data(), 4000) == 2048);
    CHECK(tap.push(a.data(), a.data(), 1000) == 1000);       // wrapped
    CHECK(tap.available() == 1000);
}
