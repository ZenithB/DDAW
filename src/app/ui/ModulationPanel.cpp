#include "app/ui/ModulationPanel.h"

#include <algorithm>
#include <cmath>

#include "app/ui/Dialogs.h"
#include "app/ui/ParamPicker.h"
#include "engine/Modulation.h"
#include "project/ProjectJson.h"

namespace ddaw::ui {

namespace {
const char* const kShapes[] = {"Sine", "Triangle", "Saw up", "Saw down", "Square", "Sample&Hold", "Smooth rand"};
const char* const kDivs[] = {"8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/8 T", "1/16"};
const ParamSpec kDepthSpec{0, "depth", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, false};
const ParamSpec kMacroSpec{0, "macro", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, false};

}  // namespace

// ============================================================ LFO row
class ModulationPanel::LfoRow : public juce::Component {
public:
    LfoRow(app::AppModel& m, project::Uid track, int index) : model_(m), track_(track), idx_(index), depth_(kDepthSpec, "Depth") {
        const auto* t = trackOf(m, track);
        const auto& l = t->lfos[size_t(index)];
        for (juce::Component* c : std::initializer_list<juce::Component*>{&on_, &shape_, &sync_, &div_, &rate_, &depth_, &targets_, &del_}) addAndMakeVisible(c);
        on_.setToggleable(true); sync_.setToggleable(true);
        on_.onToggle = [this](bool v) { edit([v](project::LfoSpec& s) { s.on = v; }); };
        sync_.onToggle = [this](bool v) { edit([v](project::LfoSpec& s) { s.sync = v; }); };
        shape_.onClick = [this] { edit([](project::LfoSpec& s) { s.shape = (s.shape + 1) % 7; }); };
        div_.onClick = [this] { edit([](project::LfoSpec& s) { s.rate = std::fmod(std::round(s.rate) + 1.0, 9.0); }); };
        depth_.onBegin = [this] { model_.beginGesture("lfo depth"); };
        depth_.onChange = [this](double v) { model_.apply({"lfo.field", {{"track", track_}, {"index", idx_}, {"field", "depth"}, {"value", v}}}); };
        depth_.onEnd = [this] { model_.endGesture(); };
        rate_.onBegin = [this] { model_.beginGesture("lfo rate"); };
        rate_.onChange = [this](double v) { model_.apply({"lfo.field", {{"track", track_}, {"index", idx_}, {"field", "hz"}, {"value", v}}}); };
        rate_.onEnd = [this] { model_.endGesture(); };
        targets_.onClick = [this] { targetsMenu(); };
        del_.onClick = [this] { model_.apply({"lfo.remove", {{"track", track_}, {"index", idx_}}}); };
        sync(l);
    }
    void sync(const project::LfoSpec& l) {
        on_.setOn(l.on);
        shape_.setText(kShapes[std::clamp(l.shape, 0, 6)]);
        sync_.setOn(l.sync);
        div_.setText(kDivs[std::clamp(int(std::lround(l.rate)), 0, 8)]);
        div_.setVisible(l.sync);
        rate_.setVisible(!l.sync);
        rate_.setValue(l.hz);
        depth_.setValue(l.depth);
        depth_.setStored(true);
        targets_.setText(l.targets.empty() ? juce::String("no targets") : juce::String(int(l.targets.size())) + (l.targets.size() == 1 ? " target" : " targets"));
        on_.setAlpha(1.0f);
    }
    void resized() override {
        auto r = getLocalBounds().reduced(4, 6);
        on_.setBounds(r.removeFromLeft(34).withHeight(24).withY(r.getY() + 14)); r.removeFromLeft(4);
        shape_.setBounds(r.removeFromLeft(86).withHeight(24).withY(r.getY() + 14)); r.removeFromLeft(4);
        sync_.setBounds(r.removeFromLeft(46).withHeight(24).withY(r.getY() + 14)); r.removeFromLeft(4);
        auto rate = r.removeFromLeft(72).withHeight(24).withY(r.getY() + 14);
        div_.setBounds(rate); rate_.setBounds(rate); r.removeFromLeft(4);
        depth_.setBounds(r.removeFromLeft(54)); r.removeFromLeft(4);
        targets_.setBounds(r.removeFromLeft(80).withHeight(24).withY(r.getY() + 14)); r.removeFromLeft(4);
        del_.setBounds(r.removeFromLeft(22).withHeight(24).withY(r.getY() + 14));
    }
    void paint(juce::Graphics& g) override { fillRounded(g, getLocalBounds().reduced(2, 2).toFloat(), col::panel2, 4.0f); }

private:
    void edit(const std::function<void(project::LfoSpec&)>& f) {
        const auto* t = trackOf(model_, track_);
        if (!t || size_t(idx_) >= t->lfos.size()) return;
        auto s = t->lfos[size_t(idx_)];
        f(s);
        model_.apply({"lfo.edit", {{"track", track_}, {"index", idx_}, {"lfo", project::lfoToJson(s)}}});
    }
    void targetsMenu() {
        const auto* t = trackOf(model_, track_);
        if (!t || size_t(idx_) >= t->lfos.size()) return;
        juce::PopupMenu m;
        const auto& l = t->lfos[size_t(idx_)];
        m.addItem(1, "Add target...");
        m.addSeparator();
        for (size_t i = 0; i < l.targets.size(); ++i) m.addItem(int(10 + i), "Remove  " + describeTarget(*t, l.targets[i]));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&targets_), [this](int r) {
            if (r == 1) {
                pickParam(model_, track_, &targets_, [this](const PickedParam& p) { edit([&](project::LfoSpec& s) { s.targets.push_back({p.dest, p.fxId, p.pkey}); }); });
            } else if (r >= 10) {
                edit([&](project::LfoSpec& s) { if (size_t(r - 10) < s.targets.size()) s.targets.erase(s.targets.begin() + (r - 10)); });
            }
        });
    }
    app::AppModel& model_;
    project::Uid track_;
    int idx_;
    Chip on_{"ON", col::play}, shape_{"Sine"}, sync_{"SYNC"}, div_{"1 bar"}, targets_{"no targets"}, del_{"x"};
    NumberBox rate_{0.01, 30.0, 2, " Hz"};
    Knob depth_;
};

// ============================================================ macro row
class ModulationPanel::MacroRow : public juce::Component {
public:
    MacroRow(app::AppModel& m, project::Uid track, int index) : model_(m), track_(track), idx_(index), value_(kMacroSpec, "Value") {
        for (juce::Component* c : std::initializer_list<juce::Component*>{&value_, &targets_, &del_}) addAndMakeVisible(c);
        value_.onBegin = [this] { model_.beginGesture("macro"); };
        value_.onChange = [this](double v) { model_.apply({"macro.value", {{"track", track_}, {"index", idx_}, {"value", v}}}); };
        value_.onEnd = [this] { model_.endGesture(); };
        targets_.onClick = [this] { targetsMenu(); };
        del_.onClick = [this] { model_.apply({"macro.remove", {{"track", track_}, {"index", idx_}}}); };
        const auto& mc = trackOf(m, track)->macros[size_t(index)];
        sync(mc);
    }
    void sync(const project::MacroSpec& mc) {
        name_ = mc.name.empty() ? "Macro " + std::to_string(idx_ + 1) : mc.name;
        value_.setValue(mc.value);
        value_.setStored(true);
        targets_.setText(mc.targets.empty() ? juce::String("no targets") : juce::String(int(mc.targets.size())) + (mc.targets.size() == 1 ? " target" : " targets"));
        repaint();
    }
    void resized() override {
        auto r = getLocalBounds().reduced(4, 6);
        r.removeFromLeft(110);
        value_.setBounds(r.removeFromLeft(54)); r.removeFromLeft(8);
        targets_.setBounds(r.removeFromLeft(80).withHeight(24).withY(r.getY() + 14)); r.removeFromLeft(6);
        del_.setBounds(r.removeFromLeft(22).withHeight(24).withY(r.getY() + 14));
    }
    void paint(juce::Graphics& g) override {
        fillRounded(g, getLocalBounds().reduced(2, 2).toFloat(), col::panel2, 4.0f);
        g.setColour(col::text);
        g.setFont(uiFont(12.5f, true));
        g.drawText(name_, juce::Rectangle<int>(12, 4, 100, getHeight() - 8), juce::Justification::centredLeft, true);
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if (e.x > 110) return;
        promptText("Rename macro", name_, [this](juce::String n) { edit([&](project::MacroSpec& s) { s.name = n.toStdString(); }); });
    }

private:
    void edit(const std::function<void(project::MacroSpec&)>& f) {
        const auto* t = trackOf(model_, track_);
        if (!t || size_t(idx_) >= t->macros.size()) return;
        auto s = t->macros[size_t(idx_)];
        f(s);
        model_.apply({"macro.edit", {{"track", track_}, {"index", idx_}, {"macro", project::macroToJson(s)}}});
    }
    void targetsMenu() {
        const auto* t = trackOf(model_, track_);
        if (!t || size_t(idx_) >= t->macros.size()) return;
        juce::PopupMenu m;
        m.addItem(1, "Add target...");
        m.addSeparator();
        const auto& mc = t->macros[size_t(idx_)];
        for (size_t i = 0; i < mc.targets.size(); ++i) m.addItem(int(10 + i), "Remove  " + describeTarget(*t, mc.targets[i]));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&targets_), [this](int r) {
            if (r == 1) {
                pickParam(model_, track_, &targets_, [this](const PickedParam& p) {
                    edit([&](project::MacroSpec& s) {
                        // the first target: start the knob where the parameter already is, so adding it changes nothing
                        if (s.targets.empty() && p.spec.max > p.spec.min) s.value = std::clamp((p.stored - double(p.spec.min)) / double(p.spec.max - p.spec.min), 0.0, 1.0);
                        s.targets.push_back({p.dest, p.fxId, p.pkey});
                    });
                });
            } else if (r >= 10) {
                edit([&](project::MacroSpec& s) { if (size_t(r - 10) < s.targets.size()) s.targets.erase(s.targets.begin() + (r - 10)); });
            }
        });
    }
    app::AppModel& model_;
    project::Uid track_;
    int idx_;
    std::string name_;
    Knob value_;
    Chip targets_{"no targets"}, del_{"x"};
};

// ============================================================ panel
ModulationPanel::ModulationPanel(app::AppModel& m) : View(m) {
    addAndMakeVisible(addLfo_);
    addAndMakeVisible(addMacro_);
    lanes_ = std::make_unique<LaneEditor>(m);
    addAndMakeVisible(*lanes_);
    addLfo_.onClick = [this] {
        const auto* t = trackOf(model, model.selection().track);
        if (!t) return;
        project::LfoSpec l;
        l.id = "lfo" + std::to_string(t->lfos.size() + 1);
        for (int n = int(t->lfos.size()) + 1;; ++n) { l.id = "lfo" + std::to_string(n); bool used = false; for (auto& e : t->lfos) used |= e.id == l.id; if (!used) break; }
        model.apply({"lfo.insert", {{"track", t->uid}, {"index", t->lfos.size()}, {"lfo", project::lfoToJson(l)}}});
    };
    addMacro_.onClick = [this] {
        const auto* t = trackOf(model, model.selection().track);
        if (!t) return;
        project::MacroSpec mc;
        mc.name = "Macro " + std::to_string(t->macros.size() + 1);
        model.apply({"macro.insert", {{"track", t->uid}, {"index", t->macros.size()}, {"macro", project::macroToJson(mc)}}});
    };
    rebuild();
}
ModulationPanel::~ModulationPanel() = default;

juce::String ModulationPanel::signature() const {
    const auto* t = trackOf(model, model.selection().track);
    if (!t) return "-";
    juce::String s = juce::String(juce::int64(t->uid)) + "|";
    for (auto& l : t->lfos) s << juce::String(l.id) << ":" << l.shape << int(l.sync) << int(l.on) << int(std::lround(l.rate)) << ":" << int(l.targets.size()) << ",";
    s << "|";
    for (auto& mc : t->macros) s << juce::String(mc.name) << ":" << int(mc.targets.size()) << ",";
    return s;
}

void ModulationPanel::rebuild() {
    lfoRows_.clear();
    macroRows_.clear();
    sig_ = signature();
    const auto* t = trackOf(model, model.selection().track);
    track_ = t ? t->uid : 0;
    if (t) {
        for (size_t i = 0; i < t->lfos.size(); ++i) { lfoRows_.push_back(std::make_unique<LfoRow>(model, t->uid, int(i))); addAndMakeVisible(*lfoRows_.back()); }
        for (size_t i = 0; i < t->macros.size(); ++i) { macroRows_.push_back(std::make_unique<MacroRow>(model, t->uid, int(i))); addAndMakeVisible(*macroRows_.back()); }
    }
    addLfo_.setVisible(t != nullptr);
    addMacro_.setVisible(t != nullptr);
    lanes_->setTrack(track_);
    resized();
    repaint();
}

void ModulationPanel::refresh(app::ModelEvent) {
    if (signature() != sig_) { rebuild(); return; }
    const auto* t = trackOf(model, model.selection().track);
    if (t) {
        for (size_t i = 0; i < lfoRows_.size() && i < t->lfos.size(); ++i) lfoRows_[i]->sync(t->lfos[i]);
        for (size_t i = 0; i < macroRows_.size() && i < t->macros.size(); ++i) macroRows_[i]->sync(t->macros[i]);
    }
    lanes_->reload();
}

void ModulationPanel::tick() { if (lanes_->isVisible() && model.meters().playing) lanes_->repaint(); }

void ModulationPanel::resized() {
    auto r = getLocalBounds();
    auto left = r.removeFromLeft(kLeftW);
    auto mid = r.removeFromLeft(kMacroW);
    lanes_->setBounds(r);
    addLfo_.setBounds(left.removeFromTop(kHeaderH).removeFromRight(70).reduced(2, 3));
    addMacro_.setBounds(mid.removeFromTop(kHeaderH).removeFromRight(80).reduced(2, 3));
    int y = left.getY();
    for (auto& row : lfoRows_) { row->setBounds(left.getX(), y, kLeftW, kRowH); y += kRowH; }
    y = mid.getY();
    for (auto& row : macroRows_) { row->setBounds(mid.getX(), y, kMacroW, kRowH); y += kRowH; }
}

void ModulationPanel::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    g.setColour(col::panel);
    g.fillRect(0, 0, getWidth(), kHeaderH);
    g.setColour(col::dim);
    g.setFont(uiFont(11.5f, true));
    const auto* t = trackOf(model, model.selection().track);
    g.drawText(t ? "LFOs on " + juce::String(t->name) : juce::String("Select a track to modulate it"), 10, 0, kLeftW - 90, kHeaderH, juce::Justification::centredLeft);
    g.drawText("Macros", kLeftW + 10, 0, 100, kHeaderH, juce::Justification::centredLeft);
    g.setColour(col::line);
    g.drawVerticalLine(kLeftW, 0, float(getHeight()));
    g.drawVerticalLine(kLeftW + kMacroW, 0, float(getHeight()));
    if (t && t->lfos.empty()) { g.setColour(col::faint); g.setFont(uiFont(12.0f)); g.drawText("No LFOs. Add one, pick what it moves.", 10, kHeaderH + 10, kLeftW - 20, 20, juce::Justification::centredLeft); }
    if (t && t->macros.empty()) { g.setColour(col::faint); g.setFont(uiFont(12.0f)); g.drawText("No macros. A macro is one knob for many parameters.", kLeftW + 10, kHeaderH + 10, kMacroW - 20, 34, juce::Justification::topLeft); }
}

}  // namespace ddaw::ui
