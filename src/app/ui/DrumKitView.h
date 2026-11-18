#pragma once
// The panel of a drum machine (ARCH 29): a kit dropdown on top, then the eight pads as blocks, each with its name (click it to hear the
// pad) and its tune, decay and level. Switching kits replaces the instrument and keeps the pads' settings and samples.
#include <array>
#include <memory>

#include "app/model/AppModel.h"
#include "app/ui/Widgets.h"

namespace ddaw::ui {

inline juce::Colour fx_blue() { return juce::Colour(0xff5aa9e6); }

class DrumKitView : public juce::Component {
public:
    DrumKitView(app::AppModel& m, project::Uid instUid, const app::DeviceInfo& info, int bodyH);
    ~DrumKitView() override;
    static constexpr int kBlockW = 154, kColumns = 4, kTopH = 24;
    int preferredWidth() const { return kColumns * kBlockW; }
    void sync(const project::DeviceSpec& d);
    void resized() override;
    void paint(juce::Graphics&) override;

    // for tests
    Chip& kitChip() { return kit_; }
    Chip& padButton(int pad) { return *pads_[size_t(pad)]; }
    Knob* knob(int pad, const char* what);         // what: "tune", "decay" or "level"
    juce::String padLabel(int pad) const { return pads_[size_t(pad)]->text(); }
    void switchKit(const std::string& type);
    juce::String kitLabel() const { return kit_.text(); }

private:
    project::Uid trackUid() const;
    app::AppModel& model_;
    project::Uid uid_;
    const app::DeviceInfo& info_;
    std::string type_;
    int bodyH_;
    Chip kit_{"Standard"};
    std::array<std::unique_ptr<Chip>, 8> pads_;
    std::array<std::array<std::unique_ptr<Knob>, 3>, 8> knobs_;
};

}  // namespace ddaw::ui
