#pragma once
// The session and the mixer in one view (ARCH 31). Each track is a column: its mixing section (a MixStrip: fader and meter, pan, mute and
// solo, a send to every bus, the output of a bus) in the header, the clips of every scene below. The header, the scene column and the
// master fader stay where they are: the grid scrolls down for more scenes, the columns scroll sideways for more tracks, and the master
// is locked at the far right so it never scrolls away. Input channels come first, a vertical line separates them from the buses.
#include <map>
#include <memory>

#include "app/ui/MixStrip.h"
#include "app/ui/View.h"

namespace ddaw::ui {

class SessionView : public View, public juce::DragAndDropTarget {
public:
    explicit SessionView(app::AppModel& m);
    ~SessionView() override;
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override;
    void tick() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void resized() override;

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override { dropSlot_ = {-1, -1}; repaint(); }

    static constexpr int kSceneW = 104, kTrackW = MixStrip::kW, kRowH = 34, kAddW = 46, kSepW = 14, kMasterW = MixStrip::kMasterW;

    // Hit testing, public for tests. `track` is the track's place in the project's list (not the column).
    struct Hit {
        enum Kind { None, Slot, SlotLaunch, TrackHeader, SceneLaunch, SceneHeader, AddTrack, AddScene } kind = None;
        int track = -1, scene = -1;
    };
    Hit hitAt(juce::Point<int> p) const;

    int headerH() const { return mixOpen_ ? MixStrip::kOpenH : MixStrip::kClosedH; }
    bool mixOpen() const { return mixOpen_; }
    void setMixOpen(bool open);
    int inputCount() const { return nInputs_; }
    int busCount() const { return int(order_.size()) - nInputs_; }
    int columnOf(int projectTrack) const { return projectTrack >= 0 && size_t(projectTrack) < col_.size() ? col_[size_t(projectTrack)] : -1; }
    int columnX(int projectTrack) const;                      // the left edge of that track's column
    int separatorX() const;                                    // the vertical line between the inputs and the buses (-1: none)
    juce::Rectangle<int> slotRect(int projectTrack, int scene) const;
    MixStrip* strip(project::Uid uid);
    MixStrip* masterStrip() { return master_.get(); }
    juce::Rectangle<int> masterBounds() const { return {getWidth() - kMasterW, 0, kMasterW, headerH()}; }
    int scrollX() const { return scrollX_; }
    int scrollY() const { return scrollY_; }
    void scrollTo(int x, int y) { scrollX_ = x; scrollY_ = y; clampScroll(); layoutStrips(); repaint(); }

    std::function<void()> onRouting;                           // the Routing button: the app shows the routing matrix

private:
    juce::Rectangle<int> viewport() const { return {kSceneW, headerH(), std::max(0, getWidth() - kSceneW - kMasterW), std::max(0, getHeight() - headerH())}; }
    void rebuildStrips();
    void layoutStrips();
    void clampScroll();
    void contextMenu(const Hit&);
    void showAddTrackMenu();
    int sceneCount() const { return int(model.project().scenes.size()); }
    int trackCount() const { return int(model.project().tracks.size()); }

    class Host;
    std::unique_ptr<Host> host_;                                // clips the strips to the scrolling area
    std::map<project::Uid, std::unique_ptr<MixStrip>> strips_;
    std::unique_ptr<MixStrip> master_;
    std::vector<std::pair<project::Uid, int>> key_;             // what the strips were built for (uid, role)
    std::vector<int> order_, col_;                              // display order: project indices, inputs first; and the reverse
    int nInputs_ = 0;
    bool mixOpen_ = true;
    Chip mixToggle_{"Mix"}, routing_{"Routing"};
    int scrollX_ = 0, scrollY_ = 0;
    std::vector<int> playing_;                                  // last drawn scene per track (-1 none)
    std::vector<bool> queued_;
    juce::Point<int> dropSlot_{-1, -1};
};

}  // namespace ddaw::ui
