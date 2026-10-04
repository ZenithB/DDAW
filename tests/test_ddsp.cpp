// B1 unit tests that need no checkpoints: scaling, loader errors, Nyquist masking, and the
// real-time rule (no allocation) for the decoder step and the harmonic synth.
// Checkpoint-dependent checks live in ddaw_b1 (ctest: b1_verify_*, b1_synth_spec_*).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <fstream>
#include <memory>
#include <vector>

#include "AllocGuard.h"
#include "ddsp/Decoder.h"
#include "ddsp/HarmonicSynth.h"
#include "ddsp/Weights.h"

using namespace ddaw::ddsp;

TEST_CASE("model input scaling matches ddsp preprocessing", "[ddsp]") {
    CHECK(scaleLoudnessDb(-80.0f) == Catch::Approx(0.0f));
    CHECK(scaleLoudnessDb(0.0f) == Catch::Approx(1.0f));
    CHECK(scaleF0Hz(440.0f) == Catch::Approx(69.0f / 127.0f).epsilon(1e-6));
    CHECK(scaleF0Hz(0.0f) == 0.0f);  // unvoiced maps to MIDI 0
}

TEST_CASE("weights loader rejects bad files", "[ddsp]") {
    CHECK_THROWS(loadWeights("/nonexistent.ddspw"));
    const std::string path = std::string(DDAW_FIXTURE_DIR) + "/_bad.ddspw";
    std::ofstream(path, std::ios::binary) << "NOTDDSP-garbage";
    CHECK_THROWS(loadWeights(path));
    std::remove(path.c_str());
    Decoder d;
    CHECK_FALSE(d.loaded());
    CHECK_THROWS(d.load("/nonexistent.ddspw"));
}

TEST_CASE("harmonic frame: Nyquist masking and normalisation", "[ddsp]") {
    std::vector<float> raw(kDecoderOut, 0.0f);
    HarmonicFrame f;
    makeHarmonicFrame(raw.data(), 5000.0f, kModelSampleRate, f);  // 2nd harmonic = 10 kHz > 8 kHz
    CHECK(f.hd[0] > 0.0f);
    for (int k = 1; k < kNumHarmonics; ++k) CHECK(f.hd[k] == 0.0f);
    CHECK(f.hd[0] == Catch::Approx(1.0f));

    makeHarmonicFrame(raw.data(), 200.0f, kModelSampleRate, f);
    float sum = 0;
    for (float h : f.hd) sum += h;
    CHECK(sum == Catch::Approx(1.0f).epsilon(1e-5));  // 39 harmonics below Nyquist share the unit mass
    CHECK(f.hd[38] > 0.0f);   // 39 * 200 = 7800 Hz
    CHECK(f.hd[39] == 0.0f);  // 40 * 200 = 8000 Hz is >= Nyquist and is removed, as in ddsp
}

TEST_CASE("harmonic synth: finite, silent at zero amplitude, no partial above Nyquist", "[ddsp]") {
    HarmonicSynth syn;
    syn.prepare(kModelSampleRate, kHopSamples);
    HarmonicFrame loud, quiet;
    std::vector<float> raw(kDecoderOut, 2.0f);
    makeHarmonicFrame(raw.data(), 440.0f, kModelSampleRate, loud);
    loud.amp = 1.0f;
    quiet = loud; quiet.amp = 0.0f;

    float out[kHopSamples];
    syn.renderInterval(quiet, quiet, out);
    for (float v : out) CHECK(v == 0.0f);
    for (int i = 0; i < 100; ++i) {
        syn.renderInterval(loud, loud, out);
        for (float v : out) REQUIRE(std::isfinite(v));
    }
    // A 7900 Hz fundamental has no energy beyond itself; 2f is above Nyquist and must not alias in.
    HarmonicFrame hi;
    makeHarmonicFrame(raw.data(), 7900.0f, kModelSampleRate, hi);
    hi.amp = 1.0f;
    syn.reset();
    double e = 0;
    for (int i = 0; i < 50; ++i) {
        syn.renderInterval(hi, hi, out);
        for (float v : out) e += double(v) * double(v);
    }
    CHECK(e > 0.0);
    CHECK(std::sqrt(e / (50.0 * kHopSamples)) < 1.01 * 0.7072);  // <= one unit sinusoid (rms 1/sqrt 2)
}

TEST_CASE("decoder step and harmonic synth do not allocate", "[ddsp][realtime]") {
    auto dec = std::make_unique<Decoder>();  // constructed (and allocated) outside the guard
    HarmonicSynth syn;
    syn.prepare(kModelSampleRate, kHopSamples);
    std::vector<float> out(kDecoderOut);
    HarmonicFrame a, b;
    float audio[kHopSamples];
    ddaw::test::AllocGuard guard;
    dec->reset();
    for (int i = 0; i < 50; ++i) {
        dec->step(0.7f, 0.5f, out.data());
        makeHarmonicFrame(out.data(), 330.0f, kModelSampleRate, a);
        b = a;
        syn.renderInterval(a, b, audio);
    }
    CHECK(guard.count() == 0);
}
