// B2 in the engine: input -> tracker -> the target track's instrument, the frame queue, live monitoring
// through a track's effects, and the real-time rules.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

#include "../AllocGuard.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;

const char* kProj = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
  "tracks":[{"id":"t1","kind":"synth","inst":{"type":"follow","params":{"wave":3,"level":1.0,"release":20}},"fx":[],"gain":0,"pan":0},
            {"id":"t2","kind":"audio","fx":[{"type":"stubgain","on":true,"params":{"gain":1.0}}],"gain":0,"pan":0}],
  "clips":{}}})";

struct Rig {
    Engine e;
    Rig() {
        e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(buildGraph(project::importFixtureJson(kProj), kSr, 1).graph);
    }
    void send(Cmd c) { c.epoch = 1; e.commands().push(c); }
};

// Feeds `x` through processIO in `block`-frame callbacks; returns the master output.
std::vector<float> run(Engine& e, const std::vector<float>& x, int block = 64) {
    const size_t nb = static_cast<size_t>(block);
    std::vector<float> out(x.size()), r(nb);
    for (size_t i = 0; i + size_t(block) <= x.size(); i += size_t(block)) e.processIO(x.data() + i, x.data() + i, out.data() + i, r.data(), block);
    return out;
}

std::vector<float> tone(double hz, double secs, float amp = 0.5f) {
    std::vector<float> x(size_t(secs * kSr));
    for (size_t i = 0; i < x.size(); ++i) x[i] = amp * float(std::sin(2.0 * std::numbers::pi * hz * double(i) / kSr));
    return x;
}

double freqOf(const std::vector<float>& x) {
    std::vector<double> t;
    for (size_t i = 1; i < x.size(); ++i) if (x[i - 1] < 0.0f && x[i] >= 0.0f) t.push_back(double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]));
    return t.size() < 3 ? 0.0 : double(t.size() - 1) * kSr / (t.back() - t.front());
}

}  // namespace

TEST_CASE("tracking: the voice follower sings what the input sings", "[tracking][engine]") {
    Rig r;
    Cmd t; t.type = CmdType::Tracking; t.tracking = {0}; r.send(t);
    r.e.setTrackerEnabled(true);
    const auto out = run(r.e, tone(247.0, 1.5));
    const std::vector<float> tail(out.end() - 9600, out.end());
    CHECK(freqOf(tail) == Approx(247.0).epsilon(0.01));
    double e2 = 0; for (float v : tail) e2 += double(v) * v;
    CHECK(std::sqrt(e2 / double(tail.size())) > 0.05);
    CHECK(r.e.performanceFrameCount() > 300);
    const auto f = r.e.latestPerformance();
    CHECK(f.f0Hz == Approx(247.0f).epsilon(0.01));
    CHECK(f.confidence > 0.9f);
    CHECK(f.loudnessDb > -30.0f);
}

TEST_CASE("tracking: without a target track the frames are produced but nothing sounds; off means off", "[tracking][engine]") {
    Rig r;
    r.e.setTrackerEnabled(true);
    const auto out = run(r.e, tone(247.0, 0.5));
    double e2 = 0; for (float v : out) e2 += double(v) * v;
    CHECK(e2 == 0.0);
    CHECK(r.e.performanceFrameCount() > 50);
    Rig off;
    run(off.e, tone(247.0, 0.5));
    CHECK(off.e.performanceFrameCount() == 0);       // tracker disabled: no work, no frames
}

TEST_CASE("tracking: input to sound latency through the engine is inside the interactive budget", "[tracking][engine][latency]") {
    for (int block : {64, 128}) {
        Rig r;
        Cmd t; t.type = CmdType::Tracking; t.tracking = {0}; r.send(t);
        r.e.setTrackerEnabled(true);
        std::vector<float> x(size_t(1.0 * kSr), 0.0f);
        const size_t onset = size_t(0.5 * kSr);
        for (size_t i = onset; i < x.size(); ++i) x[i] = 0.5f * float(std::sin(2.0 * std::numbers::pi * 247.0 * double(i - onset) / kSr));
        const auto out = run(r.e, x, block);
        size_t first = 0;
        for (size_t i = onset; i < out.size(); ++i) if (std::abs(out[i]) > 0.02f) { first = i; break; }
        REQUIRE(first > onset);
        const double ms = 1000.0 * double(first - onset) / kSr;
        std::printf("[tracking latency] %d-frame callbacks: the input starts, the synth is audible %.1f ms later (tracker look-ahead alone %.1f ms)\n", block, ms,
                    1000.0 * r.e.trackerLatencySamples() / kSr);
        CHECK(ms < 50.0);
    }
}

TEST_CASE("tracking: frames are queued in order with their input timestamps", "[tracking][engine]") {
    Rig r;
    r.e.setTrackerEnabled(true);
    run(r.e, tone(300.0, 0.5));
    Engine::TimedPerf p, prev{};
    int n = 0;
    double firstStamp = -1;
    while (r.e.popPerformance(p)) {
        if (n == 0) firstStamp = p.inputFrame;
        if (n > 0) CHECK(p.inputFrame - prev.inputFrame == Approx(192.0).margin(1.0));   // 64 samples at 16 kHz = 192 at 48 kHz
        prev = p;
        ++n;
    }
    CHECK(n > 50);
    CHECK(firstStamp == Approx(0.0).margin(1.0));
    CHECK(r.e.inputFrames() == 24000);
}

TEST_CASE("monitoring: the input passes through a track's effects, delayed by exactly one grid chunk", "[tracking][monitor]") {
    Cmd m; m.type = CmdType::MonitorInput; m.monitorInput = {1, 1};
    std::vector<float> x(8192);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * float(std::sin(0.05 * double(i)) + 0.5 * std::sin(0.31 * double(i)));
    for (int block : {64, 100, 512}) {
        Rig q;
        q.send(m);
        const auto out = run(q.e, x, block);
        const size_t n = (x.size() / size_t(block)) * size_t(block);
        for (size_t i = 128 + 8; i < n; ++i) REQUIRE(out[i] == Approx(x[i - 128]).margin(1e-6));   // the same signal, 128 frames later
        CHECK(Engine::monitorLatencyFrames() == 128);
    }
    // not switched on: silence
    Rig off;
    const auto dry = run(off.e, x);
    for (float v : dry) REQUIRE(v == 0.0f);
}

TEST_CASE("tracking and monitoring survive a graph swap, and never allocate on the audio thread", "[tracking][monitor][rt]") {
    Rig r;
    Cmd t; t.type = CmdType::Tracking; t.tracking = {0}; r.send(t);
    Cmd m; m.type = CmdType::MonitorInput; m.monitorInput = {1, 1}; r.send(m);
    r.e.setTrackerEnabled(true);
    const auto x = tone(220.0, 2.0);
    std::vector<float> l(64u), rr(64u);
    auto fresh = buildGraph(project::importFixtureJson(kProj), kSr, 2).graph;   // built off the audio thread, as in the app
    test::AllocGuard guard;
    for (size_t i = 0; i + 64 <= x.size(); i += 64) {
        if (i == 24000) {   // swap in a fresh graph mid-run
            r.e.postGraph(fresh);
        }
        r.e.processIO(x.data() + i, x.data() + i, l.data(), rr.data(), 64);
        if (i % 8192 == 0) { Cmd c; c.type = CmdType::TrackerConfig; c.trackerConfig = {100.0f, 1200.0f, 0.12f, 0.6f}; r.e.commands().push(c); }
    }
    CHECK(guard.count() == 0);
    CHECK(r.e.liveEpoch() == 2);
    // after the swap the follower still sings and the monitor path still carries the input
    double e2 = 0; for (size_t i = 0; i < 64; ++i) e2 += double(l[i]) * l[i];
    CHECK(e2 > 0.0);
    while (auto old = r.e.takeRetired()) {}
}

namespace {
// The audio track's gain effect (0..2) is driven by the input's pitch, 100 Hz -> 0 and 1 kHz -> 2 on a log scale.
const char* kPerfProj = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
  "tracks":[{"id":"t1","kind":"audio","fx":[{"type":"stubgain","on":true,"params":{"gain":1.0}}],"gain":0,"pan":0,
             "perf":[{"source":"f0","min":100,"max":1000,"targets":[{"dest":"fx","fxId":"stubgain","pkey":"gain"}]}]}],
  "clips":{}}})";

double peakOf(const std::vector<float>& x, size_t from, size_t to) { double m = 0; for (size_t i = from; i < to; ++i) m = std::max(m, double(std::abs(x[i]))); return m; }
}  // namespace

TEST_CASE("performance routes: the input's pitch drives a parameter, and it snaps back when tracking stops", "[tracking][perf]") {
    Cmd mon; mon.type = CmdType::MonitorInput; mon.monitorInput = {0, 1}; mon.epoch = 1;
    for (double hz : {200.0, 500.0}) {
        Engine e;
        e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(buildGraph(project::importFixtureJson(kPerfProj), kSr, 1).graph);
        e.commands().push(mon);
        e.setTrackerEnabled(true);
        const auto out = run(e, tone(hz, 1.0, 0.4f));
        const double expected = 2.0 * std::log(hz / 100.0) / std::log(10.0);      // gain = 2 * u
        INFO("pitch " << hz << " Hz, expected gain " << expected);
        CHECK(peakOf(out, 40000, 47000) == Approx(0.4 * expected).epsilon(0.03));
        // tracker off: the route deactivates and the stored gain (1.0) comes back
        e.setTrackerEnabled(false);
        const auto after = run(e, tone(hz, 0.6, 0.4f));
        CHECK(peakOf(after, 20000, 28000) == Approx(0.4).epsilon(0.03));
    }
}
