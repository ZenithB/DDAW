#include "app/ui/MixStrip.h"

#include "app/ui/Dialogs.h"

namespace ddaw::ui {

namespace {
juce::String shortName(const std::string& n) { return n.size() > 7 ? juce::String(n).substring(0, 7) : juce::String(n); }
}  // namespace

MixStrip::MixStrip(app::AppModel& m, project::Uid uid, Role role, size_t colourIndex)
    : model_(m), uid_(uid), role_(role), colour_(colourIndex), pan_("Pan", -1.0, 1.0, 0.0, true) {
    addAndMakeVisible(fader_);
    fader_.setColour(role == Role::Master ? col::accent : role == Role::Bus ? col::meterMid : trackColour(colourIndex));
    fader_.onBegin = [this] { model_.beginGesture("fader"); };
    fader_.onChange = [this](double db) {
        if (role_ == Role::Master) model_.apply(document::cmd::setMeta("masterGain", db));
        else model_.apply(document::cmd::setTrack(uid_, "gain", db));
    };
    fader_.onEnd = [this] { model_.endGesture(); };
    if (role == Role::Master) return;
    for (juce::Component* c : std::initializer_list<juce::Component*>{&pan_, &mute_, &solo_, &arm_, &mon_, &stop_, &out_, &more_}) addAndMakeVisible(c);
    pan_.setAccent(role == Role::Bus ? col::meterMid : trackColour(colourIndex));
    pan_.setFormatter([](double v) { return std::abs(v) < 0.005 ? juce::String("C") : juce::String(std::abs(int(std::lround(v * 100)))) + (v < 0 ? "L" : "R"); });
    pan_.onBegin = [this] { model_.beginGesture("pan"); };
    pan_.onChange = [this](double v) { model_.apply(document::cmd::setTrack(uid_, "pan", v)); };
    pan_.onEnd = [this] { model_.endGesture(); };
    mute_.setToggleable(true); solo_.setToggleable(true); arm_.setToggleable(true); mon_.setToggleable(true);
    mute_.onToggle = [this](bool on) { model_.apply(document::cmd::setTrack(uid_, "mute", on)); };
    solo_.onToggle = [this](bool on) { model_.apply(document::cmd::setTrack(uid_, "solo", on)); };
    arm_.onToggle = [this](bool on) { model_.recording().arm(uid_, on); };
    mon_.onToggle = [this](bool on) { model_.setMonitor(uid_, on); };
    stop_.onClick = [this] { if (const auto* t = app::edit::findTrack(model_.project(), uid_)) model_.stopTrack(t->id); };
    out_.onClick = [this] { outMenu(); };
    more_.onClick = [this] { if (onContext) onContext(0); };                     // "+n": the rest are in the routing matrix (the view decides)
}

BarSlider* MixStrip::send(const std::string& busId) {
    for (auto& r : rows_) if (r.busId == busId) return r.slider.get();
    return nullptr;
}

void MixStrip::setCompact(bool c) { if (c != compact_) { compact_ = c; sync(); resized(); repaint(); } }

void MixStrip::rebuildSends(const project::Track& t) {
    std::vector<std::string> ids;
    for (auto* x : app::edit::sendTargets(model_.project(), uid_)) ids.push_back(x->id);
    const bool same = ids == targetIds_;
    targetIds_ = ids;
    if (!same) { rows_.clear(); }
    if (compact_) { rows_.clear(); hidden_ = int(ids.size()); return; }
    // how many rows fit: what is left of the right-hand column under the buttons, above the output chip
    const int top = 24 + 20 + 4 + 22 + (armable_ ? 22 : 0) + 4, bottom = getHeight() - 22 - (role_ == Role::Bus ? 24 : 0);
    int capacity = std::max(0, (bottom - top) / kSendRowH);
    if (int(ids.size()) > capacity && capacity > 0) --capacity;                  // one row's room for the "+n" chip
    capacity = std::min<int>(capacity, int(ids.size()));
    hidden_ = int(ids.size()) - capacity;
    if (rows_.size() != size_t(capacity) || !same) {
        rows_.clear();
        for (int i = 0; i < capacity; ++i) {
            const std::string busId = ids[size_t(i)];
            const auto* bus = app::edit::findTrackById(model_.project(), busId);
            Row r;
            r.busId = busId;
            r.slider = std::make_unique<BarSlider>(shortName(bus ? bus->name : busId), 0.0, 1.0, 0.0);
            r.slider->setAccent(col::meterMid);
            auto* s = r.slider.get();
            s->onBegin = [this] { model_.beginGesture("send"); };
            s->onChange = [this, busId](double v) { model_.apply(document::cmd::setBusSend(uid_, busId, v)); };
            s->onEnd = [this] { model_.endGesture(); };
            addAndMakeVisible(*s);
            rows_.push_back(std::move(r));
        }
        resized();
    }
    for (auto& r : rows_) {
        const auto* bus = app::edit::findTrackById(model_.project(), r.busId);
        r.slider->setLabel(shortName(bus ? bus->name : r.busId));
        const auto it = t.sends.find(r.busId);
        r.slider->setValue(it == t.sends.end() ? 0.0 : it->second);
    }
    more_.setText("+" + juce::String(hidden_));
}

void MixStrip::sync() {
    const auto& p = model_.project();
    if (role_ == Role::Master) {
        fader_.setDb(p.meta.masterGainDb);
        const bool sel = model_.selection().master;
        if (sel != selected_) { selected_ = sel; repaint(); }
        fader_.setVisible(!compact_);
        return;
    }
    const auto* t = app::edit::findTrack(p, uid_);
    if (!t) return;
    name_ = t->name;
    selected_ = !model_.selection().master && model_.selection().track == uid_;
    fader_.setDb(t->gainDb);
    pan_.setValue(t->pan);
    mute_.setOn(t->mute);
    solo_.setOn(t->solo);
    audio_ = t->kind == project::TrackKind::Audio;
    clipTrack_ = role_ == Role::Track;
    armable_ = role_ == Role::Track;
    arm_.setVisible(armable_);
    mon_.setVisible(audio_ && !compact_);
    stop_.setVisible(clipTrack_);
    arm_.setOn(model_.recording().armed(uid_));
    mon_.setOn(model_.monitored(uid_));
    fader_.setVisible(!compact_);
    pan_.setVisible(!compact_);
    out_.setVisible(role_ == Role::Bus && !compact_);
    if (role_ == Role::Bus) out_.setText("Out: " + (t->output.empty() || t->output == "master" ? juce::String("Master") : [&] { const auto* o = app::edit::findTrackById(p, t->output); return juce::String(o ? o->name : t->output); }()));
    rebuildSends(*t);
    more_.setVisible(!compact_ && hidden_ > 0);
    for (auto& r : rows_) r.slider->setVisible(!compact_);
    repaint();
}

void MixStrip::resized() {
    const int W = getWidth(), H = getHeight();
    if (role_ == Role::Master) {
        fader_.setBounds(juce::Rectangle<int>(4, 26, W - 8, H - 26 - 20).withSizeKeepingCentre(60, std::max(20, H - 46)));
        return;
    }
    stop_.setBounds(W - 22, 3, 18, 16);
    if (compact_) {
        auto row = juce::Rectangle<int>(4, 24, W - 8, 20);
        const int n = armable_ ? 3 : 2, w = row.getWidth() / n;
        mute_.setBounds(row.removeFromLeft(w).reduced(1, 0));
        solo_.setBounds(row.removeFromLeft(w).reduced(1, 0));
        if (armable_) arm_.setBounds(row.reduced(1, 0));
        return;
    }
    fader_.setBounds(4, 24, 34, H - 24 - 22);
    auto col = juce::Rectangle<int>(42, 24, W - 46, H - 24);
    pan_.setBounds(col.removeFromTop(16));
    col.removeFromTop(4);
    auto ms = col.removeFromTop(18);
    mute_.setBounds(ms.removeFromLeft(ms.getWidth() / 2).reduced(1, 0));
    solo_.setBounds(ms.reduced(1, 0));
    if (armable_) {
        col.removeFromTop(4);
        auto r2 = col.removeFromTop(18);
        if (audio_) { arm_.setBounds(r2.removeFromLeft(r2.getWidth() / 2).reduced(1, 0)); mon_.setBounds(r2.reduced(1, 0)); }
        else arm_.setBounds(r2.reduced(1, 0));
    }
    col.removeFromTop(4);
    col.removeFromBottom(22);                                                        // the dB readout under the strip
    if (role_ == Role::Bus) out_.setBounds(col.removeFromBottom(20).reduced(0, 0));
    for (auto& r : rows_) r.slider->setBounds(col.removeFromTop(kSendRowH));
    if (hidden_ > 0) more_.setBounds(col.removeFromTop(kSendRowH).reduced(0, 1));
}

void MixStrip::paint(juce::Graphics& g) {
    auto b = getLocalBounds().toFloat().reduced(2, 0);
    fillRounded(g, b, selected_ ? col::raised : col::panel2, 4.0f);
    const auto c = role_ == Role::Master ? col::accent : role_ == Role::Bus ? col::meterMid : trackColour(colour_);
    g.setColour(c);
    g.fillRoundedRectangle(b.removeFromTop(4.0f), 2.0f);
    g.setColour(selected_ ? col::text : col::text.withAlpha(0.9f));
    g.setFont(uiFont(12.5f, true));
    const int nameW = getWidth() - (role_ == Role::Master || !clipTrack_ ? 12 : 34);
    g.drawText(role_ == Role::Master ? juce::String("Master") : name_, juce::Rectangle<int>(6, 5, nameW, 18), juce::Justification::centredLeft, true);
    if (role_ == Role::Master || !compact_) {
        g.setColour(col::black);
        g.fillRoundedRectangle(juce::Rectangle<float>(8.0f, float(getHeight()) - 20.0f, float(getWidth()) - 16.0f, 14.0f), 3.0f);
        g.setColour(col::text);
        g.setFont(monoFont(11.0f));
        g.drawText(juce::String(fader_.db(), 1) + " dB", juce::Rectangle<int>(8, getHeight() - 20, getWidth() - 16, 14), juce::Justification::centred);
    }
    if (clipTrack_) {                                                                // the stop square on the stop chip
        g.setColour(col::dim);
        g.fillRect(juce::Rectangle<float>(7, 7).withCentre(stop_.getBounds().getCentre().toFloat()));
    }
}

void MixStrip::mouseDown(const juce::MouseEvent& e) {
    if (role_ == Role::Master) { model_.selectMaster(); return; }
    if (e.mods.isPopupMenu()) { if (onContext) onContext(uid_); return; }
    model_.selectTrack(uid_);
}

void MixStrip::mouseDoubleClick(const juce::MouseEvent& e) {
    if (role_ == Role::Master || e.y > 24) return;
    promptText("Rename", name_, [this](juce::String n) { if (n.isNotEmpty()) model_.apply(document::cmd::setTrack(uid_, "name", n.toStdString())); });
}

void MixStrip::outMenu() {
    const auto& p = model_.project();
    juce::PopupMenu m;
    m.addSectionHeader("Output");
    m.addItem(1, "Master");
    auto ids = std::make_shared<std::vector<std::string>>();
    int id = 2;
    const auto* self = app::edit::findTrack(p, uid_);
    for (auto* x : app::edit::outputTargets(p, uid_)) {
        m.addItem(id++, juce::String(x->name), true, self && self->output == x->id);
        ids->push_back(x->id);
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&out_), [this, ids](int r) {
        if (r == 1) model_.apply(document::cmd::setTrack(uid_, "output", "master"));
        else if (r >= 2 && size_t(r - 2) < ids->size()) model_.apply(document::cmd::setTrack(uid_, "output", (*ids)[size_t(r - 2)]));
    });
}

}  // namespace ddaw::ui
