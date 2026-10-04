#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/Constants.h"
#include "devices/effects/stub_gain.h"
#include "harness/Metrics.h"

using namespace ddaw;

namespace {
ProcessContext ctx() { return {48000.0, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4}; }
}

TEST_CASE("ParamSpec table is well formed", "[contract]") {
    StubGain d;
    auto ps = d.params();
    REQUIRE(!ps.empty());
    for (size_t i = 0; i < ps.size(); ++i) {
        CHECK(ps[i].index == i);  // index addresses the table
        CHECK(ps[i].key != nullptr);
        CHECK(ps[i].min <= ps[i].def);
        CHECK(ps[i].def <= ps[i].max);
        if (ps[i].curve == Curve::Stepped) CHECK(ps[i].smoothingMs == 0.0f);
    }
}

TEST_CASE("StubGain: non-silent, finite, correct gain", "[contract]") {
    StubGain d;
    d.prepare(48000.0, kMaxBlock);
    d.setParam(StubGain::Gain, 0.5f);
    d.reset();  // snap to target

    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    for (int i = 0; i < kMaxBlock; ++i) l[size_t(i)] = r[size_t(i)] = std::sin(0.05f * float(i));
    auto in = l;
    d.process(l.data(), r.data(), kMaxBlock, ctx(), {});

    CHECK(harness::allFinite(l));
    CHECK(harness::rms(l) > 0.0);
    for (size_t i = 0; i < in.size(); ++i) REQUIRE(l[i] == Catch::Approx(in[i] * 0.5f).margin(1e-6));
}

TEST_CASE("StubGain: parameter sweep stays stable", "[contract]") {
    StubGain d;
    d.prepare(44100.0, kMaxBlock);
    std::vector<float> l(kMaxBlock, 1.0f), r(kMaxBlock, 1.0f);
    for (int b = 0; b < 200; ++b) {
        d.setParam(StubGain::Gain, (b % 2) ? 2.0f : 0.0f);
        std::fill(l.begin(), l.end(), 1.0f);
        std::fill(r.begin(), r.end(), 1.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        REQUIRE(harness::allFinite(l));
        for (float v : l) REQUIRE(v >= 0.0f);
        for (float v : l) REQUIRE(v <= 2.0f);
    }
}
