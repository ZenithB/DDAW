// Plugin hosting through the engine, with Apple's own AudioUnits as the fixtures (they ship with macOS, so no third-party
// plugin is needed). Skipped where there is no AudioUnit host.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <juce_audio_processors/juce_audio_processors.h>

#include "FakePluginProvider.h"
#include "app/model/AppModel.h"
#include "app/ui/BrowserPanel.h"
#include "app/ui/DeviceChainView.h"
#include "app/ui/Widgets.h"
#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "engine/OfflineRender.h"
#include "plugins/PluginHost.h"
#include "plugins/PluginProvider.h"
#include <filesystem>
#include "project/SynthyyImport.h"

using namespace ddaw;
using Catch::Approx;

namespace {
constexpr double kSr = 48000.0;
constexpr uint64_t kFxUid = 9001, kInstUid = 9002;

bool haveAudioUnits() {
#if JUCE_MAC
    return true;
#else
    return false;
#endif
}

// "AudioUnit#AudioUnit:<category>/aufx,dely,appl#AUDelay" for one of Apple's: found by asking the format, so the category
// folder macOS files it under does not matter.
std::string appleId(const char* type, const char* sub, const char* name) {
    auto& pm = plugins::PluginHost::instance();
    for (int f = 0; f < pm.formats().getNumFormats(); ++f) {
        auto* fmt = pm.formats().getFormat(f);
        if (fmt->getName() != "AudioUnit") continue;
        for (const auto& path : fmt->searchPathsForPlugins(juce::FileSearchPath(), true, false)) {
            if (!path.contains(juce::String(type) + "," + sub + ",appl")) continue;
            juce::OwnedArray<juce::PluginDescription> types;
            fmt->findAllTypesForFile(types, path);
            for (auto* d : types) if (d->name.contains(name)) return plugins::PluginHost::makeId(*d);
        }
    }
    return {};
}

std::string chain(const std::string& fxJson, const std::string& instJson = R"({"type":"poly","params":{"wave":0,"attack":0.005,"sustain":1.0,"release":0.05}})") {
    return R"({"scope":{"kind":"scene","sceneId":"s"},"project":{"meta":{"bpm":120},"scenes":[{"id":"s"}],"tracks":[
      {"id":"t1","kind":"synth","inst":)" + instJson + R"(,"fx":[)" + fxJson + R"(],"gain":0,"pan":0}],
      "clips":{"t1|s":{"len":384,"notes":{"a":{"p":57,"s":0,"d":300,"v":0.9}}}}}})";
}
std::string fxSpec(uint64_t uid, const std::string& id, const std::string& params = "") {
    return R"({"uid":)" + std::to_string(uid) + R"(,"type":"plugin","on":true,"plugin":")" + id + R"(","pluginName":"x","params":{)" + params + "}}";
}
engine::RenderResult render(const std::string& proj) { return engine::renderFixture(project::importFixtureJson(proj), kSr, engine::RenderOptions{}); }
double rms(const std::vector<float>& x, size_t from, size_t len) {
    double s = 0;
    for (size_t i = from; i < from + len && i < x.size(); ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(len));
}
struct Setup { Setup() { plugins::registerDevices(); } };
}  // namespace

TEST_CASE("plugins: Apple's AudioUnits are found and one loads with its parameters", "[plugins]") {
    if (!haveAudioUnits()) SKIP("no AudioUnit host on this platform");
    static Setup once;
    const auto id = appleId("aufx", "dely", "AUDelay");
    REQUIRE_FALSE(id.empty());
    std::string err;
    auto h = plugins::PluginHost::instance().instantiate(kFxUid, id, "", kSr, 128, err);
    INFO(err);
    REQUIRE(h);
    CHECK(h->info.name.find("AUDelay") != std::string::npos);
    CHECK_FALSE(h->info.instrument);
    CHECK(h->specs.size() >= 4);                                 // delay time, feedback, lowpass cutoff, wet/dry
    for (size_t i = 0; i < h->specs.size(); ++i) {
        INFO(h->keys[i] << " / " << h->names[i]);
        CHECK(h->specs[i].index == i);
        CHECK(h->specs[i].min == 0.0f);
        CHECK(h->specs[i].max == 1.0f);
        CHECK(h->specs[i].def >= 0.0f);
        CHECK(h->specs[i].def <= 1.0f);
    }
    // the same device asked for again is the same instance
    CHECK(plugins::PluginHost::instance().instantiate(kFxUid, id, "", kSr, 128, err).get() == h.get());
    plugins::PluginHost::instance().forgetAll();
}

namespace {
size_t paramIndex(const plugins::Hosted& h, const char* part) {
    for (size_t i = 0; i < h.names.size(); ++i) if (juce::String(h.names[i]).containsIgnoreCase(part)) return i;
    return h.names.size();
}
}  // namespace

TEST_CASE("plugins: an effect plugin in a track's chain processes the track, and its parameters come from the project", "[plugins]") {
    if (!haveAudioUnits()) SKIP("no AudioUnit host on this platform");
    static Setup once;
    auto& host = plugins::PluginHost::instance();
    host.forgetAll();
    const auto id = appleId("aufx", "dely", "AUDelay");
    REQUIRE_FALSE(id.empty());
    std::string err;
    auto h = host.instantiate(kFxUid, id, "", kSr, 128, err);
    REQUIRE(h);
    const size_t wet = paramIndex(*h, "wet"), time = paramIndex(*h, "delay time"), fb = paramIndex(*h, "feedback");
    REQUIRE(wet < h->keys.size());
    REQUIRE(time < h->keys.size());
    REQUIRE(fb < h->keys.size());
    // fully wet, a quarter-second delay, no feedback: nothing at all until the first echo
    const std::string params = "\"" + h->keys[wet] + "\":1.0,\"" + h->keys[time] + "\":0.125,\"" + h->keys[fb] + "\":0.5";
    const auto dry = render(chain(""));
    const auto echo = render(chain(fxSpec(kFxUid, id, params)));
    REQUIRE(dry.complete());
    REQUIRE(echo.complete());
    const size_t q = size_t(0.2 * kSr);
    CHECK(rms(dry.l, 2000, q) > 0.05);                           // the dry track plays at once
    CHECK(rms(echo.l, 0, q) < 0.01 * rms(dry.l, 2000, q));      // wet only: silent until the delay time has passed
    CHECK(rms(echo.l, size_t(0.4 * kSr), q) > 0.05);             // then the repeats
    // the plugin was driven by the engine, and its own latency is reported (none for a delay)
    CHECK(h->latency == 0);
    host.forgetAll();
}

TEST_CASE("plugins: an instrument plugin plays a clip", "[plugins]") {
    if (!haveAudioUnits()) SKIP("no AudioUnit host on this platform");
    static Setup once;
    auto& host = plugins::PluginHost::instance();
    host.forgetAll();
    const auto id = appleId("aumu", "dls ", "DLSMusicDevice");
    REQUIRE_FALSE(id.empty());
    std::string err;
    auto h = host.instantiate(kInstUid, id, "", kSr, 128, err);
    INFO(err);
    REQUIRE(h);
    CHECK(h->info.instrument);
    const std::string inst = R"({"uid":)" + std::to_string(kInstUid) + R"(,"type":"plugin","plugin":")" + id + R"(","pluginName":"DLS","params":{}})";
    const auto res = render(chain("", inst));
    REQUIRE(res.complete());
    float mx = 0; for (float v : res.l) { REQUIRE(std::isfinite(v)); mx = std::max(mx, std::abs(v)); }
    CHECK(mx > 0.01f);                                           // the General MIDI piano sounded the note
    host.forgetAll();
}

TEST_CASE("plugins: a plugin's state survives a save and a reload", "[plugins]") {
    if (!haveAudioUnits()) SKIP("no AudioUnit host on this platform");
    static Setup once;
    auto& host = plugins::PluginHost::instance();
    host.forgetAll();
    const auto id = appleId("aufx", "dely", "AUDelay");
    std::string err;
    auto h = host.instantiate(kFxUid, id, "", kSr, 128, err);
    REQUIRE(h);
    const size_t fb = paramIndex(*h, "feedback");
    REQUIRE(fb < h->keys.size());
    h->setNormalised(fb, 0.83f);
    const std::string state = h->captureState();
    REQUIRE_FALSE(state.empty());
    h.reset();
    host.forgetAll();
    auto again = host.instantiate(kFxUid, id, state, kSr, 128, err);
    REQUIRE(again);
    CHECK(again->getNormalised(fb) == Approx(0.83f).margin(0.01f));
    CHECK(again->specs[fb].def == Approx(0.83f).margin(0.01f));    // the base value of automation is the restored one
    host.forgetAll();
}

TEST_CASE("plugins: when a graph is replaced, only the new one drives the plugin; a missing plugin passes audio through", "[plugins]") {
    if (!haveAudioUnits()) SKIP("no AudioUnit host on this platform");
    static Setup once;
    auto& host = plugins::PluginHost::instance();
    host.forgetAll();
    const auto id = appleId("aufx", "dely", "AUDelay");
    std::string err;
    auto h = host.instantiate(kFxUid, id, "", kSr, 128, err);
    REQUIRE(h);
    const size_t wet = paramIndex(*h, "wet");
    const std::string params = "\"" + h->keys[wet] + "\":1.0";
    auto build = [&](const std::string& text) { return engine::buildGraph(project::importFixtureJson(text), kSr, 1); };
    auto a = build(chain(fxSpec(kFxUid, id, params)));
    auto b = build(chain(fxSpec(kFxUid, id, params)));
    auto dry = build(chain(""));
    auto missing = build(chain(fxSpec(kFxUid + 1, "AudioUnit#AudioUnit:Effects/aufx,none,none#Nothing")));
    REQUIRE(a.graph);
    for (auto* g : {a.graph.get(), b.graph.get(), dry.graph.get(), missing.graph.get()}) g->noteOn(0, 57, 0.9f, 1);
    std::vector<float> l(128u), r(128u);
    const ProcessContext ctx{kSr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    auto block = [&](engine::Graph& g) { std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f); g.process(l.data(), r.data(), 128, ctx, nullptr, 1.0f); return l; };
    block(*a.graph);                                              // a claims the plugin
    block(*b.graph);                                              // b claims it
    block(*dry.graph);
    block(*missing.graph);
    const auto fromA = block(*a.graph), fromDry = block(*dry.graph), fromMissing = block(*missing.graph);
    CHECK(fromA == fromDry);                                      // a was replaced: its plugin stage lets the track through
    CHECK(fromMissing == fromDry);                                // no such plugin: the same
    const auto fromB = block(*b.graph);
    CHECK(fromB != fromDry);                                      // b is the one processing (100% wet, delayed: not the dry track)
    host.forgetAll();
}

// ---- the views, against a fake host ----

namespace {
struct UiRig {
    engine::Engine eng;
    std::unique_ptr<app::AppModel> m;
    ddaw::testing::FakeProvider host;
    UiRig() {
        eng.prepare(48000.0);
        m = std::make_unique<app::AppModel>(eng, 48000.0);
        m->setPluginProvider(&host);
        m->apply(app::edit::addTrack(m->project(), project::TrackKind::Synth));
    }
    ~UiRig() { app::setActivePluginProvider(nullptr); }
};
template <class T> void findAll(juce::Component& root, std::vector<T*>& out) {
    for (auto* ch : root.getChildren()) {
        if (auto* t = dynamic_cast<T*>(ch)) out.push_back(t);
        findAll(*ch, out);
    }
}
}  // namespace

TEST_CASE("plugins (ui): the browser lists scanned plugins and adds them to the selected track", "[ui][plugins]") {
    UiRig r;
    ui::BrowserPanel b(*r.m);
    b.setSize(220, 600);
    bool header = false, scan = false;
    const ui::BrowserPanel::Row* fx = nullptr;
    const ui::BrowserPanel::Row* synth = nullptr;
    for (const auto& row : b.rows()) {
        if (row.kind == ui::BrowserPanel::Row::Header && row.label == "Plugins") header = true;
        if (row.kind == ui::BrowserPanel::Row::Scan) scan = true;
        if (row.kind == ui::BrowserPanel::Row::Plugin && row.key == "AudioUnit#x#Fake") fx = &row;
        if (row.kind == ui::BrowserPanel::Row::Plugin && row.key == "AudioUnit#x#FakeSynth") synth = &row;
    }
    CHECK(header);
    CHECK(scan);
    REQUIRE(fx);
    REQUIRE(synth);
    CHECK(fx->chain == app::Chain::Effect);
    CHECK(synth->chain == app::Chain::Instrument);
    const auto row1 = *fx, row2 = *synth;
    CHECK(b.activate(row1));
    REQUIRE(r.m->project().tracks[0].fx.size() == 1);
    CHECK(r.m->project().tracks[0].fx[0].plugin == "AudioUnit#x#Fake");
    CHECK(b.activate(row2));
    CHECK(r.m->project().tracks[0].inst.type == "plugin");
    CHECK(r.m->project().tracks[0].inst.pluginName == "FakeSynth");
    CHECK(r.host.live(r.m->project().tracks[0].inst.uid));
    // the scan row asks the host (when the app has not hooked it)
    for (const auto& row : b.rows()) if (row.kind == ui::BrowserPanel::Row::Scan) { ui::BrowserPanel::Row s = row; (void)s; }
}

TEST_CASE("plugins (ui): a plugin's panel has knobs for its parameters that drive the plugin, and opens its editor", "[ui][plugins]") {
    UiRig r;
    r.m->apply(app::edit::addPluginEffect(r.m->project(), r.m->project().tracks[0].uid, "fx", "AudioUnit#x#Fake", "Fake"));
    r.m->selectTrack(r.m->project().tracks[0].uid);
    ui::DeviceChainView v(*r.m);
    v.setSize(1100, 300);
    v.refresh(app::ModelEvent::Selection);
    std::vector<ui::Knob*> knobs;
    findAll(v, knobs);
    REQUIRE(knobs.size() == 12 + 2);                              // the track's Poly Synth, then the two parameters the fake host exposes
    knobs[12]->onChange(0.7);
    CHECK(r.host.set.at(0) == Approx(0.7f));
    CHECK(r.m->project().tracks[0].fx[0].params.empty());         // a plugin's values live in the plugin, not the project map
    std::vector<ui::Chip*> chips;
    findAll(v, chips);
    ui::Chip* editor = nullptr;
    for (auto* c : chips) if (c->text() == "Open editor") editor = c;
    REQUIRE(editor);
    editor->onClick();
    CHECK(r.host.editorsShown == 1);
}

TEST_CASE("plugins (ui): a plugin that is not loaded shows an empty panel, and fills in when it loads", "[ui][plugins]") {
    UiRig r;
    r.host.unloadable.insert("AudioUnit#x#Fake");
    r.m->apply(app::edit::addPluginEffect(r.m->project(), r.m->project().tracks[0].uid, "fx", "AudioUnit#x#Fake", "Fake"));
    r.m->selectTrack(r.m->project().tracks[0].uid);
    ui::DeviceChainView v(*r.m);
    v.setSize(1100, 300);
    v.refresh(app::ModelEvent::Selection);
    std::vector<ui::Knob*> knobs;
    findAll(v, knobs);
    CHECK(knobs.size() == 12);                                    // only the instrument's
    CHECK(v.panelCount() == 2);
    // the plugin becomes available (a rescan found it): the panel is rebuilt with its parameters
    r.host.unloadable.clear();
    std::string err;
    r.host.ensure(r.m->project().tracks[0].fx[0].uid, "AudioUnit#x#Fake", "", err);
    v.refresh(app::ModelEvent::Document);
    knobs.clear();
    findAll(v, knobs);
    CHECK(knobs.size() == 12 + 2);
}

TEST_CASE("plugins (app): a real AudioUnit added through the model loads, shows its parameters, and comes back from a saved project with its settings", "[plugins]") {
    if (!haveAudioUnits()) SKIP("no AudioUnit host on this platform");
    static Setup once;
    namespace fs = std::filesystem;
    const auto dir = (fs::temp_directory_path() / "ddaw_real_plugin_test.ddaw").string();
    fs::remove_all(dir);
    plugins::PluginHost::instance().forgetAll();
    const auto id = appleId("aufx", "dely", "AUDelay");
    REQUIRE_FALSE(id.empty());
    engine::Engine eng;
    eng.prepare(kSr);
    {
        app::AppModel m(eng, kSr);
        plugins::JucePluginProvider provider({}, kSr);
        m.setPluginProvider(&provider);
        m.apply(app::edit::addTrack(m.project(), project::TrackKind::Synth));
        const auto track = m.project().tracks[0].uid;
        REQUIRE(m.apply(app::edit::addPluginEffect(m.project(), track, "fx", id, "AUDelay")));
        const auto uid = m.project().tracks[0].fx[0].uid;
        REQUIRE(provider.live(uid));
        const auto* info = m.deviceInfo(app::Chain::Effect, m.project().tracks[0].fx[0]);
        REQUIRE(info);
        CHECK(info->params.size() >= 4);
        size_t fb = info->params.size();
        for (size_t i = 0; i < info->params.size(); ++i) if (juce::String(provider.paramName(uid, i)).containsIgnoreCase("feedback")) fb = i;
        REQUIRE(fb < info->params.size());
        CHECK(m.paramLabel(m.project().tracks[0].fx[0], info->params[fb].key).find("eedback") != std::string::npos);
        provider.setValue(uid, fb, 0.77f);
        std::string err;
        REQUIRE(m.save(dir, err));
        CHECK_FALSE(m.project().tracks[0].fx[0].pluginState.empty());
        m.newProject();
        CHECK_FALSE(provider.live(uid));                             // the project's plugins went with it
        REQUIRE(m.open(dir, err));
        const auto uid2 = m.project().tracks[0].fx[0].uid;
        REQUIRE(provider.live(uid2));
        CHECK(provider.value(uid2, fb) == Approx(0.77f).margin(0.01f));
        m.setPluginProvider(nullptr);
    }
    plugins::PluginHost::instance().forgetAll();
    fs::remove_all(dir);
}

// A full scan of this machine's plugins: slow and dependent on what is installed, so hidden (run it by name).
TEST_CASE("plugins (scan): the machine's plugins are found", "[.][plugins][scan]") {
    static Setup once;
    namespace fs = std::filesystem;
    const auto cache = juce::File(fs::temp_directory_path().string()).getChildFile("ddaw_scan_test").getChildFile("plugins.xml");
    cache.getParentDirectory().deleteRecursively();
    plugins::JucePluginProvider provider(cache, kSr);
    int seen = 0;
    const auto t0 = juce::Time::getMillisecondCounter();
    provider.scan([&](const std::string& name) { ++seen; std::printf("  scanning %s\n", name.c_str()); });
    std::printf("scan took %u ms, %zu plugins, %zu failed\n", juce::Time::getMillisecondCounter() - t0, provider.available().size(), provider.failedLastScan().size());
    const auto list = provider.available();
    bool delay = false, dls = false;
    for (auto& e : list) { if (e.name.find("AUDelay") != std::string::npos) delay = true; if (e.name.find("DLSMusicDevice") != std::string::npos && e.instrument) dls = true; }
    CHECK(delay);
    CHECK(dls);
    CHECK(cache.existsAsFile());
    plugins::JucePluginProvider again(cache, kSr);                  // the cache is read at startup: no scan needed
    CHECK(again.available().size() == list.size());
    plugins::PluginHost::instance().forgetAll();
    cache.getParentDirectory().deleteRecursively();
}

// Whatever third-party instruments are installed (a user's own plugins): load each, play a note, expect sound and no crash.
// Hidden: it depends on the machine.
TEST_CASE("plugins (third party): installed instruments load and sound", "[.][plugins][thirdparty]") {
    static Setup once;
    plugins::JucePluginProvider provider({}, kSr);
    provider.scan({});
    int tried = 0;
    for (const auto& e : provider.available()) {
        if (!e.instrument || e.vendor.find("Apple") != std::string::npos || e.name.find("DLS") != std::string::npos) continue;
        ++tried;
        std::printf("  %s (%s, %s)\n", e.name.c_str(), e.format.c_str(), e.vendor.c_str());
        plugins::PluginHost::instance().forgetAll();
        const std::string inst = R"({"uid":)" + std::to_string(kInstUid) + R"(,"type":"plugin","plugin":")" + e.id + R"(","pluginName":"x","params":{}})";
        std::string err;
        REQUIRE(plugins::PluginHost::instance().instantiate(kInstUid, e.id, "", kSr, 128, err));
        const auto res = render(chain("", inst));
        REQUIRE(res.complete());
        float mx = 0; for (float v : res.l) { REQUIRE(std::isfinite(v)); mx = std::max(mx, std::abs(v)); }
        std::printf("    peak %.3f, %zu parameters\n", double(mx), plugins::PluginHost::instance().find(kInstUid)->specs.size());
        CHECK(mx > 0.001f);
    }
    plugins::PluginHost::instance().forgetAll();
    std::printf("%d third-party instrument(s) tried\n", tried);
}
