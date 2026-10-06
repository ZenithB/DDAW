// Sidechain on dynamics devices: a comp, opto, mbcomp or gate whose source track is set detects on that track's audio (the tap
// after its effects, before pan, fader and mute) and applies the result to its own signal. The source renders first whatever
// the document order; loops, buses, missing sources and devices without a key input are reported and dropped.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <set>
#include <string>

#include "engine/GraphBuilder.h"
#include "engine/OfflineRender.h"
#include "project/ProjectJson.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::engine;
using Catch::Approx;

namespace {

constexpr double kSr = 48000.0;

// "pad": a steady sine (quiet: below the compressor's threshold on its own) with `padFx`. "key": loud notes at 0.5 s and 2.5 s (0.25 s
// long), muted by default, with `keyFx`. Document order: pad first unless keyFirst.
struct Song {
    std::string padFx, keyFx, extraTracks, masterFx = "[]";
    bool keyMuted = true, keyFirst = false;
    int keyWave = 0, keyPitch = 48;       // a saw at 130 Hz; wave 3 is a sine
    int keyGainDb = 0;
};
std::string songWith(const Song& o) {
    const std::string pad = R"({"id":"pad","kind":"synth","inst":{"type":"poly","params":{"wave":3,"attack":0.005,"sustain":1.0,"release":0.05,"cutoff":14000}},"fx":[)" + o.padFx + R"(],"gain":0,"pan":0})";
    const std::string key = R"({"id":"key","kind":"synth","inst":{"type":"poly","params":{"wave":)" + std::to_string(o.keyWave) + R"(,"attack":0.002,"sustain":1.0,"release":0.05,"cutoff":8000}},"fx":[)" + o.keyFx +
                            R"(],"gain":)" + std::to_string(o.keyGainDb) + R"(,"pan":0,"mute":)" + (o.keyMuted ? "true" : "false") + "}";
    const std::string p = std::to_string(o.keyPitch);
    return R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[)" + (o.keyFirst ? key + "," + pad : pad + "," + key) + o.extraTracks + R"(],
      "masterFx":)" + o.masterFx + R"(,
      "clips":{"pad|s":{"len":768,"notes":{"a":{"p":57,"s":0,"d":760,"v":0.12}}},
               "key|s":{"len":768,"notes":{"a":{"p":)" + p + R"(,"s":96,"d":48,"v":1.0},"b":{"p":)" + p + R"(,"s":480,"d":48,"v":1.0}}}}}})";
}
std::string songWith(const std::string& padFx) { Song s; s.padFx = padFx; return songWith(s); }
std::string comp(const std::string& src = "", const std::string& extra = "") {
    return R"({"type":"comp","on":true,"params":{"thresh":-30,"ratio":12,"attack":0.003,"release":0.05})" + (src.empty() ? "" : R"(,"srcTrack":")" + src + "\"") + extra + "}";
}
RenderResult render(const std::string& text) { return renderFixture(project::importFixtureJson(text), kSr, RenderOptions{}); }
double rms(const std::vector<float>& x, double from, double len) {
    double s = 0; const size_t a = size_t(from * kSr), n = size_t(len * kSr);
    for (size_t i = a; i < a + n && i < x.size(); ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(n));
}
std::set<std::string> issues(const std::string& text) { auto b = buildGraph(project::importFixtureJson(text), kSr, 1); return {b.unsupported.begin(), b.unsupported.end()}; }
bool mentions(const std::set<std::string>& s, const std::string& part) { for (auto& i : s) if (i.find(part) != std::string::npos) return true; return false; }
}  // namespace

TEST_CASE("sidechain: a compressor keyed by another track ducks its own signal while the key plays", "[sidechain][engine]") {
    const auto free = render(songWith(comp()));                  // no key: the pad is below its own threshold, untouched all along
    const auto keyed = render(songWith(comp("key")));
    REQUIRE(free.complete());
    REQUIRE(keyed.complete());
    const double padFree = rms(free.l, 0.2, 0.2);
    CHECK(padFree > 0.01);
    CHECK(rms(free.l, 0.6, 0.1) == Approx(padFree).epsilon(0.1));         // nothing happens when the key track plays (and it is muted)
    CHECK(rms(keyed.l, 0.2, 0.2) == Approx(padFree).epsilon(0.2));        // before the key: much as free (it sits at its threshold; the key is silent)
    const double ducked = rms(keyed.l, 0.62, 0.1);                         // the muted key track still keys it
    CHECK(ducked < 0.35 * padFree);
    CHECK(rms(keyed.l, 1.2, 0.2) == Approx(padFree).epsilon(0.15));       // released again
    CHECK(rms(keyed.l, 2.62, 0.1) < 0.35 * padFree);                       // and again at the second key note
}

TEST_CASE("sidechain: the source can come later in the document, and keys from before its fader and mute", "[sidechain][engine]") {
    Song a; a.padFx = comp("key");
    Song b = a; b.keyFirst = true;                                        // key listed first
    const auto ra = render(songWith(a)), rb = render(songWith(b));
    for (size_t i = 0; i < ra.l.size(); i += 997) REQUIRE(ra.l[i] == Approx(rb.l[i]).margin(1e-6));
    // the key track's fader does not change how hard it keys: with it at -60 dB (inaudible) the pad ducks as with it at 0 dB, muted
    Song q = a; q.keyMuted = false; q.keyGainDb = -60;
    const auto rq = render(songWith(q));
    CHECK(rms(rq.l, 0.62, 0.1) < 0.35 * rms(rq.l, 0.2, 0.2));
    CHECK(rms(rq.l, 0.62, 0.1) == Approx(rms(ra.l, 0.62, 0.1)).epsilon(0.15));
}

TEST_CASE("sidechain: the key high-pass keeps low rumble from triggering the compressor", "[sidechain][engine]") {
    Song open; open.padFx = comp("key"); open.keyWave = 3; open.keyPitch = 36;       // a 65 Hz sine key
    Song filtered = open; filtered.padFx = comp("key", R"(,"keyHpf":1000)");
    const auto ro = render(songWith(open)), rf = render(songWith(filtered));
    const double ref = rms(ro.l, 0.2, 0.2);
    CHECK(rms(ro.l, 0.62, 0.1) < 0.35 * ref);                             // unfiltered: it ducks
    CHECK(rms(rf.l, 0.62, 0.1) > 0.8 * rms(rf.l, 0.2, 0.2));             // a 1 kHz high-pass takes the 65 Hz key away: no ducking
}

TEST_CASE("sidechain: opto, multiband and gate take a key too", "[sidechain][engine]") {
    // opto: the same ducking
    {
        const auto keyed = render(songWith(R"({"type":"opto","on":true,"params":{"reduction":0.9,"gain":0,"mode":1},"srcTrack":"key"})"));
        const auto free = render(songWith(R"({"type":"opto","on":true,"params":{"reduction":0.9,"gain":0,"mode":1}})"));
        CHECK(rms(keyed.l, 0.65, 0.08) < 0.5 * rms(free.l, 0.65, 0.08));
    }
    // multiband compressor: reduces the bands the key excites (a 130 Hz saw: the low band and its harmonics)
    {
        const std::string mb = R"({"type":"mbcomp","on":true,"params":{"mode":0},"srcTrack":"key"})";
        const auto keyed = render(songWith(mb));
        const auto free = render(songWith(R"({"type":"mbcomp","on":true,"params":{"mode":0}})"));
        CHECK(rms(keyed.l, 0.65, 0.08) < 0.6 * rms(free.l, 0.65, 0.08));
    }
    // gate: the pad passes only while the key plays (it is closed with a quiet key track)
    {
        const std::string gate = R"({"type":"gate","on":true,"params":{"thresh":-30,"range":-60,"attack":0.002,"release":0.05,"hold":0.05},"srcTrack":"key"})";
        const auto keyed = render(songWith(gate));
        const auto own = render(songWith(R"({"type":"gate","on":true,"params":{"thresh":-30,"range":-60,"attack":0.002,"release":0.05,"hold":0.05}})"));
        CHECK(rms(keyed.l, 0.2, 0.2) < 0.05 * rms(own.l, 0.2, 0.2));        // key silent: shut
        CHECK(rms(keyed.l, 0.62, 0.1) > 0.5 * rms(own.l, 0.62, 0.1));       // key playing: open (the pad on its own is above the threshold)
    }
}

TEST_CASE("sidechain: a master-chain compressor can be keyed by a track", "[sidechain][engine]") {
    const std::string master = R"([{"type":"comp","on":true,"params":{"thresh":-30,"ratio":12,"attack":0.003,"release":0.05},"srcTrack":"key"}])";
    Song ms; ms.masterFx = master;
    Song mf; mf.masterFx = R"([{"type":"comp","on":true,"params":{"thresh":-30,"ratio":12,"attack":0.003,"release":0.05}}])";
    const auto keyed = render(songWith(ms));
    const auto free = render(songWith(mf));
    CHECK(rms(keyed.l, 0.62, 0.1) < 0.35 * rms(free.l, 0.62, 0.1));
    CHECK(rms(keyed.l, 0.2, 0.2) == Approx(rms(free.l, 0.2, 0.2)).epsilon(0.25));
}

TEST_CASE("sidechain: bad keys are reported and the device falls back to its own signal", "[sidechain][engine]") {
    CHECK(mentions(issues(songWith(comp("nosuch"))), "not found"));
    // a device with no sidechain input
    CHECK(mentions(issues(songWith(R"({"type":"reverb","on":true,"params":{},"srcTrack":"key"})")), "no sidechain input"));
    // a loop: the key track's compressor is keyed by the pad
    { Song l; l.padFx = comp("key"); l.keyFx = comp("pad"); CHECK(mentions(issues(songWith(l)), "loop")); }
    // a bus cannot key
    { Song b; b.padFx = comp("bus"); b.extraTracks = R"(,{"id":"bus","kind":"bus","inst":{"type":"audiobus","params":{}},"fx":[],"gain":0,"pan":0})"; CHECK(mentions(issues(songWith(b)), "bus")); }
    // the missing source falls back to detection on the pad itself: below its threshold, so it is just the free render
    const auto bad = render(songWith(comp("nosuch")));
    const auto free = render(songWith(comp()));
    REQUIRE(bad.l.size() == free.l.size());
    for (size_t i = 0; i < bad.l.size(); i += 997) REQUIRE(bad.l[i] == Approx(free.l[i]).margin(1e-6));
    // a ducker keeps its own meaning of srcTrack (note triggers): no sidechain report for it
    CHECK_FALSE(mentions(issues(songWith(R"({"type":"duck","on":true,"params":{},"srcTrack":"key"})")), "sidechain"));
}

TEST_CASE("sidechain: the project format keeps the source and the key filter", "[sidechain][engine]") {
    const auto fx = project::importFixtureJson(songWith(comp("key", R"(,"keyHpf":150)"))).project.tracks[0].fx[0];
    CHECK(fx.srcTrack == "key");
    REQUIRE(fx.keyHpf.has_value());
    CHECK(*fx.keyHpf == Approx(150.0));
    const auto j = project::projectToJson(project::importFixtureJson(songWith(comp("key", R"(,"keyHpf":150)"))).project);
    const auto& d = j["tracks"][0]["fx"][0];
    CHECK(d["srcTrack"] == "key");
    CHECK(d["keyHpf"].get<double>() == Approx(150.0));
}
