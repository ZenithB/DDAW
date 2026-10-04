// A3 scheduler and transport behaviour: swing, launch quantisation, looping patterns, gate lengths,
// arrangement mode and loop wrap, probability and humanise determinism, stop, and swap continuity.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "engine/Scheduler.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;
constexpr double kFpt = kSr * 60.0 / 120.0 / 96.0;  // 250 frames per tick at 120 bpm

// Records every note on/off with the absolute frame it arrives at (frames counted by process()).
struct Rec { std::vector<std::pair<int64_t, int>> on, off; std::vector<float> vels; int64_t frame = 0; };

class RecInst final : public InstrumentDevice {
public:
    explicit RecInst(Rec* r) : r_(r) {}
    std::span<const ParamSpec> params() const override { return {}; }
    void prepare(double, int) override {}
    void setParam(uint16_t, float) override {}
    void noteOn(uint8_t pitch, float vel, uint32_t) override { r_->on.push_back({r_->frame, pitch}); r_->vels.push_back(vel); }
    void noteOff(uint32_t id) override { r_->off.push_back({r_->frame, int(id)}); }
    void process(float*, float*, int, const ProcessContext&, const ModInputs&) override {}
    void reset() override {}
private:
    Rec* r_;
};

// One-track graph with a RecInst and the given session pattern in scene 0.
std::unique_ptr<Graph> recGraph(Rec* rec, std::vector<NoteEv> events, double loopLen, SchedParams sp = {0, 24, 0}, double launchQ = 1) {
    auto g = std::make_unique<Graph>(1, kSr);
    auto& t = g->addTrack();
    t.inst = std::make_unique<RecInst>(rec);
    t.sched.session.resize(1);
    t.sched.session[0] = ClipPattern{loopLen, std::move(events)};
    g->setTiming(sp, launchQ, 4, 4);
    g->finalize();
    return g;
}

NoteEv ev(double tick, int pitch, double dur, float vel = 1.0f, float pr = 1.0f) {
    NoteEv e; e.tick = tick; e.pitch = uint8_t(pitch); e.durTicks = dur; e.vel = vel; e.pr = pr; return e;
}

Cmd cmd(CmdType t, uint32_t epoch = 0) { Cmd c; c.type = t; c.epoch = epoch; return c; }
void launch(Engine& e, int track, int scene) { Cmd c = cmd(CmdType::ClipLaunch, 1); c.clipLaunch = {uint16_t(track), uint16_t(scene)}; e.commands().push(c); }
void play(Engine& e, uint8_t mode = 0, double from = 0) { Cmd c = cmd(CmdType::TransportPlay); c.transportPlay = {mode, from}; e.commands().push(c); }
void tempo(Engine& e, double bpm) { Cmd c = cmd(CmdType::SetTempo); c.setTempo = {bpm}; e.commands().push(c); }

// Process `frames` in callbacks of `block`, tracking the frame count for the recorder.
void run(Engine& e, Rec& rec, int64_t frames, int block = 64) {
    const auto len = static_cast<size_t>(block);
    std::vector<float> l(len), r(len);
    // The recorder reads rec.frame at note time; Engine::process splits internally, so give it the chunk start.
    // To get exact frames we drive one callback per chunk-sized piece and let the recorder observe the
    // callback start. Event splitting makes every event land on a chunk start, so use block = 1 for exactness.
    for (int64_t pos = 0; pos < frames; pos += block) {
        rec.frame = pos;
        e.process(l.data(), r.data(), int(std::min<int64_t>(block, frames - pos)));
    }
}

}  // namespace

TEST_CASE("swing offsets: downbeats and even subdivisions stay, odd ones are pushed late", "[scheduler]") {
    // 16n subdivision = 24 ticks, period 48: tick 24 is the odd 16th, progress 0.5 -> sin(pi/2) = 1 -> swing * 16
    CHECK(swingOffsetTicks(24, 0.5, 24) == Approx(8.0));
    CHECK(swingOffsetTicks(24, 1.0, 24) == Approx(16.0));
    CHECK(swingOffsetTicks(0, 0.5, 24) == 0.0);      // downbeat
    CHECK(swingOffsetTicks(96, 0.5, 24) == 0.0);     // quarter note
    CHECK(swingOffsetTicks(48, 0.5, 24) == 0.0);     // even subdivision
    CHECK(swingOffsetTicks(24, 0.0, 24) == 0.0);     // no swing
    CHECK(swingOffsetTicks(12, 0.5, 24) > 0.0);      // off-grid positions get a partial push
    CHECK(swingSubdivTicks("8n") == 48.0);
    CHECK(swingSubdivTicks("16n") == 24.0);
    CHECK(swingSubdivTicks("anything") == 24.0);
}

TEST_CASE("TrackSched: a launched pattern loops from its anchor", "[scheduler]") {
    TrackSched s;
    s.session.resize(1);
    s.session[0] = ClipPattern{96, {ev(0, 60, 10), ev(48, 64, 10)}};
    XorShift rng(1);
    const SchedParams sp{0, 24, 0};
    CHECK_FALSE(s.peek(TransportMode::Session, 0, sp, rng).has_value());   // nothing launched
    s.launch(0, 384);                                                       // anchor at the next bar
    std::vector<double> fires;
    for (int i = 0; i < 6; ++i) {
        const auto t = s.peek(TransportMode::Session, 0, sp, rng);
        REQUIRE(t.has_value());
        fires.push_back(*t);
        REQUIRE(s.takeDue(*t).has_value());
    }
    CHECK(fires == std::vector<double>{384, 432, 480, 528, 576, 624});     // 96-tick loop, two notes each
    s.stopClip();
    CHECK_FALSE(s.peek(TransportMode::Session, 0, sp, rng).has_value());
}

TEST_CASE("TrackSched: relocate seeks into the loop; takeDue only returns due events", "[scheduler]") {
    TrackSched s;
    s.session.resize(1);
    s.session[0] = ClipPattern{96, {ev(0, 60, 10), ev(24, 62, 10), ev(48, 64, 10)}};
    XorShift rng(1);
    const SchedParams sp{0, 24, 0};
    s.launch(0, 0);
    s.relocate(TransportMode::Session, 100);     // 4 ticks into the second pass: next is tick 24 of pass 2 = 120
    const auto t = s.peek(TransportMode::Session, 100, sp, rng);
    REQUIRE(t.has_value());
    CHECK(*t == 120.0);
    CHECK_FALSE(s.takeDue(119.0).has_value());   // not due yet, and the pending event survives
    const auto p = s.takeDue(120.0);
    REQUIRE(p.has_value());
    CHECK(p->pitch == 62);
}

TEST_CASE("TrackSched: probability zero never fires and cannot spin the audio thread", "[scheduler]") {
    TrackSched s;
    s.session.resize(1);
    s.session[0] = ClipPattern{96, {ev(0, 60, 10, 1.0f, 0.0f), ev(48, 62, 10, 1.0f, 0.0f)}};
    XorShift rng(7);
    s.launch(0, 0);
    const auto t = s.peek(TransportMode::Session, 0, {0, 24, 0}, rng);
    REQUIRE(t.has_value());
    const auto p = s.takeDue(*t);
    REQUIRE(p.has_value());
    CHECK_FALSE(p->audible);                     // the silent marker, parked a few bars ahead
    CHECK(*t >= 4 * 384.0);
}

TEST_CASE("TrackSched: humanise is deterministic per seed and never schedules into the past", "[scheduler]") {
    auto fires = [](uint64_t seed) {
        TrackSched s;
        s.session.resize(1);
        s.session[0] = ClipPattern{96, {ev(0, 60, 10, 0.5f), ev(48, 64, 10, 0.5f)}};
        XorShift rng(seed);
        s.launch(0, 0);
        std::vector<std::pair<double, float>> out;
        double now = 0;
        for (int i = 0; i < 20; ++i) {
            const auto t = s.peek(TransportMode::Session, now, {0, 24, 1.0}, rng);
            const auto p = s.takeDue(*t);
            out.push_back({p->fireTick, p->vel});
            now = std::max(now, *t);
        }
        return out;
    };
    CHECK(fires(5) == fires(5));
    CHECK(fires(5) != fires(6));
    for (auto [tick, vel] : fires(5)) { CHECK(tick >= 0.0); CHECK(vel >= 0.02f); CHECK(vel <= 1.0f); }
}

TEST_CASE("launch quantisation: the next bar boundary, or immediately when launchQ is 0", "[scheduler]") {
    Graph g(1, kSr);
    g.setTiming({}, 1, 4, 4);
    CHECK(g.nextBoundaryTicks(100) == 384.0);
    CHECK(g.nextBoundaryTicks(383) == 384.0);
    CHECK(g.nextBoundaryTicks(384) == 768.0);    // exactly on the boundary waits for the next one
    g.setTiming({}, 2, 4, 4);
    CHECK(g.nextBoundaryTicks(100) == 768.0);
    g.setTiming({}, 0, 4, 4);
    CHECK(g.nextBoundaryTicks(100) == 104.0);
    g.setTiming({}, 1, 3, 4);                     // 3/4: bars are 288 ticks
    CHECK(g.barTicks() == 288.0);
    CHECK(g.nextBoundaryTicks(100) == 288.0);
}

TEST_CASE("session playback: sample-accurate note on and gate-off frames, looping", "[scheduler][engine]") {
    Rec rec;
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(recGraph(&rec, {ev(0, 60, 48), ev(96, 64, 12)}, 192));
    tempo(e, 120); launch(e, 0, 0); play(e);
    run(e, rec, int64_t(2.5 * 192 * kFpt), 1);       // block = 1: every event is observed at its exact frame

    REQUIRE(rec.on.size() >= 4);
    CHECK(rec.on[0] == std::pair<int64_t, int>{0, 60});
    CHECK(rec.on[1] == std::pair<int64_t, int>{int64_t(96 * kFpt), 64});
    CHECK(rec.on[2] == std::pair<int64_t, int>{int64_t(192 * kFpt), 60});      // the loop restarts at the pattern length
    CHECK(rec.on[3] == std::pair<int64_t, int>{int64_t(288 * kFpt), 64});
    // gate lengths: 48 ticks = 12000 frames; 12 ticks = 3000 frames
    REQUIRE(rec.off.size() >= 2);
    CHECK(rec.off[0].first == int64_t(48 * kFpt));
    CHECK(rec.off[1].first == int64_t(96 * kFpt) + int64_t(12 * kFpt));
}

TEST_CASE("a very short note is held for at least 20 ms", "[scheduler][engine]") {
    Rec rec;
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(recGraph(&rec, {ev(0, 60, 0.1)}, 96));
    tempo(e, 120); launch(e, 0, 0); play(e);
    run(e, rec, 4800, 1);
    REQUIRE(rec.off.size() >= 1);
    CHECK(rec.off[0].first == int64_t(0.02 * kSr));   // 960 frames, not 25
}

TEST_CASE("launching while playing waits for the next bar boundary", "[scheduler][engine]") {
    Rec rec;
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(recGraph(&rec, {ev(0, 60, 24)}, 96));
    tempo(e, 120); play(e);                           // playing, nothing launched
    run(e, rec, int64_t(100 * kFpt), 64);
    CHECK(rec.on.empty());
    launch(e, 0, 0);                                  // at tick ~100: the next bar line is 384
    const int64_t start = int64_t(100 * kFpt);
    std::vector<float> l(64), r(64);
    for (int64_t pos = start; pos < int64_t(400 * kFpt); pos += 64) { rec.frame = pos; e.process(l.data(), r.data(), 64); }
    REQUIRE_FALSE(rec.on.empty());
    CHECK(rec.on[0].first >= int64_t(384 * kFpt) - 64);
    CHECK(rec.on[0].first <= int64_t(384 * kFpt) + 64);
}

TEST_CASE("transport stop silences notes and stops the scheduler", "[scheduler][engine]") {
    Rec rec;
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(recGraph(&rec, {ev(0, 60, 96), ev(48, 62, 96)}, 192));
    tempo(e, 120); launch(e, 0, 0); play(e);
    run(e, rec, int64_t(60 * kFpt), 64);              // the first note is sounding, the second just started
    REQUIRE(rec.on.size() == 2);
    const size_t offsBefore = rec.off.size();
    e.commands().push(cmd(CmdType::TransportStop));
    rec.frame = int64_t(60 * kFpt);
    std::vector<float> l(256), r(256);
    e.process(l.data(), r.data(), 256);               // commands land at the next internal chunk start (< 128 frames away)
    CHECK(rec.off.size() == offsBefore + 2);          // both sounding notes were released
    const size_t ons = rec.on.size();
    for (int i = 0; i < 100; ++i) e.process(l.data(), r.data(), 256);
    CHECK(rec.on.size() == ons);                      // nothing fires while stopped
}

TEST_CASE("arrangement mode: bounded events fire once, and the loop region wraps", "[scheduler][engine]") {
    auto build = [](Rec* rec, bool loop) {
        auto g = std::make_unique<Graph>(1, kSr);
        auto& t = g->addTrack();
        t.inst = std::make_unique<RecInst>(rec);
        t.sched.arr = {ev(0, 60, 24), ev(96, 62, 24)};
        g->setTiming({}, 1, 4, 4);
        g->setLoop(loop, 0, 384);
        g->finalize();
        return g;
    };
    {   // no loop: the arrangement plays through once
        Rec rec; Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(build(&rec, false));
        tempo(e, 120); play(e, 1);
        run(e, rec, int64_t(3 * 384 * kFpt), 64);
        CHECK(rec.on.size() == 2);
    }
    {   // loop 0..384 (one bar): the two events repeat every bar
        Rec rec; Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(build(&rec, true));
        tempo(e, 120); play(e, 1);
        run(e, rec, int64_t(3.5 * 384 * kFpt), 64);
        CHECK(rec.on.size() == 8);                    // 2 events x 4 passes (the 4th is half played)
        CHECK(rec.on[2].first == Approx(384 * kFpt).margin(64));
    }
    {   // an offline render ignores the loop region
        Rec rec; Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setLoopOverrideOff(true);
        e.setInitialGraph(build(&rec, true));
        tempo(e, 120); play(e, 1);
        run(e, rec, int64_t(3 * 384 * kFpt), 64);
        CHECK(rec.on.size() == 2);
    }
}

TEST_CASE("a graph swap keeps launched clips playing (same track index)", "[scheduler][engine]") {
    Rec rec;
    Engine e; e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
    e.setInitialGraph(recGraph(&rec, {ev(0, 60, 24)}, 96));
    tempo(e, 120); launch(e, 0, 0); play(e);
    run(e, rec, int64_t(200 * kFpt), 64);
    const size_t before = rec.on.size();
    REQUIRE(before >= 2);
    auto next = recGraph(&rec, {ev(0, 72, 24)}, 96);   // a new graph with a different pattern, never launched by the host
    next->finalize();
    // epoch 2 so the engine treats it as a swap
    auto g2 = std::make_unique<Graph>(2, kSr);
    auto& t = g2->addTrack();
    t.inst = std::make_unique<RecInst>(&rec);
    t.sched.session.resize(1);
    t.sched.session[0] = ClipPattern{96, {ev(0, 72, 24)}};
    g2->setTiming({}, 1, 4, 4);
    g2->finalize();
    REQUIRE(e.postGraph(g2));
    std::vector<float> l(64), r(64);
    for (int64_t pos = int64_t(200 * kFpt); pos < int64_t(500 * kFpt); pos += 64) { rec.frame = pos; e.process(l.data(), r.data(), 64); }
    bool newPatternPlayed = false;
    for (size_t i = before; i < rec.on.size(); ++i) newPatternPlayed |= rec.on[i].second == 72;
    CHECK(newPatternPlayed);                           // the launch carried over, so the new pattern sounds without a relaunch
}
