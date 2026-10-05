#pragma once
#include <deque>

#include "app/ui/LiveInput.h"
#include "app/ui/View.h"

namespace ddaw::ui {

// Live performance tracking: switch the tracker on, choose the instrument it plays, set the pitch range,
// watch the pitch / loudness / envelope, and manage the performance routes (input -> parameter) of the
// selected track, optionally recording their curves into automation while recording.
class TrackingPanel : public View {
public:
    TrackingPanel(app::AppModel& m, LiveInput& live);
    void paint(juce::Graphics&) override;
    void resized() override;
    void refresh(app::ModelEvent) override;
    void tick() override;
    void mouseDown(const juce::MouseEvent&) override;

    // One row of the route list per perf spec of the selected track (public for tests).
    int routeCount() const;
    static constexpr int kRouteH = 28, kTopH = 234;
    juce::Rectangle<int> routeRect(int i) const { return {330, kTopH + 8 + i * kRouteH, getWidth() - 346, kRouteH - 3}; }

private:
    void addRouteMenu();
    void targetMenu(const std::string& source);
    juce::String describe(const project::PerfSpec&) const;

    Chip track_{"TRACK INPUT", col::play}, target_{"Play: none"}, curves_{"RECORD CURVES", col::rec}, addRoute_{"+ Route", col::accent}, keys_{"COMPUTER KEYBOARD"}, poly_{"AUDIO -> NOTES", col::play}, notes_{"RECORD NOTES", col::rec}, mpe_{"MPE", col::play};
    NumberBox bendRange_{1, 96, 0, " st"}, mpeRange_{1, 96, 0, " st"};
    LiveInput& live_;
    NumberBox minHz_{40, 600, 0, " Hz"};
    std::deque<float> pitchTrace_, loudTrace_;   // recent samples for the plots (semitones above C0 and 0..1)
    PerformanceFrame last_{};
};

}  // namespace ddaw::ui
