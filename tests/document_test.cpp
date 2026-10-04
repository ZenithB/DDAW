// The document model: every command round-trips through its inverse, failures are atomic, groups
// undo as one step, uids are stable across undo/redo, and the change report tells the engine session
// what to do. Plus the native format: versioned save/load, migration, and sample packages.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstring>
#include <filesystem>
#include <set>

#include "document/Document.h"
#include "document/NativeFormat.h"
#include "project/ProjectJson.h"
#include "project/SynthyyImport.h"

using namespace ddaw;
using namespace ddaw::document;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const char* kBase = R"({"project":{"meta":{"bpm":120,"root":9,"scale":"minor"},"scenes":[{"id":"s1"},{"id":"s2"}],
  "tracks":[
    {"id":"lead","name":"Lead","kind":"synth","inst":{"type":"mono","params":{"cutoff":900}},
     "fx":[{"type":"delay","on":true,"params":{"time":0.3}},{"id":"c1","type":"comp","on":true,"params":{}}],
     "midifx":[{"type":"arp","on":true,"params":{}}],"gain":-3,"pan":0.1,"mute":false,"solo":false,"sendA":0,"sendB":0,
     "lfos":[{"id":"l1","on":true,"shape":0,"sync":false,"rate":5,"hz":2,"depth":0.3,"phase":0,"dest":"mix","fxId":"","pkey":"pan"}],
     "macros":[{"name":"m","value":0.5,"targets":[]}]},
    {"id":"bass","name":"Bass","kind":"synth","inst":{"type":"poly","params":{}},"fx":[],"gain":0,"pan":0,"mute":false,"solo":false,"sendA":0,"sendB":0}],
  "clips":{"lead|s1":{"len":192,"notes":{"a":{"p":60,"s":0,"d":24,"v":0.8},"b":{"p":64,"s":48,"d":24,"v":0.7}},"env":{"inst||cutoff":[{"t":0,"v":0.2},{"t":96,"v":0.8}]}},
           "bass|s1":{"len":96,"notes":{"a":{"p":36,"s":0,"d":48,"v":1.0}}}},
  "arr":{"a1":{"trackId":"lead","start":384,"clip":{"len":96,"notes":{"n":{"p":67,"s":0,"d":24,"v":1}}}}},
  "returns":[{"id":"r1","name":"Verb","fxType":"reverb","params":{},"gain":-6}],
  "masterFx":[{"type":"comp","on":true,"params":{}}],"masterAuto":{"mix||gain":[{"t":0,"v":0.5}]}}})";

Document makeDoc() { return Document(project::importFixtureJson(kBase).project); }
json dump(const Document& d) { return project::projectToJson(d.project()); }
Command C(const char* kind, json args) { return Command{kind, std::move(args)}; }

project::Uid trackUid(const Document& d, const char* id) { for (auto& t : d.project().tracks) if (t.id == id) return t.uid; return 0; }

// Apply `c`, check that the document changed, then undo and check it is restored exactly, then redo
// and check the same state as after the first apply (including identical uids).
void roundTrip(Document& d, const Command& c) {
    INFO("command " << c.kind);
    const json before = dump(d);
    d.apply(c);
    const json after = dump(d);
    CHECK(after != before);
    d.undo();
    CHECK(dump(d) == before);
    d.redo();
    CHECK(dump(d) == after);
    d.undo();
    CHECK(dump(d) == before);
}

}  // namespace

TEST_CASE("a document assigns stable uids to everything addressable", "[document]") {
    Document d = makeDoc();
    CHECK(project::maxUid(d.project()) > 0);
    project::Uid next = d.nextUid();
    CHECK(project::assignUids(const_cast<project::Project&>(d.project()), next) == 0);   // nothing left without an id
    std::set<project::Uid> seen;
    for (auto& t : d.project().tracks) { CHECK(seen.insert(t.uid).second); CHECK(seen.insert(t.inst.uid).second); for (auto& f : t.fx) CHECK(seen.insert(f.uid).second); }
    for (auto& [k, c] : d.project().clips) for (auto& n : c.notes) CHECK(seen.insert(n.uid).second);
}

TEST_CASE("every command is undoable to the exact previous project", "[document]") {
    Document d = makeDoc();
    const auto lead = trackUid(d, "lead"), bass = trackUid(d, "bass");
    const auto delayUid = d.project().tracks[0].fx[0].uid, instUid = d.project().tracks[0].inst.uid;
    const auto noteUid = d.project().clips.at("lead|s1").notes[0].uid;

    roundTrip(d, C("meta.set", {{"field", "bpm"}, {"value", 140}}));
    roundTrip(d, C("meta.set", {{"field", "scale"}, {"value", "dorian"}}));
    roundTrip(d, C("meta.set", {{"field", "loopOn"}, {"value", true}}));
    roundTrip(d, C("track.set", {{"uid", lead}, {"field", "gain"}, {"value", -9.0}}));
    roundTrip(d, C("track.set", {{"uid", lead}, {"field", "mute"}, {"value", true}}));
    roundTrip(d, C("track.set", {{"uid", bass}, {"field", "name"}, {"value", "Sub"}}));
    roundTrip(d, C("track.set", {{"uid", bass}, {"field", "send"}, {"value", "A"}}));
    roundTrip(d, C("track.move", {{"uid", bass}, {"index", 0}}));
    roundTrip(d, C("track.insert", {{"index", 1}, {"track", {{"id", "pad"}, {"name", "Pad"}, {"kind", "synth"}, {"inst", {{"type", "keys"}, {"params", json::object()}}}, {"fx", json::array()}, {"gain", 0}, {"pan", 0}}}}));
    roundTrip(d, C("track.remove", {{"uid", lead}}));        // removes its clips and arrangement clips too, and restores them
    roundTrip(d, C("device.param", {{"uid", delayUid}, {"key", "time"}, {"value", 0.6}}));
    roundTrip(d, C("device.param", {{"uid", delayUid}, {"key", "time"}, {"value", nullptr}}));
    roundTrip(d, C("device.param", {{"uid", instUid}, {"key", "res"}, {"value", 4.0}}));
    roundTrip(d, C("device.set", {{"uid", delayUid}, {"field", "on"}, {"value", false}}));
    roundTrip(d, C("device.set", {{"uid", delayUid}, {"field", "out"}, {"value", -4.0}}));
    roundTrip(d, C("device.insert", {{"chain", "fx"}, {"track", lead}, {"index", 1}, {"device", {{"type", "chorus"}, {"on", true}, {"params", json::object()}}}}));
    roundTrip(d, C("device.insert", {{"chain", "midifx"}, {"track", lead}, {"index", 0}, {"device", {{"type", "scale"}, {"on", true}, {"params", json::object()}}}}));
    roundTrip(d, C("device.insert", {{"chain", "master"}, {"index", 0}, {"device", {{"type", "opto"}, {"on", true}, {"params", json::object()}}}}));
    roundTrip(d, C("device.remove", {{"uid", delayUid}}));
    roundTrip(d, C("inst.set", {{"track", lead}, {"device", {{"type", "fm"}, {"params", {{"harm", 2.0}}}}}}));
    roundTrip(d, C("scene.insert", {{"index", 1}, {"id", "s3"}}));
    roundTrip(d, C("scene.remove", {{"id", "s1"}}));         // removes the scene's clips, and restores them
    roundTrip(d, C("clip.set", {{"track", lead}, {"scene", "s2"}, {"clip", {{"len", 96}, {"notes", {{"x", {{"p", 50}, {"s", 0}, {"d", 12}, {"v", 1}}}}}}}}));
    roundTrip(d, C("clip.set", {{"track", lead}, {"scene", "s1"}, {"clip", nullptr}}));
    roundTrip(d, C("arrclip.set", {{"key", "a2"}, {"arr", {{"trackId", "bass"}, {"start", 96}, {"clip", {{"len", 96}, {"notes", json::object()}}}}}}));
    roundTrip(d, C("arrclip.set", {{"key", "a1"}, {"arr", nullptr}}));
    const json clipRef = {{"track", lead}, {"scene", "s1"}};
    roundTrip(d, C("note.add", {{"clip", clipRef}, {"note", {{"p", 72}, {"s", 96}, {"d", 12}, {"v", 0.9}}}}));
    roundTrip(d, C("note.add", {{"clip", clipRef}, {"index", 0}, {"note", {{"p", 72}, {"s", 96}, {"d", 12}, {"v", 0.9}}}}));
    roundTrip(d, C("note.remove", {{"clip", clipRef}, {"uid", noteUid}}));
    roundTrip(d, C("note.edit", {{"clip", clipRef}, {"uid", noteUid}, {"note", {{"p", 61}, {"s", 5}, {"d", 30}, {"v", 0.5}}}}));
    roundTrip(d, C("note.add", {{"clip", {{"arr", "a1"}}}, {"note", {{"p", 70}, {"s", 24}, {"d", 12}, {"v", 1.0}}}}));
    roundTrip(d, C("env.set", {{"scope", clipRef}, {"key", "inst||cutoff"}, {"points", json::array({{{"t", 0}, {"v", 1}}})}}));
    roundTrip(d, C("env.set", {{"scope", clipRef}, {"key", "inst||cutoff"}, {"points", nullptr}}));
    roundTrip(d, C("env.set", {{"scope", {{"track", lead}}}, {"key", "mix||pan"}, {"points", json::array({{{"t", 0}, {"v", 0.3}}})}}));
    roundTrip(d, C("env.set", {{"scope", {{"master", true}}}, {"key", "mix||gain"}, {"points", nullptr}}));
    roundTrip(d, C("lfo.insert", {{"track", lead}, {"index", 1}, {"lfo", {{"id", "l2"}, {"hz", 4}, {"depth", 0.2}}}}));
    roundTrip(d, C("lfo.edit", {{"track", lead}, {"index", 0}, {"lfo", {{"id", "l1"}, {"hz", 7}, {"depth", 0.9}}}}));
    roundTrip(d, C("lfo.remove", {{"track", lead}, {"index", 0}}));
    roundTrip(d, C("macro.insert", {{"track", lead}, {"index", 1}, {"macro", {{"name", "m2"}, {"value", 0.1}, {"targets", json::array()}}}}));
    roundTrip(d, C("macro.edit", {{"track", lead}, {"index", 0}, {"macro", {{"name", "m"}, {"value", 0.9}, {"targets", json::array()}}}}));
    roundTrip(d, C("macro.remove", {{"track", lead}, {"index", 0}}));
    roundTrip(d, C("return.insert", {{"index", 1}, {"ret", {{"name", "Echo"}, {"fxType", "delay"}, {"params", json::object()}, {"gain", 0}}}}));
    roundTrip(d, C("return.edit", {{"index", 0}, {"ret", {{"name", "Verb2"}, {"fxType", "plate"}, {"params", {{"decay", 0.5}}}, {"gain", -3}}}}));
    roundTrip(d, C("return.remove", {{"index", 0}}));
}

TEST_CASE("a failed command changes nothing, not even history", "[document]") {
    Document d = makeDoc();
    const json before = dump(d);
    const auto rev = d.revision();
    CHECK_THROWS_AS(d.apply(C("track.set", {{"uid", 999999}, {"field", "gain"}, {"value", 1}})), std::invalid_argument);
    CHECK_THROWS_AS(d.apply(C("track.set", {{"uid", trackUid(d, "lead")}, {"field", "nonsense"}, {"value", 1}})), std::invalid_argument);
    CHECK_THROWS_AS(d.apply(C("meta.set", {{"field", "bpm"}, {"value", 5}})), std::invalid_argument);          // out of range
    CHECK_THROWS_AS(d.apply(C("meta.set", {{"field", "tsBottom"}, {"value", 5}})), std::invalid_argument);
    CHECK_THROWS_AS(d.apply(C("track.insert", {{"index", 0}, {"track", {{"id", "lead"}, {"inst", {{"type", "mono"}}}}}})), std::invalid_argument);   // duplicate id
    CHECK_THROWS_AS(d.apply(C("device.remove", {{"uid", d.project().tracks[0].inst.uid}})), std::invalid_argument);   // instruments are replaced, not removed
    CHECK_THROWS_AS(d.apply(C("note.remove", {{"clip", {{"track", trackUid(d, "lead")}, {"scene", "s1"}}}, {"uid", 424242}})), std::invalid_argument);
    CHECK_THROWS_AS(d.apply(C("no.such.command", json::object())), std::invalid_argument);
    CHECK(dump(d) == before);
    CHECK(d.revision() == rev);
    CHECK_FALSE(d.canUndo());

    // a compound that fails halfway rolls back what it already did
    Command bad = C("compound", {{"label", "x"}, {"commands", json::array({
        {{"kind", "meta.set"}, {"args", {{"field", "bpm"}, {"value", 150}}}},
        {{"kind", "track.set"}, {"args", {{"uid", 999999}, {"field", "gain"}, {"value", 1}}}}})}});
    CHECK_THROWS(d.apply(bad));
    CHECK(dump(d) == before);
}

TEST_CASE("groups undo and redo as one step", "[document]") {
    Document d = makeDoc();
    const json before = dump(d);
    const auto lead = trackUid(d, "lead");
    d.beginGroup("tweak lead");
    d.apply(cmd::setTrack(lead, "gain", -12.0));
    d.apply(cmd::setTrack(lead, "pan", -0.5));
    d.apply(cmd::setMeta("bpm", 100));
    const ChangeInfo info = d.endGroup();
    CHECK(info.tempo.value() == 100);
    CHECK(info.params.size() == 2);
    CHECK(d.undoLabel() == "tweak lead");
    const json after = dump(d);
    CHECK(after != before);
    d.undo();
    CHECK(dump(d) == before);                               // one undo reverts all three
    CHECK_FALSE(d.canUndo());
    d.redo();
    CHECK(dump(d) == after);
    d.beginGroup("a");
    CHECK_THROWS_AS(d.beginGroup("b"), std::logic_error);   // groups do not nest
    d.endGroup();
}

TEST_CASE("a new edit clears redo, and uids stay stable through undo and redo", "[document]") {
    Document d = makeDoc();
    const auto lead = trackUid(d, "lead");
    const json clipRef = {{"track", lead}, {"scene", "s1"}};
    Command applied;
    d.apply(C("note.add", {{"clip", clipRef}, {"note", {{"p", 72}, {"s", 96}, {"d", 12}, {"v", 0.9}}}}), &applied);
    const auto uid = applied.args["note"]["uid"].get<project::Uid>();
    CHECK(uid != 0);                                         // the applied command carries the uid that was assigned
    d.undo();
    d.redo();
    CHECK(d.project().clips.at("lead|s1").notes.back().uid == uid);   // redo restores the same identity
    d.undo();
    CHECK(d.canRedo());
    d.apply(cmd::setMeta("title", "x"));
    CHECK_FALSE(d.canRedo());
    // the applied (normalised) command replays to the same result on a second document
    Document other = makeDoc();
    Document source = makeDoc();
    Command a2;
    source.apply(C("note.add", {{"clip", clipRef}, {"note", {{"p", 72}, {"s", 96}, {"d", 12}, {"v", 0.9}}}}), &a2);
    other.apply(a2);
    CHECK(dump(other) == dump(source));
}

TEST_CASE("the change report separates live parameter edits from rebuilds", "[document]") {
    Document d = makeDoc();
    const auto lead = trackUid(d, "lead");
    const auto instUid = d.project().tracks[0].inst.uid;
    const auto delayUid = d.project().tracks[0].fx[0].uid, compUid = d.project().tracks[0].fx[1].uid;
    const auto masterUid = d.project().masterFx[0].uid, arpUid = d.project().tracks[0].midifx[0].uid;

    ChangeInfo i = d.apply(cmd::setParam(instUid, "cutoff", 1200));
    REQUIRE(i.params.size() == 1);
    CHECK_FALSE(i.structural);
    CHECK(i.params[0].key == "lead|inst|cutoff");
    CHECK(i.params[0].value == 1200);
    i = d.apply(cmd::setParam(delayUid, "time", 0.5));
    CHECK(i.params[0].key == "lead|delay|time");              // fxId falls back to the type
    i = d.apply(cmd::setParam(compUid, "ratio", 8));
    CHECK(i.params[0].key == "lead|c1|ratio");                // an explicit id wins
    i = d.apply(cmd::setParam(masterUid, "thresh", -20));
    CHECK(i.params[0].key == "master|comp|thresh");
    i = d.apply(cmd::setTrack(lead, "gain", -6));
    CHECK(i.params[0].key == "lead|mix|gain");
    i = d.apply(cmd::setTrack(lead, "pan", 0.4));
    CHECK(i.params[0].key == "lead|mix|pan");
    i = d.apply(cmd::setMeta("bpm", 90));
    CHECK(i.tempo.value() == 90);
    CHECK_FALSE(i.structural);
    i = d.apply(cmd::setMeta("masterGain", -2));
    CHECK(i.params[0].key == "master|gain");
    i = d.apply(cmd::setMeta("title", "renamed"));
    CHECK_FALSE(i.changed());                                 // no engine effect at all
    CHECK(d.apply(cmd::setParam(arpUid, "rate", 2)).structural);      // MIDI fx are not live-addressable
    CHECK(d.apply(cmd::setTrack(lead, "mute", true)).structural);
    CHECK(d.apply(C("device.set", {{"uid", delayUid}, {"field", "on"}, {"value", false}})).structural);
    CHECK(d.apply(cmd::setParam(delayUid, "time", 0.7)).structural);  // a bypassed device has no live slot
    CHECK(d.apply(C("note.add", {{"clip", {{"track", lead}, {"scene", "s1"}}}, {"note", {{"p", 60}, {"s", 0}, {"d", 10}, {"v", 1}}}})).structural);
    CHECK(d.apply(cmd::setMeta("swing", 0.3)).structural);
}

// ---------------------------------------------------------------- native format

TEST_CASE("native format: save, load, and a synthyy project loads as format 0", "[document][format]") {
    Document d = makeDoc();
    const std::string text = saveProjectJson(d.project(), d.nextUid());
    const auto loaded = loadProjectJson(text);
    CHECK(loaded.fileVersion == kFormatVersion);
    CHECK_FALSE(loaded.migrated);
    CHECK(project::projectToJson(loaded.project) == dump(d));
    CHECK(loaded.nextUid == d.nextUid());

    // a headerless synthyy project migrates (uids assigned), and so does a synthyy fixture wrapper
    const auto syn = loadProjectJson(R"({"meta":{"bpm":100},"tracks":[{"id":"a","inst":{"type":"mono","params":{}},"fx":[]}],"scenes":[],"clips":{}})");
    CHECK(syn.fileVersion == 0);
    CHECK(syn.migrated);
    CHECK(syn.project.tracks[0].uid != 0);
    CHECK_FALSE(syn.notes.empty());
    const auto fix = loadProjectJson(kBase);
    CHECK(fix.migrated);
    CHECK(fix.project.tracks.size() == 2);
}

TEST_CASE("native format: refuses what it cannot read, and never reuses an id", "[document][format]") {
    CHECK_THROWS_WITH(loadProjectJson("{not json"), Catch::Matchers::ContainsSubstring("not valid JSON"));
    CHECK_THROWS_WITH(loadProjectJson(R"({"hello":1})"), Catch::Matchers::ContainsSubstring("not a DDAW or synthyy project"));
    CHECK_THROWS_WITH(loadProjectJson(R"({"ddaw":{"format":"something-else","version":1},"project":{"tracks":[]}})"), Catch::Matchers::ContainsSubstring("unknown format"));
    CHECK_THROWS_WITH(loadProjectJson(R"({"ddaw":{"format":"ddaw-project","version":99},"project":{"tracks":[]}})"), Catch::Matchers::ContainsSubstring("newer DDAW"));
    // a stale nextUid in the header cannot cause an id to be handed out twice
    Document d = makeDoc();
    json doc = json::parse(saveProjectJson(d.project(), d.nextUid()));
    doc["ddaw"]["nextUid"] = 1;
    const auto loaded = loadProjectJson(doc.dump());
    CHECK(loaded.nextUid > project::maxUid(loaded.project));
}

TEST_CASE("migrations run as an ordered chain, step by step", "[document][format]") {
    MigrationChain chain;
    chain.add(0, [](json& doc, std::vector<std::string>& notes) { if (!doc.contains("project")) doc = json{{"project", doc}}; notes.push_back("0->1"); });
    chain.add(1, [](json& doc, std::vector<std::string>& notes) { doc["project"]["meta"]["title"] = "migrated"; notes.push_back("1->2"); });
    chain.add(2, [](json& doc, std::vector<std::string>& notes) { doc["project"]["meta"]["bpm"] = 111; notes.push_back("2->3"); });
    json doc = json::parse(R"({"meta":{"bpm":100},"tracks":[]})");
    std::vector<std::string> notes;
    chain.run(doc, 0, 3, notes);
    CHECK(notes == std::vector<std::string>{"0->1", "1->2", "2->3"});
    CHECK(doc["project"]["meta"]["title"] == "migrated");
    CHECK(doc["project"]["meta"]["bpm"] == 111);
    json again = doc;
    notes.clear();
    chain.run(again, 2, 3, notes);                           // starting mid-chain only runs the remaining step
    CHECK(notes == std::vector<std::string>{"2->3"});
    CHECK_THROWS(chain.run(doc, 3, 5, notes));               // a missing step is an error, not a silent skip
    CHECK_THROWS(chain.run(doc, 4, 3, notes));               // downgrading is refused
}

TEST_CASE("native package: project.json plus sample files, lossless", "[document][format]") {
    const fs::path dir = fs::temp_directory_path() / "ddaw_pkg_test";
    fs::remove_all(dir);
    Document d = makeDoc();
    project::Project p = d.project();
    p.tracks[0].inst = project::DeviceSpec{};
    p.tracks[0].inst.type = "sampler";
    p.tracks[0].inst.sampleId = "kick";
    p.tracks[1].inst.padSamples["3"] = "snare";
    p.clips["bass|s2"].audio = project::AudioClipData{};
    p.clips["bass|s2"].audio->sampleId = "loop";   // referenced but absent from the bank: listed, no file
    project::Uid next = d.nextUid();
    project::assignUids(p, next);                   // a saved project always has its ids assigned

    project::SampleBank bank;
    auto kick = std::make_shared<SampleBuf>();
    kick->sampleRate = 48000.0f;
    for (int i = 0; i < 4800; ++i) kick->l.push_back(std::sin(0.05f * float(i)) * std::exp(-float(i) / 800.0f));
    auto snare = std::make_shared<SampleBuf>();   // stereo, 22.05 kHz
    snare->sampleRate = 22050.0f;
    for (int i = 0; i < 2000; ++i) { snare->l.push_back(0.5f * std::sin(0.3f * float(i))); snare->r.push_back(-0.25f * std::sin(0.31f * float(i))); }
    bank.put("kick", kick);
    bank.put("snare", snare);

    saveProjectPackage(dir.string(), p, next, bank, {{"kick", "Kick 808.wav"}});
    CHECK(fs::exists(dir / "project.json"));
    CHECK(fs::exists(dir / "samples" / "kick.wav"));
    CHECK(fs::exists(dir / "samples" / "snare.wav"));
    CHECK_FALSE(fs::exists(dir / "samples" / "loop.wav"));

    project::SampleBank loadedBank;
    const auto loaded = loadProjectPackage(dir.string(), loadedBank);
    CHECK(project::projectToJson(loaded.project) == project::projectToJson(p));
    REQUIRE(loadedBank.contains("kick"));
    REQUIRE(loadedBank.contains("snare"));
    CHECK_FALSE(loadedBank.contains("loop"));
    CHECK(loadedBank.get("kick")->l == kick->l);                    // float32 WAV is bit-exact
    CHECK(loadedBank.get("snare")->l == snare->l);
    CHECK(loadedBank.get("snare")->r == snare->r);
    CHECK(loadedBank.get("snare")->sampleRate == 22050.0f);
    CHECK(loadedBank.get("kick")->r.empty());                       // mono stays mono
    bool noted = false;
    for (auto& n : loaded.notes) noted |= n.find("'loop'") != std::string::npos;
    CHECK(noted);                                                    // the missing sample is reported, not fatal
    fs::remove_all(dir);
}

TEST_CASE("WAV decoding: 16, 24, 32-bit and float, mono and stereo", "[document][format]") {
    auto wav = [](uint16_t fmt, uint16_t bits, uint16_t ch, const std::vector<unsigned char>& data) {
        std::vector<unsigned char> o;
        auto w32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<unsigned char>(x >> (8 * i))); };
        auto w16 = [&](uint16_t x) { o.push_back(static_cast<unsigned char>(x)); o.push_back(static_cast<unsigned char>(x >> 8)); };
        o.insert(o.end(), {'R', 'I', 'F', 'F'}); w32(uint32_t(36 + data.size())); o.insert(o.end(), {'W', 'A', 'V', 'E'});
        o.insert(o.end(), {'f', 'm', 't', ' '}); w32(16); w16(fmt); w16(ch); w32(8000); w32(8000u * ch * bits / 8); w16(uint16_t(ch * bits / 8)); w16(bits);
        o.insert(o.end(), {'d', 'a', 't', 'a'}); w32(uint32_t(data.size()));
        o.insert(o.end(), data.begin(), data.end());
        return o;
    };
    // 16-bit mono: 0, +full, -full
    auto b16 = project::decodeWavSample(wav(1, 16, 1, {0, 0, 0xFF, 0x7F, 0x00, 0x80}));
    REQUIRE(b16->frames() == 3);
    CHECK(b16->l[0] == 0.0f); CHECK(b16->l[1] == Catch::Approx(32767.0f / 32768.0f)); CHECK(b16->l[2] == -1.0f);
    CHECK(b16->r.empty()); CHECK(b16->sampleRate == 8000.0f);
    // 24-bit stereo: one frame, L = +0.5, R = -0.5
    auto b24 = project::decodeWavSample(wav(1, 24, 2, {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0}));
    CHECK(b24->l[0] == Catch::Approx(0.5f)); CHECK(b24->r[0] == Catch::Approx(-0.5f));
    // 32-bit PCM
    auto b32 = project::decodeWavSample(wav(1, 32, 1, {0x00, 0x00, 0x00, 0x40}));
    CHECK(b32->l[0] == Catch::Approx(0.5f));
    // float
    float f = 0.25f; unsigned char fb[4]; std::memcpy(fb, &f, 4);
    auto bf = project::decodeWavSample(wav(3, 32, 1, {fb[0], fb[1], fb[2], fb[3]}));
    CHECK(bf->l[0] == 0.25f);
    CHECK_THROWS(project::decodeWavSample({1, 2, 3}));                                 // not a WAV
    CHECK_THROWS(project::decodeWavSample(wav(2, 4, 1, {0, 0})));                      // ADPCM-style: unsupported
    // out-of-range reads are silence, never a crash
    float l, r;
    b16->readLin(-1.0, l, r); CHECK(l == 0.0f);
    b16->readLin(99.0, l, r); CHECK(l == 0.0f);
    b16->readLin(0.5, l, r);  CHECK(l == Catch::Approx(0.5f * 32767.0f / 32768.0f));
}
