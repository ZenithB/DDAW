#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

#include "harness/Metrics.h"
#include "harness/Wav.h"

using namespace ddaw::harness;

namespace {
std::vector<float> sine(size_t n, double hz, double sr, double amp = 0.5) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = float(amp * std::sin(2.0 * std::numbers::pi * hz * double(i) / sr));
    return x;
}
}  // namespace

TEST_CASE("rmsNullDb", "[metrics]") {
    auto a = sine(4096, 440, 44100);
    CHECK(rmsNullDb(a, a) == -160.0);
    auto b = a;
    for (auto& v : b) v *= 1.01f;  // 1% error -> -40 dB
    CHECK(rmsNullDb(b, a) == Catch::Approx(-40.0).margin(0.1));
}

TEST_CASE("spectralSimilarity", "[metrics]") {
    auto a = sine(16384, 440, 44100);
    auto same = sine(16384, 440, 44100, 0.25);  // level change leaves log-spectrum shape alone
    auto other = sine(16384, 3000, 44100);
    CHECK(spectralSimilarity(a, a) == Catch::Approx(1.0).margin(1e-9));
    CHECK(spectralSimilarity(a, same) > 0.99);
    CHECK(spectralSimilarity(a, other) < spectralSimilarity(a, same));
    CHECK(spectralSimilarity(std::vector<float>(100), std::vector<float>(100)) == 0.0);
}

TEST_CASE("WAV round trip (PCM16)", "[metrics]") {
    Audio a;
    a.sampleRate = 44100;
    a.l = sine(1000, 440, 44100);
    a.r = sine(1000, 880, 44100);
    const std::string path = std::string(DDAW_FIXTURE_DIR) + "/_roundtrip.wav";
    writeWavPcm16(path, a);
    Audio b = readWav(path);
    std::remove(path.c_str());
    REQUIRE(b.frames() == 1000);
    CHECK(b.sampleRate == 44100);
    CHECK(rmsNullDb(b.l, a.l) < -80.0);  // 16-bit quantisation floor
    CHECK(rmsNullDb(b.r, a.r) < -80.0);
}

TEST_CASE("readWav reads a synthyy golden when available", "[metrics][.synthyy]") {
    Audio g = readWav(std::string(DDAW_FIXTURE_DIR) + "/synthyy/golden/fx-eq.wav");
    CHECK(g.sampleRate == 44100);
    CHECK(g.frames() > 0);
    CHECK(allFinite(g.l));
}
