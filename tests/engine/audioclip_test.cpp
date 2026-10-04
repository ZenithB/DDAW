// A4 audio tracks and clips: derivation (crop, loop crossfade, reverse, pitch rate, gain, fades) and
// playback through the real Engine: session loop, one-shot re-fire, arrangement start/stop and fade-out.
// The test sample is constant 0.5 so levels are exact; the master limiter is bypassed.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "../AllocGuard.h"
#include "engine/AudioClips.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "project/SampleBank.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;

SamplePtr dc(size_t frames, float v, float sr = 48000.0f) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = sr;
    b->l.assign(frames, v);
    return b;
}
SamplePtr ramp(size_t frames, float sr = 48000.0f) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = sr;
    for (size_t i = 0; i < frames; ++i) b->l.push_back(float(i));
    return b;
}

std::string audioProj(const std::string& audio, double len, const std::string& arr = "") {
    return R"({"scope":{"kind":")" + std::string(arr.empty() ? "scene" : "arr") + R"(","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],
      "tracks":[{"id":"a1","kind":"audio","fx":[],"gain":0,"pan":0}],
      "clips":{"a1|s":{"len":)" + std::to_string(len) + R"(,"audio":)" + audio + R"(}})" + arr + "}}";
}

struct Out { std::vector<float> l; };
Out play(const std::string& json, const project::SampleBank& bank, double secs, bool arrangement) {
    const auto fx = project::importFixtureJson(json);
    auto built = buildGraph(fx, kSr, 1, &bank);
    REQUIRE(built.unsupported.empty());
    Engine e;
    e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(std::move(built.graph));
    if (!arrangement) { Cmd c; c.type = CmdType::ClipLaunch; c.epoch = 1; c.clipLaunch = {0, 0}; e.commands().push(c); }
    Cmd p; p.type = CmdType::TransportPlay; p.transportPlay = {uint8_t(arrangement ? 1 : 0), 0.0}; e.commands().push(p);
    const size_t total = static_cast<size_t>(secs * kSr);
    Out o; o.l.resize(total);
    std::vector<float> r(total);
    for (size_t pos = 0; pos < total; pos += 128) e.process(o.l.data() + pos, r.data() + pos, int(std::min<size_t>(128, total - pos)));
    return o;
}
float at(const Out& o, double sec) { return o.l[static_cast<size_t>(sec * kSr)]; }

}  // namespace

TEST_CASE("DerivedClip: crop, reverse, rate and gain", "[audioclip]") {
    SampleBuf b; b.sampleRate = 48000.0f; for (int i = 0; i < 1000; ++i) b.l.push_back(float(i));
    project::AudioClipData a;
    a.offset = 100.0 / 48000.0; a.dur = 200.0 / 48000.0;
    auto c = DerivedClip::build(a, b, kSr, 120);
    REQUIRE(c);
    CHECK(c->l.size() == 200);
    CHECK(c->l.front() == Approx(100.0f));
    CHECK(c->step == Approx(1.0));
    a.rev = 1; a.pitch = 12; a.gainDb = -6.0206;
    c = DerivedClip::build(a, b, kSr, 120);
    CHECK(c->l.front() == Approx(299.0f));
    CHECK(c->step == Approx(2.0));
    CHECK(c->gain == Approx(0.5f).margin(1e-3));
    a = {}; a.fadeIn = 96;  // one beat at 120 bpm = 0.5 s
    CHECK(DerivedClip::build(a, b, kSr, 120)->fadeIn == Approx(24000.0));
    CHECK_FALSE(DerivedClip::build(a, SampleBuf{}, kSr, 120));
}

TEST_CASE("DerivedClip: loop crossfade shortens the loop and is equal power", "[audioclip]") {
    SampleBuf b; b.sampleRate = 48000.0f; b.l.assign(1000, 1.0f);
    project::AudioClipData a; a.loop = 1; a.xfade = 100.0 / 48000.0;
    auto c = DerivedClip::build(a, b, kSr, 120);
    REQUIRE(c);
    CHECK(c->l.size() == 900);
    CHECK(c->l[0] == Approx(1.0f));                                   // t=0: all tail (cos 0 = 1)
    CHECK(c->l[50] == Approx(std::sin(0.25f * 3.14159265f) + std::cos(0.25f * 3.14159265f)).epsilon(1e-3));
    CHECK(c->looped);
}

TEST_CASE("session: a looped audio clip plays continuously from the launch", "[audioclip]") {
    project::SampleBank bank; bank.put("k", dc(12000, 0.5f));  // 0.25 s, looped over 2 s
    const auto o = play(audioProj(R"({"sampleId":"k","loop":1})", 384), bank, 1.5, false);
    for (double t : {0.01, 0.3, 0.7, 1.2, 1.49}) CHECK(at(o, t) == Approx(0.5f).margin(1e-4));
}

TEST_CASE("session: a one-shot plays once, then re-fires each clip length", "[audioclip]") {
    project::SampleBank bank; bank.put("k", dc(12000, 0.5f));  // 0.25 s; the clip is 384 ticks = 2 s at 120 bpm
    const auto o = play(audioProj(R"({"sampleId":"k"})", 384), bank, 4.5, false);
    CHECK(at(o, 0.1) == Approx(0.5f).margin(1e-4));
    CHECK(at(o, 0.5) == Approx(0.0f).margin(1e-6));
    CHECK(at(o, 1.9) == Approx(0.0f).margin(1e-6));
    CHECK(at(o, 2.1) == Approx(0.5f).margin(1e-4));   // fired again
    CHECK(at(o, 2.5) == Approx(0.0f).margin(1e-6));
    CHECK(at(o, 4.1) == Approx(0.5f).margin(1e-4));
}

TEST_CASE("session: pitch up an octave doubles the read rate", "[audioclip]") {
    project::SampleBank bank; bank.put("r", ramp(48000));
    const auto o = play(audioProj(R"({"sampleId":"r","pitch":12})", 384), bank, 0.5, false);
    CHECK(o.l[1000] == Approx(2000.0f).margin(0.5));
}

TEST_CASE("session: a missing sample plays silent, not an error", "[audioclip]") {
    project::SampleBank bank;
    const auto o = play(audioProj(R"({"sampleId":"nope","loop":1})", 384), bank, 0.3, false);
    for (float v : o.l) REQUIRE(v == 0.0f);
}

TEST_CASE("arrangement: a clip starts at its tick, stops at its end and fades out", "[audioclip]") {
    project::SampleBank bank; bank.put("k", dc(96000, 0.5f));  // 2 s of DC
    // starts at 192 ticks = 1 s, 192 ticks long = 1 s; fade-out 96 ticks = 0.5 s
    const std::string arr = R"(,"arr":{"x":{"trackId":"a1","start":192,"clip":{"len":192,"audio":{"sampleId":"k","fadeOut":96}}}})";
    const auto o = play(audioProj(R"({"sampleId":"k"})", 384, arr), bank, 3.0, true);
    CHECK(at(o, 0.5) == Approx(0.0f).margin(1e-6));
    CHECK(at(o, 1.2) == Approx(0.5f).margin(1e-4));
    CHECK(at(o, 1.95) == Approx(0.5f).margin(1e-4));
    CHECK(at(o, 2.25) == Approx(0.25f).margin(0.01));  // halfway through the 0.5 s fade
    CHECK(at(o, 2.6) == Approx(0.0f).margin(1e-6));
}

TEST_CASE("audio clips render identically for every callback size", "[audioclip][engine]") {
    project::SampleBank bank; bank.put("r", ramp(30000));
    const auto fx = project::importFixtureJson(audioProj(R"({"sampleId":"r","loop":1,"pitch":3.3})", 384));
    auto run = [&](int blk) {
        auto built = buildGraph(fx, kSr, 1, &bank);
        Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(std::move(built.graph));
        Cmd c; c.type = CmdType::ClipLaunch; c.epoch = 1; c.clipLaunch = {0, 0}; e.commands().push(c);
        Cmd p; p.type = CmdType::TransportPlay; p.transportPlay = {0, 0.0}; e.commands().push(p);
        std::vector<float> l(20000), r(20000);
        for (size_t pos = 0; pos < l.size(); pos += size_t(blk)) e.process(l.data() + pos, r.data() + pos, int(std::min<size_t>(size_t(blk), l.size() - pos)));
        return l;
    };
    const auto a = run(128), b = run(37), c = run(512);
    CHECK(a == b);
    CHECK(a == c);
}

TEST_CASE("audio clip playback never allocates on the audio thread", "[audioclip][rt]") {
    project::SampleBank bank; bank.put("r", ramp(30000)); bank.put("k", dc(48000, 0.3f));
    const std::string arr = R"(,"arr":{"x":{"trackId":"a1","start":96,"clip":{"len":96,"audio":{"sampleId":"k","fadeOut":48,"fadeIn":24}}},
        "y":{"trackId":"a1","start":120,"clip":{"len":200,"audio":{"sampleId":"r","loop":1}}}})";
    const auto fx = project::importFixtureJson(audioProj(R"({"sampleId":"r","loop":1})", 384, arr));
    auto built = buildGraph(fx, kSr, 1, &bank);
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(std::move(built.graph));
    std::vector<float> l(64), r(64);
    test::AllocGuard guard;
    Cmd p; p.type = CmdType::TransportPlay; p.transportPlay = {1, 0.0};
    e.commands().push(p);
    for (int i = 0; i < 2000; ++i) {
        if (i == 700) { Cmd q; q.type = CmdType::TransportPlay; q.transportPlay = {1, 50.0}; e.commands().push(q); }  // jump: resync
        e.process(l.data(), r.data(), 64);
    }
    CHECK(guard.count() == 0);
}
