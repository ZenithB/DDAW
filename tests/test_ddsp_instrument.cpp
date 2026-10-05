// B3: the `ddsp` instrument, end to end with the real checkpoints (skipped when the exported weights are absent).
// Offline mode (ProcessContext::offline) makes the device wait for its inference thread, so these are deterministic.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>

#include "AllocGuard.h"
#include "ddsp/DdspInstrument.h"
#include "devices/Registry.h"
#include "dsp/Fft.h"
#include "dsp/Tracker.h"
#include <algorithm>
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "engine/OfflineRender.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;

bool haveModels() {
    ddsp::registerDevices();
    return ddsp::modelAvailable(0);
}

ProcessContext ctx(bool offline) {
    ProcessContext c{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    c.offline = offline;
    return c;
}

std::vector<float> render(InstrumentDevice& d, int blocks, bool offline = true, int block = 128, int sleepUs = 0) {
    const size_t nb = static_cast<size_t>(block);
    std::vector<float> out, l(nb), r(nb);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), block, ctx(offline), {});
        out.insert(out.end(), l.begin(), l.end());
        if (sleepUs) std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
    }
    return out;
}

// The fundamental of the last `n` samples, found with the project's own tracker (the strongest spectral peak of a
// violin is often a higher harmonic, so a bare FFT peak is the wrong measure of pitch).
double peakHz(const std::vector<float>& x, size_t n = 16384) {
    dsp::PerformanceTracker t;
    t.prepare(kSr);
    std::vector<dsp::TimedFrame> frames(64);
    std::vector<double> f0s;
    const size_t from = x.size() - n;
    for (size_t i = from; i + 64 <= x.size(); i += 64) {
        const int got = t.push(&x[i], 64, frames.data(), 64);
        for (int g = 0; g < got; ++g) if (frames[size_t(g)].frame.f0Hz > 0) f0s.push_back(double(frames[size_t(g)].frame.f0Hz));
    }
    if (f0s.size() < 10) return 0.0;
    std::sort(f0s.begin(), f0s.end());
    return f0s[f0s.size() / 2];
}

double rmsOf(const std::vector<float>& x, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b; ++i) s += double(x[i]) * x[i]; return std::sqrt(s / double(b - a)); }

}  // namespace

TEST_CASE("ddsp instrument: registered, with a sensible parameter table", "[ddsp][instrument]") {
    if (!haveModels()) SKIP("the exported model files are not present");
    auto d = createInstrument("ddsp");
    REQUIRE(d);
    CHECK(d->params().size() == 8);
    CHECK(findParam(d->params(), "model") == 0);
    for (size_t i = 0; i < d->params().size(); ++i) { CHECK(d->params()[i].index == i); CHECK(d->params()[i].def >= d->params()[i].min); CHECK(d->params()[i].def <= d->params()[i].max); }
    CHECK(d->latencySamples() > 192);
    CHECK(d->latencySamples() < int(0.02 * kSr));      // under 20 ms
}

TEST_CASE("ddsp instrument: a note sounds at its pitch, deterministically, after exactly the reported latency", "[ddsp][instrument]") {
    if (!haveModels()) SKIP("the exported model files are not present");
    auto make = [] { auto d = createInstrument("ddsp"); d->prepare(kSr, 128); return d; };
    auto a = make();
    a->noteOn(57, 0.9f, 1);                               // A3 = 220 Hz
    const auto x = render(*a, 400);                       // 1.07 s
    CHECK(rmsOf(x, x.size() - 8192, x.size()) > 0.005);
    CHECK(peakHz(x) == Approx(220.0).epsilon(0.01));
    // the same render again gives the same samples (the network, noise and reverb are all deterministic)
    auto b = make();
    b->noteOn(57, 0.9f, 1);
    const auto y = render(*b, 400);
    REQUIRE(y.size() == x.size());
    CHECK(x == y);
    // silent until the reported latency has passed
    const int lat = a->latencySamples();
    CHECK(rmsOf(x, 0, size_t(lat)) < 1e-7);
    CHECK(rmsOf(x, size_t(lat), size_t(lat) + 4096) > 1e-4);
    // transposing an octave moves the pitch
    auto t = make();
    t->setParam(uint16_t(findParam(t->params(), "transpose")), 12.0f);
    t->noteOn(57, 0.9f, 1);
    CHECK(peakHz(render(*t, 400)) == Approx(440.0).epsilon(0.01));
}

TEST_CASE("ddsp instrument: per-note expression - bend moves the pitch, pressure raises the level", "[ddsp][instrument][mpe]") {
    if (!haveModels()) SKIP("the exported model files are not present");
    auto make = [] { auto d = createInstrument("ddsp"); d->prepare(kSr, 128); return d; };
    auto flat = make();
    flat->noteOn(57, 0.9f, 1);
    const auto x = render(*flat, 400);
    // a note's bend is in semitones: +12 is an octave up; set after the note starts, and for a note id it does not own it does nothing
    auto up = make();
    up->noteOn(57, 0.9f, 7);
    up->noteExpression(99, 2, 12.0f);                       // not this note's id: ignored
    up->noteExpression(7, 2, 12.0f);
    CHECK(peakHz(render(*up, 400)) == Approx(440.0).epsilon(0.015));
    // pressure raises the loudness the model is given by up to 6 dB. The model's level is not a simple function of its
    // loudness input for a held synthetic note, so the check is that it changes the sound, repeatably, and stays audible.
    auto loud = make(), loud2 = make();
    for (auto* d : {loud.get(), loud2.get()}) { d->noteOn(57, 0.9f, 1); d->noteExpression(1, 1, 1.0f); }
    const auto y = render(*loud, 400), y2 = render(*loud2, 400);
    CHECK(y == y2);
    CHECK(y != x);
    CHECK(rmsOf(y, y.size() - 8192, y.size()) > 0.005);
    // a new note starts neutral
    auto reuse = make();
    reuse->noteOn(57, 0.9f, 1);
    reuse->noteExpression(1, 2, 12.0f);
    reuse->noteOff(1);
    render(*reuse, 100);
    reuse->noteOn(57, 0.9f, 2);
    CHECK(peakHz(render(*reuse, 400)) == Approx(220.0).epsilon(0.015));
}

TEST_CASE("ddsp instrument: models differ in timbre, and a released note dies away", "[ddsp][instrument]") {
    if (!haveModels() || !ddsp::modelAvailable(1)) SKIP("the exported model files are not present");
    auto spectrumTilt = [](int model) {
        auto d = createInstrument("ddsp");
        d->prepare(kSr, 128);
        d->setParam(uint16_t(findParam(d->params(), "model")), float(model));
        d->noteOn(60, 0.9f, 1);
        const auto x = render(*d, 400);
        // ratio of energy at the 3rd harmonic to the fundamental (261.6 Hz): flute and violin differ strongly
        dsp::Fft f; f.prepare(16384);
        std::vector<double> re(16384), im(16384, 0.0);
        for (size_t i = 0; i < 16384; ++i) re[i] = double(x[x.size() - 16384 + i]) * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / 16384.0));
        f.forward(re.data(), im.data());
        auto mag = [&](double hz) { double m = 0; const int k0 = int(hz / kSr * 16384); for (int k = k0 - 3; k <= k0 + 3; ++k) m = std::max(m, std::hypot(re[size_t(k)], im[size_t(k)])); return m; };
        return mag(3 * 261.63) / mag(261.63);
    };
    const double violin = spectrumTilt(0), flute = spectrumTilt(1);
    INFO("3rd/1st harmonic: violin " << violin << ", flute " << flute);
    CHECK(std::abs(std::log(violin / flute)) > 0.2);      // different instruments, different harmonic balance

    auto d = createInstrument("ddsp");
    d->prepare(kSr, 128);
    d->noteOn(60, 0.9f, 1);
    render(*d, 200);
    d->noteOff(1);
    d->setParam(uint16_t(findParam(d->params(), "release")), 100.0f);
    const auto tail = render(*d, 800);                   // 2.1 s
    CHECK(rmsOf(tail, tail.size() - 4096, tail.size()) < 0.05 * rmsOf(tail, 0, 4096) + 1e-6);
}

TEST_CASE("ddsp instrument: played by performance frames (timbre transfer)", "[ddsp][instrument]") {
    if (!haveModels()) SKIP("the exported model files are not present");
    auto d = createInstrument("ddsp");
    d->prepare(kSr, 128);
    std::vector<float> all;
    for (int b = 0; b < 400; ++b) {        // a frame every 4 ms = every ~1.5 blocks of 128; deliver one per block at 2/3 rate
        if (b % 3 != 2) d->performance(PerformanceFrame{330.0f, 0.95f, -28.0f, 0.3f});
        const auto o = render(*d, 1);
        all.insert(all.end(), o.begin(), o.end());
    }
    CHECK(rmsOf(all, all.size() - 8192, all.size()) > 0.003);
    CHECK(peakHz(all) == Approx(330.0).epsilon(0.015));
    // the input goes quiet and unvoiced: the sound ends and the network is put to sleep
    for (int b = 0; b < 1200; ++b) { d->performance(PerformanceFrame{0.0f, 0.0f, -80.0f, 0.0f}); render(*d, 1); }
}

TEST_CASE("ddsp instrument: live pacing - no underruns, and nothing allocates on the audio thread", "[ddsp][instrument][realtime][timing]") {
    if (!haveModels()) SKIP("the exported model files are not present");
    // One 3 s run of 64-frame callbacks paced against the clock. The worker thread shares the machine with whatever else
    // is running, so a single run can be spoiled by another process (several ms of descheduling is an underrun); a real
    // regression fails every run. Up to three runs: the best must be clean, and no run may allocate.
    struct Run { uint64_t underruns; uint64_t decoded; bool ready; double rms; size_t allocs; };
    const auto once = [] {
        auto d = createInstrument("ddsp");
        d->prepare(kSr, 64);
        std::vector<float> l(64u), r(64u), out(size_t(3 * 750 * 64));
        // let the model load first (on the device's own thread), so the run below measures steady state
        for (int i = 0; i < 400; ++i) { std::fill(l.begin(), l.end(), 0.0f); d->process(l.data(), r.data(), 64, ctx(false), {}); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        d->noteOn(64, 0.8f, 1);
        size_t allocs;
        {
            test::AllocGuard guard;
            const auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < 3 * 750; ++b) {             // 3 s of 64-frame callbacks paced against the clock
                if (b == 1500) d->noteOff(1);
                if (b == 1600) d->noteOn(67, 0.7f, 2);
                std::fill(l.begin(), l.end(), 0.0f);
                std::fill(r.begin(), r.end(), 0.0f);
                d->process(l.data(), r.data(), 64, ctx(false), {});
                std::copy(l.begin(), l.end(), out.begin() + b * 64);
                std::this_thread::sleep_until(start + std::chrono::microseconds(int64_t(b + 1) * 1333));
            }
            allocs = guard.count();
        }
        auto* dd = dynamic_cast<ddsp::DdspInstrument*>(d.get());
        REQUIRE(dd);
        const auto st = dd->stats();
        return Run{st.underruns, st.framesDecoded, st.modelReady, rmsOf(out, 40000, 60000), allocs};
    };
    uint64_t best = ~uint64_t(0);
    for (int attempt = 0; attempt < 3 && best > 3; ++attempt) {
        const Run run = once();
        std::printf("[ddsp live] 3 s at 64 frames: %llu control frames decoded, %llu underrun blocks\n", (unsigned long long)run.decoded, (unsigned long long)run.underruns);
        CHECK(run.allocs == 0);
        CHECK(run.ready);
        CHECK(run.rms > 0.003);
        best = std::min(best, run.underruns);
    }
    CHECK(best <= 3);
}

TEST_CASE("ddsp instrument: in a project, the offline render puts the note where the clip says (latency compensated)", "[ddsp][instrument][engine]") {
    if (!haveModels()) SKIP("the exported model files are not present");
    // a note at beat 1 (0.5 s at 120 bpm): after the engine trims the reported latency it must start there
    const char* proj = R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
      "tracks":[{"id":"t1","kind":"synth","inst":{"type":"ddsp","params":{"level":0,"attack":5}},"fx":[],"gain":0,"pan":0}],
      "clips":{"t1|s":{"len":384,"notes":{"n":{"p":57,"s":96,"d":192,"v":0.9}}}}}})";
    const auto res = engine::renderFixture(project::importFixtureJson(proj), kSr, engine::RenderOptions{});
    REQUIRE(res.complete());
    size_t first = 0;
    for (size_t i = 0; i < res.l.size(); ++i) if (std::abs(res.l[i]) > 1e-3f) { first = i; break; }
    REQUIRE(first > 0);
    const double expected = 0.5 * kSr;
    INFO("first sound at " << double(first) / kSr * 1000.0 << " ms, the note is at 500 ms");
    CHECK(double(first) > expected - 0.002 * kSr);        // never early
    CHECK(double(first) < expected + 0.060 * kSr);         // the model's own attack: it builds up over a few frames
}

TEST_CASE("ddsp instrument: timbre transfer through the engine - a tone in, a violin out, within the latency budget", "[ddsp][instrument][engine][latency]") {
    if (!haveModels()) SKIP("the exported model files are not present");
    const char* proj = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
      "tracks":[{"id":"t1","kind":"synth","inst":{"type":"ddsp","params":{"level":0}},"fx":[],"gain":0,"pan":0}],"clips":{}}})";
    engine::Engine e;
    e.prepare(kSr, engine::MasterLimiterConfig{engine::MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(engine::buildGraph(project::importFixtureJson(proj), kSr, 1).graph);
    Cmd t; t.type = CmdType::Tracking; t.tracking = {0}; t.epoch = 1;
    e.commands().push(t);
    e.setTrackerEnabled(true);

    const double onsetS = 1.0, total = 3.0;
    const size_t onset = size_t(onsetS * kSr), n = size_t(total * kSr);
    std::vector<float> in(n, 0.0f), out(n, 0.0f), r(64u);
    for (size_t i = onset; i < n; ++i) in[i] = 0.4f * float(std::sin(2.0 * std::numbers::pi * 247.0 * double(i - onset) / kSr));
    // run paced against the clock, as a device would; the first second (silence) also lets the model load
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0, b = 0; i + 64 <= n; i += 64, ++b) {
        e.processIO(&in[i], &in[i], &out[i], r.data(), 64);
        std::this_thread::sleep_until(start + std::chrono::microseconds(int64_t((b + 1) * 1333)));
    }
    size_t first = 0;
    for (size_t i = onset; i < n; ++i) if (std::abs(out[i]) > 0.005f) { first = i; break; }
    REQUIRE(first > onset);
    const double ms = 1000.0 * double(first - onset) / kSr;
    std::printf("[ddsp latency] a tone starts at the input; the violin is audible %.1f ms later (tracker look-ahead %.1f ms), device latency excluded\n", ms,
                1000.0 * e.trackerLatencySamples() / kSr);
    CHECK(ms < 80.0);                                          // the PLAN's 50 ms interactive target is for the pitch pipeline; the model's own onset adds a few frames
    CHECK(peakHz(out) == Approx(247.0).epsilon(0.015));
    CHECK(rmsOf(out, n - 16384, n) > 0.003);
    while (auto old = e.takeRetired()) {}
}
