// The B6 synth families through the engine: built from project JSON with their parameters, playing clips offline, taking
// audio-rate routes and morph maps, reporting latency, and not allocating on the audio thread with all six playing.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <string>

#include "../AllocGuard.h"
#include "dsp/Fft.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "engine/OfflineRender.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;
const char* const kTypes[] = {"harmnoise", "subtractive", "wavetable", "waveshaper", "modal", "perc"};

std::string track(const std::string& id, const std::string& type, const std::string& params, const std::string& extra = "") {
    return R"({"id":")" + id + R"(","kind":"synth","inst":{"type":")" + type + R"(","params":{)" + params + R"(}},"fx":[],"gain":0,"pan":0)" + extra + "}";
}
std::string live(const std::string& tracks) {
    return R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[)" + tracks + R"(],"clips":{}}})";
}
std::string clipProject(const std::string& type, const std::string& params) {
    return R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[)" + track("t1", type, params) +
           R"(],"clips":{"t1|s":{"len":384,"notes":{"a":{"p":57,"s":0,"d":300,"v":0.9}}}}}})";
}

struct Run {
    BuildResult br;
    std::vector<float> l = std::vector<float>(128u), r = std::vector<float>(128u);
    explicit Run(const std::string& text) : br(buildGraph(project::importFixtureJson(text), kSr, 1)) {}
    Graph& g() { return *br.graph; }
    std::vector<float> render(int blocks, bool modulation = false) {
        const ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) {
            if (modulation) g().applyModulation(0.0, true, TransportMode::Session, 128);
            g().process(l.data(), r.data(), 128, ctx, nullptr, 1.0f);
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
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
double rms(const std::vector<float>& x, size_t from, size_t len) {
    double s = 0;
    for (size_t i = from; i < from + len && i < x.size(); ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(len));
}
RenderResult render(const std::string& proj) { return renderFixture(project::importFixtureJson(proj), kSr, RenderOptions{}); }
}  // namespace

TEST_CASE("B6 instruments play a clip from project JSON, and level 0 silences them", "[b6][engine]") {
    for (const char* type : kTypes) {
        INFO(type);
        const auto on = render(clipProject(type, R"("level":0.8)"));
        REQUIRE(on.complete());
        float mx = 0; for (float v : on.l) { REQUIRE(std::isfinite(v)); mx = std::max(mx, std::abs(v)); }
        CHECK(mx > 0.02f);
        CHECK(mx < 1.2f);
        const auto off = render(clipProject(type, R"("level":0)"));
        REQUIRE(off.complete());
        float mo = 0; for (float v : off.l) mo = std::max(mo, std::abs(v));
        CHECK(mo == 0.0f);
    }
}

TEST_CASE("B6 instruments read their parameters from the project", "[b6][engine]") {
    auto ratio2 = [&](const std::string& type, const std::string& params) {
        Run run(live(track("t1", type, params)));
        REQUIRE(run.br.complete());
        run.g().noteOn(0, 57, 1.0f, 1);
        const auto x = run.render(400);
        return mag(x, 440.0) / mag(x, 220.0);
    };
    // harmnoise: odd partials only, vs a 1/k series
    CHECK(ratio2("harmnoise", R"("noise":0,"sustain":1.0,"attack":0.001,"oddEven":1)") < 1e-3);
    CHECK(ratio2("harmnoise", R"("noise":0,"sustain":1.0,"attack":0.001,"oddEven":0)") == Approx(0.5).epsilon(0.1));
    // subtractive: a sine oscillator has no second partial, a saw has
    CHECK(ratio2("subtractive", R"("wave1":3,"mix2":0,"cutoff":16000,"drive":0,"sustain":1.0,"attack":0.001,"envAmt":0,"keytrack":0,"velFilt":0)") < 1e-3);
    CHECK(ratio2("subtractive", R"("wave1":0,"mix2":0,"cutoff":16000,"drive":0,"sustain":1.0,"attack":0.001,"envAmt":0,"keytrack":0,"velFilt":0)") == Approx(0.5).epsilon(0.1));
    // wavetable: the classic bank's sine frame, and its saw frame (position 2/7)
    CHECK(ratio2("wavetable", R"("bank":0,"pos":0,"sustain":1.0,"attack":0.001)") < 1e-3);
    CHECK(ratio2("wavetable", R"("bank":0,"pos":0.2857142857,"sustain":1.0,"attack":0.001)") == Approx(0.5).epsilon(0.1));
    // waveshaper: a Chebyshev shaper with h2 = 1 puts as much into the 2nd harmonic as the 1st
    CHECK(ratio2("waveshaper", R"("shape":2,"drive":1,"dSus":1,"h2":1,"h3":0,"h4":0,"h5":0,"sustain":1.0,"attack":0.001)") == Approx(1.0).epsilon(0.1));
    // modal: a string struck in the middle has no 2nd mode
    CHECK(ratio2("modal", R"("model":0,"pos":0.5,"exciter":0,"decay":6)") < 1e-3);
}

TEST_CASE("B6 instruments take audio-rate routes on their A-rate ports", "[b6][arate][engine]") {
    struct Case { const char* type; const char* params; const char* key; double depth; };
    const Case cases[] = {
        {"subtractive", R"("wave1":0,"mix2":0,"cutoff":1500,"sustain":1.0,"attack":0.001)", "cutoff", 0.3},
        {"subtractive", R"("wave1":3,"mix2":0,"sustain":1.0,"attack":0.001)", "pitch", 0.2},
        {"wavetable", R"("bank":0,"pos":0.3,"sustain":1.0,"attack":0.001)", "pos", 0.3},
        {"wavetable", R"("bank":0,"pos":0.3,"sustain":1.0,"attack":0.001)", "pitch", 0.2},
        {"waveshaper", R"("shape":0,"drive":2,"dSus":1,"sustain":1.0,"attack":0.001)", "drive", 0.4},
        {"waveshaper", R"("shape":0,"drive":2,"dSus":1,"sustain":1.0,"attack":0.001)", "bias", 0.4},
        {"waveshaper", R"("shape":0,"drive":2,"dSus":1,"sustain":1.0,"attack":0.001)", "pitch", 0.2},
    };
    for (const auto& c : cases) {
        INFO(c.type << " " << c.key);
        const std::string route = std::string(R"({"source":"osc","shape":0,"hz":30,"depth":)") + std::to_string(c.depth) + R"(,"target":{"dest":"inst","fxId":"","pkey":")" + c.key + R"("}})";
        Run plain(live(track("t1", c.type, c.params)));
        Run routed(live(track("t1", c.type, c.params, R"(,"arate":[)" + route + "]")));
        REQUIRE(plain.br.complete());
        REQUIRE(routed.br.complete());                           // the key is an A-rate port: no "unsupported" report
        plain.g().noteOn(0, 57, 1.0f, 1); routed.g().noteOn(0, 57, 1.0f, 1);
        const auto a = plain.render(300), b = routed.render(300);
        double diff = 0; for (size_t i = 0; i < a.size(); ++i) diff = std::max(diff, double(std::abs(a[i] - b[i])));
        CHECK(diff > 0.02);                                      // the route is doing something
        CHECK(rms(b, 20000, 8192) > 0.02);
    }
    // a parameter that is not an A-rate port is reported and dropped
    Run bad(live(track("t1", "wavetable", "", R"(,"arate":[{"source":"osc","shape":0,"hz":30,"depth":0.3,"target":{"dest":"inst","fxId":"","pkey":"unison"}}])")));
    CHECK_FALSE(bad.br.complete());
}

TEST_CASE("B6 instruments can be moved by a morph map", "[b6][morph][engine]") {
    // harmnoise tilt (-24..6 dB/oct) between two anchors: steep at one, flat at the other
    auto map = [&](double x) {
        return R"({"name":"m","x":)" + std::to_string(x) + R"(,"y":0.5,"method":"idw","power":2,"targets":[{"dest":"inst","fxId":"","pkey":"tilt"}],"anchors":[
          {"name":"A","x":0.2,"y":0.5,"values":[0.0]},{"name":"B","x":0.8,"y":0.5,"values":[0.8]}]})";   // -24 and 0 dB/octave
    };
    for (auto [x, expect] : {std::pair{0.2, 1.0 / 16.0}, std::pair{0.8, 1.0}}) {
        Run run(live(track("t1", "harmnoise", R"("noise":0,"sustain":1.0,"attack":0.001)", R"(,"morph":[)" + map(x) + "]")));
        REQUIRE(run.br.complete());
        run.g().noteOn(0, 57, 1.0f, 1);
        const auto out = run.render(400, true);
        INFO("stick at " << x);
        CHECK(mag(out, 440.0) / mag(out, 220.0) == Approx(expect).epsilon(0.1));
    }
}

TEST_CASE("B6: the waveshaper reports its oversampler's delay, the others none", "[b6][engine]") {
    for (const char* type : kTypes) {
        Run run(live(track("t1", type, "")));
        CHECK(run.g().latencySamples() == (std::string(type) == "waveshaper" ? 13 : 0));
    }
}

TEST_CASE("B6: six instruments playing at once allocate nothing on the audio thread", "[b6][engine][realtime]") {
    std::string tracks;
    for (size_t i = 0; i < std::size(kTypes); ++i) tracks += (i ? "," : "") + track("t" + std::to_string(i), kTypes[i], "");
    Run run(live(tracks));
    REQUIRE(run.br.complete());
    for (int t = 0; t < 6; ++t) run.g().noteOn(uint16_t(t), 52, 0.9f, uint32_t(t + 1));
    run.render(4);
    const ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    test::AllocGuard guard;
    for (int b = 0; b < 600; ++b) {
        if (b % 40 == 0) for (int t = 0; t < 6; ++t) { run.g().noteOff(uint16_t(t), uint32_t(t + 1 + b)); run.g().noteOn(uint16_t(t), uint8_t(48 + (b / 40) % 12 + t), 0.8f, uint32_t(t + 1 + b + 40)); }
        run.g().process(run.l.data(), run.r.data(), 128, ctx, nullptr, 1.0f);   // not render(): that grows a vector
    }
    CHECK(guard.count() == 0);
}
