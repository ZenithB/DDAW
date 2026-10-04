// A3 bus network, sends, returns, master chain and sidechain ducking, end to end through the
// builder and the Engine. Levels are measured with the master limiter bypassed.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "harness/Metrics.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;

// A held 440 Hz tone (stubtone, level 0.1) on every listed track; extra JSON fields per track.
std::string proj(const std::string& tracks, const std::string& extra = "", const std::string& clipTracks = "t1") {
    std::string clips;
    size_t from = 0;
    while (from <= clipTracks.size()) {
        const size_t comma = clipTracks.find(',', from);
        const std::string id = clipTracks.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
        if (!id.empty())
            clips += std::string(clips.empty() ? "" : ",") + "\"" + id + "|s\":{\"len\":1536,\"notes\":{\"n\":{\"p\":69,\"s\":0,\"d\":1500,\"v\":1.0}}}";
        if (comma == std::string::npos) break;
        from = comma + 1;
    }
    return R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[)" + tracks +
           R"(],"clips":{)" + clips + "}" + extra + "}}";
}

std::string synth(const std::string& id, const std::string& more = "") {
    return R"({"id":")" + id + R"(","kind":"synth","inst":{"type":"stubtone","params":{"level":0.1}},"fx":[],"gain":0,"pan":0)" + more + "}";
}

struct Rendered { std::vector<float> l, r; int latency = 0; std::vector<std::string> unsupported; };

// Render `secs` through the real path (launch the scene on every track, then play), limiter bypassed.
Rendered render(const std::string& json, double secs = 1.0, int tracks = 1) {
    const auto fx = project::importFixtureJson(json);
    auto built = buildGraph(fx, kSr, 1);
    Rendered out;
    out.unsupported = built.unsupported;
    Engine e;
    e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    out.latency = built.graph->latencySamples();
    e.setInitialGraph(std::move(built.graph));
    const int n = static_cast<int>(fx.project.tracks.size());
    (void)tracks;
    for (int t = 0; t < n; ++t) {
        Cmd c; c.type = CmdType::ClipLaunch; c.epoch = 1; c.clipLaunch = {uint16_t(t), 0};
        e.commands().push(c);
    }
    Cmd p; p.type = CmdType::TransportPlay; p.transportPlay = {0, 0.0};
    e.commands().push(p);
    const size_t total = static_cast<size_t>(secs * kSr);
    out.l.resize(total); out.r.resize(total);
    for (size_t pos = 0; pos < total; pos += 128) e.process(out.l.data() + pos, out.r.data() + pos, int(std::min<size_t>(128, total - pos)));
    return out;
}

double steadyRms(const std::vector<float>& v) { return harness::rms(std::span<const float>(v).subspan(v.size() / 2)); }

}  // namespace

TEST_CASE("an A send bus carries a post-fader copy of the track", "[buses]") {
    const double direct = steadyRms(render(proj(synth("t1"))).l);
    REQUIRE(direct > 0.01);
    // bus A: stubgain 0.5, sendA 0.5 -> master = direct * (1 + 0.5 * 0.5)
    const auto r = render(proj(synth("t1", R"(,"sendA":0.5)") + "," +
        R"({"id":"A","kind":"bus","send":"A","inst":{"type":"audiobus","params":{}},"fx":[{"type":"stubgain","on":true,"params":{"gain":0.5}}],"gain":0,"pan":0})"));
    CHECK(r.unsupported.empty());
    CHECK(steadyRms(r.l) / direct == Approx(1.25).epsilon(0.01));
}

TEST_CASE("legacy return effects take sends A and B when the project has no send buses", "[buses]") {
    const double direct = steadyRms(render(proj(synth("t1"))).l);
    const std::string returns = R"(,"returns":[{"name":"ra","fxType":"stubgain","params":{"gain":0.5},"gain":0},
                                              {"name":"rb","fxType":"stubgain","params":{"gain":1.0},"gain":0}])";
    // sendA 0.5 into return 0 (x0.5): +0.25 ; sendB 0.4 into return 1 (x1.0): +0.4
    CHECK(steadyRms(render(proj(synth("t1", R"(,"sendA":0.5)"), returns)).l) / direct == Approx(1.25).epsilon(0.01));
    CHECK(steadyRms(render(proj(synth("t1", R"(,"sendB":0.4)"), returns)).l) / direct == Approx(1.4).epsilon(0.01));
    CHECK(steadyRms(render(proj(synth("t1", R"(,"sendA":0.5,"sendB":0.4)"), returns)).l) / direct == Approx(1.65).epsilon(0.01));
}

TEST_CASE("per-bus sends and bus-to-bus routing, whatever the document order", "[buses]") {
    const double direct = steadyRms(render(proj(synth("t1"))).l);
    const std::string b1 = R"({"id":"b1","kind":"bus","output":"b2","inst":{"type":"audiobus","params":{}},"fx":[],"gain":0,"pan":0})";
    const std::string b2 = R"({"id":"b2","kind":"bus","inst":{"type":"audiobus","params":{}},"fx":[{"type":"stubgain","on":true,"params":{"gain":0.5}}],"gain":0,"pan":0})";
    // t1 -> b1 (send 0.8) -> b2 (x0.5) -> master : 1 + 0.8*0.5 = 1.4
    const std::string t1 = synth("t1", R"(,"sends":{"b1":0.8})");
    const double inOrder = steadyRms(render(proj(t1 + "," + b1 + "," + b2)).l) / direct;
    const double reversed = steadyRms(render(proj(t1 + "," + b2 + "," + b1)).l) / direct;   // b2 declared before b1
    CHECK(inOrder == Approx(1.4).epsilon(0.01));
    CHECK(reversed == Approx(inOrder).epsilon(1e-4));      // topological order, not document order
}

TEST_CASE("a bus-to-bus cycle closes through a delay and stays stable", "[buses]") {
    const std::string b1 = R"({"id":"b1","kind":"bus","output":"b2","inst":{"type":"audiobus","params":{}},"fx":[],"gain":-6,"pan":0})";
    const std::string b2 = R"({"id":"b2","kind":"bus","output":"b1","inst":{"type":"audiobus","params":{}},"fx":[],"gain":-6,"pan":0})";
    const auto r = render(proj(synth("t1", R"(,"sends":{"b1":1.0})") + "," + b1 + "," + b2), 3.0);
    CHECK(harness::allFinite(r.l));
    float mx = 0; for (float v : r.l) mx = std::max(mx, std::abs(v));
    CHECK(mx < 1.0f);                                      // -12 dB around the loop: bounded
    CHECK(steadyRms(r.l) > 0.01);
}

TEST_CASE("the feedback bus echoes after the source stops, and decays", "[buses]") {
    // one 0.25 s note, then silence; the F bus sends to itself at 0.5
    std::string t1 = R"({"id":"t1","kind":"synth","inst":{"type":"stubtone","params":{"level":0.1}},"fx":[],"gain":0,"pan":0,"sends":{"F":1.0}})";
    const std::string fb = R"({"id":"F","kind":"bus","send":"F","sends":{"F":0.5},"inst":{"type":"audiobus","params":{}},"fx":[],"gain":0,"pan":0})";
    std::string json = R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[)" + t1 + "," + fb +
        R"(],"clips":{"t1|s":{"len":1536,"notes":{"n":{"p":69,"s":0,"d":48,"v":1.0}}}}}})";
    const auto r = render(json, 2.5);
    auto window = [&](double a, double b) { return harness::rms(std::span<const float>(r.l).subspan(size_t(a * kSr), size_t((b - a) * kSr))); };
    const double during = window(0.05, 0.2);
    const double echo1 = window(0.35, 0.45);               // after the 0.25 s note: the first lap (0.09 s) and following
    const double late = window(2.0, 2.4);
    CHECK(during > 0.01);
    CHECK(echo1 > 0.005);                                   // the feedback loop keeps ringing
    CHECK(late < echo1);                                    // and decays (loop gain 0.5)
    CHECK(harness::allFinite(r.l));
}

TEST_CASE("master gain and master effects apply to the whole mix", "[buses]") {
    const double direct = steadyRms(render(proj(synth("t1"))).l);
    const auto fxr = render(proj(synth("t1"), R"(,"masterFx":[{"type":"stubgain","on":true,"params":{"gain":0.5}}])"));
    CHECK(steadyRms(fxr.l) / direct == Approx(0.5).epsilon(0.01));
    const auto outr = render(proj(synth("t1"), R"(,"masterFx":[{"type":"stubgain","on":true,"params":{"gain":1.0},"out":-6.0206}])"));
    CHECK(steadyRms(outr.l) / direct == Approx(0.5).epsilon(0.01));   // the device `out` gain
    // an effect with lookahead on the master chain adds to the graph latency
    const auto lat = render(proj(synth("t1"), R"(,"masterFx":[{"type":"opto","on":true,"params":{}}])"), 0.1);
    CHECK(lat.latency == static_cast<int>(0.006f * 48000.0f));
}

TEST_CASE("instrument and effect `out` gains and mixer sends are addressable", "[buses]") {
    auto b = buildGraph(project::importFixtureJson(proj(synth("t1") + "," +
        R"({"id":"A","kind":"bus","send":"A","inst":{"type":"audiobus","params":{}},"fx":[],"gain":0,"pan":0})")), kSr, 1);
    ParamAddr a{};
    CHECK(b.resolver.resolve("t1|mix|sendA", a));
    CHECK(a.param == kMixSendA);
    CHECK(b.resolver.resolve("t1|mix|sendB", a));
    CHECK(b.resolver.resolve("t1|inst|out", a));
    CHECK(a.param == kParamOut);
    CHECK(b.resolver.resolve("t1|inst|level", a));
    uint16_t idx = 99;
    CHECK(b.resolver.track("A", idx));
    CHECK(idx == 1);
    CHECK(b.resolver.scene("s", idx));
    CHECK(idx == 0);
    CHECK_FALSE(b.resolver.track("nope", idx));
}

TEST_CASE("sidechain duck: a note on the source track dips the target", "[buses][duck]") {
    // t2 plays continuously through a duck keyed to t1; t1 fires one short note at 0.5 s
    const std::string t2 = R"({"id":"t2","kind":"synth","inst":{"type":"stubtone","params":{"level":0.1}},
        "fx":[{"type":"duck","on":true,"srcTrack":"t1","params":{"rate":1,"amount":1.0,"curve":0.5}}],"gain":0,"pan":0})";
    const std::string t1 = R"({"id":"t1","kind":"synth","inst":{"type":"stubtone","params":{"level":0.0}},"fx":[],"gain":0,"pan":0})";
    std::string json = R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[)" + t1 + "," + t2 +
        R"(],"clips":{"t2|s":{"len":1536,"notes":{"n":{"p":69,"s":0,"d":1500,"v":1.0}}},
                      "t1|s":{"len":1536,"notes":{"n":{"p":60,"s":96,"d":24,"v":1.0}}}}}})";   // t1's note fires at 0.5 s
    const auto r = render(json, 2.0);
    auto window = [&](double a, double b) { return harness::rms(std::span<const float>(r.l).subspan(size_t(a * kSr), size_t((b - a) * kSr))); };
    const double before = window(0.2, 0.4);
    const double dip = window(0.5, 0.55);                   // right after the trigger: the duck is closed
    const double recovered = window(1.3, 1.5);
    CHECK(before > 0.03);
    CHECK(dip < 0.5 * before);
    CHECK(recovered > 0.8 * before);
}

TEST_CASE("a duck without a source track stays in tempo mode", "[buses][duck]") {
    const std::string t = R"({"id":"t1","kind":"synth","inst":{"type":"stubtone","params":{"level":0.1}},
        "fx":[{"type":"duck","on":true,"params":{"rate":1,"amount":1.0,"curve":0.5}}],"gain":0,"pan":0})";
    const auto r = render(proj(t), 2.0);
    // a quarter-note pump at 120 bpm dips twice a second: the level swings
    float lo = 1e9f, hi = 0;
    for (size_t pos = 0; pos + 2400 < r.l.size(); pos += 2400) {
        const double w = harness::rms(std::span<const float>(r.l).subspan(pos, 2400));
        lo = std::min<float>(lo, float(w)); hi = std::max<float>(hi, float(w));
    }
    CHECK(hi > 3.0f * lo);
}
