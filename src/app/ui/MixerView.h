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

    static constexpr int kStripW = 92, kHeaderH = 22;
    int stripCount() const { return int(strips_.size()); }          // track, bus and send-return strips (not the master)
    int returnCount() const;                                         // of those, the send returns (A, B, F), which sit in their own group
    int legacyReturnCount() const { return int(legacy_.size()); }   // the project's built-in return channels (imported projects)
    // The "+ Return" button: adds the send return for the next free letter (A, then B); hidden when both exist.
    bool addReturnVisible() const { return addReturn_.isVisible(); }
    void addReturn();

private:
    class Strip;
    class LegacyStrip;
    void rebuild();

    std::vector<std::unique_ptr<Strip>> strips_;       // display order: tracks and buses in document order, then the send returns
    std::vector<size_t> docIdx_;                       // each strip's index in the project's track list (meters, colours)
    std::vector<std::unique_ptr<LegacyStrip>> legacy_;
    std::unique_ptr<Strip> master_;
    std::vector<std::pair<project::Uid, int>> key_;    // what the strips were built for: (uid, send letter)
    Chip addReturn_{"+ Return"};
    int scrollX_ = 0, returnsX_ = 0, nReturns_ = 0;   // nReturns_: how many of the strips (the last ones) are send returns
};

}  // namespace ddaw::ui
