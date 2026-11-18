#pragma once
// The chord resonator's panel (ARCH 26): not a row of knobs but a picture of the chord.
//
//   The plot. The horizontal axis is the interval above the tonic, -12..+36 semitones, one column per semitone, so the picture is the
//   same whatever note is played; the vertical axis is level in dB. Each active filter is a coloured bead at its interval and its
//   level (boost + its own level), with its response curve under it, drawn from the same formula the device uses (a cascade of
//   band-pass sections: Q and steepness change the curve you see). A white outline is the combined response, a dashed line is the
//   original, which sinks as the boost rises. Drag a bead sideways to change its interval and up or down to change its level;
//   double-click empty space (or click the ruler) to add a filter, double-click a bead to remove it. The diamond at the tonic is the
//   optional resonance on the note itself (click to switch it on, double-click to switch it off, drag for its level).
//   The readout names what you point at (interval, note, frequency) and the chord that is sounding comes from the live tonic.
//
//   The column on the right has the rest: the source and steepness selectors, and knobs for boost, Q, the exciter, the final
//   saturation, mix, glide and the audio-mode threshold.
#include <array>
#include <memory>
#include <vector>

#include "app/model/AppModel.h"
#include "app/ui/Widgets.h"

namespace ddaw::ui {

class ChordResonatorView : public juce::Component {
public:
    static constexpr int kWidth = 664;
    static constexpr int kFilters = 6;

    ChordResonatorView(app::AppModel& m, project::Uid uid, const app::DeviceInfo& info);

    void sync(const project::DeviceSpec& d);   // follow the document
    void tick();                               // follow the engine (the tonic being played)

    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    // Geometry the tests drive: the bead of filter k (0..5), or the tonic's diamond (-1), in this component's coordinates.
    juce::Point<float> beadCentre(int k) const;
    juce::Rectangle<float> plotBounds() const { return plot_; }
    juce::Rectangle<float> rulerBounds() const { return ruler_; }
    float xOfSemitone(int semis) const;
    float yOfDb(double db) const;
    int activeFilters() const;
    double value(const char* key) const;
    juce::Component* sourceChip(int i) { return srcChips_[size_t(i)].get(); }
    juce::Component* steepChip(int i) { return steepChips_[size_t(i)].get(); }
    juce::Component* tonicChip() { return tonicChip_.get(); }
    juce::Component* knob(const char* key);
    juce::String readout() const { return readout_; }

private:
    enum P : int { Source, Filters, Tonic, TonicLevel, I1, I2, I3, I4, I5, I6, L1, L2, L3, L4, L5, L6, Boost, Q, Steep, Excite, Sat, Mix, Glide, Thresh, kNumP };

    void set(int idx, double v);
    void setGroup(const std::string& label, const std::vector<std::pair<int, double>>& changes);
    int hit(juce::Point<float> p) const;               // the bead or diamond under p: 0..5, -1 for the tonic, -2 none
    int semitoneAt(float x) const;
    double levelAt(float y, double boost, int idx) const;
    juce::String intervalName(int semis) const;
    juce::String noteName(double midi) const;
    void addFilterAt(int semis);
    void removeFilter(int k);
    void updateReadout(int bead);
    void drawPlot(juce::Graphics&);
    void drawRuler(juce::Graphics&);
    void refreshChips();

    app::AppModel& model_;
    project::Uid uid_;
    const app::DeviceInfo& info_;
    std::array<double, kNumP> v_{};
    juce::Rectangle<float> plot_, ruler_, hint_, controls_;
    std::array<std::unique_ptr<Chip>, 3> srcChips_;
    std::array<std::unique_ptr<Chip>, 4> steepChips_;
    std::unique_ptr<Chip> tonicChip_;
    std::vector<std::unique_ptr<Knob>> knobs_;
    std::vector<int> knobParam_;
    float tonic_ = -1.0f;
    int hover_ = -2, drag_ = -2, rulerHover_ = -1000;
    bool dragMoved_ = false;
    juce::String readout_;
};

}  // namespace ddaw::ui
