#include "app/ui/RecordOptions.h"

namespace ddaw::ui {

RecordOptions::RecordOptions(app::AppModel& m) : model_(m) {
    auto& r = model_.recording();
    for (int i = 0; i < 3; ++i) {
        addAndMakeVisible(countIn_[i]);
        countIn_[i].onClick = [this, i] { model_.recording().settings().countInBars = i; model_.notify(app::ModelEvent::Recording); refreshChips(); };
        addAndMakeVisible(mode_[i]);
        mode_[i].onClick = [this, i] {
            if (model_.recording().env().setInputMode) model_.recording().env().setInputMode(i);
            model_.notify(app::ModelEvent::Recording);
            refreshChips();
        };
    }
    addAndMakeVisible(monitor_);
    monitor_.setToggleable(true);
    monitor_.onToggle = [this](bool on) { model_.recording().setMonitor(on); model_.notify(app::ModelEvent::Recording); };
    addAndMakeVisible(offset_);
    offset_.onChange = [this](double v) { model_.recording().settings().offsetMs = v; model_.notify(app::ModelEvent::Recording); };
    if (r.env().deviceInfo) info_ = r.env().deviceInfo();
    refreshChips();
    setSize(340, 214);
}

void RecordOptions::refreshChips() {
    const auto& s = model_.recording().settings();
    for (int i = 0; i < 3; ++i) countIn_[i].setOn(s.countInBars == i);
    const int mode = model_.recording().env().inputMode ? model_.recording().env().inputMode() : 0;
    for (int i = 0; i < 3; ++i) mode_[i].setOn(mode == i);
    monitor_.setOn(s.monitor);
    offset_.setValue(s.offsetMs);
}

void RecordOptions::resized() {
    auto r = getLocalBounds().reduced(14);
    r.removeFromTop(22);
    auto row = r.removeFromTop(26);
    row.removeFromLeft(100);
    for (int i = 0; i < 3; ++i) { countIn_[i].setBounds(row.removeFromLeft(44)); row.removeFromLeft(6); }
    r.removeFromTop(10);
    row = r.removeFromTop(26);
    row.removeFromLeft(100);
    for (int i = 0; i < 3; ++i) { mode_[i].setBounds(row.removeFromLeft(62)); row.removeFromLeft(6); }
    r.removeFromTop(10);
    row = r.removeFromTop(26);
    row.removeFromLeft(100);
    monitor_.setBounds(row.removeFromLeft(90));
    r.removeFromTop(10);
    row = r.removeFromTop(28);
    row.removeFromLeft(100);
    offset_.setBounds(row.removeFromLeft(90));
}

void RecordOptions::paint(juce::Graphics& g) {
    g.fillAll(col::panel2);
    g.setColour(col::text);
    g.setFont(uiFont(13.0f, true));
    g.drawText("Recording", 14, 8, 200, 18, juce::Justification::centredLeft);
    g.setColour(col::dim);
    g.setFont(uiFont(12.0f));
    g.drawText("Count-in (bars)", 14, 44, 100, 26, juce::Justification::centredLeft);
    g.drawText("Input", 14, 80, 100, 26, juce::Justification::centredLeft);
    g.drawText("Monitoring", 14, 116, 100, 26, juce::Justification::centredLeft);
    g.drawText("Latency offset", 14, 152, 100, 28, juce::Justification::centredLeft);
    g.setFont(uiFont(10.5f));
    g.setColour(col::faint);
    g.drawFittedText(info_, juce::Rectangle<int>(14, 182, getWidth() - 28, 28), juce::Justification::topLeft, 2);
}

}  // namespace ddaw::ui
