// The morph map's weights: exact at anchors (Idw), non-negative and summing to 1, smooth, symmetric.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <random>
#include <vector>

#include "dsp/MorphMap.h"

using namespace ddaw::dsp;
using Catch::Approx;

namespace {
const float AX[] = {0.1f, 0.9f, 0.5f, 0.2f}, AY[] = {0.1f, 0.2f, 0.9f, 0.7f};
std::vector<float> weights(MorphMethod m, float x, float y, float power = 2.0f, float width = 0.3f) {
    std::vector<float> w(4u);
    morphWeights(m, power, width, AX, AY, 4, x, y, w.data());
    return w;
}
}  // namespace

TEST_CASE("morph weights: Idw gives exactly the anchor's values on an anchor", "[morph]") {
    for (int a = 0; a < 4; ++a) {
        const auto w = weights(MorphMethod::Idw, AX[a], AY[a]);
        for (int i = 0; i < 4; ++i) CHECK(w[size_t(i)] == (i == a ? 1.0f : 0.0f));
    }
}

TEST_CASE("morph weights: non-negative and summing to one everywhere, for both methods", "[morph]") {
    std::mt19937 g(5);
    std::uniform_real_distribution<float> u(-0.5f, 1.5f);   // also outside the field
    for (const auto method : {MorphMethod::Idw, MorphMethod::Rbf})
        for (int i = 0; i < 2000; ++i) {
            const auto w = weights(method, u(g), u(g), 1.0f + 3.0f * std::abs(u(g)) * 0.5f, 0.05f + 0.4f * std::abs(u(g)) * 0.5f);
            float sum = 0;
            for (float v : w) { REQUIRE(v >= 0.0f); REQUIRE(std::isfinite(v)); sum += v; }
            REQUIRE(sum == Approx(1.0f).margin(1e-5));
        }
}

TEST_CASE("morph weights: the nearest anchor dominates, and the blend moves continuously", "[morph]") {
    for (const auto method : {MorphMethod::Idw, MorphMethod::Rbf}) {
        const auto w = weights(method, 0.12f, 0.12f);
        CHECK(w[0] > 0.5f);
        // no jump along a path through the middle of the field: small steps change the weights a little
        auto prev = weights(method, 0.0f, 0.5f);
        for (int i = 1; i <= 400; ++i) {
            const auto cur = weights(method, float(i) / 400.0f, 0.5f);
            for (size_t k = 0; k < 4; ++k) REQUIRE(std::abs(cur[k] - prev[k]) < 0.06f);
            prev = cur;
        }
    }
}

TEST_CASE("morph weights: symmetric anchors give symmetric weights; one anchor is always weight one", "[morph]") {
    const float ax[] = {0.2f, 0.8f}, ay[] = {0.5f, 0.5f};
    float w[2];
    for (const auto method : {MorphMethod::Idw, MorphMethod::Rbf}) {
        morphWeights(method, 2.0f, 0.3f, ax, ay, 2, 0.5f, 0.9f, w);
        CHECK(w[0] == Approx(0.5f).margin(1e-5));
        CHECK(w[1] == Approx(0.5f).margin(1e-5));
        morphWeights(method, 2.0f, 0.3f, ax, ay, 1, 0.7f, 0.1f, w);
        CHECK(w[0] == 1.0f);
    }
}

TEST_CASE("morph weights: Rbf far from every anchor falls back to Idw instead of dividing by nothing", "[morph]") {
    const auto w = weights(MorphMethod::Rbf, 40.0f, -30.0f, 2.0f, 0.05f);
    float sum = 0;
    for (float v : w) { CHECK(std::isfinite(v)); sum += v; }
    CHECK(sum == Approx(1.0f).margin(1e-5));
}

TEST_CASE("morph shape: the response curve is the identity at 1 and bends the middle otherwise", "[morph]") {
    CHECK(morphShape(0.3f, 1.0f) == 0.3f);
    CHECK(morphShape(0.0f, 2.0f) == 0.0f);
    CHECK(morphShape(1.0f, 0.5f) == Approx(1.0f));
    CHECK(morphShape(0.25f, 0.5f) == Approx(0.5f));   // c < 1 lifts
    CHECK(morphShape(0.5f, 2.0f) == Approx(0.25f));   // c > 1 holds back
    CHECK(morphShape(-1.0f, 2.0f) == 0.0f);
    CHECK(morphShape(7.0f, 2.0f) == 1.0f);
}
