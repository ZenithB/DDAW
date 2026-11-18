// Colour, utility and pitch: Distortion, Bit Crusher, Chebyshev, Widener, Frequency Shifter, Auto-Tune.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "app/ui/FxViewsInternal.h"
#include "dsp/Scales.h"

namespace ddaw::ui {
namespace {

constexpr double kPi = std::numbers::pi;

// A transfer curve plot (-1..1 in, `yMax` out) beside the waveform it makes of a sine.
struct CurveAndWave {
    static juce::Rectangle<float> left(juce::Rectangle<float> r) { auto a = r.reduced(10, 24).withTrimmedBottom(8); return a.removeFromLeft(std::min(a.getHeight(), a.getWidth() * 0.5f)); }
    static juce::Rectangle<float> right(juce::Rectangle<float> r) { auto a = r.reduced(10, 24).withTrimmedBottom(8); return a.withTrimmedLeft(std::min(a.getHeight(), a.getWidth() * 0.5f) + 10); }
    static void paint(juce::Graphics& g, juce::Rectangle<float> r, const std::function<double(double)>& f, double yMax, juce::Colour c, double phase, juce::Colour dim) {
        const auto a = left(r), b = right(r);
        fx::frame(g, a); fx::frame(g, b);
        fx::hLine(g, a, a.getCentreY(), col::grid); g.setColour(col::grid); g.drawVerticalLine(int(a.getCentreX()), a.getY(), a.getBottom());
        g.setColour(col::dim.withAlpha(0.4f)); g.drawLine(a.getX(), fx::yLin(a, -1.0 * yMax / 1.0 * 1.0, -yMax, yMax) * 0 + a.getBottom(), a.getRight(), a.getY(), 1.0f);
        auto path = fx::curve(a, 200, [&](float u) { return fx::yLin(a, std::clamp(f(-1.0 + 2.0 * u), -yMax, yMax), -yMax, yMax); });
        g.setColour(c); g.strokePath(path, juce::PathStrokeType(2.0f));
        // input sine and the output it makes
        fx::hLine(g, b, b.getCentreY(), col::grid);
        auto in = fx::curve(b, 160, [&](float u) { return fx::yLin(b, std::sin(u * 4.0 * kPi + phase), -1.0, 1.0); });
        g.setColour(dim); g.strokePath(in, juce::PathStrokeType(1.2f));
        auto out = fx::curve(b, 160, [&](float u) { return fx::yLin(b, std::clamp(f(std::sin(u * 4.0 * kPi + phase)) / yMax, -1.0, 1.0), -1.0, 1.0); });
        g.setColour(c); g.strokePath(out, juce::PathStrokeType(2.0f));
    }
};

// ---- Distortion ----
class DistView final : public FxView {
public:
    DistView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(400);
        addKnob("amt", "Drive", fx::coral);
        addKnob("mix", "Mix");
    }

protected:
    static constexpr double kYMax = 0.4;
    static double shape(double x, double amt) { const double k = amt * 100.0; return ((3.0 + k) * x * (kPi / 9.0)) / (kPi + k * std::abs(x)); }
    bool animated() const override { return true; }
    void onTick(double dt) override { phase_ += dt * 1.2; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        const double amt = v("amt");
        CurveAndWave::paint(g, r, [amt](double x) { return shape(x, amt); }, kYMax, fx::coral, phase_, col::dim);
        const auto a = CurveAndWave::left(r);
        fx::node(g, nodePos(), fx::amber, drag_);
        fx::label(g, "drive " + juce::String(int(std::lround(amt * 100))) + "%   (input in grey, output in colour)", {r.getX() + 10, r.getY() + 4, r.getWidth() - 20, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
        (void)a;
    }
    juce::Point<float> nodePos() const { const auto a = CurveAndWave::left(display_); return {a.getX() + a.getWidth() * (0.5f + 0.5f * 0.25f), fx::yLin(a, shape(0.25, v("amt")), -kYMax, kYMax)}; }
    void pointer(const juce::MouseEvent& e, int ph) override {
        if (ph == 1) { drag_ = nodePos().getDistanceFrom(e.position) < 16.0f; if (drag_) begin(); return; }
        if (ph == 2 && drag_) {
            const double target = fx::valueAt(CurveAndWave::left(display_), e.position.y, -kYMax, kYMax);
            double lo = 0.0, hi = 1.0;
            for (int i = 0; i < 20; ++i) { const double mid = 0.5 * (lo + hi); (shape(0.25, mid) < target ? lo : hi) = mid; }
            set("amt", std::round(0.5 * (lo + hi) * 100.0) / 100.0);
            return;
        }
        if (ph == 3 && drag_) { drag_ = false; end(); }
    }
    double phase_ = 0.0;
    bool drag_ = false;
};

// ---- Bit Crusher ----
class CrushView final : public FxView {
public:
    CrushView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(400);
        addKnob("bits", "Bits", fx::coral);
        addKnob("mix", "Mix");
    }

protected:
    static double quant(double x, int bits) { const double step = std::pow(0.5, bits - 1); return std::floor(x / step + 0.5) * step; }
    bool animated() const override { return true; }
    void onTick(double dt) override { phase_ += dt * 1.2; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        const int bits = int(std::lround(v("bits")));
        CurveAndWave::paint(g, r, [bits](double x) { return quant(x, bits); }, 1.0, fx::coral, phase_, col::dim);
        fx::label(g, juce::String(bits) + " bit" + (bits == 1 ? "" : "s") + "   " + juce::String(1 << std::min(bits, 16)) + " levels   (drag across the picture)", {r.getX() + 10, r.getY() + 4, r.getWidth() - 20, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        if (ph == 1) { drag_ = display_.contains(e.position); if (drag_) begin(); }
        if ((ph == 1 || ph == 2) && drag_) set("bits", std::round(1.0 + std::clamp(double(e.position.x - display_.getX()) / display_.getWidth(), 0.0, 1.0) * 15.0));
        if (ph == 3 && drag_) { drag_ = false; end(); }
    }
    double phase_ = 0.0;
    bool drag_ = false;
};

// ---- Chebyshev: one harmonic, chosen by the order ----
class ChebyView final : public FxView {
public:
    ChebyView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(400);
        addKnob("order", "Order", fx::coral);
        addKnob("mix", "Mix");
    }

protected:
    juce::Rectangle<float> plot() const { return display_.reduced(10, 24).withTrimmedBottom(14); }
    float xOfHarm(int h) const { const auto p = plot(); return p.getX() + (float(h) - 0.5f) / 14.0f * p.getWidth(); }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        const auto p = plot();
        const int order = int(std::lround(v("order")));
        const double mix = v("mix"), wet = std::sin(mix * kPi / 2.0), dry = std::cos(mix * kPi / 2.0);
        fx::hLine(g, p, p.getBottom(), col::gridBar);
        for (int h = 1; h <= 14; ++h) {
            const bool is = h == order;
            const float w = p.getWidth() / 14.0f - 6.0f, x = xOfHarm(h) - w / 2;
            const double a = (h == 1 ? dry : 0.0) + (is ? wet : 0.0);
            if (is || h == 1) {
                g.setColour(is ? fx::coral : col::dim);
                g.fillRoundedRectangle(x, p.getBottom() - float(a) * p.getHeight(), w, float(a) * p.getHeight(), 2.0f);
            }
            g.setColour(is ? fx::amber.withAlpha(0.18f) : juce::Colours::transparentBlack); g.fillRect(x - 3, p.getY(), w + 6, p.getHeight());
            fx::label(g, juce::String(h), {x - 4, p.getBottom() + 1, w + 8, 11}, is ? fx::amber : col::faint, 9.5f, juce::Justification::centred, is);
        }
        fx::label(g, "order " + juce::String(order) + ": the " + juce::String(order) + (order == 2 ? "nd" : order == 3 ? "rd" : "th") + " harmonic (click a bar)", {r.getX() + 10, r.getY() + 4, r.getWidth() - 20, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        if (ph == 1) { drag_ = plot().expanded(0, 14).contains(e.position); if (drag_) begin(); }
        if ((ph == 1 || ph == 2) && drag_) set("order", std::floor(std::clamp(double(e.position.x - plot().getX()) / plot().getWidth(), 0.0, 0.999) * 14.0) + 1.0);
        if (ph == 3 && drag_) { drag_ = false; end(); }
    }
    bool drag_ = false;
};

// ---- Widener: the stereo image as an ellipse ----
class WidenView final : public FxView {
public:
    WidenView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(300);
        addKnob("width", "Width", fx::teal);
        claim({"width"});
    }

protected:
    bool animated() const override { return true; }
    void onTick(double dt) override { t_ += dt; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        const double w = v("width"), mid = 2.0 * (1.0 - w), side = 2.0 * w;
        const auto c = r.getCentre().translated(0, 6);
        const float R = std::min(r.getWidth(), r.getHeight()) * 0.36f;
        g.setColour(col::grid); g.drawEllipse(c.x - R, c.y - R, 2 * R, 2 * R, 1.0f);
        g.drawLine(c.x - R, c.y, c.x + R, c.y, 1.0f); g.drawLine(c.x, c.y - R, c.x, c.y + R, 1.0f);
        fx::label(g, "L", {c.x - R - 14, c.y - 6, 12, 12}, col::faint, 10.0f); fx::label(g, "R", {c.x + R + 3, c.y - 6, 12, 12}, col::faint, 10.0f);
        // the image: a cloud of points from a mono-heavy signal with a little side; mid goes up the middle, side across
        juce::Random rnd(7);
        for (int i = 0; i < 220; ++i) {
            const double m = (rnd.nextDouble() * 2 - 1) * 0.8 * mid * 0.5, s = (rnd.nextDouble() * 2 - 1) * 0.5 * side * 0.5 * (0.6 + 0.4 * std::sin(t_ * 1.5 + i));
            const float x = c.x + float(s) * R * 1.0f, y = c.y - float(m) * R * 1.0f;
            g.setColour(fx::teal.withAlpha(0.45f)); g.fillEllipse(x - 1.6f, y - 1.6f, 3.2f, 3.2f);
        }
        const float ex = float(std::min(side, 2.0)) * 0.5f * R, ey = float(std::min(mid, 2.0)) * 0.5f * R;
        g.setColour(fx::amber); g.drawEllipse(c.x - ex, c.y - ey, 2 * ex, 2 * ey, 1.5f);
        fx::node(g, {c.x + ex, c.y}, fx::amber, drag_, 6.0f);
        fx::label(g, w < 0.02 ? "mono" : w > 0.98 ? "side only" : juce::String(int(std::lround(w * 100))) + "%", {r.getX() + 10, r.getY() + 4, r.getWidth() - 20, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
        fx::label(g, "mid " + fx::dbText(20.0 * std::log10(std::max(mid, 1e-3)) - 0.0, 0) + "   side " + fx::dbText(20.0 * std::log10(std::max(side, 1e-3)), 0), {r.getX() + 10, r.getBottom() - 14, r.getWidth() - 20, 12}, col::dim, 10.0f);
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        if (ph == 1) { drag_ = display_.contains(e.position); if (drag_) begin(); }
        if ((ph == 1 || ph == 2) && drag_) {
            const float R = std::min(display_.getWidth(), display_.getHeight()) * 0.36f;
            set("width", std::round(std::clamp(double(std::abs(e.position.x - display_.getCentreX())) / (R * 1.0) , 0.0, 1.0) * 100.0) / 100.0);
        }
        if (ph == 3 && drag_) { drag_ = false; end(); }
        if (ph == 4) set("width", 0.5);
    }
    double t_ = 0.0;
    bool drag_ = false;
};

// ---- Frequency Shifter: every partial moves by the same number of Hz ----
class ShiftView final : public FxView {
public:
    ShiftView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(400);
        addKnob("amt", "Shift", fx::violet);
        addKnob("mix", "Mix");
        claim({"amt"});
    }

protected:
    static constexpr double kMax = 2000.0, kRoot = 110.0;
    juce::Rectangle<float> plot() const { return display_.reduced(10, 24).withTrimmedBottom(14); }
    float xOfHz(double hz) const { const auto p = plot(); return p.getX() + float(std::clamp(hz, 0.0, kMax) / kMax) * p.getWidth(); }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        const auto p = plot();
        for (double hz = 0; hz <= kMax; hz += 500) { g.setColour(col::grid.withAlpha(0.7f)); g.drawVerticalLine(int(xOfHz(hz)), p.getY(), p.getBottom()); fx::label(g, juce::String(int(hz)), {xOfHz(hz) + 2, p.getBottom() + 1, 30, 10}, col::faint, 9.0f); }
        const double s = v("amt");
        const float mid = p.getCentreY();
        for (int h = 1; h * kRoot <= kMax; ++h) {
            const double f = h * kRoot, a = 1.0 / std::pow(double(h), 0.7);
            g.setColour(col::dim.withAlpha(0.7f)); g.fillRect(xOfHz(f) - 1.5f, mid - float(a) * p.getHeight() * 0.45f, 3.0f, float(a) * p.getHeight() * 0.45f);
            if (f + s >= 0 && f + s <= kMax) { g.setColour(fx::violet); g.fillRect(xOfHz(f + s) - 1.5f, mid + 2, 3.0f, float(a) * p.getHeight() * 0.45f); }
        }
        g.setColour(col::dim.withAlpha(0.4f)); g.drawHorizontalLine(int(mid), p.getX(), p.getRight());
        fx::label(g, "in", {p.getX() + 2, mid - 12, 20, 10}, col::dim, 9.0f); fx::label(g, "out", {p.getX() + 2, mid + 2, 24, 10}, fx::violet, 9.0f);
        fx::label(g, (s > 0 ? "+" : "") + juce::String(int(std::lround(s))) + " Hz: every partial moves the same amount, so harmonics stop being harmonic", {r.getX() + 10, r.getY() + 4, r.getWidth() - 20, 14}, col::text, 10.5f, juce::Justification::centredLeft, true);
        fx::label(g, "drag sideways to shift", {r.getX() + 10, r.getBottom() - 12, r.getWidth() - 20, 10}, col::faint, 9.0f);
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        if (ph == 1) { drag_ = display_.contains(e.position); if (drag_) { begin(); a0_ = v("amt"); x0_ = e.position.x; } }
        if (ph == 2 && drag_) set("amt", std::round(a0_ + double(e.position.x - x0_) / plot().getWidth() * kMax * 0.4));
        if (ph == 3 && drag_) { drag_ = false; end(); }
        if (ph == 4) set("amt", 0.0);
    }
    bool drag_ = false;
    double a0_ = 0;
    float x0_ = 0;
};

// ---- Auto-Tune: the scale on a keyboard, and what the correction does to a drifting pitch ----
class TuneView final : public FxView {
public:
    TuneView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(420);
        addChoice("mode", "MODE");
        addChoice("root", "ROOT");
        addChoice("scale", "SCALE");
        addKnob("amount", "Amount", fx::teal);
        addKnob("speed", "Speed");
        addKnob("mix", "Mix");
    }

protected:
    int root() const { const int r = int(std::lround(v("root"))); return r >= 0 ? r : ((int(std::lround(model_.project().meta.root)) % 12) + 12) % 12; }
    std::vector<int> scaleIvs() const {
        const int s = int(std::lround(v("scale")));
        const auto& all = dsp::scales();
        if (s >= 0 && s < dsp::kScaleCount) return {all[size_t(s)].ivs.begin(), all[size_t(s)].ivs.end()};
        for (const auto& d : all) if (model_.project().meta.scale == d.id) return {d.ivs.begin(), d.ivs.end()};
        return {all[0].ivs.begin(), all[0].ivs.end()};
    }
    bool inScale(int pc) const {
        if (int(std::lround(v("mode"))) == 1) return true;
        for (int iv : scaleIvs()) if ((root() + iv) % 12 == pc) return true;
        return false;
    }
    double snap(double midi) const {                                  // nearest note of the scale
        double best = midi, bd = 1e9;
        for (int n = int(std::floor(midi)) - 3; n <= int(std::ceil(midi)) + 3; ++n) if (inScale(((n % 12) + 12) % 12) && std::abs(n - midi) < bd) { bd = std::abs(n - midi); best = n; }
        return best;
    }
    bool animated() const override { return true; }
    void onTick(double dt) override { t_ += dt * 0.35; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        auto keys = r.reduced(10, 22).removeFromTop(54);
        const int base = 48;                                          // C3, two octaves
        const float kw = keys.getWidth() / 14.0f;
        static const bool white[12] = {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1};
        int wi = 0;
        for (int n = 0; n < 24; ++n) {
            const int pc = (base + n) % 12;
            if (!white[pc]) continue;
            const juce::Rectangle<float> k(keys.getX() + float(wi) * kw, keys.getY(), kw - 1.0f, keys.getHeight());
            g.setColour(inScale(pc) ? (pc == root() ? fx::amber : fx::blue.withAlpha(0.85f)) : col::raised); g.fillRect(k);
            ++wi;
        }
        wi = 0;
        for (int n = 0; n < 24; ++n) {
            const int pc = (base + n) % 12;
            if (white[pc]) { ++wi; continue; }
            const juce::Rectangle<float> k(keys.getX() + float(wi) * kw - kw * 0.3f, keys.getY(), kw * 0.6f, keys.getHeight() * 0.6f);
            g.setColour(inScale(pc) ? (pc == root() ? fx::amber : fx::blue) : col::black); g.fillRect(k);
            g.setColour(col::black.withAlpha(0.5f)); g.drawRect(k, 1.0f);
        }
        // the correction: a drifting pitch (grey) and where the tuner puts it (colour), over a stretch of time
        auto p = r.reduced(10, 22).withTrimmedTop(64).withTrimmedBottom(14);
        fx::frame(g, p);
        const double amount = v("amount"), tau = std::max(v("speed"), 1.0) * 0.001;
        juce::Path in, out;
        double y = 0, lastNote = 0;
        const int steps = 240;
        const double span = 1.6;
        for (int i = 0; i <= steps; ++i) {
            const double tt = double(i) / steps * span + t_;
            const double midi = 60.0 + 3.0 * std::sin(tt * 2.1) + 1.1 * std::sin(tt * 7.3);
            const double target = midi + amount * (snap(midi) - midi);
            const double dt = span / steps;
            y = i == 0 ? target : y + (target - y) * (1.0 - std::exp(-dt / tau));
            const float x = p.getX() + float(i) / steps * p.getWidth();
            const float yi = fx::yLin(p, midi, 54.0, 66.0), yo = fx::yLin(p, y, 54.0, 66.0);
            if (i == 0) { in.startNewSubPath(x, yi); out.startNewSubPath(x, yo); } else { in.lineTo(x, yi); out.lineTo(x, yo); }
            lastNote = snap(midi);
        }
        for (int n = 54; n <= 66; ++n) if (inScale(((n % 12) + 12) % 12)) fx::hLine(g, p, fx::yLin(p, n, 54.0, 66.0), col::grid.withAlpha(n % 12 == root() ? 0.9f : 0.5f));
        g.setColour(col::dim); g.strokePath(in, juce::PathStrokeType(1.3f));
        g.setColour(fx::teal); g.strokePath(out, juce::PathStrokeType(2.2f));
        (void)lastNote;
        fx::label(g, juce::String(kNames[root()]) + (int(std::lround(v("mode"))) == 1 ? " chromatic" : " " + scaleName()) + "   amount " + juce::String(int(std::lround(amount * 100))) + "%   speed " + juce::String(int(std::lround(v("speed")))) + " ms",
                  {r.getX() + 10, r.getY() + 3, r.getWidth() - 20, 14}, col::text, 10.5f, juce::Justification::centredLeft, true);
    }
    juce::String scaleName() const {
        const int s = int(std::lround(v("scale")));
        if (s >= 0 && s < dsp::kScaleCount) return dsp::scales()[size_t(s)].name;
        for (const auto& d : dsp::scales()) if (model_.project().meta.scale == d.id) return d.name;
        return dsp::scales()[0].name;
    }
    static constexpr const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    double t_ = 0.0;
};

}  // namespace

std::unique_ptr<FxView> makeColourView(const std::string& t, app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) {
    if (t == "dist") return std::make_unique<DistView>(m, u, i, h);
    if (t == "crush") return std::make_unique<CrushView>(m, u, i, h);
    if (t == "cheby") return std::make_unique<ChebyView>(m, u, i, h);
    if (t == "widen") return std::make_unique<WidenView>(m, u, i, h);
    if (t == "shift") return std::make_unique<ShiftView>(m, u, i, h);
    if (t == "autotune") return std::make_unique<TuneView>(m, u, i, h);
    return nullptr;
}

}  // namespace ddaw::ui
