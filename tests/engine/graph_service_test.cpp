// The live pipeline: GraphService (builder thread) and document::Session against an Engine that is being
// run by a real "audio" thread. Checks hand-over, coalescing, failure handling, live parameter pushes,
// rebuilds, the edit-during-rebuild race, and that the audio thread never allocates.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

#include "../AllocGuard.h"
#include "document/Session.h"
#include "engine/GraphService.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using namespace ddaw::document;
using nlohmann::json;

namespace {

constexpr double kSr = 48000.0;

std::string projJson(double level, double gainDb = 0.0, bool mute = false) {
    return R"({"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[{"id":"t1","kind":"synth",
      "inst":{"type":"stubtone","params":{"level":)" + std::to_string(level) + R"(}},"fx":[{"type":"stubgain","on":true,"params":{"gain":1.0}}],
      "gain":)" + std::to_string(gainDb) + R"(,"pan":0,"mute":)" + (mute ? "true" : "false") + R"(}],
      "clips":{"t1|s":{"len":1536,"notes":{"n":{"p":69,"s":0,"d":1500,"v":1.0}}}}}})";
}
project::Project tooManyTracks() { project::Project p; p.tracks.resize(engine::kMaxMeterTracks + 1); return p; }   // the builder refuses it
project::Project proj(double level, double gainDb = 0.0, bool mute = false) { return project::importFixtureJson(projJson(level, gainDb, mute)).project; }

// An "audio thread": runs the engine in 64-frame blocks, publishing the latest block RMS, and counts any
// allocation it performs.
struct AudioThread {
    Engine& e;
    std::atomic<bool> run{true};
    std::atomic<float> rms{0.0f};
    std::atomic<size_t> allocations{0};
    std::atomic<uint64_t> blocks{0};
    std::thread t;
    explicit AudioThread(Engine& eng) : e(eng) {
        t = std::thread([this] {
            std::vector<float> l(64), r(64);
            test::AllocGuard guard;
            while (run.load(std::memory_order_acquire)) {
                e.process(l.data(), r.data(), 64);
                double s = 0; for (float v : l) s += double(v) * v;
                rms.store(float(std::sqrt(s / 64)), std::memory_order_relaxed);
                blocks.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
            allocations.store(guard.count());
        });
    }
    ~AudioThread() { stop(); }
    void stop() { if (t.joinable()) { run = false; t.join(); } }
    // Mean block RMS over about `ms` milliseconds (the tone is a sine, so blocks vary; average many).
    float level(int ms = 60) {
        double sum = 0; int n = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) { sum += rms.load(); ++n; std::this_thread::sleep_for(std::chrono::microseconds(400)); }
        return n ? float(sum / n) : 0.0f;
    }
};

void startPlaying(Engine& e) {
    Cmd l; l.type = CmdType::ClipLaunch; l.epoch = 1; l.clipLaunch = {0, 0}; e.commands().push(l);
    Cmd p; p.type = CmdType::TransportPlay; p.transportPlay = {0, 0.0}; e.commands().push(p);
}

bool waitFor(const std::function<bool()>& f, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    return f();
}

}  // namespace

TEST_CASE("buildNow installs the first graph and publishes a matching resolver", "[service]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    GraphService svc(e, kSr);
    svc.buildNow(proj(0.1));
    CHECK(svc.publishedEpoch() == 1);
    CHECK(e.liveEpoch() == 1);
    ParamAddr a;
    REQUIRE(svc.resolver());
    CHECK(svc.resolver()->resolve("t1|mix|gain", a));
    CHECK(svc.resolver()->resolve("t1|stubgain|gain", a));
    CHECK_FALSE(svc.resolver()->resolve("t1|nope|gain", a));
    CHECK_FALSE(svc.pushParam("t1|nope|gain", 0.0f));        // unknown key: reported, nothing pushed
    CHECK_THROWS(svc.buildNow(tooManyTracks()));              // more tracks than the engine supports
}

TEST_CASE("a snapshot submitted while audio runs swaps in with no audio-thread allocation", "[service][realtime]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    GraphService svc(e, kSr);
    svc.buildNow(proj(0.1));
    startPlaying(e);
    {
        AudioThread audio(e);
        svc.start();
        const float before = audio.level();
        REQUIRE(before > 0.02f);

        svc.submit(std::make_shared<const project::Project>(proj(0.3)));     // three times louder
        REQUIRE(svc.waitForEpoch(2, 3000));
        CHECK(e.liveEpoch() == 2);
        REQUIRE(waitFor([&] { return audio.level(30) > 2.0f * before; }));
        CHECK(audio.level() == Catch::Approx(3.0f * before).epsilon(0.1));    // launched clip carried over, new level applied

        svc.submit(std::make_shared<const project::Project>(proj(0.1)));
        REQUIRE(svc.waitForEpoch(3, 3000));
        svc.stop();
        audio.stop();
        CHECK(audio.allocations.load() == 0);                                  // swaps, resolver publication, graph deletion: none on this thread
    }
    CHECK(svc.stats().builds == 3);                                            // buildNow + two submissions
}

TEST_CASE("a burst of edits coalesces into few builds, and the last one wins", "[service]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    GraphService svc(e, kSr);
    svc.buildNow(proj(0.1));
    startPlaying(e);
    AudioThread audio(e);
    svc.start();
    const float base = audio.level();
    for (int i = 1; i <= 30; ++i) svc.submit(std::make_shared<const project::Project>(proj(0.05 + 0.01 * i)));   // ends at 0.35
    REQUIRE(waitFor([&] { return !svc.busy(); }, 5000));
    const auto st = svc.stats();
    CHECK(st.submitted == 30);
    CHECK(st.coalesced > 0);
    CHECK(st.builds < 30);
    CHECK(audio.level() == Catch::Approx(3.5f * base).epsilon(0.1));          // the newest snapshot (0.35 / 0.1)
}

TEST_CASE("a build failure keeps the previous graph live and is reported", "[service]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    GraphService svc(e, kSr);
    svc.buildNow(proj(0.1));
    startPlaying(e);
    AudioThread audio(e);
    svc.start();
    const float before = audio.level();
    svc.submit(std::make_shared<const project::Project>(tooManyTracks()));     // the builder throws
    REQUIRE(waitFor([&] { return svc.stats().failures == 1; }));
    CHECK_FALSE(svc.stats().lastError.empty());
    CHECK(svc.publishedEpoch() == 1);
    CHECK(audio.level() == Catch::Approx(before).epsilon(0.1));               // still playing the old graph
    REQUIRE(waitFor([&] { return !svc.busy(); }));
}

TEST_CASE("session: parameter edits go live without a rebuild; structural edits rebuild; undo restores", "[service][session]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    GraphService svc(e, kSr);
    Document doc(proj(0.1));
    svc.buildNow(doc.project());
    startPlaying(e);
    AudioThread audio(e);
    svc.start();
    Session session(doc, svc, e);
    const float base = audio.level();
    REQUIRE(base > 0.02f);
    const auto trackUid = doc.project().tracks[0].uid;
    const auto fxUid = doc.project().tracks[0].fx[0].uid;

    // a fader move: a live SetParam, no rebuild
    const auto builds0 = svc.stats().builds;
    CHECK_FALSE(session.apply(cmd::setTrack(trackUid, "gain", -6.0206)).structural);
    REQUIRE(waitFor([&] { return audio.level(30) < 0.6f * base; }));
    CHECK(audio.level() == Catch::Approx(0.5f * base).epsilon(0.1));
    CHECK(svc.stats().builds == builds0);

    // an effect parameter (stubgain): live as well
    session.apply(cmd::setParam(fxUid, "gain", 0.5));
    REQUIRE(waitFor([&] { return audio.level(30) < 0.3f * base; }));
    CHECK(audio.level() == Catch::Approx(0.25f * base).epsilon(0.12));
    CHECK(svc.stats().builds == builds0);

    // mute is structural: a rebuild, and the new graph is silent
    CHECK(session.apply(cmd::setTrack(trackUid, "mute", true)).structural);
    REQUIRE(svc.waitForEpoch(2, 3000));
    REQUIRE(waitFor([&] { return audio.level(30) < 1e-4f; }));
    CHECK(svc.stats().builds == builds0 + 1);

    // undo the mute: back to audible, with the earlier live edits still applied (the snapshot carries them)
    session.undo();
    REQUIRE(svc.waitForEpoch(3, 3000));
    REQUIRE(waitFor([&] { return audio.level(30) > 0.1f * base; }));
    CHECK(audio.level() == Catch::Approx(0.25f * base).epsilon(0.15));
    audio.stop();
    CHECK(audio.allocations.load() == 0);
}

TEST_CASE("session: a parameter edit made while a rebuild is in flight is not lost", "[service][session]") {
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    GraphService svc(e, kSr);
    Document doc(proj(0.1));
    svc.buildNow(doc.project());
    startPlaying(e);
    AudioThread audio(e);
    svc.start();
    Session session(doc, svc, e);
    const float base = audio.level();
    const auto trackUid = doc.project().tracks[0].uid;
    const auto fxUid = doc.project().tracks[0].fx[0].uid;
    // Structural edit (starts a rebuild), then immediately a parameter edit that races its swap. Repeat with
    // different values so a lost edit cannot hide behind a coincidence. After everything settles the audio must
    // reflect the LAST edit, whichever graph it ended up on.
    for (int round = 0; round < 6; ++round) {
        session.apply(cmd::setTrack(trackUid, "pan", (round % 2) ? 0.0 : 0.01));     // pan: structural? no, live, but keeps the lane busy
        session.apply(Command{"track.set", {{"uid", trackUid}, {"field", "mute"}, {"value", round % 2 == 1}}});   // structural: rebuild
        session.apply(cmd::setParam(fxUid, "gain", 0.2 + 0.1 * round));              // races the rebuild
    }
    session.apply(Command{"track.set", {{"uid", trackUid}, {"field", "mute"}, {"value", false}}});
    session.apply(cmd::setParam(fxUid, "gain", 0.5));                                  // the last edit
    REQUIRE(waitFor([&] { return !svc.busy(); }, 8000));
    REQUIRE(waitFor([&] { return std::abs(audio.level(40) - 0.5f * base) < 0.12f * base; }, 4000));
    CHECK(audio.level() == Catch::Approx(0.5f * base).epsilon(0.12));
    audio.stop();
    CHECK(audio.allocations.load() == 0);
}
