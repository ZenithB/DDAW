#pragma once
#include "app/ui/View.h"

namespace ddaw::ui {

// Play/stop, tempo, metronome, loop, position, undo/redo, master meter.
class TransportBar : public View {
public:
    explicit TransportBar(app::AppModel& m);
    void resized() override;
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override;
    void tick() override;
    std::function<void()> onAudioSettings;

private:
    Chip play_{"PLAY", col::play}, stop_{"STOP"}, rec_{"REC", col::rec}, recOpt_{"..."}, metro_{"METRO"}, loop_{"LOOP"}, mode_{"ARR"}, undo_{"UNDO"}, redo_{"REDO"}, audio_{"AUDIO"};
    NumberBox bpm_{20, 400, 1, " bpm"};
    NumberBox swing_{0, 100, 0, "% swg"};
    Chip key_{"A Minor"};   // the project's key: root and scale (MIDI scale effect, arpeggiator scale walk, autotune)
    juce::String title_, pos_;
    juce::Rectangle<int> posRect_, titleRect_;
    juce::Rectangle<float> meterRect_;
    float masterPeak_ = 0, grDb_ = 0, inputPeak_ = 0;
    juce::String recText_;
    juce::Rectangle<float> inMeterRect_;
    void showRecordOptions();
};

}  // namespace ddaw::ui
