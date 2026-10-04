#pragma once
#include "app/ui/View.h"

namespace ddaw::ui {

// The clip launcher: tracks are columns, scenes are rows. Painted and hit-tested by hand so the headers
// and the scene column stay fixed while the grid scrolls.
class SessionView : public View, public juce::DragAndDropTarget {
public:
    explicit SessionView(app::AppModel& m) : View(m) { setWantsKeyboardFocus(true); }
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override;
    void tick() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void resized() override { clampScroll(); }

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override { dropSlot_ = {-1, -1}; repaint(); }

    static constexpr int kSceneW = 104, kTrackW = 122, kHeaderH = 72, kRowH = 34, kAddW = 46;

    // Hit testing, public for tests.
    struct Hit {
        enum Kind { None, Slot, SlotLaunch, TrackHeader, Mute, Solo, TrackStop, Arm, SceneLaunch, SceneHeader, AddTrack, AddScene } kind = None;
        int track = -1, scene = -1;
    };
    Hit hitAt(juce::Point<int> p) const;

private:
    juce::Rectangle<int> slotRect(int t, int s) const;
    juce::Rectangle<int> headerRect(int t) const;
    void clampScroll();
    void contextMenu(const Hit&);
    void showAddTrackMenu();
    int sceneCount() const { return int(model.project().scenes.size()); }
    int trackCount() const { return int(model.project().tracks.size()); }

    int scrollX_ = 0, scrollY_ = 0;
    std::vector<int> playing_;      // last drawn scene per track (-1 none)
    std::vector<bool> queued_;
    juce::Point<int> dropSlot_{-1, -1};
};

}  // namespace ddaw::ui
