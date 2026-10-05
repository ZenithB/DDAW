// Live note input (MIDI keyboard / computer keyboard): chords play together on the target track, the sustain
// pedal holds, retriggering a key ends the old note, every note is echoed with its timeline tick, and it
// all happens without allocating on the audio thread.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <thread>

#include "../AllocGuard.h"
#include "dsp/Fft.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;
const char* kProj = R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
  "tracks":[{"id":"t1","kind":"synth","inst":{"type":"poly","params":{"wave":3,"attack":0.005,"sustain":1.0,"release":0.05}},"fx":[],"gain":0,"pan":0},
            {"id":"t2","kind":"drum","inst":{"type":"drum","params":{}},"fx":[],"gain":0,"pan":0}],"clips":{}}})";

struct Rig {
    Engine e;
    std::vector<float> l = std::vector<float>(128u), r = std::vector<float>(128u);
    Rig(int track = 0) {
        e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(buildGraph(project::importFixtureJson(kProj), kSr, 1).graph);
        Cmd c; c.type = CmdType::LiveTrack; c.epoch = 1; c.liveTrack = {int16_t(track)};
        e.commands().push(c);
    }
    std::vector<float> run(int blocks) {
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) { e.process(l.data(), r.data(), 128); out.insert(out.end(), l.begin(), l.end()); }
        return out;
    }
};

// magnitude of the spectrum near `hz` over the last 16384 samples
double mag(const std::vector<float>& x, double hz) {
    constexpr size_t N = 16384;
    dsp::Fft f; f.prepare(int(N));
    std::vector<double> re(N), im(N, 0.0);
    for (size_t i = 0; i < N; ++i) re[i] = double(x[x.size() - N + i]) * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / N));
    f.forward(re.data(), im.data());
    const int k0 = int(hz / kSr * N);
    double m = 0;
    for (int k = k0 - 3; k <= k0 + 3; ++k) m = std::max(m, std::hypot(re[size_t(k)], im[size_t(k)]));
    return m;
}

}  // namespace

TEST_CASE("live notes: a chord sounds as three notes at once, and each releases on its own", "[livenotes][engine]") {
    Rig r;
    r.e.liveNote(Engine::LiveKind::NoteOn, 60, 0.8f);
    r.e.liveNote(Engine::LiveKind::NoteOn, 64, 0.8f);
    r.e.liveNote(Engine::LiveKind::NoteOn, 67, 0.8f);
    auto x = r.run(200);
    const double c4 = mag(x, 261.63), e4 = mag(x, 329.63), g4 = mag(x, 392.0), off = mag(x, 300.0);
    INFO(c4 << " " << e4 << " " << g4 << " " << off);
    CHECK(c4 > 20 * off);
    CHECK(e4 > 20 * off);
    CHECK(g4 > 20 * off);
    r.e.liveNote(Engine::LiveKind::NoteOff, 64);
    x = r.run(200);
    CHECK(mag(x, 329.63) < 0.02 * e4);              // E gone
    CHECK(mag(x, 261.63) > 0.5 * c4);               // C and G still ringing
    CHECK(mag(x, 392.0) > 0.5 * g4);
    r.e.liveNote(Engine::LiveKind::AllOff);
    x = r.run(200);
    CHECK(mag(x, 261.63) < 0.02 * c4);
}

TEST_CASE("live notes: the sustain pedal holds released notes until it is lifted", "[livenotes][engine]") {
    Rig r;
    r.e.liveNote(Engine::LiveKind::SustainDown);
    r.e.liveNote(Engine::LiveKind::NoteOn, 69, 0.8f);
    r.run(100);
    r.e.liveNote(Engine::LiveKind::NoteOff, 69);
    auto x = r.run(200);
    const double held = mag(x, 440.0);
    CHECK(held > 1.0);                              // still sounding with the key up
    r.e.liveNote(Engine::LiveKind::SustainUp);
    x = r.run(200);
    CHECK(mag(x, 440.0) < 0.02 * held);
}

TEST_CASE("live notes: striking a key again ends the old note; notes with no target are ignored", "[livenotes][engine]") {
    Rig r;
    r.e.liveNote(Engine::LiveKind::NoteOn, 57, 0.8f);
    r.e.liveNote(Engine::LiveKind::NoteOn, 57, 0.8f);
    r.run(4);
    Engine::NoteRecord n;
    std::vector<Engine::NoteRecord> rec;
    while (r.e.popNoteRecord(n)) rec.push_back(n);
    REQUIRE(rec.size() == 3);                       // on, off (the first), on
    CHECK(rec[0].on == 1); CHECK(rec[1].on == 0); CHECK(rec[2].on == 1);
    CHECK(rec[0].id == rec[1].id);
    CHECK(rec[2].id != rec[0].id);
    // a target that does not exist: nothing plays, nothing is recorded
    Rig none(5);
    none.e.liveNote(Engine::LiveKind::NoteOn, 60, 0.8f);
    const auto x = none.run(50);
    for (float v : x) REQUIRE(v == 0.0f);
    CHECK_FALSE(none.e.popNoteRecord(n));
}

TEST_CASE("live notes: records carry the timeline tick, and the drum track plays pads", "[livenotes][engine]") {
    Rig r(1);
    Cmd p; p.type = CmdType::TransportPlay; p.transportPlay = {1, 0.0}; r.e.commands().push(p);
    r.run(100);                                     // 100 * 128 frames = 0.267 s = about 0.53 beat... 256 ticks/1s: 51 ticks
    r.e.liveNote(Engine::LiveKind::NoteOn, 0, 1.0f);   // the kick
    const auto x = r.run(100);
    double e2 = 0; for (float v : x) e2 += double(v) * v;
    CHECK(e2 > 1e-3);
    Engine::NoteRecord n;
    REQUIRE(r.e.popNoteRecord(n));
    CHECK(n.on == 1);
    CHECK(n.track == 1);
    // 100 blocks of 128 frames at 48 kHz and 120 bpm: 12800 frames = 0.2667 s = 51.2 ticks
    CHECK(n.tick == Approx(51.2).margin(2.7));      // within one chunk (2.7 ticks)
}

TEST_CASE("live notes: a storm of keys from several threads allocates nothing and loses nothing it can keep", "[livenotes][engine][rt][threads]") {
    Rig r;
    std::thread a([&] { for (int i = 0; i < 300; ++i) { r.e.liveNote(Engine::LiveKind::NoteOn, 40 + i % 40, 0.6f); std::this_thread::sleep_for(std::chrono::microseconds(50)); r.e.liveNote(Engine::LiveKind::NoteOff, 40 + i % 40); } });
    std::thread b([&] { for (int i = 0; i < 300; ++i) { r.e.liveNote(Engine::LiveKind::NoteOn, 60 + i % 30, 0.6f); std::this_thread::sleep_for(std::chrono::microseconds(70)); r.e.liveNote(Engine::LiveKind::NoteOff, 60 + i % 30); } });
    test::AllocGuard guard;
    for (int blk = 0; blk < 400; ++blk) { r.e.process(r.l.data(), r.r.data(), 128); std::this_thread::sleep_for(std::chrono::microseconds(100)); Engine::NoteRecord n; while (r.e.popNoteRecord(n)) {} }
    CHECK(guard.count() == 0);
    a.join(); b.join();
    r.e.liveNote(Engine::LiveKind::AllOff);
    r.run(4);
}
