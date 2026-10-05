// Morph maps (B5): the stick blends anchor values into the targeted parameters, live, smoothly and deterministically.
// The probe is an fmop carrier whose ratio r1 sets its pitch: ratio 1 -> 440 Hz, ratio 2 -> 880 Hz (r1 is a log parameter,
// so a halfway blend is the geometric mean, 622 Hz).
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
// log-unit of ratio r on r1's range 0.25..16
double unitOfRatio(double r) { return std::log(r / 0.25) / std::log(64.0); }

std::string num(double v) { return std::to_string(v); }

std::string projectWith(const std::string& morphJson) {
    return R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[
      {"id":"t1","kind":"synth","inst":{"type":"fmop","params":{"algo":4,"l1":1,"l2":0,"l3":0,"l4":0,"attack":0.001,"sustain":1.0,"release":0.05}},
       "fx":[],"gain":0,"pan":0)" + (morphJson.empty() ? "" : R"(,"morph":[)" + morphJson + "]") + R"(}],"clips":{}}})";
}
// two anchors on a horizontal line: A (0.2, 0.5) ratio 1, B (0.8, 0.5) ratio 2
std::string pitchMap(double x, const std::string& extra = "") {
    return R"({"name":"m","x":)" + num(x) + R"(,"y":0.5,"method":"idw","power":2,"targets":[{"dest":"inst","fxId":"","pkey":"r1"}],"anchors":[
      {"name":"A","x":0.2,"y":0.5,"values":[)" + num(unitOfRatio(1.0)) + R"(]},{"name":"B","x":0.8,"y":0.5,"values":[)" + num(unitOfRatio(2.0)) + "]}]" + extra + "}";
}

struct Run {
    BuildResult br;
    std::vector<float> l = std::vector<float>(128u), r = std::vector<float>(128u);
    explicit Run(const std::string& text) : br(buildGraph(project::importFixtureJson(text), kSr, 1)) {}
    Graph& g() { return *br.graph; }
    std::vector<float> render(int blocks) {
        const ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) {
            g().applyModulation(0.0, true, TransportMode::Session, 128);
            g().process(l.data(), r.data(), 128, ctx, nullptr, 1.0f);
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    }
    void stick(double x, double y) {
        ParamAddr ax{}, ay{};
        REQUIRE(br.resolver.resolve("t1|morph0|x", ax));
        REQUIRE(br.resolver.resolve("t1|morph0|y", ay));
        g().setParam(ax, float(x));
        g().setParam(ay, float(y));
    }
};

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
// frequency of the strongest component in [lo, hi], by the largest FFT bin and a parabolic fit through its neighbours
double peakHz(const std::vector<float>& x, double lo, double hi) {
    constexpr size_t N = 16384;
    dsp::Fft f; f.prepare(int(N));
    std::vector<double> re(N), im(N, 0.0), m(N / 2);
    for (size_t i = 0; i < N; ++i) re[i] = double(x[x.size() - N + i]) * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / N));
    f.forward(re.data(), im.data());
    for (size_t k = 0; k < N / 2; ++k) m[k] = std::hypot(re[k], im[k]);
    size_t best = size_t(lo / kSr * N);
    for (size_t k = size_t(lo / kSr * N); k <= size_t(hi / kSr * N); ++k) if (m[k] > m[best]) best = k;
    const double a = std::log(m[best - 1] + 1e-12), b = std::log(m[best] + 1e-12), c = std::log(m[best + 1] + 1e-12);
    const double off = 0.5 * (a - c) / (a - 2.0 * b + c);
    return (double(best) + off) * kSr / double(N);
}
// frequency from zero crossings over x[from, from + len): good enough for a clean tone, and works on short windows
double zeroCrossHz(const std::vector<float>& x, size_t from, size_t len) {
    int crossings = 0;
    for (size_t i = from + 1; i < from + len; ++i) if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++crossings;
    return 0.5 * double(crossings) * kSr / double(len);
}
}  // namespace

TEST_CASE("morph map: on an anchor the targets take exactly that anchor's values", "[morph][engine]") {
    Run a(projectWith(pitchMap(0.2)));
    REQUIRE(a.br.complete());
    a.g().noteOn(0, 69, 1.0f, 1);
    CHECK(peakHz(a.render(250), 400.0, 500.0) == Approx(440.0).margin(4.0));
    Run b(projectWith(pitchMap(0.8)));
    b.g().noteOn(0, 69, 1.0f, 1);
    CHECK(peakHz(b.render(250), 800.0, 960.0) == Approx(880.0).margin(6.0));
}

TEST_CASE("morph map: halfway between two anchors is the blend, in the parameter's own (log) scale", "[morph][engine]") {
    Run run(projectWith(pitchMap(0.5)));
    run.g().noteOn(0, 69, 1.0f, 1);
    CHECK(peakHz(run.render(250), 560.0, 700.0) == Approx(440.0 * std::sqrt(2.0)).margin(6.0));
}

TEST_CASE("morph map: moving the stick is live, and glides rather than jumps", "[morph][engine]") {
    Run run(projectWith(pitchMap(0.2)));
    run.g().noteOn(0, 69, 1.0f, 1);
    run.render(100);
    run.stick(0.8, 0.5);
    const auto during = run.render(16);                        // the first 43 ms after the move (the stick smooths over 25 ms, the parameter over 15 ms more)
    const double start = zeroCrossHz(during, 0, 512), end = zeroCrossHz(during, during.size() - 512, 512);
    CHECK(start < 560.0);                                      // still near 440 Hz right after the move
    CHECK(end > start + 60.0);                                 // on its way up ...
    CHECK(end < 860.0);                                        // ... not there yet: a glide, not a jump
    const auto settled = run.render(300);
    CHECK(peakHz(settled, 800.0, 960.0) == Approx(880.0).margin(6.0));
}

TEST_CASE("morph map: a target's response curve bends the blend", "[morph][engine]") {
    // anchors at l1 = 0 and l1 = 1; halfway is 0.5 linearly and 0.25 with an exponent of 2
    const auto levelMap = [](double x, const std::string& curves) {
        return R"({"name":"m","x":)" + num(x) + R"(,"y":0.5,"method":"idw","power":2,"targets":[{"dest":"inst","fxId":"","pkey":"l1"}],"curves":)" + curves +
               R"(,"anchors":[{"name":"A","x":0.2,"y":0.5,"values":[0.0]},{"name":"B","x":0.8,"y":0.5,"values":[1.0]}]})";
    };
    Run full(projectWith(levelMap(0.8, "[1.0]")));
    Run lin(projectWith(levelMap(0.5, "[1.0]")));
    Run bent(projectWith(levelMap(0.5, "[2.0]")));
    for (auto* r : {&full, &lin, &bent}) r->g().noteOn(0, 69, 1.0f, 1);
    const double ref = mag(full.render(250), 440.0);
    CHECK(mag(lin.render(250), 440.0) / ref == Approx(0.5).margin(0.03));
    CHECK(mag(bent.render(250), 440.0) / ref == Approx(0.25).margin(0.03));
}

TEST_CASE("morph map: an unknown target is reported and dropped, the rest still moves; a disabled map leaves the stored values", "[morph][engine]") {
    const std::string two = R"({"name":"m","x":0.8,"y":0.5,"targets":[{"dest":"inst","fxId":"","pkey":"nope"},{"dest":"inst","fxId":"","pkey":"r1"}],
      "anchors":[{"name":"A","x":0.2,"y":0.5,"values":[0.5,)" + num(unitOfRatio(1.0)) + R"(]},{"name":"B","x":0.8,"y":0.5,"values":[0.5,)" + num(unitOfRatio(2.0)) + "]}]}";
    Run run(projectWith(two));
    REQUIRE(run.br.unsupported.size() == 1);
    CHECK(run.br.unsupported[0].find("'nope' not found") != std::string::npos);
    run.g().noteOn(0, 69, 1.0f, 1);
    CHECK(peakHz(run.render(250), 800.0, 960.0) == Approx(880.0).margin(6.0));

    std::string off = pitchMap(0.8);
    off.replace(off.find("\"name\":\"m\""), 10, "\"name\":\"m\",\"on\":false");
    Run idle(projectWith(off));
    idle.g().noteOn(0, 69, 1.0f, 1);
    CHECK(peakHz(idle.render(250), 400.0, 500.0) == Approx(440.0).margin(4.0));   // r1's stored default is 1
}

TEST_CASE("morph map: a stick on one side of a 2D field picks the nearer anchors, in both axes", "[morph][engine]") {
    // four anchors at the corners with ratios 1, 2, 4, 0.5: the stick near each corner gives that ratio's pitch
    const std::string four = R"({"name":"m","x":0.05,"y":0.05,"method":"idw","power":3,"targets":[{"dest":"inst","fxId":"","pkey":"r1"}],"anchors":[
      {"name":"a","x":0.0,"y":0.0,"values":[)" + num(unitOfRatio(1.0)) + R"(]},{"name":"b","x":1.0,"y":0.0,"values":[)" + num(unitOfRatio(2.0)) +
        R"(]},{"name":"c","x":0.0,"y":1.0,"values":[)" + num(unitOfRatio(4.0)) + R"(]},{"name":"d","x":1.0,"y":1.0,"values":[)" + num(unitOfRatio(0.5)) + "]}]}";
    Run run(projectWith(four));
    run.g().noteOn(0, 57, 1.0f, 1);   // 220 Hz
    CHECK(peakHz(run.render(250), 200.0, 240.0) == Approx(220.0).margin(4.0));
    run.stick(0.0, 1.0);
    CHECK(peakHz(run.render(400), 840.0, 920.0) == Approx(880.0).margin(8.0));
    run.stick(1.0, 1.0);
    CHECK(peakHz(run.render(400), 100.0, 120.0) == Approx(110.0).margin(3.0));
}

TEST_CASE("morph map: evaluating the map allocates nothing", "[morph][engine][realtime]") {
    Run run(projectWith(pitchMap(0.3)));
    run.g().noteOn(0, 69, 1.0f, 1);
    run.render(10);
    const ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    test::AllocGuard guard;
    for (int b = 0; b < 300; ++b) {
        if (b % 20 == 0) run.stick(double(b % 100) / 100.0, 0.5);
        run.g().applyModulation(0.0, true, TransportMode::Session, 128);
        run.g().process(run.l.data(), run.r.data(), 128, ctx, nullptr, 1.0f);
    }
    CHECK(guard.count() == 0);
}

TEST_CASE("morph map: the output is bit-identical for every callback size when the stick moves on a chunk boundary", "[morph][engine]") {
    const std::string text = projectWith(pitchMap(0.2));
    const auto play = [&](int callback) {
        Engine e;
        e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        auto br = buildGraph(project::importFixtureJson(text), kSr, 1);
        ParamAddr ax{};
        REQUIRE(br.resolver.resolve("t1|morph0|x", ax));
        e.setInitialGraph(std::move(br.graph));
        Cmd c; c.type = CmdType::LiveTrack; c.epoch = 1; c.liveTrack = {0};
        e.commands().push(c);
        e.liveNote(Engine::LiveKind::NoteOn, 69, 0.9f);
        const size_t cbN = size_t(callback);
        std::vector<float> out, l(cbN), r(cbN);
        for (int done = 0; done < 16384; done += callback) {
            if (done == 4096) { Cmd m; m.type = CmdType::SetParam; m.epoch = 1; m.setParam = {ax, 0.9f}; e.commands().push(m); }
            e.process(l.data(), r.data(), callback);
            out.insert(out.end(), l.begin(), l.end());
        }
        out.resize(16384);
        return out;
    };
    const auto ref = play(128);
    for (const int cb : {64, 256, 512, 1024}) { INFO("callback size " << cb); CHECK(play(cb) == ref); }
}
