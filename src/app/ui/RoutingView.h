#pragma once
// The routing matrix (ARCH 31): every source down the side (the input channels, then the buses), every destination across the top (the
// buses, then the master). One picture for both ways a bus is routed:
//   - a cell's dial is the send level from that row to that bus (drag up and down; click to switch it on or off; right-click clears);
//   - the small diamond in a cell's corner marks the row's output: exactly one per row (a track always feeds the master; a bus may be
//     sent to the master or to any other bus that does not already feed it). Click a diamond to route there.
// Cells that would make a loop (a bus feeding itself, or a bus that already feeds the row) are hatched and do nothing. The cells glow
// with the signal passing through them, and the line under the matrix traces what is under the pointer.
#include <map>

#include "app/ui/View.h"

namespace ddaw::ui {

class RoutingView : public View {
public:
    explicit RoutingView(app::AppModel& m);
    void paint(juce::Graphics&) override;
    void refresh(app::ModelEvent) override;
    void tick() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override { hover_ = {}; repaint(); }
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;       // scrolls the cells; the labels stay
    void resized() override { clampScroll(); }
    int scrollX() const { return scrollX_; }
    int scrollY() const { return scrollY_; }

    static constexpr int kLabelW = 150, kHeadH = 64, kCellW = 76, kCellH = 36, kGap = 16, kFootH = 26;

    // Geometry and state, public for tests. `dst` 0 is the master.
    juce::Rectangle<int> cellRect(project::Uid src, project::Uid dst) const;
    juce::Rectangle<int> cornerRect(project::Uid src, project::Uid dst) const;       // the diamond that makes this cell the row's output
    bool sendAllowed(project::Uid src, project::Uid dst) const;                       // a send from src to the bus dst is possible
    bool outputAllowed(project::Uid src, project::Uid dst) const;
    double sendLevel(project::Uid src, project::Uid dst) const;
    bool isOutput(project::Uid src, project::Uid dst) const;
    juce::String hoverText() const { return hoverText_; }
    int rowCount() const { return int(rows_.size()); }
    int columnCount() const { return int(cols_.size()); }

private:
    struct Cell { project::Uid src = 0, dst = 0; bool valid = false; bool corner = false; };
    Cell cellAt(juce::Point<int>) const;
    void rebuild();
    void clampScroll();
    int rowY(size_t r) const;
    int colX(size_t c) const;
    void describe(const Cell&);
    std::vector<project::Uid> rows_, cols_;       // sources (inputs first, then buses) and destinations (buses, then 0 = master)
    size_t nInputRows_ = 0;
    Cell hover_, drag_;
    int scrollX_ = 0, scrollY_ = 0;
    double dragStart_ = 0.0;
    float dragY_ = 0.0f;
    bool dragged_ = false;
    juce::String hoverText_;
    std::map<project::Uid, float> peak_;
};

}  // namespace ddaw::ui
