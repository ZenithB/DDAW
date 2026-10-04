// M0 exit criterion: a stub device passes the harness end to end.
// Pipeline: render offline through the device -> write WAV -> read it back ->
// compare against a golden (here analytic, later synthyy's) -> report.
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

#include "core/Constants.h"
#include "devices/effects/stub_gain.h"
#include "harness/Metrics.h"
#include "harness/Wav.h"

using namespace ddaw;
using namespace ddaw::harness;

namespace {
constexpr double kSr = 44100.0;
constexpr size_t kLen = 44100;           // 1 s
constexpr size_t kGoldenLead = 882;      // synthyy goldens start transport at +0.02 s

Audio renderThrough(EffectDevice& d, const std::vector<float>& src) {
    d.prepare(kSr, kMaxBlock);
    Audio out;
    out.sampleRate = kSr;
    out.l = src; out.r = src;
    ProcessContext c{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    for (size_t pos = 0; pos < kLen; pos += kMaxBlock) {
        int n = int(std::min<size_t>(kMaxBlock, kLen - pos));
        d.process(out.l.data() + pos, out.r.data() + pos, n, c, {});
    }
    return out;
}
}  // namespace

TEST_CASE("stub device passes the fixture harness end to end", "[harness]") {
    std::vector<float> src(kLen);
    for (size_t i = 0; i < kLen; ++i)
        src[i] = float(0.8 * std::sin(2.0 * std::numbers::pi * 440.0 * double(i) / kSr));

    StubGain dev;
    dev.setParam(StubGain::Gain, 0.5f);
    Audio rendered = renderThrough(dev, src);
    CHECK(allFinite(rendered.l));
    CHECK(rms(rendered.l) > 1e-3);  // hard assertion: non-silent

    // Analytic golden: same sine at gain 0.5, preceded by a synthyy-style lead-in of silence.
    Audio golden;
    golden.sampleRate = kSr;
    golden.l.assign(kGoldenLead, 0.0f);
    for (float v : src) golden.l.push_back(0.5f * v);
    golden.r = golden.l;
    const std::string path = std::string(DDAW_FIXTURE_DIR) + "/_stub_golden.wav";
    writeWavPcm16(path, golden);
    Audio g = readWav(path);
    std::remove(path.c_str());

    // Align by dropping the golden's lead-in. The stub snaps its gain at prepare(),
    // so only 16-bit quantisation separates render and golden.
    auto gl = skip(g.l, kGoldenLead);
    auto gr = skip(g.r, kGoldenLead);
    double null = rmsNullDb(rendered.l, gl);
    double sim  = spectralSimilarity(toMono(rendered.l, rendered.r), toMono(gl, gr));

    INFO("rms-null " << null << " dB, spectral " << sim);
    CHECK(null < -60.0);  // Tight tier
    CHECK(sim > 0.98);
}
