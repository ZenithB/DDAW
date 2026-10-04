// Analytic checks for the shared DSP utilities ported from sf-dsp/src/util (ARCH 12).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

#include "AllocGuard.h"
#include "dsp/Adsr.h"
#include "dsp/DelayLine.h"
#include "dsp/Ladder.h"
#include "dsp/Lfo.h"
#include "dsp/Math.h"
#include "dsp/OnePole.h"
#include "dsp/Oversampler4.h"
#include "dsp/PolyBlepOsc.h"
#include "dsp/Svf.h"

using namespace ddaw::dsp;
using Catch::Approx;

namespace {
constexpr double kPi = std::numbers::pi;

double rms(const std::vector<float>& v, size_t from = 0) {
    double s = 0; for (size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i];
    return std::sqrt(s / double(v.size() - from));
}
std::vector<float> sine(size_t n, double hz, double sr, double amp = 1.0) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = float(amp * std::sin(2 * kPi * hz * double(i) / sr));
    return x;
}
int zeroCrossings(const std::vector<float>& b) {
    int c = 0; for (size_t i = 1; i < b.size(); ++i) c += (b[i - 1] >= 0) != (b[i] >= 0); return c;
}
}  // namespace

TEST_CASE("math helpers", "[dsp]") {
    CHECK(dbToLin(0.0f) == Approx(1.0f));
    CHECK(dbToLin(-6.0f) == Approx(0.501187f).epsilon(1e-4));
    CHECK(linToDb(dbToLin(-13.5f)) == Approx(-13.5f).margin(1e-3));
    CHECK(linToDb(0.0f) == -120.0f);
    CHECK(midiHz(69) == Approx(440.0f).margin(1e-3));
    CHECK(midiHz(57) == Approx(220.0f).margin(1e-3));
    auto [l, r] = panGains(0.0f);
    CHECK(l == Approx(r).margin(1e-6));
    CHECK(l * l + r * r == Approx(1.0f).margin(1e-5));
    CHECK(panGains(-1.0f).first == Approx(1.0f));
    CHECK(panGains(1.0f).second == Approx(1.0f));
}

TEST_CASE("adsr: attack, decay, sustain, release shape", "[dsp]") {
    const float sr = 44100.0f;
    Adsr e; e.prepare(sr); e.setAdsr(0.01f, 0.05f, 0.6f, 0.1f); e.noteOn();
    float prev = 0;
    for (int i = 0; i < int(0.01 * sr); ++i) { const float v = e.next(); REQUIRE(v >= prev); prev = v; }
    for (int i = 0; i < 4; ++i) prev = std::max(prev, e.next());
    CHECK(prev == Approx(1.0f).margin(2e-3));
    for (int i = 0; i < int(0.2 * sr); ++i) { const float v = e.next(); REQUIRE(v <= prev + 1e-6f); REQUIRE(v >= 0.6f - 1e-3f); prev = v; }
    CHECK(prev == Approx(0.6f).margin(5e-3));
    e.noteOff();
    for (int i = 0; i < int(0.3 * sr); ++i) { const float v = e.next(); REQUIRE(v <= prev + 1e-6f); prev = v; }
    CHECK(prev < 1e-3f);
    CHECK_FALSE(e.isActive());
}

TEST_CASE("adsr: release from mid-attack and instant stages", "[dsp]") {
    Adsr e; e.prepare(48000.0f); e.setAdsr(0.5f, 0.1f, 0.8f, 0.05f); e.noteOn();
    for (int i = 0; i < 1000; ++i) e.next();
    const float level = e.value();
    CHECK((level > 0.0f && level < 1.0f));
    e.noteOff();
    CHECK(e.next() <= level);
    for (int i = 0; i < 48000; ++i) e.next();
    CHECK(e.value() == 0.0f);

    Adsr z; z.prepare(44100.0f); z.setAdsr(0.0f, 0.0f, 0.25f, 0.0f); z.noteOn();
    CHECK(z.next() == Approx(1.0f).margin(1e-6));
    z.next();
    CHECK(z.value() == Approx(0.25f).margin(1e-3));
}

TEST_CASE("oscillator: pitch, bounds and phase-continuous retune", "[dsp]") {
    const float sr = 44100.0f;
    for (Wave w : {Wave::Sine, Wave::Saw, Wave::Square, Wave::Triangle}) {
        PolyBlepOsc o(w); o.prepare(sr); o.setFreq(440.0f);
        std::vector<float> b(static_cast<size_t>(sr));
        for (auto& x : b) x = o.next();
        CHECK(std::abs(zeroCrossings(b) - 880) < 20);
    }
    for (Wave w : {Wave::Sine, Wave::Saw, Wave::Square, Wave::Triangle}) {
        PolyBlepOsc o(w); o.prepare(sr); o.setFreq(8000.0f);
        for (int i = 0; i < 8192; ++i) { const float v = o.next(); REQUIRE(std::isfinite(v)); REQUIRE(std::abs(v) < 1.5f); }
    }
    PolyBlepOsc o(Wave::Saw); o.prepare(sr); o.setFreq(220.0f);
    std::vector<float> saw(static_cast<size_t>(sr)); for (auto& x : saw) x = o.next();
    CHECK(rms(saw) == Approx(0.577).margin(0.05));
    PolyBlepOsc s(Wave::Sine); s.prepare(48000.0f); s.setFreq(440.0f);
    float prev = 0;
    for (int i = 0; i < 4096; ++i) { if (i == 2048) s.setFreq(880.0f); const float v = s.next(); REQUIRE(std::abs(v - prev) < 0.13f); prev = v; }
    CHECK(waveFromIndex(0) == Wave::Saw); CHECK(waveFromIndex(1) == Wave::Square); CHECK(waveFromIndex(2) == Wave::Triangle);
    CHECK(waveFromIndex(3) == Wave::Sine); CHECK(waveFromIndex(4) == Wave::Saw); CHECK(waveFromIndex(5) == Wave::Square);
    CHECK(waveFromIndex(6) == Wave::Triangle);
}

TEST_CASE("svf: lowpass corner, highpass and stop-band", "[dsp]") {
    const double sr = 48000;
    auto gainAt = [&](SvfMode m, double fc, double hz) {
        Svf f(m); f.prepare(float(sr)); f.setCutoffQ(float(fc), 0.70710678f);
        auto x = sine(48000, hz, sr);
        for (auto& v : x) v = f.processSample(v);
        return rms(x, 24000) / (1.0 / std::numbers::sqrt2);
    };
    CHECK(gainAt(SvfMode::Lowpass, 1000, 1000) == Approx(std::numbers::sqrt2 / 2).margin(0.02));   // -3 dB at the corner
    CHECK(gainAt(SvfMode::Lowpass, 1000, 100) == Approx(1.0).margin(0.01));
    CHECK(gainAt(SvfMode::Lowpass, 1000, 8000) < 0.02);                                              // 12 dB/oct stop band
    CHECK(gainAt(SvfMode::Highpass, 1000, 8000) == Approx(1.0).margin(0.02));
    CHECK(gainAt(SvfMode::Highpass, 1000, 100) < 0.02);
}

TEST_CASE("one-pole and ladder filters", "[dsp]") {
    const double sr = 48000;
    OnePole lp(OnePoleMode::Lowpass); lp.prepare(float(sr)); lp.setCutoff(1000.0f);
    auto x = sine(48000, 1000, sr);
    for (auto& v : x) v = lp.processSample(v);
    CHECK(rms(x, 24000) / (1.0 / std::numbers::sqrt2) == Approx(std::numbers::sqrt2 / 2).margin(0.03));

    Ladder lad; lad.prepare(float(sr)); lad.setCutoffRes(1000.0f, 0.0f);
    auto y = sine(48000, 8000, sr);
    for (auto& v : y) v = lad.processSample(v);
    CHECK(rms(y, 24000) < 0.01);                                       // 24 dB/oct stop band
    lad.reset(); lad.setCutoffRes(500.0f, 1.0f);                       // near self-oscillation stays bounded
    float mx = 0; for (int i = 0; i < 96000; ++i) { const float v = lad.processSample(i == 0 ? 1.0f : 0.0f); REQUIRE(std::isfinite(v)); mx = std::max(mx, std::abs(v)); }
    CHECK(mx < 4.0f);
}

TEST_CASE("delay line: indexing, interpolation and clamping", "[dsp]") {
    DelayLine d; d.prepare(100);
    CHECK(d.maxDelay() == 127);                    // rounded up to a power of two, minus one
    for (int i = 1; i <= 10; ++i) d.write(float(i));
    CHECK(d.read(1) == 10.0f);                     // the sample from the latest write
    CHECK(d.read(4) == 7.0f);
    CHECK(d.readFrac(2.5f) == Approx(8.5f));
    CHECK(d.read(0) == 10.0f);                     // clamped to 1
    CHECK(d.read(100000) == d.read(d.maxDelay()));
    d.clear();
    CHECK(d.read(1) == 0.0f);
}

TEST_CASE("lfo shapes mirror schema.ts", "[dsp]") {
    CHECK(lfoShapeValue(0, 0.25) == Approx(1.0f).margin(1e-6));      // sine
    CHECK(lfoShapeValue(1, 0.25) == Approx(0.0f).margin(1e-6));      // triangle
    CHECK(lfoShapeValue(1, 0.5) == Approx(1.0f).margin(1e-6));
    CHECK(lfoShapeValue(2, 0.75) == Approx(0.5f).margin(1e-6));      // saw up
    CHECK(lfoShapeValue(3, 0.75) == Approx(-0.5f).margin(1e-6));     // saw down
    CHECK(lfoShapeValue(4, 0.2) == 1.0f); CHECK(lfoShapeValue(4, 0.7) == -1.0f);   // square
    CHECK(lfoShapeValue(5, 3.1) == lfoShapeValue(5, 3.9));           // S&H holds within a cycle
    CHECK(lfoShapeValue(5, 3.1) != lfoShapeValue(5, 4.1));
    CHECK(lfoShapeValue(99, 0.25) == lfoShapeValue(0, 0.25));        // out of range falls back to sine
    Lfo l; l.prepare(1000.0f); l.setShape(0); l.setFreq(10.0f);
    int zc = 0; float prev = l.next(); for (int i = 0; i < 1000; ++i) { const float v = l.next(); zc += (prev < 0) != (v < 0); prev = v; }
    CHECK(zc == Approx(20).margin(1));
}

TEST_CASE("4x oversampler: unity passband, DC and alias suppression", "[dsp]") {
    Oversampler4 os;
    const double sr = 48000;
    auto run = [&](double hz, auto&& f) {
        os.reset();
        auto l = sine(8192, hz, sr, 0.5), r = l;
        for (size_t pos = 0; pos < l.size(); pos += 128) os.process(l.data() + pos, r.data() + pos, 128, f);
        return l;
    };
    auto identity = [](float*, float*, int) {};
    auto thru = run(1000, identity);
    CHECK(rms(thru, 4096) == Approx(0.5 / std::numbers::sqrt2).epsilon(0.01));    // passband gain ~1
    // DC through the cascade settles to unity gain
    os.reset(); std::vector<float> dc(8192, 0.25f), dc2 = dc;
    for (size_t pos = 0; pos < dc.size(); pos += 128) os.process(dc.data() + pos, dc2.data() + pos, 128, identity);
    CHECK(dc.back() == Approx(0.25f).margin(0.002));
    // A hard clipper at 4x produces less aliasing than at 1x: a 7 kHz tone's 3rd harmonic (21 kHz) folds
    // to 3 kHz at 48 kHz without oversampling.
    auto clip = [](float* l, float* r, int n) { for (int i = 0; i < n; ++i) { l[i] = std::clamp(l[i] * 4.0f, -1.0f, 1.0f); r[i] = l[i]; } };
    auto os4 = run(7000, clip);
    auto raw = sine(8192, 7000, sr, 0.5); for (auto& v : raw) v = std::clamp(v * 4.0f, -1.0f, 1.0f);
    auto energyAt = [&](const std::vector<float>& x, double hz) {
        double re = 0, im = 0; for (size_t i = 4096; i < x.size(); ++i) { re += x[i] * std::cos(2 * kPi * hz * double(i) / sr); im += x[i] * std::sin(2 * kPi * hz * double(i) / sr); }
        return std::sqrt(re * re + im * im) / double(x.size() - 4096) * 2.0;
    };
    CHECK(energyAt(os4, 3000) < 0.5 * energyAt(raw, 3000));   // the alias at 3 kHz drops by at least 6 dB
}

TEST_CASE("utilities do not allocate when processing", "[dsp][realtime]") {
    Svf svf; svf.prepare(48000.0f); svf.setCutoffQ(1000.0f, 1.0f);
    Ladder lad; lad.prepare(48000.0f);
    Adsr env; env.prepare(48000.0f); env.noteOn();
    PolyBlepOsc osc; osc.prepare(48000.0f); osc.setFreq(220.0f);
    Lfo lfo; lfo.prepare(48000.0f); lfo.setFreq(3.0f);
    DelayLine dl; dl.prepare(4800);
    Oversampler4 os;
    std::vector<float> l(128, 0.3f), r(128, 0.3f);
    ddaw::test::AllocGuard g;
    for (int i = 0; i < 200; ++i) {
        for (int k = 0; k < 128; ++k) { dl.write(l[size_t(k)]); l[size_t(k)] = svf.processSample(lad.processSample(osc.next() * env.next())) + lfo.next() * 0.01f + dl.readFrac(77.5f) * 0.001f; }
        os.process(l.data(), r.data(), 128, [](float* a, float*, int n) { for (int k = 0; k < n; ++k) a[k] = std::tanh(a[k]); });
    }
    CHECK(g.count() == 0);
}
