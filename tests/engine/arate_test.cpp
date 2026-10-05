// Audio-rate modulation (B4): oscillator and track sources drive the A-rate ports of a device, tracks render
// sources first whatever their order in the document, loops and bad targets are reported and dropped, live
// fields land without a rebuild, and none of it allocates on the audio thread.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <string>

#include "../AllocGuard.h"
#include "dsp/Fft.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;

// An fmop carrier alone (additive wiring, only operator 1 audible) as track "t1", plus the extra tracks / routes given.
std::string projectOf(const std::string& tracks) {
    return R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[)" + tracks + R"(],"clips":{}}})";
}
const std::string kCarrier = R"("inst":{"type":"fmop","params":{"algo":4,"l1":1,"l2":0,"l3":0,"l4":0,"attack":0.001,"sustain":1.0,"release":0.05}})";
std::string carrierTrack(const std::string& routes) {
    return R"({"id":"t1","kind":"synth",)" + kCarrier + R"(,"fx":[],"gain":0,"pan":0)" + (routes.empty() ? "" : R"(,"arate":[)" + routes + "]") + "}";
}
std::string polyTrack(const std::string& id, const std::string& extra = "") {
    return R"({"id":")" + id + R"(","kind":"synth","inst":{"type":"poly","params":{"wave":3,"attack":0.005,"sustain":1.0,"release":0.05}},"fx":[],"gain":0,"pan":0)" + extra + "}";
}
const char* kAm = R"({"source":"osc","shape":0,"hz":55,"depth":1.0,"target":{"dest":"inst","fxId":"","pkey":"amp"}})";

struct Run {
    BuildResult br;
    std::vector<float> l = std::vector<float>(128u), r = std::vector<float>(128u);
    explicit Run(const std::string& text) : br(buildGraph(project::importFixtureJson(text), kSr, 1)) {}
    Graph& g() { return *br.graph; }
    std::vector<float> render(int blocks) {
        const ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) {
            g().process(l.data(), r.data(), 128, ctx, nullptr, 1.0f);
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    }
};

// magnitude of the spectrum near `hz` over the last 16384 samples
double mag(const std::vector<float>& x, double hz) {
    constexpr size_t N = 16384;
    dsp::Fft f; f.prepare(int(N));
    std::vector<double> re(N), im(N, 0.0);
    for (size_t i = 0; i < N; ++i) re[i] = double(x[x.size() - N + i]) * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / N));
    f.forward(re.data(), im.data());
    const int k0 = int(std::lround(hz / kSr * N));
    double m = 0;
    for (int k = k0 - 2; k <= k0 + 2; ++k) m = std::max(m, std::hypot(re[size_t(k)], im[size_t(k)]));
    return m;
}
double rms(const std::vector<float>& x, size_t from) {
    double s = 0;
    for (size_t i = from; i < x.size(); ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(x.size() - from));
}
}  // namespace

TEST_CASE("audio-rate route: an oscillator amplitude-modulates the amp port, to the sideband", "[arate][engine]") {
    Run run(projectOf(carrierTrack(kAm)));
    REQUIRE(run.br.complete());
    run.g().noteOn(0, 69, 1.0f, 1);
    const auto x = run.render(300);
    // amp = 1 + 0.5 sin(2 pi 55 t): sidebands at 440 +- 55, each a quarter of the carrier
    const double c = mag(x, 440.0);
    CHECK(c > 1.0);
    CHECK(mag(x, 440.0 + 55.0) / c == Approx(0.25).margin(0.03));
    CHECK(mag(x, 440.0 - 55.0) / c == Approx(0.25).margin(0.03));
    // the same project without the route has no sidebands
    Run plain(projectOf(carrierTrack("")));
    plain.g().noteOn(0, 69, 1.0f, 1);
    const auto y = plain.render(300);
    CHECK(mag(y, 440.0 + 55.0) / mag(y, 440.0) < 0.01);
}

TEST_CASE("audio-rate route: a route to a parameter that is not audio-rate is reported and ignored", "[arate][engine]") {
    Run run(projectOf(carrierTrack(R"({"source":"osc","hz":55,"depth":1.0,"target":{"dest":"inst","fxId":"","pkey":"r1"}},
                                    {"source":"osc","hz":55,"depth":1.0,"target":{"dest":"inst","fxId":"","pkey":"nope"}},
                                    {"source":"osc","hz":55,"depth":1.0,"target":{"dest":"fx9","fxId":"fx9","pkey":"x"}})")));
    REQUIRE(run.br.unsupported.size() == 3);
    bool notARate = false;
    for (auto& s : run.br.unsupported) notARate |= s.find("does not accept audio-rate modulation") != std::string::npos;
    CHECK(notARate);
    run.g().noteOn(0, 69, 1.0f, 1);
    const auto x = run.render(300);
    CHECK(mag(x, 440.0 + 55.0) / mag(x, 440.0) < 0.01);
}

TEST_CASE("audio-rate route: another track's audio is a source, and the source renders first in either order", "[arate][engine]") {
    const std::string route = R"({"source":"track","track":"src","depth":1.0,"target":{"dest":"inst","fxId":"","pkey":"amp"}})";
    // document order: the target first, the source second - the builder must reorder
    Run late(projectOf(carrierTrack(route) + "," + polyTrack("src")));
    Run early(projectOf(polyTrack("src") + "," + carrierTrack(route)));
    REQUIRE(late.br.complete());
    REQUIRE(early.br.complete());
    late.g().noteOn(0, 69, 1.0f, 1);  late.g().noteOn(1, 45, 0.8f, 2);       // 110 Hz sine on the source
    early.g().noteOn(1, 69, 1.0f, 1); early.g().noteOn(0, 45, 0.8f, 2);
    const auto a = late.render(300), b = early.render(300);
    const double c = mag(a, 440.0);
    CHECK(mag(a, 440.0 + 110.0) / c > 0.02);          // modulated: sidebands at the source's frequency
    CHECK(mag(a, 440.0 - 110.0) / c > 0.02);
    // and the carrier track is the same sound in both orders (it is only the order of rendering that differs)
    REQUIRE(a.size() == b.size());
    // the source track also sounds in the mix, so compare the modulation, not the mix: the sidebands agree
    CHECK(mag(a, 550.0) == Approx(mag(b, 550.0)).epsilon(1e-3));
    CHECK(mag(a, 330.0) == Approx(mag(b, 330.0)).epsilon(1e-3));
}

TEST_CASE("audio-rate route: an envelope follower on another track ducks the amp port", "[arate][engine]") {
    const std::string route = R"({"source":"track","track":"src","follow":true,"attackMs":2,"releaseMs":50,"depth":-1.0,"target":{"dest":"inst","fxId":"","pkey":"amp"}})";
    Run run(projectOf(carrierTrack(route) + "," + polyTrack("src")));
    REQUIRE(run.br.complete());
    run.g().noteOn(0, 69, 1.0f, 1);
    const auto quiet = run.render(200);              // source silent: amp stays 1
    run.g().noteOn(1, 57, 1.0f, 2);                  // source plays: the follower rises, amp falls
    const auto ducked = run.render(200);
    // the mix includes the source's own sine, so measure the carrier by its bin
    CHECK(mag(ducked, 440.0) < 0.9 * mag(quiet, 440.0));
}

TEST_CASE("audio-rate route: a loop between two tracks is rejected, the rest of the project builds", "[arate][engine]") {
    const std::string a = R"({"source":"track","track":"b","depth":1.0,"target":{"dest":"inst","fxId":"","pkey":"amp"}})";
    const std::string b = R"({"source":"track","track":"t1","depth":1.0,"target":{"dest":"inst","fxId":"","pkey":"amp"}})";
    const std::string tb = R"({"id":"b","kind":"synth",)" + kCarrier + R"(,"fx":[],"gain":0,"pan":0,"arate":[)" + b + "]}";
    Run run(projectOf(carrierTrack(a) + "," + tb));
    REQUIRE(run.br.unsupported.size() == 1);
    CHECK(run.br.unsupported[0].find("modulation loop") != std::string::npos);
    run.g().noteOn(0, 69, 1.0f, 1);
    CHECK(rms(run.render(50), 2000) > 0.01);
}

TEST_CASE("audio-rate route: depth and rate are live fields, no rebuild", "[arate][engine]") {
    Run run(projectOf(carrierTrack(kAm)));
    run.g().noteOn(0, 69, 1.0f, 1);
    const auto before = run.render(300);
    CHECK(mag(before, 440.0 + 55.0) / mag(before, 440.0) == Approx(0.25).margin(0.03));
    ParamAddr depth{}, hz{};
    REQUIRE(run.br.resolver.resolve("t1|arate0|depth", depth));
    REQUIRE(run.br.resolver.resolve("t1|arate0|hz", hz));
    run.g().setParam(hz, 100.0f);
    const auto moved = run.render(300);
    CHECK(mag(moved, 440.0 + 100.0) / mag(moved, 440.0) == Approx(0.25).margin(0.03));
    run.g().setParam(depth, 0.0f);
    const auto off = run.render(300);
    CHECK(mag(off, 440.0 + 100.0) / mag(off, 440.0) < 0.01);
}

TEST_CASE("audio-rate route: rendering allocates nothing", "[arate][engine][realtime]") {
    const std::string route = R"({"source":"track","track":"src","follow":true,"depth":0.5,"target":{"dest":"inst","fxId":"","pkey":"index"}})";
    Run run(projectOf(carrierTrack(std::string(kAm) + "," + route) + "," + polyTrack("src")));
    run.g().noteOn(0, 69, 1.0f, 1);
    run.g().noteOn(1, 57, 1.0f, 2);
    run.render(10);
    const ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    test::AllocGuard guard;
    for (int b = 0; b < 200; ++b) run.g().process(run.l.data(), run.r.data(), 128, ctx, nullptr, 1.0f);   // (render() would grow a vector)
    CHECK(guard.count() == 0);
}

TEST_CASE("audio-rate route: the output is bit-identical for every callback size", "[arate][engine]") {
    const std::string route = R"({"source":"track","track":"src","follow":true,"depth":0.5,"target":{"dest":"inst","fxId":"","pkey":"index"}})";
    const std::string text = projectOf(carrierTrack(std::string(kAm) + "," + route) + "," + polyTrack("src"));
    const auto play = [&](int callback) {
        Engine e;
        e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(buildGraph(project::importFixtureJson(text), kSr, 1).graph);
        Cmd c; c.type = CmdType::LiveTrack; c.epoch = 1; c.liveTrack = {0};
        e.commands().push(c);
        e.liveNote(Engine::LiveKind::NoteOn, 69, 0.9f);
        const size_t cbN = size_t(callback);
        std::vector<float> out, l(cbN), r(cbN);
        for (int done = 0; done < 16384; done += callback) { e.process(l.data(), r.data(), callback); out.insert(out.end(), l.begin(), l.end()); }
        out.resize(16384);
        return out;
    };
    const auto ref = play(128);
    for (const int cb : {1, 7, 64, 100, 333, 1024}) {
        INFO("callback size " << cb);
        CHECK(play(cb) == ref);
    }
}
