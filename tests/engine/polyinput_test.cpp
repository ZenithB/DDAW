// Polyphonic audio input end to end: a chord played into the input becomes several notes on the live target,
// found by the analysis thread, within a stated latency, and silence releases them.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <numbers>
#include <set>
#include <thread>

#include "dsp/Fft.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "engine/PolyInput.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;

namespace {

constexpr double kSr = 48000.0;
const char* kProj = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
  "tracks":[{"id":"t1","kind":"synth","inst":{"type":"poly","params":{"wave":3,"attack":0.005,"sustain":1.0,"release":0.05}},"fx":[],"gain":0,"pan":0}],"clips":{}}})";

double hz(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

float sample(const std::vector<int>& notes, double t) {
    double s = 0;
    for (int m : notes) for (int h = 1; h <= 6; ++h) s += std::sin(2.0 * std::numbers::pi * hz(m) * h * t) / h;
    return float(0.12 * s);
}

}  // namespace

TEST_CASE("polyphonic audio input: a chord in becomes a chord out, and it stops when the sound does", "[polyinput][engine][threads]") {
    Engine e;
    e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(buildGraph(project::importFixtureJson(kProj), kSr, 1).graph);
    Cmd lt; lt.type = CmdType::LiveTrack; lt.epoch = 1; lt.liveTrack = {0}; e.commands().push(lt);
    PolyInput poly(e);
    poly.start();
    CHECK(poly.latencyFrames() > int(0.08 * kSr));
    CHECK(poly.latencyFrames() < int(0.2 * kSr));

    const double onset = 0.5, offset = 1.7, total = 2.6;
    const size_t n = size_t(total * kSr);
    std::vector<float> in(n, 0.0f), out(n, 0.0f), r(64u);
    const std::vector<int> chord{60, 64, 67};
    for (size_t i = size_t(onset * kSr); i < size_t(offset * kSr); ++i) in[i] = sample(chord, double(i) / kSr);

    std::set<int> started;
    double firstOn = -1, lastOff = -1;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0, b = 0; i + 64 <= n; i += 64, ++b) {   // paced against the clock, as a device
        e.processIO(&in[i], &in[i], &out[i], r.data(), 64);
        Engine::NoteRecord nr;
        while (e.popNoteRecord(nr)) {
            const double at = double(i) / kSr;
            if (nr.on) { started.insert(nr.pitch); if (firstOn < 0) firstOn = at; }
            else lastOff = at;
        }
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t((b + 1) * 1333)));
    }
    poly.stop();
    INFO("notes started: " << started.size());
    CHECK(started == std::set<int>{60, 64, 67});
    REQUIRE(firstOn > 0);
    const double latencyMs = (firstOn - onset) * 1000.0;
    std::printf("[poly latency] chord starts at the input; its notes start %.0f ms later (reported %.0f ms)\n", latencyMs, 1000.0 * poly.latencyFrames() / kSr);
    CHECK(latencyMs > 40.0);
    CHECK(latencyMs < 260.0);
    REQUIRE(lastOff > 0);
    CHECK(lastOff - offset < 0.35);                            // released within a few frames of the sound ending
    // the instrument actually sounded the chord: all three fundamentals in the output while it was held
    dsp::Fft f; f.prepare(16384);
    std::vector<double> re(16384), im(16384, 0.0);
    const size_t from = size_t(1.2 * kSr);
    for (size_t i = 0; i < 16384; ++i) re[i] = double(out[from + i]) * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / 16384.0));
    f.forward(re.data(), im.data());
    auto mag = [&](double h) { double m = 0; const int k0 = int(h / kSr * 16384); for (int k = k0 - 3; k <= k0 + 3; ++k) m = std::max(m, std::hypot(re[size_t(k)], im[size_t(k)])); return m; };
    const double off = mag(300.0);
    CHECK(mag(hz(60)) > 20 * off);
    CHECK(mag(hz(64)) > 20 * off);
    CHECK(mag(hz(67)) > 20 * off);
    double tail = 0; for (size_t i = n - 9600; i < n; ++i) tail += double(out[i]) * out[i];
    CHECK(std::sqrt(tail / 9600.0) < 1e-3);                     // silent again at the end
    while (auto old = e.takeRetired()) {}
}

TEST_CASE("polyphonic audio input: nothing plays from noise, and stopping releases held notes", "[polyinput][engine][threads]") {
    Engine e;
    e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(buildGraph(project::importFixtureJson(kProj), kSr, 1).graph);
    Cmd lt; lt.type = CmdType::LiveTrack; lt.epoch = 1; lt.liveTrack = {0}; e.commands().push(lt);
    PolyInput poly(e);
    poly.start();
    std::vector<float> in(size_t(1.2 * kSr)), out(in.size()), r(64u);
    uint32_t s = 7;
    for (auto& v : in) { s = s * 1664525u + 1013904223u; v = 0.3f * (float(s >> 9) / 4194304.0f - 1.0f); }
    for (size_t i = 0, b = 0; i + 64 <= in.size(); i += 64, ++b) { e.processIO(&in[i], &in[i], &out[i], r.data(), 64); std::this_thread::sleep_for(std::chrono::microseconds(1000)); }
    poly.stop();
    CHECK(poly.notesStarted() == 0);
    CHECK(poly.framesAnalysed() > 5);

    // a note held while the service is stopped is released
    PolyInput again(e);
    again.start();
    std::vector<float> tone(size_t(0.9 * kSr));
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = sample({69}, double(i) / kSr);
    std::vector<float> o2(tone.size());
    for (size_t i = 0, b = 0; i + 64 <= tone.size(); i += 64, ++b) { e.processIO(&tone[i], &tone[i], &o2[i], r.data(), 64); std::this_thread::sleep_for(std::chrono::microseconds(1000)); }
    CHECK(again.notesStarted() >= 1);
    again.stop();
    std::vector<float> silence(64 * 400, 0.0f), o3(silence.size());
    for (size_t i = 0; i + 64 <= silence.size(); i += 64) e.processIO(&silence[i], &silence[i], &o3[i], r.data(), 64);
    double tail = 0; for (size_t i = silence.size() - 4096; i < silence.size(); ++i) tail += double(o3[i]) * o3[i];
    CHECK(std::sqrt(tail / 4096.0) < 1e-3);                     // the stop released the note; it died away
    while (auto old = e.takeRetired()) {}
}
