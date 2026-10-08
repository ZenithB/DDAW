#include "app/ui/MixerView.h"

#include "app/ui/Dialogs.h"

namespace ddaw::ui {

namespace {
const ParamSpec kPanSpec{0, "pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, false};
const ParamSpec kSendSpec{0, "send", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, false};
}  // namespace

class MixerView::Strip : public juce::Component {
public:
    Strip(app::AppModel& m, project::Uid uid, size_t index, bool isMaster)
        : model_(m), uid_(uid), index_(index), master_(isMaster), pan_(kPanSpec, "Pan"), sendA_(kSendSpec, "A"), sendB_(kSendSpec, "B"),
          mute_("M", col::queued), solo_("S", col::accent), arm_("R", col::rec), mon_("IN", col::meterMid) {
        addAndMakeVisible(fader_);
        fader_.setColour(master_ ? col::accent : trackColour(index));   // (a return's strip is recoloured when it learns its role, in sync())
        if (!master_) {
            for (juce::Component* c : std::initializer_list<juce::Component*>{&pan_, &sendA_, &sendB_, &mute_, &solo_, &arm_, &mon_}) addAndMakeVisible(c);
            mon_.setToggleable(true);
            mon_.onToggle = [this](bool on) { model_.setMonitor(uid_, on); };
            arm_.setToggleable(true);
            arm_.onToggle = [this](bool on) { model_.recording().arm(uid_, on); };
            mute_.setToggleable(true);
            solo_.setToggleable(true);
            pan_.setAccent(trackColour(index));
            wire(pan_, "pan");
            wire(sendA_, "sendA");
            wire(sendB_, "sendB");
            mute_.onToggle = [this](bool on) { model_.apply(document::cmd::setTrack(uid_, "mute", on)); };
            solo_.onToggle = [this](bool on) { model_.apply(document::cmd::setTrack(uid_, "solo", on)); };
        }
        fader_.onBegin = [this] { model_.beginGesture("fader"); };
        fader_.onChange = [this](double db) {
            if (master_) model_.apply(document::cmd::setMeta("masterGain", db));
            else model_.apply(document::cmd::setTrack(uid_, "gain", db));
        };
        fader_.onEnd = [this] { model_.endGesture(); };
    }
    void sync() {
        const auto& p = model_.project();
        if (master_) { fader_.setDb(p.meta.masterGainDb); return; }
        const auto* t = app::edit::findTrack(p, uid_);
        if (!t) return;
        ret_ = t->kind == project::TrackKind::Bus && t->send != project::SendBus::None;
        retLetter_ = t->send == project::SendBus::A ? 'A' : t->send == project::SendBus::B ? 'B' : 'F';
        name_ = t->name;
        out_ = t->output.empty() || t->output == "master" ? "Master" : t->output;
        fader_.setDb(t->gainDb);
        fader_.setColour(ret_ ? col::meterMid : trackColour(index_));
        pan_.setValue(t->pan);
        sendA_.setValue(t->sendA);
        sendB_.setValue(t->sendB);
        mute_.setOn(t->mute);
        solo_.setOn(t->solo);
        audio_ = t->kind == project::TrackKind::Audio;
        const bool bus = t->kind == project::TrackKind::Bus;
        sendA_.setVisible(!bus);                 // a bus has no sends of its own
        sendB_.setVisible(!bus);
        sendA_.setAlpha(app::edit::hasReturn(p, project::SendBus::A) ? 1.0f : 0.35f);     // a send with no return behind it does nothing
        sendB_.setAlpha(app::edit::hasReturn(p, project::SendBus::B) ? 1.0f : 0.35f);
        armable_ = t->kind != project::TrackKind::Bus;
        arm_.setVisible(armable_);
        mon_.setVisible(audio_);
        mon_.setOn(model_.monitored(uid_));
        arm_.setOn(model_.recording().armed(uid_));
        if (armable_ != laidOutArmable_ || audio_ != laidOutAudio_) resized();
        selected_ = model_.selection().track == uid_;
        kind_ = ret_ ? std::string("return ") + retLetter_ : t->kind == project::TrackKind::Bus ? "bus" : t->kind == project::TrackKind::Audio ? "audio" : t->kind == project::TrackKind::Drum ? "drum" : t->inst.type;
        repaint();
    }
    void setPeak(float p) { fader_.setPeak(p); }
    bool isReturn() const { return ret_; }
    void resized() override {
        auto r = getLocalBounds().reduced(4, 0);
        r.removeFromTop(34);  // name
        if (!master_) {
            auto knobs = r.removeFromTop(48);
            sendA_.setBounds(knobs.removeFromLeft(40));
            sendB_.setBounds(knobs.removeFromLeft(40));
            auto pan = r.removeFromTop(56);
            pan_.setBounds(pan.withSizeKeepingCentre(52, 56));
            auto ms = r.removeFromTop(26);
            laidOutArmable_ = armable_;
            laidOutAudio_ = audio_;
            if (armable_) {   // a second row: record-arm, and for audio tracks input monitoring
                const int w = ms.getWidth() / 2;
                auto row2 = r.removeFromTop(22);
                mute_.setBounds(ms.removeFromLeft(w).reduced(1, 2));
                solo_.setBounds(ms.reduced(1, 2));
                if (audio_) { arm_.setBounds(row2.removeFromLeft(w).reduced(1, 2)); mon_.setBounds(row2.reduced(1, 2)); }
                else arm_.setBounds(row2.reduced(1, 2));
            } else {
                mute_.setBounds(ms.removeFromLeft(36).reduced(1, 2));
                solo_.setBounds(ms.removeFromRight(36).reduced(1, 2));
            }
        } else r.removeFromTop(128);
        r.removeFromBottom(34);
        fader_.setBounds(r.withSizeKeepingCentre(52, r.getHeight()));
    }
    void paint(juce::Graphics& g) override {
        auto b = getLocalBounds().toFloat().reduced(2, 0);
        fillRounded(g, b, selected_ ? col::raised : col::panel2, 4.0f);
        const auto c = master_ ? col::accent : ret_ ? col::meterMid : trackColour(index_);
        g.setColour(c);
        g.fillRoundedRectangle(b.removeFromTop(4.0f), 2.0f);
        g.setColour(col::text);
        g.setFont(uiFont(12.5f, true));
        g.drawText(master_ ? juce::String("Master") : juce::String(name_), juce::Rectangle<int>(6, 6, getWidth() - 12, 18), juce::Justification::centred, true);
        g.setColour(col::dim);
        g.setFont(uiFont(10.5f));
        if (!master_) g.drawText(kind_, juce::Rectangle<int>(6, 21, getWidth() - 12, 13), juce::Justification::centred, true);
        g.setColour(col::black);
        g.fillRoundedRectangle(juce::Rectangle<float>(8.0f, float(getHeight()) - 30, float(getWidth()) - 16, 14.0f), 3.0f);
        g.setColour(col::text);
        g.setFont(monoFont(11.0f));
        g.drawText(juce::String(fader_.db(), 1) + " dB", juce::Rectangle<int>(8, getHeight() - 30, getWidth() - 16, 14), juce::Justification::centred);
        g.setColour(col::dim);
        g.setFont(uiFont(10.5f));
        g.drawText(master_ ? juce::String("") : juce::String("> ") + juce::String(out_), juce::Rectangle<int>(6, getHeight() - 15, getWidth() - 12, 13), juce::Justification::centred, true);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (master_) return;
        if (e.mods.isPopupMenu() || e.y > 24) { if (!e.mods.isPopupMenu()) model_.selectTrack(uid_); }
        else model_.selectTrack(uid_);
        if (e.mods.isPopupMenu()) routeMenu();
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if (master_ || e.y > 24) return;
        promptText("Rename track", name_, [this](juce::String n) { if (n.isNotEmpty()) model_.apply(document::cmd::setTrack(uid_, "name", n.toStdString())); });
    }

private:
    void routeMenu() {
        const auto& p = model_.project();
        juce::PopupMenu m;
        m.addSectionHeader("Output");
        m.addItem(1, "Master");
        int id = 2;
        std::vector<std::string> ids;
        for (auto& t : p.tracks) if (t.kind == project::TrackKind::Bus && t.uid != uid_) { m.addItem(id++, juce::String(t.name)); ids.push_back(t.id); }
        m.showMenuAsync({}, [this, ids](int r) {
            if (r == 1) model_.apply(document::cmd::setTrack(uid_, "output", "master"));
            else if (r >= 2 && size_t(r - 2) < ids.size()) model_.apply(document::cmd::setTrack(uid_, "output", ids[size_t(r - 2)]));
        });
    }
    void wire(Knob& k, const char* field) {
        k.onBegin = [this] { model_.beginGesture("track"); };
        k.onChange = [this, field](double v) { model_.apply(document::cmd::setTrack(uid_, field, v)); };
        k.onEnd = [this] { model_.endGesture(); };
    }
    app::AppModel& model_;
    project::Uid uid_;
    size_t index_;
    bool master_, selected_ = false, audio_ = false, armable_ = false, laidOutAudio_ = false, laidOutArmable_ = false, ret_ = false;
    char retLetter_ = 'A';
    std::string name_, out_, kind_;
    Fader fader_;
    Knob pan_, sendA_, sendB_;
    Chip mute_, solo_, arm_, mon_;
};

// A built-in return channel of an imported project (project.returns: sends A and B when there are no send buses): its name, its
// effect, and its level.
class MixerView::LegacyStrip : public juce::Component {
public:
    LegacyStrip(app::AppModel& m, size_t index) : model_(m), index_(index), remove_("x") {
        addAndMakeVisible(fader_);
        addAndMakeVisible(remove_);
        fader_.setColour(col::meterMid);
        remove_.onClick = [this] { model_.apply({"return.remove", {{"index", index_}}}); };
        fader_.onBegin = [this] { model_.beginGesture("return level"); };
        fader_.onChange = [this](double db) { edit(db); };
        fader_.onEnd = [this] { model_.endGesture(); };
    }
    void sync() {
        const auto& rs = model_.project().returns;
        if (index_ >= rs.size()) return;
        name_ = rs[index_].name.empty() ? std::string("Return ") + char('A' + int(index_)) : rs[index_].name;
        const auto* info = app::findDevice(app::Chain::Effect, rs[index_].fxType);
        fx_ = info ? info->label : rs[index_].fxType;
        fader_.setDb(rs[index_].gainDb);
        repaint();
    }
    void resized() override {
        auto r = getLocalBounds().reduced(4, 0);
        remove_.setBounds(r.removeFromTop(22).removeFromRight(22).reduced(1));
        r.removeFromTop(12 + 128);
        r.removeFromBottom(34);
        fader_.setBounds(r.withSizeKeepingCentre(52, r.getHeight()));
    }
    void paint(juce::Graphics& g) override {
        auto b = getLocalBounds().toFloat().reduced(2, 0);
        fillRounded(g, b, col::panel2, 4.0f);
        g.setColour(col::meterMid);
        g.fillRoundedRectangle(b.removeFromTop(4.0f), 2.0f);
        g.setColour(col::text);
        g.setFont(uiFont(12.5f, true));
        g.drawText(juce::String(name_), juce::Rectangle<int>(6, 6, getWidth() - 34, 18), juce::Justification::centredLeft, true);
        g.setColour(col::dim);
        g.setFont(uiFont(10.5f));
        g.drawText(juce::String(fx_), juce::Rectangle<int>(6, 25, getWidth() - 12, 13), juce::Justification::centredLeft, true);
        g.drawText("built-in return", juce::Rectangle<int>(6, 40, getWidth() - 12, 13), juce::Justification::centredLeft, true);
        g.setColour(col::black);
        g.fillRoundedRectangle(juce::Rectangle<float>(8.0f, float(getHeight()) - 30, float(getWidth()) - 16, 14.0f), 3.0f);
        g.setColour(col::text);
        g.setFont(monoFont(11.0f));
        g.drawText(juce::String(fader_.db(), 1) + " dB", juce::Rectangle<int>(8, getHeight() - 30, getWidth() - 16, 14), juce::Justification::centred);
    }

private:
    void edit(double db) {
        const auto& rs = model_.project().returns;
        if (index_ >= rs.size()) return;
        const auto& r = rs[index_];
        nlohmann::json params = nlohmann::json::object();
        for (auto& [k, v] : r.params) params[k] = v;
        model_.apply({"return.edit", {{"index", index_}, {"ret", {{"id", r.id}, {"name", r.name}, {"fxType", r.fxType}, {"params", params}, {"gain", db}}}}});
    }
    app::AppModel& model_;
    size_t index_;
    std::string name_, fx_;
    Fader fader_;
    Chip remove_;
};

MixerView::MixerView(app::AppModel& m) : View(m) {
    addAndMakeVisible(addReturn_);
    addReturn_.onClick = [this] { addReturn(); };
    rebuild();
}
MixerView::~MixerView() = default;

int MixerView::returnCount() const {
    int n = 0;
    for (auto& s : strips_) n += s->isReturn();
    return n;   // (what the strips themselves say; the layout uses the count taken when they were built)
}

void MixerView::addReturn() {
    const auto& p = model.project();
    for (auto which : {project::SendBus::A, project::SendBus::B})
        if (!app::edit::hasReturn(p, which)) { model.apply(app::edit::addReturnBus(p, which)); return; }
}

void MixerView::rebuild() {
    const auto& p = model.project();
    strips_.clear();
    docIdx_.clear();
    legacy_.clear();
    key_.clear();
    // tracks and ordinary buses in document order, then the send returns A, B, F
    auto isReturn = [](const project::Track& t) { return t.kind == project::TrackKind::Bus && t.send != project::SendBus::None; };
    auto add = [&](size_t i) {
        key_.push_back({p.tracks[i].uid, int(p.tracks[i].send)});
        docIdx_.push_back(i);
        strips_.push_back(std::make_unique<Strip>(model, p.tracks[i].uid, i, false));
        addAndMakeVisible(*strips_.back());
    };
    for (size_t i = 0; i < p.tracks.size(); ++i) if (!isReturn(p.tracks[i])) add(i);
    nReturns_ = int(std::count_if(p.tracks.begin(), p.tracks.end(), isReturn));
    for (auto which : {project::SendBus::A, project::SendBus::B, project::SendBus::F})
        for (size_t i = 0; i < p.tracks.size(); ++i) if (isReturn(p.tracks[i]) && p.tracks[i].send == which) add(i);
    if (p.tracks.empty() || !std::any_of(p.tracks.begin(), p.tracks.end(), isReturn))   // the built-in returns only matter while there are no send buses
        for (size_t i = 0; i < p.returns.size(); ++i) { legacy_.push_back(std::make_unique<LegacyStrip>(model, i)); addAndMakeVisible(*legacy_.back()); }
    master_ = std::make_unique<Strip>(model, 0, 0, true);
    addAndMakeVisible(*master_);
    resized();
}

void MixerView::refresh(app::ModelEvent) {
    const auto& p = model.project();
    std::vector<std::pair<project::Uid, int>> now;
    auto isReturn = [](const project::Track& t) { return t.kind == project::TrackKind::Bus && t.send != project::SendBus::None; };
    for (auto& t : p.tracks) if (!isReturn(t)) now.push_back({t.uid, int(t.send)});
    for (auto which : {project::SendBus::A, project::SendBus::B, project::SendBus::F})
        for (auto& t : p.tracks) if (isReturn(t) && t.send == which) now.push_back({t.uid, int(t.send)});
    const bool legacyNow = !std::any_of(p.tracks.begin(), p.tracks.end(), isReturn);
    if (now != key_ || (legacyNow ? p.returns.size() : size_t(0)) != legacy_.size()) rebuild();
    for (auto& s : strips_) s->sync();
    for (auto& s : legacy_) s->sync();
    if (master_) master_->sync();
    addReturn_.setVisible(!app::edit::hasReturn(p, project::SendBus::A) || !app::edit::hasReturn(p, project::SendBus::B));
    addReturn_.setText(!app::edit::hasReturn(p, project::SendBus::A) ? "+ Return A" : "+ Return B");
}

void MixerView::resized() {
    const int h = getHeight() - kHeaderH;
    int x = -scrollX_;
    const size_t firstReturn = strips_.size() - size_t(nReturns_);   // the returns are the last strips
    for (size_t i = 0; i < strips_.size(); ++i) {
        if (i == firstReturn) { x += 12; returnsX_ = x; }
        strips_[i]->setBounds(x, kHeaderH, kStripW, h);
        x += kStripW;
    }
    if (firstReturn == strips_.size()) { x += 12; returnsX_ = x; }
    for (auto& s : legacy_) { s->setBounds(x, kHeaderH, kStripW, h); x += kStripW; }
    if (master_) master_->setBounds(getWidth() - kStripW - 4, kHeaderH, kStripW, h);
    addReturn_.setBounds(returnsX_ + 62, 2, 84, kHeaderH - 4);
}

void MixerView::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    g.setColour(col::panel);
    g.fillRect(0, 0, getWidth(), kHeaderH);
    g.setColour(col::line);
    g.drawHorizontalLine(kHeaderH - 1, 0, float(getWidth()));
    g.setColour(col::dim);
    g.setFont(uiFont(10.5f, true));
    g.drawText("TRACKS", juce::Rectangle<int>(-scrollX_ + 8, 0, 80, kHeaderH), juce::Justification::centredLeft);
    g.drawText("RETURNS", juce::Rectangle<int>(returnsX_ + 4, 0, 60, kHeaderH), juce::Justification::centredLeft);
    g.setColour(col::line);
    g.drawVerticalLine(returnsX_ - 7, float(kHeaderH), float(getHeight()));
    if (strips_.empty()) {
        g.setColour(col::dim);
        g.setFont(uiFont(14.0f));
        g.drawText("No tracks yet", getLocalBounds().withTrimmedTop(kHeaderH), juce::Justification::centred);
    }
    g.setColour(col::line);
    g.drawVerticalLine(getWidth() - kStripW - 8, float(kHeaderH), float(getHeight()));
}

void MixerView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    const int content = int(strips_.size() + legacy_.size()) * kStripW + 24;
    const int maxScroll = std::max(0, content - (getWidth() - kStripW - 12));
    scrollX_ = juce::jlimit(0, maxScroll, scrollX_ - int(w.deltaX * 500.0f) - int(w.deltaY * 200.0f));
    resized();
    repaint();
}

void MixerView::tick() {
    const auto m = model.meters();
    for (size_t i = 0; i < strips_.size(); ++i) strips_[i]->setPeak(m.trackPeak[docIdx_[i]]);
    if (master_) master_->setPeak(m.masterPeak);
}

}  // namespace ddaw::ui
