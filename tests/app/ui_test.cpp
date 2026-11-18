// The views driven by synthetic mouse and key events (no window, no audio): every gesture ends in the
// document, so the assertions are on the project, not on pixels.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EmptyModel.h"
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/model/Demo.h"
#include "app/ui/ArrangementView.h"
#include "app/ui/BrowserPanel.h"
#include "app/ui/ChordResonatorView.h"
#include "app/ui/ClipEditor.h"
#include "app/ui/DeviceChainView.h"
#include "app/ui/DrumKitView.h"
#include "app/ui/FxView.h"
#include "app/ui/MainComponent.h"
#include "app/ui/RoutingView.h"
#include "app/ui/SessionView.h"
#include "app/ui/TransportBar.h"
#include "plugins/PluginHost.h"

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
    void testRunEnded(const Catch::TestRunStats&) override { ddaw::plugins::PluginHost::shutdown(); gGui.reset(); }
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
    Rig() { gui(); eng.prepare(48000.0); m = std::make_unique<app::AppModel>(eng, 48000.0); startEmpty(*m); }
    void demo() { app::buildDemo(*m); }
};

}  // namespace

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

TEST_CASE("piano roll: expression lanes edit per-note bend, slide and pressure curves", "[ui][clipeditor][expression]") {
    Rig r; r.demo();
    r.m->selectClip({r.m->project().tracks[3].uid, "s3", ""});
    ui::ClipEditor v(*r.m);
    v.setSize(1100, 380);
    v.refresh(app::ModelEvent::Selection);
    v.scale().originTick = 0; v.scale().pxPerBeat = 64;
    const edit::ClipRef ref{r.m->project().tracks[3].uid, "s3", ""};
    auto notes = [&]() -> const std::vector<project::Note>& { return edit::findClip(r.m->project(), ref)->notes; };
    const auto uid = notes().front().uid;
    auto note = [&]() { return *std::find_if(notes().begin(), notes().end(), [&](auto& q) { return q.uid == uid; }); };
    const double dur = note().durTicks;
    REQUIRE(dur >= 48);
    CHECK(note().bend.empty());

    // no expression lane is up yet: the lane is velocity, and the expression chips exist
    CHECK(v.lane() == ui::ClipEditor::Lane::Velocity);
    const int velLaneH = v.laneH();
    v.setLane(ui::ClipEditor::Lane::Bend);
    CHECK(v.laneH() > velLaneH);
    CHECK(v.laneRect().getBottom() == v.getHeight());
    auto at = [&](double tRel, double value) { return pt(v.laneX(note(), tRel), v.laneY(value)); };

    // a click inside the note adds a point there (and selects the note); the note itself is not moved
    const auto before = note();
    Mouse(v).click(at(dur / 2, 5.0));
    REQUIRE(note().bend.size() == 1);
    CHECK(note().bend[0].t == Approx(dur / 2).margin(1.5));
    CHECK(note().bend[0].v == Approx(5.0).margin(0.35));
    CHECK(note().startTicks == before.startTicks);
    CHECK(note().pitch == before.pitch);
    CHECK(v.selected().count(uid) == 1);
    CHECK(v.selectedPoint() == 0);
    CHECK(note().slide.empty());                                       // only the lane being edited changes

    // a second point later in the note; the curve stays sorted
    Mouse(v).click(at(dur * 0.8, -3.0));
    Mouse(v).click(at(dur * 0.15, 2.0));
    REQUIRE(note().bend.size() == 3);
    CHECK(note().bend[0].t < note().bend[1].t);
    CHECK(note().bend[1].t < note().bend[2].t);
    CHECK(note().bend[2].v == Approx(-3.0).margin(0.35));

    // dragging a point moves it in time and value, but not past its neighbours
    {
        const auto p1 = note().bend[1];
        Mouse m(v);
        const auto from = at(p1.t, p1.v);
        m.down(from).drag(pt(from.x + 5, from.y)).drag(at(dur * 0.95, 8.0)).up(at(dur * 0.95, 8.0));
        REQUIRE(note().bend.size() == 3);
        CHECK(note().bend[1].v == Approx(8.0).margin(0.35));
        CHECK(note().bend[1].t < note().bend[2].t);                    // stopped one tick short of the next point
        CHECK(note().bend[1].t > p1.t);
    }
    CHECK(v.selectedPoint() == 1);

    // one gesture is one undo step
    r.m->undo();
    CHECK(note().bend[1].v == Approx(5.0).margin(0.35));
    r.m->redo();
    CHECK(note().bend[1].v == Approx(8.0).margin(0.35));

    // Delete removes the picked point, not the note
    CHECK(v.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(note().bend.size() == 2);
    CHECK(notes().size() == 6);

    // Alt-click on a point removes it too
    {
        const auto p = note().bend[0];
        Mouse m(v, juce::ModifierKeys(juce::ModifierKeys::altModifier));
        m.click(at(p.t, p.v));
        CHECK(note().bend.size() == 1);
    }

    // a click in the lane outside every note's span adds nothing
    {
        const size_t had = note().bend.size();
        Mouse(v).click(pt(v.laneX(note(), dur) + 40, v.laneY(1.0)));
        CHECK(note().bend.size() == had);
    }

    // values beyond +-12 semitones widen the lane
    CHECK(v.bendSpan() == 12.0);
    {
        auto n = note();
        n.bend.push_back({dur * 0.9, 20.0});
        r.m->apply(edit::editNote(ref, n));
        CHECK(v.bendSpan() == 24.0);
        r.m->undo();
        CHECK(v.bendSpan() == 12.0);
    }

    // slide and pressure lanes are separate curves, range 0..1
    v.setLane(ui::ClipEditor::Lane::Slide);
    Mouse(v).click(at(dur * 0.5, 0.75));
    REQUIRE(note().slide.size() == 1);
    CHECK(note().slide[0].v == Approx(0.75).margin(0.02));
    v.setLane(ui::ClipEditor::Lane::Pressure);
    Mouse(v).click(at(dur * 0.25, 5.0));                               // above the lane: clamps to 1
    REQUIRE(note().pressure.size() == 1);
    CHECK(note().pressure[0].v == Approx(1.0).margin(0.001));
    CHECK(note().bend.size() == 1);                                    // the bend curve is untouched

    // freehand drawing replaces what is under the stroke with a dense, sorted run of points
    v.setLane(ui::ClipEditor::Lane::Slide);
    v.setDraw(true);
    {
        Mouse m(v);
        m.down(at(0, 0.1));
        for (int i = 1; i <= 20; ++i) m.drag(at(dur * i / 20.0, 0.1 + 0.04 * i));
        m.up(at(dur, 0.9));
    }
    {
        const auto& sl = note().slide;
        REQUIRE(sl.size() >= 8);
        for (size_t i = 1; i < sl.size(); ++i) { CHECK(sl[i].t > sl[i - 1].t); CHECK(sl[i].v >= sl[i - 1].v - 1e-9); }
        CHECK(sl.front().v < 0.2);
        CHECK(sl.back().v > 0.8);
    }
    v.setDraw(false);

    // a duplicate carries the curves; Clear empties the lane for the selected notes
    v.selectAll();
    CHECK(v.keyPressed(juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)));
    REQUIRE(notes().size() == 12);
    {
        size_t withSlide = 0;
        for (auto& n : notes()) if (!n.slide.empty()) ++withSlide;
        CHECK(withSlide == 2);
        CHECK(notes().back().uid != uid);
    }
    CHECK(!notes()[6].slide.empty());                                  // the copy of the first note keeps its curve
    r.m->undo();
    CHECK(notes().size() == 6);
    v.selectAll();
    // Clear empties the lane's curves on the selected notes
    std::vector<ui::Chip*> chips;
    findAll(v, chips);
    ui::Chip* clear = nullptr;
    for (auto* c : chips) if (c->text() == "Clear") clear = c;
    REQUIRE(clear != nullptr);
    CHECK(clear->isVisible());
    clear->onClick();
    CHECK(note().slide.empty());
    CHECK(note().bend.size() == 1);                                    // other lanes stay
    r.m->undo();
    CHECK_FALSE(note().slide.empty());

    // drum clips take no expression: the lane is velocity and the expression chips are hidden
    r.m->selectClip({r.m->project().tracks[0].uid, "s1", ""});
    v.refresh(app::ModelEvent::Selection);
    CHECK(v.lane() == ui::ClipEditor::Lane::Velocity);
    for (auto* c : chips) if (c->text() == "Bend" || c->text() == "Slide" || c->text() == "Press") CHECK_FALSE(c->isVisible());
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
    for (auto t : {ui::MainComponent::MainTab::Arrangement, ui::MainComponent::MainTab::Routing, ui::MainComponent::MainTab::Session}) {
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
    CHECK(r.m->project().tracks.size() == 2);                                      // a new session: Send A and Send B
    r.demo();
    main.tick();
    CHECK(r.m->project().tracks.size() == 8);                                      // the song's six tracks and the two sends
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
    REQUIRE(knobs.size() == 3);                                   // the LFO's depth and phase, and the macro's value
    {
        Mouse m(*knobs[0]);                                       // drag the LFO depth knob up: a live edit, one undo step
        m.down(pt(25, 40)).drag(pt(25, 10)).drag(pt(25, -30)).up(pt(25, -30));
    }
    CHECK(r.m->project().tracks[2].lfos[0].depth > 0.3);
    r.m->undo();
    CHECK(r.m->project().tracks[2].lfos[0].depth == Approx(0.3));
    {
        Mouse m(*knobs[2]);
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

#include "app/ui/AudioRatePanel.h"

TEST_CASE("audio-rate panel: routes are added, retargeted and edited live from the panel", "[ui][modulation][arate]") {
    Rig r; r.demo();
    const auto chords = r.m->project().tracks[2].uid;
    r.m->selectTrack(chords);
    project::DeviceSpec fmop; fmop.type = "fmop";
    REQUIRE(r.m->apply({"inst.set", {{"track", chords}, {"device", project::deviceToJson(fmop, true)}}}));
    ui::AudioRatePanel panel(*r.m);
    panel.setSize(900, 300);
    CHECK(panel.rows() == 0);

    panel.addRoute();                                              // what the "+ Route" chip does
    panel.refresh(app::ModelEvent::Document);
    REQUIRE(panel.rows() == 1);
    CHECK(r.m->project().tracks[2].arate.size() == 1);
    CHECK(r.m->project().tracks[2].arate[0].source == "osc");

    // retarget it to the instrument's amp port (the picker's choice), through the same command
    auto spec = r.m->project().tracks[2].arate[0];
    spec.target = {"inst", "inst", "amp"};
    REQUIRE(r.m->apply({"arate.edit", {{"track", chords}, {"index", 0}, {"arate", project::arateToJson(spec)}}}));
    panel.refresh(app::ModelEvent::Document);
    CHECK(panel.row(0).targetChip().text().containsIgnoreCase("amp"));

    // source and shape chips cycle; each is a structural edit
    Mouse(panel.row(0).shapeChip()).click(pt(10, 10));
    CHECK(r.m->project().tracks[2].arate[0].shape == 1);
    Mouse(panel.row(0).sourceChip()).click(pt(10, 10));
    panel.refresh(app::ModelEvent::Document);
    CHECK(r.m->project().tracks[2].arate[0].source == "track");
    CHECK(panel.row(0).trackChip().isVisible());
    CHECK(! panel.row(0).shapeChip().isVisible());
    Mouse(panel.row(0).sourceChip()).click(pt(10, 10));
    panel.refresh(app::ModelEvent::Document);
    CHECK(r.m->project().tracks[2].arate[0].source == "osc");

    // depth is a live knob: one undo step per drag, and no rebuild needed
    const double before = r.m->project().tracks[2].arate[0].depth;
    {
        Mouse m(panel.row(0).depthKnob());
        m.down(pt(25, 40)).drag(pt(25, 10)).drag(pt(25, -30)).up(pt(25, -30));
    }
    CHECK(r.m->project().tracks[2].arate[0].depth > before);
    r.m->undo();
    CHECK(r.m->project().tracks[2].arate[0].depth == Approx(before));

    // the rate is a live field too, clamped
    REQUIRE(r.m->apply({"arate.field", {{"track", chords}, {"index", 0}, {"field", "hz"}, {"value", 440.0}}}));
    CHECK(r.m->project().tracks[2].arate[0].hz == Approx(440.0));
    REQUIRE(r.m->apply({"arate.field", {{"track", chords}, {"index", 0}, {"field", "hz"}, {"value", 1e9}}}));
    CHECK(r.m->project().tracks[2].arate[0].hz == Approx(12000.0));

    // remove, and undo/redo restore it
    REQUIRE(r.m->apply({"arate.remove", {{"track", chords}, {"index", 0}}}));
    panel.refresh(app::ModelEvent::Document);
    CHECK(panel.rows() == 0);
    r.m->undo();
    panel.refresh(app::ModelEvent::Document);
    CHECK(panel.rows() == 1);
}

TEST_CASE("audio-rate routes survive save and load", "[ui][arate]") {
    Rig r; r.demo();
    const auto chords = r.m->project().tracks[2].uid;
    project::ARateSpec a; a.id = "ar1"; a.source = "track"; a.srcTrack = r.m->project().tracks[0].id; a.follow = true; a.attackMs = 3; a.releaseMs = 120;
    a.depth = -0.4; a.target = {"inst", "inst", "index"};
    REQUIRE(r.m->apply({"arate.insert", {{"track", chords}, {"index", 0}, {"arate", project::arateToJson(a)}}}));
    const auto j = project::trackToJson(r.m->project().tracks[2]);
    REQUIRE(j.contains("arate"));
    const auto back = project::trackFromJson(j);
    REQUIRE(back.arate.size() == 1);
    CHECK(back.arate[0].source == "track");
    CHECK(back.arate[0].srcTrack == a.srcTrack);
    CHECK(back.arate[0].follow);
    CHECK(back.arate[0].depth == Approx(-0.4));
    CHECK(back.arate[0].attackMs == Approx(3.0));
    CHECK(back.arate[0].releaseMs == Approx(120.0));
    CHECK(back.arate[0].target.pkey == "index");
}

#include "app/ui/MorphPanel.h"

TEST_CASE("morph panel: maps, targets and anchors are built from the panel, and the stick is live and undoable", "[ui][morph]") {
    Rig r; r.demo();
    const auto chords = r.m->project().tracks[2].uid;
    r.m->selectTrack(chords);
    project::DeviceSpec fmop; fmop.type = "fmop";
    REQUIRE(r.m->apply({"inst.set", {{"track", chords}, {"device", project::deviceToJson(fmop, true)}}}));
    ui::MorphPanel panel(*r.m);
    panel.setSize(1100, 320);
    CHECK(panel.current() == nullptr);

    panel.addMap();                                                   // "+ Map"
    REQUIRE(panel.current() != nullptr);
    CHECK(r.m->project().tracks[2].morph.size() == 1);

    // targets come from the picker; give it the same data the picker menu produces
    const auto* info = app::findDevice(app::Chain::Instrument, "fmop");
    REQUIRE(info != nullptr);
    const auto specOf = [&](const char* key) { for (const auto& ps : info->params) if (std::string(ps.key) == key) return ps; return ParamSpec{}; };
    panel.addTarget({"inst", "inst", "r1", "R1", specOf("r1"), 1.0});
    panel.addTarget({"inst", "inst", "l1", "L1", specOf("l1"), 1.0});
    panel.addTarget({"inst", "inst", "l1", "L1", specOf("l1"), 1.0});    // a duplicate is ignored
    REQUIRE(r.m->project().tracks[2].morph[0].targets.size() == 2);
    CHECK(r.m->project().tracks[2].morph[0].curves.size() == 2);

    // an anchor captures the targets' current (stored) values: r1 = 1 is 1/3 of its log range, l1 = 1 is the top
    panel.addAnchorHere();
    {
        const auto& a = r.m->project().tracks[2].morph[0].anchors;
        REQUIRE(a.size() == 1);
        CHECK(a[0].x == Approx(0.5));
        REQUIRE(a[0].values.size() == 2);
        CHECK(a[0].values[0] == Approx(std::log(4.0) / std::log(64.0)).margin(1e-3));
        CHECK(a[0].values[1] == Approx(1.0).margin(1e-3));
    }

    // dragging the stick moves it live (no rebuild), as one undo step
    auto& pad = panel.pad();
    {
        const auto from = pad.toPixel(0.5, 0.5), to = pad.toPixel(0.85, 0.2);
        Mouse m(pad);
        m.down(pt(from.x + 40, from.y + 40)).drag(pt(to.x, to.y)).up(pt(to.x, to.y));   // (not on the anchor: the stick follows)
    }
    CHECK(r.m->project().tracks[2].morph[0].x == Approx(0.85).margin(0.01));
    CHECK(r.m->project().tracks[2].morph[0].y == Approx(0.2).margin(0.01));
    r.m->undo();
    CHECK(r.m->project().tracks[2].morph[0].x == Approx(0.5).margin(0.01));

    // dragging an anchor moves it (committed on release); the stick stays
    {
        const auto a = pad.toPixel(0.5, 0.5), to = pad.toPixel(0.25, 0.75);
        Mouse m(pad);
        m.down(pt(a.x, a.y)).drag(pt(to.x, to.y)).up(pt(to.x, to.y));
    }
    CHECK(r.m->project().tracks[2].morph[0].anchors[0].x == Approx(0.25).margin(0.01));
    CHECK(r.m->project().tracks[2].morph[0].anchors[0].y == Approx(0.75).margin(0.01));
    CHECK(r.m->project().tracks[2].morph[0].x == Approx(0.5).margin(0.01));
    CHECK(panel.selectedAnchor() == 0);

    // capture re-reads the parameters into the selected anchor; delete removes it
    REQUIRE(r.m->apply(document::cmd::setParam(r.m->project().tracks[2].inst.uid, "l1", 0.5)));
    panel.captureIntoSelected();
    CHECK(r.m->project().tracks[2].morph[0].anchors[0].values[1] == Approx(0.5).margin(1e-3));
    panel.removeSelectedAnchor();
    CHECK(r.m->project().tracks[2].morph[0].anchors.empty());

    // a target added after anchors exist gives every anchor a value for it
    panel.addAnchorHere();
    panel.addTarget({"inst", "inst", "index", "Index", specOf("index"), 2.0});
    CHECK(r.m->project().tracks[2].morph[0].anchors[0].values.size() == 3);

    // the method chip flips between IDW and RBF, the ON chip switches the map off
    CHECK(r.m->project().tracks[2].morph[0].method == "idw");
    Mouse(panel.methodChip()).click(pt(10, 10));
    CHECK(r.m->project().tracks[2].morph[0].method == "rbf");
    Mouse(panel.methodChip()).click(pt(10, 10));
    CHECK(r.m->project().tracks[2].morph[0].method == "idw");
    Mouse(panel.onChip()).click(pt(10, 10));
    CHECK(! r.m->project().tracks[2].morph[0].on);
}

TEST_CASE("morph maps survive save and load", "[ui][morph]") {
    Rig r; r.demo();
    const auto chords = r.m->project().tracks[2].uid;
    project::MorphSpec m;
    m.name = "Space"; m.x = 0.3; m.y = 0.7; m.method = "rbf"; m.width = 0.25; m.power = 3;
    m.targets = {{"inst", "inst", "cutoff"}, {"fx", r.m->project().tracks[2].fx[0].id, "mix"}};
    m.curves = {0.5, 2.0};
    m.anchors = {{"dark", 0.1, 0.1, {0.1, 0.0}}, {"bright", 0.9, 0.8, {0.9, 0.7}}};
    REQUIRE(r.m->apply({"morph.insert", {{"track", chords}, {"index", 0}, {"morph", project::morphToJson(m)}}}));
    const auto back = project::trackFromJson(project::trackToJson(r.m->project().tracks[2]));
    REQUIRE(back.morph.size() == 1);
    const auto& b = back.morph[0];
    CHECK(b.name == "Space");
    CHECK(b.method == "rbf");
    CHECK(b.width == Approx(0.25));
    CHECK(b.x == Approx(0.3));
    REQUIRE(b.targets.size() == 2);
    CHECK(b.targets[1].fxId == m.targets[1].fxId);
    CHECK(b.curves == std::vector<double>{0.5, 2.0});
    REQUIRE(b.anchors.size() == 2);
    CHECK(b.anchors[1].name == "bright");
    CHECK(b.anchors[1].values == std::vector<double>{0.9, 0.7});
}

#include "app/ui/ControllerInput.h"
#include "app/ui/LiveInput.h"

TEST_CASE("controllers: learn from the panel, gamepad and MIDI values reach the stick", "[ui][morph][controllers]") {
    Rig r; r.demo();
    const auto chords = r.m->project().tracks[2].uid;
    const std::string cid = r.m->project().tracks[2].id;
    r.m->selectTrack(chords);
    project::MorphSpec map; map.name = "m"; map.targets = {{"inst", "inst", "cutoff"}};
    map.anchors = {{"lo", 0.1, 0.5, {0.1}}, {"hi", 0.9, 0.5, {0.9}}};
    REQUIRE(r.m->apply({"morph.insert", {{"track", chords}, {"index", 0}, {"morph", project::morphToJson(map)}}}));
    ui::LiveInput live(*r.m);
    ui::ControllerInput controllers(*r.m, live);
    ui::MorphPanel panel(*r.m, &controllers);
    panel.setSize(1100, 340);

    // "Learn X": the next clear movement of any control becomes the binding
    Mouse(panel.learnXChip()).click(pt(10, 10));
    CHECK(r.m->learnTarget() == app::morphTarget(cid, 0, 'x'));
    controllers.feed("pad:rx", 0.5);
    controllers.feed("pad:rx", 0.95);
    CHECK(r.m->learnTarget().empty());
    panel.refresh(app::ModelEvent::Document);
    REQUIRE(r.m->project().bindings.size() == 1);
    CHECK(r.m->project().bindings[0].source == "pad:rx");
    CHECK(panel.bindingRows() == 1);

    // a stubbed gamepad reading: stick right = 1.0, centred (inside the deadzone) = 0.5
    app::GamepadState s; s.connected = true; s.name = "Test pad"; s.rx = 1.0f;
    controllers.setPadOverride(&s);
    controllers.pollOnce();
    CHECK(r.m->project().tracks[2].morph[0].x == Approx(1.0));
    s.rx = 0.05f;
    controllers.pollOnce();
    CHECK(r.m->project().tracks[2].morph[0].x == Approx(0.5));
    s.rx = -1.0f;
    controllers.pollOnce();
    CHECK(r.m->project().tracks[2].morph[0].x == Approx(0.0));
    CHECK(controllers.padConnected());
    controllers.setPadOverride(nullptr);

    // a MIDI CC reaches a binding through LiveInput's table
    project::ControlBinding b; b.source = "midi:cc74"; b.target = app::morphTarget(cid, 0, 'y');
    REQUIRE(r.m->apply({"binding.insert", {{"index", 1}, {"binding", project::bindingToJson(b)}}}));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::controllerEvent(1, 74, 127));
    controllers.pollOnce();
    CHECK(r.m->project().tracks[2].morph[0].y == Approx(1.0));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::controllerEvent(1, 74, 0));
    controllers.pollOnce();
    CHECK(r.m->project().tracks[2].morph[0].y == Approx(0.0));

    // removing a binding from the panel
    panel.refresh(app::ModelEvent::Document);
    REQUIRE(panel.bindingRows() == 2);
}

TEST_CASE("gamepad: the GameController backend can be polled with or without controllers attached", "[ui][controllers]") {
    app::GamepadInput pad;
    for (int i = 0; i < 5; ++i) {
        const auto pads = pad.poll();                    // no hardware is needed: with none connected this is an empty list
        CHECK(pads.size() <= app::GamepadInput::kMaxPads);
        for (const auto& s : pads) { CHECK(s.connected); CHECK(s.lx >= -1.0f); CHECK(s.lx <= 1.0f); CHECK(s.lt >= 0.0f); CHECK(s.lt <= 1.0f); }
    }
}

TEST_CASE("gamepad: rumble is safe without hardware, and a learned control is acknowledged", "[ui][controllers]") {
    app::GamepadInput pad;
    CHECK_FALSE(pad.rumble(99));                         // there is no pad 99, whatever is plugged in
    Rig r; r.demo();
    const auto chords = r.m->project().tracks[2].uid;
    r.m->selectTrack(chords);
    project::MorphSpec map; map.name = "m"; map.targets = {{"inst", "inst", "cutoff"}};
    map.anchors = {{"lo", 0.1, 0.5, {0.1}}, {"hi", 0.9, 0.5, {0.9}}};
    REQUIRE(r.m->apply({"morph.insert", {{"track", chords}, {"index", 0}, {"morph", project::morphToJson(map)}}}));
    ui::LiveInput live(*r.m);
    ui::ControllerInput controllers(*r.m, live);
    controllers.feed("pad:lx", 0.5);                     // not learning: no pulse
    CHECK(controllers.pulses() == 0);
    r.m->startLearn(app::morphTarget(r.m->project().tracks[2].id, 0, 'x'));
    controllers.feed("pad2:rt", 0.0);
    controllers.feed("pad2:rt", 1.0);                    // the control that just got bound is on pad 2
    CHECK(controllers.pulses() == 1);
    CHECK(r.m->project().bindings.size() == 1);
    controllers.feed("pad2:rt", 0.3);                    // ordinary use afterwards: no more pulses
    CHECK(controllers.pulses() == 1);
}

TEST_CASE("controllers: several gamepads are separate sources, the first keeping the plain name", "[ui][controllers]") {
    Rig r; r.demo();
    const std::string cid = r.m->project().tracks[2].id;
    const auto chords = r.m->project().tracks[2].uid;
    r.m->selectTrack(chords);
    project::MorphSpec map; map.name = "m"; map.targets = {{"inst", "inst", "cutoff"}};
    map.anchors = {{"lo", 0.1, 0.5, {0.1}}, {"hi", 0.9, 0.5, {0.9}}};
    REQUIRE(r.m->apply({"morph.insert", {{"track", chords}, {"index", 0}, {"morph", project::morphToJson(map)}}}));
    for (const auto& [src, axis] : {std::pair{"pad:lx", 'x'}, std::pair{"pad2:lx", 'y'}}) {
        project::ControlBinding b; b.source = src; b.target = app::morphTarget(cid, 0, axis);
        REQUIRE(r.m->apply({"binding.insert", {{"index", r.m->project().bindings.size()}, {"binding", project::bindingToJson(b)}}}));
    }
    ui::LiveInput live(*r.m);
    ui::ControllerInput controllers(*r.m, live);
    std::vector<app::GamepadState> pads(2);
    pads[0].connected = pads[1].connected = true;
    pads[0].name = "First"; pads[1].name = "Second";
    pads[0].lx = 1.0f;       // pad one's stick all the way right: X = 1
    pads[1].lx = -1.0f;      // pad two's all the way left: Y = 0
    controllers.setPadsOverride(&pads);
    controllers.pollOnce();
    CHECK(controllers.padCount() == 2);
    CHECK(controllers.padName(1) == "Second");
    CHECK(r.m->project().tracks[2].morph[0].x == Approx(1.0));
    CHECK(r.m->project().tracks[2].morph[0].y == Approx(0.0));
    pads[1].lx = 1.0f;
    controllers.pollOnce();
    CHECK(r.m->project().tracks[2].morph[0].y == Approx(1.0));
    // unplugging pad two leaves its binding idle, pad one still works
    pads.pop_back();
    pads[0].lx = 0.0f;
    controllers.pollOnce();
    CHECK(controllers.padCount() == 1);
    CHECK(r.m->project().tracks[2].morph[0].x == Approx(0.5));
    CHECK(r.m->project().tracks[2].morph[0].y == Approx(1.0));
    CHECK(app::describeSource("pad:lx") == "Left stick X");
    CHECK(app::describeSource("pad3:rt") == "Pad 3 Right trigger");
}

#include "dsp/Fft.h"

namespace {
// strongest spectral component of the last 16384 samples inside [lo, hi] Hz, parabolic-interpolated
double peakNear(const std::vector<float>& x, double lo, double hi, double sr = 48000.0) {
    constexpr size_t N = 16384;
    dsp::Fft f; f.prepare(int(N));
    std::vector<double> re(N), im(N, 0.0), m(N / 2);
    for (size_t i = 0; i < N; ++i) re[i] = double(x[x.size() - N + i]) * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * double(i) / N));
    f.forward(re.data(), im.data());
    for (size_t k = 0; k < N / 2; ++k) m[k] = std::hypot(re[k], im[k]);
    size_t best = size_t(lo / sr * N);
    for (size_t k = size_t(lo / sr * N); k <= size_t(hi / sr * N); ++k) if (m[k] > m[best]) best = k;
    const double a = std::log(m[best - 1] + 1e-12), b = std::log(m[best] + 1e-12), c = std::log(m[best + 1] + 1e-12);
    return (double(best) + 0.5 * (a - c) / (a - 2.0 * b + c)) * sr / double(N);
}
double energyNear(const std::vector<float>& x, double hz, double sr = 48000.0) {
    constexpr size_t N = 16384;
    dsp::Fft f; f.prepare(int(N));
    std::vector<double> re(N), im(N, 0.0);
    for (size_t i = 0; i < N; ++i) re[i] = double(x[x.size() - N + i]) * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * double(i) / N));
    f.forward(re.data(), im.data());
    const int k0 = int(std::lround(hz / sr * N));
    double best = 0;
    for (int k = k0 - 3; k <= k0 + 3; ++k) best = std::max(best, std::hypot(re[size_t(k)], im[size_t(k)]));
    return best;
}
}  // namespace

TEST_CASE("MPE through the MIDI input: per-channel notes bend alone, the master channel bends all, expression before the note-on counts", "[ui][midi][mpe]") {
    Rig r;   // an empty project with one clean synth track (no effects to colour the measurement)
    REQUIRE(r.m->apply(edit::addTrack(r.m->project(), project::TrackKind::Synth)));
    const auto chords = r.m->project().tracks[0].uid;
    project::DeviceSpec fmop; fmop.type = "fmop";
    fmop.params = {{"algo", 4}, {"l1", 1}, {"l2", 0}, {"l3", 0}, {"l4", 0}, {"attack", 0.001}, {"sustain", 1.0}, {"release", 0.02}};
    REQUIRE(r.m->apply({"inst.set", {{"track", chords}, {"device", project::deviceToJson(fmop, true)}}}));
    r.m->selectTrack(chords);
    std::vector<float> l(128u), rr(128u);
    // the builder is busy until the audio side swaps the new graph in, so the engine must run while we wait
    for (int i = 0; i < 2000 && r.m->service().busy(); ++i) { r.eng.process(l.data(), rr.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    REQUIRE(!r.m->service().busy());
    r.m->tick();
    ui::LiveInput live(*r.m);
    const auto run = [&](int blocks) {
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) { r.eng.process(l.data(), rr.data(), 128); out.insert(out.end(), l.begin(), l.end()); }
        return out;
    };
    run(4);   // the rebuilt graph swaps in

    // ---- MPE off: a bend wheel (any channel) bends every note by the bend range, 2 semitones by default ----
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOn(1, 69, 0.9f));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::pitchWheel(1, 16383));
    CHECK(peakNear(run(300), 400.0, 600.0) == Approx(440.0 * std::pow(2.0, 2.0 / 12.0)).margin(3.0));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::pitchWheel(1, 8192));
    CHECK(peakNear(run(300), 400.0, 600.0) == Approx(440.0).margin(3.0));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOff(1, 69));
    run(50);

    // ---- MPE on: member channels carry their own notes ----
    r.m->setMpe(true);
    r.m->setMpeRange(48.0f);
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::pitchWheel(2, 8192 + 2048));    // channel 2's bend BEFORE its note: +12 semitones
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOn(2, 69, 0.9f));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOn(3, 72, 0.9f));           // channel 3: no bend
    const auto both = run(300);
    CHECK(peakNear(both, 800.0, 960.0) == Approx(880.0).margin(5.0));                           // the A, an octave up
    CHECK(peakNear(both, 500.0, 560.0) == Approx(523.25).margin(4.0));                          // the C, untouched
    // bending channel 3 while it sounds moves the C only
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::pitchWheel(3, 8192 + 2048));
    const auto moved = run(300);
    CHECK(peakNear(moved, 1000.0, 1100.0) == Approx(1046.5).margin(6.0));
    CHECK(peakNear(moved, 800.0, 960.0) == Approx(880.0).margin(5.0));
    // pressure on a channel is that note's pressure: +50% level at full pressure
    const double before = energyNear(moved, 880.0);
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::channelPressureChange(2, 127));
    CHECK(energyNear(run(300), 880.0) / before == Approx(1.5).margin(0.08));
    // the master channel (1) bends every note, by the (default 2 semitone) bend range
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::pitchWheel(1, 16383));
    CHECK(peakNear(run(300), 900.0, 1000.0) == Approx(880.0 * std::pow(2.0, 2.0 / 12.0)).margin(6.0));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::pitchWheel(1, 8192));
    // note-offs end the notes and their channels' note memory: a later bend on channel 2 touches no note
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOff(2, 69));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOff(3, 72));
    run(60);
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::pitchWheel(2, 0));
    CHECK_NOTHROW(run(10));

    // polyphonic key pressure (non-MPE controllers) is one note's pressure
    r.m->setMpe(false);
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::pitchWheel(2, 8192));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOn(1, 69, 0.9f));
    const auto base = run(300);
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::aftertouchChange(1, 69, 127));
    CHECK(energyNear(run(300), 440.0) / energyNear(base, 440.0) == Approx(1.5).margin(0.08));
    live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::allNotesOff(1));
}

TEST_CASE("MPE setup over MIDI: the configuration message sets the zones, the sensitivity RPN sets the ranges", "[ui][midi][mpe]") {
    Rig r;
    ui::LiveInput live(*r.m);
    int refreshes = 0;
    r.m->addListener([&](app::ModelEvent e) { if (e == app::ModelEvent::Recording) ++refreshes; });
    const auto cc = [&](int ch, int n, int v) { live.handleIncomingMidiMessage(nullptr, juce::MidiMessage::controllerEvent(ch, n, v)); };
    const auto rpn = [&](int ch, int msb, int lsb, int data, int dataLsb = -1) {
        cc(ch, 101, msb); cc(ch, 100, lsb); cc(ch, 6, data);
        if (dataLsb >= 0) cc(ch, 38, dataLsb);
        cc(ch, 101, 127); cc(ch, 100, 127);   // RPN null
    };

    // switching MPE on by hand means "all channels but the first are members"
    CHECK_FALSE(r.m->mpeMember(2));
    r.m->setMpe(true);
    CHECK(r.m->mpeMember(2));
    CHECK(r.m->mpeMember(16));
    CHECK_FALSE(r.m->mpeMember(1));

    // MPE configuration message (RPN 6) on the master channel: the lower zone gets 4 member channels
    rpn(1, 0, 6, 4);
    CHECK(r.m->mpeLowerMembers() == 4);
    CHECK(r.m->mpeMember(2));  CHECK(r.m->mpeMember(5));
    CHECK_FALSE(r.m->mpeMember(6));                   // beyond the zone: an ordinary channel
    CHECK_FALSE(r.m->mpeMember(1));
    // and the upper zone (master 16) 3 members: channels 15, 14, 13
    rpn(16, 0, 6, 3);
    CHECK(r.m->mpeUpperMembers() == 3);
    CHECK(r.m->mpeMember(13)); CHECK(r.m->mpeMember(15));
    CHECK_FALSE(r.m->mpeMember(12));
    CHECK_FALSE(r.m->mpeMember(16));
    // the zones cannot overlap: asking for more than the channels left is cut to fit
    rpn(1, 0, 6, 15);
    CHECK(r.m->mpeLowerMembers() + r.m->mpeUpperMembers() <= 15);
    // a configuration message on a channel that is not a master channel does nothing
    const int lower = r.m->mpeLowerMembers();
    rpn(7, 0, 6, 2);
    CHECK(r.m->mpeLowerMembers() == lower);
    // zero members disables a zone; both zones off turns MPE off
    rpn(1, 0, 6, 4);
    rpn(16, 0, 6, 3);
    CHECK(r.m->mpeUpperMembers() == 3);
    rpn(1, 0, 6, 0);
    CHECK(r.m->mpeLowerMembers() == 0);
    CHECK(r.m->mpe());
    rpn(16, 0, 6, 0);
    CHECK_FALSE(r.m->mpe());

    // pitch bend sensitivity (RPN 0): semitones, plus cents from the LSB; a member channel sets the per-note range, otherwise the bend range
    r.m->setMpeZones(15, 0);
    r.m->setMpe(true);
    rpn(2, 0, 0, 12);
    CHECK(r.m->mpeRange() == Approx(12.0f));
    rpn(1, 0, 0, 3, 50);
    CHECK(r.m->bendRange() == Approx(3.5f));
    rpn(1, 0, 0, 0, 40);                              // under a semitone: ignored
    CHECK(r.m->bendRange() == Approx(3.5f));
    rpn(1, 0, 0, 127);                                // the largest data byte: clamped to the limit
    CHECK(r.m->bendRange() == Approx(96.0f));

    // the UI hears about changes made on the MIDI thread at its next tick
    const int before = refreshes;
    r.m->tick();
    CHECK(refreshes == before + 1);
    r.m->tick();
    CHECK(refreshes == before + 1);                   // once
}

TEST_CASE("device chain: a dynamics device shows its sidechain source and follows changes to it", "[ui][sidechain]") {
    Rig r; r.demo();
    auto& m = *r.m;
    // the Chords track gets a compressor
    const auto chords = m.project().tracks[2].uid;
    REQUIRE(m.apply(edit::addDevice(m.project(), chords, "fx", "comp")));
    const auto devUid = m.project().tracks[2].fx.back().uid;
    m.selectTrack(chords);
    ui::DeviceChainView v(m);
    v.setSize(1300, 300);
    v.refresh(app::ModelEvent::Selection);
    auto sidechainChip = [&]() -> ui::Chip* {
        std::vector<ui::Chip*> chips;
        findAll(v, chips);
        for (auto* c : chips) if (c->text().startsWith("Sidechain")) return c;
        return nullptr;
    };
    REQUIRE(sidechainChip());
    CHECK(sidechainChip()->text() == "Sidechain: off");
    CHECK_FALSE(sidechainChip()->isOn());
    // keyed by the drums
    REQUIRE(m.apply({"device.set", {{"uid", devUid}, {"field", "srcTrack"}, {"value", m.project().tracks[0].id}}}));
    v.refresh(app::ModelEvent::Document);
    REQUIRE(sidechainChip());
    CHECK(sidechainChip()->text() == juce::String("Sidechain: ") + juce::String(m.project().tracks[0].name));
    CHECK(sidechainChip()->isOn());
    // the key filter is part of the device and of undo
    REQUIRE(m.apply({"device.set", {{"uid", devUid}, {"field", "keyHpf"}, {"value", 200.0}}}));
    REQUIRE(m.project().tracks[2].fx.back().keyHpf.has_value());
    CHECK(*m.project().tracks[2].fx.back().keyHpf == Approx(200.0));
    m.undo();
    CHECK_FALSE(m.project().tracks[2].fx.back().keyHpf.has_value());
    m.undo();
    v.refresh(app::ModelEvent::Document);
    CHECK(sidechainChip()->text() == "Sidechain: off");
    // other effects have no such chip
    REQUIRE(m.apply(edit::addDevice(m.project(), chords, "fx", "reverb")));
    v.refresh(app::ModelEvent::Selection);
    std::vector<ui::Chip*> chips;
    findAll(v, chips);
    int n = 0;
    for (auto* c : chips) if (c->text().startsWith("Sidechain")) ++n;
    CHECK(n == 1);                                                    // the compressor's only
}

TEST_CASE("transport: the project's key is shown and follows the document", "[ui][transport][key]") {
    Rig r;
    auto& m = *r.m;
    ui::TransportBar v(m);
    v.setSize(1300, 48);
    v.refresh(app::ModelEvent::Document);
    auto keyText = [&]() -> juce::String {
        std::vector<ui::Chip*> chips;
        findAll(v, chips);
        for (auto* c : chips) if (c->text().contains("Minor") || c->text().contains("Major") || c->text().contains("Dorian") || c->text().contains("Blues")) return c->text();
        return {};
    };
    CHECK(keyText() == "A Minor");
    REQUIRE(m.apply(document::cmd::setMeta("root", 2.0)));
    REQUIRE(m.apply(document::cmd::setMeta("scale", std::string("dorian"))));
    v.refresh(app::ModelEvent::Document);
    CHECK(keyText() == "D Dorian");
    m.undo();
    m.undo();
    v.refresh(app::ModelEvent::Document);
    CHECK(keyText() == "A Minor");
}

// ---- the chord resonator's panel ----

namespace {
struct ChordRig {
    Rig r;
    project::Uid uid = 0;
    const app::DeviceInfo* info = nullptr;
    std::unique_ptr<ui::ChordResonatorView> v;
    ChordRig() {
        r.demo();
        const auto track = r.m->project().tracks[1].uid;
        r.m->apply(edit::addDevice(r.m->project(), track, "fx", "chordres"));
        uid = edit::findTrack(r.m->project(), track)->fx.back().uid;
        info = app::findDevice(app::Chain::Effect, "chordres");
        v = std::make_unique<ui::ChordResonatorView>(*r.m, uid, *info);
        v->setSize(ui::ChordResonatorView::kWidth, 195);
        sync();
    }
    void sync() { v->sync(*r.m->document().findDevice(uid)); }
    double param(const char* key) {
        const auto* d = r.m->document().findDevice(uid);
        const auto it = d->params.find(key);
        if (it != d->params.end()) return it->second;
        for (const auto& p : info->params) if (std::string(p.key) == key) return double(p.def);
        return -999;
    }
    juce::Point<float> at(int semis, double db) { return {v->xOfSemitone(semis), v->yOfDb(db)}; }
};
}  // namespace

TEST_CASE("chord resonator panel: dragging a bead changes its interval and level, clamped to the ranges", "[ui][chordres]") {
    ChordRig c;
    CHECK(c.v->activeFilters() == 3);
    CHECK(c.param("i1") == 4.0);
    const double boost = c.param("boost");
    Mouse m(*c.v);
    const auto start = c.v->beadCentre(0);
    m.down(start).drag(c.at(9, boost + 6.0)).up(c.at(9, boost + 6.0));
    CHECK(c.param("i1") == 9.0);
    CHECK(c.param("l1") == Approx(6.0).margin(0.26));
    CHECK(c.v->readout().contains("1:"));
    // far past the ends: the interval stops at -12..+36 and the level at -18..+12
    c.sync();
    const auto b = c.v->beadCentre(0);
    m.down(b).drag({c.v->plotBounds().getRight() + 50.0f, c.v->plotBounds().getY() - 40.0f}).up(b);
    CHECK(c.param("i1") == 36.0);
    CHECK(c.param("l1") == 12.0);
    c.sync();
    const auto b2 = c.v->beadCentre(0);
    m.down(b2).drag({c.v->plotBounds().getX() - 50.0f, c.v->plotBounds().getBottom() + 40.0f}).up(b2);
    CHECK(c.param("i1") == -12.0);
    CHECK(c.param("l1") == -18.0);
    // one drag is one undo step
    const auto before = c.param("i1");
    c.sync();
    m.down(c.v->beadCentre(1)).drag(c.at(2, boost)).drag(c.at(3, boost)).drag(c.at(5, boost)).up(c.at(5, boost));
    CHECK(c.param("i2") == 5.0);
    c.r.m->undo();
    CHECK(c.param("i2") == 7.0);
    CHECK(c.param("i1") == before);
}

TEST_CASE("chord resonator panel: double-click adds and removes filters (as one undo step), up to six and down to one", "[ui][chordres]") {
    ChordRig c;
    Mouse m(*c.v);
    const double boost = c.param("boost");
    // add: on empty space, the interval under the pointer, level 0
    m.dbl(c.at(14, boost - 12.0));
    c.sync();
    CHECK(c.v->activeFilters() == 4);
    CHECK(c.param("filters") == 4.0);
    CHECK(c.param("i4") == 14.0);
    CHECK(c.param("l4") == 0.0);
    c.r.m->undo();
    CHECK(c.param("filters") == 3.0);
    c.sync();
    // the ruler adds too, and stops at six
    for (int semis : {2, 9, 21, 30}) { c.sync(); m.click({c.v->xOfSemitone(semis), c.v->rulerBounds().getCentreY() - 4}); }
    c.sync();
    CHECK(c.v->activeFilters() == 6);
    CHECK(c.param("i4") == 2.0);
    CHECK(c.param("i5") == 9.0);
    CHECK(c.param("i6") == 21.0);                                        // the seventh click (30) found no room
    m.dbl(c.at(33, boost - 12.0));
    c.sync();
    CHECK(c.v->activeFilters() == 6);
    // remove the first: the others move up, in order
    const double i2 = c.param("i2"), i3 = c.param("i3"), l2 = c.param("l2");
    m.dbl(c.v->beadCentre(0));
    c.sync();
    CHECK(c.v->activeFilters() == 5);
    CHECK(c.param("i1") == i2);
    CHECK(c.param("i2") == i3);
    CHECK(c.param("l1") == l2);
    // down to one: the last cannot be removed
    for (int k = 0; k < 6; ++k) { m.dbl(c.v->beadCentre(0)); c.sync(); }
    CHECK(c.v->activeFilters() == 1);
}

TEST_CASE("chord resonator panel: the tonic diamond switches on with a click, drags for its level and switches off with a double-click", "[ui][chordres]") {
    ChordRig c;
    Mouse m(*c.v);
    CHECK(c.param("tonic") == 0.0);
    const auto d = c.v->beadCentre(-1);
    m.click(d);
    CHECK(c.param("tonic") == 1.0);
    c.sync();
    const double boost = c.param("boost");
    m.down(d).drag(c.at(0, boost - 6.0)).up(c.at(0, boost - 6.0));
    CHECK(c.param("tonicLevel") == Approx(-6.0).margin(0.26));
    CHECK(c.param("tonic") == 1.0);
    c.sync();
    m.dbl(c.v->beadCentre(-1));
    CHECK(c.param("tonic") == 0.0);
    // the Tonic chip does the same
    c.sync();
    c.v->tonicChip()->mouseUp(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), {5.0f, 5.0f}, {}, 0, 0, 0, 0, 0, c.v->tonicChip(), c.v->tonicChip(), juce::Time::getCurrentTime(), {5.0f, 5.0f}, juce::Time::getCurrentTime(), 1, false));
    CHECK(c.param("tonic") == 1.0);
}

TEST_CASE("chord resonator panel: source and steepness are chosen with words, and the knobs are there for the rest", "[ui][chordres]") {
    ChordRig c;
    auto click = [&](juce::Component* ch) { Mouse m(*ch); m.click({5.0f, 5.0f}); };
    click(c.v->sourceChip(1));
    CHECK(c.param("source") == 1.0);
    click(c.v->sourceChip(2));
    CHECK(c.param("source") == 2.0);
    click(c.v->steepChip(3));
    CHECK(c.param("steep") == 3.0);
    click(c.v->steepChip(0));
    CHECK(c.param("steep") == 0.0);
    for (const char* k : {"boost", "q", "excite", "sat", "mix", "glide", "thresh"}) CHECK(c.v->knob(k) != nullptr);
    CHECK(c.v->knob("l1") == nullptr);                                   // no knob for what the beads do
    // a knob drives its parameter
    auto* boost = dynamic_cast<ui::Knob*>(c.v->knob("boost"));
    REQUIRE(boost);
    Mouse mk(*boost);
    mk.down({20.0f, 30.0f}).drag({20.0f, 10.0f}).up({20.0f, 10.0f});
    CHECK(c.param("boost") > 12.0);
}

TEST_CASE("chord resonator panel: in the device chain it is one wide panel without a grid of knobs", "[ui][chordres]") {
    Rig r; r.demo();
    const auto track = r.m->project().tracks[1].uid;
    r.m->selectTrack(track);
    ui::DeviceChainView v(*r.m);
    v.setSize(1800, 300);
    v.refresh(app::ModelEvent::Selection);
    const int before = v.panelCount();
    r.m->apply(edit::addDevice(r.m->project(), track, "fx", "chordres"));
    v.refresh(app::ModelEvent::Document);
    CHECK(v.panelCount() == before + 1);
    std::vector<ui::ChordResonatorView*> views;
    findAll(v, views);
    REQUIRE(views.size() == 1);
    CHECK(views[0]->getWidth() >= ui::ChordResonatorView::kWidth - 20);
    std::vector<ui::Knob*> knobs;
    findAll(*views[0], knobs);
    CHECK(knobs.size() == 7);                                            // the parameters the beads and chips do not cover
    // a change in the document reaches the picture
    r.m->apply(document::cmd::setParam(edit::findTrack(r.m->project(), track)->fx.back().uid, "i1", 9));
    v.refresh(app::ModelEvent::Document);
    CHECK(views[0]->value("i1") == 9.0);
}

// ---- the effect pictures (ARCH 27) ----

namespace {
struct FxRig {
    Rig r;
    project::Uid uid = 0;
    const app::DeviceInfo* info = nullptr;
    std::unique_ptr<ui::FxView> v;
    explicit FxRig(const std::string& type, int availH = 260) {
        r.demo();
        const auto track = r.m->project().tracks[1].uid;
        r.m->apply(edit::addDevice(r.m->project(), track, "fx", type));
        uid = edit::findTrack(r.m->project(), track)->fx.back().uid;
        info = app::findDevice(app::Chain::Effect, type);
        v = ui::FxView::make(*r.m, uid, *info, availH);
        if (!v) return;
        v->setSize(v->preferredWidth(), availH - 36);
        sync();
    }
    void sync() { v->sync(*r.m->document().findDevice(uid)); }
    double param(const char* key) {
        const auto* d = r.m->document().findDevice(uid);
        const auto it = d->params.find(key);
        if (it != d->params.end()) return it->second;
        for (const auto& p : info->params) if (std::string(p.key) == key) return double(p.def);
        return -999;
    }
    juce::Point<float> logAt(double hz, double db, double lo, double hi) {
        const auto d = v->displayBounds();
        return {ddaw::ui::fx::xLog(d, hz), ddaw::ui::fx::yLin(d, db, lo, hi)};
    }
};
}  // namespace

TEST_CASE("effect pictures: every effect has one, every parameter has a control, and it fits its panel", "[ui][fx]") {
    int checked = 0;
    for (const auto& d : app::deviceCatalog()) {
        if (d.chain != app::Chain::Effect || d.type == "chordres") continue;          // the chord resonator has its own view
        for (int availH : {200, 260, 360}) {
            INFO(d.type << " at " << availH);
            FxRig f(d.type, availH);
            REQUIRE(f.v != nullptr);
            for (const auto& p : d.params) { INFO(p.key); CHECK(f.v->covers(p.key)); }
            CHECK(f.v->preferredWidth() >= 280);
            CHECK(f.v->preferredWidth() <= 1000);
            const auto bounds = f.v->getLocalBounds();
            std::vector<juce::Component*> kids;
            for (auto* c : f.v->getChildren()) if (c->isVisible()) kids.push_back(c);
            for (auto* c : kids) { INFO("child at " << c->getX() << "," << c->getY() << " " << c->getWidth() << "x" << c->getHeight()); CHECK(bounds.contains(c->getBounds())); }
            const auto img = f.v->createComponentSnapshot(bounds, true, 1.0f);            // paints without trouble, animated or not
            CHECK(img.getWidth() == bounds.getWidth());
            f.v->tick();
            ++checked;
        }
    }
    CHECK(checked >= 3 * 24);
}

TEST_CASE("effect pictures: the EQs and the filter are dragged on a frequency plot", "[ui][fx]") {
    {   // EQ Three: a drag in a band's region sets that band's gain
        FxRig f("eq");
        const auto d = f.v->displayBounds();
        Mouse m(*f.v);
        const auto from = f.logAt(120, 0, -15, 15), to = f.logAt(120, 6, -15, 15);
        m.down(from).drag(to).up(to);
        CHECK(f.param("low") == Approx(6.0).margin(0.6));
        const auto hi = f.logAt(8000, 0, -15, 15);
        m.down(hi).drag(f.logAt(8000, -9, -15, 15)).up(f.logAt(8000, -9, -15, 15));
        CHECK(f.param("high") == Approx(-9.0).margin(0.6));
        m.dbl(f.logAt(8000, 0, -15, 15));                                              // a double-click flattens the band
        CHECK(f.param("high") == 0.0);
        (void)d;
    }
    {   // EQ Seven: a node moves in frequency and gain; the wheel is its Q
        FxRig f("eq7");
        Mouse m(*f.v);
        const auto node = f.logAt(f.param("b3_freq"), f.param("b3_gain"), -18, 18);
        m.down(node).drag(f.logAt(2000, 6, -18, 18)).up(f.logAt(2000, 6, -18, 18));
        CHECK(f.param("b3_freq") == Approx(2000.0).margin(60.0));
        CHECK(f.param("b3_gain") == Approx(6.0).margin(0.6));
        f.sync();
        const double q0 = f.param("b3_q");
        const auto at = f.logAt(f.param("b3_freq"), f.param("b3_gain"), -18, 18);
        Mouse shift(*f.v, juce::ModifierKeys(juce::ModifierKeys::shiftModifier));        // Shift-drag is the Q
        shift.down(at).drag({at.x, at.y - 10.0f}).drag({at.x, at.y - 40.0f}).up({at.x, at.y - 40.0f});
        CHECK(f.param("b3_q") > q0 * 1.8);
        CHECK(f.param("b3_freq") == Approx(2000.0).margin(60.0));                         // frequency and gain stayed put
        f.sync();
        const auto low = f.logAt(f.param("b0_freq"), f.param("b0_gain"), -18, 18);
        shift.down(low).drag({low.x, low.y - 40.0f}).up({low.x, low.y - 40.0f});          // a shelf has no Q
        CHECK(f.param("b0_q") == 1.0);
        // the wheel over a node or a knob edits nothing: it is left for the pane to scroll
        juce::MouseWheelDetails w{}; w.deltaY = 1.0f;
        const auto gainBefore = f.param("b3_gain"), qBefore = f.param("b3_q");
        f.v->mouseWheelMove(shift.ev(at), w);
        CHECK(f.param("b3_gain") == gainBefore);
        CHECK(f.param("b3_q") == qBefore);
    }
    {   // Filter: the node is the cutoff (sideways) and the resonance (up and down)
        FxRig f("filter");
        Mouse m(*f.v);
        const auto node = f.logAt(f.param("freq"), f.param("q"), -54, 18);
        m.down(node).drag(f.logAt(500, 0, -54, 18)).up(f.logAt(500, 0, -54, 18));
        CHECK(f.param("freq") == Approx(500.0).margin(25.0));
        CHECK(f.param("q") < 1.0);
    }
}

TEST_CASE("effect pictures: dynamics, time, colour and utility pictures edit their parameters", "[ui][fx]") {
    {   // Chebyshev: clicking a harmonic's bar sets the order
        FxRig f("cheby");
        Mouse m(*f.v);
        const auto d = f.v->displayBounds().reduced(10, 24).withTrimmedBottom(14);
        m.click({d.getX() + (6 - 0.5f) / 14.0f * d.getWidth(), d.getCentreY()});
        CHECK(f.param("order") == 6.0);
    }
    {   // Bit Crusher: dragging across the picture sets the bit depth
        FxRig f("crush");
        Mouse m(*f.v);
        const auto d = f.v->displayBounds();
        m.down({d.getX() + 2.0f, d.getCentreY()}).drag({d.getRight() - 2.0f, d.getCentreY()}).up({d.getRight() - 2.0f, d.getCentreY()});
        CHECK(f.param("bits") == 16.0);
        m.down({d.getX() + 2.0f, d.getCentreY()}).up({d.getX() + 2.0f, d.getCentreY()});
        CHECK(f.param("bits") == 1.0);
    }
    {   // Frequency Shifter: drag sideways for Hz, double-click to zero
        FxRig f("shift");
        Mouse m(*f.v);
        const auto c = f.v->displayBounds().getCentre();
        m.down(c).drag(c.translated(100.0f, 0.0f)).up(c.translated(100.0f, 0.0f));
        CHECK(f.param("amt") > 100.0);
        m.dbl(c);
        CHECK(f.param("amt") == 0.0);
    }
    {   // Widener: the distance of the handle from the middle is the width; double-click for unity
        FxRig f("widen");
        Mouse m(*f.v);
        const auto d = f.v->displayBounds();
        m.down({d.getCentreX() + 5.0f, d.getCentreY()}).drag({d.getCentreX() + 200.0f, d.getCentreY()}).up({d.getCentreX() + 200.0f, d.getCentreY()});
        CHECK(f.param("width") == 1.0);
        m.dbl(d.getCentre());
        CHECK(f.param("width") == 0.5);
    }
    {   // a choice is a segmented control or a menu, and picking one sets the parameter
        FxRig f("autotune");
        auto* mode = dynamic_cast<ui::Choice*>(f.v->control("mode"));
        REQUIRE(mode);
        CHECK(!mode->isMenu());
        const auto seg = mode->segment(1);
        Mouse m(*mode);
        m.click(seg.getCentre().toFloat());
        CHECK(f.param("mode") == 1.0);
        auto* root = dynamic_cast<ui::Choice*>(f.v->control("root"));
        REQUIRE(root);
        CHECK(root->isMenu());                                                          // thirteen names: a menu
        CHECK(root->count() == 13);
    }
    {   // an LFO effect: dragging the scope sideways is the phase, the knob and the shape choice are there
        FxRig f("trem");
        Mouse m(*f.v);
        const auto d = f.v->displayBounds().reduced(10, 24).withTrimmedBottom(8);
        const auto start = juce::Point<float>(d.getCentreX(), d.getY() + 4.0f);
        m.down(start).drag(start.translated(-d.getWidth() / 4.0f, 0.0f)).up(start.translated(-d.getWidth() / 4.0f, 0.0f));
        CHECK(f.param("phase") == Approx(180.0).margin(2.0));                           // a quarter of the two-cycle plot is half a cycle
        CHECK(f.v->control("phase") != nullptr);
        CHECK(f.v->control("shape") != nullptr);
    }
}

TEST_CASE("effect pictures: they sit in the device chain in place of a knob grid", "[ui][fx]") {
    Rig r; r.demo();
    const auto track = r.m->project().tracks[1].uid;
    r.m->selectTrack(track);
    ui::DeviceChainView v(*r.m);
    v.setSize(2400, 300);
    v.refresh(app::ModelEvent::Selection);
    for (const char* t : {"eq7", "comp", "delay", "reverb", "chorus", "dist"}) r.m->apply(edit::addDevice(r.m->project(), track, "fx", t));
    v.refresh(app::ModelEvent::Document);
    std::vector<ui::FxView*> views;
    findAll(v, views);
    CHECK(views.size() == 7);                                                            // the six, and the demo track's own Filter
    // the panel's own knobs belong to the instrument and the existing filter only: effect parameters are not knob grids any more
    std::vector<ui::Knob*> knobs;
    for (auto* c : v.getChildren()) findAll(*c, knobs);
    size_t inViews = 0;
    for (auto* fv : views) { std::vector<ui::Knob*> k; findAll(*fv, k); inViews += k.size(); }
    CHECK(inViews > 10);
}

TEST_CASE("parameter display: attack, release and hold in milliseconds, depth as 0-100, phase in degrees", "[appmodel][names]") {
    auto spec = [](const char* type, const char* key) -> const ParamSpec& {
        for (const auto& d : app::deviceCatalog()) if (d.type == type) for (const auto& p : d.params) if (std::string(p.key) == key) return p;
        FAIL("no such parameter " << type << "." << key);
        static ParamSpec none{}; return none;
    };
    using app::formatParam;
    CHECK(formatParam(spec("comp", "attack"), 0.01) == "10 ms");
    CHECK(formatParam(spec("comp", "attack"), 0.001) == "1.0 ms");
    CHECK(formatParam(spec("comp", "release"), 0.2) == "200 ms");
    CHECK(formatParam(spec("poly", "release"), 1.5) == "1500 ms");
    CHECK(formatParam(spec("gate", "hold"), 0.05) == "50 ms");
    CHECK(formatParam(spec("chorus", "depth"), 0.5) == "50");
    CHECK(formatParam(spec("chorus", "depth"), 1.0) == "100");
    CHECK(formatParam(spec("vib", "depth"), 0.6) == "100");                              // a vibrato's 0..0.6 still reads 0-100
    CHECK(formatParam(spec("vib", "depth"), 0.15) == "25");
    CHECK(formatParam(spec("chorus", "phase"), 90.0) == juce::String(juce::CharPointer_UTF8("90\xC2\xB0")).toStdString());
}

// ---- the piano roll's tools, snap and note operations (ARCH 28) ----

namespace {
struct RollRig {
    Rig r;
    std::unique_ptr<ui::ClipEditor> v;
    edit::ClipRef ref;
    RollRig() {
        r.demo();
        ref = {r.m->project().tracks[3].uid, "s3", ""};
        r.m->selectClip(ref);
        v = std::make_unique<ui::ClipEditor>(*r.m);
        v->setSize(1200, 380);
        v->refresh(app::ModelEvent::Selection);
        v->scale().originTick = 0; v->scale().pxPerBeat = 64;
        v->setGridTicks(24.0);                                                           // 1/16 notes: 24 ticks
        clear();                                                                         // an empty clip; the tests add what they need
    }
    const std::vector<project::Note>& notes() const { return edit::findClip(r.m->project(), ref)->notes; }
    juce::Point<float> at(double tick, int pitch) const { return {float(ui::ClipEditor::kKeysW + v->scale().toX(tick)), float(v->rowTop(pitch) + 5)}; }
    void clear() { std::vector<project::Uid> ids; for (auto& n : notes()) ids.push_back(n.uid); for (auto id : ids) r.m->apply(edit::removeNote(ref, id)); }
    const project::Note* find(int pitch, double start) const { for (auto& n : notes()) if (n.pitch == pitch && std::abs(n.startTicks - start) < 0.01) return &n; return nullptr; }
};
}  // namespace

TEST_CASE("piano roll tools: the Draw tool adds a note on click, sets its length by dragging, and remembers the length", "[ui][clipeditor][tools]") {
    RollRig f;
    f.v->setTool(ui::ClipEditor::Tool::Draw);
    const size_t n0 = f.notes().size();
    Mouse m(*f.v);
    m.click(f.at(24 * 10 + 5, 66));                                                    // click: a note at the snapped start, one grid step long
    REQUIRE(f.notes().size() == n0 + 1);
    REQUIRE(f.find(66, 240) != nullptr);
    CHECK(f.find(66, 240)->durTicks == Approx(24.0));
    const auto from = f.at(24 * 12 + 3, 68), to = f.at(24 * 12 + 24 * 4 + 3, 68);       // drag right: a longer note
    m.down(from).drag({from.x + 2, from.y}).drag(to).up(to);
    REQUIRE(f.find(68, 288) != nullptr);
    CHECK(f.find(68, 288)->durTicks == Approx(96.0));
    CHECK(f.v->lastLength() == Approx(96.0));
    m.click(f.at(24 * 20, 70));                                                        // the next note has the length of the last one
    CHECK(f.find(70, 480)->durTicks == Approx(96.0));
    f.r.m->undo();                                                                      // each is one undo step
    CHECK(f.find(70, 480) == nullptr);
    CHECK(f.find(68, 288) != nullptr);
    // a note under the pointer moves like in Select
    const auto* existing = f.find(68, 288);
    const auto start = f.at(existing->startTicks + 10, 68);
    m.down(start).drag({start.x + 4, start.y}).drag({start.x + float(f.v->scale().toX(48) - f.v->scale().toX(0)), start.y}).up({start.x + float(f.v->scale().toX(48) - f.v->scale().toX(0)), start.y});
    CHECK(f.find(68, 336) != nullptr);
}

TEST_CASE("piano roll tools: Paint lays a note per grid step along the drag, and clicking a note takes it away", "[ui][clipeditor][tools]") {
    RollRig f;
    f.v->setTool(ui::ClipEditor::Tool::Paint);
    const size_t n0 = f.notes().size();
    Mouse m(*f.v);
    const auto a = f.at(24 * 8, 66);
    m.down(a).drag(f.at(24 * 9 + 2, 66)).drag(f.at(24 * 10 + 2, 67)).drag(f.at(24 * 11 + 2, 68)).up(f.at(24 * 11 + 2, 68));
    CHECK(f.notes().size() == n0 + 4);
    CHECK(f.find(66, 192) != nullptr);
    CHECK(f.find(66, 216) != nullptr);
    CHECK(f.find(67, 240) != nullptr);                                                 // the pitch follows the row under the pointer
    CHECK(f.find(68, 264) != nullptr);
    f.r.m->undo();                                                                      // the whole stroke is one undo step
    CHECK(f.notes().size() == n0);
    f.r.m->redo();
    m.click(f.at(24 * 9 + 4, 66));                                                     // a click on a note deletes it
    CHECK(f.find(66, 216) == nullptr);
    CHECK(f.notes().size() == n0 + 3);
}

TEST_CASE("piano roll tools: Erase deletes along the drag, and the right button erases in the drawing tools", "[ui][clipeditor][tools]") {
    RollRig f;
    f.v->setTool(ui::ClipEditor::Tool::Paint);
    Mouse m(*f.v);
    m.down(f.at(24 * 8, 66)).drag(f.at(24 * 12, 66)).up(f.at(24 * 12, 66));
    const size_t n1 = f.notes().size();
    f.v->setTool(ui::ClipEditor::Tool::Erase);
    m.down(f.at(24 * 8 + 4, 66)).drag(f.at(24 * 9 + 4, 66)).drag(f.at(24 * 10 + 4, 66)).up(f.at(24 * 10 + 4, 66));
    CHECK(f.notes().size() == n1 - 3);
    CHECK(f.find(66, 264) != nullptr);                                                 // the note the stroke did not reach
    f.v->setTool(ui::ClipEditor::Tool::Draw);
    Mouse right(*f.v, juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier));
    right.down(f.at(24 * 11 + 4, 66)).up(f.at(24 * 11 + 4, 66));
    CHECK(f.find(66, 264) == nullptr);
    CHECK(f.notes().size() == n1 - 4);
}

TEST_CASE("piano roll tools: Slice cuts the notes it crosses at the snapped line", "[ui][clipeditor][tools]") {
    RollRig f;
    f.r.m->apply(edit::addNote(f.ref, 66, 192.0, 96.0));
    f.r.m->apply(edit::addNote(f.ref, 72, 192.0, 96.0));
    const size_t n0 = f.notes().size();
    f.v->setTool(ui::ClipEditor::Tool::Slice);
    Mouse m(*f.v);
    m.click(f.at(192 + 48 + 3, 66));                                                   // a click cuts the note in that row at the line
    REQUIRE(f.notes().size() == n0 + 1);
    const auto* a = f.find(66, 192);
    const auto* b = f.find(66, 240);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->durTicks == Approx(48.0));
    CHECK(b->durTicks == Approx(48.0));
    CHECK(f.find(72, 192)->durTicks == Approx(96.0));                                  // the other row is untouched
    // a drag across two rows cuts both
    const auto top = f.at(192 + 72 + 2, 72), bottom = f.at(192 + 72 + 2, 65);
    m.down(top).drag(bottom).up(bottom);
    CHECK(f.find(72, 264) != nullptr);
    f.r.m->undo();
    CHECK(f.find(72, 264) == nullptr);
}

TEST_CASE("piano roll tools: the Chord tool stamps a chord, and the shape is chosen from a list", "[ui][clipeditor][tools]") {
    RollRig f;
    f.v->setTool(ui::ClipEditor::Tool::Chord);
    const size_t n0 = f.notes().size();
    Mouse m(*f.v);
    m.click(f.at(24 * 10, 60));
    CHECK(f.notes().size() == n0 + 3);
    CHECK(f.find(60, 240) != nullptr);
    CHECK(f.find(64, 240) != nullptr);
    CHECK(f.find(67, 240) != nullptr);
    CHECK(f.v->selected().size() == 3);
    f.v->setChord(4);                                                                   // major seventh
    m.click(f.at(24 * 14, 60));
    CHECK(f.notes().size() == n0 + 7);
    CHECK(f.find(71, 336) != nullptr);
    f.r.m->undo();
    CHECK(f.notes().size() == n0 + 3);                                                  // a chord is one undo step
}

TEST_CASE("piano roll snap: Alt bypasses it, the magnet switches it off, triplets change the step, the key can be snapped to", "[ui][clipeditor][snap]") {
    RollRig f;
    f.r.m->apply(edit::addNote(f.ref, 66, 96.0, 48.0));
    const auto id = f.find(66, 96.0)->uid;
    auto movedStart = [&](juce::ModifierKeys mods) {
        Mouse m(*f.v, mods);
        const auto* n = f.find(66, 96.0);
        if (!n) for (auto& q : f.notes()) if (q.uid == id) n = &q;
        const auto from = f.at(n->startTicks + 12, n->pitch);
        const auto to = juce::Point<float>(from.x + float(f.v->scale().toX(30) - f.v->scale().toX(0)), from.y);
        m.down(from).drag({from.x + 4, from.y}).drag(to).up(to);
        for (auto& q : f.notes()) if (q.uid == id) return q.startTicks;
        return -1.0;
    };
    CHECK(movedStart({}) == Approx(120.0));                                              // 96 + 30 snapped to the 24-tick grid
    f.r.m->undo();
    CHECK(movedStart(juce::ModifierKeys(juce::ModifierKeys::altModifier)) == Approx(126.0).margin(1.0));   // Alt: exactly where the pointer went
    f.r.m->undo();
    f.v->setSnap(false);                                                                 // the magnet is off: free movement...
    CHECK(movedStart({}) == Approx(126.0).margin(1.0));
    f.r.m->undo();
    CHECK(movedStart(juce::ModifierKeys(juce::ModifierKeys::altModifier)) == Approx(120.0));   // ...and Alt turns it on for one gesture
    f.r.m->undo();
    f.v->setSnap(true);
    CHECK(f.v->gridStep() == Approx(24.0));
    f.v->setTriplet(true);
    CHECK(f.v->gridStep() == Approx(16.0));
    f.v->setTriplet(false);
    // the key: A minor (the demo's) lights its notes, and drawing can snap pitches into it
    f.v->setScaleHighlight(true);
    f.v->refresh(app::ModelEvent::Selection);
    CHECK(f.v->inScale(57));                                                             // A
    CHECK(f.v->inScale(60));                                                             // C
    CHECK_FALSE(f.v->inScale(58));                                                       // A#
    f.v->setScaleSnap(true);
    f.v->setTool(ui::ClipEditor::Tool::Draw);
    Mouse m(*f.v);
    m.click(f.at(24 * 30, 61));                                                          // C#: not in the key, drawn on the nearest note of it
    bool found = false;
    for (auto& n : f.notes()) if (std::abs(n.startTicks - 24 * 30) < 0.01 && (n.pitch == 60 || n.pitch == 62)) found = true;
    CHECK(found);
}

TEST_CASE("piano roll tools: Cmd-drag copies, Shift holds an axis, the left edge resizes from the start, the right button opens the edit menu", "[ui][clipeditor][tools]") {
    RollRig f;
    f.r.m->apply(edit::addNote(f.ref, 66, 192.0, 96.0));
    const size_t n0 = f.notes().size();
    const auto id = f.notes().back().uid;
    const auto rect = f.v->noteRect(f.notes().back());
    const double beat = f.v->scale().toX(96) - f.v->scale().toX(0);
    {   // Cmd-drag: a copy lands a beat later, the original stays
        Mouse m(*f.v, juce::ModifierKeys(juce::ModifierKeys::commandModifier));
        const auto from = juce::Point<float>(float(rect.getX() + rect.getWidth() / 2), float(rect.getCentreY()));
        m.down(from).drag({from.x + 4, from.y}).drag({from.x + float(beat), from.y}).up({from.x + float(beat), from.y});
        CHECK(f.notes().size() == n0 + 1);
        CHECK(f.find(66, 192) != nullptr);
        CHECK(f.find(66, 288) != nullptr);
        f.r.m->undo();
        CHECK(f.notes().size() == n0);
    }
    {   // Shift: a diagonal drag moves only along the axis it is mostly going
        Mouse m(*f.v, juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
        const auto from = juce::Point<float>(float(rect.getX() + rect.getWidth() / 2), float(rect.getCentreY()));
        m.down(from).drag({from.x + 10, from.y + 2}).drag({from.x + float(beat), from.y - float(3 * f.v->rowH())}).up({from.x + float(beat), from.y - float(3 * f.v->rowH())});
        for (auto& n : f.notes()) if (n.uid == id) { CHECK(n.pitch == 66); CHECK(n.startTicks == Approx(288.0)); }
        f.r.m->undo();
    }
    {   // the left edge: the note starts a beat later and is a beat shorter, the end stays put
        f.v->selectAll();
        Mouse m(*f.v);
        const auto r2 = f.v->noteRect(*std::find_if(f.notes().begin(), f.notes().end(), [&](auto& n) { return n.uid == id; }));
        const auto from = juce::Point<float>(float(r2.getX() + 1), float(r2.getCentreY()));
        f.v->selectAll();
        m.down(from).drag({from.x + 4, from.y}).drag({from.x + float(beat / 2.0), from.y}).up({from.x + float(beat / 2.0), from.y});
        for (auto& n : f.notes()) if (n.uid == id) { CHECK(n.startTicks == Approx(240.0)); CHECK(n.durTicks == Approx(48.0)); }
    }
    {   // the right button on a note selects it (the menu itself is async): nothing is edited
        f.v->selectAll();
        Mouse right(*f.v, juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier));
        const auto r3 = f.v->noteRect(f.notes().back());
        const size_t before = f.notes().size();
        right.down(juce::Point<float>(float(r3.getCentreX()), float(r3.getCentreY()))).up(juce::Point<float>(float(r3.getCentreX()), float(r3.getCentreY())));
        CHECK(f.notes().size() == before);
        juce::PopupMenu::dismissAllActiveMenus();
    }
}

TEST_CASE("piano roll operations: quantize, legato, chop, reverse, transpose, stretch and velocity apply to the selection (or all notes) as one undo step", "[ui][clipeditor][ops]") {
    RollRig f;
    f.clear();
    f.r.m->apply(edit::addNote(f.ref, 60, 10.0, 20.0));
    f.r.m->apply(edit::addNote(f.ref, 62, 100.0, 20.0));
    f.r.m->apply(edit::addNote(f.ref, 64, 200.0, 20.0));
    REQUIRE(f.notes().size() == 3);
    using Op = ui::ClipEditor::Op;
    CHECK(f.v->applyOp(Op::Quantize));                                                   // nothing selected: all notes
    CHECK(f.find(60, 0.0) != nullptr);
    CHECK(f.find(62, 96.0) != nullptr);
    CHECK(f.find(64, 192.0) != nullptr);
    f.r.m->undo();
    CHECK(f.find(60, 10.0) != nullptr);                                                  // one undo for the whole operation
    CHECK(f.v->applyOp(Op::Legato));
    CHECK(f.find(60, 10.0)->durTicks == Approx(90.0));
    f.r.m->undo();
    f.v->selectAll();
    CHECK(f.v->applyOp(Op::OctaveUp));
    CHECK(f.find(72, 10.0) != nullptr);
    CHECK(f.v->applyOp(Op::OctaveDown));
    CHECK(f.v->applyOp(Op::Double));
    CHECK(f.find(60, 10.0 + 0.0) != nullptr);
    CHECK(f.find(62, 190.0) != nullptr);                                                 // 10 + (100 - 10) * 2
    CHECK(f.v->applyOp(Op::Half));
    CHECK(f.find(62, 100.0) != nullptr);
    CHECK(f.v->applyOp(Op::FlipTime));
    CHECK(f.find(64, 10.0) != nullptr);                                                  // the last note is first now
    CHECK(f.v->applyOp(Op::FlipTime));
    const size_t before = f.notes().size();
    CHECK(f.v->applyOp(Op::Chop2));                                                      // each note into two: three more
    CHECK(f.notes().size() == before + 3);
    f.r.m->undo();
    CHECK(f.notes().size() == before);
    CHECK(f.v->applyOp(Op::VelocityFull));
    for (auto& n : f.notes()) CHECK(n.velocity == 1.0);
    CHECK(f.v->applyOp(Op::VelocityDown));
    for (auto& n : f.notes()) CHECK(n.velocity == Approx(0.85));
}

TEST_CASE("piano roll: copy, cut and paste, tool shortcuts, and painting velocities across several bars", "[ui][clipeditor][tools]") {
    RollRig f;
    f.clear();
    for (int i = 0; i < 4; ++i) f.r.m->apply(edit::addNote(f.ref, 60 + i, 96.0 * i, 48.0, 0.5));
    f.v->selectAll();
    CHECK(f.v->keyPressed(juce::KeyPress('c', juce::ModifierKeys::commandModifier, 0)));
    CHECK(ui::ClipEditor::hasClipboard());
    f.v->scale().originTick = 384.0;                                                     // paste at the left edge of the view (snapped)
    CHECK(f.v->keyPressed(juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0)));
    CHECK(f.notes().size() == 8);
    CHECK(f.find(60, 384.0) != nullptr);
    CHECK(f.find(63, 384.0 + 288.0) != nullptr);
    CHECK(f.v->selected().size() == 4);
    CHECK(f.v->keyPressed(juce::KeyPress('x', juce::ModifierKeys::commandModifier, 0)));  // cut the pasted ones
    CHECK(f.notes().size() == 4);
    f.r.m->undo();
    CHECK(f.notes().size() == 8);
    // keys pick the tools
    CHECK(f.v->keyPressed(juce::KeyPress('b', 0, 'b')));
    CHECK(f.v->tool() == ui::ClipEditor::Tool::Draw);
    CHECK(f.v->keyPressed(juce::KeyPress('d', 0, 'd')));
    CHECK(f.v->tool() == ui::ClipEditor::Tool::Erase);
    CHECK(f.v->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    CHECK(f.v->tool() == ui::ClipEditor::Tool::Select);
    // the velocity lane: one drag across the bars sets them all along the stroke
    f.v->scale().originTick = 0;
    const auto g = f.v->gridRect();
    Mouse m(*f.v);
    const int x0 = ui::ClipEditor::kKeysW + int(f.v->scale().toX(0)) + 1, x1 = ui::ClipEditor::kKeysW + int(f.v->scale().toX(96.0 * 3)) + 1;
    m.down(pt(x0, g.getBottom() + 50)).drag(pt((x0 + x1) / 2, g.getBottom() + 30)).drag(pt(x1, g.getBottom() + 10)).up(pt(x1, g.getBottom() + 10));
    std::vector<double> vs;
    for (int i = 0; i < 4; ++i) vs.push_back(f.find(60 + i, 96.0 * i)->velocity);
    CHECK(vs[0] < vs[1]);
    CHECK(vs[1] < vs[2]);
    CHECK(vs[2] < vs[3]);                                                                 // a ramp up along the stroke
    CHECK(vs[3] > 0.8);
}

TEST_CASE("the mouse wheel never changes a parameter: knobs and choices leave it for the pane to scroll", "[ui][wheel]") {
    FxRig f("delay");
    juce::MouseWheelDetails w{}; w.deltaY = 1.0f; w.deltaX = 0.5f;
    const auto ev = [&](juce::Component& c) { return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), {5.0f, 5.0f}, {}, 0, 0, 0, 0, 0, &c, &c, juce::Time::getCurrentTime(), {5.0f, 5.0f}, juce::Time::getCurrentTime(), 1, false); };
    for (const char* key : {"fb", "mix", "tone", "time"}) {
        INFO(key);
        auto* c = f.v->control(key);
        REQUIRE(c != nullptr);
        const double before = f.param(key);
        c->mouseWheelMove(ev(*c), w);                      // the default handler hands the event to the parent: nothing is edited here
        f.sync();
        CHECK(f.param(key) == before);
    }
    // a wheel over a panel reaches the chain, which scrolls
    Rig r; r.demo();
    const auto track = r.m->project().tracks[1].uid;
    r.m->selectTrack(track);
    ui::DeviceChainView v(*r.m);
    v.setSize(500, 300);
    v.refresh(app::ModelEvent::Selection);
    r.m->apply(edit::addDevice(r.m->project(), track, "fx", "delay"));
    v.refresh(app::ModelEvent::Document);
    std::vector<ui::Knob*> knobs;
    findAll(v, knobs);
    REQUIRE(!knobs.empty());
    const double before = knobs.back()->value();
    knobs.back()->mouseWheelMove(ev(*knobs.back()), w);
    CHECK(knobs.back()->value() == before);
}

// ---- drum machines: named pads and the kit dropdown (ARCH 29) ----

TEST_CASE("drum machines: pad parameters are labelled with the pad's name, and every kit names its pads", "[appmodel][drumkit]") {
    CHECK(app::drumKits().size() == 4);
    CHECK(std::string(app::drumPadName("drum", 3)) == "Closed Hat");
    CHECK(std::string(app::drumPadName("drum808", 6)) == "Cowbell");
    CHECK(std::string(app::drumPadName("drum909", 6)) == "Rim Shot");
    CHECK(std::string(app::drumPadName("drumlinn", 7)) == "Tambourine");
    CHECK(std::string(app::drumPadName("drum", 9)) == "Snare");                    // the pitch wraps around eight pads
    for (const auto& k : app::drumKits()) {
        INFO(k.type);
        CHECK(app::isDrumKit(k.type));
        const auto* info = app::findDevice(app::Chain::Instrument, k.type);
        REQUIRE(info != nullptr);
        CHECK(info->params.size() == 24);
    }
    CHECK_FALSE(app::isDrumKit("poly"));
    project::DeviceSpec d;
    d.type = "drum909";
    CHECK(app::paramLabelFor(d, "p3_decay") == "Closed Hat Decay");
    CHECK(app::paramLabelFor(d, "p0_tune") == "Kick Tune");
    CHECK(app::paramLabelFor(d, "p7_level") == "Crash Level");
    d.type = "drumlinn";
    CHECK(app::paramLabelFor(d, "p7_decay") == "Tambourine Decay");
    d.type = "drum";
    CHECK(app::paramLabelFor(d, "p1_level") == "Snare Level");
    d.type = "poly";
    CHECK(app::paramLabelFor(d, "p1_level") != "Snare Level");                       // only the drum machines
}

TEST_CASE("drum machines: the panel shows the pads by name and a dropdown switches the kit, keeping settings and samples", "[ui][drumkit]") {
    Rig r; r.demo();
    const auto track = r.m->project().tracks[0].uid;                                 // the demo's drum track
    r.m->selectTrack(track);
    ui::DeviceChainView v(*r.m);
    v.setSize(1400, 300);
    v.refresh(app::ModelEvent::Selection);
    std::vector<ui::DrumKitView*> views;
    findAll(v, views);
    REQUIRE(views.size() == 1);
    CHECK(views[0]->kitLabel().contains("Standard"));
    CHECK(views[0]->padLabel(0) == "Kick");
    CHECK(views[0]->padLabel(3) == "Closed Hat");
    REQUIRE(views[0]->knob(3, "decay") != nullptr);
    // a knob drives the pad's parameter
    r.m->apply(document::cmd::setParam(r.m->project().tracks[0].inst.uid, "p0_tune", 3.0));
    // switch to the 808: the instrument changes, the settings stay, the pads are renamed
    const auto before = r.m->project().tracks[0].inst.params;
    views[0]->switchKit("drum808");
    CHECK(r.m->project().tracks[0].inst.type == "drum808");
    CHECK(r.m->project().tracks[0].inst.params.at("p0_tune") == 3.0);
    CHECK(r.m->project().tracks[0].inst.params.size() >= before.size());
    v.refresh(app::ModelEvent::Document);
    views.clear();
    findAll(v, views);
    REQUIRE(views.size() == 1);
    CHECK(views[0]->kitLabel().contains("808"));
    CHECK(views[0]->padLabel(6) == "Cowbell");
    CHECK(views[0]->knob(0, "tune")->value() == 3.0);
    // and back, in one undo step each
    views[0]->switchKit("drumlinn");
    CHECK(r.m->project().tracks[0].inst.type == "drumlinn");
    r.m->undo();
    CHECK(r.m->project().tracks[0].inst.type == "drum808");
    r.m->undo();
    CHECK(r.m->project().tracks[0].inst.type == "drum");
    // the generic knob grid is gone: every knob in the device panel belongs to the pads
    v.refresh(app::ModelEvent::Document);
    views.clear();
    findAll(v, views);
    REQUIRE(views.size() == 1);
    std::vector<ui::Knob*> knobs;
    findAll(*views[0], knobs);
    CHECK(knobs.size() == 24);
}

// ---- the trim strips on every effect panel (ARCH 30) ----

TEST_CASE("effect panels: every effect has an input and an output trim with a meter; instruments do not", "[ui][fxtrim]") {
    Rig r; r.demo();
    auto& bank = const_cast<engine::MeterBank&>(r.eng.meters());
    const auto track = r.m->project().tracks[1].uid;
    r.m->selectTrack(track);
    r.m->apply(edit::addDevice(r.m->project(), track, "fx", "comp"));
    const auto comp = r.m->project().tracks[1].fx.back().uid;
    ui::DeviceChainView v(*r.m);
    v.setSize(2000, 300);
    v.refresh(app::ModelEvent::Selection);
    std::vector<ui::TrimStrip*> strips;
    findAll(v, strips);
    const size_t effects = r.m->project().tracks[1].fx.size();
    CHECK(strips.size() == 2 * effects);                                        // none on the instrument's panel
    // find the compressor's pair: its slot is the last one
    ui::TrimStrip *in = nullptr, *out = nullptr;
    for (size_t i = 0; i + 1 < strips.size(); i += 2) if (i / 2 == effects - 1) { in = strips[i]; out = strips[i + 1]; }
    REQUIRE(in != nullptr);
    REQUIRE(out != nullptr);
    CHECK(in->getX() < out->getX());                                            // input on the far left, output on the far right
    // dragging sets the trims: up is louder, one gesture is one undo step, double-click resets
    {
        Mouse m(*in);
        m.down(pt(10, 12)).drag(pt(10, 2)).drag(pt(10, -8)).up(pt(10, -8));
        CHECK(r.m->project().tracks[1].fx.back().inDb.value_or(0.0) == Approx(5.0).margin(0.3));
        r.m->undo();
        CHECK_FALSE(r.m->project().tracks[1].fx.back().inDb.has_value());
        r.m->redo();
        m.dbl(pt(10, 12));
        CHECK_FALSE(r.m->project().tracks[1].fx.back().inDb.has_value());       // zero is stored as nothing
    }
    {
        Mouse m(*out);
        m.down(pt(10, 12)).drag(pt(10, 32)).up(pt(10, 32));
        CHECK(r.m->project().tracks[1].fx.back().outDb.value_or(0.0) == Approx(-5.0).margin(0.3));
    }
    // the meters follow the engine's input and output levels of this effect (its slot among the track's enabled effects)
    const size_t slot = effects - 1;
    bank.setFxIo(1, int(slot), 0.5f, 0.25f, 1.0f);
    v.tick();
    CHECK(in->level() == Approx(0.5f));
    CHECK(out->level() == Approx(0.25f));
    CHECK(r.m->fxInputPeak(comp) == Approx(0.5f));
    // the wheel over a trim leaves it alone
    juce::MouseWheelDetails w{}; w.deltaY = 1.0f;
    const double before = out->value();
    out->mouseWheelMove(Mouse(*out).ev(pt(10, 12)), w);
    CHECK(out->value() == before);
}

TEST_CASE("effect trims: the document keeps them (in and out), saves them and restores them", "[document][fxtrim]") {
    Rig r; r.demo();
    const auto track = r.m->project().tracks[1].uid;
    r.m->apply(edit::addDevice(r.m->project(), track, "fx", "delay"));
    const auto uid = r.m->project().tracks[1].fx.back().uid;
    r.m->apply({"device.set", {{"uid", uid}, {"field", "in"}, {"value", -4.5}}});
    r.m->apply({"device.set", {{"uid", uid}, {"field", "out"}, {"value", 3.0}}});
    CHECK(r.m->project().tracks[1].fx.back().inDb.value() == -4.5);
    const auto json = project::deviceToJson(r.m->project().tracks[1].fx.back());
    CHECK(json.at("in") == -4.5);
    CHECK(json.at("out") == 3.0);
    r.m->undo();
    r.m->undo();
    CHECK_FALSE(r.m->project().tracks[1].fx.back().inDb.has_value());
    CHECK_FALSE(r.m->project().tracks[1].fx.back().outDb.has_value());
    CHECK_FALSE(project::deviceToJson(r.m->project().tracks[1].fx.back()).contains("in"));
}

// ---- the unified session view: mixing in the header, clips below, the master locked right (ARCH 31) ----

namespace {
struct SessRig {
    Rig r;
    std::unique_ptr<ui::SessionView> v;
    SessRig(int w = 1400, int h = 700) {
        r.demo();
        v = std::make_unique<ui::SessionView>(*r.m);
        v->setSize(w, h);
        v->refresh(app::ModelEvent::Document);
    }
    int idx(const char* name) { for (size_t i = 0; i < r.m->project().tracks.size(); ++i) if (r.m->project().tracks[i].name == name) return int(i); return -1; }
    project::Uid uid(const char* name) { return r.m->project().tracks[size_t(idx(name))].uid; }
};
}  // namespace

TEST_CASE("session view: hit testing the clip grid under the mixing header, creating clips and deleting them", "[ui][session]") {
    SessRig f;
    auto& v = *f.v;
    using K = ui::SessionView::Hit::Kind;
    const int hh = v.headerH();
    const int x1 = v.columnX(0) + ui::SessionView::kTrackW / 2 + 20, y1 = hh + ui::SessionView::kRowH / 2;           // the first track, scene 0
    CHECK(v.hitAt({x1, y1}).kind == K::Slot);
    CHECK(v.hitAt({x1, y1}).track == 0);
    CHECK(v.hitAt({v.columnX(0) + 8, y1}).kind == K::SlotLaunch);
    CHECK(v.hitAt({8, y1}).kind == K::SceneLaunch);
    CHECK(v.hitAt({x1, 10}).kind == K::TrackHeader);
    CHECK(v.hitAt({20, hh + int(f.r.m->project().scenes.size()) * ui::SessionView::kRowH + 10}).kind == K::AddScene);
    // a bus has no clips: its column is only the mixing section
    const int bus = f.idx("Space");
    CHECK(v.hitAt({v.columnX(bus) + 40, y1}).kind == K::None);
    CHECK(v.hitAt({v.columnX(bus) + 40, 10}).kind == K::TrackHeader);
    // an empty slot on the Lead track, scene s1: double-click creates a clip
    const int lead = f.idx("Lead");
    const std::string key = f.r.m->project().tracks[size_t(lead)].id + "|s1";
    REQUIRE_FALSE(f.r.m->project().clips.count(key));
    Mouse(v).dbl(pt(v.columnX(lead) + 60, y1));
    REQUIRE(f.r.m->project().clips.count(key));
    CHECK(f.r.m->selection().clip.scene == "s1");
    CHECK(v.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK_FALSE(f.r.m->project().clips.count(key));
    f.r.m->undo();
    CHECK(f.r.m->project().clips.count(key));
    const size_t scenes = f.r.m->project().scenes.size();
    Mouse(v).click(pt(20, hh + int(scenes) * ui::SessionView::kRowH + 10));
    CHECK(f.r.m->project().scenes.size() == scenes + 1);
}

TEST_CASE("session view: each column has its mixing section, the inputs come before the buses with a line between them", "[ui][session][mixer]") {
    SessRig f;
    auto& v = *f.v;
    CHECK(v.inputCount() == 5);                                                   // drums, bass, chords, lead, pluck
    CHECK(v.busCount() == 3);                                                     // Space, Send A, Send B
    CHECK(v.separatorX() > v.columnX(f.idx("Pluck")) + ui::SessionView::kTrackW - 2);
    CHECK(v.separatorX() < v.columnX(f.idx("Space")) + 2);
    for (const char* n : {"Drums", "Bass", "Chords", "Lead", "Pluck", "Space", "Send A", "Send B"}) {
        INFO(n);
        auto* st = v.strip(f.uid(n));
        REQUIRE(st != nullptr);
        CHECK(st->getBounds().getY() == 0);
        CHECK(st->getHeight() == v.headerH());
        CHECK(st->getWidth() == ui::SessionView::kTrackW);
    }
    // a bus is listed after every input however the document orders them
    f.r.m->apply(edit::addTrack(f.r.m->project(), project::TrackKind::Synth, "Late"));
    v.refresh(app::ModelEvent::Document);
    CHECK(v.inputCount() == 6);
    CHECK(v.columnOf(f.idx("Late")) < v.columnOf(f.idx("Space")));
    CHECK(v.columnX(f.idx("Late")) < v.separatorX());
}

TEST_CASE("session view: the mixing section's fader, pan, mute and solo edit the track, one undo step per gesture", "[ui][session][mixer]") {
    SessRig f;
    auto& v = *f.v;
    auto* st = v.strip(f.uid("Bass"));
    REQUIRE(st);
    const int ti = f.idx("Bass");
    const double before = f.r.m->project().tracks[size_t(ti)].gainDb;
    {
        Mouse m(st->fader());
        const int h = st->fader().getHeight();
        m.down(pt(15, h / 2)).drag(pt(15, h / 6)).drag(pt(15, 4)).up(pt(15, 4));
    }
    CHECK(f.r.m->project().tracks[size_t(ti)].gainDb > before);
    f.r.m->undo();
    CHECK(f.r.m->project().tracks[size_t(ti)].gainDb == Approx(before));
    {
        Mouse m(st->pan());
        m.down(pt(st->pan().getWidth() - 4, 6)).up(pt(st->pan().getWidth() - 4, 6));
        CHECK(f.r.m->project().tracks[size_t(ti)].pan > 0.8);
        m.dbl(pt(10, 6));
        CHECK(f.r.m->project().tracks[size_t(ti)].pan == 0.0);
    }
    st->mute().mouseUp(Mouse(st->mute()).ev(pt(5, 5)));
    CHECK(f.r.m->project().tracks[size_t(ti)].mute);
    st->solo().mouseUp(Mouse(st->solo()).ev(pt(5, 5)));
    CHECK(f.r.m->project().tracks[size_t(ti)].solo);
    // clicking a strip selects its track (and clears a master selection)
    f.r.m->selectMaster();
    Mouse(*st).click(pt(60, 10));
    CHECK(f.r.m->selection().track == f.uid("Bass"));
    CHECK_FALSE(f.r.m->selection().master);
}

TEST_CASE("session view: record arm and input monitoring are on the strips that can use them", "[ui][session][recording]") {
    SessRig f;
    f.r.m->apply(edit::addTrack(f.r.m->project(), project::TrackKind::Audio));
    const auto audio = f.r.m->project().tracks.back().uid;
    f.v->refresh(app::ModelEvent::Document);
    auto* st = f.v->strip(audio);
    REQUIRE(st);
    CHECK(st->arm().isVisible());
    CHECK(st->monitor().isVisible());                                              // an audio track can monitor its input
    CHECK_FALSE(f.v->strip(f.uid("Bass"))->monitor().isVisible());                  // a synth track cannot
    CHECK(f.v->strip(f.uid("Bass"))->arm().isVisible());                            // but it records the notes played live
    CHECK_FALSE(f.v->strip(f.uid("Space"))->arm().isVisible());                     // a bus has neither
    CHECK_FALSE(f.v->strip(f.uid("Space"))->stop().isVisible());
    CHECK_FALSE(f.r.m->recording().armed(audio));
    st->arm().mouseUp(Mouse(st->arm()).ev(pt(5, 5)));
    CHECK(f.r.m->recording().armed(audio));
    CHECK_FALSE(f.r.m->monitored(audio));
    st->monitor().mouseUp(Mouse(st->monitor()).ev(pt(5, 5)));
    CHECK(f.r.m->monitored(audio));
    f.r.m->tick();
    st->arm().mouseUp(Mouse(st->arm()).ev(pt(5, 5)));
    CHECK_FALSE(f.r.m->recording().armed(audio));
    // a bus cannot be armed; a synth or drum track can (for notes)
    f.r.m->recording().arm(f.uid("Space"), true);
    CHECK_FALSE(f.r.m->recording().anyArmed());
    f.r.m->recording().arm(f.uid("Drums"), true);
    CHECK(f.r.m->recording().anyArmed());
    f.r.m->recording().arm(f.uid("Drums"), false);
    // REC without an armed track reports instead of starting
    ui::TransportBar bar(*f.r.m);
    bar.setSize(1400, 48);
    std::vector<ui::Chip*> chips;
    findAll(bar, chips);
    REQUIRE(chips.size() > 3);
    ui::Chip* rec = chips[2];   // PLAY, STOP, REC in the order the bar adds them
    rec->mouseUp(Mouse(*rec).ev(pt(5, 5)));
    CHECK_FALSE(f.r.m->recording().recording());
    CHECK(f.r.m->status().find("arm a track first") != std::string::npos);
    // arming survives, and is dropped when the track is deleted
    f.r.m->recording().arm(audio, true);
    f.r.m->apply(edit::removeTrack(audio));
    CHECK_FALSE(f.r.m->recording().anyArmed());
}

TEST_CASE("session view: the master fader is locked at the far right however many tracks scroll past, and selects the master chain", "[ui][session][master]") {
    SessRig f(900, 700);
    auto& v = *f.v;
    for (int i = 0; i < 12; ++i) f.r.m->apply(edit::addTrack(f.r.m->project(), project::TrackKind::Synth));
    v.refresh(app::ModelEvent::Document);
    const auto where = v.masterBounds();
    CHECK(where.getRight() == v.getWidth());
    CHECK(where.getWidth() == ui::SessionView::kMasterW);
    REQUIRE(v.masterStrip() != nullptr);
    CHECK(v.masterStrip()->getBounds() == where);
    // scrolled all the way along: the master has not moved, and the strips under it are clipped away from it
    v.scrollTo(100000, 0);
    CHECK(v.scrollX() > 0);
    CHECK(v.masterStrip()->getBounds() == where);
    CHECK(v.strip(f.uid("Send B"))->getRight() + ui::SessionView::kSceneW <= v.getWidth() - ui::SessionView::kMasterW + ui::SessionView::kTrackW);
    v.scrollTo(0, 100000);                                                          // the grid scrolls down; the header and the master stay
    CHECK(v.masterStrip()->getBounds() == where);
    CHECK(v.strip(f.uid("Bass"))->getY() == 0);
    // the master fader edits the master gain
    {
        auto& fader = v.masterStrip()->fader();
        Mouse m(fader);
        const int h = fader.getHeight();
        m.down(pt(20, h)).drag(pt(20, h * 2 / 3)).up(pt(20, h * 2 / 3));
    }
    CHECK(f.r.m->project().meta.masterGainDb != 0.0);
    // clicking the master selects the master chain: the device pane shows it, and there is no button for it
    ui::DeviceChainView chain(*f.r.m);
    chain.setSize(1200, 300);
    chain.refresh(app::ModelEvent::Selection);
    CHECK_FALSE(chain.showingMaster());
    Mouse(*v.masterStrip()).click(pt(50, 10));
    CHECK(f.r.m->selection().master);
    chain.refresh(app::ModelEvent::Selection);
    CHECK(chain.showingMaster());
    std::vector<ui::Chip*> chips;
    findAll(chain, chips);
    for (auto* c : chips) CHECK(c->text() != "Master chain");
    f.r.m->selectTrack(f.uid("Bass"));
    chain.refresh(app::ModelEvent::Selection);
    CHECK_FALSE(chain.showingMaster());
}

TEST_CASE("session view: a strip has a send level to every bus, each its own, and a bus can send to the other buses", "[ui][session][sends]") {
    SessRig f;
    auto& v = *f.v;
    auto* bass = v.strip(f.uid("Bass"));
    REQUIRE(bass);
    const auto& p = f.r.m->project();
    std::vector<std::string> buses;
    for (auto& t : p.tracks) if (t.kind == project::TrackKind::Bus) buses.push_back(t.id);
    CHECK(bass->sendRows() + bass->hiddenSends() == int(buses.size()));            // a row for each (or the "+n" chip when they do not fit)
    for (const auto& b : buses) if (auto* s = bass->send(b)) CHECK(s->getHeight() == ui::MixStrip::kSendRowH);
    // different levels to different buses
    auto* toA = bass->send(p.tracks[size_t(f.idx("Send A"))].id);
    auto* toB = bass->send(p.tracks[size_t(f.idx("Send B"))].id);
    REQUIRE(toA != nullptr);
    REQUIRE(toB != nullptr);
    Mouse(*toA).click(pt(toA->getWidth() / 4, 5));
    Mouse(*toB).click(pt(toB->getWidth() * 3 / 4, 5));
    const auto& sends = f.r.m->project().tracks[size_t(f.idx("Bass"))].sends;
    CHECK(sends.at(p.tracks[size_t(f.idx("Send A"))].id) < sends.at(p.tracks[size_t(f.idx("Send B"))].id));
    Mouse(*toA).dbl(pt(5, 5));                                                      // double-click clears it
    CHECK_FALSE(f.r.m->project().tracks[size_t(f.idx("Bass"))].sends.count(p.tracks[size_t(f.idx("Send A"))].id));
    // a bus sends to the others too, but never round to a bus that feeds it
    auto* sendA = v.strip(f.uid("Send A"));
    REQUIRE(sendA);
    const std::string idB = p.tracks[size_t(f.idx("Send B"))].id;
    REQUIRE(sendA->send(idB) != nullptr);
    CHECK(sendA->send(p.tracks[size_t(f.idx("Send A"))].id) == nullptr);             // not to itself
    Mouse(*sendA->send(idB)).click(pt(30, 5));
    CHECK(f.r.m->project().tracks[size_t(f.idx("Send A"))].sends.count(idB));
    v.refresh(app::ModelEvent::Document);
    CHECK(v.strip(f.uid("Send B"))->send(p.tracks[size_t(f.idx("Send A"))].id) == nullptr);      // B -> A would close the loop
    CHECK(v.strip(f.uid("Send B"))->send(p.tracks[size_t(f.idx("Space"))].id) != nullptr);
}

TEST_CASE("session view: a bus's output is chosen on its strip; tracks have none", "[ui][session][sends]") {
    SessRig f;
    auto* a = f.v->strip(f.uid("Send A"));
    REQUIRE(a);
    CHECK(a->out().isVisible());
    CHECK(a->outLabel().contains("Master"));
    CHECK_FALSE(f.v->strip(f.uid("Bass"))->out().isVisible());
    f.r.m->apply(document::cmd::setTrack(f.uid("Send A"), "output", f.r.m->project().tracks[size_t(f.idx("Space"))].id));
    f.v->refresh(app::ModelEvent::Document);
    CHECK(a->outLabel().contains("Space"));
    // a loop is refused, and so is a target that is not a bus
    CHECK_FALSE(f.r.m->apply(document::cmd::setTrack(f.uid("Space"), "output", f.r.m->project().tracks[size_t(f.idx("Send A"))].id)));
    CHECK_FALSE(f.r.m->apply(document::cmd::setTrack(f.uid("Send B"), "output", f.r.m->project().tracks[size_t(f.idx("Bass"))].id)));
    CHECK_FALSE(f.r.m->apply(document::cmd::setTrack(f.uid("Send B"), "output", f.r.m->project().tracks[size_t(f.idx("Send B"))].id)));
    CHECK(f.r.m->apply(document::cmd::setTrack(f.uid("Send B"), "output", "master")));
}

TEST_CASE("session view: the mixing header collapses to names and buttons for small windows", "[ui][session][mixer]") {
    SessRig f;
    auto& v = *f.v;
    const int open = v.headerH();
    v.setMixOpen(false);
    CHECK(v.headerH() < open);
    CHECK(v.strip(f.uid("Bass"))->getHeight() == v.headerH());
    CHECK_FALSE(v.strip(f.uid("Bass"))->fader().isVisible());
    CHECK(v.strip(f.uid("Bass"))->mute().isVisible());                               // the buttons stay
    const int y1 = v.headerH() + ui::SessionView::kRowH / 2;
    CHECK(v.hitAt({v.columnX(0) + 40, y1}).kind == ui::SessionView::Hit::Kind::Slot);   // and the clips come up under it
    v.setMixOpen(true);
    CHECK(v.headerH() == open);
    CHECK(v.strip(f.uid("Bass"))->fader().isVisible());
}

// ---- the routing matrix ----

namespace {
struct RoutRig {
    SessRig s;
    std::unique_ptr<ui::RoutingView> v;
    RoutRig() {
        v = std::make_unique<ui::RoutingView>(*s.r.m);
        v->setSize(1100, 700);
        v->refresh(app::ModelEvent::Document);
    }
    juce::Point<float> centre(const char* from, const char* to) { return v->cellRect(s.uid(from), std::string(to) == "Master" ? 0 : s.uid(to)).getCentre().toFloat(); }
    juce::Point<float> corner(const char* from, const char* to) { return v->cornerRect(s.uid(from), std::string(to) == "Master" ? 0 : s.uid(to)).getCentre().toFloat(); }
    double level(const char* from, const char* to) { return v->sendLevel(s.uid(from), s.uid(to)); }
};
}  // namespace

TEST_CASE("routing matrix: every source down the side, every bus and the master across, inputs and buses apart", "[ui][routing]") {
    RoutRig f;
    CHECK(f.v->rowCount() == 8);                                                     // five inputs and three buses
    CHECK(f.v->columnCount() == 4);                                                  // three buses and the master
    // inputs always output to the master; a bus may output to the master or to another bus
    CHECK(f.v->isOutput(f.s.uid("Bass"), 0));
    CHECK_FALSE(f.v->outputAllowed(f.s.uid("Bass"), f.s.uid("Space")));
    CHECK(f.v->outputAllowed(f.s.uid("Send A"), f.s.uid("Space")));
    CHECK(f.v->outputAllowed(f.s.uid("Send A"), 0));
    CHECK_FALSE(f.v->sendAllowed(f.s.uid("Send A"), f.s.uid("Send A")));
    CHECK(f.v->sendAllowed(f.s.uid("Send A"), f.s.uid("Send B")));
    CHECK(f.v->sendAllowed(f.s.uid("Bass"), f.s.uid("Send A")));
    // the cells of the inputs sit above those of the buses, with a gap between the groups
    CHECK(f.v->cellRect(f.s.uid("Pluck"), 0).getY() < f.v->cellRect(f.s.uid("Space"), 0).getY() - ui::RoutingView::kCellH);
}

TEST_CASE("routing matrix: click a cell to switch a send on, drag for its level, right-click to clear; a bus has a send to every other bus", "[ui][routing][sends]") {
    RoutRig f;
    Mouse m(*f.v);
    // a click turns it on, a click again turns it off
    m.click(f.centre("Bass", "Send A"));
    CHECK(f.level("Bass", "Send A") == Approx(0.6));
    m.click(f.centre("Bass", "Send A"));
    CHECK(f.level("Bass", "Send A") == 0.0);
    // a drag up sets the level, and one gesture is one undo step
    const auto from = f.centre("Bass", "Send B");
    m.down(from).drag({from.x, from.y - 10}).drag({from.x, from.y - 40}).up({from.x, from.y - 40});
    CHECK(f.level("Bass", "Send B") == Approx(0.5).margin(0.02));
    f.s.r.m->undo();
    CHECK(f.level("Bass", "Send B") == 0.0);
    // every bus can have its own level to every other bus
    m.click(f.centre("Send A", "Send B"));
    m.click(f.centre("Send A", "Space"));
    const auto d = f.centre("Send A", "Space");
    m.down(d).drag({d.x, d.y - 10}).drag({d.x, d.y - 30}).up({d.x, d.y - 30});
    CHECK(f.level("Send A", "Send B") == Approx(0.6));
    CHECK(f.level("Send A", "Space") > 0.7);
    CHECK(f.level("Send A", "Send B") != f.level("Send A", "Space"));
    // the right button clears
    Mouse right(*f.v, juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier));
    right.down(f.centre("Send A", "Send B")).up(f.centre("Send A", "Send B"));
    CHECK(f.level("Send A", "Send B") == 0.0);
    // a cell that would close a loop does nothing: Send A now feeds Space, so Space cannot send back to it
    CHECK_FALSE(f.v->sendAllowed(f.s.uid("Space"), f.s.uid("Send A")));
    m.click(f.centre("Space", "Send A"));
    CHECK(f.level("Space", "Send A") == 0.0);
    // nor does a cell on the diagonal
    m.click(f.centre("Space", "Space"));
    CHECK(f.level("Space", "Space") == 0.0);
}

TEST_CASE("routing matrix: the diamond routes a bus's output to the master or to another bus, never in a loop", "[ui][routing][output]") {
    RoutRig f;
    Mouse m(*f.v);
    const auto& tracks = [&]() -> const std::vector<project::Track>& { return f.s.r.m->project().tracks; };
    auto outOf = [&](const char* n) { return tracks()[size_t(f.s.idx(n))].output; };
    CHECK((outOf("Send A").empty() || outOf("Send A") == "master"));
    CHECK(f.v->isOutput(f.s.uid("Send A"), 0));
    m.click(f.corner("Send A", "Space"));                                              // Send A -> Space
    CHECK(outOf("Send A") == tracks()[size_t(f.s.idx("Space"))].id);
    CHECK(f.v->isOutput(f.s.uid("Send A"), f.s.uid("Space")));
    CHECK_FALSE(f.v->isOutput(f.s.uid("Send A"), 0));                                  // exactly one output per row
    // Space can no longer be routed into Send A (it would loop), nor can its send go there
    CHECK_FALSE(f.v->outputAllowed(f.s.uid("Space"), f.s.uid("Send A")));
    m.click(f.corner("Space", "Send A"));
    CHECK((outOf("Space").empty() || outOf("Space") == "master"));
    // chain: Space -> Send B -> Master; and back to the master with a click
    m.click(f.corner("Space", "Send B"));
    CHECK(outOf("Space") == tracks()[size_t(f.s.idx("Send B"))].id);
    m.click(f.corner("Send A", "Master"));
    CHECK(outOf("Send A") == "master");
    // an input's output is always the master and its diamond there cannot be moved
    m.click(f.corner("Bass", "Master"));
    CHECK(f.v->isOutput(f.s.uid("Bass"), 0));
    // hovering says what a cell does
    m.down(f.centre("Send A", "Send B")).up(f.centre("Send A", "Send B"));
    f.v->mouseMove(m.ev(f.centre("Send A", "Send B")));
    CHECK(f.v->hoverText().contains("Send A"));
    CHECK(f.v->hoverText().contains("Send B"));
}

TEST_CASE("routing matrix: the wheel scrolls it and never edits a send", "[ui][routing][wheel]") {
    RoutRig f;
    f.v->setSize(1100, 260);
    f.v->refresh(app::ModelEvent::Document);
    juce::MouseWheelDetails w{}; w.deltaY = -1.0f;
    const auto before = f.s.r.m->project().tracks[size_t(f.s.idx("Bass"))].sends;
    f.v->mouseWheelMove(Mouse(*f.v).ev(f.centre("Bass", "Send A")), w);
    CHECK(f.s.r.m->project().tracks[size_t(f.s.idx("Bass"))].sends == before);
    CHECK(f.v->scrollY() > 0);                                                         // a pane scrolled
}

TEST_CASE("sessions start with Send A (a reverb) and Send B (a ping pong) as ordinary buses; older sends fold into per-bus sends", "[appmodel][sends]") {
    struct Fresh { engine::Engine eng; std::unique_ptr<app::AppModel> m; Fresh() { eng.prepare(48000.0); m = std::make_unique<app::AppModel>(eng, 48000.0); } } r;   // not blank: what a new session is
    const auto& p = r.m->project();
    REQUIRE(p.tracks.size() == 2);
    CHECK(p.tracks[0].name == "Send A");
    CHECK(p.tracks[1].name == "Send B");
    for (auto& t : p.tracks) { CHECK(t.kind == project::TrackKind::Bus); CHECK(t.send == project::SendBus::None); }      // plain buses: no letter
    CHECK(p.tracks[0].fx.at(0).type == "reverb");
    CHECK(p.tracks[1].fx.at(0).type == "pingpong");
    // a new project too
    r.m->apply(edit::addTrack(r.m->project(), project::TrackKind::Synth));
    r.m->newProject();
    CHECK(r.m->project().tracks.size() == 2);
    // legacy: a project with sendA/sendB knobs and letter buses (or built-in returns) becomes per-bus sends on plain buses
    project::Project old;
    project::Track a; a.id = "t1"; a.name = "Lead"; a.sendA = 0.3; a.sendB = 0.5;
    project::Track bus; bus.id = "t2"; bus.name = "Return A"; bus.kind = project::TrackKind::Bus; bus.send = project::SendBus::A;
    project::Return ret; ret.id = "r1"; ret.name = "Echo"; ret.fxType = "delay"; ret.gainDb = -3.0; ret.params["mix"] = 1.0;
    old.tracks = {a, bus};
    old.returns = {ret, ret};
    edit::migrateLegacySends(old);
    REQUIRE(old.tracks.size() == 4);                                                       // the two returns became buses
    CHECK(old.returns.empty());
    CHECK(old.tracks[1].send == project::SendBus::None);
    CHECK(old.tracks[0].sendA == 0.0);
    CHECK(old.tracks[0].sendB == 0.0);
    CHECK(old.tracks[0].sends.at("t2") == Approx(0.3));                                     // send A went to the letter bus
    CHECK(old.tracks[0].sends.size() == 2);                                                 // send B to the bus made from the second return
    CHECK(old.tracks[3].fx.at(0).type == "delay");
    CHECK(old.tracks[3].gainDb == Approx(-3.0));
    edit::migrateLegacySends(old);                                                          // idempotent
    CHECK(old.tracks.size() == 4);
}
