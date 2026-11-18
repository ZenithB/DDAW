// The time family: Delay, Ping Pong, Reverb, Plate. A timeline of the echoes, and the decay of the room.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "app/ui/FxViewsInternal.h"

namespace ddaw::ui {
namespace {

// ---- Delay and Ping Pong: the echoes laid out in beats ----
class DelayView final : public FxView {
public:
    DelayView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h, bool pingpong) : FxView(m, u, i, h), pp_(pingpong) {
        setDisplayWidth(420);
        addChoice("time", "TIME");
        addKnob("fb", "Feedback");
        addKnob("mix", "Mix");
        addKnob("tone", "Tone", fx::amber);          // a low-pass in the feedback path: each repeat darker
        addKnob("duck", "Duck", fx::amber);          // the echoes sink while you play and return in the gaps
        addKnob("wobble", "Wobble", fx::amber);      // tape drift of the delay time
    }

protected:
    juce::Rectangle<float> plot() const { return display_.reduced(10, 22).withTrimmedBottom(14); }
    double dtBeats() const { static const double b[] = {0.125, 0.25, 0.5, 0.75, 1.0, 2.0}; return b[std::clamp(int(std::lround(v("time"))), 0, 5)]; }
    static constexpr double kWindow = 4.0;                              // beats shown
    double wet() const { return std::sin(v("mix") * std::numbers::pi / 2.0); }
    double dry() const { return std::cos(v("mix") * std::numbers::pi / 2.0); }
    float xOfBeat(double b) const { const auto p = plot(); return p.getX() + float(b / kWindow) * p.getWidth(); }
    // echo n (1-based): the level, and for ping pong the lane (0 left, 1 right)
    double level(int n) const { return wet() * std::pow(v("fb"), n - 1); }
    float yOfLevel(double a, int lane) const {
        const auto p = plot();
        if (!pp_) return p.getBottom() - float(a) * p.getHeight();
        const float half = p.getHeight() * 0.5f - 2.0f;
        return lane == 0 ? p.getCentreY() - 2.0f - float(a) * half : p.getCentreY() + 2.0f + float(a) * half;
    }
    bool animated() const override { return true; }
    void onTick(double dt) override { head_ = std::fmod(head_ + dt * bpm() / 60.0, kWindow); }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        const auto p = plot();
        for (int b = 0; b <= int(kWindow); ++b) {
            g.setColour(b % 4 == 0 ? col::gridBar : col::grid.withAlpha(0.7f)); g.drawVerticalLine(int(xOfBeat(b)), p.getY(), p.getBottom());
            fx::label(g, juce::String(b + 1), {xOfBeat(b) - 8, p.getBottom() + 1, 16, 10}, col::faint, 9.0f, juce::Justification::centred);
        }
        if (pp_) { fx::hLine(g, p, p.getCentreY(), col::gridBar); fx::label(g, "L", {p.getX() + 2, p.getY(), 10, 10}, col::faint, 9.0f); fx::label(g, "R", {p.getX() + 2, p.getBottom() - 10, 10, 10}, col::faint, 9.0f); }
        const double dt = dtBeats();
        // the dry hit
        const auto dryTop = yOfLevel(dry(), 0);
        g.setColour(col::dim); g.fillRect(xOfBeat(0) - 2, dryTop, 4.0f, (pp_ ? p.getCentreY() - 2.0f : p.getBottom()) - dryTop);
        const float hx = xOfBeat(head_);
        for (int n = 1; n < 40; ++n) {
            const double b = n * dt;
            if (b > kWindow + 1e-6) break;
            const double a = level(n);
            if (a < 0.004) break;
            const int lane = pp_ ? (n % 2 == 1 ? 0 : 1) : 0;
            const float x = xOfBeat(b), top = yOfLevel(a, lane), base = pp_ ? (lane == 0 ? p.getCentreY() - 2.0f : p.getCentreY() + 2.0f) : p.getBottom();
            const float lit = std::clamp(1.0f - std::abs(hx - x) / 40.0f, 0.0f, 1.0f);
            g.setColour((pp_ ? (lane == 0 ? fx::blue : fx::coral) : fx::blue).withAlpha(0.55f + 0.45f * lit));
            g.fillRoundedRectangle(x - 3, std::min(top, base), 6.0f, std::abs(base - top), 2.0f);
            if (n <= 2) fx::node(g, {x, top}, n == 1 ? fx::amber : fx::violet, drag_ == n, 5.5f);
        }
        g.setColour(col::text.withAlpha(0.4f)); g.drawVerticalLine(int(hx), p.getY(), p.getBottom());
        fx::label(g, juce::String(app::formatParam(*spec("time"), v("time"))) + "  feedback " + juce::String(int(std::lround(v("fb") * 100))) + "%  wet " + juce::String(int(std::lround(v("mix") * 100))) + "%",
                  {r.getX() + 10, r.getY() + 4, r.getWidth() - 20, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
        fx::label(g, "amber: wet level   violet: feedback", {r.getX() + 10, r.getBottom() - 12, r.getWidth() - 20, 10}, col::faint, 9.0f);
    }
    int hit(juce::Point<float> pt) const {
        for (int n = 1; n <= 2; ++n) {
            const double a = level(n);
            if (juce::Point<float>(xOfBeat(n * dtBeats()), yOfLevel(a, pp_ ? (n % 2 == 1 ? 0 : 1) : 0)).getDistanceFrom(pt) < 14.0f) return n;
        }
        return 0;
    }
    double levelFromY(float y, int n) const {
        const auto p = plot();
        if (!pp_) return std::clamp(double((p.getBottom() - y) / p.getHeight()), 0.0, 1.0);
        const int lane = n % 2 == 1 ? 0 : 1;
        const float half = p.getHeight() * 0.5f - 2.0f;
        return std::clamp(double(lane == 0 ? (p.getCentreY() - 2.0f - y) / half : (y - p.getCentreY() - 2.0f) / half), 0.0, 1.0);
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        if (ph == 1) { drag_ = hit(e.position); if (drag_) begin(); return; }
        if (ph == 2 && drag_ == 1) { set("mix", std::round(std::asin(std::clamp(levelFromY(e.position.y, 1), 0.0, 1.0)) * 2.0 / std::numbers::pi * 100.0) / 100.0); return; }
        if (ph == 2 && drag_ == 2) { const double w = wet(); if (w > 0.01) set("fb", std::round(std::clamp(levelFromY(e.position.y, 2) / w, 0.0, 0.85) * 100.0) / 100.0); return; }
        if (ph == 3 && drag_) { drag_ = 0; end(); }
    }
    bool pp_;
    double head_ = 0.0;
    int drag_ = 0;
};

// ---- Reverb and Plate: the decay ----
class RoomView final : public FxView {
public:
    RoomView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h, bool plate) : FxView(m, u, i, h), plate_(plate) {
        setDisplayWidth(plate ? 460 : 380);
        if (plate) { addKnob("predelay", "Pre-delay"); addKnob("damp", "Damp"); claim({"decay"}); addKnob("decay", "Decay"); }
        else { addKnob("size", "Size"); addKnob("predelay", "Pre-delay"); addKnob("tone", "Tone", fx::amber); }
        addKnob("lowcut", "Low cut", fx::amber);      // a high-pass on the tail: no mud
        addKnob("duck", "Duck", fx::amber);           // the tail sinks while you play and blooms in the gaps
        addKnob("mix", "Mix");
    }

protected:
    const char* decayKey() const { return plate_ ? "decay" : "size"; }
    double rt60() const { return v(decayKey()); }
    double tMax() const { return plate_ ? 8.0 : 10.0; }
    juce::Rectangle<float> decayPlot() const {
        auto r = display_.reduced(10, 22).withTrimmedBottom(14);
        return plate_ ? r.withTrimmedRight(r.getWidth() * 0.38f) : r;
    }
    juce::Rectangle<float> dampPlot() const { auto r = display_.reduced(10, 22).withTrimmedBottom(14); return r.withTrimmedLeft(r.getWidth() * 0.66f); }
    double pre() const { return v("predelay"); }
    float xOfT(double t) const { const auto p = decayPlot(); return p.getX() + float(t / tMax()) * p.getWidth(); }
    float yOfDb(double db) const { return fx::yLin(decayPlot(), db, -72.0, 0.0); }
    bool animated() const override { return true; }
    void onTick(double dt) override { head_ = std::fmod(head_ + dt * 0.5 * (4.0 / std::max(0.5, rt60())) * 1.0, 1.4); }
    double lowpassDb(double hz) const { const double x = hz / v("damp"); return -10.0 * std::log10(1.0 + x * x * x * x); }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        const auto p = decayPlot();
        for (double t = 0; t <= tMax() + 1e-9; t += 2.0) { g.setColour(col::grid.withAlpha(0.7f)); g.drawVerticalLine(int(xOfT(t)), p.getY(), p.getBottom()); fx::label(g, juce::String(int(t)) + " s", {xOfT(t) + 1, p.getBottom() + 1, 24, 10}, col::faint, 9.0f); }
        for (double db = -60; db <= 0; db += 20) { fx::hLine(g, p, yOfDb(db), db == -60 ? fx::amber.withAlpha(0.35f) : col::grid.withAlpha(0.7f)); fx::label(g, juce::String(int(db)), {p.getX() + 2, yOfDb(db) - 10, 24, 10}, col::faint, 9.0f); }
        // the tail: a straight decay in dB, with the grain of a diffuse tail on top (the same every time)
        const double t60 = rt60();
        auto path = fx::curve(p, 220, [&](float u) {
            const double t = u * tMax();
            double db = t < pre() ? -80.0 : -60.0 * (t - pre()) / t60;
            if (t >= pre()) db += 2.2 * std::sin(t * 61.0) * std::sin(t * 23.0 + 1.0) * std::min(1.0, (t - pre()) * 12.0);
            return yOfDb(std::clamp(db, -72.0, 0.0));
        });
        juce::Path fill = path; fill.lineTo(p.getRight(), p.getBottom()); fill.lineTo(p.getX(), p.getBottom()); fill.closeSubPath();
        g.setColour(fx::blue.withAlpha(0.18f)); g.fillPath(fill);
        g.setColour(fx::blue); g.strokePath(path, juce::PathStrokeType(1.6f));
        const float hx = xOfT(std::min(head_, 1.0) * tMax());
        g.setColour(col::text.withAlpha(0.35f)); g.drawVerticalLine(int(hx), p.getY(), p.getBottom());
        const juce::Point<float> n(xOfT(std::min(pre() + t60, tMax())), yOfDb(-60.0));
        fx::node(g, n, fx::amber, drag_);
        fx::label(g, "RT60 " + juce::String(t60, 2) + " s" + (plate_ ? "   pre-delay " + juce::String(int(std::lround(pre() * 1000))) + " ms" : juce::String()), {r.getX() + 10, r.getY() + 4, r.getWidth() * 0.65f, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
        if (plate_) {                                                   // the damping filter on the tail
            const auto d = dampPlot();
            fx::frame(g, d);
            fx::freqGrid(g, d, 100.0, 20000.0, false);
            auto dp = fx::curve(d, 80, [&](float u) { return fx::yLin(d, std::clamp(lowpassDb(fx::hzAt(d, d.getX() + u * d.getWidth(), 100.0, 20000.0)), -24.0, 0.0), -24.0, 0.0); });
            g.setColour(fx::coral); g.strokePath(dp, juce::PathStrokeType(1.8f));
            fx::node(g, {fx::xLog(d, v("damp"), 100.0, 20000.0), fx::yLin(d, lowpassDb(v("damp")), -24.0, 0.0)}, fx::coral, drag_ == 2, 5.5f);
            fx::label(g, "damping " + fx::hzText(v("damp")) + " Hz", {d.getX() - 4, d.getY() - 18, d.getWidth() + 8, 14}, col::dim, 10.0f, juce::Justification::centred);
        }
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        const auto p = decayPlot();
        if (ph == 1) {
            drag_ = juce::Point<float>(xOfT(std::min(pre() + rt60(), tMax())), yOfDb(-60.0)).getDistanceFrom(e.position) < 16.0f ? 1 : 0;
            if (plate_ && !drag_) { const auto d = dampPlot(); if (juce::Point<float>(fx::xLog(d, v("damp"), 100.0, 20000.0), fx::yLin(d, lowpassDb(v("damp")), -24.0, 0.0)).getDistanceFrom(e.position) < 16.0f) drag_ = 2; }
            if (drag_) begin();
            return;
        }
        if (ph == 2 && drag_ == 1) { set(decayKey(), std::round(std::max(0.0, double(e.position.x - p.getX()) / p.getWidth() * tMax() - pre()) * 20.0) / 20.0); return; }
        if (ph == 2 && drag_ == 2) { set("damp", std::round(fx::hzAt(dampPlot(), e.position.x, 100.0, 20000.0))); return; }
        if (ph == 3 && drag_) { drag_ = 0; end(); }
    }
    bool plate_;
    double head_ = 0.0;
    int drag_ = 0;
};

}  // namespace

std::unique_ptr<FxView> makeTimeView(const std::string& t, app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) {
    if (t == "delay") return std::make_unique<DelayView>(m, u, i, h, false);
    if (t == "pingpong") return std::make_unique<DelayView>(m, u, i, h, true);
    if (t == "reverb") return std::make_unique<RoomView>(m, u, i, h, false);
    if (t == "plate") return std::make_unique<RoomView>(m, u, i, h, true);
    return nullptr;
}

}  // namespace ddaw::ui
