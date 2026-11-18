#include "app/ui/DrumKitView.h"

#include <algorithm>

#include "project/ProjectJson.h"

namespace ddaw::ui {

namespace {
const char* const kWhat[3] = {"tune", "decay", "level"};
const char* const kCaption[3] = {"Tune", "Decay", "Level"};
}  // namespace

DrumKitView::DrumKitView(app::AppModel& m, project::Uid instUid, const app::DeviceInfo& info, int bodyH) : model_(m), uid_(instUid), info_(info), type_(info.type), bodyH_(bodyH) {
    addAndMakeVisible(kit_);
    kit_.onClick = [this] {
        juce::PopupMenu real;
        const auto& kits = app::drumKits();
        for (size_t i = 0; i < kits.size(); ++i) real.addItem(int(i) + 1, juce::String(kits[i].label) + (kits[i].type == type_ ? "  (current)" : ""), true, kits[i].type == type_);
        real.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&kit_), [this](int r) { if (r > 0) switchKit(app::drumKits()[size_t(r - 1)].type); });
    };
    for (size_t p = 0; p < 8; ++p) {
        pads_[p] = std::make_unique<Chip>(app::drumPadName(type_, int(p)), fx_blue());
        addAndMakeVisible(*pads_[p]);
        pads_[p]->onClick = [this, p] {                                         // hear the pad
            if (const auto uid = trackUid()) {
                model_.audition(uid, int(p), true);
                juce::Timer::callAfterDelay(180, [this, uid, p] { model_.audition(uid, int(p), false); });
            }
        };
        for (size_t w = 0; w < 3; ++w) {
            const std::string key = "p" + std::to_string(p) + "_" + kWhat[w];
            const ParamSpec* spec = nullptr;
            for (const auto& s : info_.params) if (key == s.key) spec = &s;
            if (!spec) continue;
            auto k = std::make_unique<Knob>(*spec, kCaption[w], w == 2 ? col::accent : fx_blue());
            k->onBegin = [this] { model_.beginGesture("parameter"); };
            k->onChange = [this, key](double v) { model_.apply(document::cmd::setParam(uid_, key, v)); };
            k->onEnd = [this] { model_.endGesture(); };
            addAndMakeVisible(*k);
            knobs_[p][w] = std::move(k);
        }
    }
    kit_.setText(juce::String("Kit: ") + [&] { for (const auto& k : app::drumKits()) if (k.type == type_) return k.label; return "Standard"; }());
}
DrumKitView::~DrumKitView() = default;

Knob* DrumKitView::knob(int pad, const char* what) {
    for (size_t w = 0; w < 3; ++w) if (std::string(what) == kWhat[w]) return knobs_[size_t(pad)][w].get();
    return nullptr;
}

project::Uid DrumKitView::trackUid() const {
    for (const auto& t : model_.project().tracks) if (t.inst.uid == uid_) return t.uid;
    return 0;
}

void DrumKitView::switchKit(const std::string& type) {
    if (type == type_) return;
    for (const auto& t : model_.project().tracks)
        if (t.inst.uid == uid_) {
            auto n = t.inst;                                                      // the pads' values (the same keys on every kit) and samples stay
            n.type = type;
            model_.apply({"inst.set", {{"track", t.uid}, {"device", project::deviceToJson(n, true)}}});
            return;
        }
}

void DrumKitView::sync(const project::DeviceSpec& d) {
    for (size_t p = 0; p < 8; ++p)
        for (size_t w = 0; w < 3; ++w) {
            if (!knobs_[p][w]) continue;
            const std::string key = "p" + std::to_string(p) + "_" + kWhat[w];
            const auto it = d.params.find(key);
            knobs_[p][w]->setStored(it != d.params.end());
            knobs_[p][w]->setValue(it != d.params.end() ? it->second : double(knobs_[p][w]->spec().def));
        }
}

void DrumKitView::resized() {
    auto r = getLocalBounds();
    auto top = r.removeFromTop(kTopH);
    top.removeFromLeft(118);                                                      // the panel's title is drawn here
    kit_.setBounds(top.removeFromLeft(168).reduced(0, 2));
    const int rows = 2, blockH = std::max(70, r.getHeight() / rows);
    for (int p = 0; p < 8; ++p) {
        const int col = p % kColumns, row = p / kColumns;
        auto b = juce::Rectangle<int>(r.getX() + col * kBlockW, r.getY() + row * blockH, kBlockW, blockH).reduced(2, 1);
        pads_[size_t(p)]->setBounds(b.removeFromTop(18));
        const int kw = b.getWidth() / 3;
        for (int w = 0; w < 3; ++w) if (knobs_[size_t(p)][size_t(w)]) knobs_[size_t(p)][size_t(w)]->setBounds(b.getX() + w * kw, b.getY(), kw, b.getHeight());
    }
}

void DrumKitView::paint(juce::Graphics& g) {
    for (int p = 0; p < 8; ++p) {
        const int col = p % kColumns, row = p / kColumns;
        const auto r = getLocalBounds().withTrimmedTop(kTopH);
        const int blockH = std::max(70, r.getHeight() / 2);
        fillRounded(g, juce::Rectangle<int>(col * kBlockW, r.getY() + row * blockH, kBlockW, blockH).reduced(1, 0).toFloat(), col::panel.withAlpha(0.6f), 4.0f);
    }
}

}  // namespace ddaw::ui
