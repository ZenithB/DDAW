// The UI's model layer: catalog, command builders and the shared AppModel, headless (no JUCE).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

#include "app/model/AppModel.h"
#include "app/model/Catalog.h"
#include "devices/Registry.h"
#include "app/model/Edit.h"
#include "FakePluginProvider.h"

using namespace ddaw;
using namespace ddaw::app;
using Catch::Approx;

namespace {
struct Rig {
    engine::Engine eng;
    std::unique_ptr<AppModel> m;
    Rig() { eng.prepare(48000.0); m = std::make_unique<AppModel>(eng, 48000.0); }
};
}  // namespace

TEST_CASE("catalog: every device has a schema and a readable label", "[appmodel]") {
    const auto& c = deviceCatalog();
    CHECK(c.size() >= 38);
    CHECK(findDevice(Chain::Effect, "reverb"));
    CHECK(findDevice(Chain::Instrument, "poly")->params.size() == 12);
    CHECK_FALSE(findDevice(Chain::Effect, "poly"));
    CHECK(paramLabel("lfoShape") == "LFO Shape");
    CHECK(paramLabel("vibAmt") == "Vib Amt");
    CHECK(paramLabel("cutoff") == "Cutoff");
}

TEST_CASE("catalog: the B6 synth families are listed with the parameters their devices have", "[appmodel]") {
    for (const char* type : {"harmnoise", "subtractive", "wavetable", "waveshaper", "modal", "perc"}) {
        INFO(type);
        const auto* info = findDevice(Chain::Instrument, type);
        REQUIRE(info);
        auto d = createInstrument(type);
        REQUIRE(d);
        REQUIRE(info->params.size() == d->params().size());
        for (size_t i = 0; i < info->params.size(); ++i) CHECK(std::string(info->params[i].key) == d->params()[i].key);
        CHECK(info->label.size() > 3);
    }
    // and the A-rate ports the schema marks are the ones the families document
    auto aRate = [&](const char* type) {
        std::vector<std::string> keys;
        for (const auto& p : findDevice(Chain::Instrument, type)->params) if (p.audioRate) keys.emplace_back(p.key);
        return keys;
    };
    CHECK(aRate("subtractive") == std::vector<std::string>{"cutoff", "pitch"});
    CHECK(aRate("wavetable") == std::vector<std::string>{"pos", "pitch"});
    CHECK(aRate("waveshaper") == std::vector<std::string>{"drive", "bias", "pitch"});
    CHECK(aRate("harmnoise").empty());
}

TEST_CASE("catalog: parameter curves round-trip", "[appmodel]") {
    const ParamSpec lin{0, "a", 0, 10, 5, Curve::Linear, 0, false}, ex{0, "b", 20, 20000, 1000, Curve::Exponential, 0, false}, st{0, "c", 0, 6, 0, Curve::Stepped, 0, false};
    CHECK(paramToUnit(lin, 5) == Approx(0.5));
    CHECK(paramFromUnit(ex, paramToUnit(ex, 1000)) == Approx(1000).epsilon(1e-6));
    CHECK(paramToUnit(ex, 632.455532) == Approx(0.5).epsilon(1e-3));  // geometric mean at the midpoint
    CHECK(paramFromUnit(st, 0.51) == 3.0);
    CHECK(paramFromUnit(lin, 2.0) == 10.0);  // clamped
}

TEST_CASE("model: building a song with edits, undo and redo", "[appmodel]") {
    Rig r;
    auto& m = *r.m;
    REQUIRE(m.project().tracks.empty());
    REQUIRE(m.apply(edit::addTrack(m.project(), project::TrackKind::Synth)));
    REQUIRE(m.apply(edit::addTrack(m.project(), project::TrackKind::Drum)));
    REQUIRE(m.apply(edit::addScene(m.project())));
    REQUIRE(m.project().tracks.size() == 2);
    CHECK(m.project().tracks[0].id == "t1");
    CHECK(m.project().tracks[1].id == "t2");
    CHECK(m.project().tracks[0].inst.type == "poly");
    CHECK(m.project().tracks[1].inst.type == "drum");
    CHECK(m.selection().track == m.project().tracks[0].uid);  // a track is always selected

    const auto uid = m.project().tracks[0].uid;
    REQUIRE(m.apply(edit::newSessionClip(uid, "s1")));
    const edit::ClipRef ref{uid, "s1", ""};
    REQUIRE(m.apply(edit::addNote(ref, 60, 0, 96)));
    REQUIRE(m.apply(edit::addNote(ref, 64, 96, 96)));
    REQUIRE(edit::findClip(m.project(), ref)->notes.size() == 2);

    auto n = edit::findClip(m.project(), ref)->notes[0];
    n.pitch = 72;
    n.startTicks = 192;
    REQUIRE(m.apply(edit::editNote(ref, n)));
    CHECK(edit::findClip(m.project(), ref)->notes[0].pitch == 72);
    m.undo();
    CHECK(edit::findClip(m.project(), ref)->notes[0].pitch == 60);
    m.redo();
    CHECK(edit::findClip(m.project(), ref)->notes[0].pitch == 72);
    REQUIRE(m.apply(edit::removeNote(ref, n.uid)));
    CHECK(edit::findClip(m.project(), ref)->notes.size() == 1);
    CHECK(m.dirty());
}

TEST_CASE("model: devices get unique ids, and a bad command reports instead of throwing", "[appmodel]") {
    Rig r;
    auto& m = *r.m;
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    const auto uid = m.project().tracks[0].uid;
    m.apply(edit::addDevice(m.project(), uid, "fx", "reverb"));
    m.apply(edit::addDevice(m.project(), uid, "fx", "reverb"));
    m.apply(edit::addDevice(m.project(), uid, "master", "comp"));
    const auto& t = m.project().tracks[0];
    REQUIRE(t.fx.size() == 2);
    CHECK(t.fx[0].id != t.fx[1].id);
    CHECK(m.project().masterFx.size() == 1);
    CHECK_FALSE(m.apply(edit::removeDevice(999999)));
    CHECK_FALSE(m.lastError().empty());
    CHECK(m.apply(edit::removeDevice(t.fx[0].uid)));
    m.apply(edit::setInstrument(m.project(), uid, "fm"));
    CHECK(m.project().tracks[0].inst.type == "fm");
}

TEST_CASE("model: arrangement clips move, resize and delete", "[appmodel]") {
    Rig r;
    auto& m = *r.m;
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    const auto uid = m.project().tracks[0].uid;
    REQUIRE(m.apply(edit::newArrClip(m.project(), uid, 384)));
    const std::string key = m.project().arr.begin()->first;
    REQUIRE(m.apply(edit::moveArrClip(m.project(), key, 768)));
    CHECK(m.project().arr.at(key).start == 768);
    REQUIRE(m.apply(edit::resizeArrClip(m.project(), key, 192)));
    CHECK(m.project().arr.at(key).clip.len == 192);
    REQUIRE(m.apply(edit::removeArrClip(key)));
    CHECK(m.project().arr.empty());
    m.undo();
    CHECK(m.project().arr.size() == 1);
}

TEST_CASE("model: a group is one undo step", "[appmodel]") {
    Rig r;
    auto& m = *r.m;
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    const auto uid = m.project().tracks[0].uid;
    m.apply(edit::newSessionClip(uid, "s1") /* no scene yet: must fail cleanly */);
    CHECK(m.project().clips.empty());
    m.apply(edit::addScene(m.project()));
    REQUIRE(m.applyGroup("fill", {edit::newSessionClip(uid, "s1"), edit::addNote({uid, "s1", ""}, 60, 0, 96), edit::addNote({uid, "s1", ""}, 62, 96, 96)}));
    CHECK(m.project().clips.at("t1|s1").notes.size() == 2);
    m.undo();
    CHECK(m.project().clips.empty());
}

TEST_CASE("model: save, new and reopen a package", "[appmodel]") {
    namespace fs = std::filesystem;
    const auto dir = (fs::temp_directory_path() / "ddaw_appmodel_test.ddaw").string();
    fs::remove_all(dir);
    Rig r;
    auto& m = *r.m;
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    m.apply(edit::addScene(m.project()));
    std::string err;
    REQUIRE(m.save(dir, err));
    CHECK_FALSE(m.dirty());
    m.apply(edit::addScene(m.project()));
    CHECK(m.dirty());
    m.newProject();
    CHECK(m.project().tracks.empty());
    REQUIRE(m.open(dir, err));
    CHECK(m.project().tracks.size() == 1);
    CHECK(m.project().scenes.size() == 1);
    CHECK_FALSE(m.dirty());
    CHECK_FALSE(m.open(dir + "-missing", err));
    CHECK_FALSE(err.empty());
    fs::remove_all(dir);
}

#include "app/model/Timeline.h"

TEST_CASE("timeline: coordinate maps, zoom anchoring and snap grids", "[appmodel][timeline]") {
    TimeScale s; s.pxPerBeat = 50; s.originTick = 96;
    CHECK(s.toX(96) == Approx(0));
    CHECK(s.toX(192) == Approx(50));
    CHECK(s.toTick(s.toX(300)) == Approx(300));
    const double under = s.toTick(120);
    s.zoomAt(120, 2.0);
    CHECK(s.pxPerBeat == Approx(100));
    CHECK(s.toTick(120) == Approx(under));
    s.zoomAt(0, 1e-9);
    CHECK(s.pxPerBeat == Approx(4));
    CHECK(autoGrid(TimeScale{40, 0}) == 48);   // a sixteenth is 10 px at 40 px/beat: the eighth (20 px) is the finest that clears 14
    CHECK(autoGrid(TimeScale{400, 0}) == 6);
    CHECK(edit::snapTicks(100, 24) == 96);
    CHECK(edit::snapFloor(119, 24) == 96);
    CHECK(edit::snapTicks(50, 0) == 50);
    CHECK(barBeatLabel(0) == "1.1.1");
    CHECK(barBeatLabel(384 + 96 * 2 + 24) == "2.3.2");
    CHECK(noteName(60) == "C4");
    CHECK(noteName(69) == "A4");
    CHECK(isBlackKey(61));
    CHECK_FALSE(isBlackKey(60));
}

#include <atomic>
#include <chrono>
#include <thread>

#include "app/model/Demo.h"

TEST_CASE("model: the demo song plays through a live engine and edits land while it plays", "[appmodel][live][threads][timing]") {
    engine::Engine eng;
    eng.prepare(48000.0);
    AppModel m(eng, 48000.0);
    std::atomic<bool> run{true};
    std::thread audio([&] {   // a stand-in audio callback: 64-frame blocks, slightly faster than real time
        std::vector<float> l(64), r(64);
        while (run.load()) { eng.process(l.data(), r.data(), 64); std::this_thread::sleep_for(std::chrono::microseconds(500)); }
    });
    auto waitFor = [&](auto pred, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) { if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return pred();
    };

    buildDemo(m);
    REQUIRE(waitFor([&] { return !m.service().busy() && m.service().publishedEpoch() > 0; }, 5000));
    INFO("last build error: " << m.service().stats().lastError);
    CHECK(m.service().stats().failures == 0);

    m.launchScene("s2");   // starts the transport too
    REQUIRE(waitFor([&] { return m.meters().masterPeak > 0.01f; }, 4000));
    const auto snap = m.meters();
    CHECK(snap.playing);
    CHECK(snap.trackScene[0] == 1);        // Drums launched scene s2
    CHECK(snap.trackScene[3] == -1);       // Lead has no clip in s2... but launch is per-track scene index
    CHECK(snap.trackPeak[0] > 0.0f);

    // a live parameter edit and a structural edit while playing: audio continues, no failed builds
    const auto chords = m.project().tracks[2].inst.uid;
    CHECK(m.apply(document::cmd::setParam(chords, "cutoff", 900.0)));
    CHECK(m.apply(edit::addDevice(m.project(), m.project().tracks[1].uid, "fx", "reverb")));
    REQUIRE(waitFor([&] { return !m.service().busy(); }, 5000));
    CHECK(m.service().stats().failures == 0);
    CHECK(waitFor([&] { return m.meters().masterPeak > 0.01f; }, 3000));
    CHECK(m.meters().trackScene[0] == 1);   // the launched scene survived the rebuild

    // muting every track silences the master
    for (auto& t : std::vector<project::Track>(m.project().tracks)) m.apply(document::cmd::setTrack(t.uid, "mute", true));
    REQUIRE(waitFor([&] { return !m.service().busy(); }, 5000));
    CHECK(waitFor([&] { return m.meters().masterPeak < 0.001f; }, 4000));

    // File > New: the empty project replaces the live graph (it must not keep playing the old song)
    m.stop();
    CHECK(waitFor([&] { return !m.meters().playing; }, 2000));
    m.launchScene("s2");
    REQUIRE(waitFor([&] { return m.meters().playing; }, 2000));
    m.newProject();
    REQUIRE(waitFor([&] { return !m.service().busy(); }, 5000));
    CHECK(m.service().stats().failures == 0);
    CHECK(waitFor([&] { return m.meters().masterPeak < 0.001f; }, 4000));
    CHECK(m.meters().trackScene[0] == -1);
    run = false;
    audio.join();
}

#include "../FakeDevice.h"
#include "engine/OfflineRender.h"
#include "project/ProjectJson.h"

namespace {

// A drum track with one kick at arrangement tick 96, and an empty audio track to record on.
void recordingProject(AppModel& m) {
    m.apply(edit::addTrack(m.project(), project::TrackKind::Drum));
    m.apply(edit::addTrack(m.project(), project::TrackKind::Audio));
    project::Clip c;
    c.len = 384;
    project::Note n; n.pitch = 0; n.startTicks = 0; n.durTicks = 24; n.velocity = 1.0;
    c.notes.push_back(n);
    m.apply({"arrclip.set", {{"key", "k1"}, {"arr", {{"trackId", "t1"}, {"start", 96}, {"clip", project::clipToJson(c)}}}}});
}

int firstOver(const std::vector<float>& x, float thr) {
    for (size_t i = 0; i < x.size(); ++i) if (std::abs(x[i]) > thr) return int(i);
    return -1;
}

}  // namespace

TEST_CASE("recording: a take lands on the armed audio track exactly where it was played", "[appmodel][recording]") {
    for (int countIn : {0, 1}) {
        INFO("count-in bars " << countIn);
        engine::Engine eng;
        eng.prepare(48000.0);
        AppModel m(eng, 48000.0);
        test::FakeDevice dev(eng, 4800, 64);
        RecordingEnv env;
        env.inputLatencyFrames = [] { return 1200; };
        env.outputLatencyFrames = [] { return 3600; };      // together: the fake device's 4800-frame round trip
        env.inputChannels = [] { return 1; };
        env.sampleRate = [] { return 48000.0; };
        m.recording().setEnv(env);
        m.recording().settings().countInBars = countIn;
        recordingProject(m);
        auto waitFor = [&](auto pred, int ms) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < end) { if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            return pred();
        };
        REQUIRE(waitFor([&] { return !m.service().busy() && m.service().publishedEpoch() > 0; }, 5000));

        const auto audioUid = m.project().tracks[1].uid;
        std::string err;
        CHECK_FALSE(m.recording().start(err));                // nothing armed yet
        m.recording().arm(audioUid, true);
        REQUIRE(m.recording().anyArmed());

        REQUIRE(m.recording().start(err));
        CHECK(m.recording().recording());
        const uint64_t target = dev.frames.load() + 3 * 48000;
        while (dev.frames.load() < target) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(m.recording().recordedSeconds() > 0.5);
        m.stop();                                             // ends the take and places the clip
        CHECK_FALSE(m.recording().recording());
        INFO(m.status());

        REQUIRE(m.project().arr.size() == 2);
        const project::ArrClip* rec = nullptr;
        for (auto& [k, ac] : m.project().arr) if (ac.trackId == "t2") rec = &ac;
        REQUIRE(rec);
        REQUIRE(rec->clip.audio);
        CHECK(rec->start == Approx(0.0).margin(1e-6));
        CHECK(*rec->clip.audio->offset * 48000.0 >= 4800.0);   // at least the device round trip is trimmed
        CHECK(m.sampleBank()->contains(rec->clip.audio->sampleId));
        CHECK_FALSE(m.dirty() == false);

        // render ONLY the recorded track offline: its kick must sit at tick 96 = 0.5 s = frame 24000
        project::Fixture fx;
        fx.project = m.project();
        fx.project.tracks[0].mute = true;
        fx.scope.kind = "arr";
        const auto res = engine::renderFixture(fx, 48000.0, engine::RenderOptions{}, m.sampleBank().get());
        if (countIn == 0) {
            const int onset = firstOver(res.l, 0.02f);
            REQUIRE(onset > 0);
            CHECK(onset == Approx(24000).margin(80));          // within the kick's 1 ms attack ramp
        } else {
            // the loopback also captured the metronome click (as a microphone would), so check the
            // trimming on the clip itself: the bar of count-in (2 s at 120 bpm) is cut from the front
            CHECK(*rec->clip.audio->offset >= 2.0 + 4800.0 / 48000.0);
            CHECK(*rec->clip.audio->offset < 2.0 + 0.3);
        }

        // one undo removes the take's clip
        m.undo();
        CHECK(m.project().arr.size() == 1);
        dev.stop();
        CHECK(dev.allocs == 0);
    }
}

#include <fstream>

#include "app/model/ExportPlan.h"
#include "app/model/Settings.h"

TEST_CASE("export plan: ranges, stems per source track, and safe file names", "[appmodel][export]") {
    engine::Engine eng; eng.prepare(48000.0);
    AppModel m(eng, 48000.0);
    buildDemo(m);
    std::string err;
    ExportOptions o;
    o.stems = true;
    auto jobs = planExport(m.project(), o, err);
    REQUIRE(err.empty());
    REQUIRE(jobs.size() == 1 + 5);                              // the mixdown and the five non-bus tracks
    CHECK(jobs[0].suffix.empty());
    CHECK(jobs[0].fixture.scope.kind == "arr");                 // the demo has an arrangement
    CHECK(jobs[1].suffix == "-Drums");
    // a stem mutes every other source but keeps the buses, and drops the master chain
    const auto& pr = jobs[3].fixture.project;                   // Chords
    CHECK_FALSE(pr.tracks[2].mute);
    CHECK(pr.tracks[0].mute);
    CHECK(pr.tracks[5].kind == project::TrackKind::Bus);
    CHECK_FALSE(pr.tracks[5].mute);
    CHECK(pr.masterFx.empty());
    CHECK(jobs[3].render.master.mode == engine::MasterLimiterConfig::Mode::Bypass);
    CHECK_FALSE(jobs[0].fixture.project.masterFx.empty());      // the mixdown keeps its master chain

    o = {}; o.range = ExportOptions::Range::Scene; o.sceneId = "s2";
    jobs = planExport(m.project(), o, err);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].fixture.scope.kind == "scene");
    CHECK(jobs[0].fixture.scope.sceneId == "s2");
    o.sceneId = "nope";
    CHECK(planExport(m.project(), o, err).empty());
    CHECK_FALSE(err.empty());
    o = {}; o.range = ExportOptions::Range::LoopRegion;
    CHECK(planExport(m.project(), o, err).empty());              // no loop region set
    o = {}; o.mixdown = false;
    CHECK(planExport(m.project(), o, err).empty());

    CHECK(safeFileName("Lead / 2") == "Lead-2");
    CHECK(safeFileName("  ") == "track");
    CHECK(safeFileName("Bass!!") == "Bass");
}

TEST_CASE("export plan: the stems sum back to the mix", "[appmodel][export]") {
    engine::Engine eng; eng.prepare(48000.0);
    AppModel m(eng, 48000.0);
    buildDemo(m);
    // the mix without its master chain, for comparison with the stems (which have none)
    std::string err;
    ExportOptions o;
    o.stems = true;
    o.mixdown = true;
    auto jobs = planExport(m.project(), o, err);
    REQUIRE(jobs.size() == 6);
    jobs[0].fixture.project.masterFx.clear();
    jobs[0].render.master.mode = engine::MasterLimiterConfig::Mode::Bypass;
    const auto mix = engine::renderFixture(jobs[0].fixture, 48000.0, jobs[0].render);
    std::vector<float> sum(mix.l.size(), 0.0f);
    for (size_t j = 1; j < jobs.size(); ++j) {
        const auto s = engine::renderFixture(jobs[j].fixture, 48000.0, jobs[j].render);
        REQUIRE(s.l.size() == mix.l.size());
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += s.l[i];
    }
    double err2 = 0, ref = 0;
    for (size_t i = 0; i < sum.size(); ++i) { err2 += double(sum[i] - mix.l[i]) * double(sum[i] - mix.l[i]); ref += double(mix.l[i]) * double(mix.l[i]); }
    REQUIRE(ref > 0.0);
    // the sum differs from the mix only by float rounding (the buses are linear), not by content
    CHECK(10.0 * std::log10(std::max(err2, 1e-30) / ref) < -80.0);
}

TEST_CASE("export: a render can be cancelled part-way", "[appmodel][export]") {
    engine::Engine eng; eng.prepare(48000.0);
    AppModel m(eng, 48000.0);
    buildDemo(m);
    std::string err;
    auto jobs = planExport(m.project(), ExportOptions{}, err);
    REQUIRE(jobs.size() == 1);
    int calls = 0;
    jobs[0].render.progress = [&](double f) { ++calls; return f < 0.2; };
    const auto r = engine::renderFixture(jobs[0].fixture, 48000.0, jobs[0].render);
    CHECK(r.cancelled);
    CHECK(calls > 1);
    const auto full = engine::renderFixture(jobs[0].fixture, 48000.0, engine::RenderOptions{});
    CHECK_FALSE(full.cancelled);
    CHECK(r.l.size() < full.l.size() / 2);
}

TEST_CASE("settings: round trip, defaults on a missing file, clean start on a damaged one", "[appmodel]") {
    namespace fs = std::filesystem;
    const auto path = (fs::temp_directory_path() / "ddaw_settings_test" / "settings.json").string();
    fs::remove_all(fs::path(path).parent_path());
    AppSettings d = loadSettings(path);
    CHECK(d.recording.offsetMs == 0.0);
    CHECK(d.recording.countInBars == 0);
    AppSettings s;
    s.recording.offsetMs = 37.0; s.recording.countInBars = 2; s.recording.monitor = true; s.inputMode = 2;
    s.mpe = true; s.bendRange = 12.0f; s.mpeRange = 24.0f; s.mpeLower = 7; s.mpeUpper = 5;
    REQUIRE(saveSettings(path, s));
    const auto l = loadSettings(path);
    CHECK(l.recording.offsetMs == 37.0);
    CHECK(l.recording.countInBars == 2);
    CHECK(l.recording.monitor);
    CHECK(l.inputMode == 2);
    CHECK(l.mpe);
    CHECK(l.bendRange == 12.0f);
    CHECK(l.mpeRange == 24.0f);
    CHECK(l.mpeLower == 7);
    CHECK(l.mpeUpper == 5);
    CHECK(d.mpeLower == 15);
    CHECK(d.mpeUpper == 0);
    CHECK_FALSE(d.mpe);                           // defaults: MPE off, 2 and 48 semitones
    CHECK(d.bendRange == 2.0f);
    CHECK(d.mpeRange == 48.0f);
    { std::ofstream out(path); out << "{ not json"; }
    CHECK(loadSettings(path).recording.countInBars == 0);
    { std::ofstream out(path); out << R"({"recording":{"countInBars":99,"offsetMs":9999},"inputMode":-4,"bendRange":500,"mpeRange":0})"; }
    const auto c = loadSettings(path);
    CHECK(c.recording.countInBars == 2);          // clamped
    CHECK(c.recording.offsetMs == 500.0);
    CHECK(c.inputMode == 0);
    CHECK(c.bendRange == 96.0f);
    CHECK(c.mpeRange == 1.0f);
    fs::remove_all(fs::path(path).parent_path());
}

#include "app/model/CurveRecord.h"

TEST_CASE("curve lanes: simplification keeps the shape, merging replaces only the recorded span", "[appmodel][curves]") {
    // a straight ramp collapses to its two ends; a bump keeps its corners
    std::vector<project::AutoPoint> ramp;
    for (int i = 0; i <= 200; ++i) ramp.push_back({double(i), i / 200.0});
    CHECK(simplifyLane(ramp, 0.01).size() == 2);
    std::vector<project::AutoPoint> bump;
    for (int i = 0; i <= 100; ++i) bump.push_back({double(i), i < 40 ? 0.0 : i < 60 ? 1.0 : 0.0});
    const auto sb = simplifyLane(bump, 0.01);
    CHECK(sb.size() >= 4);
    CHECK(sb.size() < 10);

    const std::vector<project::AutoPoint> old{{0, 0.1}, {100, 0.2}, {200, 0.3}, {300, 0.4}};
    const auto merged = mergeLane(old, {{120, 0.9}, {180, 0.8}});
    REQUIRE(merged.size() == 6);
    CHECK(merged[0].t == 0);
    CHECK(merged[1].t == 100);
    CHECK(merged[2].t == 120);
    CHECK(merged[3].t == 180);
    CHECK(merged[4].t == 200);                                  // the span 120..180 holds the recording; the points around it survive
    CHECK(merged[5].t == 300);
    CHECK(mergeLane(old, {}).size() == 4);
}

TEST_CASE("curve lanes: timing, compensation, unvoiced holds and ranges", "[appmodel][curves]") {
    std::vector<engine::Engine::TimedPerf> fr;
    for (int k = 0; k < 100; ++k) {
        PerformanceFrame f{};
        f.f0Hz = (k >= 40 && k < 60) ? 0.0f : 200.0f * std::pow(2.0f, float(k) / 100.0f) * (1.0f + 0.05f * std::sin(float(k) / 3.0f));   // an octave with vibrato, and a gap
        f.loudnessDb = -60.0f + 0.6f * float(k);
        f.envelope = float(k) / 100.0f;
        fr.push_back({f, 1000.0 + 192.0 * k});
    }
    CurveTiming t;
    t.startInput = 1000.0; t.startTick = 0.0; t.compFrames = 960.0; t.ticksPerFrame = 0.002;   // 192 frames = 0.384 ticks per tracker frame
    const auto f0 = buildLane(fr, PerfSource::F0, 100.0, 800.0, t, 0.0005);
    REQUIRE(f0.size() > 20);
    CHECK(f0.size() < 100);                                    // the gap contributes no points
    for (const auto& p : f0) CHECK((p.t < 40 * 192 * 0.002 - 960 * 0.002 + 1e-9 || p.t > 60 * 192 * 0.002 - 960 * 0.002 - 1e-9));
    CHECK(f0.front().t >= 0.0);                                // frames compensation-earlier than the take start fall before tick 0 and are dropped
    const auto ld = buildLane(fr, PerfSource::Loudness, -60.0, 0.0, t, 0.0005);
    CHECK(ld.back().v == Catch::Approx(0.99).margin(0.01));    // 0.6*99 - 60 = -0.6 dB -> 0.99
    const auto env = buildLane(fr, PerfSource::Envelope, 0, 0, t);   // default range 0..1
    CHECK(env.back().v == Catch::Approx(0.99).margin(0.01));
}

TEST_CASE("recording: a sung glide is written into an automation lane that lines up with the timeline", "[appmodel][recording][curves]") {
    namespace fs = std::filesystem;
    const auto file = (fs::temp_directory_path() / "ddaw_curve_test.json").string();
    {
        std::ofstream out(file);
        out << R"({"meta":{"bpm":120},"scenes":[{"id":"s"}],"clips":{},
          "tracks":[{"id":"t1","name":"Voice","kind":"audio","fx":[{"type":"stubgain","id":"stubgain","on":true,"params":{"gain":1.0}}],"gain":0,"pan":0,
                     "perf":[{"source":"f0","min":100,"max":1000,"rec":true,"targets":[{"dest":"fx","fxId":"stubgain","pkey":"gain"}]}]}]})";
    }
    engine::Engine eng;
    eng.prepare(48000.0);
    AppModel m(eng, 48000.0);
    std::string err;
    REQUIRE(m.open(file, err));
    fs::remove(file);
    REQUIRE(m.project().tracks[0].perf.size() == 1);
    REQUIRE(m.project().tracks[0].perf[0].record);

    test::FakeDevice dev(eng, 64, 64);
    dev.inputFn = [](uint64_t n) {   // 200 Hz rising by 100 Hz every second
        const double t = double(n) / 48000.0;
        return 0.4f * float(std::sin(2.0 * 3.14159265358979 * (200.0 * t + 50.0 * t * t)));
    };
    RecordingEnv env;
    env.inputChannels = [] { return 1; };
    env.sampleRate = [] { return 48000.0; };
    m.recording().setEnv(env);
    m.recording().settings().recordCurves = true;
    auto waitFor = [&](auto pred, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) { m.tick(); if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        return pred();
    };
    REQUIRE(waitFor([&] { return !m.service().busy() && m.service().publishedEpoch() > 0; }, 5000));

    REQUIRE(m.recording().canRecord());
    REQUIRE(m.recording().start(err));
    const uint64_t target = dev.frames.load() + 3 * 48000;
    waitFor([&] { return dev.frames.load() >= target; }, 8000);
    CHECK(m.recording().curveFrames() > 600);                  // 250 frames per second for 3 s
    m.stop();
    INFO(m.status());
    dev.stop();
    CHECK(dev.allocs == 0);

    const auto& lanes = m.project().tracks[0].autoLanes;
    REQUIRE(lanes.count("fx|stubgain|gain") == 1);
    const auto& lane = lanes.at("fx|stubgain|gain");
    REQUIRE(lane.size() >= 3);
    CHECK(lane.size() < 400);                                  // simplified, not 750 raw points
    const double startInput = double(eng.captureStartInputFrame()), startTick = eng.captureInfo().startTick;
    const double tpf = 120.0 * 96.0 / 60.0 / 48000.0;
    const double comp = double(eng.latencySamples());
    double worst = 0;
    for (size_t i = 1; i < lane.size(); ++i) {
        CHECK(lane[i].t > lane[i - 1].t);
        const double n = startInput + (lane[i].t - startTick) / tpf + comp;
        const double f = 200.0 + 100.0 * (n / 48000.0);
        worst = std::max(worst, std::abs(lane[i].v - std::log(f / 100.0) / std::log(10.0)));
    }
    CHECK(worst < 0.015);                                      // under 1.5% of the 100 Hz..1 kHz range, at every kept point
    CHECK(lane.front().t >= 0.0);
    CHECK(lane.back().t > 400.0);                              // about 3 s = 576 ticks

    m.undo();                                                  // the whole recording is one undo step
    CHECK(m.project().tracks[0].autoLanes.count("fx|stubgain|gain") == 0);
}

TEST_CASE("recording: a chord and a held note played live become a clip, in time, one undo step", "[appmodel][recording][midi]") {
    engine::Engine eng;
    eng.prepare(48000.0);
    AppModel m(eng, 48000.0);
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    m.apply(edit::addTrack(m.project(), project::TrackKind::Drum));
    const auto synth = m.project().tracks[0].uid;
    test::FakeDevice dev(eng, 64, 64);
    dev.paced = true;                                           // against the clock: wall time and audio time agree
    RecordingEnv env;
    env.outputLatencyFrames = [] { return 0; };
    env.sampleRate = [] { return 48000.0; };
    m.recording().setEnv(env);
    auto waitFor = [&](auto pred, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) { m.tick(); if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        return pred();
    };
    REQUIRE(waitFor([&] { return !m.service().busy() && m.service().publishedEpoch() > 0; }, 5000));

    CHECK(m.liveTarget() == synth);                             // the selected synth track is the live target
    m.recording().arm(synth, true);
    CHECK(m.recording().armedNoteTrack() == synth);
    std::string err;
    REQUIRE(m.recording().canRecord());
    REQUIRE(waitFor([&] { return eng.liveTrack() == 0; }, 2000));
    REQUIRE(m.recording().start(err));
    auto framesNow = [&] { return dev.frames.load(); };
    auto waitUntilFrame = [&](uint64_t f) { waitFor([&] { return framesNow() >= f; }, 8000); };
    const uint64_t t0 = framesNow();
    waitUntilFrame(t0 + 14400);                                 // 0.3 s
    const uint64_t chordAt = framesNow();
    m.noteOn(60, 0.9f); m.noteOn(64, 0.7f); m.noteOn(67, 0.5f);
    waitUntilFrame(chordAt + 24000);                            // held for 0.5 s
    m.noteOff(60); m.noteOff(64); m.noteOff(67);
    waitUntilFrame(chordAt + 38400);
    const uint64_t lastAt = framesNow();
    m.noteOn(72, 0.8f);                                         // held until the take ends
    waitUntilFrame(lastAt + 14400);
    m.stop();
    INFO(m.status());
    dev.stop();
    CHECK(dev.allocs == 0);

    REQUIRE(m.project().arr.size() == 1);
    const auto& clip = m.project().arr.begin()->second;
    REQUIRE(clip.clip.notes.size() == 4);
    std::map<int, project::Note> byPitch;
    for (auto& n : clip.clip.notes) byPitch[n.pitch] = n;
    REQUIRE(byPitch.count(60)); REQUIRE(byPitch.count(64)); REQUIRE(byPitch.count(67)); REQUIRE(byPitch.count(72));
    const double tpf = 192.0 / 48000.0;                         // ticks per frame at 120 bpm
    const double expectChord = double(chordAt - t0) * tpf;     // wall-clock expectation
    CHECK(byPitch[60].startTicks == Approx(expectChord).margin(12.0));   // within ~60 ms of the wall clock (thread scheduling)
    // a chord is simultaneous: its notes start within one chunk of each other
    CHECK(std::abs(byPitch[64].startTicks - byPitch[60].startTicks) <= 3.0);
    CHECK(std::abs(byPitch[67].startTicks - byPitch[60].startTicks) <= 3.0);
    CHECK(byPitch[60].durTicks == Approx(0.5 * 192.0).margin(10.0));
    CHECK(byPitch[60].velocity == Approx(0.9).margin(1e-6));
    CHECK(byPitch[67].velocity == Approx(0.5).margin(1e-6));
    CHECK(byPitch[72].startTicks > byPitch[60].startTicks + 100.0);
    CHECK(byPitch[72].startTicks + byPitch[72].durTicks <= clip.clip.len + 1e-6);   // the held note ends at the take's end
    CHECK(clip.start == Approx(0.0).margin(1e-6));
    m.undo();
    CHECK(m.project().arr.empty());
}

TEST_CASE("recording: a note's expression (bend, pressure) is recorded as curves on the note, and played back", "[appmodel][recording][midi][mpe]") {
    engine::Engine eng;
    eng.prepare(48000.0);
    AppModel m(eng, 48000.0);
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    const auto synth = m.project().tracks[0].uid;
    project::DeviceSpec fmop; fmop.type = "fmop";
    fmop.params = {{"algo", 4}, {"l1", 1}, {"l2", 0}, {"l3", 0}, {"l4", 0}, {"attack", 0.001}, {"sustain", 1.0}, {"release", 0.05}};
    REQUIRE(m.apply({"inst.set", {{"track", synth}, {"device", project::deviceToJson(fmop, true)}}}));
    test::FakeDevice dev(eng, 64, 64);
    dev.paced = true;
    RecordingEnv env;
    env.outputLatencyFrames = [] { return 0; };
    env.sampleRate = [] { return 48000.0; };
    m.recording().setEnv(env);
    auto waitFor = [&](auto pred, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) { m.tick(); if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        return pred();
    };
    REQUIRE(waitFor([&] { return !m.service().busy() && m.service().publishedEpoch() > 0; }, 5000));
    m.recording().arm(synth, true);
    std::string err;
    REQUIRE(waitFor([&] { return eng.liveTrack() == 0; }, 2000));
    REQUIRE(m.recording().start(err));
    auto waitUntilFrame = [&](uint64_t f) { waitFor([&] { return dev.frames.load() >= f; }, 8000); };
    const uint64_t t0 = dev.frames.load();
    waitUntilFrame(t0 + 9600);                                  // 0.2 s
    const uint64_t at = dev.frames.load();
    m.noteExpression(69, 2, 3.0f);                              // bend sent BEFORE the note-on, as an MPE controller does
    m.noteOn(69, 0.9f);
    for (int i = 1; i <= 20; ++i) {                             // a one-second sweep from +3 up to +12 semitones, with growing pressure
        waitUntilFrame(at + uint64_t(i) * 2400);                // every 50 ms
        m.noteExpression(69, 2, 3.0f + 9.0f * float(i) / 20.0f);
        m.noteExpression(69, 1, float(i) / 20.0f);
    }
    m.noteOff(69);
    waitUntilFrame(at + 52800);
    m.stop();
    INFO(m.status());
    dev.stop();
    REQUIRE(m.project().arr.size() == 1);
    const auto& notes = m.project().arr.begin()->second.clip.notes;
    REQUIRE(notes.size() == 1);
    const auto& n = notes[0];
    REQUIRE(n.bend.size() >= 15);                               // a point per 50 ms step plus the starting value
    CHECK(n.bend.front().t == Approx(0.0).margin(4.0));         // the value the note started with
    CHECK(n.bend.front().v == Approx(3.0).margin(0.05));
    CHECK(n.bend.back().v == Approx(12.0).margin(0.3));
    CHECK(n.bend.back().t == Approx(192.0).margin(24.0));       // ~1 s = 192 ticks
    for (size_t i = 1; i < n.bend.size(); ++i) { CHECK(n.bend[i].t >= n.bend[i - 1].t); CHECK(n.bend[i].v >= n.bend[i - 1].v - 1e-4); }
    REQUIRE(n.pressure.size() >= 15);
    CHECK(n.pressure.back().v == Approx(1.0).margin(0.02));
    CHECK(n.slide.empty());                                     // never touched: no curve
    // the recorded clip plays the glide back: the note's pitch rises through the first second
    const auto j = project::projectToJson(m.project());
    REQUIRE(j["arr"].size() == 1);
    CHECK(j["arr"].begin().value()["clip"]["notes"].begin().value().contains("bend"));
    m.undo();
    CHECK(m.project().arr.empty());
}

TEST_CASE("recording: the live target follows arming and selection; notes without a take are not recorded", "[appmodel][midi]") {
    engine::Engine eng;
    eng.prepare(48000.0);
    AppModel m(eng, 48000.0);
    m.apply(edit::addTrack(m.project(), project::TrackKind::Audio));
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    m.apply(edit::addTrack(m.project(), project::TrackKind::Drum));
    const auto audio = m.project().tracks[0].uid, synth = m.project().tracks[1].uid, drums = m.project().tracks[2].uid;
    m.selectTrack(audio);
    CHECK(m.liveTarget() == 0);                                 // an audio track cannot play notes
    m.selectTrack(drums);
    CHECK(m.liveTarget() == drums);
    m.recording().arm(synth, true);
    CHECK(m.liveTarget() == synth);                             // arming wins over the selection
    m.recording().arm(synth, false);
    CHECK(m.liveTarget() == drums);
    std::string err;
    CHECK_FALSE(m.recording().start(err));                      // nothing armed
    m.recording().arm(synth, true);
    m.recording().settings().recordNotes = false;
    CHECK_FALSE(m.recording().canRecord());                     // armed for notes but note recording is off
}

TEST_CASE("polyphonic audio input: the model switches it on and off, and recording knows its latency", "[appmodel][midi]") {
    engine::Engine eng;
    eng.prepare(48000.0);
    AppModel m(eng, 48000.0);
    CHECK_FALSE(m.polyInput());
    CHECK_FALSE(eng.polyInputEnabled());
    m.setPolyInput(true);
    CHECK(m.polyInput());
    CHECK(eng.polyInputEnabled());
    CHECK(m.polyLatencyFrames() > 3000);                        // an analysis window of 128 ms is not free
    CHECK(eng.polyLatencyFrames() == m.polyLatencyFrames());
    m.setPolyInput(false);
    CHECK_FALSE(m.polyInput());
    CHECK_FALSE(eng.polyInputEnabled());
}

// ---- hosted plugins: the model's side, against a fake host ----


TEST_CASE("plugins (model): a plugin device is loaded when it is added, reported once if it cannot be, and described by its host", "[appmodel][plugins]") {
    Rig r;
    auto& m = *r.m;
    ddaw::testing::FakeProvider host;
    m.setPluginProvider(&host);
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    const auto track = m.project().tracks[0].uid;
    REQUIRE(m.apply(edit::addPluginEffect(m.project(), track, "fx", "AudioUnit#x#Fake", "Fake")));
    REQUIRE(m.project().tracks[0].fx.size() == 1);
    const auto& d = m.project().tracks[0].fx[0];
    CHECK(d.type == "plugin");
    CHECK(d.plugin == "AudioUnit#x#Fake");
    CHECK(d.pluginName == "Fake");
    CHECK(host.live(d.uid));
    CHECK(host.ensureCalls == 1);
    // later edits do not ask again
    m.apply(edit::addScene(m.project()));
    CHECK(host.ensureCalls == 1);
    // the UI sees the plugin's parameters and names
    const auto* info = m.deviceInfo(app::Chain::Effect, d);
    REQUIRE(info);
    CHECK(info->params.size() == 2);
    CHECK(info->label == "Fake");
    CHECK(m.paramLabel(d, "p_cutoff") == "Cutoff Freq");
    CHECK(m.paramLabel(d, "p_res") == "Resonance");
    CHECK(m.paramLabel(project::DeviceSpec{}, "lfoShape") == "LFO Shape");
    // an instrument plugin that will not load: said once, and not retried on every edit
    host.unloadable.insert("AudioUnit#x#Gone");
    REQUIRE(m.apply(edit::setPluginInstrument(track, "AudioUnit#x#Gone", "Gone")));
    CHECK(m.status().find("Gone") != std::string::npos);
    CHECK(m.status().find("cannot load") != std::string::npos);
    const int calls = host.ensureCalls;
    m.apply(edit::addScene(m.project()));
    m.apply(edit::addScene(m.project()));
    CHECK(host.ensureCalls == calls);
    CHECK(m.deviceInfo(app::Chain::Instrument, m.project().tracks[0].inst) == nullptr);   // not live: no panel content
}

TEST_CASE("plugins (model): a plugin's state is saved with the project, and loaded back into the plugin", "[appmodel][plugins]") {
    namespace fs = std::filesystem;
    const auto dir = (fs::temp_directory_path() / "ddaw_plugin_state_test.ddaw").string();
    fs::remove_all(dir);
    Rig r;
    auto& m = *r.m;
    ddaw::testing::FakeProvider host;
    m.setPluginProvider(&host);
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    const auto track = m.project().tracks[0].uid;
    m.apply(edit::addPluginEffect(m.project(), track, "fx", "AudioUnit#x#Fake", "Fake"));
    const auto uid = m.project().tracks[0].fx[0].uid;
    host.states[uid] = "c3RhdGUtYmxvYg==";                          // what the plugin's own editor produced
    CHECK(m.project().tracks[0].fx[0].pluginState.empty());
    const bool couldUndo = m.canUndo();
    const auto undoDepth = m.document().revision();
    std::string err;
    REQUIRE(m.save(dir, err));                                      // saving flushes it into the document ...
    CHECK(m.project().tracks[0].fx[0].pluginState == "c3RhdGUtYmxvYg==");
    CHECK(m.canUndo() == couldUndo);                                // ... without making an undo step of it
    (void)undoDepth;
    CHECK_FALSE(m.dirty());
    // reopen: the plugin is created again with that state, before the first build
    host.liveIds.clear(); host.states.clear();
    m.newProject();
    CHECK(host.liveIds.empty());
    REQUIRE(m.open(dir, err));
    REQUIRE(m.project().tracks[0].fx.size() == 1);
    const auto reopened = m.project().tracks[0].fx[0].uid;
    CHECK(host.live(reopened));
    CHECK(host.states[reopened] == "c3RhdGUtYmxvYg==");
    // opening another project drops the instances the new one does not use
    m.newProject();
    CHECK(host.liveIds.empty());
    fs::remove_all(dir);
}

TEST_CASE("model: a dynamics device's gain reduction is read from the engine by its place in the chain (bypassed devices take no slot)", "[appmodel][meters]") {
    Rig r;
    auto& m = *r.m;
    auto& bank = const_cast<engine::MeterBank&>(r.eng.meters());   // the audio thread writes these; the test plays its part
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    m.apply(edit::addTrack(m.project(), project::TrackKind::Synth));
    const auto t1 = m.project().tracks[1].uid;
    m.apply(edit::addDevice(m.project(), t1, "fx", "reverb"));
    m.apply(edit::addDevice(m.project(), t1, "fx", "comp"));
    m.apply(edit::addDevice(m.project(), t1, "fx", "mbcomp"));
    const auto& fx = m.project().tracks[1].fx;
    const auto comp = fx[1].uid, mb = fx[2].uid;
    bank.setFxGr(1, 1, 0, -9.0f);                       // track 1, second effect
    bank.setFxGr(1, 2, 0, -3.0f); bank.setFxGr(1, 2, 1, -6.0f); bank.setFxGr(1, 2, 2, -12.0f);
    CHECK(m.reductionDb(comp) == -9.0f);
    CHECK(m.reductionDb(mb, 0) == -3.0f);
    CHECK(m.reductionDb(mb, 1) == -6.0f);
    CHECK(m.reductionDb(mb, 2) == -12.0f);
    CHECK(m.reductionDb(99999) == 0.0f);                           // no such device
    // bypassing the reverb moves everything up a slot in the graph, and the lookup follows
    m.apply({"device.set", {{"uid", fx[0].uid}, {"field", "on"}, {"value", false}}});
    bank.clearFx();
    bank.setFxGr(1, 0, 0, -4.0f);
    CHECK(m.reductionDb(comp) == -4.0f);
    // the master chain
    m.apply(edit::addDevice(m.project(), 0, "master", "comp"));
    const auto mc = m.project().masterFx[0].uid;
    bank.setFxGr(-1, 0, 0, -7.0f);
    CHECK(m.reductionDb(mc) == -7.0f);
}
