#pragma once
#include "app/ui/View.h"

namespace ddaw::ui {

// One channel strip per track plus the master: fader and meter, pan, sends, mute/solo, routing.
class MixerView : public View {
public:
    explicit MixerView(app::AppModel& m);
    ~MixerView() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override;
    void tick() override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr int kStripW = 92;
    int stripCount() const { return int(strips_.size()); }

private:
    class Strip;
    void rebuild();

    std::vector<std::unique_ptr<Strip>> strips_;
    std::unique_ptr<Strip> master_;
    std::vector<project::Uid> uids_;
    int scrollX_ = 0;
};

}  // namespace ddaw::ui
