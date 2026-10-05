#pragma once
#include "app/ui/View.h"

namespace ddaw::ui {

// The selected track's devices as a row of panels: MIDI effects, the instrument, then audio effects.
// Every parameter is a knob generated from the device's schema; edits are live and undoable.
class DeviceChainView : public View, public juce::DragAndDropTarget {
public:
    explicit DeviceChainView(app::AppModel& m);
    ~DeviceChainView() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override;
    void tick() override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDown(const juce::MouseEvent&) override;

    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("device:"); }
    void itemDropped(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override { dropHint_ = true; repaint(); }
    void itemDragExit(const SourceDetails&) override { dropHint_ = false; repaint(); }

    class Panel;
    int panelCount() const { return int(panels_.size()); }
    bool showingMaster() const { return master_.isOn(); }

private:
    void rebuild();
    void layoutPanels();
    juce::String signature() const;

    std::vector<std::unique_ptr<Panel>> panels_;
    Chip addFx_{"+ Effect"}, addMidi_{"+ MIDI"}, master_{"Master chain"};
    juce::String sig_;
    int scrollX_ = 0, contentW_ = 0;
    bool dropHint_ = false;
    int builtHeight_ = 0;
};

}  // namespace ddaw::ui
