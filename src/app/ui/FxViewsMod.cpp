// The modulation family: Chorus, Phaser, Vibrato, Tremolo, Auto Pan. An LFO scope with a moving cursor, drawn the way each effect uses it.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "app/ui/FxViewsInternal.h"
#include "dsp/Lfo.h"

namespace ddaw::ui {
namespace {

class ModView final : public FxView {
public:
    enum class Kind { Chorus, Phaser, Vibrato, Tremolo, AutoPan };
    ModView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h, Kind k) : FxView(m, u, i, h), kind_(k) {
        setDisplayWidth(380);
        addRate();
        if (k == Kind::Phaser) addKnob("octaves", "Octaves");
        else addKnob("depth", "Depth");
        if (k != Kind::AutoPan) addKnob("mix", "Mix");
        addKnob("phase", "Phase", fx::violet);
        addChoice("shape", "SHAPE");
        if (k != Kind::Phaser) claim({"depth"});
    }

protected:
    static constexpr double kTwoPi = 2.0 * std::numbers::pi;
    double maxDepth() const { const auto* s = spec("depth"); return s ? double(s->max) : 1.0; }
    juce::Rectangle<float> plot() const { return display_.reduced(10, 24).withTrimmedBottom(kind_ == Kind::AutoPan ? 34 : 8); }
    // the traces: value in 0..1 (0 at the bottom) of trace n at phase theta, for a given depth
    double trace(int n, double theta, double depth) const {
        // the device's own LFO: its shape and its phase offset (a vibrato starts a quarter cycle in, a tremolo's channels are 60 and 120 degrees out)
        const double cyc = theta / kTwoPi + v("phase") / 360.0;
        const uint32_t sh = uint32_t(std::clamp(int(std::lround(v("shape"))), 0, 6));
        const auto lfo = [&](double extra) { return double(dsp::lfoShapeValue(sh, cyc + extra)); };
        switch (kind_) {
            case Kind::Chorus: return 0.5 + 0.5 * depth * (n == 0 ? lfo(0.0) : -lfo(0.0));
            case Kind::Vibrato: return 0.5 + 0.5 * depth / maxDepth() * lfo(-0.25);
            case Kind::Tremolo: return 0.5 * (1.0 - depth * lfo(n == 0 ? 60.0 / 360.0 : 120.0 / 360.0));
            case Kind::AutoPan: return 0.5 + 0.5 * depth * lfo(0.0);
            case Kind::Phaser: {
                const double lo = 350.0, hi = 350.0 * std::pow(2.0, v("octaves")), s = 0.5 * (1.0 + (n == 0 ? lfo(0.0) : -lfo(0.0)));
                return std::log((lo + (hi - lo) * s) / 100.0) / std::log(8000.0 / 100.0);
            }
        }
        return 0.5;
    }
    juce::Colour traceColour(int n) const { return n == 0 ? fx::blue : fx::coral; }
    int traces() const { return kind_ == Kind::Vibrato || kind_ == Kind::AutoPan ? 1 : 2; }
    bool animated() const override { return true; }
    void onTick(double dt) override { phase_ = std::fmod(phase_ + dt * std::min(rateHz(), 1.5) * kTwoPi, 2.0 * kTwoPi); }
    // where the depth handle sits: the first extreme of the first trace
    double nodeTheta() const {
        double best = 0.0, bd = -1.0;
        for (int i = 0; i < 360; ++i) { const double th = kTwoPi * (double(i) + 0.5) / 360.0, d = std::abs(trace(0, th, maxDepth()) - 0.5); if (d > bd + 1e-9) { bd = d; best = th; } }
        return best;
    }
    juce::Point<float> depthNode() const {
        const auto p = plot();
        return {p.getX() + p.getWidth() * float(nodeTheta() / (2.0 * kTwoPi)), fx::yLin(p, trace(0, nodeTheta(), v("depth")), 0.0, 1.0)};
    }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        const auto p = plot();
        for (int c = 0; c <= 4; ++c) { g.setColour(col::grid.withAlpha(c % 2 ? 0.4f : 0.8f)); g.drawVerticalLine(int(p.getX() + p.getWidth() * float(c) / 4.0f), p.getY(), p.getBottom()); }
        fx::hLine(g, p, p.getCentreY(), col::grid.withAlpha(0.8f));
        for (int n = 0; n < traces(); ++n) {
            auto path = fx::curve(p, 200, [&](float u) { return fx::yLin(p, trace(n, double(u) * 2.0 * kTwoPi, v("depth")), 0.0, 1.0); });
            if (n == 0) { juce::Path fill = path; fill.lineTo(p.getRight(), p.getCentreY()); fill.lineTo(p.getX(), p.getCentreY()); fill.closeSubPath(); g.setColour(fx::blue.withAlpha(0.12f)); g.fillPath(fill); }
            g.setColour(traceColour(n)); g.strokePath(path, juce::PathStrokeType(2.0f));
        }
        const float cx = p.getX() + float(phase_ / (2.0 * kTwoPi)) * p.getWidth();
        g.setColour(col::text.withAlpha(0.5f)); g.drawVerticalLine(int(cx), p.getY(), p.getBottom());
        for (int n = 0; n < traces(); ++n) fx::node(g, {cx, fx::yLin(p, trace(n, phase_, v("depth")), 0.0, 1.0)}, traceColour(n), false, 4.0f);
        if (kind_ != Kind::Phaser) fx::node(g, depthNode(), fx::amber, drag_);
        const juce::String rate = int(std::lround(v("rateMode"))) == 0 ? juce::String(app::formatParam(*spec("rateSync"), v("rateSync"))) : juce::String(rateHz(), rateHz() < 10 ? 2 : 1) + " Hz";
        const char* what = kind_ == Kind::Chorus ? "voice delay (L and R in antiphase)" : kind_ == Kind::Phaser ? "notch sweep (Hz, L and R)" : kind_ == Kind::Vibrato ? "pitch wobble" : kind_ == Kind::Tremolo ? "level (L and R out of phase)" : "pan (left to right)";
        fx::label(g, rate + "   " + what, {r.getX() + 10, r.getY() + 4, r.getWidth() - 20, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
        fx::label(g, kind_ == Kind::Phaser ? "drag the picture sideways for phase" : "amber point: depth   drag the picture sideways: phase", {r.getX() + 10, r.getBottom() - 12, r.getWidth() - 20, 10}, col::faint, 9.0f);
        if (kind_ == Kind::AutoPan) {                                  // where the sound sits now
            auto bar = juce::Rectangle<float>(r.getX() + 24, r.getBottom() - 26, r.getWidth() - 48, 8);
            g.setColour(col::black.withAlpha(0.6f)); g.fillRoundedRectangle(bar, 4.0f);
            fx::label(g, "L", {bar.getX() - 14, bar.getY() - 2, 12, 12}, col::dim, 10.0f); fx::label(g, "R", {bar.getRight() + 3, bar.getY() - 2, 12, 12}, col::dim, 10.0f);
            fx::node(g, {bar.getCentreX() + float(v("depth") * std::sin(phase_)) * bar.getWidth() * 0.5f, bar.getCentreY()}, fx::amber, false, 6.0f);
        }
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        if (kind_ == Kind::Phaser) return;
        if (ph == 1) {
            drag_ = depthNode().getDistanceFrom(e.position) < 16.0f;
            dragPhase_ = !drag_ && plot().contains(e.position);
            if (drag_ || dragPhase_) { begin(); p0_ = v("phase"); x0_ = e.position.x; }
            return;
        }
        if (ph == 2 && dragPhase_) {                               // slide the picture sideways: the phase follows (two cycles span the plot)
            const double deg = p0_ - double(e.position.x - x0_) / (plot().getWidth() / 2.0f) * 360.0;
            set("phase", std::round(std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0)));
            return;
        }
        if (ph == 2 && drag_) {
            const auto p = plot();
            const double th = nodeTheta();
            const double y0 = fx::yLin(p, trace(0, th, 0.0), 0, 1), y1 = fx::yLin(p, trace(0, th, maxDepth()), 0, 1);
            set("depth", std::round(std::clamp((e.position.y - y0) / (y1 - y0), 0.0, 1.0) * maxDepth() * 100.0) / 100.0);
            return;
        }
        if (ph == 3 && (drag_ || dragPhase_)) { drag_ = false; dragPhase_ = false; end(); }
    }
    Kind kind_;
    double phase_ = 0.0, p0_ = 0.0;
    float x0_ = 0.0f;
    bool drag_ = false, dragPhase_ = false;
};

}  // namespace

std::unique_ptr<FxView> makeModView(const std::string& t, app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) {
    using K = ModView::Kind;
    if (t == "chorus") return std::make_unique<ModView>(m, u, i, h, K::Chorus);
    if (t == "phaser") return std::make_unique<ModView>(m, u, i, h, K::Phaser);
    if (t == "vib") return std::make_unique<ModView>(m, u, i, h, K::Vibrato);
    if (t == "trem") return std::make_unique<ModView>(m, u, i, h, K::Tremolo);
    if (t == "autopan") return std::make_unique<ModView>(m, u, i, h, K::AutoPan);
    return nullptr;
}

}  // namespace ddaw::ui
