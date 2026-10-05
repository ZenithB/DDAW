// MPE / per-note expression (B5): a controller's per-note bend, slide and pressure reach the playing instrument through the
// live path, including the values sent before the note-on, and they leave the notes they were not meant for alone.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <string>
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

std::string projectWith(const std::string& inst) {
    return R"({"scope":{"kind":"live"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[
      {"id":"t1","kind":"synth","inst":)" + inst + R"(,"fx":[],"gain":0,"pan":0}],"clips":{}}})";
}
const std::string kCarrier = R"({"type":"fmop","params":{"algo":4,"l1":1,"l2":0,"l3":0,"l4":0,"attack":0.001,"sustain":1.0,"release":0.02}})";
// operator 2 (ratio 0.25 -> 110 Hz for an A4) modulates operator 1 at index 1: sidebands at 440 +- k*110
const std::string kFm = R"({"type":"fmop","params":{"algo":0,"index":1.0,"iSus":1.0,"velIdx":0,"r2":0.25,"l1":1,"l2":1,"l3":0,"l4":0,"attack":0.001,"sustain":1.0,"release":0.02}})";
const std::string kPoly = R"({"type":"poly","params":{"wave":3,"attack":0.003,"sustain":1.0,"release":0.02}})";
const std::string kMono = R"({"type":"mono","params":{"wave":3,"attack":0.003,"sustain":1.0,"release":0.02}})";

struct Rig {
    Engine e;
    std::vector<float> l = std::vector<float>(128u), r = std::vector<float>(128u);
    explicit Rig(const std::string& inst) {
        e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(buildGraph(project::importFixtureJson(projectWith(inst)), kSr, 1).graph);
        Cmd c; c.type = CmdType::LiveTrack; c.epoch = 1; c.liveTrack = {0};
        e.commands().push(c);
    }
    std::vector<float> run(int blocks) {
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) { e.process(l.data(), r.data(), 128); out.insert(out.end(), l.begin(), l.end()); }
        return out;
    }
    void on(int p, float v = 0.9f) { e.liveNote(Engine::LiveKind::NoteOn, p, v); }
    void off(int p) { e.liveNote(Engine::LiveKind::NoteOff, p); }
};

std::vector<double> spectrum(const std::vector<float>& x) {
    constexpr size_t N = 16384;
    dsp::Fft f; f.prepare(int(N));
    std::vector<double> re(N), im(N, 0.0), m(N / 2);
    for (size_t i = 0; i < N; ++i) re[i] = double(x[x.size() - N + i]) * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / N));
    f.forward(re.data(), im.data());
    for (size_t k = 0; k < N / 2; ++k) m[k] = std::hypot(re[k], im[k]);
    return m;
}
double mag(const std::vector<float>& x, double hz) {
    const auto m = spectrum(x);
    const int k0 = int(std::lround(hz / kSr * 16384.0));
    double best = 0;
    for (int k = k0 - 2; k <= k0 + 2; ++k) best = std::max(best, m[size_t(k)]);
    return best;
}
double peakHz(const std::vector<float>& x, double lo, double hi) {
    const auto m = spectrum(x);
    size_t best = size_t(lo / kSr * 16384.0);
    for (size_t k = size_t(lo / kSr * 16384.0); k <= size_t(hi / kSr * 16384.0); ++k) if (m[k] > m[best]) best = k;
    const double a = std::log(m[best - 1] + 1e-12), b = std::log(m[best] + 1e-12), c = std::log(m[best + 1] + 1e-12);
    return (double(best) + 0.5 * (a - c) / (a - 2.0 * b + c)) * kSr / 16384.0;
}
}  // namespace

TEST_CASE("mpe: a note's own bend moves only that note, in semitones", "[mpe][engine]") {
    Rig r(kCarrier);
    r.on(69); r.on(72);
    r.e.liveExpression(69, 2, 12.0f);                 // one octave up on the A, the C stays
    const auto x = r.run(300);
    CHECK(peakHz(x, 800.0, 960.0) == Approx(880.0).margin(5.0));
    CHECK(peakHz(x, 500.0, 560.0) == Approx(523.25).margin(4.0));
    CHECK(mag(x, 440.0) < 0.02 * mag(x, 880.0));      // the unbent A is gone
}

TEST_CASE("mpe: expression sent before the note-on shapes the note from its first sample", "[mpe][engine]") {
    Rig r(kCarrier);
    r.e.liveExpression(69, 2, 12.0f);                 // MPE controllers send the channel's bend first
    r.on(69);
    const auto x = r.run(250);
    CHECK(peakHz(x, 800.0, 960.0) == Approx(880.0).margin(5.0));
    // the first block already carries the bend: the attack is 1 ms, so it is audible at once and not 440 Hz
    const std::vector<float> first(x.begin(), x.begin() + 128);
    int crossings = 0;
    for (size_t i = 1; i < first.size(); ++i) if ((first[i - 1] < 0.0f) != (first[i] < 0.0f)) ++crossings;
    CHECK(crossings >= 3);                            // 128 samples of 880 Hz hold ~4.7 zero crossings; 440 Hz holds ~2.3
}

TEST_CASE("mpe: a bend for every note applies to sounding notes and to the next ones, and adds to a note's own", "[mpe][engine]") {
    Rig r(kCarrier);
    r.on(69);
    r.e.liveBend(12.0f);
    r.on(72);                                         // struck after the bend: starts bent
    const auto x = r.run(300);
    CHECK(peakHz(x, 800.0, 960.0) == Approx(880.0).margin(5.0));
    CHECK(peakHz(x, 1000.0, 1100.0) == Approx(1046.5).margin(6.0));
    r.e.liveExpression(69, 2, -12.0f);                // its own bend cancels the global one
    const auto y = r.run(300);
    CHECK(peakHz(y, 400.0, 480.0) == Approx(440.0).margin(4.0));
    CHECK(peakHz(y, 1000.0, 1100.0) == Approx(1046.5).margin(6.0));
}

TEST_CASE("mpe: a released note forgets its expression", "[mpe][engine]") {
    Rig r(kCarrier);
    r.e.liveExpression(69, 2, 12.0f);
    r.on(69);
    r.run(100);
    r.off(69);
    r.run(100);                                       // released and gone (release 20 ms)
    r.on(69);                                         // struck again with no new expression: neutral
    const auto x = r.run(300);
    CHECK(peakHz(x, 400.0, 480.0) == Approx(440.0).margin(4.0));
}

TEST_CASE("mpe: pressure raises the level and slide opens the sound", "[mpe][engine]") {
    Rig plain(kCarrier), pressed(kCarrier);
    plain.on(69); pressed.on(69);
    pressed.e.liveExpression(69, 1, 1.0f);
    const double a = mag(plain.run(300), 440.0), b = mag(pressed.run(300), 440.0);
    CHECK(b / a == Approx(1.5).margin(0.06));

    Rig dull(kFm), bright(kFm);
    dull.on(69); bright.on(69);
    bright.e.liveExpression(69, 0, 1.0f);            // slide 1: the modulation index is 3x
    const auto d = dull.run(300), s = bright.run(300);
    const double side2dull = mag(d, 440.0 + 220.0), side2bright = mag(s, 440.0 + 220.0);
    CHECK(side2bright > 2.5 * side2dull);            // J2(3)/J2(1) is about 4
}

TEST_CASE("mpe: poly and mono follow a note's bend too", "[mpe][engine]") {
    for (const auto& inst : {kPoly, kMono}) {
        Rig r(inst);
        r.on(69);
        r.e.liveExpression(69, 2, 12.0f);
        CHECK(peakHz(r.run(300), 800.0, 960.0) == Approx(880.0).margin(6.0));
    }
}

TEST_CASE("mpe: a device with no expression support ignores it", "[mpe][engine]") {
    Rig r(R"({"type":"fm","params":{}})");
    r.on(69);
    r.e.liveExpression(69, 2, 12.0f);
    r.e.liveExpression(69, 1, 1.0f);
    r.e.liveBend(5.0f);
    const auto x = r.run(100);
    double rms = 0; for (float v : x) rms += double(v) * v;
    CHECK(rms > 0.0);
    for (float v : x) REQUIRE(std::isfinite(v));
}

TEST_CASE("mpe: a storm of expression from several threads allocates nothing", "[mpe][engine][realtime]") {
    Rig r(kCarrier);
    r.on(60); r.on(64); r.on(67);
    std::thread a([&] { for (int i = 0; i < 400; ++i) { r.e.liveExpression(60 + (i % 3) * 4 - (i % 3 == 2 ? 1 : 0), 2, float(i % 24) - 12.0f); std::this_thread::sleep_for(std::chrono::microseconds(40)); } });
    std::thread b([&] { for (int i = 0; i < 400; ++i) { r.e.liveExpression(64, 1, float(i % 10) / 10.0f); r.e.liveBend(float(i % 5)); std::this_thread::sleep_for(std::chrono::microseconds(60)); } });
    test::AllocGuard guard;
    for (int blk = 0; blk < 400; ++blk) { r.e.process(r.l.data(), r.r.data(), 128); std::this_thread::sleep_for(std::chrono::microseconds(100)); }
    CHECK(guard.count() == 0);
    a.join(); b.join();
}

TEST_CASE("mpe: the output is bit-identical for every callback size when expression arrives on a chunk boundary", "[mpe][engine]") {
    const auto play = [&](int callback) {
        Rig r(kCarrier);
        r.on(69); r.on(72);
        const size_t cbN = size_t(callback);
        std::vector<float> out, l(cbN), rr(cbN);
        for (int done = 0; done < 16384; done += callback) {
            if (done == 4096) { r.e.liveExpression(69, 2, 7.0f); r.e.liveExpression(72, 1, 0.8f); r.e.liveBend(-2.0f); }
            r.e.process(l.data(), rr.data(), callback);
            out.insert(out.end(), l.begin(), l.end());
        }
        out.resize(16384);
        return out;
    };
    const auto ref = play(128);
    for (const int cb : {64, 256, 512, 1024}) { INFO("callback size " << cb); CHECK(play(cb) == ref); }
}

// ---------------------------------------------------------------------------------------------------------------------
// Every pitched instrument follows a note's bend, and (where it makes sense) pressure. Driven directly, without an engine.

#include "core/Sample.h"
#include "devices/Registry.h"

namespace {

std::shared_ptr<const SampleBuf> sineSample(double hz, double seconds, double sr) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = float(sr);
    for (size_t i = 0; i < size_t(seconds * sr); ++i) b->l.push_back(0.5f * float(std::sin(2.0 * std::numbers::pi * hz * double(i) / sr)));
    return b;
}

std::vector<float> play(const std::string& type, int note, float bend, float pressure, int blocks = 300) {
    auto d = createInstrument(type);
    d->prepare(kSr, 128);
    if (type == "sampler" || type == "ksampler" || type == "granular") d->setSample(0, sineSample(220.0, 4.0, kSr));
    d->noteOn(uint8_t(note), 0.9f, 1);
    if (bend != 0.0f) d->noteExpression(1, 2, bend);
    if (pressure != 0.0f) d->noteExpression(1, 1, pressure);
    std::vector<float> out, l(128u), r(128u);
    ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d->process(l.data(), r.data(), 128, ctx, {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}
double energy(const std::vector<float>& x, size_t from, size_t to) { double s = 0; for (size_t i = from; i < to; ++i) s += double(x[i]) * x[i]; return std::sqrt(s / double(to - from)); }

}  // namespace

TEST_CASE("mpe: every pitched instrument follows a note's bend", "[mpe][engine]") {
    struct Case { const char* type; int note; double hz; };
    // the unbent fundamental, and a five-semitone bend (x1.3348, 293.7 Hz) that lands between every harmonic of it, of duo's
    // second oscillator (a fifth up) and of the fm / keys modulators
    for (const Case c : {Case{"duo", 57, 220.0}, Case{"fm", 57, 220.0}, Case{"keys", 57, 220.0}, Case{"follow", 57, 220.0},
                         Case{"sampler", 48, 220.0}, Case{"ksampler", 48, 220.0}, Case{"granular", 60, 220.0},
                         Case{"harmnoise", 57, 220.0}, Case{"subtractive", 57, 220.0}, Case{"wavetable", 57, 220.0}, Case{"waveshaper", 57, 220.0},
                         Case{"modal", 57, 220.0}}) {
        INFO(c.type);
        const double up = c.hz * std::pow(2.0, 5.0 / 12.0);
        const auto flat = play(c.type, c.note, 0.0f, 0.0f), bent = play(c.type, c.note, 5.0f, 0.0f);
        REQUIRE(energy(flat, flat.size() - 16384, flat.size()) > 1e-4);
        CHECK(mag(flat, c.hz) > 5.0 * mag(flat, up));          // unbent: at the note, not at the bend
        CHECK(mag(bent, up) > 5.0 * mag(bent, c.hz));          // bent: at the bend, not at the note
    }
}

TEST_CASE("mpe: pressure raises the level of the instruments that take it", "[mpe][engine]") {
    for (const char* type : {"duo", "fm", "keys", "follow", "sampler", "ksampler", "harmnoise", "subtractive", "wavetable", "waveshaper", "modal"}) {   // perc is one-shot: its test is in perc_test
        INFO(type);
        const int note = (std::string(type) == "sampler" || std::string(type) == "ksampler") ? 48 : 57;
        const auto flat = play(type, note, 0.0f, 0.0f), pressed = play(type, note, 0.0f, 1.0f);
        const size_t a = flat.size() - 16384;
        CHECK(energy(pressed, a, flat.size()) / energy(flat, a, flat.size()) == Approx(1.5).margin(0.1));
    }
    // granular: the grains are random but seeded, so the same notes give the same grains: pressure scales them
    const auto g0 = play("granular", 60, 0.0f, 0.0f), g1 = play("granular", 60, 0.0f, 1.0f);
    CHECK(energy(g1, g1.size() - 16384, g1.size()) / energy(g0, g0.size() - 16384, g0.size()) == Approx(1.5).margin(0.1));
}

TEST_CASE("mpe: an unexpressed note sounds exactly as before, in every instrument", "[mpe][engine]") {
    // expression for a note id that is not sounding is ignored, and a neutral note is bit-identical to one that was never touched
    for (const char* type : {"duo", "fm", "keys", "follow", "sampler", "ksampler", "granular", "poly", "mono", "fmop",
                             "harmnoise", "subtractive", "wavetable", "waveshaper", "modal"}) {   // perc is one-shot: its test is in perc_test
        INFO(type);
        const int note = (std::string(type) == "sampler" || std::string(type) == "ksampler") ? 48 : 57;
        auto d = createInstrument(type);
        d->prepare(kSr, 128);
        if (std::string(type) == "sampler" || std::string(type) == "ksampler" || std::string(type) == "granular") d->setSample(0, sineSample(220.0, 4.0, kSr));
        d->noteOn(uint8_t(note), 0.9f, 1);
        d->noteExpression(99, 2, 12.0f);           // another note's id
        d->noteExpression(99, 1, 1.0f);
        d->noteExpression(1, 2, 0.0f);             // this note's neutral values
        d->noteExpression(1, 1, 0.0f);
        d->noteExpression(1, 0, 0.0f);
        std::vector<float> got, l(128u), r(128u);
        ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
        for (int b = 0; b < 60; ++b) { std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f); d->process(l.data(), r.data(), 128, ctx, {}); got.insert(got.end(), l.begin(), l.end()); }
        const auto ref = play(type, note, 0.0f, 0.0f, 60);
        CHECK(got == ref);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Expression curves stored in clips play back with the notes.

#include "engine/OfflineRender.h"
#include "project/ProjectJson.h"

namespace {
std::string clipProject(const std::string& noteExtra, const std::string& midifx = "[]") {
    return R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[
      {"id":"t1","kind":"synth","inst":{"type":"fmop","params":{"algo":4,"l1":1,"l2":0,"l3":0,"l4":0,"attack":0.001,"sustain":1.0,"release":0.05}},
       "fx":[],"midifx":)" + midifx + R"(,"gain":0,"pan":0}],
      "clips":{"t1|s":{"len":768,"notes":{"n":{"p":69,"s":0,"d":576,"v":0.9)" + noteExtra + R"(}}}}}})";
}
double zeroCrossHz(const std::vector<float>& x, double fromSec, double lenSec) {
    const size_t a = size_t(fromSec * kSr), n = size_t(lenSec * kSr);
    int crossings = 0;
    for (size_t i = a + 1; i < a + n && i < x.size(); ++i) if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++crossings;
    return 0.5 * double(crossings) / lenSec;
}
double rmsOver(const std::vector<float>& x, double fromSec, double lenSec) {
    double s = 0; const size_t a = size_t(fromSec * kSr), n = size_t(lenSec * kSr);
    for (size_t i = a; i < a + n && i < x.size(); ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(n));
}
engine::RenderResult render(const std::string& proj) { return engine::renderFixture(project::importFixtureJson(proj), kSr, engine::RenderOptions{}); }
}  // namespace

TEST_CASE("clip expression: a bend curve on a note glides its pitch while it plays", "[mpe][clip][engine]") {
    // 120 bpm: 192 ticks = 1 s. The bend rises 0 -> 12 semitones over the first second, then holds.
    const auto res = render(clipProject(R"(,"bend":[{"t":0,"v":0},{"t":192,"v":12}])"));
    REQUIRE(res.complete());
    CHECK(zeroCrossHz(res.l, 0.05, 0.1) == Approx(440.0 * std::pow(2.0, 1.2 / 12.0)).margin(12.0));          // centred on 0.1 s: 1.2 semitones up
    CHECK(zeroCrossHz(res.l, 0.45, 0.1) == Approx(440.0 * std::pow(2.0, 6.0 / 12.0)).margin(20.0));          // halfway: +6 semitones, 622 Hz
    CHECK(zeroCrossHz(res.l, 1.2, 0.3) == Approx(880.0).margin(10.0));                                          // held at +12
    // a note without curves is untouched
    const auto flat = render(clipProject(""));
    CHECK(zeroCrossHz(flat.l, 1.2, 0.3) == Approx(440.0).margin(8.0));
}

TEST_CASE("clip expression: pressure and slide curves play too, and a note's curves end with the note", "[mpe][clip][engine]") {
    const auto flat = render(clipProject(""));
    const auto pressed = render(clipProject(R"(,"pressure":[{"t":0,"v":1.0}])"));
    CHECK(rmsOver(pressed.l, 0.5, 0.5) / rmsOver(flat.l, 0.5, 0.5) == Approx(1.5).margin(0.08));
    // a curve that stays high past the note: the note (d = 576 ticks = 3 s) is over at 3 s and so are its values
    const auto longer = render(clipProject(R"(,"bend":[{"t":0,"v":12},{"t":5000,"v":12}])"));
    CHECK(zeroCrossHz(longer.l, 1.0, 0.3) == Approx(880.0).margin(10.0));
    // expression curves survive a clip's JSON round trip
    const auto j = project::projectToJson(project::importFixtureJson(clipProject(R"(,"bend":[{"t":0,"v":0},{"t":192,"v":12}],"slide":[{"t":10,"v":0.5}])")).project);
    const auto& note = j["clips"]["t1|s"]["notes"]["n000000"];
    REQUIRE(note.contains("bend"));
    CHECK(note["bend"].size() == 2);
    CHECK(note["bend"][1]["v"].get<double>() == Approx(12.0));
    CHECK(note["slide"][0]["t"].get<double>() == Approx(10.0));
    CHECK_FALSE(note.contains("pressure"));
}

TEST_CASE("clip expression: a looping clip plays its curves every pass, and a chord effect gives every tone the note's curves", "[mpe][clip][engine]") {
    // the 768-tick loop is 4 s: a note at the start with a short bend, listened to on the second pass
    const std::string looped = R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[
      {"id":"t1","kind":"synth","inst":{"type":"fmop","params":{"algo":4,"l1":1,"l2":0,"l3":0,"l4":0,"attack":0.001,"sustain":1.0,"release":0.05}},
       "fx":[],"gain":0,"pan":0}],
      "clips":{"t1|s":{"len":384,"notes":{"n":{"p":69,"s":0,"d":300,"v":0.9,"bend":[{"t":0,"v":12}]}}}}}})";
    const auto res = render(looped);                  // the scene render is two loops long
    REQUIRE(res.complete());
    REQUIRE(res.l.size() > size_t(3.0 * kSr));
    CHECK(zeroCrossHz(res.l, 0.5, 0.3) == Approx(880.0).margin(10.0));     // first pass
    CHECK(zeroCrossHz(res.l, 2.5, 0.3) == Approx(880.0).margin(10.0));     // second pass: the curve plays again
}

TEST_CASE("clip expression: a graph swap while a note sounds keeps its curves, as edited", "[mpe][clip][engine]") {
    auto play = [&](const std::string& before, const std::string& after) {
        Engine e;
        e.prepare(kSr, MasterLimiterConfig{MasterLimiterConfig::Mode::Bypass, 0});
        e.setInitialGraph(buildGraph(project::importFixtureJson(clipProject(before)), kSr, 1).graph);
        Cmd c; c.type = CmdType::ClipLaunch; c.epoch = 1; c.clipLaunch = {0, 0}; e.commands().push(c);   // launches while stopped wait for play
        Cmd t; t.type = CmdType::TransportPlay; t.epoch = 1; t.transportPlay = {0, 0.0}; e.commands().push(t);
        std::vector<float> l(128u), r(128u), out;
        auto run = [&](double sec) { for (int b = 0; b < int(sec * kSr / 128); ++b) { e.process(l.data(), r.data(), 128); out.insert(out.end(), l.begin(), l.end()); } };
        run(1.0);
        auto next = buildGraph(project::importFixtureJson(clipProject(after)), kSr, 2).graph;   // the note is edited while it sounds
        REQUIRE(e.postGraph(next));
        run(1.2);
        return out;
    };
    // the curve is replaced: +12 semitones before the swap, -12 after. Without the carry the sounding note would fall to neutral.
    auto out = play(R"(,"uid":7,"bend":[{"t":0,"v":12}])", R"(,"uid":7,"bend":[{"t":0,"v":-12}])");
    CHECK(zeroCrossHz(out, 0.5, 0.3) == Approx(880.0).margin(10.0));
    CHECK(zeroCrossHz(out, 1.6, 0.3) == Approx(220.0).margin(6.0));
    // the curve is removed in the edit: the note carries on at neutral expression
    out = play(R"(,"uid":7,"bend":[{"t":0,"v":12}])", R"(,"uid":7)");
    CHECK(zeroCrossHz(out, 1.6, 0.3) == Approx(440.0).margin(8.0));
    // another note's curves are not borrowed
    out = play(R"(,"uid":7,"bend":[{"t":0,"v":12}])", R"(,"uid":8,"bend":[{"t":0,"v":-12}])");
    CHECK(zeroCrossHz(out, 1.6, 0.3) == Approx(440.0).margin(8.0));
}
