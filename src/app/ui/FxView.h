#pragma once
// Graphic panels for the audio effects (ARCH 27). Every effect is shown as a picture of what it does - a frequency response you drag,
// a transfer curve, a timeline of echoes, an LFO scope - with the few controls that do not belong in the picture (a knob, a
// segmented selector) in a column beside it. This is the shared part: the base class holds the parameter values, writes edits to the
// document (one undoable command per change, gestures grouped), lays out the control column and offers a few drawing helpers.
// The per-family pictures live in FxViews*.cpp and are found by FxView::make().
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app/model/AppModel.h"
#include "app/ui/Widgets.h"

namespace ddaw::ui {

// A stepped parameter chosen by name: a row of segments for up to four values, a drop-down menu for more.
class Choice : public juce::Component {
public:
    Choice(const ParamSpec& spec, juce::String caption, juce::Colour on);
    std::function<void(int)> onPick;       // the new value (min + index)
    void setValue(double v);
    double value() const { return min_ + sel_; }
    int preferredWidth() const;
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool isMenu() const { return names_.size() > 4; }
    int count() const { return names_.size(); }
    juce::Rectangle<int> segment(int i) const;     // for tests
    static constexpr int kCaptionW = 46, kH = 20;

private:
    int indexAt(juce::Point<int> p) const;
    juce::StringArray names_;
    juce::String caption_;
    juce::Colour on_;
    int min_ = 0, sel_ = 0;
};

class FxView : public juce::Component {
public:
    FxView(app::AppModel& m, project::Uid uid, const app::DeviceInfo& info, int availH);
    ~FxView() override;

    // The panel for an effect, or nullptr when the effect has no picture (hosted plugins, unknown types).
    static std::unique_ptr<FxView> make(app::AppModel& m, project::Uid uid, const app::DeviceInfo& info, int availH);

    int preferredWidth() const;
    void sync(const project::DeviceSpec& d);      // follow the document
    void tick();                                  // follow the engine and animate
    void resized() override;
    void paint(juce::Graphics&) override;

    double value(const std::string& key) const;
    bool covers(const std::string& key) const { return covered_.count(key) > 0; }
    const std::set<std::string>& covered() const { return covered_; }
    juce::Component* control(const std::string& key);      // the knob or choice for a key, or nullptr
    juce::Rectangle<float> displayBounds() const { return display_; }

protected:
    // ---- setup (derived constructors) ----
    void setDisplayWidth(int w) { displayW_ = w; }
    Knob* addKnob(const char* key, const char* label, juce::Colour c = juce::Colour(0xff5aa9e6));
    Choice* addChoice(const char* key, const char* caption);
    // The tempo-synced rate of the modulation effects: a Sync/Low/High selector, then the division, the Hz knob or the fast-Hz knob.
    void addRate();
    // The picture edits these keys itself (drag gestures), so no knob is needed.
    void claim(std::initializer_list<const char*> keys) { for (auto* k : keys) covered_.insert(k); }

    // ---- values and edits ----
    double v(const char* key) const { return value(key); }
    void set(const char* key, double value);
    void setMany(const std::string& label, const std::vector<std::pair<const char*, double>>& changes);
    void begin() { model_.beginGesture("parameter"); }
    void end() { model_.endGesture(); }
    double bpm() const { return model_.project().meta.bpm; }
    // The modulation rate in Hz, from rateMode / rate / rateHi / rateSync and the project tempo.
    double rateHz() const;
    const ParamSpec* spec(const char* key) const;

    // ---- the picture ----
    virtual void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) = 0;
    virtual void onSync() {}
    virtual void onTick(double /*dtSeconds*/) {}
    virtual bool animated() const { return false; }
    virtual void pointer(const juce::MouseEvent&, int /*phase 0 move 1 down 2 drag 3 up 4 double*/) {}
    void mouseMove(const juce::MouseEvent& e) override { pointer(e, 0); }
    void mouseDown(const juce::MouseEvent& e) override { pointer(e, 1); }
    void mouseDrag(const juce::MouseEvent& e) override { pointer(e, 2); }
    void mouseUp(const juce::MouseEvent& e) override { pointer(e, 3); }
    void mouseDoubleClick(const juce::MouseEvent& e) override { pointer(e, 4); }

    app::AppModel& model_;
    project::Uid uid_;
    const app::DeviceInfo& info_;
    juce::Rectangle<float> display_;
    double time_ = 0.0;       // seconds of animation clock

private:
    void layoutControls();
    int knobRows() const;
    std::map<std::string, size_t> index_;
    std::vector<double> vals_;
    std::vector<std::unique_ptr<Knob>> knobs_;
    std::vector<std::unique_ptr<Choice>> choices_;
    std::vector<std::string> knobKeys_, choiceKeys_;
    std::set<std::string> covered_;
    Choice* rateMode_ = nullptr;
    Choice* rateSync_ = nullptr;
    Knob* rateLow_ = nullptr;
    Knob* rateHigh_ = nullptr;
    int displayW_ = 340, availH_ = 200;
    double lastMs_ = 0.0;
    int ctlW_ = 200;
};

// ---- drawing helpers shared by the pictures ----
namespace fx {
inline const juce::Colour blue{0xff5aa9e6}, green{0xff7bc47f}, violet{0xffc990e8}, amber{0xfff2c14e}, coral{0xffee7b6f}, teal{0xff4fd0c4};
void frame(juce::Graphics& g, juce::Rectangle<float> r);                       // the dark plot background
float xLog(juce::Rectangle<float> r, double hz, double lo = 20.0, double hi = 20000.0);
double hzAt(juce::Rectangle<float> r, float x, double lo = 20.0, double hi = 20000.0);
float yLin(juce::Rectangle<float> r, double v, double lo, double hi);       // v = hi at the top
double valueAt(juce::Rectangle<float> r, float y, double lo, double hi);
void freqGrid(juce::Graphics& g, juce::Rectangle<float> r, double lo = 20.0, double hi = 20000.0, bool labels = true);
void hLine(juce::Graphics& g, juce::Rectangle<float> r, float y, juce::Colour c, float thick = 1.0f);
void label(juce::Graphics& g, juce::String text, juce::Rectangle<float> r, juce::Colour c = col::dim, float size = 10.0f, juce::Justification j = juce::Justification::centredLeft, bool bold = false);
void node(juce::Graphics& g, juce::Point<float> c, juce::Colour colr, bool hot, float radius = 6.5f);
// A polyline of f(x) over the rectangle's width, y from `toY`.
juce::Path curve(juce::Rectangle<float> r, int steps, const std::function<float(float /*x01*/)>& yOf);
juce::String hzText(double hz);
juce::String dbText(double db, int dp = 1);
// Magnitude (dB) of an RBJ / Web Audio biquad at `hz`.
double biquadDb(int kind /*0 peak 1 low shelf 2 high shelf*/, double f0, double gainDb, double q, double hz, double sr = 48000.0);
}  // namespace fx

}  // namespace ddaw::ui
