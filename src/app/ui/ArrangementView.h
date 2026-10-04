#pragma once
#include <map>

#include "app/model/Timeline.h"
#include "app/ui/View.h"

namespace ddaw::ui {

// The timeline: one lane per track, clips at absolute ticks. Drags are previewed and committed on mouse
// up as a single undoable edit.
class ArrangementView : public View, public juce::DragAndDropTarget {
public:
    explicit ArrangementView(app::AppModel& m) : View(m) { setWantsKeyboardFocus(true); }
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override { clampScroll(); repaint(); }
    void tick() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void resized() override { clampScroll(); }

    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("sample:"); }
    void itemDropped(const SourceDetails&) override;
    void itemDragMove(const SourceDetails& d) override { dropPos_ = d.localPosition; repaint(); }
    void itemDragExit(const SourceDetails&) override { dropPos_ = {-1, -1}; repaint(); }

    static constexpr int kHeaderW = 150, kRulerH = 30, kLaneH = 68;

    app::TimeScale& scale() { return scale_; }
    // Which clip (arrangement key) and which part is under a point. Public for tests.
    struct Hit { enum Part { None, Body, RightEdge } part = None; std::string key; int lane = -1; };
    Hit clipAt(juce::Point<int> p) const;
    juce::Rectangle<int> clipRect(const std::string& key) const;

private:
    int laneOf(int y) const { return (y - kRulerH + scrollY_) / kLaneH; }
    int laneTop(int lane) const { return kRulerH + lane * kLaneH - scrollY_; }
    double tickAt(int x) const { return scale_.toTick(double(x - kHeaderW)); }
    double grid(const juce::MouseEvent& e) const { return e.mods.isShiftDown() ? 0.0 : std::max(app::autoGrid(scale_, 12.0), 24.0); }
    void clampScroll();
    void drawClip(juce::Graphics&, const std::string& key, const project::ArrClip&, juce::Rectangle<int> r, juce::Colour c, bool selected, bool ghost);
    const std::vector<float>& peaksFor(const std::string& sampleId);

    app::TimeScale scale_{24.0, 0.0};
    int scrollY_ = 0;
    enum class Drag { None, Move, Resize, Cursor, Loop } drag_ = Drag::None;
    std::string dragKey_;
    double dragOrigStart_ = 0, dragOrigLen_ = 0, dragGrabOffset_ = 0, previewStart_ = 0, previewLen_ = 0, loopA_ = 0;
    int previewLane_ = -1;
    bool dragMoved_ = false;
    juce::Point<int> dropPos_{-1, -1};
    std::map<std::string, std::vector<float>> peaks_;
    double lastPlayhead_ = -1;
};

}  // namespace ddaw::ui
