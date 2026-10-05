// B2: the tracking path. Loudness against Magenta's own numbers, the resampler, YIN on tones and noise,
// the envelope follower, the combined tracker's latency, and that none of it allocates.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numbers>
#include <random>
#include <sstream>

#include "../AllocGuard.h"
#include "dsp/EnvelopeFollower.h"
#include "dsp/Tracker.h"
#include "harness/Wav.h"

using namespace ddaw;
using namespace ddaw::dsp;
using Catch::Approx;

namespace {

std::vector<float> tone(double hz, double sr, double secs, float amp = 0.5f) {
    std::vector<float> x(size_t(secs * sr));
    for (size_t i = 0; i < x.size(); ++i) x[i] = amp * float(std::sin(2.0 * std::numbers::pi * hz * double(i) / sr));
    return x;
}

std::vector<TimedFrame> run(PerformanceTracker& t, const std::vector<float>& x, int block = 64) {
    std::vector<TimedFrame> all, buf(64);
    for (size_t i = 0; i < x.size(); i += size_t(block)) {
        const int n = int(std::min<size_t>(size_t(block), x.size() - i));
        const int got = t.push(x.data() + i, n, buf.data(), int(buf.size()));
        all.insert(all.end(), buf.begin(), buf.begin() + got);
    }
    return all;
}

std::string fixture(const char* name) { return std::string(DDAW_FIXTURE_DIR) + "/ddaw/b2/" + name; }

}  // namespace

TEST_CASE("loudness matches Magenta's compute_loudness on the reference signal", "[tracker][loudness]") {
    const auto wav = harness::readWav(fixture("signal_16k.wav"));
    REQUIRE(wav.sampleRate == 16000.0);
    std::vector<double> ref;
    {
        std::ifstream in(fixture("loudness_ref.csv"));
        REQUIRE(in);
        double v;
        while (in >> v) ref.push_back(v);
    }
    PerformanceTracker t;
    t.prepare(16000.0);
    const auto frames = run(t, wav.l);
    REQUIRE(frames.size() >= 550);
    REQUIRE(ref.size() >= frames.size());
    double worst = 0;
    size_t worstAt = 0;
    for (size_t k = 0; k < frames.size(); ++k) {
        const double d = std::abs(double(frames[k].frame.loudnessDb) - ref[k]);
        if (d > worst) { worst = d; worstAt = k; }
    }
    INFO("worst difference " << worst << " dB at frame " << worstAt);
    CHECK(worst < 0.02);                                   // float32 TensorFlow against our double FFT
    CHECK(frames[100].inputFrame == Approx(100.0 * 64));   // frame k is stamped at k * 64 input samples
}

TEST_CASE("resampler: unity DC gain, tone amplitude and alignment preserved, images and aliases rejected", "[tracker][resampler]") {
    for (double host : {44100.0, 48000.0, 96000.0}) {
        INFO("host rate " << host);
        StreamResampler r;
        r.prepare(host, 16000.0);
        const auto x = tone(1000.0, host, 0.5);
        std::vector<float> y; std::vector<double> ts;
        r.process(x.data(), int(x.size()), [&](float v, double t) { y.push_back(v); ts.push_back(t); });
        REQUIRE(y.size() > 7000);
        double err = 0;
        for (size_t m = 200; m < y.size() - 200; ++m)       // away from the edges
            err = std::max(err, std::abs(double(y[m]) - 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * ts[m] / host)));
        CHECK(err < 2e-3);                                     // time-aligned to the input, no gain error
        // an 11 kHz tone is above the 8 kHz output Nyquist: it must vanish, not fold to 5 kHz
        StreamResampler a;
        a.prepare(host, 16000.0);
        const auto hi = tone(11000.0, host, 0.25);
        double peak = 0; int n = 0;
        a.process(hi.data(), int(hi.size()), [&](float v, double) { if (++n > 400) peak = std::max(peak, std::abs(double(v))); });
        CHECK(peak < 0.5 * std::pow(10.0, -70.0 / 20.0));       // better than 70 dB down
        StreamResampler dc;
        dc.prepare(host, 16000.0);
        std::vector<float> ones(int(host * 0.1), 1.0f);
        double last = 0;
        dc.process(ones.data(), int(ones.size()), [&](float v, double) { last = v; });
        CHECK(last == Approx(1.0).margin(1e-4));
    }
    StreamResampler same;
    same.prepare(16000.0, 16000.0);
    CHECK(same.latencyIn() == 0);
}

TEST_CASE("loudness through the resampler agrees with the 16 kHz path", "[tracker][loudness]") {
    PerformanceTracker ref;
    ref.prepare(16000.0);
    const auto a = run(ref, tone(440.0, 16000.0, 1.0, 0.3f));
    for (double host : {44100.0, 48000.0}) {
        PerformanceTracker t;
        t.prepare(host);
        const auto b = run(t, tone(440.0, host, 1.0, 0.3f));
        REQUIRE(b.size() > 200);
        for (size_t k = 20; k < 200; ++k) CHECK(double(b[k].frame.loudnessDb) == Approx(double(a[k].frame.loudnessDb)).margin(0.15));
    }
}

TEST_CASE("yin: tones across the range are tracked within a few cents at every host rate", "[tracker][yin]") {
    for (double host : {44100.0, 48000.0}) {
        for (double hz : {82.4, 110.0, 196.0, 261.6, 440.0, 880.0, 1175.0}) {
            INFO("host " << host << " f0 " << hz);
            PerformanceTracker t;
            TrackerConfig cfg; cfg.minHz = 70.0f;
            t.prepare(host, cfg);
            const auto f = run(t, tone(hz, host, 0.6));
            REQUIRE(f.size() > 100);
            for (size_t k = 30; k < f.size(); ++k) {
                REQUIRE(f[k].frame.f0Hz > 0.0f);
                CHECK(std::abs(1200.0 * std::log2(double(f[k].frame.f0Hz) / hz)) < 8.0);   // cents
                CHECK(f[k].frame.confidence > 0.9f);
            }
        }
    }
}

TEST_CASE("yin: a harmonic-rich tone is not heard an octave off; noise and silence are unvoiced", "[tracker][yin]") {
    PerformanceTracker t;
    t.prepare(48000.0);
    // sawtooth-like: 20 harmonics, the fundamental weak
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); ++i) {
        double s = 0;
        for (int h = 1; h <= 20; ++h) s += (h == 1 ? 0.3 : 1.0 / h) * std::sin(2.0 * std::numbers::pi * 146.8 * h * double(i) / 48000.0);
        x[i] = 0.3f * float(s);
    }
    auto f = run(t, x);
    int wrong = 0;
    for (size_t k = 30; k < f.size(); ++k) if (std::abs(1200.0 * std::log2(double(f[k].frame.f0Hz) / 146.8)) > 20.0) ++wrong;
    CHECK(wrong == 0);

    PerformanceTracker n;
    n.prepare(48000.0);
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0.0f, 0.2f);
    std::vector<float> noise(48000);
    for (auto& v : noise) v = g(rng);
    f = run(n, noise);
    size_t voiced = 0;
    for (size_t k = 30; k < f.size(); ++k) voiced += f[k].frame.f0Hz > 0.0f;
    CHECK(double(voiced) / double(f.size() - 30) < 0.03);        // white noise has no pitch

    PerformanceTracker s;
    s.prepare(48000.0);
    f = run(s, std::vector<float>(24000, 0.0f));
    for (auto& fr : f) { CHECK(fr.frame.f0Hz == 0.0f); CHECK(fr.frame.loudnessDb == Approx(-80.0f)); }
}

TEST_CASE("yin: vibrato and glides follow the sung pitch", "[tracker][yin]") {
    const double sr = 48000.0;
    PerformanceTracker t;
    t.prepare(sr);
    std::vector<float> x(size_t(sr * 1.0));
    double ph = 0;
    std::vector<double> truth(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        const double f = 330.0 * std::pow(2.0, 0.5 / 12.0 * std::sin(2.0 * std::numbers::pi * 5.5 * double(i) / sr));   // +-50 cents at 5.5 Hz
        ph += 2.0 * std::numbers::pi * f / sr;
        x[i] = 0.4f * float(std::sin(ph));
        truth[i] = f;
    }
    const auto f = run(t, x);
    double worst = 0;
    for (size_t k = 30; k < f.size(); ++k) {
        const double expected = truth[size_t(f[k].inputFrame)];
        worst = std::max(worst, std::abs(1200.0 * std::log2(double(f[k].frame.f0Hz) / expected)));
    }
    CHECK(worst < 12.0);   // the 2-period window smooths a 50-cent, 5.5 Hz vibrato by only a few cents
}

TEST_CASE("tracker latency: a pitch step is reported within the budget, lower floors cost more", "[tracker][latency]") {
    const double sr = 48000.0;
    struct Row { float floorHz; double detectMs; double totalMs; };
    std::vector<Row> rows;
    for (float floorHz : {70.0f, 150.0f, 300.0f}) {
        PerformanceTracker t;
        TrackerConfig cfg; cfg.minHz = floorHz;
        t.prepare(sr, cfg);
        const double base = floorHz < 100 ? 140.0 : 2.0 * floorHz, up = base * 1.5;
        const size_t stepAt = size_t(0.5 * sr);
        std::vector<float> x(size_t(1.0 * sr));
        double ph = 0;
        for (size_t i = 0; i < x.size(); ++i) { ph += 2.0 * std::numbers::pi * (i < stepAt ? base : up) / sr; x[i] = 0.4f * float(std::sin(ph)); }
        const auto f = run(t, x);
        double detect = -1;
        for (const auto& fr : f)
            if (fr.inputFrame >= double(stepAt) && std::abs(1200.0 * std::log2(double(fr.frame.f0Hz) / up)) < 30.0) { detect = (fr.inputFrame - double(stepAt)) / sr; break; }
        REQUIRE(detect >= 0.0);
        rows.push_back({floorHz, 1000.0 * detect, 1000.0 * (detect + double(t.latencySamples()) / sr)});
        INFO("floor " << floorHz << " Hz: window centre reaches the new pitch " << rows.back().detectMs << " ms after the step, available " << rows.back().totalMs << " ms after it");
        CHECK(rows.back().totalMs < 45.0);                      // PLAN 4.7: well inside the 50 ms interactive target
        CHECK(double(t.latencySamples()) / sr * 1000.0 < 25.0);  // the look-ahead alone (YIN half window + 16 ms loudness + resampler)
    }
    CHECK(rows[0].totalMs >= rows[2].totalMs - 1.0);             // a higher floor is never slower
    for (const auto& r : rows) std::printf("[tracker latency] floor %.0f Hz: step detected %.1f ms after the step, frame available %.1f ms after it\n", r.floorHz, r.detectMs, r.totalMs);
}

TEST_CASE("envelope follower: attack and release time constants", "[tracker][envelope]") {
    EnvelopeFollower e;
    e.prepare(48000.0);
    e.setAttackMs(10.0f);
    e.setReleaseMs(100.0f);
    int n = 0;
    float y = 0;
    while (y < 0.632f && n < 48000) { y = e.next(1.0f); ++n; }
    CHECK(n == Approx(480).margin(6));                // 63% in one attack time constant
    for (int i = 0; i < 20000; ++i) e.next(1.0f);
    n = 0;
    while ((y = e.next(0.0f)) > 0.368f && n < 48000) ++n;
    CHECK(n == Approx(4800).margin(30));              // and 37% left after one release time constant
    EnvelopeFollower r;
    r.prepare(48000.0, EnvelopeFollower::Mode::Rms);
    r.setReleaseMs(50.0f);
    float v = 0;
    for (int i = 0; i < 48000; ++i) v = r.next(0.5f * std::sin(2.0f * float(std::numbers::pi) * 1000.0f * float(i) / 48000.0f));
    CHECK(v == Approx(0.5f * 0.7071f).margin(0.03f));  // RMS of a sine
}

TEST_CASE("tracker never allocates after prepare", "[tracker][rt]") {
    PerformanceTracker t;
    t.prepare(48000.0);
    const auto x = tone(220.0, 48000.0, 1.0);
    std::vector<TimedFrame> buf(16);
    test::AllocGuard guard;
    int frames = 0;
    for (size_t i = 0; i + 64 <= x.size(); i += 64) frames += t.push(x.data() + i, 64, buf.data(), 16);
    CHECK(frames > 200);
    CHECK(guard.count() == 0);
}

TEST_CASE("tracker cost: CPU per callback at 64 frames", "[tracker][cost][timing]") {
    PerformanceTracker t;
    t.prepare(48000.0);
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * float(std::sin(0.0288 * double(i)) + 0.3 * std::sin(0.0576 * double(i)));
    std::vector<TimedFrame> buf(8);
    double worst = 0, sum = 0;
    int n = 0;
    for (size_t i = 0; i + 64 <= x.size(); i += 64) {
        const auto t0 = std::chrono::steady_clock::now();
        t.push(x.data() + i, 64, buf.data(), 8);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, us); sum += us; ++n;
    }
    std::printf("[tracker cost] mean %.1f us, worst %.1f us per 64-frame callback (budget 1333 us)\n", sum / n, worst);
#ifdef NDEBUG
    CHECK(worst < 400.0);        // a hop's FFT and YIN land in one callback; still under a third of the buffer
    CHECK(sum / n < 100.0);
#endif
}
