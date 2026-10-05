#pragma once
#include <memory>

#include "app/model/Catalog.h"
#include "app/model/Controllers.h"
#include "app/ui/ControllerInput.h"
#include "app/ui/ParamPicker.h"
#include "app/ui/View.h"
#include "project/ProjectJson.h"

namespace ddaw::ui {

// Morph maps of the selected track (B5): a 2D field with anchor presets and a stick. Dragging the stick moves every
// targeted parameter at once (live, no rebuild); anchors capture the targets' current values; the method, its
// spread and each target's response curve shape the blend. Everything but the stick is a structural edit.
class MorphPanel : public View {
public:
    // `controllers` (optional) lets the panel show the gamepad's state.
    explicit MorphPanel(app::AppModel& m, ControllerInput* controllers = nullptr);
    ~MorphPanel() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override;

    // The XY field. Public so tests can drive it with mouse events.
    class Pad : public juce::Component {
    public:
        explicit Pad(MorphPanel& owner) : owner_(owner) {}
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseDoubleClick(const juce::MouseEvent&) override;
        juce::Rectangle<int> field() const { return getLocalBounds().reduced(10); }
        juce::Point<int> toPixel(double x, double y) const;    // field coordinates (0..1, y up) -> pixels
        juce::Point<double> toField(juce::Point<int> p) const;
    private:
        MorphPanel& owner_;
        int dragAnchor_ = -1;
        bool dragStick_ = false, moved_ = false;
        project::MorphSpec preview_;                           // the map while an anchor is being dragged
        bool previewing_ = false;
        friend class MorphPanel;
    };

    Pad& pad() { return pad_; }
    Chip& methodChip() { return method_; }
    Chip& onChip() { return on_; }
    Chip& learnXChip() { return learnX_; }
    Chip& learnYChip() { return learnY_; }
    int bindingRows() const { return int(bindChips_.size()); }
    int selectedMap() const { return sel_; }
    int selectedAnchor() const { return anchor_; }
    const project::MorphSpec* current() const;
    // actions the controls perform (public for tests)
    void addMap();
    void addAnchorHere();
    void captureIntoSelected();
    void removeSelectedAnchor();
    void addTarget(const PickedParam& p);
    void select(int map) { sel_ = map; anchor_ = -1; refresh(app::ModelEvent::Document); }

    static constexpr int kPadW = 340, kHeaderH = 30;

private:
    void edit(const std::function<void(project::MorphSpec&)>& f);
    std::vector<double> captureValues(const project::Track& t, const project::MorphSpec& m) const;
    void rebuildControls();

    Pad pad_{*this};
    Chip add_{"+ Map", col::accent}, on_{"ON", col::play}, method_{"IDW"}, addAnchor_{"+ Anchor here", col::accent}, capture_{"Capture into anchor"},
        delAnchor_{"Delete anchor"}, addTarget_{"+ Target", col::accent}, learnX_{"Learn X", col::rec}, learnY_{"Learn Y", col::rec};
    NumberBox spread_{0.5, 8.0, 1, ""};
    std::vector<std::unique_ptr<Chip>> mapChips_;
    std::vector<std::unique_ptr<Chip>> targetChips_, removeChips_, bindChips_, bindRemove_;
    ControllerInput* controllers_ = nullptr;
    std::vector<std::unique_ptr<NumberBox>> curveBoxes_;
    juce::String sig_;
    juce::Rectangle<int> controllerLine_;
    int sel_ = 0, anchor_ = -1;
    project::Uid track_ = 0;
};

}  // namespace ddaw::ui
