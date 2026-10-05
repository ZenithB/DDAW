#pragma once
// Small controls: a rotary knob bound to a ParamSpec, a vertical fader with a meter, a toggle chip.
#include <functional>

#include "app/model/Catalog.h"
#include "app/ui/Theme.h"

namespace ddaw::ui {

// Chip-style button: momentary or toggling, optional accent colour when on.
class Chip : public juce::Component {
public:
    explicit Chip(juce::String text = {}, juce::Colour on = col::accent) : text_(std::move(text)), on_(on) {}
    std::function<void()> onClick;
    std::function<void(bool)> onToggle;
    void setToggleable(bool t) { toggleable_ = t; }
    void setText(juce::String t) { text_ = std::move(t); repaint(); }
    const juce::String& text() const { return text_; }
    void setOn(bool on) { if (on != state_) { state_ = on; repaint(); } }
    bool isOn() const { return state_; }
    void setOnColour(juce::Colour c) { on_ = c; repaint(); }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        fillRounded(g, r, state_ ? on_ : (hover_ ? col::raised.brighter(0.15f) : col::raised), 3.0f);
        g.setColour(state_ ? col::black : col::text);
        g.setFont(uiFont(11.5f, true));
        g.drawText(text_, getLocalBounds(), juce::Justification::centred);
    }
    void mouseEnter(const juce::MouseEvent&) override { hover_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hover_ = false; repaint(); }
    void mouseUp(const juce::MouseEvent& e) override {
        if (!getLocalBounds().contains(e.getPosition()) || e.mods.isPopupMenu()) return;  // not contains(): that re-tests the point in the parent's space
        if (toggleable_) { state_ = !state_; repaint(); if (onToggle) onToggle(state_); }
        if (onClick) onClick();
    }

private:
    juce::String text_;
    juce::Colour on_;
    bool state_ = false, hover_ = false, toggleable_ = false;
};

// Rotary knob with a name above and a value below. Vertical drag; shift = fine; double-click = default.
class Knob : public juce::Component {
public:
    Knob(const ParamSpec& spec, juce::String label, juce::Colour colour = col::accent)
        : spec_(spec), label_(std::move(label)), colour_(colour), value_(spec.def) {}
    std::function<void()> onBegin, onEnd;
    std::function<void(double)> onChange;

    void setValue(double v) { if (std::abs(v - value_) > 1e-9 && !dragging_) { value_ = v; repaint(); } }
    double value() const { return value_; }
    void setAccent(juce::Colour c) { colour_ = c; repaint(); }
    void setStored(bool stored) { if (stored != stored_) { stored_ = stored; repaint(); } }
    const ParamSpec& spec() const { return spec_; }

    void paint(juce::Graphics& g) override {
        auto b = getLocalBounds().toFloat();
        g.setColour(col::dim);
        g.setFont(uiFont(10.5f));
        g.drawText(label_, b.removeFromTop(13.0f).toNearestInt(), juce::Justification::centred, true);
        auto valRect = b.removeFromBottom(13.0f);
        g.setColour(dragging_ ? col::text : col::dim);
        g.setFont(monoFont(10.5f));
        g.drawText(app::formatParam(spec_, value_), valRect.toNearestInt(), juce::Justification::centred);
        const float d = std::min(b.getWidth(), b.getHeight()) - 4.0f;
        auto k = juce::Rectangle<float>(d, d).withCentre(b.getCentre());
        const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
        const float u = static_cast<float>(app::paramToUnit(spec_, value_));
        juce::Path track, arc;
        track.addCentredArc(k.getCentreX(), k.getCentreY(), d / 2, d / 2, 0, a0, a1, true);
        g.setColour(col::line);
        g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        const float aa = a0 + u * (a1 - a0);
        arc.addCentredArc(k.getCentreX(), k.getCentreY(), d / 2, d / 2, 0, a0, aa, true);
        g.setColour(stored_ ? colour_ : colour_.withAlpha(0.7f));
        g.strokePath(arc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        const float ir = d / 2 - 6;
        g.setColour(col::text);
        g.drawLine(k.getCentreX() + std::sin(aa) * 2, k.getCentreY() - std::cos(aa) * 2, k.getCentreX() + std::sin(aa) * ir,
                   k.getCentreY() - std::cos(aa) * ir, 2.0f);
    }
    void mouseDown(const juce::MouseEvent&) override { dragging_ = true; dragStart_ = app::paramToUnit(spec_, value_); if (onBegin) onBegin(); }
    void mouseDrag(const juce::MouseEvent& e) override {
        const double sens = e.mods.isShiftDown() ? 800.0 : 160.0;
        set(app::paramFromUnit(spec_, dragStart_ - double(e.getDistanceFromDragStartY()) / sens));
    }
    void mouseUp(const juce::MouseEvent&) override { dragging_ = false; if (onEnd) onEnd(); repaint(); }
    void mouseDoubleClick(const juce::MouseEvent&) override {
        if (onBegin) onBegin();
        set(spec_.def);
        if (onEnd) onEnd();
    }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override {
        if (onBegin) onBegin();
        set(app::paramFromUnit(spec_, app::paramToUnit(spec_, value_) + double(w.deltaY) * 0.25));
        if (onEnd) onEnd();
    }

private:
    void set(double v) {
        if (spec_.curve == Curve::Stepped) v = std::round(v);
        if (std::abs(v - value_) < 1e-12) return;
        value_ = v;
        stored_ = true;
        repaint();
        if (onChange) onChange(v);
    }
    ParamSpec spec_;
    juce::String label_;
    juce::Colour colour_;
    double value_, dragStart_ = 0;
    bool dragging_ = false, stored_ = false;
};

// Vertical level meter (peak, dB scale -60..+6) drawn into a rectangle.
inline void drawMeter(juce::Graphics& g, juce::Rectangle<float> r, float peak) {
    g.setColour(col::black);
    g.fillRoundedRectangle(r, 2.0f);
    const float db = 20.0f * std::log10(std::max(peak, 1e-6f));
    const float u = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 66.0f);
    auto fill = r.reduced(1.0f);
    const float h = fill.getHeight() * u;
    auto bar = fill.removeFromBottom(h);
    juce::ColourGradient grad(col::meterHigh, r.getX(), r.getY(), col::meterLow, r.getX(), r.getBottom(), false);
    grad.addColour(0.15, col::meterMid);
    grad.addColour(0.35, col::meterLow);
    g.setGradientFill(grad);
    g.fillRect(bar);
}

// Vertical fader on a dB scale -inf..+6 with a 0 dB tick.
class Fader : public juce::Component {
public:
    std::function<void()> onBegin, onEnd;
    std::function<void(double)> onChange;
    void setDb(double db) { if (!dragging_ && std::abs(db - db_) > 1e-9) { db_ = db; repaint(); } }
    void setPeak(float p) { if (std::abs(p - peak_) > 1e-4f) { peak_ = p; repaint(); } }
    void setColour(juce::Colour c) { colour_ = c; }
    double db() const { return db_; }
    // Cube-root law: 0 dB sits at 75% of the travel, -7.5 dB at the middle, steep near the bottom.
    static double dbToUnit(double db) { return db <= -60 ? 0.0 : db >= 6 ? 1.0 : std::pow((db + 60.0) / 66.0, 3.0); }
    static double unitToDb(double u) { u = juce::jlimit(0.0, 1.0, u); return u <= 0.0 ? -60.0 : std::cbrt(u) * 66.0 - 60.0; }

    void paint(juce::Graphics& g) override {
        auto b = getLocalBounds().toFloat();
        auto meter = b.removeFromRight(7.0f);
        drawMeter(g, meter.reduced(0, 2), peak_);
        b.removeFromRight(4.0f);
        const float x = b.getCentreX();
        g.setColour(col::black);
        g.fillRoundedRectangle(juce::Rectangle<float>(4.0f, b.getHeight() - 8).withCentre({x, b.getCentreY()}), 2.0f);
        const float zeroY = yFor(0.0);
        g.setColour(col::faint);
        g.drawLine(x - 11, zeroY, x + 11, zeroY, 1.0f);
        const float y = yFor(db_);
        auto cap = juce::Rectangle<float>(22.0f, 12.0f).withCentre({x, y});
        fillRounded(g, cap, col::raised.brighter(0.2f), 2.0f);
        g.setColour(colour_);
        g.fillRect(cap.withSizeKeepingCentre(16.0f, 2.0f));
    }
    void mouseDown(const juce::MouseEvent& e) override { dragging_ = true; if (onBegin) onBegin(); drag(e); }
    void mouseDrag(const juce::MouseEvent& e) override { drag(e); }
    void mouseUp(const juce::MouseEvent&) override { dragging_ = false; if (onEnd) onEnd(); }
    void mouseDoubleClick(const juce::MouseEvent&) override { if (onBegin) onBegin(); db_ = 0; repaint(); if (onChange) onChange(0.0); if (onEnd) onEnd(); }

private:
    float yFor(double db) const {
        const float top = 6.0f, bot = float(getHeight()) - 6.0f;
        return bot - float(dbToUnit(db)) * (bot - top);
    }
    void drag(const juce::MouseEvent& e) {
        const float top = 6.0f, bot = float(getHeight()) - 6.0f;
        double db = unitToDb(double((bot - e.position.y) / (bot - top)));
        if (std::abs(db) < 1.2) db = 0.0;  // detent at unity
        db_ = db;
        repaint();
        if (onChange) onChange(db);
    }
    double db_ = 0;
    float peak_ = 0;
    juce::Colour colour_ = col::accent;
    bool dragging_ = false;
};

// A number you drag (vertical) or double-click to type. Shows `suffix` after the value.
class NumberBox : public juce::Component {
public:
    NumberBox(double min, double max, int decimals, juce::String suffix = {}) : min_(min), max_(max), dec_(decimals), suffix_(std::move(suffix)) {}
    std::function<void()> onBegin, onEnd;
    std::function<void(double)> onChange;
    void setValue(double v) { if (!dragging_ && !editing_ && std::abs(v - value_) > 1e-9) { value_ = v; repaint(); } }
    double value() const { return value_; }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        fillRounded(g, r, col::black, 3.0f);
        if (editing_) return;
        g.setColour(col::text);
        g.setFont(monoFont(15.0f));
        g.drawText(juce::String(value_, dec_) + suffix_, getLocalBounds(), juce::Justification::centred);
    }
    void mouseDown(const juce::MouseEvent&) override { dragging_ = true; start_ = value_; if (onBegin) onBegin(); }
    void mouseDrag(const juce::MouseEvent& e) override {
        const double step = std::pow(10.0, -dec_) * (e.mods.isShiftDown() ? 1.0 : 10.0) * 0.1 * 10.0;
        set(start_ - double(e.getDistanceFromDragStartY()) * step * (dec_ == 0 ? 0.5 : 1.0));
    }
    void mouseUp(const juce::MouseEvent&) override { dragging_ = false; if (onEnd) onEnd(); }
    void mouseDoubleClick(const juce::MouseEvent&) override { beginEdit(); }

private:
    void set(double v) {
        v = juce::jlimit(min_, max_, dec_ == 0 ? std::round(v) : std::round(v * std::pow(10.0, dec_)) / std::pow(10.0, dec_));
        if (std::abs(v - value_) < 1e-12) return;
        value_ = v;
        repaint();
        if (onChange) onChange(v);
    }
    void beginEdit() {
        editing_ = true;
        editor_ = std::make_unique<juce::TextEditor>();
        editor_->setBounds(getLocalBounds().reduced(2));
        editor_->setFont(monoFont(15.0f));
        editor_->setJustification(juce::Justification::centred);
        editor_->setText(juce::String(value_, dec_), false);
        editor_->setInputRestrictions(8, "0123456789.-");
        addAndMakeVisible(*editor_);
        editor_->grabKeyboardFocus();
        editor_->selectAll();
        auto finish = [this] {
            const double v = editor_->getText().getDoubleValue();
            editing_ = false;
            if (onBegin) onBegin();
            set(v);
            if (onEnd) onEnd();
            juce::MessageManager::callAsync([sp = juce::Component::SafePointer<NumberBox>(this)] { if (sp) { sp->editor_.reset(); sp->repaint(); } });
        };
        editor_->onReturnKey = finish;
        editor_->onFocusLost = finish;
        editor_->onEscapeKey = [this] {
            editing_ = false;
            juce::MessageManager::callAsync([sp = juce::Component::SafePointer<NumberBox>(this)] { if (sp) { sp->editor_.reset(); sp->repaint(); } });
        };
    }
    double min_, max_, value_ = 0, start_ = 0;
    int dec_;
    juce::String suffix_;
    bool dragging_ = false, editing_ = false;
    std::unique_ptr<juce::TextEditor> editor_;
};

}  // namespace ddaw::ui
