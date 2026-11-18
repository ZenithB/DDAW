// Every effect has an input and an output trim, metered, and every dynamics processor reports its gain reduction (ARCH 30).
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

struct Pass final : EffectDevice {          // a wire: what comes out is what went in
    std::span<const ParamSpec> params() const override { return {}; }
    void prepare(double, int) override {}
    void setParam(uint16_t, float) override {}
    void process(float*, float*, int, const ProcessContext&, const ModInputs&) override {}
    void reset() override {}
};

std::string song(const std::string& fxJson, const std::string& masterFx = "") {
    return R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[
      {"id":"t1","kind":"synth","inst":{"type":"poly","params":{"wave":3,"attack":0.002,"sustain":1.0,"release":0.05}},"fx":[)" + fxJson + R"(],"gain":0,"pan":0}],
      "masterFx":[)" + masterFx + R"(],"clips":{}}})";
}
const ProcessContext kCtx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};

double peakAfter(Graph& g, MeterBank* m, int blocks = 60) {
    std::vector<float> l(128), r(128);
    double pk = 0;
    for (int b = 0; b < blocks; ++b) { g.process(l.data(), r.data(), 128, kCtx, m, 0.9f); if (b > blocks / 2) for (float v : l) pk = std::max(pk, double(std::abs(v))); }
    return pk;
}
}  // namespace

TEST_CASE("effect trims: the input gain scales what the device sees and the output gain what leaves it, live and from the project", "[engine][fxtrim]") {
    registerEffect("pass", [] { return std::unique_ptr<EffectDevice>(new Pass()); });
    auto build = [](const std::string& fx) { auto b = buildGraph(project::importFixtureJson(song(fx)), kSr, 1); REQUIRE(b.complete()); b.graph->noteOn(0, 57, 1.0f, 1); return b; };
    auto flat = build(R"({"id":"p","type":"pass","on":true,"params":{}})");
    const double base = peakAfter(*flat.graph, nullptr);
    REQUIRE(base > 0.01);
    auto in6 = build(R"({"id":"p","type":"pass","on":true,"params":{},"in":-6})");
    CHECK(peakAfter(*in6.graph, nullptr) == Approx(base * 0.501).epsilon(0.02));
    auto out6 = build(R"({"id":"p","type":"pass","on":true,"params":{},"out":6})");
    CHECK(peakAfter(*out6.graph, nullptr) == Approx(base * 1.995).epsilon(0.02));
    auto both = build(R"({"id":"p","type":"pass","on":true,"params":{},"in":-12,"out":12})");
    CHECK(peakAfter(*both.graph, nullptr) == Approx(base).epsilon(0.02));
    // live: the resolver knows both
    ParamAddr a{};
    REQUIRE(flat.resolver.resolve("t1|p|in", a));
    REQUIRE(flat.resolver.resolve("t1|p|out", a));
    flat.resolver.resolve("t1|p|in", a);
    flat.graph->setParam(a, -6.0f);
    CHECK(peakAfter(*flat.graph, nullptr, 120) == Approx(base * 0.501).epsilon(0.03));
}

TEST_CASE("effect trims: the meters show the level after the trims, per effect, on a track and on the master chain", "[engine][fxtrim][meters]") {
    registerEffect("pass", [] { return std::unique_ptr<EffectDevice>(new Pass()); });
    const std::string fx = R"({"id":"a","type":"pass","on":true,"params":{},"in":-6},{"id":"b","type":"pass","on":true,"params":{},"out":-6})";
    auto b = buildGraph(project::importFixtureJson(song(fx, R"({"id":"m","type":"pass","on":true,"params":{},"in":-12})")), kSr, 1);
    REQUIRE(b.complete());
    b.graph->noteOn(0, 57, 1.0f, 1);
    MeterBank meters;
    peakAfter(*b.graph, &meters);
    const float in0 = meters.fxIn(0, 0), out0 = meters.fxOut(0, 0), in1 = meters.fxIn(0, 1), out1 = meters.fxOut(0, 1);
    CHECK(in0 > 0.01f);
    CHECK(out0 == Approx(in0).epsilon(0.001));                              // a wire: the trim ahead of it is in both
    CHECK(in1 == Approx(out0).epsilon(0.001));                              // the second sees what the first let out
    CHECK(out1 == Approx(in1 * 0.501).epsilon(0.02));                       // and trims its own output
    CHECK(meters.fxIn(-1, 0) > 0.0f);                                       // the master chain too
    CHECK(meters.fxOut(-1, 0) == Approx(meters.fxIn(-1, 0)).epsilon(0.001));
    CHECK(meters.fxIn(0, 5) == 0.0f);                                       // a slot with no effect
    CHECK(meters.fxIn(500, 0) == 0.0f);                                     // out of range reads zero
    meters.clearFx();
    CHECK(meters.fxIn(0, 0) == 0.0f);
    CHECK(meters.fxOut(-1, 0) == 0.0f);
}

TEST_CASE("every dynamics processor reports gain reduction through the graph's meters", "[engine][fxtrim][meters]") {
    // a loud, steady saw through each: the reduction it reports is below zero, and a quiet signal through a gate reports its closed gate
    struct Case { const char* type; const char* params; };
    for (const Case& c : {Case{"comp", R"("thresh":-40,"ratio":10)"}, Case{"opto", R"("reduction":1.0)"}, Case{"mbcomp", R"("b0_thresh":-50,"b1_thresh":-50,"b2_thresh":-50,"b0_ratio":8,"b1_ratio":8,"b2_ratio":8)"},
                          Case{"gate", R"("thresh":-10,"range":-60)"}}) {
        INFO(c.type);
        const std::string text = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[
          {"id":"t1","kind":"synth","inst":{"type":"poly","params":{"wave":0,"attack":0.002,"sustain":1.0,"cutoff":8000}},
           "fx":[{"type":")" + std::string(c.type) + R"(","on":true,"params":{)" + c.params + R"(}}],"gain":0,"pan":0}],"clips":{}}})";
        auto b = buildGraph(project::importFixtureJson(text), kSr, 1);
        REQUIRE(b.complete());
        MeterBank meters;
        const bool quiet = std::string(c.type) == "gate";
        b.graph->noteOn(0, 48, quiet ? 0.02f : 1.0f, 1);
        peakAfter(*b.graph, &meters, 400);
        float deepest = 0.0f;
        for (int band = 0; band < 3; ++band) deepest = std::min(deepest, meters.fxGr(0, 0, band));
        CHECK(deepest < -1.0f);
    }
}

TEST_CASE("the ducker reports its dip", "[engine][fxtrim][meters]") {
    auto d = createEffect("duck");
    REQUIRE(d != nullptr);
    CHECK(d->hasReductionMeter());
    d->prepare(kSr, 128);
    for (auto& p : d->params()) if (std::string(p.key) == "amount") d->setParam(p.index, 1.0f);
    d->reset();
    CHECK(d->gainReductionDb() > -0.1f);                                    // no dip yet
    std::vector<float> l(128, 0.3f), r(128, 0.3f);
    d->setSidechain(true);
    d->trigger();
    float deepest = 0;
    for (int b = 0; b < 10; ++b) { d->process(l.data(), r.data(), 128, kCtx, {}); deepest = std::min(deepest, d->gainReductionDb()); }
    CHECK(deepest < -6.0f);
}
