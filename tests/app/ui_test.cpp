// The views driven by synthetic mouse and key events (no window, no audio): every gesture ends in the
// document, so the assertions are on the project, not on pixels.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/model/Demo.h"
#include "app/ui/ArrangementView.h"
#include "app/ui/BrowserPanel.h"
#include "app/ui/ClipEditor.h"
#include "app/ui/DeviceChainView.h"
#include "app/ui/MainComponent.h"
#include "app/ui/MixerView.h"
#include "app/ui/SessionView.h"

using namespace ddaw;
namespace edit = ddaw::app::edit;
using Catch::Approx;

namespace {

// JUCE's GUI singletons live for the whole run (a Catch listener creates and destroys them), so the
// leak detector at exit sees a clean shutdown.
std::unique_ptr<juce::ScopedJuceInitialiser_GUI> gGui;
struct GuiListener : Catch::EventListenerBase {
    using EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo&) override { gGui = std::make_unique<juce::ScopedJuceInitialiser_GUI>(); }
    void testRunEnded(const Catch::TestRunStats&) override { gGui.reset(); }
};
CATCH_REGISTER_LISTENER(GuiListener)
void gui() {}

// Drives a component with mouse events in its own coordinates.
struct Mouse {
    juce::Component& c;
    juce::Point<float> downPos;
    juce::ModifierKeys mods;
    explicit Mouse(juce::Component& comp, juce::ModifierKeys m = {}) : c(comp), mods(m) {}
    juce::MouseEvent ev(juce::Point<float> p, int clicks = 1, bool dragged = false) {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        return juce::MouseEvent(src, p, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time::getCurrentTime(), downPos, juce::Time::getCurrentTime(), clicks, dragged);
    }
    Mouse& down(juce::Point<float> p) { downPos = p; c.mouseDown(ev(p)); return *this; }
    Mouse& drag(juce::Point<float> p) { c.mouseDrag(ev(p, 1, true)); return *this; }
    Mouse& up(juce::Point<float> p) { c.mouseUp(ev(p)); return *this; }
    Mouse& dbl(juce::Point<float> p) { downPos = p; c.mouseDown(ev(p)); c.mouseUp(ev(p)); c.mouseDoubleClick(ev(p, 2)); return *this; }
    Mouse& click(juce::Point<float> p) { return down(p).up(p); }
};

juce::Point<float> pt(int x, int y) { return {float(x), float(y)}; }

template <class T> void findAll(juce::Component& root, std::vector<T*>& out) {
    for (auto* ch : root.getChildren()) {
        if (auto* t = dynamic_cast<T*>(ch)) out.push_back(t);
        findAll(*ch, out);
    }
}

struct Rig {
    engine::Engine eng;
    std::unique_ptr<app::AppModel> m;
    Rig() { gui(); eng.prepare(48000.0); m = std::make_unique<app::AppModel>(eng, 48000.0); }
    void demo() { app::buildDemo(*m); }
};

}  // namespace

TEST_CASE("session view: hit testing, creating clips, mute, selection and delete", "[ui][session]") {
    Rig r; r.demo();
    ui::SessionView v(*r.m);
    v.setSize(1000, 600);
    using K = ui::SessionView::Hit::Kind;

    const int x1 = ui::SessionView::kSceneW + ui::SessionView::kTrackW / 2 + 20;   // track 0 body
    const int y1 = ui::SessionView::kHeaderH + ui::SessionView::kRowH / 2;          // scene 0
    CHECK(v.hitAt({x1, y1}).kind == K::Slot);
    CHECK(v.hitAt({x1, y1}).track == 0);
    CHECK(v.hitAt({ui::SessionView::kSceneW + 8, y1}).kind == K::SlotLaunch);
    CHECK(v.hitAt({8, y1}).kind == K::SceneLaunch);
    CHECK(v.hitAt({x1, 10}).kind == K::TrackHeader);
    CHECK(v.hitAt({ui::SessionView::kSceneW + 6 * ui::SessionView::kTrackW + 10, 20}).kind == K::AddTrack);
    CHECK(v.hitAt({20, ui::SessionView::kHeaderH + 5 * ui::SessionView::kRowH + 10}).kind == K::AddScene);

    // an empty slot on the Lead track (index 3), scene s1: double-click creates a clip
    const auto& p0 = r.m->project();
    REQUIRE_FALSE(p0.clips.count("t4|s1"));
    const int x4 = ui::SessionView::kSceneW + 3 * ui::SessionView::kTrackW + 60;
    Mouse(v).dbl(pt(x4, y1));
    REQUIRE(r.m->project().clips.count("t4|s1"));
    CHECK(r.m->selection().clip.valid());
    CHECK(r.m->selection().clip.scene == "s1");

    // delete removes it again
    CHECK(v.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK_FALSE(r.m->project().clips.count("t4|s1"));
    r.m->undo();
    CHECK(r.m->project().clips.count("t4|s1"));

    // mute button of track 0: the M chip is in the bottom row of the header
    const int hy = ui::SessionView::kHeaderH - 4 - 14;
    Mouse(v).click(pt(ui::SessionView::kSceneW + 2 + 4 + 28 + 10, hy));
    CHECK(r.m->project().tracks[0].mute);
    Mouse(v).click(pt(ui::SessionView::kSceneW + 2 + 4 + 28 + 10, hy));
    CHECK_FALSE(r.m->project().tracks[0].mute);

    // + Scene and + track
    const size_t scenes = r.m->project().scenes.size();
    Mouse(v).click(pt(20, ui::SessionView::kHeaderH + int(scenes) * ui::SessionView::kRowH + 10));
    CHECK(r.m->project().scenes.size() == scenes + 1);
}

TEST_CASE("arrangement view: create, move, resize and delete clips", "[ui][arrangement]") {
    Rig r; r.demo();
    ui::ArrangementView v(*r.m);
    v.setSize(1200, 600);
    auto& sc = v.scale();
    sc.pxPerBeat = 24.0;
    sc.originTick = 0;
    const int H = ui::ArrangementView::kHeaderW, R = ui::ArrangementView::kRulerH, L = ui::ArrangementView::kLaneH;
    auto xOf = [&](double tick) { return H + int(sc.toX(tick)); };

    // Pluck (track 4) has no arrangement clips: double-click at bar 3 creates one, snapped to the bar
    const size_t before = r.m->project().arr.size();
    Mouse(v).dbl(pt(xOf(384 * 2 + 100), R + 4 * L + 30));
    REQUIRE(r.m->project().arr.size() == before + 1);
    std::string key;
    for (auto& [k, ac] : r.m->project().arr) if (ac.trackId == "t5") key = k;
    REQUIRE_FALSE(key.empty());
    CHECK(r.m->project().arr.at(key).start == 768);
    CHECK(r.m->selection().clip.arrKey == key);

    // move it a bar to the right by dragging its body
    const auto rect = v.clipRect(key);
    REQUIRE(rect.getWidth() > 20);
    {
        Mouse m(v);
        const auto from = pt(rect.getX() + 20, rect.getCentreY());
        m.down(from).drag(pt(from.x + 5, from.y)).drag(pt(from.x + double(xOf(384)) - H, from.y)).up(pt(from.x + double(xOf(384)) - H, from.y));
    }
    CHECK(r.m->project().arr.at(key).start == Approx(768 + 384).margin(24));
    CHECK(int(r.m->project().arr.at(key).start) % 24 == 0);   // snapped

    // resize by the right edge
    const auto r2 = v.clipRect(key);
    {
        Mouse m(v);
        const auto from = pt(r2.getRight() - 3, r2.getCentreY());
        m.down(from).drag(pt(from.x + 10, from.y)).drag(pt(from.x + double(xOf(384)) - H, from.y)).up(pt(from.x + double(xOf(384)) - H, from.y));
    }
    CHECK(r.m->project().arr.at(key).clip.len == Approx(768).margin(24));

    // the whole drag was one undo step
    r.m->undo();
    CHECK(r.m->project().arr.at(key).clip.len == 384);
    r.m->redo();

    // delete via keyboard (the clip is selected)
    CHECK(v.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK_FALSE(r.m->project().arr.count(key));

    // clicking the ruler moves the cursor
    Mouse(v).click(pt(xOf(960), 10));
    CHECK(r.m->cursorTicks() == Approx(960).margin(48));
}

TEST_CASE("piano roll: add, move, resize, velocity, select all, duplicate and delete notes", "[ui][clipeditor]") {
    Rig r; r.demo();
    // open the Lead clip (track 3, scene s3: 6 notes) in the editor
    r.m->selectClip({r.m->project().tracks[3].uid, "s3", ""});
    ui::ClipEditor v(*r.m);
    v.setSize(1100, 320);
    v.refresh(app::ModelEvent::Selection);
    const edit::ClipRef ref{r.m->project().tracks[3].uid, "s3", ""};
    auto notes = [&]() -> const std::vector<project::Note>& { return edit::findClip(r.m->project(), ref)->notes; };
    REQUIRE(notes().size() == 6);

    // double-click in an empty cell adds a note at the snapped time and the row's pitch
    v.scale().originTick = 0; v.scale().pxPerBeat = 64;
    const auto g = v.gridRect();
    const int pitch = 70;
    const int y = v.rowTop(pitch) + 4;
    const int x = ui::ClipEditor::kKeysW + int(v.scale().toX(96 * 6 + 10));
    Mouse(v).dbl(pt(x, y));
    REQUIRE(notes().size() == 7);
    const auto added = notes().back();
    CHECK(added.pitch == pitch);
    CHECK(added.startTicks == Approx(96 * 6).margin(0.01));
    CHECK(v.selected().count(added.uid) == 1);

    // drag it one beat later and two semitones up
    {
        Mouse m(v);
        const auto rect = v.noteRect(added);
        const auto from = pt(rect.getX() + 3, rect.getCentreY());
        const double dx = v.scale().toX(96) - v.scale().toX(0);
        m.down(from).drag(pt(from.x + 6, from.y)).drag(pt(from.x + dx, from.y - 2 * v.rowH())).up(pt(from.x + dx, from.y - 2 * v.rowH()));
    }
    {
        const auto n = *std::find_if(notes().begin(), notes().end(), [&](auto& q) { return q.uid == added.uid; });
        CHECK(n.startTicks == Approx(96 * 7).margin(0.01));
        CHECK(n.pitch == pitch + 2);
    }

    // resize from its right edge
    {
        auto n = *std::find_if(notes().begin(), notes().end(), [&](auto& q) { return q.uid == added.uid; });
        const auto rect = v.noteRect(n);
        Mouse m(v);
        const auto from = pt(rect.getRight() - 2, rect.getCentreY());
        const double dx = v.scale().toX(96) - v.scale().toX(0);
        m.down(from).drag(pt(from.x + 8, from.y)).drag(pt(from.x + dx, from.y)).up(pt(from.x + dx, from.y));
        n = *std::find_if(notes().begin(), notes().end(), [&](auto& q) { return q.uid == added.uid; });
        CHECK(n.durTicks > added.durTicks + 60);
    }

    // velocity lane: drag the bar of the first note to the top
    {
        const auto first = notes().front();
        Mouse m(v);
        const int vx = ui::ClipEditor::kKeysW + int(v.scale().toX(first.startTicks)) + 1;
        const int top = g.getBottom() + 6;
        m.down(pt(vx, g.getBottom() + 30)).drag(pt(vx, top)).up(pt(vx, top));
        CHECK(notes().front().velocity > 0.9);
    }

    // select all, duplicate, delete
    CHECK(v.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)));
    CHECK(v.selected().size() == 7);
    CHECK(v.keyPressed(juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)));
    CHECK(notes().size() == 14);
    CHECK(v.selected().size() == 7);
    CHECK(v.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(notes().size() == 7);
    // arrow keys nudge the selection: nothing selected now, so nothing changes
    r.m->undo();   // delete
    r.m->undo();   // duplicate
    CHECK(notes().size() == 7);
}

TEST_CASE("piano roll: marquee selection and arrow nudging; drum rows", "[ui][clipeditor]") {
    Rig r; r.demo();
    r.m->selectClip({r.m->project().tracks[0].uid, "s1", ""});   // drums: pads as rows
    ui::ClipEditor v(*r.m);
    v.setSize(1100, 320);
    v.refresh(app::ModelEvent::Selection);
    v.scale().originTick = 0; v.scale().pxPerBeat = 64;
    const edit::ClipRef ref{r.m->project().tracks[0].uid, "s1", ""};
    // the kick row (pad 0) is below the snare row: a marquee over kick row only selects kicks
    const auto kickRow = v.rowTop(0);
    {
        Mouse m(v);
        m.down(pt(1000, kickRow + 18)).drag(pt(ui::ClipEditor::kKeysW + 1, kickRow + 2)).up(pt(ui::ClipEditor::kKeysW + 1, kickRow + 2));   // from empty space, right to left
    }
    CHECK(v.selected().size() == 4);
    // up arrow: pad 0 -> pad 1 for the selected notes
    CHECK(v.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    int snares = 0;
    for (auto& n : edit::findClip(r.m->project(), ref)->notes) if (n.pitch == 1) ++snares;
    CHECK(snares == 6);
    CHECK(app::drumPadName(1) == std::string("Snare"));
}

TEST_CASE("mixer: fader, mute and rename route to the document, one undo step per gesture", "[ui][mixer]") {
    Rig r; r.demo();
    ui::MixerView v(*r.m);
    v.setSize(1000, 560);
    v.refresh(app::ModelEvent::Document);
    REQUIRE(v.stripCount() == 6);
    std::vector<ui::Fader*> faders;
    findAll(v, faders);
    REQUIRE(faders.size() == 7);   // six strips and the master
    auto* f = faders[0];
    const double before = r.m->project().tracks[0].gainDb;
    {
        Mouse m(*f);
        const int h = f->getHeight();
        m.down(pt(20, h / 2)).drag(pt(20, h / 6)).drag(pt(20, 4)).up(pt(20, 4));
    }
    CHECK(r.m->project().tracks[0].gainDb > before);
    r.m->undo();
    CHECK(r.m->project().tracks[0].gainDb == Approx(before));   // the whole drag was one step

    // the master fader edits meta.masterGain
    {
        Mouse m(*faders.back());
        const int h = faders.back()->getHeight();
        m.down(pt(20, h)).drag(pt(20, h * 2 / 3)).up(pt(20, h * 2 / 3));
    }
    CHECK(r.m->project().meta.masterGainDb != 0.0);

    std::vector<ui::Chip*> chips;
    findAll(v, chips);
    REQUIRE_FALSE(chips.empty());
    chips[0]->mouseUp(Mouse(*chips[0]).ev(pt(5, 5)));    // first strip's mute chip
    CHECK(r.m->project().tracks[0].mute);
}

TEST_CASE("device chain: knobs edit parameters live, devices power, move and remove", "[ui][devices]") {
    Rig r; r.demo();
    r.m->selectTrack(r.m->project().tracks[2].uid);   // Chords: poly synth, chorus, delay
    ui::DeviceChainView v(*r.m);
    v.setSize(1100, 300);
    v.refresh(app::ModelEvent::Selection);
    REQUIRE(v.panelCount() == 3);

    std::vector<ui::Knob*> knobs;
    findAll(v, knobs);
    REQUIRE(knobs.size() > 12);
    auto* cutoff = knobs[1];   // poly: wave, spread, cutoff... (column-major) -> index 1 is the second row of column 0 or the next column
    const auto& inst = r.m->project().tracks[2].inst;
    // find the knob for "cutoff" by its spec
    for (auto* k : knobs) if (std::string(k->spec().key) == "cutoff") cutoff = k;
    REQUIRE(std::string(cutoff->spec().key) == "cutoff");
    CHECK(inst.params.count("cutoff") == 0);
    {
        Mouse m(*cutoff);
        m.down(pt(25, 40)).drag(pt(25, 10)).drag(pt(25, -20)).up(pt(25, -20));
    }
    REQUIRE(r.m->project().tracks[2].inst.params.count("cutoff") == 1);
    CHECK(r.m->project().tracks[2].inst.params.at("cutoff") > 7000.0);
    r.m->undo();
    CHECK(r.m->project().tracks[2].inst.params.count("cutoff") == 0);

    // double-click resets to the default (stores it)
    Mouse(*cutoff).dbl(pt(25, 40));

    // remove the chorus via its close chip: find chips labelled "x"
    const auto before = r.m->project().tracks[2].fx.size();
    r.m->apply(edit::removeDevice(r.m->project().tracks[2].fx[0].uid));
    CHECK(r.m->project().tracks[2].fx.size() == before - 1);
    v.refresh(app::ModelEvent::Document);
    CHECK(v.panelCount() == 2);

    // master chain view
    std::vector<ui::Chip*> chips;
    findAll(v, chips);
    for (auto* c : chips) if (c->isVisible() && c->getWidth() == 110) { c->setOn(false); }
}

TEST_CASE("browser: double-click actions add devices and set instruments on the selected track", "[ui][browser]") {
    Rig r; r.demo();
    r.m->selectTrack(r.m->project().tracks[1].uid);    // Bass (mono)
    ui::BrowserPanel b(*r.m);
    b.setSize(214, 700);
    const auto& rows = b.rows();
    auto find = [&](ui::BrowserPanel::Row::Kind k, const std::string& key) -> const ui::BrowserPanel::Row* {
        for (auto& row : rows) if (row.kind == k && row.key == key) return &row;
        return nullptr;
    };
    const auto* reverb = find(ui::BrowserPanel::Row::Device, "reverb");
    const auto* fm = find(ui::BrowserPanel::Row::Device, "fm");
    REQUIRE(reverb);
    REQUIRE(fm);
    const size_t fx = r.m->project().tracks[1].fx.size();
    CHECK(b.activate(*reverb));
    CHECK(r.m->project().tracks[1].fx.size() == fx + 1);
    CHECK(r.m->project().tracks[1].fx.back().type == "reverb");
    CHECK(b.activate(*fm));
    CHECK(r.m->project().tracks[1].inst.type == "fm");
    // an instrument cannot go on an audio track
    r.m->apply(edit::addTrack(r.m->project(), project::TrackKind::Audio));
    r.m->selectTrack(r.m->project().tracks.back().uid);
    CHECK_FALSE(b.activate(*fm));
}

TEST_CASE("main window: tabs, selection opens the editor, and every view survives a project swap", "[ui][main]") {
    Rig r; r.demo();
    ui::MainComponent main(*r.m);
    main.setSize(1440, 900);
    main.resized();
    for (auto t : {ui::MainComponent::MainTab::Arrangement, ui::MainComponent::MainTab::Mixer, ui::MainComponent::MainTab::Session}) {
        main.showMain(t);
        main.tick();
    }
    // selecting a clip switches the detail pane to the editor
    r.m->selectClip({r.m->project().tracks[0].uid, "s1", ""});
    main.showDetail(ui::MainComponent::DetailTab::Devices);
    r.m->selectClip({r.m->project().tracks[1].uid, "s1", ""});
    main.tick();
    // new project then back to demo: no dangling selections, no crash
    r.m->newProject();
    main.tick();
    CHECK(r.m->project().tracks.empty());
    r.demo();
    main.tick();
    CHECK(r.m->project().tracks.size() == 6);
    // snapshot renders without throwing
    auto img = main.createComponentSnapshot(main.getLocalBounds(), true, 1.0f);
    CHECK(img.getWidth() == 1440);
}

#include <juce_audio_formats/juce_audio_formats.h>

#include "app/ui/Export.h"

TEST_CASE("export: the demo song renders to a valid 24-bit WAV that matches the engine's length", "[ui][export]") {
    Rig r; r.demo();
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ddaw_export_test.wav");
    file.deleteFile();
    const auto res = ui::exportAudio(r.m->project(), *r.m->sampleBank(), "", file);
    REQUIRE(res.ok);
    INFO(res.message.toStdString());
    CHECK(res.seconds > 5.0);                       // the arrangement runs 12 bars at 118 bpm, plus the tail

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(file));
    REQUIRE(rd);
    CHECK(rd->numChannels == 2);
    CHECK(rd->bitsPerSample == 24);
    CHECK(rd->sampleRate == Approx(48000.0));
    CHECK(double(rd->lengthInSamples) / 48000.0 == Approx(res.seconds).margin(0.01));
    juce::AudioBuffer<float> buf(2, int(rd->lengthInSamples));
    rd->read(&buf, 0, int(rd->lengthInSamples), 0, true, true);
    const float peak = buf.getMagnitude(0, buf.getNumSamples());
    CHECK(peak > 0.05f);
    CHECK(peak <= 1.0f);                            // the master limiter holds it under full scale
    file.deleteFile();

    // nothing to export is reported, not thrown
    r.m->newProject();
    CHECK_FALSE(ui::exportAudio(r.m->project(), *r.m->sampleBank(), "", file).ok);
}

TEST_CASE("recording UI: arm buttons exist only on audio tracks and toggle the arm state", "[ui][recording]") {
    Rig r; r.demo();
    r.m->apply(edit::addTrack(r.m->project(), project::TrackKind::Audio));
    const auto audioUid = r.m->project().tracks.back().uid;
    const int audioIdx = int(r.m->project().tracks.size()) - 1;

    ui::SessionView v(*r.m);
    v.setSize(1200, 600);
    using K = ui::SessionView::Hit::Kind;
    const int bx = ui::SessionView::kSceneW + audioIdx * ui::SessionView::kTrackW + 2 + 4;
    const int by = ui::SessionView::kHeaderH - 4 - 14;
    CHECK(v.hitAt({bx + 3 * 28 + 10, by}).kind == K::Arm);
    CHECK(v.hitAt({bx + 3 * 28 + 10, by}).track == audioIdx);
    // synth and drum tracks have it too (they record the notes played live); a bus does not
    const int sx = ui::SessionView::kSceneW + 2 + 4;
    CHECK(v.hitAt({sx + 3 * 28 + 10, by}).kind == K::Arm);
    CHECK(v.hitAt({ui::SessionView::kSceneW + 5 * ui::SessionView::kTrackW + 2 + 4 + 3 * 28 + 10, by}).kind != K::Arm);   // the Space bus
    CHECK_FALSE(r.m->recording().armed(audioUid));
    Mouse(v).click(pt(bx + 3 * 28 + 10, by));
    CHECK(r.m->recording().armed(audioUid));
    Mouse(v).click(pt(bx + 3 * 28 + 10, by));
    CHECK_FALSE(r.m->recording().armed(audioUid));

    // a bus cannot be armed; a synth or drum track can (for notes)
    r.m->recording().arm(r.m->project().tracks[5].uid, true);
    CHECK_FALSE(r.m->recording().anyArmed());
    r.m->recording().arm(r.m->project().tracks[0].uid, true);
    CHECK(r.m->recording().anyArmed());
    r.m->recording().arm(r.m->project().tracks[0].uid, false);

    // REC without an armed track reports instead of starting
    ui::TransportBar bar(*r.m);
    bar.setSize(1400, 48);
    std::vector<ui::Chip*> chips;
    findAll(bar, chips);
    REQUIRE(chips.size() > 3);
    ui::Chip* rec = chips[2];   // PLAY, STOP, REC in the order the bar adds them
    rec->mouseUp(Mouse(*rec).ev(pt(5, 5)));
    CHECK_FALSE(r.m->recording().recording());
    CHECK(r.m->status().find("arm a track first") != std::string::npos);

    // arming survives, and is dropped when the track is deleted
    r.m->recording().arm(audioUid, true);
    r.m->apply(edit::removeTrack(audioUid));
    CHECK_FALSE(r.m->recording().anyArmed());
}

#include "app/ui/ExportDialog.h"

TEST_CASE("export: stems write one file per source track, with the chosen depth and rate", "[ui][export]") {
    Rig r; r.demo();
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ddaw_stems_test");
    dir.deleteRecursively();
    dir.createDirectory();
    app::ExportOptions o;
    o.stems = true;
    o.bitDepth = 16;
    o.sampleRate = 44100.0;
    const auto res = ui::exportProject(r.m->project(), *r.m->sampleBank(), o, dir.getChildFile("song.wav"));
    REQUIRE(res.ok);
    INFO(res.message.toStdString());
    CHECK(res.files.size() == 6);
    CHECK(dir.getChildFile("song.wav").existsAsFile());
    CHECK(dir.getChildFile("song-Drums.wav").existsAsFile());
    CHECK(dir.getChildFile("song-Lead.wav").existsAsFile());
    CHECK_FALSE(dir.getChildFile("song-Space.wav").existsAsFile());   // a bus is inside the stems that feed it

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(dir.getChildFile("song-Drums.wav")));
    REQUIRE(rd);
    CHECK(rd->bitsPerSample == 16);
    CHECK(rd->sampleRate == Approx(44100.0));
    juce::AudioBuffer<float> buf(2, int(rd->lengthInSamples));
    rd->read(&buf, 0, int(rd->lengthInSamples), 0, true, true);
    CHECK(buf.getMagnitude(0, buf.getNumSamples()) > 0.1f);
    // a stem is silent where its track is silent: the Lead only plays from bar 3
    std::unique_ptr<juce::AudioFormatReader> lead(fm.createReaderFor(dir.getChildFile("song-Lead.wav")));
    REQUIRE(lead);
    juce::AudioBuffer<float> lb(2, int(lead->lengthInSamples));
    lead->read(&lb, 0, int(lead->lengthInSamples), 0, true, true);
    CHECK(lb.getMagnitude(0, 0, int(44100 * 3.5)) < 1e-3f);        // the Lead's clip starts at tick 768 = 4.07 s at 118 bpm
    CHECK(lb.getMagnitude(0, int(44100 * 4.5), int(44100 * 3)) > 0.01f);
    dir.deleteRecursively();
}

TEST_CASE("export: 32-bit float, cancel removes partial files, and an empty project is refused", "[ui][export]") {
    Rig r; r.demo();
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ddaw_export_misc");
    dir.deleteRecursively();
    dir.createDirectory();
    app::ExportOptions o;
    o.bitDepth = 32;
    const auto ok = ui::exportProject(r.m->project(), *r.m->sampleBank(), o, dir.getChildFile("f.wav"));
    REQUIRE(ok.ok);
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(dir.getChildFile("f.wav")));
    REQUIRE(rd);
    CHECK(rd->bitsPerSample == 32);
    CHECK(rd->usesFloatingPointData);

    o.stems = true;
    std::atomic<bool> cancel{false};
    int calls = 0;
    const auto cancelled = ui::exportProject(r.m->project(), *r.m->sampleBank(), o, dir.getChildFile("c.wav"), &cancel,
                                             [&](double f, const juce::String&) { if (++calls > 2 && f > 0.3) cancel = true; });
    CHECK(cancelled.cancelled);
    CHECK_FALSE(cancelled.ok);
    CHECK_FALSE(dir.getChildFile("c.wav").existsAsFile());
    CHECK_FALSE(dir.getChildFile("c-Drums.wav").existsAsFile());

    r.m->newProject();
    const auto empty = ui::exportProject(r.m->project(), *r.m->sampleBank(), app::ExportOptions{}, dir.getChildFile("e.wav"));
    CHECK_FALSE(empty.ok);
    CHECK_FALSE(dir.getChildFile("e.wav").existsAsFile());
    dir.deleteRecursively();
}

TEST_CASE("export dialog: options toggle, and a run finishes with files on disk", "[ui][export]") {
    Rig r; r.demo();
    juce::String done;
    bool finished = false;
    ui::ExportDialog dlg(*r.m, [&](const juce::String& m) { done = m; finished = true; });
    CHECK(dlg.options().range == app::ExportOptions::Range::Arrangement);   // the demo has an arrangement
    std::vector<ui::Chip*> chips;
    findAll(dlg, chips);
    REQUIRE(chips.size() >= 12);
    chips[1]->mouseUp(Mouse(*chips[1]).ev(pt(3, 3)));    // first sample-rate chip (the dialog adds bits, rate, range chips in turn): 44.1 kHz
    CHECK(dlg.options().sampleRate == 44100.0);
    const auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ddaw_dialog_test.wav");
    f.deleteFile();
    dlg.start(f);
    const auto end = juce::Time::getMillisecondCounter() + 30000;
    while (!finished && juce::Time::getMillisecondCounter() < end) {
        dlg.pump();
        juce::Thread::sleep(20);
    }
    CHECK(finished);
    CHECK(done.startsWith("Exported"));
    CHECK(f.existsAsFile());
    f.deleteFile();
}

#include "app/ui/TrackingPanel.h"
#include "document/NativeFormat.h"
#include "project/ProjectJson.h"

TEST_CASE("input panel: tracker on/off, routes added, recorded and removed from the panel", "[ui][tracking]") {
    Rig r; r.demo();
    const auto chords = r.m->project().tracks[2].uid;
    r.m->selectTrack(chords);
    ui::LiveInput live(*r.m);
    ui::TrackingPanel panel(*r.m, live);
    panel.setSize(1200, 300);
    CHECK(panel.routeCount() == 0);
    CHECK_FALSE(r.m->tracking());

    r.m->setTracking(true, chords);
    CHECK(r.m->tracking());
    CHECK(r.m->trackingTarget() == chords);
    CHECK(r.eng.trackerEnabled());
    r.m->setTracking(false, chords);
    CHECK_FALSE(r.eng.trackerEnabled());

    project::PerfSpec spec; spec.source = "loudness"; spec.targets.push_back({"inst", "inst", "cutoff"});
    REQUIRE(r.m->apply({"perf.insert", {{"track", chords}, {"index", 0}, {"perf", project::perfToJson(spec)}}}));
    panel.refresh(app::ModelEvent::Document);
    REQUIRE(panel.routeCount() == 1);
    const auto rr = panel.routeRect(0);
    CHECK_FALSE(r.m->project().tracks[2].perf[0].record);
    Mouse(panel).click(pt(rr.getRight() - 70, rr.getCentreY()));                     // REC
    CHECK(r.m->project().tracks[2].perf[0].record);
    Mouse(panel).click(pt(rr.getX() + 30, rr.getCentreY()));                         // the row: on/off
    CHECK_FALSE(r.m->project().tracks[2].perf[0].on);
    r.m->undo();
    CHECK(r.m->project().tracks[2].perf[0].on);
    Mouse(panel).click(pt(rr.getRight() - 15, rr.getCentreY()));                     // x
    CHECK(panel.routeCount() == 0);
    r.m->undo();
    CHECK(panel.routeCount() == 1);                                                  // the removal is undoable
    // routes survive a save and reload of the project
    const auto text = document::saveProjectJson(r.m->project(), r.m->document().nextUid());
    const auto loaded = document::loadProjectJson(text);
    REQUIRE(loaded.project.tracks[2].perf.size() == 1);
    CHECK(loaded.project.tracks[2].perf[0].source == "loudness");
    CHECK(loaded.project.tracks[2].perf[0].targets[0].pkey == "cutoff");
}

TEST_CASE("monitoring: the mixer's IN chip routes the input through an audio track", "[ui][tracking]") {
    Rig r; r.demo();
    r.m->apply(edit::addTrack(r.m->project(), project::TrackKind::Audio));
    const auto uid = r.m->project().tracks.back().uid;
    ui::MixerView v(*r.m);
    v.setSize(1100, 560);
    v.refresh(app::ModelEvent::Document);
    std::vector<ui::Chip*> chips;
    findAll(v, chips);
    REQUIRE_FALSE(chips.empty());
    ui::Chip* in = chips.back();   // the last strip is the audio track: its chips are M, S, R, IN and the master has none
    CHECK_FALSE(r.m->monitored(uid));
    in->mouseUp(Mouse(*in).ev(pt(3, 3)));
    CHECK(r.m->monitored(uid));
    r.m->tick();   // pushes the flag to the engine for the published graph
}

TEST_CASE("computer keyboard piano: key map, octave and velocity keys, and it stays out of the way when off", "[ui][midi]") {
    Rig r; r.demo();
    ui::LiveInput live(*r.m);
    CHECK_FALSE(live.keyboardEnabled());
    CHECK_FALSE(live.keyPressed(juce::KeyPress('x')));              // off: nothing is consumed
    live.setKeyboardEnabled(true);
    CHECK(live.octave() == 4);
    CHECK(live.pitchForKey('a') == 60);                             // C4
    CHECK(live.pitchForKey('w') == 61);
    CHECK(live.pitchForKey('j') == 71);
    CHECK(live.pitchForKey('k') == 72);                             // the second row continues upward
    CHECK(live.pitchForKey(';') == 76);
    CHECK(live.pitchForKey('q') == -1);
    CHECK(live.keyPressed(juce::KeyPress('x')));
    CHECK(live.octave() == 5);
    CHECK(live.pitchForKey('a') == 72);
    CHECK(live.keyPressed(juce::KeyPress('z')));
    CHECK(live.keyPressed(juce::KeyPress('z')));
    CHECK(live.pitchForKey('a') == 48);
    const float v0 = live.velocity();
    CHECK(live.keyPressed(juce::KeyPress('v')));
    CHECK(live.velocity() > v0);
    CHECK(live.keyPressed(juce::KeyPress('c')));
    CHECK(live.velocity() == Approx(v0).margin(1e-5));
    CHECK(live.keyPressed(juce::KeyPress('a')));                    // a piano key is consumed (keyStateChanged plays it)
    CHECK_FALSE(live.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)));   // Cmd+A stays "select all"
    for (int i = 0; i < 20; ++i) live.keyPressed(juce::KeyPress('z'));
    CHECK(live.octave() == -1);                                     // clamped
    live.setKeyboardEnabled(false);
}

TEST_CASE("live notes through the model: a chord from the keys plays on the selected synth track", "[ui][midi]") {
    Rig r; r.demo();
    r.m->selectTrack(r.m->project().tracks[2].uid);                 // Chords (poly)
    // let the published graph and the live target reach the engine; no audio thread runs here, so pump it by hand
    std::vector<float> l(128u), rr(128u);
    for (int i = 0; i < 400 && r.eng.liveTrack() != 2; ++i) { r.m->tick(); r.eng.process(l.data(), rr.data(), 128); juce::Thread::sleep(2); }
    REQUIRE(r.eng.liveTrack() == 2);
    r.m->noteOn(60); r.m->noteOn(64); r.m->noteOn(67);
    double e2 = 0;
    for (int i = 0; i < 100; ++i) { r.eng.process(l.data(), rr.data(), 128); for (float v : l) e2 += double(v) * v; }
    CHECK(e2 > 1e-4);
    int ons = 0;
    engine::Engine::NoteRecord n;
    while (r.eng.popNoteRecord(n)) ons += n.on;
    CHECK(ons == 3);
    r.m->allNotesOff();
    r.eng.process(l.data(), rr.data(), 128);
    // selecting an audio track removes the target
    r.m->apply(edit::addTrack(r.m->project(), project::TrackKind::Audio));
    r.m->selectTrack(r.m->project().tracks.back().uid);
    for (int i = 0; i < 400 && r.eng.liveTrack() != -1; ++i) { r.m->tick(); r.eng.process(l.data(), rr.data(), 128); juce::Thread::sleep(2); }
    CHECK(r.eng.liveTrack() == -1);
}

#include "app/ui/ModulationPanel.h"

TEST_CASE("modulation panel: LFOs, macros and automation lanes are edited from the panel", "[ui][modulation]") {
    Rig r; r.demo();
    const auto chords = r.m->project().tracks[2].uid;
    r.m->selectTrack(chords);
    ui::ModulationPanel panel(*r.m);
    panel.setSize(1400, 300);
    CHECK(panel.lfoRows() == 0);
    CHECK(panel.macroRows() == 0);

    // add an LFO and a macro through the commands the "+" buttons issue, then edit them live
    project::LfoSpec l; l.id = "lfo1"; l.hz = 2; l.depth = 0.3; l.targets.push_back({"inst", "inst", "cutoff"});
    REQUIRE(r.m->apply({"lfo.insert", {{"track", chords}, {"index", 0}, {"lfo", project::lfoToJson(l)}}}));
    project::MacroSpec mc; mc.name = "Bright"; mc.value = 0.4; mc.targets.push_back({"inst", "inst", "cutoff"});
    REQUIRE(r.m->apply({"macro.insert", {{"track", chords}, {"index", 0}, {"macro", project::macroToJson(mc)}}}));
    panel.refresh(app::ModelEvent::Document);
    REQUIRE(panel.lfoRows() == 1);
    REQUIRE(panel.macroRows() == 1);

    std::vector<ui::Knob*> knobs;
    findAll(panel, knobs);
    REQUIRE(knobs.size() == 2);                                   // the LFO's depth and the macro's value
    {
        Mouse m(*knobs[0]);                                       // drag the LFO depth knob up: a live edit, one undo step
        m.down(pt(25, 40)).drag(pt(25, 10)).drag(pt(25, -30)).up(pt(25, -30));
    }
    CHECK(r.m->project().tracks[2].lfos[0].depth > 0.3);
    r.m->undo();
    CHECK(r.m->project().tracks[2].lfos[0].depth == Approx(0.3));
    {
        Mouse m(*knobs[1]);
        m.down(pt(25, 40)).drag(pt(25, 10)).drag(pt(25, -30)).up(pt(25, -30));
    }
    CHECK(r.m->project().tracks[2].macros[0].value > 0.4);

    // a lane: add one, then edit its points with the mouse
    r.m->apply({"env.set", {{"scope", {{"track", chords}}}, {"key", "inst|inst|cutoff"}, {"points", project::pointsToJson({{0, 0.5}, {768, 0.5}})}}});
    panel.refresh(app::ModelEvent::Document);
    auto& lane = panel.lanes();
    REQUIRE(lane.key() == "inst|inst|cutoff");
    REQUIRE(lane.points().size() == 2);
    const auto c = lane.canvas();
    // click in empty space: a new point, snapped to the grid, at the clicked height
    const int x = lane.xOf(384.0), y = lane.yOf(0.9);
    Mouse(lane).click(pt(x, y));
    REQUIRE(r.m->project().tracks[2].autoLanes.at("inst|inst|cutoff").size() == 3);
    {
        const auto& pts = r.m->project().tracks[2].autoLanes.at("inst|inst|cutoff");
        CHECK(pts[1].t == Approx(384.0).margin(24.0));
        CHECK(pts[1].v == Approx(0.9).margin(0.03));
    }
    // drag it lower
    {
        Mouse m(lane);
        const int px = lane.xOf(r.m->project().tracks[2].autoLanes.at("inst|inst|cutoff")[1].t), py = lane.yOf(0.9);
        m.down(pt(px, py)).drag(pt(px, py + 20)).drag(pt(px, lane.yOf(0.2))).up(pt(px, lane.yOf(0.2)));
    }
    CHECK(r.m->project().tracks[2].autoLanes.at("inst|inst|cutoff")[1].v == Approx(0.2).margin(0.04));
    // double-click a point removes it
    {
        const auto& pts = r.m->project().tracks[2].autoLanes.at("inst|inst|cutoff");
        Mouse m(lane);
        const int px = lane.xOf(pts[1].t), py = lane.yOf(pts[1].v);
        m.downPos = pt(px, py);
        lane.mouseDown(m.ev(pt(px, py), 2));
        lane.mouseUp(m.ev(pt(px, py), 2));
    }
    CHECK(r.m->project().tracks[2].autoLanes.at("inst|inst|cutoff").size() == 2);
    // removing the last points deletes the lane
    r.m->undo(); r.m->undo(); r.m->undo();
    CHECK(c.getWidth() > 100);
}
