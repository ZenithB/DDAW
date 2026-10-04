#pragma once
#include <memory>

#include "app/model/Timeline.h"
#include "app/ui/ParamPicker.h"
#include "app/ui/View.h"
#include "project/ProjectJson.h"

namespace ddaw::ui {

inline const project::Track* trackOf(app::AppModel& m, project::Uid uid) { return app::edit::findTrack(m.project(), uid); }

// Modulation of the selected track: LFOs, macros, and automation lanes (the track's timeline lanes, or the
// envelopes of the open clip) with a breakpoint editor. Knobs for LFO depth / rate and macro values are live;
// structural edits (shape, sync, targets) go through the document and rebuild the graph.
class ModulationPanel : public View {
public:
    explicit ModulationPanel(app::AppModel& m);
    ~ModulationPanel() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override;
    void tick() override;

    class LfoRow;
    class MacroRow;
    class LaneEditor;
    int lfoRows() const { return int(lfoRows_.size()); }
    int macroRows() const { return int(macroRows_.size()); }
    LaneEditor& lanes() { return *lanes_; }

    static constexpr int kRowH = 62, kLeftW = 440, kMacroW = 330, kHeaderH = 30;

private:
    void rebuild();
    juce::String signature() const;

    Chip addLfo_{"+ LFO", col::accent}, addMacro_{"+ Macro", col::accent};
    std::vector<std::unique_ptr<LfoRow>> lfoRows_;
    std::vector<std::unique_ptr<MacroRow>> macroRows_;
    std::unique_ptr<LaneEditor> lanes_;
    juce::String sig_;
    project::Uid track_ = 0;
};


// ============================================================ lane editor
class ModulationPanel::LaneEditor : public juce::Component {
public:
    explicit LaneEditor(app::AppModel& m) : model_(m) {
        for (juce::Component* c : std::initializer_list<juce::Component*>{&mode_, &lane_, &new_, &del_}) addAndMakeVisible(c);
        mode_.onClick = [this] { clipMode_ = !clipMode_; key_.clear(); reload(); };
        lane_.onClick = [this] { laneMenu(); };
        new_.onClick = [this] { newLane(); };
        del_.onClick = [this] { commit(std::vector<project::AutoPoint>{}, true); };
    }
    void setTrack(project::Uid uid) { if (uid != track_) { track_ = uid; key_.clear(); } reload(); }
    void reload() {
        const auto& lanes = laneMap();
        if (!lanes && !key_.empty()) key_.clear();
        if (lanes && (key_.empty() || !lanes->count(key_))) key_ = lanes && !lanes->empty() ? lanes->begin()->first : std::string();
        points_ = (lanes && !key_.empty() && lanes->count(key_)) ? lanes->at(key_) : std::vector<project::AutoPoint>{};
        const auto* t = trackOf(model_, track_);
        const bool clip = model_.selection().clip.valid();
        mode_.setText(clipMode_ ? "Clip envelope" : "Track lane");
        mode_.setVisible(t != nullptr);
        mode_.setAlpha(clip || clipMode_ ? 1.0f : 0.5f);
        lane_.setText(key_.empty() ? juce::String("no lanes") : describeKey(key_));
        del_.setVisible(!key_.empty());
        repaint();
    }
    bool clipMode() const { return clipMode_; }
    const std::vector<project::AutoPoint>& points() const { return points_; }
    const std::string& key() const { return key_; }
    void setClipMode(bool c) { clipMode_ = c; key_.clear(); reload(); }
    juce::Rectangle<int> canvas() const { return getLocalBounds().withTrimmedTop(32).reduced(6, 4); }
    // canvas <-> data
    double tickAt(int x) const { return scale_.toTick(double(x - canvas().getX())); }
    int xOf(double tick) const { return canvas().getX() + int(std::round(scale_.toX(tick))); }
    int yOf(double v) const { const auto c = canvas(); return c.getBottom() - int(std::round(v * c.getHeight())); }
    double vAt(int y) const { const auto c = canvas(); return std::clamp(double(c.getBottom() - y) / std::max(1, c.getHeight()), 0.0, 1.0); }
    double length() const {
        if (clipMode_) { const auto* c = app::edit::findClip(model_.project(), model_.selection().clip); return c ? c->len : 384.0; }
        double last = 0;
        for (auto& p : points_) last = std::max(last, p.t);
        return std::max(last * 1.1, 4 * 384.0);
    }

    void resized() override {
        auto top = getLocalBounds().removeFromTop(30).reduced(4, 3);
        mode_.setBounds(top.removeFromLeft(104)); top.removeFromLeft(4);
        lane_.setBounds(top.removeFromLeft(190)); top.removeFromLeft(4);
        new_.setBounds(top.removeFromLeft(62)); top.removeFromLeft(4);
        del_.setBounds(top.removeFromLeft(100));
        fit();
    }
    void fit() { const auto c = canvas(); if (c.getWidth() > 20) scale_.pxPerBeat = std::max(2.0, double(c.getWidth()) / (length() / 96.0)); scale_.originTick = 0; }

    void paint(juce::Graphics& g) override {
        fillRounded(g, getLocalBounds().toFloat(), col::panel, 0.0f);
        const auto c = canvas();
        fillRounded(g, c.toFloat(), col::black, 3.0f);
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(c);
        // grid: bars and quarter values
        for (double t = 0; scale_.toX(t) < c.getWidth(); t += 96.0) {
            g.setColour(std::fmod(t, 384.0) < 1e-6 ? col::gridBar : col::grid);
            g.fillRect(xOf(t), c.getY(), 1, c.getHeight());
        }
        for (int q = 1; q < 4; ++q) { g.setColour(col::grid); g.fillRect(c.getX(), yOf(q / 4.0), c.getWidth(), 1); }
        g.setColour(col::faint);
        g.setFont(monoFont(10.0f));
        for (double t = 0; scale_.toX(t) < c.getWidth(); t += 384.0) g.drawText(juce::String(int(t / 384.0) + 1), xOf(t) + 3, c.getY() + 1, 30, 12, juce::Justification::centredLeft);
        if (key_.empty() || key_.empty()) {
            g.setColour(col::dim);
            g.setFont(uiFont(12.5f));
            g.drawText(clipMode_ ? "Open a clip, then add an envelope with + New" : "Add a lane with + New to automate a parameter along the timeline", c, juce::Justification::centred);
            return;
        }
        juce::Path p;
        const double len = length();
        if (points_.empty()) return;
        p.startNewSubPath(float(xOf(0)), float(yOf(points_.front().v)));
        for (auto& pt : points_) p.lineTo(float(xOf(pt.t)), float(yOf(pt.v)));
        p.lineTo(float(xOf(len)), float(yOf(points_.back().v)));
        g.setColour(col::accent.withAlpha(0.9f));
        g.strokePath(p, juce::PathStrokeType(2.0f));
        juce::Path fill(p);
        fill.lineTo(float(xOf(len)), float(c.getBottom()));
        fill.lineTo(float(xOf(0)), float(c.getBottom()));
        fill.closeSubPath();
        g.setColour(col::accent.withAlpha(0.12f));
        g.fillPath(fill);
        for (size_t i = 0; i < points_.size(); ++i) {
            const auto cx = float(xOf(points_[i].t)), cy = float(yOf(points_[i].v));
            g.setColour(int(i) == drag_ ? col::text : col::accent);
            g.fillEllipse(cx - 4.5f, cy - 4.5f, 9.0f, 9.0f);
        }
        if (drag_ >= 0 && size_t(drag_) < points_.size()) {
            g.setColour(col::text);
            g.setFont(monoFont(11.0f));
            g.drawText(readout(points_[size_t(drag_)]), c.withHeight(16).reduced(6, 0), juce::Justification::centredRight);
        }
        // playhead
        const auto m = model_.meters();
        if (m.playing && !clipMode_ && model_.arrangementMode()) { g.setColour(col::accent); g.fillRect(xOf(m.playheadTicks), c.getY(), 1, c.getHeight()); }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (key_.empty() || !canvas().contains(e.getPosition())) return;
        drag_ = hit(e.getPosition());
        if (e.mods.isPopupMenu() || (drag_ >= 0 && e.getNumberOfClicks() > 1)) {
            if (drag_ >= 0) { points_.erase(points_.begin() + drag_); commit(points_, points_.empty()); }
            drag_ = -1;
            return;
        }
        if (drag_ < 0) {   // a new point where the mouse went down
            const double t = std::clamp(snap(tickAt(e.x), e), 0.0, length());
            points_.push_back({t, vAt(e.y)});
            std::sort(points_.begin(), points_.end(), [](const project::AutoPoint& a, const project::AutoPoint& b) { return a.t < b.t; });
            drag_ = int(std::min_element(points_.begin(), points_.end(), [t](const project::AutoPoint& a, const project::AutoPoint& b) { return std::abs(a.t - t) < std::abs(b.t - t); }) - points_.begin());
        }
        moved_ = false;
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (drag_ < 0 || size_t(drag_) >= points_.size()) return;
        moved_ = true;
        double t = std::clamp(snap(tickAt(e.x), e), 0.0, length());
        if (drag_ > 0) t = std::max(t, points_[size_t(drag_ - 1)].t + 1.0);
        if (size_t(drag_) + 1 < points_.size()) t = std::min(t, points_[size_t(drag_ + 1)].t - 1.0);
        points_[size_t(drag_)] = {t, vAt(e.y)};
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (drag_ >= 0) commit(points_, false);
        drag_ = -1;
        repaint();
    }

private:
    double snap(double tick, const juce::MouseEvent& e) const { return e.mods.isShiftDown() ? tick : app::edit::snapTicks(tick, std::max(app::autoGrid(scale_, 10.0), 6.0)); }
    int hit(juce::Point<int> p) const {
        for (size_t i = 0; i < points_.size(); ++i)
            if (std::abs(xOf(points_[i].t) - p.x) <= 7 && std::abs(yOf(points_[i].v) - p.y) <= 7) return int(i);
        return -1;
    }
    const std::map<std::string, std::vector<project::AutoPoint>>* laneMap() const {
        const auto* t = trackOf(model_, track_);
        if (!t) return nullptr;
        if (!clipMode_) return &t->autoLanes;
        const auto* c = app::edit::findClip(model_.project(), model_.selection().clip);
        return c ? &c->env : nullptr;
    }
    nlohmann::json scopeJson() const {
        if (!clipMode_) return {{"track", track_}};
        const auto& r = model_.selection().clip;
        if (!r.arrKey.empty()) return {{"arr", r.arrKey}};
        return {{"track", r.track}, {"scene", r.scene}};
    }
    using json = nlohmann::json;
    juce::String describeKey(const std::string& key) const {
        const auto* t = trackOf(model_, track_);
        const auto a = key.find('|'), b = key.find('|', a + 1);
        if (!t || a == std::string::npos || b == std::string::npos) return key;
        return describeTarget(*t, {key.substr(0, a), key.substr(a + 1, b - a - 1), key.substr(b + 1)});
    }
    void commit(const std::vector<project::AutoPoint>& pts, bool remove) {
        if (key_.empty()) return;
        model_.apply({"env.set", {{"scope", scopeJson()}, {"key", key_}, {"points", remove ? json(nullptr) : project::pointsToJson(pts)}}});
        reload();
    }
    juce::String readout(const project::AutoPoint& p) const {
        const auto* t = trackOf(model_, track_);
        const auto a = key_.find('|'), b = key_.find('|', a + 1);
        juce::String s = juce::String(app::barBeatLabel(p.t)) + "   " + juce::String(p.v, 2);
        ParamSpec spec; double stored;
        if (t && a != std::string::npos && b != std::string::npos && targetSpec(*t, {key_.substr(0, a), key_.substr(a + 1, b - a - 1), key_.substr(b + 1)}, spec, stored)) {
            const double value = (spec.curve == Curve::Exponential && spec.min > 0) ? std::exp(std::log(double(spec.min)) + p.v * (std::log(double(spec.max)) - std::log(double(spec.min)))) : double(spec.min) + p.v * double(spec.max - spec.min);
            s << "   =  " << juce::String(app::formatParam(spec, value));
        }
        return s;
    }
    void laneMenu() {
        const auto* lanes = laneMap();
        if (!lanes || lanes->empty()) return;
        juce::PopupMenu m;
        std::vector<std::string> keys;
        int id = 1;
        for (auto& [k, v] : *lanes) { m.addItem(id++, describeKey(k), true, k == key_); keys.push_back(k); }
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&lane_), [this, keys](int r) { if (r >= 1 && size_t(r) <= keys.size()) { key_ = keys[size_t(r - 1)]; reload(); } });
    }
    void newLane() {
        if (clipMode_ && !model_.selection().clip.valid()) return;
        pickParam(model_, track_, &new_, [this](const PickedParam& p) {
            // a flat lane at the parameter's current position, so creating it changes nothing
            double u = p.spec.max > p.spec.min ? (p.spec.curve == Curve::Exponential && p.spec.min > 0 ? app::paramToUnit(p.spec, p.stored) : (p.stored - double(p.spec.min)) / double(p.spec.max - p.spec.min)) : 0.0;
            u = std::clamp(u, 0.0, 1.0);
            const double len = clipMode_ ? length() : 4 * 384.0;
            const std::string key = p.dest + "|" + p.fxId + "|" + p.pkey;
            key_ = key;
            model_.apply({"env.set", {{"scope", scopeJson()}, {"key", key}, {"points", project::pointsToJson({{0.0, u}, {len, u}})}}});
            reload();
        });
    }

    app::AppModel& model_;
    project::Uid track_ = 0;
    bool clipMode_ = false, moved_ = false;
    std::string key_;
    std::vector<project::AutoPoint> points_;
    int drag_ = -1;
    app::TimeScale scale_{40.0, 0.0};
    Chip mode_{"Track lane"}, lane_{"no lanes"}, new_{"+ New", col::accent}, del_{"Delete lane"};
};


}  // namespace ddaw::ui
