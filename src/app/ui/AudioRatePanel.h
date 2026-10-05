#pragma once
#include <memory>

#include "app/ui/ParamPicker.h"
#include "app/ui/View.h"
#include "project/ProjectJson.h"

namespace ddaw::ui {

// Audio-rate modulation of the selected track (B4): per-sample sources (an oscillator, or another track's audio,
// raw or through an envelope follower) driving the audio-rate ports of the track's instrument and effects (the
// parameters a device marks `audioRate`, e.g. FM Operators: index, pitch, amp). Depth and rate are live knobs;
// source, target and the rest are structural edits that rebuild the graph.
class AudioRatePanel : public View {
public:
    explicit AudioRatePanel(app::AppModel& m);
    ~AudioRatePanel() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override;

    class Row;
    int rows() const { return int(rows_.size()); }
    Row& row(int i) { return *rows_[size_t(i)]; }
    void addRoute();

    static constexpr int kRowH = 62, kHeaderH = 30;

private:
    void rebuild();
    juce::String signature() const;

    Chip add_{"+ Route", col::accent};
    std::vector<std::unique_ptr<Row>> rows_;
    juce::String sig_;
    project::Uid track_ = 0;
};

// One route. Public so tests can drive its controls.
class AudioRatePanel::Row : public juce::Component {
public:
    Row(app::AppModel& m, project::Uid track, int index);
    void sync(const project::ARateSpec& r);
    void resized() override;
    void paint(juce::Graphics& g) override;
    // controls, for tests
    Chip& sourceChip() { return source_; }
    Chip& shapeChip() { return shape_; }
    Chip& followChip() { return follow_; }
    Chip& targetChip() { return target_; }
    Chip& trackChip() { return track_; }
    NumberBox& rateBox() { return rate_; }
    Knob& depthKnob() { return depth_; }

private:
    void edit(const std::function<void(project::ARateSpec&)>& f);
    void trackMenu();
    app::AppModel& model_;
    project::Uid uid_;
    int idx_;
    Chip on_{"ON", col::play}, source_{"Osc"}, shape_{"Sine"}, track_{"track"}, follow_{"FOLLOW"}, target_{"no target"}, del_{"x"};
    NumberBox rate_{0.05, 12000.0, 1, " Hz"};
    Knob depth_;
};

}  // namespace ddaw::ui
