// The dynamics family: Compressor, Opto, Multiband, Gate, Ducker. Transfer curves you drag, with the engine's gain reduction shown live.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "app/ui/FxViewsInternal.h"
#include "dsp/CompKernel.h"

namespace ddaw::ui {
namespace {

// A vertical gain-reduction bar (0 at the top, `full` dB at the bottom).
void grBar(juce::Graphics& g, juce::Rectangle<float> r, float grDb, float full = 24.0f) {
    g.setColour(col::black.withAlpha(0.5f)); g.fillRoundedRectangle(r, 2.0f);
    const float f = std::clamp(-grDb / full, 0.0f, 1.0f);
    g.setColour(f > 0.5f ? col::rec : col::meterMid);
    g.fillRoundedRectangle(r.withHeight(r.getHeight() * f), 2.0f);
}

// ---- Compressor ----
class CompView final : public FxView {
public:
    CompView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(330);
        addKnob("attack", "Attack");
        addKnob("release", "Release");
        claim({"thresh", "ratio"});
    }

protected:
    static constexpr double kLo = -60.0, kHi = 12.0;
    juce::Rectangle<float> plot() const { return display_.reduced(6, 6).withTrimmedRight(18).withTrimmedBottom(14); }
    juce::Point<float> at(const juce::Rectangle<float>& p, double inDb, double outDb) const {
        return {fx::yLin(p, 0, 0, 1) * 0 + p.getX() + float((inDb - kLo) / (0.0 - kLo)) * p.getWidth(), fx::yLin(p, outDb, kLo, kHi)};
    }
    bool animated() const override { return true; }
    void onTick(double) override { gr_ += (model_.reductionDb(uid_, 0) - gr_) * 0.5f; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        const auto p = plot();
        for (double db = -60; db <= 0; db += 12) {
            const float x = at(p, db, 0).x, y = fx::yLin(p, db, kLo, kHi);
            g.setColour(col::grid.withAlpha(0.7f)); g.drawVerticalLine(int(x), p.getY(), p.getBottom()); g.drawHorizontalLine(int(y), p.getX(), p.getRight());
            fx::label(g, juce::String(int(db)), {x - 12, p.getBottom() + 1, 24, 10}, col::faint, 9.0f, juce::Justification::centred);
        }
        const dsp::CompKernel::StaticCurve c(float(v("thresh")), float(v("ratio")));
        // the diagonal (no change) and the curve
        g.setColour(col::dim.withAlpha(0.5f)); g.drawLine(at(p, kLo, kLo).x, at(p, kLo, kLo).y, at(p, 0, 0).x, at(p, 0, 0).y, 1.0f);
        auto path = fx::curve(p, 160, [&](float u) { return fx::yLin(p, std::clamp<double>(c.outDb(float(kLo + u * (0 - kLo))), kLo, kHi), kLo, kHi); });
        juce::Path fill = path; fill.lineTo(p.getRight(), p.getBottom()); fill.lineTo(p.getX(), p.getBottom()); fill.closeSubPath();
        g.setColour(fx::blue.withAlpha(0.15f)); g.fillPath(fill);
        g.setColour(fx::blue); g.strokePath(path, juce::PathStrokeType(2.0f));
        // threshold, and the knee (a Web Audio compressor's knee is 30 dB above the threshold)
        const float tx = at(p, v("thresh"), 0).x;
        const float dashes[] = {3.0f, 3.0f};
        g.setColour(fx::amber.withAlpha(0.7f)); g.drawDashedLine(juce::Line<float>(tx, p.getY(), tx, p.getBottom()), dashes, 2, 1.0f);
        const float kx = at(p, std::min(0.0, v("thresh") + 30.0), 0).x;
        g.setColour(col::dim.withAlpha(0.4f)); g.drawDashedLine(juce::Line<float>(kx, p.getY(), kx, p.getBottom()), dashes, 2, 1.0f);
        fx::label(g, "knee", {kx - 20, p.getY() + 1, 40, 10}, col::faint, 9.0f, juce::Justification::centred);
        // the live operating point, found from the reduction the engine reports
        if (gr_ < -0.2f) {
            double lo = v("thresh"), hi = 0.0;
            for (int i = 0; i < 24; ++i) { const double mid = 0.5 * (lo + hi); (c.outDb(float(mid)) - mid - c.makeupDb > gr_ ? lo : hi) = mid; }
            const auto d = at(p, lo, c.outDb(float(lo)));
            g.setColour(fx::coral.withAlpha(0.5f)); g.drawLine(d.x, d.y, d.x, at(p, lo, lo + c.makeupDb).y, 3.0f);
            fx::node(g, d, fx::coral, false, 5.0f);
        }
        fx::node(g, {tx, at(p, v("thresh"), c.outDb(float(v("thresh")))).y}, fx::amber, hover_ == 1 || drag_ == 1);
        const auto rn = at(p, 0, c.outDb(0.0f));
        fx::node(g, {p.getRight() - 1, std::clamp(rn.y, p.getY() + 6, p.getBottom() - 6)}, fx::violet, hover_ == 2 || drag_ == 2, 5.5f);
        fx::label(g, juce::String(int(v("thresh"))) + " dB   " + juce::String(v("ratio"), 1) + ":1", {p.getX() + 6, p.getY() + 3, 140, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
        grBar(g, {r.getRight() - 14, p.getY(), 8, p.getHeight()}, gr_);
        fx::label(g, "GR", {r.getRight() - 22, p.getBottom() + 1, 24, 10}, col::faint, 9.0f, juce::Justification::centred);
    }
    int hit(juce::Point<float> pt) const {
        const auto p = plot();
        const dsp::CompKernel::StaticCurve c(float(v("thresh")), float(v("ratio")));
        if (juce::Point<float>(at(p, v("thresh"), c.outDb(float(v("thresh")))).x, at(p, v("thresh"), c.outDb(float(v("thresh")))).y).getDistanceFrom(pt) < 14.0f) return 1;
        const auto rn = at(p, 0, c.outDb(0.0f));
        if (juce::Point<float>(p.getRight() - 1, std::clamp(rn.y, p.getY() + 6, p.getBottom() - 6)).getDistanceFrom(pt) < 14.0f) return 2;
        return 0;
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        if (ph == 0) { const int h = hit(e.position); if (h != hover_) { hover_ = h; repaint(); } return; }
        const auto p = plot();
        if (ph == 1) { drag_ = hit(e.position); if (drag_) begin(); return; }
        if (ph == 2 && drag_ == 1) { set("thresh", std::round(kLo + (e.position.x - p.getX()) / p.getWidth() * (0 - kLo))); return; }
        if (ph == 2 && drag_ == 2) {                        // the output at 0 dB in decides the ratio: find the ratio that puts the curve there
            const double target = fx::valueAt(p, e.position.y, kLo, kHi);
            double lo = 1.0, hi = 20.0;
            for (int i = 0; i < 18; ++i) { const double mid = 0.5 * (lo + hi); (dsp::CompKernel::StaticCurve(float(v("thresh")), float(mid)).outDb(0.0f) > target ? lo : hi) = mid; }
            set("ratio", std::round(0.5 * (lo + hi) * 10.0) / 10.0);
            return;
        }
        if (ph == 3 && drag_) { drag_ = 0; end(); return; }
        if (ph == 4) { set("thresh", -20.0); set("ratio", 4.0); }
    }
    float gr_ = 0.0f;
    int hover_ = 0, drag_ = 0;
};

// ---- Opto: a needle meter and two dials ----
class OptoView final : public FxView {
public:
    OptoView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(300);
        addChoice("mode", "MODE");
        addKnob("reduction", "Peak Red.", fx::amber);
        addKnob("gain", "Gain");
    }

protected:
    bool animated() const override { return true; }
    void onTick(double) override { gr_ += (model_.reductionDb(uid_, 0) - gr_) * 0.25f; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        auto face = r.reduced(10, 10);
        fillRounded(g, face, juce::Colour(0xff2b2a24), 6.0f);                         // a warm VU face
        const juce::Point<float> pivot(face.getCentreX(), face.getBottom() - 8);
        const float rad = std::min(face.getWidth() * 0.42f, face.getHeight() * 0.78f);
        const float a0 = -0.85f, a1 = 0.85f;                                           // needle sweep (rad from vertical)
        g.setFont(uiFont(10.0f, true));
        static const int marks[] = {0, -3, -6, -10, -15, -20};
        for (int db : marks) {
            const float u = std::clamp(-float(db) / 20.0f, 0.0f, 1.0f), a = a0 + u * (a1 - a0);
            const juce::Point<float> p0(pivot.x + std::sin(a) * rad, pivot.y - std::cos(a) * rad), p1(pivot.x + std::sin(a) * (rad - 9), pivot.y - std::cos(a) * (rad - 9));
            g.setColour(db <= -10 ? fx::coral : juce::Colour(0xffe9dfc3)); g.drawLine(p0.x, p0.y, p1.x, p1.y, 2.0f);
            fx::label(g, juce::String(-db), {pivot.x + std::sin(a) * (rad + 10) - 10, pivot.y - std::cos(a) * (rad + 10) - 6, 20, 12}, juce::Colour(0xffe9dfc3), 10.0f, juce::Justification::centred);
        }
        const float u = std::clamp(-gr_ / 20.0f, 0.0f, 1.0f), a = a0 + u * (a1 - a0);
        g.setColour(juce::Colour(0xff1a1a18)); g.drawLine(pivot.x, pivot.y, pivot.x + std::sin(a) * (rad - 4), pivot.y - std::cos(a) * (rad - 4), 2.2f);
        g.fillEllipse(pivot.x - 5, pivot.y - 5, 10, 10);
        fx::label(g, "GAIN REDUCTION  dB", {face.getX() + 8, face.getBottom() - 20, face.getWidth() - 16, 12}, juce::Colour(0xffe9dfc3), 10.0f, juce::Justification::centredLeft, true);
        fx::label(g, juce::String(int(std::lround(v("mode"))) == 0 ? "COMPRESS" : "LIMIT"), {face.getX() + 8, face.getBottom() - 20, face.getWidth() - 16, 12}, juce::Colour(0xffb5aa8c), 10.0f, juce::Justification::centredRight, true);
    }
    float gr_ = 0.0f;
};

// ---- Multiband: three cards between two draggable crossovers ----
class MbcompView final : public FxView {
public:
    MbcompView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(470);
        addChoice("mode", "MODE");
        addKnob("attack", "Attack");
        addKnob("release", "Release");
        for (int b = 0; b < 3; ++b) { t_[b] = "b" + std::to_string(b) + "_thresh"; r_[b] = "b" + std::to_string(b) + "_ratio"; g_[b] = "b" + std::to_string(b) + "_gain"; claim({t_[b].c_str(), r_[b].c_str(), g_[b].c_str()}); }
        claim({"xlo", "xhi"});
    }

protected:
    juce::Rectangle<float> inner() const { return display_.reduced(6, 6).withTrimmedBottom(16); }
    juce::Rectangle<float> card(int b) const {
        const auto r = inner();
        const float x1 = fx::xLog(r, v("xlo"), 20.0, 20000.0), x2 = fx::xLog(r, v("xhi"), 20.0, 20000.0);
        const float xs[4] = {r.getX(), x1, x2, r.getRight()};
        return juce::Rectangle<float>(xs[b], r.getY(), xs[b + 1] - xs[b], r.getHeight()).reduced(2, 0);
    }
    struct Mini { juce::Rectangle<float> plot, gain, gr; };
    Mini mini(int b) const {
        auto c = card(b).withTrimmedTop(16);
        Mini m;
        m.gr = c.removeFromBottom(10).reduced(2, 1);
        m.gain = c.removeFromRight(14).reduced(2, 2);
        m.plot = c.reduced(2, 2);
        return m;
    }
    double outFor(int b, double in) const {
        const double T = v(t_[b].c_str()), R = v(r_[b].c_str()), G = v(g_[b].c_str());
        const bool expand = int(std::lround(v("mode"))) == 1;
        double out = in;
        if (!expand && in > T) out = T + (in - T) / R;
        if (expand && in < T) out = T + (in - T) * R;
        return out + G;
    }
    bool animated() const override { return true; }
    void onTick(double) override { for (int b = 0; b < 3; ++b) gr_[b] += (model_.reductionDb(uid_, b) - gr_[b]) * 0.4f; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        static const char* names[] = {"LOW", "MID", "HIGH"};
        const double edges[4] = {20.0, v("xlo"), v("xhi"), 20000.0};
        for (int b = 0; b < 3; ++b) {
            const auto c = card(b);
            fillRounded(g, c, col::panel2.withAlpha(0.8f), 4.0f);
            fx::label(g, juce::String(names[b]) + "  " + fx::hzText(edges[b]) + "-" + fx::hzText(edges[b + 1]), {c.getX() + 4, c.getY() + 1, c.getWidth() - 6, 14}, trackColour(size_t(b)), 10.0f, juce::Justification::centredLeft, true);
            const auto m = mini(b);
            fx::frame(g, m.plot);
            g.setColour(col::dim.withAlpha(0.4f)); g.drawLine(m.plot.getX(), m.plot.getBottom(), m.plot.getRight(), m.plot.getY(), 1.0f);
            auto path = fx::curve(m.plot, 60, [&](float u) { return fx::yLin(m.plot, std::clamp(outFor(b, -60.0 + u * 60.0), -60.0, 12.0), -60.0, 12.0); });
            g.setColour(trackColour(size_t(b))); g.strokePath(path, juce::PathStrokeType(1.8f));
            const juce::Point<float> n(m.plot.getX() + float((v(t_[b].c_str()) + 60.0) / 60.0) * m.plot.getWidth(), fx::yLin(m.plot, std::clamp(outFor(b, v(t_[b].c_str())), -60.0, 12.0), -60.0, 12.0));
            fx::node(g, n, trackColour(size_t(b)), drag_ == b * 10 + 1, 5.0f);
            // the band's make-up gain: a slider along the card's right edge
            g.setColour(col::black.withAlpha(0.5f)); g.fillRoundedRectangle(m.gain, 2.0f);
            const float gy = fx::yLin(m.gain, v(g_[b].c_str()), -24.0, 24.0);
            g.setColour(trackColour(size_t(b))); g.fillRect(m.gain.getX(), gy - 2, m.gain.getWidth(), 4.0f);
            g.setColour(col::dim.withAlpha(0.6f)); g.drawHorizontalLine(int(fx::yLin(m.gain, 0, -24, 24)), m.gain.getX(), m.gain.getRight());
            // reduction
            g.setColour(col::black.withAlpha(0.5f)); g.fillRoundedRectangle(m.gr, 2.0f);
            g.setColour(gr_[b] < -9.0f ? col::rec : col::meterMid);
            g.fillRoundedRectangle(m.gr.withWidth(m.gr.getWidth() * std::clamp(-gr_[b] / 18.0f, 0.0f, 1.0f)), 2.0f);
            fx::label(g, juce::String(int(v(t_[b].c_str()))) + " dB  " + juce::String(v(r_[b].c_str()), 1) + ":1  " + fx::dbText(v(g_[b].c_str()), 0), {m.plot.getX() + 3, m.plot.getY() + 1, m.plot.getWidth(), 10}, col::text, 9.0f);
        }
        // the crossovers
        for (int x = 0; x < 2; ++x) {
            const float cx = fx::xLog(inner(), x ? v("xhi") : v("xlo"));
            g.setColour(fx::amber.withAlpha(drag_ == 100 + x ? 1.0f : 0.8f));
            g.fillRoundedRectangle(cx - 20, inner().getBottom() + 2, 40, 12, 3.0f);
            fx::label(g, fx::hzText(x ? v("xhi") : v("xlo")), {cx - 20, inner().getBottom() + 2, 40, 12}, col::black, 9.5f, juce::Justification::centred, true);
        }
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        const auto p = e.position;
        if (ph == 1) {
            drag_ = -1;
            for (int x = 0; x < 2; ++x) if (std::abs(p.x - fx::xLog(inner(), x ? v("xhi") : v("xlo"))) < 20.0f && p.y > inner().getBottom() - 4) drag_ = 100 + x;
            for (int b = 0; b < 3 && drag_ < 0; ++b) {
                const auto m = mini(b);
                if (m.gain.expanded(3).contains(p)) drag_ = b * 10 + 2;
                else if (m.plot.contains(p)) { drag_ = b * 10 + 1; r0_ = v(r_[b].c_str()); y0_ = p.y; }
            }
            if (drag_ >= 0) begin();
            return;
        }
        if (ph == 2 && drag_ >= 100) { set(drag_ == 100 ? "xlo" : "xhi", std::round(fx::hzAt(inner(), p.x))); return; }
        if (ph == 2 && drag_ >= 0) {
            const int b = drag_ / 10;
            const auto m = mini(b);
            if (drag_ % 10 == 2) set(g_[b].c_str(), std::round(fx::valueAt(m.gain, p.y, -24.0, 24.0) * 2.0) / 2.0);
            else { set(t_[b].c_str(), std::round(-60.0 + (p.x - m.plot.getX()) / m.plot.getWidth() * 60.0)); set(r_[b].c_str(), std::round((r0_ + double(y0_ - p.y) / m.plot.getHeight() * 19.0) * 10.0) / 10.0); }
            return;
        }
        if (ph == 3 && drag_ >= 0) { drag_ = -1; end(); }
        if (ph == 4) for (int b = 0; b < 3; ++b) if (mini(b).plot.contains(p)) { set(g_[b].c_str(), 0.0); set(r_[b].c_str(), 2.0); set(t_[b].c_str(), -24.0); }
    }
    std::string t_[3], r_[3], g_[3];
    float gr_[3] = {0, 0, 0};
    int drag_ = -1;
    double r0_ = 2.0;
    float y0_ = 0;
};

// ---- Gate: the expander curve, with the open and close thresholds ----
class GateView final : public FxView {
public:
    GateView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(330);
        addChoice("key", "KEY");
        addChoice("det", "DETECT");
        addChoice("listen", "LISTEN");
        addChoice("inverse", "INVERSE");
        for (auto [k, l] : {std::pair{"ratio", "Ratio"}, {"knee", "Knee"}, {"range", "Range"}, {"up", "Open +"}, {"dn", "Close -"}, {"attack", "Attack"}, {"hold", "Hold"}, {"release", "Release"},
                            {"look", "Look"}, {"hpf", "Key HP"}, {"lpf", "Key LP"}, {"mix", "Mix"}, {"gain", "Gain"}})
            addKnob(k, l);
        claim({"thresh"});
    }

protected:
    static constexpr double kLo = -80.0;
    juce::Rectangle<float> plot() const { return display_.reduced(6, 6).withTrimmedRight(18).withTrimmedBottom(14); }
    double redAt(double x) const {                          // reduction (dB >= 0) at x dB past the threshold, as the device computes it
        const double k = std::max(0.0, v("knee")), ratio = std::max(1.0, v("ratio")), rng = v("range");
        if (ratio >= 19.95) return -rng;
        double red;
        if (x >= k * 0.5) red = 0.0;
        else if (x <= -k * 0.5) red = (ratio - 1.0) * -x;
        else { const double d = x - k * 0.5; red = (ratio - 1.0) * d * d / (2.0 * k); }
        return std::min(red, -rng);
    }
    double outDb(double in) const { return in - redAt(in - v("thresh")); }
    float xOf(double in) const { const auto p = plot(); return p.getX() + float((in - kLo) / -kLo) * p.getWidth(); }
    bool animated() const override { return true; }
    void onTick(double) override { gr_ += (model_.reductionDb(uid_, 0) - gr_) * 0.5f; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        const auto p = plot();
        for (double db = -80; db <= 0; db += 20) {
            const float x = xOf(db), y = fx::yLin(p, db, kLo, 0.0);
            g.setColour(col::grid.withAlpha(0.7f)); g.drawVerticalLine(int(x), p.getY(), p.getBottom()); g.drawHorizontalLine(int(y), p.getX(), p.getRight());
            fx::label(g, juce::String(int(db)), {x - 12, p.getBottom() + 1, 24, 10}, col::faint, 9.0f, juce::Justification::centred);
        }
        g.setColour(col::dim.withAlpha(0.5f)); g.drawLine(p.getX(), p.getBottom(), p.getRight(), p.getY(), 1.0f);
        auto path = fx::curve(p, 200, [&](float u) { return fx::yLin(p, std::clamp(outDb(kLo + double(u) * -kLo), kLo, 0.0), kLo, 0.0); });
        juce::Path fill = path; fill.lineTo(p.getRight(), p.getBottom()); fill.lineTo(p.getX(), p.getBottom()); fill.closeSubPath();
        g.setColour(fx::green.withAlpha(0.15f)); g.fillPath(fill);
        g.setColour(fx::green); g.strokePath(path, juce::PathStrokeType(2.0f));
        // hysteresis: opens above thresh + up, closes below thresh + dn
        const float dashes[] = {3.0f, 3.0f};
        const double openAt = v("thresh") + v("up"), closeAt = v("thresh") + v("dn");
        g.setColour(fx::green.withAlpha(0.7f)); g.drawDashedLine(juce::Line<float>(xOf(openAt), p.getY(), xOf(openAt), p.getBottom()), dashes, 2, 1.0f);
        g.setColour(fx::coral.withAlpha(0.7f)); g.drawDashedLine(juce::Line<float>(xOf(closeAt), p.getY(), xOf(closeAt), p.getBottom()), dashes, 2, 1.0f);
        fx::label(g, "open", {xOf(openAt) + 2, p.getY() + 1, 30, 10}, fx::green, 9.0f);
        fx::label(g, "close", {xOf(closeAt) - 32, p.getY() + 1, 30, 10}, fx::coral, 9.0f, juce::Justification::centredRight);
        const juce::Point<float> n(xOf(v("thresh")), fx::yLin(p, std::clamp(outDb(v("thresh")), kLo, 0.0), kLo, 0.0));
        fx::node(g, n, fx::amber, drag_);
        const bool open = gr_ > -3.0f;
        g.setColour(open ? col::play : col::rec.withAlpha(0.8f)); g.fillEllipse(p.getX() + 6, p.getY() + 5, 8, 8);
        fx::label(g, open ? "OPEN" : "CLOSED", {p.getX() + 18, p.getY() + 3, 60, 12}, col::text, 10.0f, juce::Justification::centredLeft, true);
        fx::label(g, juce::String(int(v("thresh"))) + " dB", {p.getX() + 80, p.getY() + 3, 60, 12}, col::dim, 10.0f);
        grBar(g, {r.getRight() - 14, p.getY(), 8, p.getHeight()}, gr_, 60.0f);
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        const auto p = plot();
        if (ph == 1) { drag_ = juce::Point<float>(xOf(v("thresh")), fx::yLin(p, std::clamp(outDb(v("thresh")), kLo, 0.0), kLo, 0.0)).getDistanceFrom(e.position) < 16.0f; if (drag_) begin(); return; }
        if (ph == 2 && drag_) { set("thresh", std::round(kLo + (e.position.x - p.getX()) / p.getWidth() * -kLo)); return; }
        if (ph == 3 && drag_) { drag_ = false; end(); }
    }
    float gr_ = 0.0f;
    bool drag_ = false;
};

// ---- Ducker: the gain over one cycle ----
class DuckView final : public FxView {
public:
    DuckView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(330);
        addChoice("rate", "RATE");
        addKnob("amount", "Amount");
        addKnob("curve", "Curve");
    }

protected:
    juce::Rectangle<float> plot() const { return display_.reduced(10, 22).withTrimmedBottom(8).withTrimmedRight(14); }
    double expo() const { return 0.35 + v("curve") * 1.6; }
    double gainAt(double ph) const { return (1.0 - v("amount")) + v("amount") * std::pow(ph, expo()); }
    double beats() const { static const double b[] = {2.0, 1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25}; return b[std::clamp(int(std::lround(v("rate"))), 0, 5)]; }
    bool animated() const override { return true; }
    void onTick(double dt) override { phase_ = std::fmod(phase_ + dt * bpm() / 60.0 / beats(), 1.0); gr_ += (model_.reductionDb(uid_, 0) - gr_) * 0.5f; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        const auto p = plot();
        for (int i = 0; i <= 4; ++i) { const float x = p.getX() + p.getWidth() * float(i) / 4.0f; g.setColour(col::grid.withAlpha(0.7f)); g.drawVerticalLine(int(x), p.getY(), p.getBottom()); }
        for (double gdb : {1.0, 0.5, 0.0}) { fx::hLine(g, p, fx::yLin(p, gdb, 0, 1), col::grid.withAlpha(0.7f)); }
        auto path = fx::curve(p, 120, [&](float u) { return fx::yLin(p, gainAt(u), 0, 1); });
        juce::Path fill = path; fill.lineTo(p.getRight(), p.getBottom()); fill.lineTo(p.getX(), p.getBottom()); fill.closeSubPath();
        g.setColour(fx::blue.withAlpha(0.2f)); g.fillPath(fill);
        g.setColour(fx::blue); g.strokePath(path, juce::PathStrokeType(2.2f));
        const float px = p.getX() + float(phase_) * p.getWidth();
        g.setColour(col::text.withAlpha(0.6f)); g.drawVerticalLine(int(px), p.getY(), p.getBottom());
        fx::node(g, {px, fx::yLin(p, gainAt(phase_), 0, 1)}, fx::coral, false, 4.0f);
        fx::node(g, {p.getX(), fx::yLin(p, gainAt(0), 0, 1)}, fx::amber, drag_ == 1, 6.5f);
        fx::node(g, {p.getX() + p.getWidth() * 0.5f, fx::yLin(p, gainAt(0.5), 0, 1)}, fx::violet, drag_ == 2, 6.0f);
        grBar(g, {r.getRight() - 14, p.getY(), 8, p.getHeight()}, gr_);                         // the dip the engine is making right now
        fx::label(g, "GR", {r.getRight() - 22, p.getBottom() + 1, 24, 10}, col::faint, 9.0f, juce::Justification::centred);
        fx::label(g, "dip " + juce::String(int(std::lround(v("amount") * 100))) + "%   shape " + juce::String(v("curve"), 2), {r.getX() + 10, r.getY() + 4, r.getWidth() - 20, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
        fx::label(g, "one cycle - drag the amber point for depth, the violet one for shape", {r.getX() + 10, r.getBottom() - 12, r.getWidth() - 20, 10}, col::faint, 9.0f);
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        const auto p = plot();
        if (ph == 1) {
            const juce::Point<float> a(p.getX(), fx::yLin(p, gainAt(0), 0, 1)), b(p.getX() + p.getWidth() * 0.5f, fx::yLin(p, gainAt(0.5), 0, 1));
            drag_ = a.getDistanceFrom(e.position) < 16.0f ? 1 : b.getDistanceFrom(e.position) < 16.0f ? 2 : 0;
            if (drag_) begin();
            return;
        }
        if (ph == 2 && drag_ == 1) { set("amount", std::round((1.0 - fx::valueAt(p, e.position.y, 0, 1)) * 100.0) / 100.0); return; }
        if (ph == 2 && drag_ == 2) {
            const double a = v("amount"), g = std::clamp(fx::valueAt(p, e.position.y, 0, 1), 1.0 - a + 0.001, 0.999);
            if (a > 0.01) set("curve", std::round(((std::log((g - (1.0 - a)) / a) / std::log(0.5)) - 0.35) / 1.6 * 100.0) / 100.0);
            return;
        }
        if (ph == 3 && drag_) { drag_ = 0; end(); }
    }
    double phase_ = 0.0;
    float gr_ = 0.0f;
    int drag_ = 0;
};

}  // namespace

std::unique_ptr<FxView> makeDynView(const std::string& t, app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) {
    if (t == "comp") return std::make_unique<CompView>(m, u, i, h);
    if (t == "opto") return std::make_unique<OptoView>(m, u, i, h);
    if (t == "mbcomp") return std::make_unique<MbcompView>(m, u, i, h);
    if (t == "gate") return std::make_unique<GateView>(m, u, i, h);
    if (t == "duck") return std::make_unique<DuckView>(m, u, i, h);
    return nullptr;
}

}  // namespace ddaw::ui
