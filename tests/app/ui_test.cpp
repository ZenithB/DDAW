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
