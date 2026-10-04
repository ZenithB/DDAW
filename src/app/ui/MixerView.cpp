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
        fader_.setColour(master_ ? col::accent : trackColour(index));
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
        name_ = t->name;
        out_ = t->output.empty() || t->output == "master" ? "Master" : t->output;
        fader_.setDb(t->gainDb);
        pan_.setValue(t->pan);
        sendA_.setValue(t->sendA);
        sendB_.setValue(t->sendB);
        mute_.setOn(t->mute);
        solo_.setOn(t->solo);
        audio_ = t->kind == project::TrackKind::Audio;
        armable_ = t->kind != project::TrackKind::Bus;
        arm_.setVisible(armable_);
        mon_.setVisible(audio_);
        mon_.setOn(model_.monitored(uid_));
        arm_.setOn(model_.recording().armed(uid_));
        if (armable_ != laidOutArmable_ || audio_ != laidOutAudio_) resized();
        selected_ = model_.selection().track == uid_;
        kind_ = t->kind == project::TrackKind::Bus ? "bus" : t->kind == project::TrackKind::Audio ? "audio" : t->kind == project::TrackKind::Drum ? "drum" : t->inst.type;
        repaint();
    }
    void setPeak(float p) { fader_.setPeak(p); }
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
        const auto c = master_ ? col::accent : trackColour(index_);
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
    bool master_, selected_ = false, audio_ = false, armable_ = false, laidOutAudio_ = false, laidOutArmable_ = false;
    std::string name_, out_, kind_;
    Fader fader_;
    Knob pan_, sendA_, sendB_;
    Chip mute_, solo_, arm_, mon_;
};

MixerView::MixerView(app::AppModel& m) : View(m) { rebuild(); }
MixerView::~MixerView() = default;

void MixerView::rebuild() {
    const auto& p = model.project();
    strips_.clear();
    uids_.clear();
    for (size_t i = 0; i < p.tracks.size(); ++i) {
        uids_.push_back(p.tracks[i].uid);
        strips_.push_back(std::make_unique<Strip>(model, p.tracks[i].uid, i, false));
        addAndMakeVisible(*strips_.back());
    }
    master_ = std::make_unique<Strip>(model, 0, 0, true);
    addAndMakeVisible(*master_);
    resized();
}

void MixerView::refresh(app::ModelEvent) {
    const auto& p = model.project();
    std::vector<project::Uid> now;
    for (auto& t : p.tracks) now.push_back(t.uid);
    if (now != uids_) rebuild();
    for (auto& s : strips_) s->sync();
    if (master_) master_->sync();
}

void MixerView::resized() {
    const int h = getHeight();
    int x = -scrollX_;
    for (auto& s : strips_) { s->setBounds(x, 0, kStripW, h); x += kStripW; }
    if (master_) master_->setBounds(getWidth() - kStripW - 4, 0, kStripW, h);
}

void MixerView::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    if (strips_.empty()) {
        g.setColour(col::dim);
        g.setFont(uiFont(14.0f));
        g.drawText("No tracks yet", getLocalBounds(), juce::Justification::centred);
    }
    g.setColour(col::line);
    g.drawVerticalLine(getWidth() - kStripW - 8, 0, float(getHeight()));
}

void MixerView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    const int maxScroll = std::max(0, int(strips_.size()) * kStripW - (getWidth() - kStripW - 12));
    scrollX_ = juce::jlimit(0, maxScroll, scrollX_ - int(w.deltaX * 500.0f) - int(w.deltaY * 200.0f));
    resized();
}

void MixerView::tick() {
    const auto m = model.meters();
    for (size_t i = 0; i < strips_.size(); ++i) strips_[i]->setPeak(m.trackPeak[i]);
    if (master_) master_->setPeak(m.masterPeak);
}

}  // namespace ddaw::ui
