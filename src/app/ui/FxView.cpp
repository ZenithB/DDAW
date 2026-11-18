#include "app/ui/FxView.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

#include "app/ui/FxViewsInternal.h"

namespace ddaw::ui {

// ---- Choice ----

Choice::Choice(const ParamSpec& s, juce::String caption, juce::Colour on) : caption_(std::move(caption)), on_(on), min_(int(std::lround(s.min))) {
    const int n = int(std::lround(s.max)) - int(std::lround(s.min)) + 1;
    for (int i = 0; i < n; ++i)
        names_.add(s.names && i < int(s.nNames) ? juce::String(s.names[i]) : juce::String(int(std::lround(s.min)) + i));
    sel_ = std::clamp(int(std::lround(s.def)) - min_, 0, std::max(0, n - 1));
}
void Choice::setValue(double v) {
    const int i = std::clamp(int(std::lround(v)) - min_, 0, std::max(0, names_.size() - 1));
    if (i != sel_) { sel_ = i; repaint(); }
}
int Choice::preferredWidth() const {
    const auto f = uiFont(11.0f, true);
    if (isMenu()) { float w = 0; for (auto& n : names_) w = std::max(w, textWidth(f, n)); return kCaptionW + int(w) + 28; }
    int w = kCaptionW;
    for (auto& n : names_) w += int(textWidth(f, n)) + 14;
    return w;
}
juce::Rectangle<int> Choice::segment(int i) const {
    if (isMenu()) return {kCaptionW, 0, getWidth() - kCaptionW, getHeight()};
    const auto f = uiFont(11.0f, true);
    int x = kCaptionW;
    for (int k = 0; k < names_.size(); ++k) {
        const int w = int(textWidth(f, names_[k])) + 14;
        if (k == i) return {x, 0, w, getHeight()};
        x += w;
    }
    return {};
}
int Choice::indexAt(juce::Point<int> p) const {
    if (isMenu()) return -1;
    for (int i = 0; i < names_.size(); ++i) if (segment(i).contains(p)) return i;
    return -1;
}
void Choice::paint(juce::Graphics& g) {
    g.setColour(col::dim);
    g.setFont(uiFont(9.5f, true));
    g.drawText(caption_, 0, 0, kCaptionW - 2, getHeight(), juce::Justification::centredLeft, true);
    g.setFont(uiFont(11.0f, true));
    if (isMenu()) {
        auto r = segment(0).toFloat().reduced(0.5f);
        fillRounded(g, r, col::raised, 3.0f);
        g.setColour(col::text);
        g.drawText(names_[sel_], r.reduced(6, 0), juce::Justification::centredLeft);
        g.setColour(col::dim);
        juce::Path t; t.addTriangle(r.getRight() - 14, r.getCentreY() - 2, r.getRight() - 6, r.getCentreY() - 2, r.getRight() - 10, r.getCentreY() + 3);
        g.fillPath(t);
        return;
    }
    for (int i = 0; i < names_.size(); ++i) {
        auto r = segment(i).toFloat().reduced(0.5f);
        fillRounded(g, r, i == sel_ ? on_ : col::raised, 3.0f);
        g.setColour(i == sel_ ? col::black : col::text);
        g.drawText(names_[i], r, juce::Justification::centred);
    }
}
void Choice::mouseUp(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu() || !getLocalBounds().contains(e.getPosition())) return;
    if (isMenu()) {
        juce::PopupMenu m;
        for (int i = 0; i < names_.size(); ++i) m.addItem(i + 1, names_[i], true, i == sel_);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int r) { if (r > 0 && onPick) onPick(min_ + r - 1); });
        return;
    }
    const int i = indexAt(e.getPosition());
    if (i >= 0 && i != sel_ && onPick) onPick(min_ + i);
}
void Choice::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    const int i = std::clamp(sel_ + (w.deltaY > 0 ? -1 : 1), 0, names_.size() - 1);
    if (w.deltaY != 0.0f && i != sel_ && onPick) onPick(min_ + i);
}

// ---- drawing helpers ----

namespace fx {
void frame(juce::Graphics& g, juce::Rectangle<float> r) { fillRounded(g, r, col::black.withAlpha(0.55f), 4.0f); }
float xLog(juce::Rectangle<float> r, double hz, double lo, double hi) {
    return r.getX() + float(std::log(std::clamp(hz, lo, hi) / lo) / std::log(hi / lo)) * r.getWidth();
}
double hzAt(juce::Rectangle<float> r, float x, double lo, double hi) {
    const double u = std::clamp(double((x - r.getX()) / r.getWidth()), 0.0, 1.0);
    return lo * std::pow(hi / lo, u);
}
float yLin(juce::Rectangle<float> r, double v, double lo, double hi) { return r.getY() + float((hi - v) / (hi - lo)) * r.getHeight(); }
double valueAt(juce::Rectangle<float> r, float y, double lo, double hi) {
    const double u = std::clamp(double((y - r.getY()) / r.getHeight()), 0.0, 1.0);
    return hi - u * (hi - lo);
}
void freqGrid(juce::Graphics& g, juce::Rectangle<float> r, double lo, double hi, bool labels) {
    static const double lines[] = {20, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 20000};
    g.setFont(uiFont(9.0f));
    for (double f : lines) {
        if (f < lo || f > hi) continue;
        const float x = xLog(r, f, lo, hi);
        const bool major = f == 100 || f == 1000 || f == 10000;
        g.setColour(major ? col::gridBar : col::grid.withAlpha(0.6f));
        g.drawVerticalLine(int(x), r.getY(), r.getBottom());
        if (labels && (major || f == 20 || f == 50 || f == 500 || f == 5000))
            label(g, hzText(f), {x + 2, r.getBottom() - 11, 36, 10}, col::faint, 9.0f);
    }
}
void hLine(juce::Graphics& g, juce::Rectangle<float> r, float y, juce::Colour c, float thick) {
    g.setColour(c);
    g.drawLine(r.getX(), y, r.getRight(), y, thick);
}
void label(juce::Graphics& g, juce::String text, juce::Rectangle<float> r, juce::Colour c, float size, juce::Justification j, bool bold) {
    g.setColour(c);
    g.setFont(uiFont(size, bold));
    g.drawText(text, r.toNearestInt(), j, true);
}
void node(juce::Graphics& g, juce::Point<float> c, juce::Colour colr, bool hot, float radius) {
    if (hot) { g.setColour(colr.withAlpha(0.3f)); g.fillEllipse(c.x - radius - 4, c.y - radius - 4, 2 * (radius + 4), 2 * (radius + 4)); }
    g.setColour(colr);
    g.fillEllipse(c.x - radius, c.y - radius, 2 * radius, 2 * radius);
    g.setColour(col::black.withAlpha(0.6f));
    g.drawEllipse(c.x - radius, c.y - radius, 2 * radius, 2 * radius, 1.0f);
}
juce::Path curve(juce::Rectangle<float> r, int steps, const std::function<float(float)>& yOf) {
    juce::Path p;
    for (int i = 0; i <= steps; ++i) {
        const float u = float(i) / float(steps), x = r.getX() + u * r.getWidth(), y = yOf(u);
        if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
    }
    return p;
}
juce::String hzText(double hz) { return hz >= 1000.0 ? juce::String(hz / 1000.0, hz >= 10000.0 ? 0 : 1) + "k" : juce::String(int(std::lround(hz))); }
juce::String dbText(double db, int dp) { return (db > 0.05 ? "+" : "") + (dp <= 0 ? juce::String(int(std::lround(db))) : juce::String(db, dp)) + " dB"; }

double biquadDb(int kind, double f0, double gainDb, double q, double hz, double sr) {
    const double w0 = 2.0 * std::numbers::pi * std::clamp(f0, 10.0, sr * 0.49) / sr, c = std::cos(w0), s = std::sin(w0);
    const double A = std::pow(10.0, gainDb / 40.0);
    double b0, b1, b2, a0, a1, a2;
    if (kind == 0) {
        const double al = s / (2.0 * std::max(q, 0.05));
        b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A;
    } else {
        const double al = s / 2.0 * std::sqrt(2.0), sa = 2.0 * std::sqrt(A) * al;
        if (kind == 1) {
            b0 = A * ((A + 1) - (A - 1) * c + sa); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sa);
            a0 = (A + 1) + (A - 1) * c + sa; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sa;
        } else {
            b0 = A * ((A + 1) + (A - 1) * c + sa); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sa);
            a0 = (A + 1) - (A - 1) * c + sa; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sa;
        }
    }
    const double w = 2.0 * std::numbers::pi * hz / sr;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
    const double mag = std::abs(b0 + b1 * z1 + b2 * z2) / std::max(std::abs(a0 + a1 * z1 + a2 * z2), 1e-12);
    return 20.0 * std::log10(std::max(mag, 1e-9));
}
}  // namespace fx

// ---- FxView ----

namespace {
const double kDivBeats[] = {32, 16, 8, 4, 2, 1, 0.5, 1.0 / 3.0, 0.25};   // 8 bars .. 1/16 in beats (the rateSync names)
constexpr int kKnobW = 50, kKnobH = 66, kHeaderPad = 36;
}  // namespace

FxView::FxView(app::AppModel& m, project::Uid uid, const app::DeviceInfo& info, int availH) : model_(m), uid_(uid), info_(info), availH_(availH) {
    for (size_t i = 0; i < info_.params.size(); ++i) { index_[info_.params[i].key] = i; vals_.push_back(double(info_.params[i].def)); }
}
FxView::~FxView() = default;

const ParamSpec* FxView::spec(const char* key) const {
    const auto it = index_.find(key);
    return it == index_.end() ? nullptr : &info_.params[it->second];
}
double FxView::value(const std::string& key) const {
    const auto it = index_.find(key);
    return it == index_.end() ? 0.0 : vals_[it->second];
}

Knob* FxView::addKnob(const char* key, const char* label, juce::Colour c) {
    const auto* s = spec(key);
    if (!s) return nullptr;
    auto k = std::make_unique<Knob>(*s, label, c);
    const std::string kk = key;
    k->onBegin = [this] { begin(); };
    k->onChange = [this, kk](double val) { vals_[index_[kk]] = val; model_.apply(document::cmd::setParam(uid_, kk, val)); onSync(); repaint(display_.toNearestInt()); };
    k->onEnd = [this] { end(); };
    addAndMakeVisible(*k);
    covered_.insert(key);
    knobKeys_.push_back(key);
    knobs_.push_back(std::move(k));
    return knobs_.back().get();
}

Choice* FxView::addChoice(const char* key, const char* caption) {
    const auto* s = spec(key);
    if (!s) return nullptr;
    auto c = std::make_unique<Choice>(*s, caption, fx::blue);
    const std::string kk = key;
    c->onPick = [this, kk](int val) { set(kk.c_str(), val); };
    addAndMakeVisible(*c);
    covered_.insert(key);
    choiceKeys_.push_back(key);
    choices_.push_back(std::move(c));
    return choices_.back().get();
}

void FxView::addRate() {
    rateMode_ = addChoice("rateMode", "RATE");
    rateSync_ = addChoice("rateSync", "DIV");
    rateLow_ = addKnob("rate", "Rate", fx::amber);
    rateHigh_ = addKnob("rateHi", "Rate", fx::amber);
}

void FxView::set(const char* key, double value) {
    const auto it = index_.find(key);
    if (it == index_.end()) return;
    const ParamSpec& s = info_.params[it->second];
    value = std::clamp(value, double(s.min), double(s.max));
    if (s.curve == Curve::Stepped) value = std::round(value);
    if (std::abs(value - vals_[it->second]) < 1e-9) return;
    vals_[it->second] = value;
    model_.apply(document::cmd::setParam(uid_, key, value));
    for (size_t i = 0; i < knobKeys_.size(); ++i) if (knobKeys_[i] == key) knobs_[i]->setValue(value);
    for (size_t i = 0; i < choiceKeys_.size(); ++i) if (choiceKeys_[i] == key) choices_[i]->setValue(value);
    layoutControls();
    onSync();
    repaint(display_.toNearestInt());
}

void FxView::setMany(const std::string& label, const std::vector<std::pair<const char*, double>>& changes) {
    std::vector<document::Command> cs;
    for (auto [k, val] : changes) {
        const auto it = index_.find(k);
        if (it == index_.end()) continue;
        const ParamSpec& s = info_.params[it->second];
        val = std::clamp(val, double(s.min), double(s.max));
        if (s.curve == Curve::Stepped) val = std::round(val);
        vals_[it->second] = val;
        cs.push_back(document::cmd::setParam(uid_, k, val));
    }
    model_.applyGroup(label, cs);
    onSync();
    repaint(display_.toNearestInt());
}

double FxView::rateHz() const {
    const int mode = int(std::lround(value("rateMode")));
    if (mode == 0) return bpm() / 60.0 / kDivBeats[std::clamp(int(std::lround(value("rateSync"))), 0, 8)];
    return mode == 2 ? value("rateHi") : value("rate");
}

void FxView::sync(const project::DeviceSpec& d) {
    for (size_t i = 0; i < info_.params.size(); ++i) {
        const auto it = d.params.find(info_.params[i].key);
        vals_[i] = it != d.params.end() ? it->second : double(info_.params[i].def);
    }
    for (size_t i = 0; i < knobKeys_.size(); ++i) {
        knobs_[i]->setStored(d.params.count(knobKeys_[i]) > 0);
        knobs_[i]->setValue(value(knobKeys_[i]));
    }
    for (size_t i = 0; i < choiceKeys_.size(); ++i) choices_[i]->setValue(value(choiceKeys_[i]));
    layoutControls();
    onSync();
    repaint();
}

void FxView::tick() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = lastMs_ > 0 ? std::min(0.25, (now - lastMs_) * 0.001) : 0.0;
    lastMs_ = now;
    time_ += dt;
    onTick(dt);
    if (animated()) repaint(display_.toNearestInt());
}

juce::Component* FxView::control(const std::string& key) {
    for (size_t i = 0; i < knobKeys_.size(); ++i) if (knobKeys_[i] == key) return knobs_[i].get();
    for (size_t i = 0; i < choiceKeys_.size(); ++i) if (choiceKeys_[i] == key) return choices_[i].get();
    return nullptr;
}

int FxView::knobRows() const {
    // the choices flow into the column first; what height is left is for knob rows
    int x = 0, rows = choices_.empty() ? 0 : 1;
    for (const auto& c : choices_) {
        if (c.get() == rateSync_ && rateMode_ && int(std::lround(value("rateMode"))) != 0) continue;
        const int w = c->preferredWidth();
        if (x > 0 && x + w > ctlW_) { ++rows; x = 0; }
        x += w + 6;
    }
    const int bodyH = std::max(60, availH_ - kHeaderPad);
    return std::max(1, (bodyH - rows * (Choice::kH + 4) - 4) / kKnobH);
}

int FxView::preferredWidth() const {
    // the column is as wide as its knobs need (cols of kKnobW) but never narrower than the widest choice row needs
    int slots = 0;
    for (size_t i = 0; i < knobs_.size(); ++i) if (knobs_[i].get() != rateHigh_) ++slots;
    int widest = 0;
    for (const auto& c : choices_) widest = std::max(widest, c->preferredWidth());
    if (slots == 0 && widest == 0) return displayW_;
    const_cast<FxView*>(this)->ctlW_ = std::max(150, widest);
    const int rows = knobRows();
    const int cols = slots == 0 ? 0 : (slots + rows - 1) / rows;
    const int w = std::max({150, widest, cols * kKnobW});
    const_cast<FxView*>(this)->ctlW_ = w;
    return displayW_ + 10 + w;
}

void FxView::layoutControls() {
    if (rateSync_) {
        const int mode = int(std::lround(value("rateMode")));
        rateSync_->setVisible(mode == 0);
        if (rateLow_) rateLow_->setVisible(mode == 1);
        if (rateHigh_) rateHigh_->setVisible(mode == 2);
    }
    if (getWidth() <= 0) return;
    auto c = juce::Rectangle<int>(int(display_.getRight()) + 10, 0, ctlW_, getHeight());
    int x = c.getX(), y = 0;
    for (auto& ch : choices_) {
        if (!ch->isVisible()) continue;
        const int w = ch->preferredWidth();
        if (x > c.getX() && x + w > c.getRight()) { x = c.getX(); y += Choice::kH + 4; }
        ch->setBounds(x, y, w, Choice::kH);
        x += w + 6;
    }
    if (!choices_.empty()) y += Choice::kH + 6;
    const int rows = std::max(1, (getHeight() - y) / kKnobH);
    int slot = 0;
    for (auto& k : knobs_) {
        if (!k->isVisible()) continue;
        k->setBounds(c.getX() + (slot / rows) * kKnobW, y + (slot % rows) * kKnobH, kKnobW, kKnobH);
        ++slot;
    }
}

void FxView::resized() {
    display_ = juce::Rectangle<float>(0, 0, float(displayW_), float(getHeight())).reduced(0, 0);
    if (rateSync_) {
        const int mode = int(std::lround(value("rateMode")));
        rateSync_->setVisible(mode == 0);
        if (rateLow_) rateLow_->setVisible(mode == 1);
        if (rateHigh_) rateHigh_->setVisible(mode == 2);
    }
    layoutControls();
}

void FxView::paint(juce::Graphics& g) { paintDisplay(g, display_); }

std::unique_ptr<FxView> FxView::make(app::AppModel& m, project::Uid uid, const app::DeviceInfo& info, int availH) {
    const std::string& t = info.type;
    if (auto v = makeEqView(t, m, uid, info, availH)) return v;
    if (auto v = makeDynView(t, m, uid, info, availH)) return v;
    if (auto v = makeTimeView(t, m, uid, info, availH)) return v;
    if (auto v = makeModView(t, m, uid, info, availH)) return v;
    if (auto v = makeColourView(t, m, uid, info, availH)) return v;
    return nullptr;
}

}  // namespace ddaw::ui
