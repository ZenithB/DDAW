#include "app/ui/DeviceChainView.h"

#include "project/ProjectJson.h"

namespace ddaw::ui {

namespace {
constexpr int kKnobW = 58, kKnobH = 80, kPanelHeader = 26, kPad = 8, kTopH = 30;
}

// One device: header (name, on, move, remove), knobs for every parameter, and the special controls the
// sample-based instruments and the ducker need.
class DeviceChainView::Panel : public juce::Component {
public:
    Panel(app::AppModel& m, const project::DeviceSpec& d, app::Chain chain, bool master, size_t slot, size_t slots, int availH)
        : model_(m), uid_(d.uid), chain_(chain), master_(master), slot_(slot), slots_(slots), type_(d.type),
          power_("", col::play), left_("<"), right_(">"), close_("x") {
        info_ = app::findDevice(chain, d.type);
        label_ = info_ ? info_->label : d.type;
        const int rows = std::max(1, (availH - kPanelHeader - kPad - (special() ? 26 : 0)) / kKnobH);
        const size_t n = info_ ? info_->params.size() : 0;
        const int cols = n ? int((n + size_t(rows) - 1) / size_t(rows)) : 0;
        width_ = std::max(150, cols * kKnobW + 2 * kPad);
        if (chain != app::Chain::Instrument) { for (juce::Component* c : std::initializer_list<juce::Component*>{&power_, &left_, &right_, &close_}) addAndMakeVisible(c); }
        power_.setToggleable(true);
        power_.onToggle = [this](bool on) { model_.apply({"device.set", {{"uid", uid_}, {"field", "on"}, {"value", on}}}); };
        close_.onClick = [this] { model_.apply(app::edit::removeDevice(uid_)); };
        left_.onClick = [this] { move(-1); };
        right_.onClick = [this] { move(+1); };
        if (info_) {
            for (size_t i = 0; i < info_->params.size(); ++i) {
                const auto& spec = info_->params[i];
                auto k = std::make_unique<Knob>(spec, app::paramLabel(spec.key), spec.audioRate ? col::meterMid : accent());   // audio-rate ports stand out
                const int r = int(i) % rows, c = int(i) / rows;
                k->setBounds(kPad + c * kKnobW, kPanelHeader + 4 + r * kKnobH, kKnobW, kKnobH);
                const std::string key = spec.key;
                k->onBegin = [this] { model_.beginGesture("parameter"); };
                k->onChange = [this, key](double v) { model_.apply(document::cmd::setParam(uid_, key, v)); };
                k->onEnd = [this] { model_.endGesture(); };
                addAndMakeVisible(*k);
                knobs_.push_back(std::move(k));
            }
        }
        if (special()) {
            extra_ = std::make_unique<Chip>(type_ == "duck" ? "Source: ..." : type_ == "drum" ? "Pad samples..." : "Load sample...");
            extra_->onClick = [this] { type_ == "duck" ? pickSource() : type_ == "drum" ? pickPadSamples() : pickSample(); };
            addAndMakeVisible(*extra_);
        }
        sync(d);
    }
    int width() const { return width_; }
    project::Uid uid() const { return uid_; }
    void sync(const project::DeviceSpec& d) {
        if (info_)
            for (size_t i = 0; i < knobs_.size(); ++i) {
                auto it = d.params.find(info_->params[i].key);
                knobs_[i]->setStored(it != d.params.end());
                knobs_[i]->setValue(it != d.params.end() ? it->second : double(info_->params[i].def));
            }
        on_ = d.on;
        power_.setOn(d.on);
        if (extra_) {
            if (type_ == "duck") {
                const auto* t = app::edit::findTrackById(model_.project(), d.srcTrack);
                extra_->setText(juce::String("Source: ") + (t ? juce::String(t->name) : juce::String("none")));
            } else if (type_ == "drum") {
                extra_->setText(d.padSamples.empty() ? juce::String("Pad samples...") : juce::String(int(d.padSamples.size())) + " pad sample(s)");
            } else extra_->setText(d.sampleId.empty() ? juce::String("Load sample...") : juce::String(d.sampleName.empty() ? d.sampleId : d.sampleName));
        }
        selected_ = model_.selection().device == uid_;
        repaint();
    }
    void resized() override {
        auto h = juce::Rectangle<int>(0, 0, getWidth(), kPanelHeader).reduced(4, 3);
        close_.setBounds(h.removeFromRight(20));
        h.removeFromRight(2);
        right_.setBounds(h.removeFromRight(20));
        left_.setBounds(h.removeFromRight(20));
        power_.setBounds(h.removeFromLeft(20));
        if (extra_) extra_->setBounds(kPad, getHeight() - 26, getWidth() - 2 * kPad, 20);
    }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat().reduced(1.0f);
        fillRounded(g, r, col::panel2, 5.0f);
        g.setColour(selected_ ? col::accent : col::line);
        g.drawRoundedRectangle(r, 5.0f, selected_ ? 1.5f : 1.0f);
        g.setColour(accent());
        g.fillRoundedRectangle(r.withHeight(3.0f).reduced(8, 0), 1.5f);
        g.setColour(on_ ? col::text : col::faint);
        g.setFont(uiFont(12.5f, true));
        const int off = chain_ == app::Chain::Instrument ? 10 : 28;
        g.drawText(label_, juce::Rectangle<int>(off, 3, getWidth() - off - 70, kPanelHeader - 3), juce::Justification::centredLeft, true);
        if (!info_ || info_->params.empty()) {
            g.setColour(col::faint);
            g.setFont(uiFont(11.5f));
            g.drawText(info_ ? "no parameters" : "unknown device", juce::Rectangle<int>(0, kPanelHeader, getWidth(), getHeight() - kPanelHeader - 28), juce::Justification::centred);
        }
        if (!on_) { g.setColour(col::black.withAlpha(0.35f)); g.fillRoundedRectangle(r, 5.0f); }
    }
    void mouseDown(const juce::MouseEvent&) override { model_.selectDevice(uid_); }

private:
    bool special() const { return type_ == "duck" || type_ == "drum" || type_ == "sampler" || type_ == "ksampler" || type_ == "granular"; }
    juce::Colour accent() const { return chain_ == app::Chain::Instrument ? col::accent : chain_ == app::Chain::MidiFx ? col::meterMid : juce::Colour(0xff5aa9e6); }
    void move(int delta) {
        const auto& p = model_.project();
        const auto* d = model_.document().findDevice(uid_);
        if (!d) return;
        const int to = int(slot_) + delta;
        if (to < 0 || to >= int(slots_)) return;
        const project::Track* tr = nullptr;
        for (auto& t : p.tracks) {
            for (auto& x : t.fx) if (x.uid == uid_) tr = &t;
            for (auto& x : t.midifx) if (x.uid == uid_) tr = &t;
        }
        document::Command ins{"device.insert", {{"chain", master_ ? "master" : chain_ == app::Chain::MidiFx ? "midifx" : "fx"}, {"index", to}, {"device", project::deviceToJson(*d)}}};
        if (tr) ins.args["track"] = tr->uid;
        model_.applyGroup("move device", {app::edit::removeDevice(uid_), ins});
    }
    void pickSource() {
        juce::PopupMenu m;
        const auto& p = model_.project();
        int id = 1;
        for (auto& t : p.tracks) m.addItem(id++, juce::String(t.name));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(extra_.get()), [this](int r) {
            const auto& pr = model_.project();
            if (r >= 1 && size_t(r) <= pr.tracks.size()) model_.apply({"device.set", {{"uid", uid_}, {"field", "srcTrack"}, {"value", pr.tracks[size_t(r - 1)].id}}});
        });
    }
    // Replace this instrument with a copy edited by `f` (the instrument is replaced whole: inst.set).
    void editInstrument(const std::function<void(project::DeviceSpec&)>& f) {
        const auto* d = model_.document().findDevice(uid_);
        if (!d) return;
        for (auto& t : model_.project().tracks)
            if (t.inst.uid == uid_) {
                auto n = *d;
                f(n);
                model_.apply({"inst.set", {{"track", t.uid}, {"device", project::deviceToJson(n, true)}}});
                return;
            }
    }
    // The bank's samples as menu items starting at `base`, then "Browse...". `done` gets the id ("" = none).
    void sampleMenu(juce::PopupMenu& m, int base, bool withNone, const std::string& current) {
        if (withNone) m.addItem(base, "Synthesized (no sample)", true, current.empty());
        int id = base + 1;
        for (const auto& sid : model_.sampleIds()) m.addItem(id++, juce::String(model_.sampleName(sid)), true, sid == current);
        m.addSeparator();
        m.addItem(base + 900, "Browse for a file...");
    }
    std::string sampleFromChoice(int choice, int base, bool withNone) {
        if (withNone && choice == base) return "";
        const auto ids = model_.sampleIds();
        const int i = choice - base - 1;
        return i >= 0 && size_t(i) < ids.size() ? ids[size_t(i)] : std::string();
    }
    void browse(std::function<void(const std::string&)> done) {
        chooser_ = std::make_unique<juce::FileChooser>("Load a sample", juce::File(), "*.wav");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, done = std::move(done)](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (!f.existsAsFile()) return;
            std::string id, err;
            if (model_.loadWavSample(f.getFullPathName().toStdString(), id, err)) done(id);
            else model_.notify(app::ModelEvent::File);
        });
    }
    void pickSample() {
        const auto* d = model_.document().findDevice(uid_);
        juce::PopupMenu m;
        sampleMenu(m, 1, false, d ? d->sampleId : std::string());
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(extra_.get()), [this](int r) {
            auto set = [this](const std::string& id) {
                editInstrument([&](project::DeviceSpec& n) { n.sampleId = id; n.sampleName = model_.sampleName(id); });
            };
            if (r == 1 + 900) browse(set);
            else if (const auto id = sampleFromChoice(r, 1, false); !id.empty()) set(id);
        });
    }
    void pickPadSamples() {
        const auto* d = model_.document().findDevice(uid_);
        juce::PopupMenu m;
        for (int pad = 0; pad < 8; ++pad) {
            juce::PopupMenu sub;
            const auto it = d ? d->padSamples.find(std::to_string(pad)) : decltype(d->padSamples.end()){};
            const std::string cur = d && it != d->padSamples.end() ? it->second : std::string();
            sampleMenu(sub, 100 * (pad + 1), true, cur);
            m.addSubMenu(juce::String(app::drumPadName(pad)) + (cur.empty() ? "" : "  *"), sub);
        }
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(extra_.get()), [this](int r) {
            if (r < 100) return;
            const int pad = r / 100 - 1, local = r % 100 == 0 ? 0 : r - 100 * (pad + 1);
            auto set = [this, pad](const std::string& id) {
                editInstrument([&](project::DeviceSpec& n) {
                    if (id.empty()) { n.padSamples.erase(std::to_string(pad)); n.padNames.erase(std::to_string(pad)); }
                    else { n.padSamples[std::to_string(pad)] = id; n.padNames[std::to_string(pad)] = model_.sampleName(id); }
                });
            };
            if (local == 900) browse(set);
            else set(sampleFromChoice(r, 100 * (pad + 1), true));
        });
    }

    app::AppModel& model_;
    project::Uid uid_;
    app::Chain chain_;
    bool master_, on_ = true, selected_ = false;
    size_t slot_, slots_;
    std::string type_;
    const app::DeviceInfo* info_ = nullptr;
    juce::String label_;
    int width_ = 150;
    Chip power_, left_, right_, close_;
    std::vector<std::unique_ptr<Knob>> knobs_;
    std::unique_ptr<Chip> extra_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

DeviceChainView::DeviceChainView(app::AppModel& m) : View(m) {
    addAndMakeVisible(addFx_);
    addAndMakeVisible(addMidi_);
    addAndMakeVisible(master_);
    master_.setToggleable(true);
    master_.onToggle = [this](bool) { rebuild(); };
    addFx_.onClick = [this] {
        juce::PopupMenu menu;
        std::vector<std::string> types;
        std::string lastCat;
        juce::PopupMenu sub;
        int id = 1;
        for (const auto& d : app::deviceCatalog()) {
            if (d.chain != app::Chain::Effect) continue;
            if (d.category != lastCat) { if (!lastCat.empty()) menu.addSubMenu(lastCat, sub); sub = juce::PopupMenu(); lastCat = d.category; }
            sub.addItem(id++, d.label);
            types.push_back(d.type);
        }
        menu.addSubMenu(lastCat, sub);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addFx_), [this, types](int r) {
            if (r < 1 || size_t(r) > types.size()) return;
            const auto& p = model.project();
            model.apply(app::edit::addDevice(p, model.selection().track, showingMaster() ? "master" : "fx", types[size_t(r - 1)]));
        });
    };
    addMidi_.onClick = [this] {
        juce::PopupMenu menu;
        std::vector<std::string> types;
        int id = 1;
        for (const auto& d : app::deviceCatalog()) if (d.chain == app::Chain::MidiFx) { menu.addItem(id++, d.label); types.push_back(d.type); }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addMidi_), [this, types](int r) {
            if (r >= 1 && size_t(r) <= types.size()) model.apply(app::edit::addDevice(model.project(), model.selection().track, "midifx", types[size_t(r - 1)]));
        });
    };
}
DeviceChainView::~DeviceChainView() = default;

juce::String DeviceChainView::signature() const {
    const auto& p = model.project();
    juce::String s = master_.isOn() ? "M" : "T" + juce::String(juce::int64(model.selection().track));
    s << "h" << getHeight();
    auto add = [&](const project::DeviceSpec& d) { s << "|" << juce::String(juce::int64(d.uid)) << d.type; };
    if (master_.isOn()) for (auto& d : p.masterFx) add(d);
    else if (const auto* t = app::edit::findTrack(p, model.selection().track)) {
        for (auto& d : t->midifx) add(d);
        if (!t->inst.type.empty()) add(t->inst);
        for (auto& d : t->fx) add(d);
    }
    return s;
}

void DeviceChainView::refresh(app::ModelEvent) {
    const auto sig = signature();
    if (sig != sig_) { rebuild(); return; }
    const auto& p = model.project();
    for (auto& panel : panels_)
        if (const auto* d = model.document().findDevice(panel->uid())) panel->sync(*d);
    (void)p;
}

void DeviceChainView::rebuild() {
    panels_.clear();
    sig_ = signature();
    const auto& p = model.project();
    const int availH = std::max(60, getHeight() - kTopH - 8);
    auto make = [&](const project::DeviceSpec& d, app::Chain c, bool master, size_t slot, size_t slots) {
        auto panel = std::make_unique<Panel>(model, d, c, master, slot, slots, availH);
        addAndMakeVisible(*panel);
        panels_.push_back(std::move(panel));
    };
    if (master_.isOn()) {
        for (size_t i = 0; i < p.masterFx.size(); ++i) make(p.masterFx[i], app::Chain::Effect, true, i, p.masterFx.size());
    } else if (const auto* t = app::edit::findTrack(p, model.selection().track)) {
        for (size_t i = 0; i < t->midifx.size(); ++i) make(t->midifx[i], app::Chain::MidiFx, false, i, t->midifx.size());
        if (!t->inst.type.empty() && t->kind != project::TrackKind::Bus) make(t->inst, app::Chain::Instrument, false, 0, 1);
        for (size_t i = 0; i < t->fx.size(); ++i) make(t->fx[i], app::Chain::Effect, false, i, t->fx.size());
    }
    addFx_.setVisible(master_.isOn() || model.selection().track != 0);
    addMidi_.setVisible(!master_.isOn() && model.selection().track != 0);
    for (auto& panel : panels_)
        if (const auto* d = model.document().findDevice(panel->uid())) panel->sync(*d);
    layoutPanels();
    repaint();
}

void DeviceChainView::layoutPanels() {
    int x = 8 - scrollX_;
    const int y = kTopH + 2, h = getHeight() - kTopH - 6;
    for (auto& p : panels_) { p->setBounds(x, y, p->width(), h); x += p->width() + 6; }
    contentW_ = x + scrollX_;
}

void DeviceChainView::resized() {
    auto top = juce::Rectangle<int>(0, 0, getWidth(), kTopH).reduced(8, 4);
    addFx_.setBounds(top.removeFromLeft(84));
    top.removeFromLeft(6);
    addMidi_.setBounds(top.removeFromLeft(70));
    master_.setBounds(top.removeFromRight(110));
    if (builtHeight_ != getHeight()) { builtHeight_ = getHeight(); if (!panels_.empty() || master_.isOn()) rebuild(); else layoutPanels(); }
    else layoutPanels();
}

void DeviceChainView::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    g.setColour(col::panel);
    g.fillRect(0, 0, getWidth(), kTopH);
    g.setColour(col::line);
    g.drawHorizontalLine(kTopH - 1, 0, float(getWidth()));
    if (panels_.empty()) {
        g.setColour(col::dim);
        g.setFont(uiFont(13.0f));
        g.drawText(master_.isOn() ? "The master chain is empty. Add an effect, or drag one from the browser." : model.selection().track ? "No devices. Double-click one in the browser, or drag it here." : "Select a track to see its devices.",
                   getLocalBounds().withTrimmedTop(kTopH), juce::Justification::centred);
    }
    if (dropHint_) { g.setColour(col::accent); g.drawRect(getLocalBounds().withTrimmedTop(kTopH), 2); }
}

void DeviceChainView::mouseDown(const juce::MouseEvent&) { model.selectDevice(0); }

void DeviceChainView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    scrollX_ = juce::jlimit(0, std::max(0, contentW_ - getWidth() + 16), scrollX_ - int(w.deltaX * 500.0f) - int(w.deltaY * 300.0f));
    layoutPanels();
}

void DeviceChainView::itemDropped(const SourceDetails& d) {
    dropHint_ = false;
    repaint();
    const auto parts = juce::StringArray::fromTokens(d.description.toString(), ":", "");
    if (parts.size() != 3) return;
    const auto& p = model.project();
    const auto uid = model.selection().track;
    const std::string chain = parts[1].toStdString(), type = parts[2].toStdString();
    if (chain == "inst") { if (uid) model.apply(app::edit::setInstrument(p, uid, type)); }
    else if (master_.isOn() && chain == "fx") model.apply(app::edit::addDevice(p, 0, "master", type));
    else if (uid) model.apply(app::edit::addDevice(p, uid, chain, type));
}

}  // namespace ddaw::ui
