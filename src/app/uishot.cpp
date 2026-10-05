// Headless UI snapshots: builds the model on a standalone engine (no audio device), loads a project,
// and renders the whole window or one view to a PNG. Usage:
//   ddaw_uishot <out.png> [--project file.json|dir.ddaw] [--demo] [--size WxH] [--main session|arrangement|mixer]
//               [--detail clip|devices] [--select-track N] [--select-clip track:scene] [--play]
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

#include "app/model/AppModel.h"
#include "app/model/Controllers.h"
#include "app/model/Demo.h"
#include "app/ui/ExportDialog.h"
#include "project/ProjectJson.h"
#include "app/ui/MainComponent.h"
#include "app/ui/RecordOptions.h"

using namespace ddaw;

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 2) { std::cerr << "usage: ddaw_uishot out.png [options]\n"; return 2; }
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);
    std::string project;
    bool demo = false, play = false, arm = false;
    std::string popup;   // "export" or "record": render that dialog alone
    int w = 1440, h = 900;
    std::string mainTab = "session", detail = "devices", selClip, instrument, lane;
    int selTrack = -1;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--project") project = next();
        else if (a == "--demo") demo = true;
        else if (a == "--play") play = true;
        else if (a == "--arm") arm = true;
        else if (a == "--popup") popup = next();
        else if (a == "--size") { const auto s = next(); const auto x = s.find('x'); w = std::stoi(s.substr(0, x)); h = std::stoi(s.substr(x + 1)); }
        else if (a == "--main") mainTab = next();
        else if (a == "--detail") detail = next();
        else if (a == "--select-track") selTrack = std::stoi(next());
        else if (a == "--select-clip") selClip = next();
        else if (a == "--lane") lane = next();   // piano roll lane: slide | pressure | bend (with some curves drawn on the first notes)
        else if (a == "--instrument") instrument = next();   // replace the selected track's instrument first
    }

    engine::Engine engine;
    engine.prepare(48000.0);
    app::AppModel model(engine, 48000.0);
    std::string err;
    if (demo) app::buildDemo(model);
    else if (!project.empty() && !model.open(project, err)) { std::cerr << "cannot open project: " << err << "\n"; return 1; }

    if (arm) {   // an audio track to record on, armed, with a take in progress look
        model.apply(app::edit::addTrack(model.project(), project::TrackKind::Audio, "Vocal"));
        model.recording().arm(model.project().tracks.back().uid, true);
    }
    if (!popup.empty()) {
        std::unique_ptr<juce::Component> c;
        if (popup == "export") c = std::make_unique<ui::ExportDialog>(model, [](const juce::String&) {});
        else c = std::make_unique<ui::RecordOptions>(model);
        const auto img = c->createComponentSnapshot(c->getLocalBounds(), true, 1.0f);
        out.deleteFile();
        juce::FileOutputStream fo(out);
        juce::PNGImageFormat().writeImageToStream(img, fo);
        std::cout << "wrote " << out.getFullPathName() << "\n";
        return 0;
    }
    ui::MainComponent main(model);
    main.setSize(w, h);
    main.showMain(mainTab == "arrangement" ? ui::MainComponent::MainTab::Arrangement : mainTab == "mixer" ? ui::MainComponent::MainTab::Mixer : ui::MainComponent::MainTab::Session);
    model.setArrangementMode(mainTab == "arrangement");
    if (selTrack >= 0 && size_t(selTrack) < model.project().tracks.size()) model.selectTrack(model.project().tracks[size_t(selTrack)].uid);
    if (!selClip.empty()) {
        const auto c = selClip.find(':');
        const size_t t = size_t(std::stoi(selClip.substr(0, c)));
        const std::string sc = selClip.substr(c + 1);
        if (t < model.project().tracks.size()) model.selectClip(sc.rfind("arr", 0) == 0 ? app::edit::ClipRef{0, "", sc.substr(3)} : app::edit::ClipRef{model.project().tracks[t].uid, sc, ""});
    }
    if (detail == "input") {   // give the panel something to show: two routes on the selected track
        const auto uid = model.selection().track;
        project::PerfSpec a; a.source = "f0"; a.targets.push_back({"inst", "inst", "cutoff"});
        project::PerfSpec b; b.source = "loudness"; b.record = true; b.targets.push_back({"fx", model.project().tracks[2].fx[0].id, "mix"});
        model.apply({"perf.insert", {{"track", uid}, {"index", 0}, {"perf", project::perfToJson(a)}}});
        model.apply({"perf.insert", {{"track", uid}, {"index", 1}, {"perf", project::perfToJson(b)}}});
    }
    if (detail == "mod") {   // something to look at: an LFO and a macro on the selected track, and an automation lane
        const auto uid = model.selection().track;
        const auto& tr = *app::edit::findTrack(model.project(), uid);
        project::LfoSpec l; l.id = "lfo1"; l.shape = 1; l.hz = 0.8; l.depth = 0.4; l.targets.push_back({"inst", "inst", "cutoff"});
        project::LfoSpec l2; l2.id = "lfo2"; l2.sync = true; l2.rate = 3; l2.shape = 4; l2.depth = 0.7; l2.targets.push_back({"fx", tr.fx[0].id, "mix"}); l2.targets.push_back({"mix", "", "pan"});
        model.apply({"lfo.insert", {{"track", uid}, {"index", 0}, {"lfo", project::lfoToJson(l)}}});
        model.apply({"lfo.insert", {{"track", uid}, {"index", 1}, {"lfo", project::lfoToJson(l2)}}});
        project::MacroSpec mc; mc.name = "Brightness"; mc.value = 0.6; mc.targets.push_back({"inst", "inst", "cutoff"}); mc.targets.push_back({"fx", tr.fx[0].id, "mix"});
        model.apply({"macro.insert", {{"track", uid}, {"index", 0}, {"macro", project::macroToJson(mc)}}});
        model.apply({"env.set", {{"scope", {{"track", uid}}}, {"key", "inst|inst|cutoff"}, {"points", project::pointsToJson({{0, 0.2}, {384, 0.8}, {768, 0.35}, {1152, 0.9}, {1536, 0.5}})}}});
    }
    if (!instrument.empty() && model.selection().track) {
        project::DeviceSpec d; d.type = instrument;
        model.apply({"inst.set", {{"track", model.selection().track}, {"device", project::deviceToJson(d, true)}}});
    }
    if (detail == "arate") {   // an FM Operators instrument with two routes: an oscillator on amp, another track's envelope on index
        const auto uid = model.selection().track;
        project::DeviceSpec fm; fm.type = "fmop";
        model.apply({"inst.set", {{"track", uid}, {"device", project::deviceToJson(fm, true)}}});
        project::ARateSpec a; a.id = "ar1"; a.source = "osc"; a.shape = 1; a.hz = 5.5; a.depth = 0.35; a.target = {"inst", "inst", "amp"};
        project::ARateSpec b; b.id = "ar2"; b.source = "track"; b.srcTrack = model.project().tracks[0].id; b.follow = true; b.depth = -0.6; b.target = {"inst", "inst", "index"};
        model.apply({"arate.insert", {{"track", uid}, {"index", 0}, {"arate", project::arateToJson(a)}}});
        model.apply({"arate.insert", {{"track", uid}, {"index", 1}, {"arate", project::arateToJson(b)}}});
    }
    if (detail == "morph") {   // an FM Operators track with a four-anchor map moving ratio, level and index
        const auto uid = model.selection().track;
        project::DeviceSpec fm; fm.type = "fmop";
        model.apply({"inst.set", {{"track", uid}, {"device", project::deviceToJson(fm, true)}}});
        project::MorphSpec m; m.name = "Timbre"; m.x = 0.62; m.y = 0.38;
        m.targets = {{"inst", "inst", "index"}, {"inst", "inst", "r2"}, {"inst", "inst", "l2"}};
        m.curves = {1.0, 1.0, 0.6};
        m.anchors = {{"soft", 0.12, 0.15, {0.05, 0.3, 0.1}}, {"bell", 0.85, 0.2, {0.4, 0.8, 0.5}}, {"brass", 0.2, 0.85, {0.8, 0.4, 0.9}}, {"glass", 0.82, 0.88, {0.6, 0.95, 0.3}}};
        model.apply({"morph.insert", {{"track", uid}, {"index", 0}, {"morph", project::morphToJson(m)}}});
        const auto id = app::edit::findTrack(model.project(), uid)->id;
        project::ControlBinding bx; bx.source = "pad:lx"; bx.target = app::morphTarget(id, 0, 'x');
        project::ControlBinding by; by.source = "pad:ly"; by.target = app::morphTarget(id, 0, 'y');
        model.apply({"binding.insert", {{"index", 0}, {"binding", project::bindingToJson(bx)}}});
        model.apply({"binding.insert", {{"index", 1}, {"binding", project::bindingToJson(by)}}});
    }
    if (!lane.empty() && model.selection().clip.valid()) {   // curves on the first two notes, then show that lane
        const auto ref = model.selection().clip;
        const auto* c = app::edit::findClip(model.project(), ref);
        for (size_t i = 0; c && i < std::min<size_t>(2, c->notes.size()); ++i) {
            auto n = c->notes[i];
            const double d = n.durTicks;
            n.bend = {{0, 0}, {d * 0.3, 2.0}, {d * 0.7, -1.5}, {d, 0}};
            n.slide = {{0, 0.1}, {d * 0.5, 0.8}, {d, 0.3}};
            n.pressure = {{0, 0.2}, {d * 0.4, 0.9}, {d, 0.1}};
            model.apply(app::edit::editNote(ref, n));
            c = app::edit::findClip(model.project(), ref);
        }
    }
    main.showDetail(detail == "morph" ? ui::MainComponent::DetailTab::Morph : detail == "arate" ? ui::MainComponent::DetailTab::AudioRate : detail == "mod" ? ui::MainComponent::DetailTab::Modulation : detail == "clip" ? ui::MainComponent::DetailTab::Clip : detail == "input" ? ui::MainComponent::DetailTab::Input : ui::MainComponent::DetailTab::Devices);
    main.resized();
    if (!lane.empty()) {
        main.clipEditor().setLane(lane == "bend" ? ui::ClipEditor::Lane::Bend : lane == "slide" ? ui::ClipEditor::Lane::Slide : ui::ClipEditor::Lane::Pressure);
        main.clipEditor().selectAll();
    }
    main.tick();
    (void)play;
    const auto img = main.createComponentSnapshot(main.getLocalBounds(), true, 1.0f);
    out.deleteFile();
    juce::FileOutputStream fo(out);
    if (!fo.openedOk()) { std::cerr << "cannot write " << out.getFullPathName() << "\n"; return 1; }
    juce::PNGImageFormat().writeImageToStream(img, fo);
    std::cout << "wrote " << out.getFullPathName() << " (" << img.getWidth() << "x" << img.getHeight() << ")\n";
    return 0;
}
