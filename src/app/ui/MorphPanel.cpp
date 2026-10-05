#include "app/ui/MorphPanel.h"

#include <algorithm>
#include <cmath>

#include "app/ui/Dialogs.h"
#include "dsp/MorphMap.h"

namespace ddaw::ui {

namespace {
constexpr int kAnchorHit = 10;

// The blend weights at the stick, for the pad's glow around each anchor.
std::vector<float> weightsAt(const project::MorphSpec& m, double x, double y) {
    std::vector<float> ax, ay, w(m.anchors.size());
    for (auto& a : m.anchors) { ax.push_back(float(a.x)); ay.push_back(float(a.y)); }
    if (!w.empty())
        dsp::morphWeights(m.method == "rbf" ? dsp::MorphMethod::Rbf : dsp::MorphMethod::Idw, float(m.power), float(m.width), ax.data(), ay.data(), int(w.size()), float(x), float(y), w.data());
    return w;
}
}  // namespace

// ============================================================ pad
juce::Point<int> MorphPanel::Pad::toPixel(double x, double y) const {
    const auto f = field();
    return {f.getX() + int(std::lround(x * f.getWidth())), f.getBottom() - int(std::lround(y * f.getHeight()))};
}
juce::Point<double> MorphPanel::Pad::toField(juce::Point<int> p) const {
    const auto f = field();
    return {std::clamp(double(p.x - f.getX()) / std::max(1, f.getWidth()), 0.0, 1.0), std::clamp(double(f.getBottom() - p.y) / std::max(1, f.getHeight()), 0.0, 1.0)};
}

void MorphPanel::Pad::paint(juce::Graphics& g) {
    fillRounded(g, getLocalBounds().toFloat(), col::panel, 4.0f);
    const auto f = field();
    fillRounded(g, f.toFloat(), col::black, 3.0f);
    g.setColour(col::grid);
    for (int q = 1; q < 4; ++q) { g.fillRect(f.getX() + f.getWidth() * q / 4, f.getY(), 1, f.getHeight()); g.fillRect(f.getX(), f.getY() + f.getHeight() * q / 4, f.getWidth(), 1); }
    const auto* m = previewing_ ? &preview_ : owner_.current();
    if (!m) {
        g.setColour(col::dim);
        g.setFont(uiFont(12.5f));
        g.drawText("Add a map, then anchors and targets", f, juce::Justification::centred);
        return;
    }
    const auto w = weightsAt(*m, m->x, m->y);
    for (size_t i = 0; i < m->anchors.size(); ++i) {
        const auto& a = m->anchors[i];
        const auto p = toPixel(a.x, a.y);
        const float glow = i < w.size() ? w[i] : 0.0f;
        g.setColour(col::accent.withAlpha(0.10f + 0.5f * glow));
        g.fillEllipse(float(p.x) - 18.0f * (0.5f + glow), float(p.y) - 18.0f * (0.5f + glow), 36.0f * (0.5f + glow), 36.0f * (0.5f + glow));
        g.setColour(int(i) == owner_.anchor_ ? col::text : col::accent);
        g.fillEllipse(float(p.x) - 6.0f, float(p.y) - 6.0f, 12.0f, 12.0f);
        g.setColour(col::dim);
        g.setFont(uiFont(11.0f));
        g.drawText(juce::String(a.name.empty() ? "A" + std::to_string(i + 1) : a.name), p.x + 8, p.y - 18, 90, 14, juce::Justification::centredLeft);
    }
    const auto s = toPixel(m->x, m->y);
    g.setColour(col::text);
    g.drawEllipse(float(s.x) - 9.0f, float(s.y) - 9.0f, 18.0f, 18.0f, 2.0f);
    g.fillRect(s.x - 1, s.y - 14, 2, 28);
    g.fillRect(s.x - 14, s.y - 1, 28, 2);
}

void MorphPanel::Pad::mouseDown(const juce::MouseEvent& e) {
    const auto* m = owner_.current();
    if (!m || !field().contains(e.getPosition())) return;
    moved_ = false;
    dragAnchor_ = -1;
    for (size_t i = 0; i < m->anchors.size(); ++i) {
        const auto p = toPixel(m->anchors[i].x, m->anchors[i].y);
        if (std::abs(p.x - e.x) <= kAnchorHit && std::abs(p.y - e.y) <= kAnchorHit) { dragAnchor_ = int(i); break; }
    }
    if (dragAnchor_ >= 0) {   // grab an anchor: select it; moving it is previewed and committed on release
        owner_.anchor_ = dragAnchor_;
        preview_ = *m;
        previewing_ = false;
        repaint();
        return;
    }
    dragStick_ = true;        // anywhere else the stick follows the mouse: live, one undo step for the whole drag
    owner_.model.beginGesture("morph");
    const auto f = toField(e.getPosition());
    owner_.model.apply({"morph.pos", {{"track", owner_.track_}, {"index", owner_.sel_}, {"x", f.x}, {"y", f.y}}});
}

void MorphPanel::Pad::mouseDrag(const juce::MouseEvent& e) {
    moved_ = true;
    const auto f = toField(e.getPosition());
    if (dragStick_) {
        owner_.model.apply({"morph.pos", {{"track", owner_.track_}, {"index", owner_.sel_}, {"x", f.x}, {"y", f.y}}});
    } else if (dragAnchor_ >= 0 && size_t(dragAnchor_) < preview_.anchors.size()) {
        previewing_ = true;
        preview_.anchors[size_t(dragAnchor_)].x = f.x;
        preview_.anchors[size_t(dragAnchor_)].y = f.y;
        repaint();
    }
}

void MorphPanel::Pad::mouseUp(const juce::MouseEvent&) {
    if (dragStick_) { owner_.model.endGesture(); dragStick_ = false; }
    if (dragAnchor_ >= 0 && previewing_ && moved_) {
        const double x = preview_.anchors[size_t(dragAnchor_)].x, y = preview_.anchors[size_t(dragAnchor_)].y;
        const int i = dragAnchor_;
        owner_.edit([&](project::MorphSpec& s) { if (size_t(i) < s.anchors.size()) { s.anchors[size_t(i)].x = x; s.anchors[size_t(i)].y = y; } });
    }
    dragAnchor_ = -1;
    previewing_ = false;
    repaint();
}

void MorphPanel::Pad::mouseDoubleClick(const juce::MouseEvent& e) {
    const auto* m = owner_.current();
    if (!m) return;
    for (size_t i = 0; i < m->anchors.size(); ++i) {
        const auto p = toPixel(m->anchors[i].x, m->anchors[i].y);
        if (std::abs(p.x - e.x) <= kAnchorHit && std::abs(p.y - e.y) <= kAnchorHit) {
            const int idx = int(i);
            promptText("Rename anchor", m->anchors[i].name, [this, idx](juce::String n) {
                owner_.edit([&](project::MorphSpec& s) { if (size_t(idx) < s.anchors.size()) s.anchors[size_t(idx)].name = n.toStdString(); });
            });
            return;
        }
    }
}

// ============================================================ panel
MorphPanel::MorphPanel(app::AppModel& m, ControllerInput* controllers) : View(m), controllers_(controllers) {
    for (juce::Component* c : std::initializer_list<juce::Component*>{&pad_, &add_, &on_, &method_, &addAnchor_, &capture_, &delAnchor_, &addTarget_, &spread_, &learnX_, &learnY_}) addAndMakeVisible(c);
    on_.setToggleable(true);
    add_.onClick = [this] { addMap(); };
    on_.onToggle = [this](bool v) { edit([v](project::MorphSpec& s) { s.on = v; }); };
    method_.onClick = [this] { edit([](project::MorphSpec& s) { s.method = s.method == "idw" ? "rbf" : "idw"; }); };
    addAnchor_.onClick = [this] { addAnchorHere(); };
    capture_.onClick = [this] { captureIntoSelected(); };
    delAnchor_.onClick = [this] { removeSelectedAnchor(); };
    for (auto* pair : {&learnX_, &learnY_}) {
        const char axis = pair == &learnX_ ? 'x' : 'y';
        pair->onClick = [this, axis] {
            const auto* t = trackOf(model, track_);
            if (!t || !current()) return;
            const auto target = app::morphTarget(t->id, sel_, axis);
            if (model.learnTarget() == target) model.cancelLearn(); else model.startLearn(target);
        };
    }
    addTarget_.onClick = [this] { pickParam(model, track_, &addTarget_, [this](const PickedParam& p) { addTarget(p); }); };
    spread_.onChange = [this](double v) { edit([v](project::MorphSpec& s) { if (s.method == "rbf") s.width = std::clamp(v / 8.0, 0.05, 1.0); else s.power = v; }); };
    refresh(app::ModelEvent::Document);
}
MorphPanel::~MorphPanel() = default;

const project::MorphSpec* MorphPanel::current() const {
    const auto* t = trackOf(model, model.selection().track);
    return t && sel_ >= 0 && size_t(sel_) < t->morph.size() ? &t->morph[size_t(sel_)] : nullptr;
}

void MorphPanel::edit(const std::function<void(project::MorphSpec&)>& f) {
    const auto* t = trackOf(model, track_);
    if (!t || sel_ < 0 || size_t(sel_) >= t->morph.size()) return;
    auto s = t->morph[size_t(sel_)];
    f(s);
    model.apply({"morph.edit", {{"track", track_}, {"index", sel_}, {"morph", project::morphToJson(s)}}});
}

// Where the targets of `m` sit now (their stored values), as 0..1 of each range.
std::vector<double> MorphPanel::captureValues(const project::Track& t, const project::MorphSpec& m) const {
    std::vector<double> v;
    for (const auto& tg : m.targets) {
        ParamSpec spec; double stored = 0;
        v.push_back(targetSpec(t, tg, spec, stored) ? std::clamp(app::paramToUnit(spec, stored), 0.0, 1.0) : 0.5);
    }
    return v;
}

void MorphPanel::addMap() {
    const auto* t = trackOf(model, model.selection().track);
    if (!t) return;
    project::MorphSpec m;
    m.name = "Morph " + std::to_string(t->morph.size() + 1);
    model.apply({"morph.insert", {{"track", t->uid}, {"index", t->morph.size()}, {"morph", project::morphToJson(m)}}});
    sel_ = int(trackOf(model, t->uid)->morph.size()) - 1;   // the new map is the last one
    anchor_ = -1;
    refresh(app::ModelEvent::Document);
}

void MorphPanel::addAnchorHere() {
    const auto* t = trackOf(model, track_);
    const auto* m = current();
    if (!t || !m) return;
    project::MorphAnchor a;
    a.name = "A" + std::to_string(m->anchors.size() + 1);
    a.x = m->x; a.y = m->y;
    a.values = captureValues(*t, *m);
    edit([&](project::MorphSpec& s) { s.anchors.push_back(a); });
    anchor_ = int(current()->anchors.size()) - 1;
    repaint();
}

void MorphPanel::captureIntoSelected() {
    const auto* t = trackOf(model, track_);
    const auto* m = current();
    if (!t || !m || anchor_ < 0 || size_t(anchor_) >= m->anchors.size()) return;
    const auto v = captureValues(*t, *m);
    edit([&](project::MorphSpec& s) { s.anchors[size_t(anchor_)].values = v; });
}

void MorphPanel::removeSelectedAnchor() {
    const auto* m = current();
    if (!m || anchor_ < 0 || size_t(anchor_) >= m->anchors.size()) return;
    const int i = anchor_;
    edit([i](project::MorphSpec& s) { s.anchors.erase(s.anchors.begin() + i); });
    anchor_ = -1;
}

void MorphPanel::addTarget(const PickedParam& p) {
    const auto* t = trackOf(model, track_);
    const auto* m = current();
    if (!t || !m) return;
    for (const auto& tg : m->targets) if (tg.dest == p.dest && tg.fxId == p.fxId && tg.pkey == p.pkey) return;   // already there
    const double unit = std::clamp(app::paramToUnit(p.spec, p.stored), 0.0, 1.0);
    edit([&](project::MorphSpec& s) {
        s.targets.push_back({p.dest, p.fxId, p.pkey});
        s.curves.resize(s.targets.size(), 1.0);
        for (auto& a : s.anchors) { a.values.resize(s.targets.size() - 1, 0.5); a.values.push_back(unit); }   // existing anchors start where the parameter is now
    });
}

static juce::String sigOf(const project::Track* t, int sel, const std::vector<project::ControlBinding>& bindings, const std::string& learn) {
    if (!t) return "-";
    juce::String s = juce::String(juce::int64(t->uid)) + "|" + juce::String(sel) + "|";
    for (auto& m : t->morph) {
        s << juce::String(m.name) << ":" << int(m.on) << juce::String(m.method) << int(m.anchors.size()) << "/" << int(m.targets.size()) << ",";
        for (auto& tg : m.targets) s << juce::String(tg.pkey) << ";";
    }
    for (auto& b : bindings) s << juce::String(b.source) << ">" << juce::String(b.target) << ";";
    s << "|" << juce::String(learn);
    return s;
}

void MorphPanel::refresh(app::ModelEvent) {
    const auto* t = trackOf(model, model.selection().track);
    if (t && t->uid != track_) { track_ = t->uid; sel_ = 0; anchor_ = -1; }
    if (!t) track_ = 0;
    if (t && sel_ >= int(t->morph.size())) sel_ = std::max(0, int(t->morph.size()) - 1);
    const auto sig = sigOf(t, sel_, model.project().bindings, model.learnTarget());
    if (sig != sig_) { sig_ = sig; rebuildControls(); }
    if (const auto* m = current()) {
        on_.setOn(m->on);
        method_.setText(m->method == "rbf" ? "RBF" : "IDW");
        spread_.setValue(m->method == "rbf" ? m->width * 8.0 : m->power);
        for (size_t i = 0; i < curveBoxes_.size() && i < m->curves.size(); ++i) curveBoxes_[i]->setValue(m->curves[i]);
    }
    if (const auto* t2 = trackOf(model, track_); t2 && current()) {
        learnX_.setOn(model.learnTarget() == app::morphTarget(t2->id, sel_, 'x'));
        learnY_.setOn(model.learnTarget() == app::morphTarget(t2->id, sel_, 'y'));
    }
    pad_.repaint();
}

void MorphPanel::rebuildControls() {
    mapChips_.clear(); targetChips_.clear(); removeChips_.clear(); curveBoxes_.clear(); bindChips_.clear(); bindRemove_.clear();
    const auto* t = trackOf(model, track_);
    if (t) {
        for (size_t i = 0; i < t->morph.size(); ++i) {
            auto c = std::make_unique<Chip>(juce::String(t->morph[i].name.empty() ? "Map " + std::to_string(i + 1) : t->morph[i].name), col::accent);
            c->setToggleable(false);
            c->setOn(int(i) == sel_);
            c->onClick = [this, i] { select(int(i)); };
            addAndMakeVisible(*c);
            mapChips_.push_back(std::move(c));
        }
        if (const auto* m = current())
            for (size_t k = 0; k < m->targets.size(); ++k) {
                auto label = std::make_unique<Chip>(describeTarget(*t, m->targets[k]));
                auto del = std::make_unique<Chip>("x");
                del->onClick = [this, k] { edit([k](project::MorphSpec& s) {
                    if (k >= s.targets.size()) return;
                    s.targets.erase(s.targets.begin() + long(k));
                    if (k < s.curves.size()) s.curves.erase(s.curves.begin() + long(k));
                    for (auto& a : s.anchors) if (k < a.values.size()) a.values.erase(a.values.begin() + long(k));
                }); };
                auto box = std::make_unique<NumberBox>(0.1, 10.0, 2, " exp");   // the response exponent: < 1 lifts the low end, > 1 holds it back
                box->setValue(k < m->curves.size() ? m->curves[k] : 1.0);
                box->onChange = [this, k](double v) { edit([k, v](project::MorphSpec& s) { s.curves.resize(s.targets.size(), 1.0); if (k < s.curves.size()) s.curves[k] = v; }); };
                for (juce::Component* c : std::initializer_list<juce::Component*>{label.get(), del.get(), box.get()}) addAndMakeVisible(c);
                targetChips_.push_back(std::move(label));
                removeChips_.push_back(std::move(del));
                curveBoxes_.push_back(std::move(box));
            }
    }
    if (t && current()) {   // the controllers bound to this map's stick
        const std::string px = app::morphTarget(t->id, sel_, 'x'), py = app::morphTarget(t->id, sel_, 'y');
        const auto& bs = model.project().bindings;
        for (size_t i = 0; i < bs.size(); ++i) {
            if (bs[i].target != px && bs[i].target != py) continue;
            auto label = std::make_unique<Chip>(juce::String(app::describeSource(bs[i].source)) + (bs[i].target == px ? "  ->  X" : "  ->  Y"));
            auto del = std::make_unique<Chip>("x");
            del->onClick = [this, i] { model.apply({"binding.remove", {{"index", i}}}); };
            addAndMakeVisible(*label); addAndMakeVisible(*del);
            bindChips_.push_back(std::move(label));
            bindRemove_.push_back(std::move(del));
        }
    }
    for (juce::Component* c : std::initializer_list<juce::Component*>{&on_, &method_, &addAnchor_, &capture_, &delAnchor_, &addTarget_, &spread_, &learnX_, &learnY_}) c->setVisible(current() != nullptr);
    add_.setVisible(t != nullptr);
    resized();
    repaint();
}

void MorphPanel::resized() {
    auto r = getLocalBounds();
    const int side = std::min(kPadW, std::min(r.getHeight() - 6, r.getWidth() / 2));
    pad_.setBounds(r.removeFromLeft(side + 8).withTrimmedLeft(4).withHeight(side));
    r.removeFromLeft(10);
    auto row = r.removeFromTop(kHeaderH).reduced(0, 3);
    controllerLine_ = row.removeFromRight(300);          // the gamepad's state, right-aligned in the header row
    add_.setBounds(row.removeFromLeft(70)); row.removeFromLeft(6);
    for (auto& c : mapChips_) { c->setBounds(row.removeFromLeft(86)); row.removeFromLeft(4); }
    auto ctl = r.removeFromTop(30).reduced(0, 3);
    on_.setBounds(ctl.removeFromLeft(40)); ctl.removeFromLeft(6);
    method_.setBounds(ctl.removeFromLeft(54)); ctl.removeFromLeft(6);
    spread_.setBounds(ctl.removeFromLeft(54)); ctl.removeFromLeft(14);
    addAnchor_.setBounds(ctl.removeFromLeft(110)); ctl.removeFromLeft(4);
    capture_.setBounds(ctl.removeFromLeft(140)); ctl.removeFromLeft(4);
    delAnchor_.setBounds(ctl.removeFromLeft(100));
    r.removeFromTop(4);
    {   // the target row: + Target, (the curve column label sits above the boxes), then the controllers bound to the stick
        auto line = r.removeFromTop(26);
        addTarget_.setBounds(line.removeFromLeft(100));
        line.removeFromLeft(10);
        learnX_.setBounds(line.removeFromLeft(76)); line.removeFromLeft(4);
        learnY_.setBounds(line.removeFromLeft(76)); line.removeFromLeft(10);
        for (size_t i = 0; i < bindChips_.size(); ++i) {
            bindChips_[i]->setBounds(line.removeFromLeft(166)); line.removeFromLeft(2);
            bindRemove_[i]->setBounds(line.removeFromLeft(22)); line.removeFromLeft(8);
        }
    }
    r.removeFromTop(4);
    for (size_t i = 0; i < targetChips_.size(); ++i) {
        auto line = r.removeFromTop(28);
        targetChips_[i]->setBounds(line.removeFromLeft(220)); line.removeFromLeft(4);
        curveBoxes_[i]->setBounds(line.removeFromLeft(92)); line.removeFromLeft(4);
        removeChips_[i]->setBounds(line.removeFromLeft(24));
        r.removeFromTop(3);
    }
}

void MorphPanel::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    const auto* t = trackOf(model, model.selection().track);
    g.setColour(col::faint);
    g.setFont(uiFont(12.0f));
    const int x = pad_.getRight() + 14;
    if (controllers_) {   // the gamepad's state, top right
        g.setColour(controllers_->padConnected() ? col::play : col::faint);
        g.drawText(controllers_->padConnected() ? "Gamepad: " + juce::String(controllers_->padName()) + (controllers_->padCount() > 1 ? " (+" + juce::String(int(controllers_->padCount()) - 1) + " more)" : juce::String())
                                                : juce::String("No gamepad (MIDI controllers work too)"), controllerLine_, juce::Justification::centredRight);
        g.setColour(col::faint);
    }
    if (!t) { g.setColour(col::dim); g.drawText("Select a track to morph its parameters", x, 8, getWidth() - x, 20, juce::Justification::centredLeft); return; }
    if (t->morph.empty()) g.drawText("No maps. Add one, add targets (the parameters it moves), then anchors: presets placed on the field.", x, kHeaderH + 6, getWidth() - x - 10, 36, juce::Justification::topLeft);
    else if (const auto* m = current()) {
        if (m->targets.empty()) g.drawText("Add the parameters this map should move.", x, kHeaderH + 70, getWidth() - x - 10, 20, juce::Justification::topLeft);
        else if (m->anchors.empty()) g.drawText("Set the knobs, then \"+ Anchor here\" to place a preset at the stick.", x, kHeaderH + 70 + int(m->targets.size()) * 31, getWidth() - x - 10, 20, juce::Justification::topLeft);

    }
}

}  // namespace ddaw::ui
