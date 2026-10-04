// B3: the noise filter, the partitioned-convolution reverb and the whole 16 kHz chain against DDSP.
// The synthetic checks need nothing; the reference checks need the exported weights and the references written by
// tools/b1/ddsp_ref.py and tools/b3/ref_noise_reverb.py (skipped when absent).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include "AllocGuard.h"
#include "ddsp/Synth16k.h"
#include "ddsp/Weights.h"

using namespace ddaw::ddsp;
namespace fs = std::filesystem;

namespace {

std::vector<float> readBin(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<float> v(static_cast<size_t>(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * 4));
    return v;
}

double rmsDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0) {
    double s = 0, r = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = from; i < n; ++i) { const double d = double(a[i]) - double(b[i]); s += d * d; r += double(b[i]) * double(b[i]); }
    return std::sqrt(s / std::max(r, 1e-30));   // relative to the reference's energy
}

}  // namespace

TEST_CASE("reverb convolution equals direct convolution (dry added, first tap masked)", "[synth16k]") {
    std::mt19937 g(5);
    std::normal_distribution<float> nd(0.0f, 0.5f);
    for (size_t irLen : {size_t(1), size_t(64), size_t(65), size_t(1000), size_t(4000)}) {
        std::vector<float> ir(irLen);
        for (auto& v : ir) v = nd(g) * 0.1f;
        std::vector<float> x(64 * 90);
        for (auto& v : x) v = nd(g);
        ReverbConv rc;
        rc.prepare(ir.data(), ir.size());
        std::vector<float> y(x.size());
        for (size_t b = 0; b < x.size(); b += 64) rc.process(&x[b], &y[b]);
        double worst = 0;
        for (size_t n = 0; n < x.size(); ++n) {
            double acc = x[n];
            for (size_t k = 1; k < irLen && k <= n; ++k) acc += double(ir[k]) * double(x[n - k]);
            worst = std::max(worst, std::abs(acc - double(y[n])));
        }
        INFO("ir length " << irLen);
        CHECK(worst < 1e-5);
    }
}

TEST_CASE("reverb convolution is real-time safe and in-place", "[synth16k][realtime]") {
    std::vector<float> ir(48000, 0.001f);
    ReverbConv rc;
    rc.prepare(ir.data(), ir.size());
    std::vector<float> blk(64, 0.3f);
    ddaw::test::AllocGuard guard;
    for (int i = 0; i < 400; ++i) rc.process(blk.data(), blk.data());
    CHECK(guard.count() == 0);
}

TEST_CASE("noise filter: flat magnitudes pass noise, a closed filter is silent, no allocation", "[synth16k]") {
    NoiseFilter nf;
    nf.prepare();
    std::mt19937 g(2);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> raw(kNumNoise, 5.0f), noise(64), out(64);   // raw + bias(-5) = 0 -> exp_sigmoid(0) = 2*0.5^ln10 = 0.405
    double e = 0;
    ddaw::test::AllocGuard guard;
    for (int f = 0; f < 200; ++f) {
        for (auto& v : noise) v = u(g);
        nf.push(raw.data(), noise.data());
        if (f >= 1) { nf.pop(out.data()); if (f > 20) for (float v : out) e += double(v) * v; }
    }
    CHECK(guard.count() == 0);
    // a flat 0.405 magnitude is a 0.405-gain all-pass: output power = 0.405^2 * input power (1/3 for uniform noise)
    CHECK(std::sqrt(e / (179.0 * 64.0)) == Catch::Approx(0.405 * std::sqrt(1.0 / 3.0)).epsilon(0.05));
    NoiseFilter closed;
    closed.prepare();
    std::vector<float> off(kNumNoise, -40.0f);
    double e2 = 0;
    for (int f = 0; f < 50; ++f) { closed.push(off.data(), noise.data()); if (f >= 1) { closed.pop(out.data()); for (float v : out) e2 += double(v) * v; } }
    CHECK(std::sqrt(e2 / (49.0 * 64.0)) < 1e-3);
}

TEST_CASE("synth16k against DDSP: noise filter, then the whole chain with reverb", "[synth16k][reference]") {
    const fs::path models = fs::path(DDAW_MODELS_DIR);
    int ran = 0;
    for (const char* inst : {"violin", "flute", "tenor_saxophone", "trumpet"}) {
        const auto ref = models / "ref" / inst;
        if (!fs::exists(ref / "wet_out.bin") || !fs::exists(models / "export" / (std::string(inst) + ".ddspw"))) continue;
        ++ran;
        INFO(inst);
        const auto raw = readBin((ref / "decoder_raw.bin").string());
        const auto f0 = readBin((ref / "f0_hz.bin").string());
        const auto noise = readBin((ref / "noise_in.bin").string());
        const auto noiseRef = readBin((ref / "noise_out.bin").string());
        const auto wetRef = readBin((ref / "wet_out.bin").string());
        const size_t frames = f0.size();
        REQUIRE(raw.size() == frames * kDecoderOut);
        REQUIRE(noise.size() == frames * kHopSamples);

        // the noise filter alone
        {
            NoiseFilter nf;
            nf.prepare();
            std::vector<float> out;
            std::vector<float> blk(64);
            for (size_t f = 0; f < frames; ++f) {
                nf.push(&raw[f * kDecoderOut + kNumAmps + kNumHarmonics], &noise[f * kHopSamples]);
                if (f >= 1) { nf.pop(blk.data()); out.insert(out.end(), blk.begin(), blk.end()); }
            }
            std::vector<float> zeros(64, 0.0f);
            nf.push(&raw[(frames - 1) * kDecoderOut + kNumAmps + kNumHarmonics], zeros.data());
            nf.pop(blk.data());
            out.insert(out.end(), blk.begin(), blk.end());
            REQUIRE(out.size() == noiseRef.size());
            CHECK(rmsDiff(out, noiseRef) < 5e-4);   // -66 dB: TensorFlow's float32 FFT on a signal 40 dB below the harmonics
        }
        // everything: harmonic + noise + reverb
        {
            const auto w = ddaw::ddsp::loadWeights((models / "export" / (std::string(inst) + ".ddspw")).string());
            const auto& ir = w.at("reverb.ir");
            REQUIRE(ir.size() == 48000);
            Synth16k s;
            s.prepare(ir.data.data(), ir.size());
            std::vector<float> out, blk(64);
            for (size_t f = 0; f < frames; ++f)
                if (s.push(&raw[f * kDecoderOut], f0[f], &noise[f * kHopSamples], blk.data())) out.insert(out.end(), blk.begin(), blk.end());
            std::vector<float> zeros(64, 0.0f);
            if (s.push(&raw[(frames - 1) * kDecoderOut], f0[frames - 1], zeros.data(), blk.data())) out.insert(out.end(), blk.begin(), blk.end());
            REQUIRE(out.size() == wetRef.size());
            CHECK(rmsDiff(out, wetRef) < 3e-3);   // -50 dB, the B1 end-to-end gate: the harmonic bank alone differs from TF's float32 by -57 dB
            // the reverb on its own, fed DDSP's own dry signal, is exact: this isolates the 48000-tap convolution
            const auto harm = readBin((ref / "harmonic_signal.bin").string());
            std::vector<float> dry(harm.size()), wet(harm.size());
            for (size_t i = 0; i < dry.size(); ++i) dry[i] = harm[i] + noiseRef[i];
            ReverbConv rc;
            rc.prepare(ir.data.data(), ir.size());
            for (size_t b = 0; b + 64 <= dry.size(); b += 64) rc.process(&dry[b], &wet[b]);
            // TensorFlow's float32 65536-point FFT is itself 7.8e-4 (relative) off the exact float64 convolution
            // (checked in numpy); this port matches the exact result, so the gap to TF is TF's rounding.
            CHECK(rmsDiff(wet, wetRef) < 1.2e-3);
        }
    }
    if (!ran) WARN("no references present: run tools/b1/ddsp_ref.py and tools/b3/ref_noise_reverb.py");
}
