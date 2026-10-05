// A1 engine tests: graph mixing, delay compensation, command lane and epochs, graph swap,
// test tone, meters, block-size independence, and the real-time rules (no allocation).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

#include "AllocGuard.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "harness/Metrics.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;

namespace {

constexpr double kSr = 48000.0;

const char* kProject = R"({"name":"t","scope":{"kind":"scene","sceneId":"s"},"project":{
  "meta":{"bpm":120},"scenes":[{"id":"s"}],
  "tracks":[
    {"id":"a","inst":{"type":"stubtone","params":{"level":0.3}},"fx":[{"type":"stubgain","on":true,"params":{"gain":0.5}}],"gain":-3,"pan":0.5},
    {"id":"b","inst":{"type":"stubtone","params":{"level":0.2}},"fx":[],"gain":0,"pan":-0.7}],
  "clips":{
    "a|s":{"len":384,"notes":{"n1":{"p":57,"s":0,"d":84,"v":0.9},"n2":{"p":64,"s":50,"d":200,"v":0.7},"n3":{"p":60,"s":301,"d":20,"v":1.0}}},
    "b|s":{"len":384,"notes":{"n1":{"p":45,"s":96,"d":96,"v":1.0},"n2":{"p":52,"s":97,"d":10,"v":0.5}}}}}})";

BuildResult build(uint32_t epoch = 1, const char* json = kProject) {
    return buildGraph(project::importFixtureJson(json), kSr, epoch);
}

Cmd makeCmd(CmdType t, uint32_t epoch = 0) { Cmd c; c.type = t; c.epoch = epoch; return c; }

// Launch scene 0 on every track (while stopped, so the batch lands together), then play.
void play(Engine& e, int tracks = 2, uint32_t epoch = 1) {
    for (int t = 0; t < tracks; ++t) {
        Cmd l = makeCmd(CmdType::ClipLaunch, epoch); l.clipLaunch = {uint16_t(t), 0};
        e.commands().push(l);
    }
    Cmd c = makeCmd(CmdType::TransportPlay); c.transportPlay = {0, 0.0};
    e.commands().push(c);
}

// Render `frames` through a fresh engine using callbacks of `block` frames.
std::pair<std::vector<float>, std::vector<float>> renderBlocks(int block, int frames) {
    Engine e; e.prepare(kSr);
    e.setInitialGraph(build().graph);
    play(e);
    const auto len = static_cast<size_t>(frames);
    std::vector<float> l(len), r(len);
    for (int pos = 0; pos < frames; pos += block) {
        const int n = std::min(block, frames - pos);
        e.process(l.data() + pos, r.data() + pos, n);
    }
    return {l, r};
}

// ---- fake devices (not registered; used through the Graph API) ----
class DelayFx final : public EffectDevice {
public:
    explicit DelayFx(int d) : d_(d), buf_(size_t(d) + 1, 0.0f) {}
    std::span<const ParamSpec> params() const override { return {}; }
    void prepare(double, int) override {}
    void setParam(uint16_t, float) override {}
    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int i = 0; i < n; ++i) {
            buf_[pos_] = l[i];
            pos_ = (pos_ + 1) % buf_.size();
            const float out = buf_[pos_];   // written d samples ago
            l[i] = r[i] = out;
        }
    }
    void reset() override {}
    int latencySamples() const override { return d_; }
private:
    int d_; std::vector<float> buf_; size_t pos_ = 0;
};

class ImpulseInst final : public InstrumentDevice {
public:
    std::span<const ParamSpec> params() const override { return {}; }
    void prepare(double, int) override {}
    void setParam(uint16_t, float) override {}
    void noteOn(uint8_t, float, uint32_t) override { pending_ = true; }
    void noteOff(uint32_t) override {}
    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        if (pending_ && n > 0) { l[0] += 0.25f; r[0] += 0.25f; pending_ = false; }
    }
    void reset() override {}
private:
    bool pending_ = false;
};

float peakOf(const std::vector<float>& v, size_t from, size_t to) {
    float p = 0; for (size_t i = from; i < to && i < v.size(); ++i) p = std::max(p, std::abs(v[i])); return p;
}

}  // namespace

TEST_CASE("empty project renders silence at a 64-frame buffer", "[engine]") {
    Engine e; e.prepare(kSr);
    e.setInitialGraph(std::make_unique<Graph>(1, kSr));   // no tracks
    std::vector<float> l(64), r(64);
    for (int i = 0; i < 1000; ++i) {
        e.process(l.data(), r.data(), 64);
        for (int k = 0; k < 64; ++k) { REQUIRE(l[size_t(k)] == 0.0f); REQUIRE(r[size_t(k)] == 0.0f); }
    }
    // no graph at all is also silent and safe
    Engine e2; e2.prepare(kSr);
    e2.process(l.data(), r.data(), 64);
    CHECK(harness::rms(l) == 0.0);
}

TEST_CASE("test tone: frequency, level, and fade-out", "[engine]") {
    Engine e; e.prepare(kSr);
    e.setInitialGraph(std::make_unique<Graph>(1, kSr));
    Cmd c = makeCmd(CmdType::TestTone); c.testTone = {1000.0f, -20.0f, 1};
    e.commands().push(c);
    std::vector<float> l(48000), r(48000);
    for (int pos = 0; pos < 48000; pos += 64) e.process(l.data() + pos, r.data() + pos, 64);
    const size_t lat = size_t(e.latencySamples());
    // steady state: peak 0.1 (-20 dBFS), 1 kHz -> 2000 zero crossings per second
    CHECK(peakOf(l, 24000, 48000) == Catch::Approx(0.1f).margin(0.002));
    int zc = 0; for (size_t i = lat + 24001; i < 48000; ++i) zc += (l[i - 1] < 0) != (l[i] < 0);
    CHECK(zc == Catch::Approx(2.0 * (48000 - 24001 - lat) / 48.0).margin(3));
    CHECK(l == r);
    // turning it off fades to silence
    c.testTone.on = 0; e.commands().push(c);
    std::vector<float> l2(24000), r2(24000);
    for (int pos = 0; pos < 24000; pos += 64) e.process(l2.data() + pos, r2.data() + pos, 64);
    CHECK(peakOf(l2, 20000, 24000) < 1e-4f);
    const auto m = e.meters().snapshot();
    CHECK(m.masterPeak < 0.025f);   // peak hold has decayed: 0.1 * exp(-0.5 s / 0.3 s) = 0.019
}

TEST_CASE("output is independent of the callback size (sample-accurate event splitting)", "[engine]") {
    constexpr int kFrames = 96000;   // 2 s
    auto ref = renderBlocks(128, kFrames);
    REQUIRE(harness::rms(ref.first) > 0.01);
    for (int block : {1, 7, 64, 100, 333, 1024, kFrames}) {
        INFO("block " << block);
        auto got = renderBlocks(block, kFrames);
        REQUIRE(got.first == ref.first);     // bit-exact
        REQUIRE(got.second == ref.second);
    }
}

TEST_CASE("mixer: gain, pan, mute and solo", "[engine]") {
    auto level = [](const char* json, bool left) {
        Engine e; e.prepare(kSr);
        e.setInitialGraph(buildGraph(project::importFixtureJson(json), kSr, 1).graph);
        play(e, 1);
        std::vector<float> l(24000), r(24000);
        for (int pos = 0; pos < 24000; pos += 128) e.process(l.data() + pos, r.data() + pos, std::min(128, 24000 - pos));
        return harness::rms(left ? l : r);
    };
    auto proj = [](const char* trackExtra) {
        return std::string(R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[{"id":"a","inst":{"type":"stubtone","params":{"level":0.2}},"fx":[])")
             + trackExtra + R"(}],"clips":{"a|s":{"len":384,"notes":{"n":{"p":69,"s":0,"d":300,"v":1.0}}}}}})";
    };
    const double base = level(proj(R"(,"gain":0,"pan":0)").c_str(), true);
    CHECK(base > 0.05);
    CHECK(level(proj(R"(,"gain":-6.0206,"pan":0)").c_str(), true) == Catch::Approx(base * 0.5).epsilon(0.01));
    CHECK(level(proj(R"(,"gain":0,"pan":1)").c_str(), true) < base * 0.001);     // hard right: left silent
    CHECK(level(proj(R"(,"gain":0,"pan":1)").c_str(), false) > base * 1.3);      // right gets left's energy (Web Audio law)
    CHECK(level(proj(R"(,"gain":0,"pan":0,"mute":true)").c_str(), true) == 0.0);
    // solo on the only track keeps it audible; muting an unsoloed track when another is soloed is covered by the builder
    CHECK(level(proj(R"(,"gain":0,"pan":0,"solo":true)").c_str(), true) == Catch::Approx(base).epsilon(1e-6));
}

TEST_CASE("param commands: resolver addresses, epoch rule, stale commands dropped", "[engine]") {
    Engine e; e.prepare(kSr);
    auto b = build(7);
    ParamAddr gainA{}, master{};
    REQUIRE(b.resolver.resolve("a|mix|gain", gainA));
    REQUIRE(b.resolver.resolve("master|gain", master));
    ParamAddr bogus;
    CHECK_FALSE(b.resolver.resolve("a|mix|nonexistent", bogus));
    ParamAddr instLevel{}, fxGain{};
    CHECK(b.resolver.resolve("a|inst|level", instLevel));
    CHECK(b.resolver.resolve("a|stubgain|gain", fxGain));
    e.setInitialGraph(std::move(b.graph));
    CHECK(e.liveEpoch() == 7);
    play(e, 2, 7);

    std::vector<float> l(24000), r(24000);
    auto run = [&] { for (int pos = 0; pos < 24000; pos += 128) e.process(l.data() + pos, r.data() + pos, std::min(128, 24000 - pos)); };
    Cmd c = makeCmd(CmdType::SetParam, 7);
    c.setParam = {master, -120.0f};          // master to silence with the right epoch
    e.commands().push(c);
    run();
    CHECK(peakOf(l, 12000, 24000) < 1e-3f);
    CHECK(e.diagnostics().staleCommandsDropped.load() == 0);

    c.epoch = 6;                              // wrong epoch: dropped, level unchanged
    c.setParam = {master, 0.0f};
    e.commands().push(c);
    run();
    CHECK(e.diagnostics().staleCommandsDropped.load() == 1);
    CHECK(peakOf(l, 12000, 24000) < 1e-3f);

    c.epoch = 7;                              // right epoch: applied (smoothed)
    e.commands().push(c);
    run();
    CHECK(peakOf(l, 12000, 24000) > 0.01f);
}

TEST_CASE("delay compensation aligns tracks with different latencies", "[engine]") {
    auto g = std::make_unique<Graph>(1, kSr);
    auto& a = g->addTrack();            // 100-sample effect latency
    a.inst = std::make_unique<ImpulseInst>();
    {
        FxSlot fs;
        fs.dev = std::make_unique<DelayFx>(100);
        fs.out.prepare(kSr, 15.0f); fs.out.snap(1.0f);
        a.fx.push_back(std::move(fs));
    }
    auto& b = g->addTrack();            // no latency; must be delayed by 100
    b.inst = std::make_unique<ImpulseInst>();
    g->finalize();
    CHECK(g->latencySamples() == 100);
    CHECK(g->trackCount() == 2);
    // pdc is internal; verify behaviour: trigger both on the same frame
    Graph* gp = g.get();
    Engine e; e.prepare(kSr);
    e.setInitialGraph(std::move(g));
    dsp::Limiter probe; probe.prepare(kSr);
    CHECK(e.latencySamples() == 100 + probe.latencySamples());   // graph latency plus the limiter lookahead
    gp->noteOn(0, 60, 1.0f, 1);
    gp->noteOn(1, 60, 1.0f, 2);
    std::vector<float> l(1024), r(1024);
    e.process(l.data(), r.data(), 1024);
    // Both impulses emerge together at index latencySamples() (100 graph + limiter lookahead).
    size_t first = 0; while (first < l.size() && std::abs(l[first]) < 1e-6f) ++first;
    REQUIRE(first < l.size());
    CHECK(int(first) == e.latencySamples());
    CHECK(l[first] == Catch::Approx(0.5f).margin(1e-5));   // 0.25 + 0.25 summed in the same sample
    int nonzero = 0; for (float v : l) nonzero += std::abs(v) > 1e-6f;
    CHECK(nonzero == 1);
}

TEST_CASE("graph swap: crossfade, retirement, no allocation on the audio thread", "[engine][realtime]") {
    Engine e; e.prepare(kSr);
    e.setInitialGraph(build(1).graph);
    play(e);
    Cmd tone = makeCmd(CmdType::TestTone); tone.testTone = {440.0f, -30.0f, 1};
    e.commands().push(tone);

    std::vector<float> l(64), r(64);
    for (int i = 0; i < 400; ++i) e.process(l.data(), r.data(), 64);    // warm: notes sounding

    auto next = build(2).graph;                                          // builder thread work, outside the guard
    REQUIRE(e.postGraph(next));
    CHECK(next == nullptr);                                              // ownership moved

    std::vector<float> out;
    out.reserve(50 * 64);   // the test's own allocation, kept outside the guard
    test::AllocGuard guard;
    for (int i = 0; i < 50; ++i) { e.process(l.data(), r.data(), 64); out.insert(out.end(), l.begin(), l.end()); }
    CHECK(guard.count() == 0);

    CHECK(e.liveEpoch() == 2);
    CHECK(e.diagnostics().graphSwaps.load() == 1);
    // continuity: no sample-to-sample jump anywhere near a click (signal peaks are < 0.5)
    float maxStep = 0; for (size_t i = 1; i < out.size(); ++i) maxStep = std::max(maxStep, std::abs(out[i] - out[i - 1]));
    CHECK(maxStep < 0.1f);
    auto old = e.takeRetired();                                          // builder collects and deletes
    REQUIRE(old != nullptr);
    CHECK(old->epoch() == 1);
    CHECK(e.takeRetired() == nullptr);
    // commands stamped with the old epoch are now stale
    Cmd c = makeCmd(CmdType::NoteOn, 1); c.noteOn = {0, 60, 1.0f, 99};
    e.commands().push(c);
    e.process(l.data(), r.data(), 64);
    CHECK(e.diagnostics().staleCommandsDropped.load() == 1);
}

TEST_CASE("meters report levels, limiter reduction and the playhead", "[engine]") {
    Engine e; e.prepare(kSr);
    e.setInitialGraph(build().graph);
    play(e);
    Cmd tone = makeCmd(CmdType::TestTone); tone.testTone = {200.0f, 6.0f, 1};   // +6 dBFS: the limiter must act
    e.commands().push(tone);
    std::vector<float> l(128), r(128);
    for (int i = 0; i < 300; ++i) e.process(l.data(), r.data(), 128);
    auto m = e.meters().snapshot();
    CHECK(m.playing);
    CHECK(m.playheadTicks == Catch::Approx(300.0 * 128.0 / (48000.0 * 60.0 / 120.0 / 96.0)).margin(0.5));
    CHECK(m.masterPeak > 0.5f);
    CHECK(m.masterPeak <= 0.8914f);          // limited to the -1 dBFS ceiling
    CHECK(m.limiterGrDb < -3.0f);
    CHECK(m.trackPeak[0] > 0.0f);
    CHECK(m.trackPeak[5] == 0.0f);           // no such track
}

TEST_CASE("engine process does not allocate (commands, events, tone, limiter)", "[engine][realtime]") {
    Engine e; e.prepare(kSr);
    e.setInitialGraph(build().graph);
    std::vector<float> l(64), r(64);
    ParamAddr g{}; build().resolver.resolve("a|mix|gain", g);
    test::AllocGuard guard;
    play(e);
    Cmd tone = makeCmd(CmdType::TestTone); tone.testTone = {330.0f, -12.0f, 1};
    e.commands().push(tone);
    for (int i = 0; i < 2000; ++i) {
        if (i % 50 == 0) { Cmd c = makeCmd(CmdType::SetParam, 1); c.setParam = {g, float(-(i % 20))}; e.commands().push(c); }
        if (i % 97 == 0) { Cmd c = makeCmd(CmdType::SetTempo); c.setTempo = {100.0 + (i % 40)}; e.commands().push(c); }
        e.process(l.data(), r.data(), 64);
    }
    CHECK(guard.count() == 0);
}

TEST_CASE("realtime budget at a 64-frame buffer, 8 tracks", "[engine][realtime][timing]") {
    std::string json = R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":150},"scenes":[{"id":"s"}],"tracks":[)";
    std::string clips;
    for (int t = 0; t < 8; ++t) {
        const std::string id = "t" + std::to_string(t);
        json += std::string(t ? "," : "") + R"({"id":")" + id + R"(","inst":{"type":"stubtone","params":{"level":0.1}},"fx":[{"type":"stubgain","on":true,"params":{"gain":0.9}}],"gain":0,"pan":)" + std::to_string(t / 4.0 - 1.0) + "}";
        clips += std::string(t ? "," : "") + "\"" + id + R"(|s":{"len":96,"notes":{"a":{"p":)" + std::to_string(48 + t) + R"(,"s":0,"d":30,"v":1},"b":{"p":)" + std::to_string(60 + t) + R"(,"s":10,"d":60,"v":0.8},"c":{"p":)" + std::to_string(67 + t) + R"(,"s":20,"d":70,"v":0.7}}})";
    }
    json += "],\"clips\":{" + clips + "}}}";
    Engine e; e.prepare(kSr);
    e.setInitialGraph(build(1, json.c_str()).graph);
    play(e, 8);
    std::vector<float> l(64), r(64);
    const int blocks = int(kSr / 64.0 * 10.0);   // 10 s
    const double budgetUs = 64.0 / kSr * 1e6;    // 1333 us
    std::vector<double> us; us.reserve(size_t(blocks));
    for (int i = 0; i < blocks; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        e.process(l.data(), r.data(), 64);
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        REQUIRE(std::isfinite(l[0]));
    }
    std::sort(us.begin(), us.end());
    const double p99 = us[size_t(0.99 * double(us.size()))], mx = us.back();
    std::printf("[realtime] 8 tracks, 64 frames @ 48 kHz: p99 %.1f us, max %.1f us, budget %.0f us (%.1f%% / %.1f%%)\n", p99, mx, budgetUs, 100 * p99 / budgetUs, 100 * mx / budgetUs);
#ifdef NDEBUG
    CHECK(p99 < budgetUs * 0.25);
    CHECK(mx < budgetUs);
#else
    WARN("timing assertions run in Release builds only");
#endif
}
