#include "app/ui/AudioRatePanel.h"

#include <algorithm>
#include <cmath>

namespace ddaw::ui {

namespace {
const char* const kShapes[] = {"Sine", "Triangle", "Saw", "Square"};
const ParamSpec kDepthSpec{0, "depth", -1.0f, 1.0f, 0.5f, Curve::Linear, 0, false};

// Does anything on the track accept audio-rate modulation?
bool hasAudioRatePorts(const project::Track& t) {
    auto any = [](const project::DeviceSpec& d, app::Chain chain) {
        const auto* info = app::deviceInfoFor(chain, d);
        if (!info) return false;
        for (const auto& ps : info->params) if (ps.audioRate) return true;
        return false;
    };
    if (any(t.inst, app::Chain::Instrument)) return true;
    for (const auto& d : t.fx) if (any(d, app::Chain::Effect)) return true;
    return false;
}
}  // namespace

// ============================================================ row
AudioRatePanel::Row::Row(app::AppModel& m, project::Uid track, int index) : model_(m), uid_(track), idx_(index), depth_(kDepthSpec, "Depth") {
    for (juce::Component* c : std::initializer_list<juce::Component*>{&on_, &source_, &shape_, &track_, &follow_, &rate_, &depth_, &target_, &del_}) addAndMakeVisible(c);
    on_.setToggleable(true); follow_.setToggleable(true);
    on_.onToggle = [this](bool v) { edit([v](project::ARateSpec& s) { s.on = v; }); };
    follow_.onToggle = [this](bool v) { edit([v](project::ARateSpec& s) { s.follow = v; }); };
    source_.onClick = [this] { edit([](project::ARateSpec& s) { s.source = s.source == "osc" ? "track" : "osc"; }); };
    shape_.onClick = [this] { edit([](project::ARateSpec& s) { s.shape = (s.shape + 1) % 4; }); };
    track_.onClick = [this] { trackMenu(); };
    depth_.onBegin = [this] { model_.beginGesture("audio-rate depth"); };
    depth_.onChange = [this](double v) { model_.apply({"arate.field", {{"track", uid_}, {"index", idx_}, {"field", "depth"}, {"value", v}}}); };
    depth_.onEnd = [this] { model_.endGesture(); };
    rate_.onBegin = [this] { model_.beginGesture("audio-rate frequency"); };
    rate_.onChange = [this](double v) { model_.apply({"arate.field", {{"track", uid_}, {"index", idx_}, {"field", "hz"}, {"value", v}}}); };
    rate_.onEnd = [this] { model_.endGesture(); };
    target_.onClick = [this] {
        pickParam(model_, uid_, &target_, [this](const PickedParam& p) { edit([&](project::ARateSpec& s) { s.target = {p.dest, p.fxId, p.pkey}; }); }, true);
    };
    del_.onClick = [this] { model_.apply({"arate.remove", {{"track", uid_}, {"index", idx_}}}); };
    if (const auto* t = trackOf(m, track)) sync(t->arate[size_t(index)]);
}

void AudioRatePanel::Row::sync(const project::ARateSpec& r) {
    const bool osc = r.source != "track";
    on_.setOn(r.on);
    source_.setText(osc ? "Osc" : "Track");
    shape_.setText(kShapes[std::clamp(r.shape, 0, 3)]);
    shape_.setVisible(osc);
    rate_.setVisible(osc);
    rate_.setValue(r.hz);
    track_.setVisible(!osc);
    follow_.setVisible(!osc);
    follow_.setOn(r.follow);
    juce::String srcName = "pick track";
    if (const auto* me = trackOf(model_, uid_))
        for (const auto& t : model_.project().tracks) if (t.id == r.srcTrack && &t != me) srcName = t.name;
    track_.setText(srcName);
    depth_.setValue(r.depth);
    depth_.setStored(true);
    if (const auto* t = trackOf(model_, uid_)) target_.setText(r.target.pkey.empty() ? juce::String("no target") : describeTarget(*t, r.target));
}

void AudioRatePanel::Row::resized() {
    auto r = getLocalBounds().reduced(4, 6);
    const auto slot = [&](int w) { auto s = r.removeFromLeft(w).withHeight(24).withY(r.getY() + 14); r.removeFromLeft(4); return s; };
    on_.setBounds(slot(34));
    source_.setBounds(slot(56));
    const auto mid = slot(184);   // an oscillator's shape + rate, or a track source's name + follow, in the same space
    shape_.setBounds(mid.withWidth(76));
    rate_.setBounds(mid.withX(mid.getX() + 80).withWidth(100));
    track_.setBounds(mid.withWidth(112));
    follow_.setBounds(mid.withX(mid.getX() + 116).withWidth(68));
    depth_.setBounds(r.removeFromLeft(54)); r.removeFromLeft(4);
    target_.setBounds(r.removeFromLeft(150).withHeight(24).withY(r.getY() + 14)); r.removeFromLeft(4);
    del_.setBounds(r.removeFromLeft(22).withHeight(24).withY(r.getY() + 14));
}

void AudioRatePanel::Row::paint(juce::Graphics& g) { fillRounded(g, getLocalBounds().reduced(2, 2).toFloat(), col::panel2, 4.0f); }

void AudioRatePanel::Row::edit(const std::function<void(project::ARateSpec&)>& f) {
    const auto* t = trackOf(model_, uid_);
    if (!t || size_t(idx_) >= t->arate.size()) return;
    auto s = t->arate[size_t(idx_)];
    f(s);
    model_.apply({"arate.edit", {{"track", uid_}, {"index", idx_}, {"arate", project::arateToJson(s)}}});
}

void AudioRatePanel::Row::trackMenu() {
    const auto* me = trackOf(model_, uid_);
    if (!me) return;
    juce::PopupMenu m;
    auto ids = std::make_shared<std::vector<std::string>>();
    for (const auto& t : model_.project().tracks) {
        if (t.uid == uid_ || t.kind == project::TrackKind::Bus) continue;
        m.addItem(int(ids->size()) + 1, juce::String(t.name));
        ids->push_back(t.id);
    }
    if (ids->empty()) m.addItem(-1, "No other tracks", false);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&track_), [this, ids](int r) {
        if (r >= 1 && size_t(r) <= ids->size()) edit([&](project::ARateSpec& s) { s.srcTrack = (*ids)[size_t(r - 1)]; });
    });
}

// ============================================================ panel
AudioRatePanel::AudioRatePanel(app::AppModel& m) : View(m) {
    addAndMakeVisible(add_);
    add_.onClick = [this] { addRoute(); };
    rebuild();
}
AudioRatePanel::~AudioRatePanel() = default;

void AudioRatePanel::addRoute() {
    const auto* t = trackOf(model, model.selection().track);
    if (!t) return;
    project::ARateSpec r;
    for (int n = int(t->arate.size()) + 1;; ++n) { r.id = "ar" + std::to_string(n); bool used = false; for (auto& e : t->arate) used |= e.id == r.id; if (!used) break; }
    model.apply({"arate.insert", {{"track", t->uid}, {"index", t->arate.size()}, {"arate", project::arateToJson(r)}}});
}

juce::String AudioRatePanel::signature() const {
    const auto* t = trackOf(model, model.selection().track);
    if (!t) return "-";
    juce::String s = juce::String(juce::int64(t->uid)) + "|" + juce::String(t->inst.type) + "|";
    for (auto& r : t->arate) s << juce::String(r.id) << ":" << juce::String(r.source) << r.shape << int(r.follow) << int(r.on) << juce::String(r.srcTrack) << juce::String(r.target.pkey) << ",";
    return s;
}

void AudioRatePanel::rebuild() {
    rows_.clear();
    sig_ = signature();
    const auto* t = trackOf(model, model.selection().track);
    track_ = t ? t->uid : 0;
    if (t)
        for (size_t i = 0; i < t->arate.size(); ++i) { rows_.push_back(std::make_unique<Row>(model, t->uid, int(i))); addAndMakeVisible(*rows_.back()); }
    add_.setVisible(t != nullptr);
    resized();
    repaint();
}

void AudioRatePanel::refresh(app::ModelEvent) {
    if (signature() != sig_) { rebuild(); return; }
    if (const auto* t = trackOf(model, model.selection().track))
        for (size_t i = 0; i < rows_.size() && i < t->arate.size(); ++i) rows_[i]->sync(t->arate[i]);
}

void AudioRatePanel::resized() {
    auto r = getLocalBounds();
    add_.setBounds(r.removeFromTop(kHeaderH).removeFromLeft(80).reduced(2, 3).translated(8, 0));
    int y = r.getY();
    for (auto& row : rows_) { row->setBounds(r.getX(), y, std::min(r.getWidth(), 640), kRowH); y += kRowH; }
}

void AudioRatePanel::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    g.setColour(col::panel);
    g.fillRect(0, 0, getWidth(), kHeaderH);
    const auto* t = trackOf(model, model.selection().track);
    g.setColour(col::dim);
    g.setFont(uiFont(11.5f, true));
    g.drawText(t ? "Audio-rate modulation on " + juce::String(t->name) : juce::String("Select a track to modulate it"), 96, 0, getWidth() - 100, kHeaderH, juce::Justification::centredLeft);
    if (!t) return;
    g.setColour(col::faint);
    g.setFont(uiFont(12.0f));
    if (!hasAudioRatePorts(*t))
        g.drawText("This track has no device with audio-rate ports. FM Operators has three: index, pitch and amp.", 12, kHeaderH + 12, getWidth() - 24, 20, juce::Justification::centredLeft);
    else if (t->arate.empty())
        g.drawText("No routes. Add one: an oscillator or another track's audio drives index, pitch or amp per sample.", 12, kHeaderH + 12, getWidth() - 24, 20, juce::Justification::centredLeft);
}

}  // namespace ddaw::ui
