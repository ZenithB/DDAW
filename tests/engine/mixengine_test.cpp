// Mixer and engine follow-ups: the project's key reaches the devices that follow it, sends are delay-compensated, per-band
// meters of the multiband compressor.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>

#include "devices/Registry.h"
#include "engine/GraphBuilder.h"
#include "engine/Meters.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {
constexpr double kSr = 48000.0;

struct Probe final : EffectDevice {
    static inline int root = -99, scale = -99, calls = 0;
    std::span<const ParamSpec> params() const override { return {}; }
    void prepare(double, int) override {}
    void setParam(uint16_t, float) override {}
    void process(float*, float*, int, const ProcessContext&, const ModInputs&) override {}
    void reset() override {}
    void setProjectKey(int r, int s) override { root = r; scale = s; ++calls; }
};
}  // namespace

TEST_CASE("the project's key reaches every effect before it is prepared", "[mixengine][engine]") {
    registerEffect("probe", [] { return std::unique_ptr<EffectDevice>(new Probe()); });
    auto text = [](const std::string& meta) {
        return R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120)" + meta + R"(},"scenes":[{"id":"s"}],"tracks":[
          {"id":"t1","kind":"synth","inst":{"type":"poly","params":{}},"fx":[{"type":"probe","on":true,"params":{}}],"gain":0,"pan":0}],
          "masterFx":[{"type":"probe","on":true,"params":{}}],"clips":{}}})";
    };
    Probe::calls = 0;
    buildGraph(project::importFixtureJson(text("")), kSr, 1);                         // the defaults: A minor
    CHECK(Probe::calls == 2);                                                          // the track's and the master's
    CHECK(Probe::root == 9);
    CHECK(Probe::scale == 1);
    buildGraph(project::importFixtureJson(text(R"(,"root":3,"scale":"dorian")")), kSr, 1);
    CHECK(Probe::root == 3);
    CHECK(Probe::scale == 2);
    buildGraph(project::importFixtureJson(text(R"(,"root":-3,"scale":"nonsense")")), kSr, 1);   // out of range wraps; unknown falls back to major
    CHECK(Probe::root == 9);
    CHECK(Probe::scale == 0);
}

// ---- delay compensation of sends ----

namespace {
class LatencyFx final : public EffectDevice {   // a real delay of d samples that reports it, like a lookahead plugin
public:
    explicit LatencyFx(int d) : d_(d), buf_(size_t(d) + 1, 0.0f) {}
    std::span<const ParamSpec> params() const override { return {}; }
    void prepare(double, int) override {}
    void setParam(uint16_t, float) override {}
    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int i = 0; i < n; ++i) {
            buf_[pos_] = l[i]; bufR_.resize(buf_.size()); bufR_[pos_] = r[i];
            pos_ = (pos_ + 1) % buf_.size();
            l[i] = buf_[pos_]; r[i] = bufR_[pos_];
        }
    }
    void reset() override {}
    int latencySamples() const override { return d_; }
private:
    int d_;
    std::vector<float> buf_, bufR_;
    size_t pos_ = 0;
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

FxSlot lat(int d) {
    FxSlot f;
    f.dev = std::make_unique<LatencyFx>(d);
    f.out.prepare(kSr, 15.0f); f.out.snap(1.0f);
    return f;
}
// Fire every track once, render `n` frames in 128-frame chunks (the graph processes up to kMaxBlock at a time) and return the left channel.
std::vector<float> fire(Graph& g, int n = 512) {
    for (size_t t = 0; t < size_t(g.trackCount()); ++t) g.noteOn(uint16_t(t), 60, 1.0f, uint32_t(t + 1));
    std::vector<float> out, l(128), r(128);
    const ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    for (int at = 0; at < n; at += 128) { g.process(l.data(), r.data(), 128, ctx, nullptr, 1.0f); out.insert(out.end(), l.begin(), l.end()); }
    return out;
}
std::vector<size_t> hits(const std::vector<float>& x) { std::vector<size_t> h; for (size_t i = 0; i < x.size(); ++i) if (std::abs(x[i]) > 1e-6f) h.push_back(i); return h; }
}  // namespace

TEST_CASE("delay compensation: a send's wet path lines up with the dry path, whatever latency the send bus has", "[mixengine][engine][pdc]") {
    auto g = std::make_unique<Graph>(1, kSr);
    auto& a = g->addTrack();                       // a source straight to the master, with a full send to the bus
    a.inst = std::make_unique<ImpulseInst>();
    a.sendA.snap(1.0f);
    auto& bus = g->addTrack();                     // the send bus: a plugin with 80 samples of latency
    bus.isBus = true;
    bus.fx.push_back(lat(80));
    g->setSendBuses(1, -1);
    g->finalize();
    CHECK(g->latencySamples() == 80);              // the longest path into the master
    const auto x = fire(*g);
    const auto h = hits(x);
    REQUIRE(h.size() == 1);                        // dry and wet are one impulse: the dry was delayed to meet the wet
    CHECK(h[0] == 80);
    CHECK(x[80] == Approx(0.5f).margin(1e-5));     // 0.25 dry + 0.25 wet
}

TEST_CASE("delay compensation: sends from sources with different latency reach the bus aligned", "[mixengine][engine][pdc]") {
    auto g = std::make_unique<Graph>(1, kSr);
    auto& a = g->addTrack();                       // no latency
    a.inst = std::make_unique<ImpulseInst>();
    a.sendA.snap(1.0f);
    auto& b = g->addTrack();                       // 50 samples
    b.inst = std::make_unique<ImpulseInst>();
    b.fx.push_back(lat(50));
    b.sendA.snap(1.0f);
    auto& bus = g->addTrack();                     // a bus with no latency of its own
    bus.isBus = true;
    g->setSendBuses(2, -1);
    g->finalize();
    CHECK(g->latencySamples() == 50);
    const auto h = hits(fire(*g));
    REQUIRE(h.size() == 1);                        // the two dry paths and the two sends all arrive on the same sample
    CHECK(h[0] == 50);
}

TEST_CASE("delay compensation: a route into a bus and a send into another bus both end up aligned at the master", "[mixengine][engine][pdc]") {
    auto g = std::make_unique<Graph>(1, kSr);
    auto& a = g->addTrack();                       // routed through bus 1 (latency 30) and sending to bus 2 (latency 70)
    a.inst = std::make_unique<ImpulseInst>();
    a.outKind = TrackStrip::Out::Bus;
    a.outTarget = 1;
    a.sendA.snap(1.0f);
    auto& b1 = g->addTrack(); b1.isBus = true; b1.fx.push_back(lat(30));
    auto& b2 = g->addTrack(); b2.isBus = true; b2.fx.push_back(lat(70));
    g->setSendBuses(2, -1);
    g->finalize();
    CHECK(g->latencySamples() == 70);
    const auto h = hits(fire(*g));
    REQUIRE(h.size() == 1);
    CHECK(h[0] == 70);
}

TEST_CASE("delay compensation: a send at level 0 does not cost latency, and a built-in return counts", "[mixengine][engine][pdc]") {
    {
        auto g = std::make_unique<Graph>(1, kSr);
        auto& a = g->addTrack(); a.inst = std::make_unique<ImpulseInst>();   // sendA stays 0
        auto& bus = g->addTrack(); bus.isBus = true; bus.fx.push_back(lat(80));
        g->setSendBuses(1, -1);
        g->finalize();
        CHECK(g->latencySamples() == 0);
        const auto h = hits(fire(*g));
        REQUIRE(h.size() == 1);
        CHECK(h[0] == 0);
    }
    {   // the project's own return channels (no send buses): a return effect with 60 samples
        auto g = std::make_unique<Graph>(1, kSr);
        auto& a = g->addTrack(); a.inst = std::make_unique<ImpulseInst>(); a.sendA.snap(1.0f);
        auto& r = g->addReturn();
        r.dev = std::make_unique<LatencyFx>(60);
        g->finalize();
        CHECK(g->latencySamples() == 60);
        const auto h = hits(fire(*g));
        REQUIRE(h.size() == 1);
        CHECK(h[0] == 60);
    }
}

// ---- gain-reduction meters ----

TEST_CASE("gain-reduction meters: track and master dynamics devices publish their reduction, the multiband one per band", "[mixengine][engine][meters]") {
    // a loud saw through a compressor and a multiband compressor on the track, and a compressor on the master
    const std::string text = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[
      {"id":"t1","kind":"synth","inst":{"type":"poly","params":{"wave":0,"attack":0.002,"sustain":1.0,"cutoff":8000}},
       "fx":[{"type":"reverb","on":true,"params":{"mix":0}},{"type":"comp","on":true,"params":{"thresh":-40,"ratio":10}},{"type":"mbcomp","on":true,"params":{"mode":0}}],"gain":0,"pan":0}],
      "masterFx":[{"type":"comp","on":true,"params":{"thresh":-40,"ratio":10}}],"clips":{}}})";
    auto br = buildGraph(project::importFixtureJson(text), kSr, 1);
    REQUIRE(br.complete());
    MeterBank meters;
    br.graph->noteOn(0, 48, 1.0f, 1);
    std::vector<float> l(128), r(128);
    const ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    for (int b = 0; b < 400; ++b) br.graph->process(l.data(), r.data(), 128, ctx, &meters, 0.9f);
    CHECK(meters.fxGr(0, 0, 0) == 0.0f);                // the reverb has no meter
    CHECK(meters.fxGr(0, 1, 0) < -3.0f);                // the compressor (second in the chain) is working hard
    for (int band = 0; band < 3; ++band) CHECK(meters.fxGr(0, 2, band) <= 0.0f);
    CHECK(meters.fxGr(0, 2, 0) + meters.fxGr(0, 2, 1) + meters.fxGr(0, 2, 2) < -0.5f);   // the multiband one has bands that move
    CHECK(meters.fxGr(-1, 0, 0) < -1.0f);               // the master chain's compressor
    meters.clearFx();
    CHECK(meters.fxGr(0, 1, 0) == 0.0f);
    CHECK(meters.fxGr(-1, 0, 0) == 0.0f);
    CHECK(meters.fxGr(500, 0, 0) == 0.0f);              // out of range reads zero, never faults
}
