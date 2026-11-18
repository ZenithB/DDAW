// The EQ and filter family: EQ Three, EQ Seven, Filter, Auto Filter. Each is a frequency-response plot you drag.
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

#include "app/ui/FxViewsInternal.h"
#include "dsp/Lfo.h"

namespace ddaw::ui {
namespace {

// ---- EQ Three: three gains, one per band (the crossovers are fixed at 400 Hz and 2.5 kHz) ----
class EqThreeView final : public FxView {
public:
    EqThreeView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(420);
        claim({"low", "mid", "high"});
    }

protected:
    static constexpr double kLo = 400.0, kHi = 2500.0, kRange = 15.0;
    const char* key(int b) const { return b == 0 ? "low" : b == 1 ? "mid" : "high"; }
    double hzOfBand(int b) const { return b == 0 ? 120.0 : b == 1 ? 1000.0 : 6000.0; }
    double responseDb(double f) const {
        const double a = std::pow(f / kLo, 4), b = std::pow(f / kHi, 4);
        const double wl = 1.0 / (1.0 + a), wh = b / (1.0 + b), wm = std::max(0.0, 1.0 - wl - wh);
        const double amp = wl * std::pow(10.0, v("low") / 20.0) + wm * std::pow(10.0, v("mid") / 20.0) + wh * std::pow(10.0, v("high") / 20.0);
        return 20.0 * std::log10(std::max(amp, 1e-6));
    }
    int bandAt(float x) const { const double f = fx::hzAt(plot(), x); return f < kLo ? 0 : f < kHi ? 1 : 2; }
    juce::Rectangle<float> plot() const { return display_.reduced(0, 0).withTrimmedBottom(0); }

    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        fx::freqGrid(g, r);
        for (double db = -12; db <= 12; db += 6) {
            const float y = fx::yLin(r, db, -kRange, kRange);
            fx::hLine(g, r, y, db == 0 ? col::gridBar : col::grid.withAlpha(0.6f));
            fx::label(g, (db > 0 ? "+" : "") + juce::String(int(db)), {r.getX() + 3, y - 10, 24, 10}, col::faint, 9.0f);
        }
        for (double xo : {kLo, kHi}) {
            const float x = fx::xLog(r, xo);
            g.setColour(col::dim.withAlpha(0.5f));
            const float dashes[] = {3.0f, 3.0f};
            g.drawDashedLine(juce::Line<float>(x, r.getY(), x, r.getBottom()), dashes, 2, 1.0f);
        }
        const float zeroY = fx::yLin(r, 0, -kRange, kRange);
        auto path = fx::curve(r, 240, [&](float u) { return fx::yLin(r, std::clamp(responseDb(fx::hzAt(r, r.getX() + u * r.getWidth())), -kRange, kRange), -kRange, kRange); });
        juce::Path fill = path;
        fill.lineTo(r.getRight(), zeroY); fill.lineTo(r.getX(), zeroY); fill.closeSubPath();
        g.setColour(fx::blue.withAlpha(0.22f)); g.fillPath(fill);
        g.setColour(fx::blue); g.strokePath(path, juce::PathStrokeType(2.0f));
        static const char* names[] = {"LOW", "MID", "HIGH"};
        for (int b = 0; b < 3; ++b) {
            const juce::Point<float> p(fx::xLog(r, hzOfBand(b)), fx::yLin(r, std::clamp(v(key(b)), -kRange, kRange), -kRange, kRange));
            fx::node(g, p, trackColour(size_t(b)), drag_ == b || hover_ == b);
            fx::label(g, names[b], {p.x - 24, r.getBottom() - 24, 48, 11}, trackColour(size_t(b)), 10.0f, juce::Justification::centred, true);
            fx::label(g, fx::dbText(v(key(b))), {p.x - 30, p.y + (v(key(b)) >= 0 ? 9.0f : -22.0f), 60, 12}, col::text, 10.0f, juce::Justification::centred);
        }
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        const auto r = display_;
        if (ph == 0) { const int b = r.contains(e.position) ? bandAt(e.position.x) : -1; if (b != hover_) { hover_ = b; repaint(); } return; }
        if (ph == 1) { if (!r.contains(e.position)) return; drag_ = bandAt(e.position.x); begin(); }
        if ((ph == 1 || ph == 2) && drag_ >= 0)
            set(key(drag_), std::round(fx::valueAt(r, e.position.y, -kRange, kRange) * 2.0) / 2.0);
        if (ph == 3 && drag_ >= 0) { drag_ = -1; end(); }
        if (ph == 4) { const int b = bandAt(e.position.x); set(key(b), 0.0); }
    }
    int drag_ = -1, hover_ = -1;
};

// ---- EQ Seven: a low shelf, five peaking bands and a high shelf, each a node ----
class Eq7View final : public FxView {
public:
    Eq7View(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(520);
        for (int b = 0; b < 7; ++b) { fk_[b] = "b" + std::to_string(b) + "_freq"; gk_[b] = "b" + std::to_string(b) + "_gain"; qk_[b] = "b" + std::to_string(b) + "_q"; claim({fk_[b].c_str(), gk_[b].c_str(), qk_[b].c_str()}); }
    }

protected:
    static constexpr double kRange = 18.0;
    int kind(int b) const { return b == 0 ? 1 : b == 6 ? 2 : 0; }
    double bandDb(int b, double hz) const { return fx::biquadDb(kind(b), v(fk_[b].c_str()), v(gk_[b].c_str()), v(qk_[b].c_str()), hz); }
    double totalDb(double hz) const { double s = 0; for (int b = 0; b < 7; ++b) s += bandDb(b, hz); return s; }
    juce::Point<float> nodePos(int b) const { return {fx::xLog(display_, v(fk_[b].c_str())), fx::yLin(display_, std::clamp(v(gk_[b].c_str()), -kRange, kRange), -kRange, kRange)}; }
    int hit(juce::Point<float> p) const {
        int best = -1; float bd = 14.0f * 14.0f;
        for (int b = 0; b < 7; ++b) { const float d = nodePos(b).getDistanceSquaredFrom(p); if (d <= bd) { bd = d; best = b; } }
        return best;
    }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        fx::freqGrid(g, r);
        for (double db = -12; db <= 12; db += 6) {
            const float y = fx::yLin(r, db, -kRange, kRange);
            fx::hLine(g, r, y, db == 0 ? col::gridBar : col::grid.withAlpha(0.6f));
            fx::label(g, (db > 0 ? "+" : "") + juce::String(int(db)), {r.getX() + 3, y - 10, 24, 10}, col::faint, 9.0f);
        }
        const float zeroY = fx::yLin(r, 0, -kRange, kRange);
        g.saveState();
        g.reduceClipRegion(r.toNearestInt());
        if (sel_ >= 0) {                                    // the selected band on its own
            auto p = fx::curve(r, 200, [&](float u) { return fx::yLin(r, std::clamp(bandDb(sel_, fx::hzAt(r, r.getX() + u * r.getWidth())), -kRange, kRange), -kRange, kRange); });
            g.setColour(trackColour(size_t(sel_)).withAlpha(0.7f)); g.strokePath(p, juce::PathStrokeType(1.2f));
        }
        auto path = fx::curve(r, 300, [&](float u) { return fx::yLin(r, std::clamp(totalDb(fx::hzAt(r, r.getX() + u * r.getWidth())), -kRange, kRange), -kRange, kRange); });
        juce::Path fill = path;
        fill.lineTo(r.getRight(), zeroY); fill.lineTo(r.getX(), zeroY); fill.closeSubPath();
        g.setColour(fx::blue.withAlpha(0.2f)); g.fillPath(fill);
        g.setColour(fx::blue); g.strokePath(path, juce::PathStrokeType(2.0f));
        g.restoreState();
        for (int b = 0; b < 7; ++b) {
            const auto p = nodePos(b);
            fx::node(g, p, trackColour(size_t(b)), hover_ == b || drag_ == b || sel_ == b, 7.0f);
            fx::label(g, juce::String(b + 1), juce::Rectangle<float>(p.x - 7, p.y - 7, 14, 14), col::black, 9.5f, juce::Justification::centred, true);
        }
        if (sel_ >= 0) {
            juce::String s = juce::String(sel_ + 1) + ": " + fx::hzText(v(fk_[sel_].c_str())) + " Hz  " + fx::dbText(v(gk_[sel_].c_str()));
            if (sel_ > 0 && sel_ < 6) s << "  Q " << juce::String(v(qk_[sel_].c_str()), 2);
            fx::label(g, s, {r.getX() + 30, r.getY() + 3, r.getWidth() - 40, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
        } else {
            fx::label(g, "drag a band: sideways = frequency, up/down = gain, Shift-drag = Q", {r.getX() + 30, r.getY() + 3, r.getWidth() - 40, 14}, col::faint, 10.0f);
        }
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        if (ph == 0) { const int h = hit(e.position); if (h != hover_) { hover_ = h; repaint(); } return; }
        if (ph == 1) { drag_ = hit(e.position); if (drag_ >= 0) { sel_ = drag_; q0_ = v(qk_[drag_].c_str()); y0_ = e.position.y; begin(); } else { sel_ = -1; repaint(); } return; }
        if (ph == 2 && drag_ >= 0 && e.mods.isShiftDown() && drag_ > 0 && drag_ < 6) {        // Shift-drag: up and down is the band's Q (the wheel never edits)
            set(qk_[drag_].c_str(), q0_ * std::pow(2.0, double(y0_ - e.position.y) / 40.0));
            return;
        }
        if (ph == 2 && drag_ >= 0) {
            set(fk_[drag_].c_str(), std::round(fx::hzAt(display_, e.position.x)));                          // inside a gesture: one undo step
            set(gk_[drag_].c_str(), std::round(fx::valueAt(display_, e.position.y, -kRange, kRange) * 2.0) / 2.0);
            return;
        }
        if (ph == 3 && drag_ >= 0) { drag_ = -1; end(); return; }
        if (ph == 4) { const int h = hit(e.position); if (h >= 0) set(gk_[h].c_str(), 0.0); }
    }
    std::string fk_[7], gk_[7], qk_[7];
    int hover_ = -1, drag_ = -1, sel_ = -1;
    double q0_ = 1.0;
    float y0_ = 0;
};

// ---- Filter: one node; the curve is the cascade of the chosen number of two-pole sections ----
class FilterView final : public FxView {
public:
    FilterView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(400);
        addChoice("ftype", "TYPE");
        addChoice("slope", "SLOPE");
        claim({"freq", "q"});
    }

protected:
    static constexpr double kHi = 18.0, kLo = -54.0;
    int sections() const { return 1 << std::clamp(int(std::lround(v("slope"))), 0, 2); }
    double responseDb(double hz) const {
        const int type = int(std::lround(v("ftype")));
        const double x = hz / v("freq"), q = v("q");
        const double ql = type == 2 ? std::max(q, 0.1) : std::pow(10.0, q / 20.0);
        const std::complex<double> den(1.0 - x * x, x / ql);
        const std::complex<double> num = type == 0 ? std::complex<double>(1.0, 0.0) : type == 1 ? std::complex<double>(-x * x, 0.0) : std::complex<double>(0.0, x / ql);
        return sections() * 20.0 * std::log10(std::max(std::abs(num / den), 1e-9));
    }
    juce::Point<float> nodePos() const {
        const int type = int(std::lround(v("ftype")));
        const double db = type == 2 ? 0.0 : sections() * v("q");
        return {fx::xLog(display_, v("freq")), fx::yLin(display_, std::clamp(db, kLo, kHi), kLo, kHi)};
    }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        fx::freqGrid(g, r);
        for (double db = -48; db <= 12; db += 12) {
            const float y = fx::yLin(r, db, kLo, kHi);
            fx::hLine(g, r, y, db == 0 ? col::gridBar : col::grid.withAlpha(0.6f));
            fx::label(g, (db > 0 ? "+" : "") + juce::String(int(db)), {r.getX() + 3, y - 10, 24, 10}, col::faint, 9.0f);
        }
        g.saveState(); g.reduceClipRegion(r.toNearestInt());
        auto path = fx::curve(r, 300, [&](float u) { return fx::yLin(r, std::clamp(responseDb(fx::hzAt(r, r.getX() + u * r.getWidth())), kLo, kHi), kLo, kHi); });
        juce::Path fill = path;
        fill.lineTo(r.getRight(), r.getBottom()); fill.lineTo(r.getX(), r.getBottom()); fill.closeSubPath();
        g.setColour(fx::blue.withAlpha(0.22f)); g.fillPath(fill);
        g.setColour(fx::blue); g.strokePath(path, juce::PathStrokeType(2.2f));
        g.restoreState();
        const auto p = nodePos();
        fx::node(g, p, fx::amber, hover_ || drag_, 7.5f);
        fx::label(g, fx::hzText(v("freq")) + " Hz   " + (int(std::lround(v("ftype"))) == 2 ? "Q " + juce::String(v("q"), 1) : "res " + fx::dbText(v("q"))),
                  {r.getX() + 30, r.getY() + 3, r.getWidth() - 40, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        const bool near = nodePos().getDistanceFrom(e.position) < 16.0f;
        if (ph == 0) { if (near != hover_) { hover_ = near; repaint(); } return; }
        if (ph == 1) { drag_ = display_.contains(e.position); if (!drag_) return; begin(); dragQ_ = v("q"); y0_ = e.position.y; }
        if ((ph == 1 || ph == 2) && drag_) {
            const int type = int(std::lround(v("ftype")));
            if (type == 2) set("q", dragQ_ + double(y0_ - e.position.y) / display_.getHeight() * 12.0);
            else set("q", fx::valueAt(display_, e.position.y, kLo, kHi) / sections());
            set("freq", fx::hzAt(display_, e.position.x, 20.0, 20000.0));
        }
        if (ph == 3 && drag_) { drag_ = false; end(); }
    }
    bool hover_ = false, drag_ = false;
    double dragQ_ = 1.0;
    float y0_ = 0;
};

// ---- Auto Filter: the sweep of an LFO-driven low-pass ----
class AutofiltView final : public FxView {
public:
    AutofiltView(app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) : FxView(m, u, i, h) {
        setDisplayWidth(400);
        addRate();
        addKnob("depth", "Depth");
        addKnob("mix", "Mix");
        addKnob("phase", "Phase", fx::violet);
        addChoice("shape", "SHAPE");
        claim({"base", "depth"});
    }

protected:
    static constexpr double kOct = 3.5, kHi = 12.0, kLo = -48.0;
    double hiMul() const { return std::pow(2.0, kOct); }
    double cutoff(double s) const { const double b = v("base"); return b + (b * hiMul() - b) * (1.0 + v("depth") * s) / 2.0; }
    double lpDb(double f, double hz) const {
        const double x = hz / f, ql = std::pow(10.0, 1.0 / 20.0);
        return 20.0 * std::log10(std::max(std::abs(1.0 / std::complex<double>(1.0 - x * x, x / ql)), 1e-9));
    }
    bool animated() const override { return true; }
    void onTick(double dt) override { phase_ += dt * rateHz() * 2.0 * std::numbers::pi; if (phase_ > 1e6) phase_ = 0; }
    void paintDisplay(juce::Graphics& g, juce::Rectangle<float> r) override {
        fx::frame(g, r);
        fx::freqGrid(g, r);
        for (double db = -36; db <= 0; db += 12) { const float y = fx::yLin(r, db, kLo, kHi); fx::hLine(g, r, y, col::grid.withAlpha(0.6f)); }
        const double lo = cutoff(-1.0), hi = cutoff(1.0), cur = cutoff(double(dsp::lfoShapeValue(uint32_t(std::clamp(int(std::lround(v("shape"))), 0, 6)), phase_ / (2.0 * std::numbers::pi) + v("phase") / 360.0)));
        g.setColour(fx::amber.withAlpha(0.12f));
        g.fillRect(juce::Rectangle<float>(fx::xLog(r, lo), r.getY(), fx::xLog(r, hi) - fx::xLog(r, lo), r.getHeight()));
        g.saveState(); g.reduceClipRegion(r.toNearestInt());
        for (double s : {-1.0, 1.0}) {
            auto p = fx::curve(r, 200, [&](float u) { return fx::yLin(r, std::clamp(lpDb(cutoff(s), fx::hzAt(r, r.getX() + u * r.getWidth())), kLo, kHi), kLo, kHi); });
            g.setColour(fx::amber.withAlpha(0.35f)); g.strokePath(p, juce::PathStrokeType(1.0f));
        }
        auto path = fx::curve(r, 240, [&](float u) { return fx::yLin(r, std::clamp(lpDb(cur, fx::hzAt(r, r.getX() + u * r.getWidth())), kLo, kHi), kLo, kHi); });
        juce::Path fill = path;
        fill.lineTo(r.getRight(), r.getBottom()); fill.lineTo(r.getX(), r.getBottom()); fill.closeSubPath();
        g.setColour(fx::blue.withAlpha(0.22f)); g.fillPath(fill);
        g.setColour(fx::blue); g.strokePath(path, juce::PathStrokeType(2.0f));
        g.restoreState();
        for (int n = 0; n < 2; ++n) fx::node(g, {fx::xLog(r, n ? hi : lo), fx::yLin(r, lpDb(n ? hi : lo, n ? hi : lo), kLo, kHi)}, fx::amber, drag_ == n);
        fx::label(g, "sweep " + fx::hzText(lo) + " - " + fx::hzText(hi) + " Hz   now " + fx::hzText(cur) + " Hz", {r.getX() + 8, r.getY() + 3, r.getWidth() - 16, 14}, col::text, 11.0f, juce::Justification::centredLeft, true);
        fx::label(g, "drag the amber ends to set the range", {r.getX() + 8, r.getBottom() - 24, r.getWidth() - 16, 12}, col::faint, 10.0f);
    }
    void pointer(const juce::MouseEvent& e, int ph) override {
        const auto r = display_;
        auto nodeAt = [&](int n) { const double f = n ? cutoff(1.0) : cutoff(-1.0); return juce::Point<float>(fx::xLog(r, f), fx::yLin(r, lpDb(f, f), kLo, kHi)); };
        if (ph == 1) { drag_ = -1; for (int n = 0; n < 2; ++n) if (nodeAt(n).getDistanceFrom(e.position) < 16.0f) drag_ = n; if (drag_ >= 0) begin(); return; }
        if (ph == 2 && drag_ >= 0) {
            double a = cutoff(-1.0), b = cutoff(1.0);
            (drag_ ? b : a) = fx::hzAt(r, e.position.x, 20.0, 20000.0);
            if (a > b) std::swap(a, b);
            const double k = hiMul() - 1.0, base = (a + b) / (2.0 + k), depth = (b - a) / (k * base);
            set("base", base);
            set("depth", depth);
            return;
        }
        if (ph == 3 && drag_ >= 0) { drag_ = -1; end(); }
    }
    double phase_ = 0.0;
    int drag_ = -1;
};

}  // namespace

std::unique_ptr<FxView> makeEqView(const std::string& t, app::AppModel& m, project::Uid u, const app::DeviceInfo& i, int h) {
    if (t == "eq") return std::make_unique<EqThreeView>(m, u, i, h);
    if (t == "eq7") return std::make_unique<Eq7View>(m, u, i, h);
    if (t == "filter") return std::make_unique<FilterView>(m, u, i, h);
    if (t == "autofilt") return std::make_unique<AutofiltView>(m, u, i, h);
    return nullptr;
}

}  // namespace ddaw::ui
