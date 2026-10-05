#include "app/ui/TrackingPanel.h"

#include <cmath>

#include "app/model/Timeline.h"
#include "app/ui/ParamPicker.h"
#include "project/ProjectJson.h"

namespace ddaw::ui {

namespace {
constexpr size_t kTraceLen = 220;   // ~7 s at 30 Hz
}

TrackingPanel::TrackingPanel(app::AppModel& m, LiveInput& live) : View(m), live_(live) {
    for (juce::Component* c : std::initializer_list<juce::Component*>{&track_, &target_, &curves_, &addRoute_, &minHz_, &keys_, &notes_, &poly_, &mpe_, &bendRange_, &mpeRange_}) addAndMakeVisible(c);
    track_.setToggleable(true);
    curves_.setToggleable(true);
    keys_.setToggleable(true);
    poly_.setToggleable(true);
    mpe_.setToggleable(true);
    mpe_.onToggle = [this](bool on) { model.setMpe(on); };
    bendRange_.setValue(model.bendRange());
    bendRange_.onChange = [this](double v) { model.setBendRange(float(v)); };
    mpeRange_.setValue(model.mpeRange());
    mpeRange_.onChange = [this](double v) { model.setMpeRange(float(v)); };
    poly_.onToggle = [this](bool on) { model.setPolyInput(on); };
    notes_.setToggleable(true);
    keys_.onToggle = [this](bool on) { live_.setKeyboardEnabled(on); repaint(); };
    keys_.setOn(live_.keyboardEnabled());
    notes_.setOn(model.recording().settings().recordNotes);
    notes_.onToggle = [this](bool on) { model.recording().settings().recordNotes = on; model.notify(app::ModelEvent::Recording); };
    track_.onToggle = [this](bool on) { model.setTracking(on, model.trackingTarget()); };
    target_.onClick = [this] {
        juce::PopupMenu menu;
        menu.addItem(1, "No instrument (routes only)");
        int id = 2;
        std::vector<project::Uid> uids;
        for (auto& t : model.project().tracks)
            if (t.kind == project::TrackKind::Synth) { menu.addItem(id++, juce::String(t.name) + "  (" + t.inst.type + ")"); uids.push_back(t.uid); }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target_), [this, uids](int r) {
            if (r == 1) model.setTracking(model.tracking(), 0);
            else if (r >= 2 && size_t(r - 2) < uids.size()) model.setTracking(model.tracking(), uids[size_t(r - 2)]);
        });
    };
    curves_.onToggle = [this](bool on) { model.recording().settings().recordCurves = on; model.notify(app::ModelEvent::Recording); };
    addRoute_.onClick = [this] { addRouteMenu(); };
    minHz_.onChange = [this](double v) { model.setTrackerRange(float(v), 1500.0f); };
    minHz_.setValue(70.0);
    refresh(app::ModelEvent::Recording);
}

int TrackingPanel::routeCount() const {
    const auto* t = app::edit::findTrack(model.project(), model.selection().track);
    return t ? int(t->perf.size()) : 0;
}

void TrackingPanel::resized() {
    track_.setBounds(14, 12, 120, 26);
    target_.setBounds(142, 12, 170, 26);
    minHz_.setBounds(14, 76, 90, 26);
    curves_.setBounds(14, 112, 120, 26);
    notes_.setBounds(142, 112, 170, 26);
    keys_.setBounds(14, 148, 160, 26);
    poly_.setBounds(180, 148, 134, 26);
    mpe_.setBounds(14, 196, 60, 26);
    bendRange_.setBounds(116, 196, 64, 26);
    mpeRange_.setBounds(250, 196, 64, 26);
    addRoute_.setBounds(getWidth() - 100, kTopH - 24, 86, 24);
}

void TrackingPanel::refresh(app::ModelEvent) {
    track_.setOn(model.tracking());
    curves_.setOn(model.recording().settings().recordCurves);
    notes_.setOn(model.recording().settings().recordNotes);
    keys_.setOn(live_.keyboardEnabled());
    poly_.setOn(model.polyInput());
    mpe_.setOn(model.mpe());
    const auto* t = app::edit::findTrack(model.project(), model.trackingTarget());
    target_.setText(t ? "Plays: " + juce::String(t->name) : juce::String("Plays: none"));
    repaint();
}

void TrackingPanel::tick() {
    if (!model.tracking() && !model.recording().recording()) { if (!pitchTrace_.empty()) { pitchTrace_.clear(); loudTrace_.clear(); repaint(); } return; }
    last_ = model.engine().latestPerformance();
    pitchTrace_.push_back(last_.f0Hz > 0 ? float(12.0 * std::log2(double(last_.f0Hz) / 16.3516)) : -1.0f);   // semitones above C0
    loudTrace_.push_back(std::clamp((last_.loudnessDb + 60.0f) / 60.0f, 0.0f, 1.0f));
    while (pitchTrace_.size() > kTraceLen) { pitchTrace_.pop_front(); loudTrace_.pop_front(); }
    repaint();
}

juce::String TrackingPanel::describe(const project::PerfSpec& p) const {
    juce::String s = juce::String(p.source) + "  ->  ";
    for (size_t i = 0; i < p.targets.size(); ++i) s << (i ? ", " : "") << juce::String(p.targets[i].dest == "inst" ? "instrument" : p.targets[i].dest == "mix" ? "mixer" : p.targets[i].fxId) << " " << juce::String(p.targets[i].pkey);
    return s;
}

void TrackingPanel::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    g.setColour(col::panel);
    g.fillRect(0, 0, 320, kTopH);
    g.setColour(col::line);
    g.drawVerticalLine(320, 0, float(kTopH));
    g.setColour(col::dim);
    g.setFont(uiFont(11.5f));
    g.drawText("Lowest pitch (a higher floor is faster)", 112, 76, 200, 26, juce::Justification::centredLeft);
    g.drawFittedText(model.recording().env().inputChannels && model.recording().env().inputChannels() > 0 ? "" : "The input opens when a track is armed or when you press REC.", juce::Rectangle<int>(14, 44, 300, 28), juce::Justification::topLeft, 2);

    g.setColour(col::dim);
    g.setFont(uiFont(11.0f));
    {
        const auto names = live_.deviceNames();
        juce::String t = names.isEmpty() ? juce::String("No MIDI inputs found") : "MIDI in: " + names.joinIntoString(", ");
        if (live_.keyboardEnabled()) t << juce::String::formatted("   keys: octave %d, velocity %d  (Z X C V)", live_.octave(), int(live_.velocity() * 127.0f));
        g.drawFittedText(t, juce::Rectangle<int>(14, 176, 300, 14), juce::Justification::topLeft, 1);
    }

    g.setColour(col::dim);
    g.setFont(uiFont(11.0f));
    g.drawText("bend", 80, 196, 34, 26, juce::Justification::centredRight);
    g.drawText("per note", 184, 196, 64, 26, juce::Justification::centredRight);
    g.drawFittedText(model.mpe() ? (juce::String("MPE on: ") + (model.mpeLowerMembers() ? "lower zone " + juce::String(model.mpeLowerMembers()) + " ch" : juce::String()) +
                                    (model.mpeLowerMembers() && model.mpeUpperMembers() ? ", " : "") + (model.mpeUpperMembers() ? "upper zone " + juce::String(model.mpeUpperMembers()) + " ch" : juce::String()) + ". Per-note bend, slide, pressure.")
                                 : juce::String("MPE off: a bend wheel bends every note."), juce::Rectangle<int>(14, 226, 300, 14), juce::Justification::topLeft, 1);

    // live readout
    const bool live = model.tracking() || model.recording().recording();
    g.setColour(col::text);
    g.setFont(monoFont(20.0f));
    juce::String note = "--";
    if (live && last_.f0Hz > 0) {
        const double midi = 69.0 + 12.0 * std::log2(double(last_.f0Hz) / 440.0);
        const int nearest = int(std::lround(midi));
        const int cents = int(std::lround((midi - nearest) * 100.0));
        note = juce::String(app::noteName(nearest)) + juce::String::formatted("  %+dc   %.1f Hz", cents, double(last_.f0Hz));
    }
    g.drawText(note, juce::Rectangle<int>(340, 8, 420, 28), juce::Justification::centredLeft);
    g.setColour(col::dim);
    g.setFont(uiFont(11.0f));
    g.drawText(juce::String::formatted("loudness %.0f dB   confidence %.0f%%   envelope %.2f", double(last_.loudnessDb), 100.0 * double(last_.confidence), double(last_.envelope)),
               juce::Rectangle<int>(340, 34, 480, 16), juce::Justification::centredLeft);

    // traces: pitch (semitone grid) over loudness
    auto plot = juce::Rectangle<int>(340, 56, std::max(100, getWidth() - 360), kTopH - 66);
    fillRounded(g, plot.toFloat(), col::black, 3.0f);
    g.setColour(col::grid);
    for (int s = 0; s <= 120; s += 12) {
        const float y = float(plot.getBottom()) - float(s - 12) / 72.0f * float(plot.getHeight());
        if (y > plot.getY() && y < plot.getBottom()) g.drawHorizontalLine(int(y), float(plot.getX()), float(plot.getRight()));
    }
    if (pitchTrace_.size() > 1) {
        const float dx = float(plot.getWidth()) / float(kTraceLen);
        juce::Path pitch, loud;
        bool pen = false;
        for (size_t i = 0; i < pitchTrace_.size(); ++i) {
            const float x = float(plot.getX()) + dx * float(kTraceLen - pitchTrace_.size() + i);
            const float ly = float(plot.getBottom()) - loudTrace_[i] * float(plot.getHeight()) * 0.35f;
            if (i == 0) loud.startNewSubPath(x, ly); else loud.lineTo(x, ly);
            if (pitchTrace_[i] < 0) { pen = false; continue; }
            const float y = float(plot.getBottom()) - (pitchTrace_[i] - 12.0f) / 72.0f * float(plot.getHeight());
            if (!pen) { pitch.startNewSubPath(x, y); pen = true; } else pitch.lineTo(x, y);
        }
        g.setColour(col::meterLow.withAlpha(0.55f));
        g.strokePath(loud, juce::PathStrokeType(1.0f));
        g.setColour(col::accent);
        g.strokePath(pitch, juce::PathStrokeType(2.0f));
    }

    // routes
    g.setColour(col::panel);
    g.fillRect(320, kTopH - 30, getWidth() - 320, 30);
    g.setColour(col::dim);
    g.setFont(uiFont(11.5f, true));
    const auto* t = app::edit::findTrack(model.project(), model.selection().track);
    g.drawText(t ? "Performance routes on " + juce::String(t->name) : juce::String("Select a track to route the performance"), juce::Rectangle<int>(340, kTopH - 28, 420, 26), juce::Justification::centredLeft);
    if (t) {
        for (int i = 0; i < int(t->perf.size()); ++i) {
            auto r = routeRect(i);
            fillRounded(g, r.toFloat(), col::panel2, 3.0f);
            g.setColour(t->perf[size_t(i)].on ? col::text : col::faint);
            g.setFont(uiFont(12.0f));
            g.drawText(describe(t->perf[size_t(i)]), r.withTrimmedLeft(10).withTrimmedRight(110), juce::Justification::centredLeft, true);
            auto rec = juce::Rectangle<float>(float(r.getRight()) - 100, float(r.getY()) + 3, 54, float(r.getHeight()) - 6);
            fillRounded(g, rec, t->perf[size_t(i)].record ? col::rec : col::raised, 3.0f);
            g.setColour(t->perf[size_t(i)].record ? col::black : col::dim);
            g.setFont(uiFont(10.5f, true));
            g.drawText("REC", rec.toNearestInt(), juce::Justification::centred);
            g.setColour(col::dim);
            g.drawText("x", juce::Rectangle<int>(r.getRight() - 40, r.getY(), 34, r.getHeight()), juce::Justification::centred);
        }
    }
}

void TrackingPanel::mouseDown(const juce::MouseEvent& e) {
    const auto* t = app::edit::findTrack(model.project(), model.selection().track);
    if (!t) return;
    for (int i = 0; i < int(t->perf.size()); ++i) {
        const auto r = routeRect(i);
        if (!r.contains(e.getPosition())) continue;
        if (e.x >= r.getRight() - 40) { model.apply({"perf.remove", {{"track", t->uid}, {"index", i}}}); return; }
        if (e.x >= r.getRight() - 100) {
            auto spec = t->perf[size_t(i)];
            spec.record = !spec.record;
            model.apply({"perf.edit", {{"track", t->uid}, {"index", i}, {"perf", project::perfToJson(spec)}}});
            return;
        }
        auto spec = t->perf[size_t(i)];
        spec.on = !spec.on;
        model.apply({"perf.edit", {{"track", t->uid}, {"index", i}, {"perf", project::perfToJson(spec)}}});
        return;
    }
}

void TrackingPanel::addRouteMenu() {
    juce::PopupMenu m;
    m.addItem(1, "Pitch (f0)");
    m.addItem(2, "Loudness");
    m.addItem(3, "Envelope");
    m.addItem(4, "Confidence");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addRoute_), [this](int r) {
        static const char* names[] = {"f0", "loudness", "envelope", "confidence"};
        if (r >= 1 && r <= 4) targetMenu(names[r - 1]);
    });
}

void TrackingPanel::targetMenu(const std::string& source) {
    const auto* t = app::edit::findTrack(model.project(), model.selection().track);
    if (!t) return;
    pickParam(model, t->uid, &addRoute_, [this, source, uid = t->uid](const PickedParam& c) {
        project::PerfSpec spec;
        spec.source = source;
        spec.targets.push_back({c.dest, c.fxId, c.pkey});
        const auto* tr = app::edit::findTrack(model.project(), uid);
        model.apply({"perf.insert", {{"track", uid}, {"index", tr ? tr->perf.size() : 0}, {"perf", project::perfToJson(spec)}}});
        if (!model.tracking()) model.setTracking(true, model.trackingTarget());   // a route is only useful with the tracker running
    });
}

}  // namespace ddaw::ui
