// A3 modulation: clip envelopes, arrangement lanes, track and master automation, LFOs (synced and
// free), macros and LFO-rate routing. Unit tests drive ModState::apply with a probe instrument that
// records the parameter values it receives; builder tests check the resolved keys end to end.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "dsp/Lfo.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "engine/Modulation.h"
#include "harness/Metrics.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;

const ParamSpec kProbeSpecs[] = {
    {0, "p0", 0.0f, 10.0f, 5.0f, Curve::Linear, 15.0f, false},
    {1, "p1", -1.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
};

struct ProbeLog { float v[2] = {5.0f, 0.0f}; int writes = 0; };

class Probe final : public InstrumentDevice {
public:
    explicit Probe(ProbeLog* l) : l_(l) {}
    std::span<const ParamSpec> params() const override { return kProbeSpecs; }
    void prepare(double, int) override {}
    void setParam(uint16_t i, float v) override { if (i < 2) { l_->v[i] = v; ++l_->writes; } }
    void noteOn(uint8_t, float, uint32_t) override {}
    void noteOff(uint32_t) override {}
    void process(float*, float*, int, const ProcessContext&, const ModInputs&) override {}
    void reset() override {}
private:
    ProbeLog* l_;
};

struct Rig {
    ProbeLog log;
    std::unique_ptr<Graph> g = std::make_unique<Graph>(1, kSr);
    std::unique_ptr<ModState> m = std::make_unique<ModState>();
    ModState::Route route(float min, float max, float base, uint16_t param = 0, bool logCurve = false) {
        ModState::Route r;
        r.kind = ModState::Kind::Param;
        r.addr = {0, kSlotInst, param};
        r.min = min; r.max = max; r.base = base; r.log = logCurve;
        return r;
    }
    Rig() {
        auto& t = g->addTrack();
        t.inst = std::make_unique<Probe>(&log);
        t.sched.session.resize(1);
        t.sched.session[0] = ClipPattern{96, {}};
        g->setTiming({}, 1, 4, 4);
        m->sampleRate = kSr;
        m->tracks.resize(1);
    }
    void finish() { m->finalize(); g->finalize(); }
    void apply(double now, TransportMode mode = TransportMode::Session, bool playing = true, int frames = 128) {
        m->apply(*g, now, playing, mode, frames);
    }
};

std::vector<ModPt> pts(std::initializer_list<std::pair<double, float>> l) {
    std::vector<ModPt> v;
    for (auto [t, x] : l) v.push_back({t, x});
    return v;
}

}  // namespace

TEST_CASE("envValueAt: interpolation, clamping and the sub-tick guard", "[modulation]") {
    const auto p = pts({{0, 0.2f}, {100, 1.0f}, {200, 0.0f}});
    CHECK(envValueAt(p, 25) == Approx(0.4f));       // 0.2 + 0.8 * 0.25
    CHECK(envValueAt(p, 100) == Approx(1.0f));
    CHECK(envValueAt(p, 150) == Approx(0.5f));
    CHECK(envValueAt(p, -5) == 0.2f);               // clamped before the first point
    CHECK(envValueAt(p, 500) == 0.0f);              // and after the last
    CHECK(envValueAt(pts({{50, 0.7f}}), 0) == 0.7f);
    CHECK(envValueAt(pts({{0, 0.0f}, {0.5, 1.0f}, {10, 1.0f}}), 0.25) == Approx(0.25f));   // segment shorter than a tick uses max(1)
}

TEST_CASE("lfoDivTicks follows schema.ts and falls back to one bar", "[modulation]") {
    CHECK(lfoDivTicks(0) == 3072.0);
    CHECK(lfoDivTicks(5) == 96.0);
    CHECK(lfoDivTicks(8) == 24.0);
    CHECK(lfoDivTicks(99) == 384.0);
    CHECK(lfoDivTicks(-1) == 384.0);
    CHECK(lfoDivTicks(5.9) == 96.0);                // truncated like the JS `|0`
}

TEST_CASE("session envelope: base from the lane, log curve, and snap back to base when it stops", "[modulation]") {
    Rig rig;
    rig.m->routes.push_back(rig.route(0.0f, 10.0f, 5.0f));
    rig.m->tracks[0].env.push_back({0, 0, 96, pts({{0, 0.0f}, {96, 1.0f}})});
    rig.finish();
    rig.g->sched(0).launch(0, 0);
    rig.apply(48.0);                                // halfway through the 96-tick loop
    CHECK(rig.log.v[0] == Approx(5.0f));            // norm 0.5 over 0..10
    rig.apply(72.0);
    CHECK(rig.log.v[0] == Approx(7.5f));
    rig.apply(96.0 + 24.0);                         // the lane loops over the clip length
    CHECK(rig.log.v[0] == Approx(2.5f));
    rig.apply(0.0, TransportMode::Session, /*playing=*/false);   // transport stopped: the mapping deactivates
    CHECK(rig.log.v[0] == Approx(5.0f));            // back to the stored base
    const int writes = rig.log.writes;
    rig.apply(0.0, TransportMode::Session, false);
    CHECK(rig.log.writes == writes);                // and nothing is rewritten while it stays inactive

    // a log-curve spec maps the norm along the exponential curve
    Rig lg;
    lg.m->routes.push_back(lg.route(40.0f, 8000.0f, 900.0f, 0, true));
    lg.m->tracks[0].env.push_back({0, 0, 96, pts({{0, 0.5f}})});
    lg.finish();
    lg.g->sched(0).launch(0, 0);
    lg.apply(10.0);
    CHECK(lg.log.v[0] == Approx(std::sqrt(40.0f * 8000.0f)).epsilon(1e-4));   // the geometric mean at norm 0.5
}

TEST_CASE("an unlaunched or different scene's envelope stays inactive", "[modulation]") {
    Rig rig;
    rig.m->routes.push_back(rig.route(0.0f, 10.0f, 5.0f));
    rig.m->tracks[0].env.push_back({0, 0, 96, pts({{0, 1.0f}})});
    rig.finish();
    rig.apply(10.0);                                // nothing launched
    CHECK(rig.log.writes == 0);
    rig.g->sched(0).launch(0, 0);
    rig.apply(10.0);
    CHECK(rig.log.v[0] == Approx(10.0f));
}

TEST_CASE("synced LFO: a bipolar offset on the base, clamped to the spec range", "[modulation]") {
    Rig rig;
    rig.m->routes.push_back(rig.route(0.0f, 10.0f, 5.0f));
    ModState::LfoState l;
    l.shape = 2; l.sync = true; l.divTicks = 96; l.depth = 1.0f; l.targets = {0};     // saw up, one cycle per quarter
    rig.m->tracks[0].lfos.push_back(l);
    rig.finish();
    rig.apply(24.0);                                // saw at a quarter cycle reads -0.5: 5 + (-0.5 * 1 * 5)
    CHECK(rig.log.v[0] == Approx(2.5f));
    rig.apply(48.0);                                // half cycle reads 0
    CHECK(rig.log.v[0] == Approx(5.0f));
    rig.apply(95.0);                                // near the top of the cycle: close to +1 -> about 10
    CHECK(rig.log.v[0] > 9.5f);
    rig.m->tracks[0].lfos[0].depth = 4.0f;          // way over: clamps at the edge
    rig.apply(95.0);
    CHECK(rig.log.v[0] == 10.0f);
}

TEST_CASE("free LFO integrates its rate per block; an off LFO does nothing but keeps time", "[modulation]") {
    Rig rig;
    rig.m->routes.push_back(rig.route(-1.0f, 1.0f, 0.0f, 1));
    ModState::LfoState l;
    l.shape = 0; l.sync = false; l.hz = 2.0; l.effHz = 2.0; l.depth = 1.0f; l.targets = {0};
    rig.m->tracks[0].lfos.push_back(l);
    rig.finish();
    // dt per block is capped at 0.1 s (as in the Rust loop); use 3000-frame blocks = 0.0625 s = 0.125 cycles at 2 Hz
    for (int i = 0; i < 2; ++i) rig.apply(0, TransportMode::Session, true, 3000);
    CHECK(rig.log.v[1] == Approx(1.0f).margin(1e-5));      // phase 0.25: the sine peak
    for (int i = 0; i < 4; ++i) rig.apply(0, TransportMode::Session, true, 3000);
    CHECK(rig.log.v[1] == Approx(-1.0f).margin(1e-5));     // phase 0.75: the trough
    rig.m->tracks[0].lfos[0].on = false;
    const int writes = rig.log.writes;
    for (int i = 0; i < 4; ++i) rig.apply(0, TransportMode::Session, true, 3000);
    CHECK(rig.m->tracks[0].lfos[0].freePhase > 1.2);       // an off LFO keeps integrating
    CHECK(rig.log.writes == writes + 1);                   // its mapping deactivated: one snap back to base
    CHECK(rig.log.v[1] == Approx(0.0f));
}

TEST_CASE("automation base plus LFO offset combine, then clamp", "[modulation]") {
    Rig rig;
    rig.m->routes.push_back(rig.route(0.0f, 10.0f, 5.0f));
    rig.m->tracks[0].env.push_back({0, 0, 96, pts({{0, 1.0f}})});          // automation at norm 1 -> 10
    ModState::LfoState l;
    l.shape = 4; l.sync = true; l.divTicks = 96; l.depth = 0.2f; l.targets = {0};   // square: +1 in the first half
    rig.m->tracks[0].lfos.push_back(l);
    rig.finish();
    rig.g->sched(0).launch(0, 0);
    rig.apply(10.0);                                // base 10 + (+1 * 0.2 * 5) = 11 -> clamps to 10
    CHECK(rig.log.v[0] == 10.0f);
    rig.apply(60.0);                                // second half of the square: 10 - 1 = 9
    CHECK(rig.log.v[0] == Approx(9.0f));
}

TEST_CASE("macro: one 0..1 value fans out linearly to every sub-target", "[modulation]") {
    Rig rig;
    ModState::Route r;
    r.kind = ModState::Kind::Macro;
    r.subStart = 0; r.subLen = 2;
    r.min = 0; r.max = 1; r.base = 0.5f;
    rig.m->routes.push_back(r);
    rig.m->subs.push_back({{0, kSlotInst, 0}, 0.0f, 10.0f});
    rig.m->subs.push_back({{0, kSlotInst, 1}, -1.0f, 1.0f});
    rig.m->tracks[0].env.push_back({0, 0, 96, pts({{0, 0.25f}})});
    rig.finish();
    rig.g->sched(0).launch(0, 0);
    rig.apply(5.0);
    CHECK(rig.log.v[0] == Approx(2.5f));            // 0 + 10 * 0.25
    CHECK(rig.log.v[1] == Approx(-0.5f));           // -1 + 2 * 0.25
}

TEST_CASE("an LFO can modulate another LFO's rate (applied the next block)", "[modulation]") {
    Rig rig;
    rig.m->routes.push_back(rig.route(-1.0f, 1.0f, 0.0f, 1));              // route 0: the audible target
    ModState::Route rateRoute;
    rateRoute.kind = ModState::Kind::LfoRate;
    rateRoute.lfoTrack = 0; rateRoute.lfoIdx = 0;
    rateRoute.min = 0.01f; rateRoute.max = 30.0f; rateRoute.log = true; rateRoute.base = 1.0f;
    rig.m->routes.push_back(rateRoute);                                      // route 1: LFO 0's rate
    ModState::LfoState a;                                                    // LFO 0: free, 1 Hz, drives the audible parameter
    a.shape = 0; a.hz = 1.0; a.effHz = 1.0; a.depth = 1.0f; a.targets = {0};
    rig.m->tracks[0].lfos.push_back(a);
    rig.m->tracks[0].env.push_back({1, 0, 96, pts({{0, 1.0f}})});            // automation drives the rate route to its max (30 Hz)
    rig.finish();
    rig.g->sched(0).launch(0, 0);
    rig.apply(0, TransportMode::Session, true, 1);                           // block 1: the rate override is computed...
    CHECK(rig.m->tracks[0].lfos[0].freePhase == Approx(1.0 / kSr).epsilon(1e-6));   // ...but used at the stored 1 Hz
    rig.apply(0, TransportMode::Session, true, 48000);                       // block 2 integrates at the 30 Hz override
    CHECK(rig.m->tracks[0].lfos[0].freePhase == Approx(30.0 * 1.0 / 1.0 * 0.1).margin(0.01));   // dt is capped at 0.1 s
    CHECK(rig.m->tracks[0].lfos[0].effHz == Approx(30.0));
}

TEST_CASE("arrangement mode: lanes use absolute ticks, clip envelopes under the playhead override them", "[modulation]") {
    Rig rig;
    rig.m->routes.push_back(rig.route(0.0f, 10.0f, 5.0f));
    rig.m->tracks[0].autoLanes.push_back({0, pts({{0, 0.0f}, {1000, 1.0f}})});
    rig.m->tracks[0].arrEnv.push_back({0, 400, 100, 100, pts({{0, 1.0f}})});     // a clip at ticks 400..500 holding norm 1
    rig.finish();
    rig.apply(200.0, TransportMode::Arrangement);
    CHECK(rig.log.v[0] == Approx(2.0f));            // the lane: 200/1000 -> norm 0.2
    rig.apply(450.0, TransportMode::Arrangement);
    CHECK(rig.log.v[0] == Approx(10.0f));           // inside the clip: its envelope overrides the lane
    rig.apply(600.0, TransportMode::Arrangement);
    CHECK(rig.log.v[0] == Approx(6.0f));            // past the clip: the lane again
    rig.apply(0.0, TransportMode::Session, true);   // session mode ignores arrangement lanes (no launched clip, so inactive)
    CHECK(rig.log.v[0] == Approx(5.0f));            // snapped back to base
}

TEST_CASE("builder: clip envelope on the instrument cutoff changes the sound over the loop", "[modulation][builder]") {
    // mono saw, cutoff swept from the bottom to the top of its (log) range over a two-bar clip
    const std::string json = R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
      "tracks":[{"id":"t1","kind":"synth","inst":{"type":"mono","params":{"envAmt":0,"res":0,"sustain":1,"attack":0.001}},"fx":[],"gain":-18,"pan":0}],
      "clips":{"t1|s":{"len":768,"notes":{"n":{"p":57,"s":0,"d":760,"v":1.0}},"env":{"inst||cutoff":[{"t":0,"v":0.0},{"t":768,"v":1.0}]}}}}})";
    const auto fx = project::importFixtureJson(json);
    auto built = buildGraph(fx, kSr, 1);
    CHECK(built.unsupported.empty());
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(std::move(built.graph));
    Cmd c; c.type = CmdType::ClipLaunch; c.epoch = 1; c.clipLaunch = {0, 0}; e.commands().push(c);
    Cmd p; p.type = CmdType::TransportPlay; p.transportPlay = {0, 0.0}; e.commands().push(p);
    std::vector<float> l(size_t(3.5 * kSr)), r(l.size());
    for (size_t pos = 0; pos < l.size(); pos += 128) e.process(l.data() + pos, r.data() + pos, int(std::min<size_t>(128, l.size() - pos)));
    // brightness proxy: energy of the first difference relative to the signal energy
    auto brightness = [&](double a, double b) {
        const size_t s0 = size_t(a * kSr), n = size_t((b - a) * kSr);
        double d = 0, x = 0;
        for (size_t i = s0 + 1; i < s0 + n; ++i) { d += double(l[i] - l[i - 1]) * (l[i] - l[i - 1]); x += double(l[i]) * l[i]; }
        return d / std::max(x, 1e-12);
    };
    const double early = brightness(0.1, 0.4), mid = brightness(1.8, 2.1), late = brightness(3.2, 3.5);
    CHECK(late > 3.0 * early);                      // the filter opens over the clip
    CHECK(mid > early);
    CHECK(late > mid);
}

TEST_CASE("builder: unresolvable keys are ignored; dest midi is reported", "[modulation][builder]") {
    const std::string json = R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
      "tracks":[{"id":"t1","kind":"synth","inst":{"type":"mono","params":{}},"fx":[],"gain":0,"pan":0,
        "midifx":[{"id":"m1","type":"arp","on":true,"params":{}}],
        "lfos":[{"id":"l1","on":true,"shape":0,"sync":false,"rate":5,"hz":1,"depth":0.5,"phase":0,"dest":"midi","fxId":"m1","pkey":"rate"},
                {"id":"l2","on":true,"shape":0,"sync":false,"rate":5,"hz":1,"depth":0.5,"phase":0,"targets":[{"dest":"mix","fxId":"","pkey":"pan"},{"dest":"inst","fxId":"","pkey":"nosuchparam"}]}]}],
      "clips":{"t1|s":{"len":96,"notes":{},"env":{"inst||nosuch":[{"t":0,"v":0.5}],"fx|nofx|x":[{"t":0,"v":0.5}]}}}}})";
    const auto fx = project::importFixtureJson(json);
    auto built = buildGraph(fx, kSr, 1);
    const auto& u = built.unsupported;
    CHECK(std::find(u.begin(), u.end(), "feature: modulating MIDI-fx parameters (dest midi)") != u.end());
    CHECK(built.graph != nullptr);                  // the bad keys neither throw nor poison the build
}

// ---------------------------------------------------------------------------------------------------------------
// B-track follow-up: macro knobs and LFO fields are live parameters (no rebuild), and a macro works without automation.

#include "document/Session.h"
#include "engine/GraphService.h"

namespace {
const char* kMacroProj = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
  "tracks":[{"id":"t1","kind":"audio","fx":[{"type":"stubgain","id":"g","on":true,"params":{"gain":1.0}}],"gain":0,"pan":0,
             "macros":[{"name":"Vol","value":0.5,"targets":[{"dest":"fx","fxId":"g","pkey":"gain"}]}],
             "lfos":[{"id":"l1","on":true,"shape":0,"sync":false,"hz":2,"depth":0.0,"phase":0,"targets":[{"dest":"mix","fxId":"","pkey":"pan"}]}]}],
  "clips":{}}})";
}

TEST_CASE("macros: the knob applies on its own, changes live through its key, and an LFO field is live too", "[modulation][macro]") {
    using namespace ddaw;
    const auto fx = project::importFixtureJson(kMacroProj);
    auto built = engine::buildGraph(fx, 48000.0, 1);
    ParamAddr a;
    REQUIRE(built.resolver.resolve("t1|macro|0", a));
    CHECK(a.slot == kSlotMacro);
    REQUIRE(built.resolver.resolve("t1|lfo0|depth", a));
    CHECK(a.slot == kSlotLfo);
    CHECK(built.resolver.resolve("t1|lfo0|hz", a));
    CHECK_FALSE(built.resolver.resolve("t1|macro|3", a));

    engine::Engine e;
    e.prepare(48000.0, engine::MasterLimiterConfig{engine::MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(std::move(built.graph));
    Cmd mon; mon.type = CmdType::MonitorInput; mon.monitorInput = {0, 1}; mon.epoch = 1; e.commands().push(mon);
    std::vector<float> in(48000), out(48000), r(64u);
    for (size_t i = 0; i < in.size(); ++i) in[i] = 0.4f * float(std::sin(0.05 * double(i)));
    auto peakAfterRun = [&](size_t from) {
        for (size_t i = 0; i + 64 <= in.size(); i += 64) e.processIO(&in[i], &in[i], &out[i], r.data(), 64);
        double m = 0; for (size_t i = from; i < out.size(); ++i) m = std::max(m, double(std::abs(out[i]))); return m;
    };
    // macro 0.5 over the gain's 0..2 range -> gain 1.0 -> peak 0.4
    CHECK(peakAfterRun(30000) == Catch::Approx(0.4).epsilon(0.03));
    // turn the knob live: SetParam on the macro key -> gain 2 * 0.25 = 0.5
    ParamAddr macro;
    REQUIRE(fx.project.tracks.size() == 1);
    {
        engine::BuildResult again = engine::buildGraph(fx, 48000.0, 1);
        REQUIRE(again.resolver.resolve("t1|macro|0", macro));
    }
    Cmd c; c.type = CmdType::SetParam; c.epoch = 1; c.setParam = {macro, 0.25f}; e.commands().push(c);
    CHECK(peakAfterRun(30000) == Catch::Approx(0.4 * 0.5).epsilon(0.05));
    {   // depth 0: a steady level
        double lo = 1e9, hi = 0;
        for (size_t b = 24000; b + 1200 <= out.size(); b += 1200) { double m = 0; for (size_t i = b; i < b + 1200; ++i) m = std::max(m, double(std::abs(out[i]))); lo = std::min(lo, m); hi = std::max(hi, m); }
        CHECK(lo / hi > 0.9);
    }
    // an LFO field live: depth 1.0 at 2 Hz sweeps the pan across its range, so the left channel's level swings
    ParamAddr depth;
    {
        engine::BuildResult again = engine::buildGraph(fx, 48000.0, 1);
        REQUIRE(again.resolver.resolve("t1|lfo0|depth", depth));
    }
    Cmd d; d.type = CmdType::SetParam; d.epoch = 1; d.setParam = {depth, 1.0f}; e.commands().push(d);
    auto swing = [&] {   // min / max of the block peaks over the last 0.5 s
        for (size_t i = 0; i + 64 <= in.size(); i += 64) e.processIO(&in[i], &in[i], &out[i], r.data(), 64);
        double lo = 1e9, hi = 0;
        for (size_t b = 24000; b + 1200 <= out.size(); b += 1200) { double m = 0; for (size_t i = b; i < b + 1200; ++i) m = std::max(m, double(std::abs(out[i]))); lo = std::min(lo, m); hi = std::max(hi, m); }
        return lo / hi;
    };
    CHECK(swing() < 0.6);
}

TEST_CASE("document: macro.value and lfo.field are live edits with exact inverses", "[modulation][macro][document]") {
    using namespace ddaw;
    document::Document doc(project::importFixtureJson(kMacroProj).project);
    const auto uid = doc.project().tracks[0].uid;
    auto info = doc.apply({"macro.value", {{"track", uid}, {"index", 0}, {"value", 0.8}}});
    CHECK_FALSE(info.structural);
    REQUIRE(info.params.size() == 1);
    CHECK(info.params[0].key == "t1|macro|0");
    CHECK(info.params[0].value == Catch::Approx(0.8));
    CHECK(doc.project().tracks[0].macros[0].value == Catch::Approx(0.8));
    info = doc.apply({"lfo.field", {{"track", uid}, {"index", 0}, {"field", "hz"}, {"value", 5.0}}});
    CHECK_FALSE(info.structural);
    CHECK(info.params[0].key == "t1|lfo0|hz");
    CHECK(doc.project().tracks[0].lfos[0].hz == 5.0);
    CHECK_THROWS(doc.apply({"lfo.field", {{"track", uid}, {"index", 0}, {"field", "shape"}, {"value", 1.0}}}));
    CHECK_THROWS(doc.apply({"macro.value", {{"track", uid}, {"index", 4}, {"value", 0.1}}}));
    doc.undo();
    CHECK(doc.project().tracks[0].lfos[0].hz == 2.0);
    doc.undo();
    CHECK(doc.project().tracks[0].macros[0].value == Catch::Approx(0.5));
}
