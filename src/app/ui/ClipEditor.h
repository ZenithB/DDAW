#pragma once
#include <set>

#include "app/model/Timeline.h"
#include "app/ui/View.h"

namespace ddaw::ui {

// Piano roll for the open clip (note clips) with a velocity lane; waveform and controls for audio clips.
class ClipEditor : public View {
public:
    explicit ClipEditor(app::AppModel& m);
    void paint(juce::Graphics&) override;
    void resized() override;
    void refresh(app::ModelEvent) override;
    void tick() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

    static constexpr int kKeysW = 64, kTopH = 30, kVelH = 64, kExprH = 112;

    // The lane under the notes: velocity bars, or one of the per-note expression curves (MPE: slide, pressure, pitch bend).
    enum class Lane { Velocity, Slide, Pressure, Bend };
    void setLane(Lane l);
    Lane lane() const { return lane_; }
    void setDraw(bool on);   // freehand drawing of curves in the expression lanes
    int laneH() const { return lane_ == Lane::Velocity ? kVelH : kExprH; }
    juce::Rectangle<int> laneRect() const;
    // Expression lane geometry: value <-> pixel row (slide and pressure 0..1, bend +-bendSpan() semitones) and the x of a
    // point t ticks into a note. Public for tests.
    double bendSpan() const;
    double laneMin() const { return lane_ == Lane::Bend ? -bendSpan() : 0.0; }
    double laneMax() const { return lane_ == Lane::Bend ? bendSpan() : 1.0; }
    int laneY(double value) const;
    double laneValue(int y) const;
    int laneX(const project::Note&, double tRel) const;
    const std::vector<project::ExprPoint>& curveOf(const project::Note&) const;
    int selectedPoint() const { return exprIdx_; }   // the point last clicked (index into its note's curve), -1 when none

    // Geometry and hit-testing, public for tests.
    juce::Rectangle<int> gridRect() const;
    int pitchAt(int y) const;
    int rowTop(int pitch) const;
    double rowH() const { return drum_ ? 22.0 : 13.0; }
    int topPitch() const { return drum_ ? 7 : 127; }
    struct NoteHit { project::Uid uid = 0; bool edge = false; };
    NoteHit noteAt(juce::Point<int>) const;
    juce::Rectangle<int> noteRect(const project::Note&) const;
    const std::set<project::Uid>& selected() const { return sel_; }
    app::TimeScale& scale() { return scale_; }
    void selectAll();

private:
    const project::Clip* clip() const;
    project::Uid trackUid() const;  // the track the open clip plays on (0: none)
    double grid() const { return gridTicks_ > 0 ? gridTicks_ : std::max(app::autoGrid(scale_, 10.0), 6.0); }
    void commitNotes(const std::string& label, const std::vector<project::Note>& edited);
    void deleteSelected();
    void duplicateSelected();
    void nudge(int dPitch, double dTicks);
    void layoutAudio();
    void cycleGrid();
    void commitAudio(const std::function<void(project::AudioClipData&)>& f);
    void centreOnNotes();
    std::vector<project::ExprPoint>& curveOf(project::Note&) const;
    const project::Note* noteById(project::Uid) const;
    // Which note an expression-lane click at x belongs to, and which of its points (if any) lies under (x, y).
    struct PointHit { project::Uid note = 0; int index = -1; };
    PointHit pointAt(juce::Point<int>) const;
    project::Uid noteAtTime(double tick) const;
    void exprDown(const juce::MouseEvent&);
    void exprDrag(const juce::MouseEvent&);
    void exprUp();
    void deletePoint(project::Uid note, int index);
    void clearCurves();
    void drawLane(juce::Graphics&, const project::Clip&, juce::Colour);

    app::edit::ClipRef ref_;
    bool drum_ = false;
    app::TimeScale scale_{64.0, 0.0};
    double scrollY_ = 0;
    double gridTicks_ = 0;  // 0 = auto
    std::set<project::Uid> sel_;

    enum class Mode { None, Move, Resize, Marquee, Velocity, Key, ExprPoint, ExprDraw } mode_ = Mode::None;
    juce::Point<int> downPos_;
    double grabTick_ = 0;
    int grabPitch_ = 0, dPitch_ = 0, keyDown_ = -1;
    double dTicks_ = 0, dDur_ = 0;
    juce::Rectangle<int> marquee_;
    Lane lane_ = Lane::Velocity;
    bool draw_ = false;
    project::Note work_;          // the note being edited in an expression lane (its curve is edited here until mouse-up)
    project::Uid selPtNote_ = 0;  // the note owning the selected point
    int exprIdx_ = -1;            // the selected point: being dragged, or last clicked (-1: none)
    double drawLastT_ = 0;
    bool exprDirty_ = false;
    project::Uid velUid_ = 0;
    double velValue_ = 0;
    bool moved_ = false;
    double playPos_ = -1;
    juce::String lastClipKey_;

    Chip gridChip_{"Grid: Auto"};
    NumberBox lenBox_{0.25, 64, 2, " bars"};
    Knob gainKnob_, pitchKnob_;
    Chip loopChip_{"LOOP"}, revChip_{"REV"};
    Chip velChip_{"Vel"}, slideChip_{"Slide"}, pressChip_{"Press"}, bendChip_{"Bend"}, drawChip_{"Draw"}, clearChip_{"Clear"};
};

}  // namespace ddaw::ui
