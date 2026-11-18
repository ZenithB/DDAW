#pragma once
#include <array>
#include <map>
#include <set>

#include "app/model/NoteOps.h"
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
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

    static constexpr int kKeysW = 64, kTopH = 58, kVelH = 64, kExprH = 112;     // the top is two rows: the tools and the snap above, the clip and its lanes below

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
    struct NoteHit { project::Uid uid = 0; bool edge = false; bool leftEdge = false; };
    NoteHit noteAt(juce::Point<int>) const;
    juce::Rectangle<int> noteRect(const project::Note&) const;
    const std::set<project::Uid>& selected() const { return sel_; }
    app::TimeScale& scale() { return scale_; }
    void selectAll();

    // ---- editing tools (ARCH 28): the modes of Ableton Live's draw mode and FL Studio's piano roll tools ----
    //   Select  click a note to select, drag to move (Alt: no snap, Cmd/Ctrl: copy, Shift: one axis), drag its edges to resize, drag in
    //           empty space for a marquee, double-click to add; right-click for the edit menu
    //   Draw    click adds a note (the length of the last one you touched), drag sets its length; existing notes move like Select; right-drag erases
    //   Paint   drag paints a note per grid step; clicking a note deletes it (Ableton's draw mode)
    //   Erase   click or drag deletes the notes under the pointer
    //   Slice   click or drag across notes to cut them at the line
    //   Chord   click stamps a chord (a shape chosen from a menu) on the clicked note
    enum class Tool { Select, Draw, Paint, Erase, Slice, Chord };
    void setTool(Tool t);
    Tool tool() const { return tool_; }
    juce::Component* toolChip(Tool t) { return &toolChips_[size_t(t)]; }
    // Snap: the grid (auto or a note value), triplets, and the magnet that can be switched off (Alt bypasses it for one gesture).
    void setSnap(bool on) { snapOn_ = on; snapChip_.setOn(on); repaint(); }
    bool snapOn() const { return snapOn_; }
    void setTriplet(bool on) { triplet_ = on; repaint(); }
    bool triplet() const { return triplet_; }
    void setGridTicks(double t);                        // 0 = auto
    double gridStep() const { return grid(); }          // the step in ticks that snapping uses right now
    void setScaleHighlight(bool on) { scaleHi_ = on; scaleChip_.setOn(on); repaint(); }
    bool scaleHighlight() const { return scaleHi_; }
    void setScaleSnap(bool on) { scaleSnap_ = on; repaint(); }
    bool scaleSnap() const { return scaleSnap_; }
    bool inScale(int pitch) const { return !drum_ && scaleMask_[size_t(((pitch % 12) + 12) % 12)]; }
    void setGhost(bool on) { ghost_ = on; ghostChip_.setOn(on); repaint(); }
    bool ghost() const { return ghost_; }
    void setChord(int index);
    int chord() const { return chordIdx_; }
    double lastLength() const { return lastLen_; }
    // Whole-selection operations (the selection, or every note when nothing is selected), each one undo step.
    enum class Op { Quantize, QuantizeHalf, QuantizeEnds, Legato, Chop2, Chop3, Chop4, Chop8, FlipTime, FlipPitch, OctaveUp, OctaveDown, SemitoneUp, SemitoneDown,
                    Double, Half, Humanize, VelocityUp, VelocityDown, VelocityRandom, VelocityFull };
    bool applyOp(Op op);
    void copySelection(bool cut);
    bool paste();
    static bool hasClipboard();
    void showEditMenu(juce::Point<int> at);

private:
    const project::Clip* clip() const;
    project::Uid trackUid() const;  // the track the open clip plays on (0: none)
    double grid() const {
        const double g = gridTicks_ > 0 ? gridTicks_ : std::max(app::autoGrid(scale_, 10.0), 6.0);
        return triplet_ ? g * 2.0 / 3.0 : g;
    }
    double snapStep(const juce::MouseEvent& e) const { return snapOn_ != e.mods.isAltDown() ? grid() : 0.0; }     // 0: no snapping for this gesture
    int snapPitch(int pitch) const;
    void blip(int pitch);
    std::vector<project::Note> targetNotes() const;                                                              // the selection, or every note
    void commitAdded(const std::string& label, const std::vector<project::Note>& edited, const std::vector<project::Note>& added);
    void showGridMenu();
    void rebuildScale();
    void layoutTop();
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

    enum class Mode { None, Move, Resize, ResizeL, Marquee, Velocity, Key, ExprPoint, ExprDraw, DrawNote, Paint, Erase, Slice } mode_ = Mode::None;
    Tool tool_ = Tool::Select;
    bool snapOn_ = true, triplet_ = false, scaleHi_ = false, scaleSnap_ = false, ghost_ = false, copyDrag_ = false;
    double lastLen_ = 0;                // the length of the note last drawn, resized or clicked: what a new note gets (0: one grid step)
    int chordIdx_ = 0, moveAxis_ = 0, lastBlip_ = -1;
    struct Preview { int pitch; double start, dur; };
    std::vector<Preview> preview_;      // notes the draw, paint and chord tools will add on mouse-up
    long lastPaintCell_ = 0;
    std::set<project::Uid> eraseSet_;
    juce::Point<int> sliceA_, sliceB_;
    std::map<project::Uid, double> velPaint_;
    int lastVelX_ = 0, velDownX_ = 0;
    double lastVelV_ = 0, dStart_ = 0;
    std::array<bool, 12> scaleMask_{};
    int scaleRoot_ = 0;
    std::array<Chip, 6> toolChips_;
    Chip snapChip_{"Snap"}, scaleChip_{"Key"}, ghostChip_{"Ghost"}, editChip_{"Edit"}, chordChip_{"Major"};
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
