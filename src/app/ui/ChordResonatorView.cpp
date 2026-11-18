#include "app/ui/ChordResonatorView.h"

#include <algorithm>
#include <cmath>

#include "devices/schema/SchemaExt.h"

namespace ddaw::ui {

namespace {
constexpr int kLoSemi = -12, kHiSemi = 36, kCells = kHiSemi - kLoSemi + 1;
constexpr double kTopDb = 50.0, kBottomDb = -24.0;
constexpr float kBeadR = 7.0f, kHitR = 10.0f;
constexpr int kCtlW = 200, kRulerH = 22, kHintH = 14, kGap = 10;

constexpr bool streq(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static_assert(streq(schema::kFxChordres[0].key, "source") && streq(schema::kFxChordres[1].key, "filters") && streq(schema::kFxChordres[3].key, "tonicLevel") &&
              streq(schema::kFxChordres[4].key, "i1") && streq(schema::kFxChordres[10].key, "l1") && streq(schema::kFxChordres[16].key, "boost") &&
              streq(schema::kFxChordres[17].key, "q") && streq(schema::kFxChordres[18].key, "steep") && streq(schema::kFxChordres[20].key, "sat") &&
              streq(schema::kFxChordres[23].key, "thresh"),
              "ChordResonatorView's parameter indices follow kFxChordres");

// The response of one resonance (dB, 0 at its centre) `semis` semitones from its centre: `stages` identical band-pass sections whose
// Q is set so the -3 dB bandwidth of the cascade is f / q (the device's own formula).
double responseDb(double semis, double q, int stages) {
    const double r = std::exp2(semis / 12.0), d = r - 1.0 / r;
    const double qs = q * std::sqrt(std::pow(2.0, 1.0 / stages) - 1.0);
    return -10.0 * stages * std::log10(1.0 + qs * qs * d * d);
}
const char* const kNote[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
}  // namespace

ChordResonatorView::ChordResonatorView(app::AppModel& m, project::Uid uid, const app::DeviceInfo& info) : model_(m), uid_(uid), info_(info) {
    for (int i = 0; i < kNumP && size_t(i) < info_.params.size(); ++i) v_[size_t(i)] = double(info_.params[size_t(i)].def);

    static const char* const srcNames[] = {"Auto", "Notes", "Audio"};
    for (int i = 0; i < 3; ++i) {
        srcChips_[size_t(i)] = std::make_unique<Chip>(srcNames[i], juce::Colour(0xff5aa9e6));
        srcChips_[size_t(i)]->onClick = [this, i] { model_.apply(document::cmd::setParam(uid_, "source", i)); };
        addAndMakeVisible(*srcChips_[size_t(i)]);
    }
    static const char* const steepNames[] = {"12", "24", "36", "48"};
    for (int i = 0; i < 4; ++i) {
        steepChips_[size_t(i)] = std::make_unique<Chip>(steepNames[i], juce::Colour(0xff5aa9e6));
        steepChips_[size_t(i)]->onClick = [this, i] { model_.apply(document::cmd::setParam(uid_, "steep", i)); };
        addAndMakeVisible(*steepChips_[size_t(i)]);
    }
    tonicChip_ = std::make_unique<Chip>("Tonic", col::text);
    tonicChip_->onClick = [this] { model_.apply(document::cmd::setParam(uid_, "tonic", v_[Tonic] >= 0.5 ? 0.0 : 1.0)); };
    addAndMakeVisible(*tonicChip_);

    struct K { int idx; const char* label; };
    for (K k : {K{Boost, "Boost"}, K{Q, "Q"}, K{Excite, "Excite"}, K{Sat, "Saturate"}, K{Mix, "Mix"}, K{Glide, "Glide"}, K{Thresh, "Thresh"}}) {
        if (size_t(k.idx) >= info_.params.size()) continue;
        const auto& spec = info_.params[size_t(k.idx)];
        auto kn = std::make_unique<Knob>(spec, k.label, k.idx == Boost ? col::accent : juce::Colour(0xff5aa9e6));
        const std::string key = spec.key;
        kn->onBegin = [this] { model_.beginGesture("parameter"); };
        kn->onChange = [this, key, idx = k.idx](double v) { v_[size_t(idx)] = v; model_.apply(document::cmd::setParam(uid_, key, v)); repaint(plot_.toNearestInt()); };
        kn->onEnd = [this] { model_.endGesture(); };
        addAndMakeVisible(*kn);
        knobParam_.push_back(k.idx);
        knobs_.push_back(std::move(kn));
    }
    setMouseCursor(juce::MouseCursor::NormalCursor);
    updateReadout(-2);
}

juce::Component* ChordResonatorView::knob(const char* key) {
    for (auto& k : knobs_) if (std::string(k->spec().key) == key) return k.get();
    return nullptr;
}

void ChordResonatorView::sync(const project::DeviceSpec& d) {
    for (int i = 0; i < kNumP && size_t(i) < info_.params.size(); ++i) {
        const auto it = d.params.find(info_.params[size_t(i)].key);
        v_[size_t(i)] = it != d.params.end() ? it->second : double(info_.params[size_t(i)].def);
    }
    for (size_t i = 0; i < knobs_.size(); ++i) {
        const auto& key = info_.params[size_t(knobParam_[i])].key;
        knobs_[i]->setStored(d.params.count(key) > 0);
        knobs_[i]->setValue(v_[size_t(knobParam_[i])]);
    }
    refreshChips();
    repaint();
}

void ChordResonatorView::refreshChips() {
    const int src = int(std::lround(v_[Source])), st = int(std::lround(v_[Steep]));
    for (int i = 0; i < 3; ++i) srcChips_[size_t(i)]->setOn(i == src);
    for (int i = 0; i < 4; ++i) steepChips_[size_t(i)]->setOn(i == st);
    tonicChip_->setOn(v_[Tonic] >= 0.5);
    if (auto* th = knob("thresh")) th->setAlpha(src == 1 ? 0.35f : 1.0f);         // only the audio source reads a level
}

void ChordResonatorView::tick() {
    const float t = model_.deviceValue(uid_);
    if (std::abs(t - tonic_) > 0.005f) { tonic_ = t; updateReadout(hover_ >= -1 ? hover_ : drag_); repaint(); }
}

// ---- geometry ----

void ChordResonatorView::resized() {
    auto b = getLocalBounds().toFloat();
    controls_ = b.removeFromRight(float(kCtlW));
    b.removeFromRight(float(kGap));
    hint_ = b.removeFromBottom(float(kHintH));
    ruler_ = b.removeFromBottom(float(kRulerH));
    plot_ = b;
    // right column: selectors, then a grid of knobs (4 x 2)
    auto c = controls_.toNearestInt();
    auto row = c.removeFromTop(20);
    row.removeFromLeft(44);
    for (auto& ch : srcChips_) ch->setBounds(row.removeFromLeft(52).reduced(1, 0));
    c.removeFromTop(4);
    row = c.removeFromTop(20);
    row.removeFromLeft(44);
    for (auto& ch : steepChips_) ch->setBounds(row.removeFromLeft(39).reduced(1, 0));
    c.removeFromTop(6);
    const int kw = c.getWidth() / 4, kh = std::min(68, c.getHeight() / 2);
    for (size_t i = 0; i < knobs_.size(); ++i) knobs_[i]->setBounds(c.getX() + int(i % 4) * kw, c.getY() + int(i / 4) * kh, kw, kh);
    tonicChip_->setBounds(c.getX() + 3 * kw + 2, c.getY() + kh + kh / 2 - 10, kw - 4, 20);
}

float ChordResonatorView::xOfSemitone(int s) const { return plot_.getX() + (float(s - kLoSemi) + 0.5f) / float(kCells) * plot_.getWidth(); }
float ChordResonatorView::yOfDb(double db) const { return plot_.getY() + float((kTopDb - db) / (kTopDb - kBottomDb)) * plot_.getHeight(); }
int ChordResonatorView::semitoneAt(float x) const {
    return std::clamp(int(std::floor((x - plot_.getX()) / plot_.getWidth() * float(kCells))) + kLoSemi, kLoSemi, kHiSemi);
}
double ChordResonatorView::levelAt(float y, double boost, int idx) const {
    const double db = kTopDb - double(y - plot_.getY()) / double(plot_.getHeight()) * (kTopDb - kBottomDb);
    const auto& s = info_.params[size_t(idx)];
    return std::clamp(std::round((db - boost) * 2.0) / 2.0, double(s.min), double(s.max));
}
int ChordResonatorView::activeFilters() const { return std::clamp(int(std::lround(v_[Filters])), 1, kFilters); }
double ChordResonatorView::value(const char* key) const {
    for (size_t i = 0; i < info_.params.size() && i < size_t(kNumP); ++i) if (std::string(info_.params[i].key) == key) return v_[i];
    return 0.0;
}

juce::Point<float> ChordResonatorView::beadCentre(int k) const {
    if (k < 0) return {xOfSemitone(0), yOfDb(v_[Boost] + v_[TonicLevel])};
    return {xOfSemitone(int(std::lround(v_[size_t(I1 + k)]))), yOfDb(v_[Boost] + v_[size_t(L1 + k)])};
}

int ChordResonatorView::hit(juce::Point<float> p) const {
    int best = -2;
    float bestD = kHitR * kHitR;
    auto consider = [&](int k) { const float d = beadCentre(k).getDistanceSquaredFrom(p); if (d <= bestD) { bestD = d; best = k; } };
    consider(-1);
    for (int k = activeFilters() - 1; k >= 0; --k) consider(k);     // later (drawn on top) filters win ties
    return best;
}

// ---- names ----

juce::String ChordResonatorView::intervalName(int semis) const {
    if (size_t(I1) < info_.params.size()) return app::formatParam(info_.params[size_t(I1)], semis);
    return juce::String(semis);
}
juce::String ChordResonatorView::noteName(double midi) const {
    const int m = int(std::lround(midi));
    return juce::String(kNote[((m % 12) + 12) % 12]) + juce::String(m / 12 - 1);
}

void ChordResonatorView::updateReadout(int bead) {
    juce::String s;
    if (bead >= 0 && bead < kFilters) {
        const int semis = int(std::lround(v_[size_t(I1 + bead)]));
        s << juce::String(bead + 1) << ": " << intervalName(semis);
        if (tonic_ >= 0.0f) {
            const double hz = 440.0 * std::pow(2.0, (double(std::round(tonic_)) + semis - 69.0) / 12.0);
            s << "  " << noteName(std::round(tonic_) + semis) << "  " << juce::String(hz, hz < 1000.0 ? 1 : 0) << " Hz";
        }
        s << "  " << juce::String(v_[size_t(L1 + bead)], 1) << " dB";
    } else if (bead == -1) {
        s << "Tonic" << (v_[Tonic] >= 0.5 ? "  " + juce::String(v_[TonicLevel], 1) + " dB" : juce::String("  (off: click to switch on)"));
    } else if (tonic_ >= 0.0f) {
        const double hz = 440.0 * std::pow(2.0, (double(tonic_) - 69.0) / 12.0);
        s << "Tonic " << noteName(tonic_) << "  " << juce::String(hz, hz < 1000.0 ? 1 : 0) << " Hz";
    } else {
        const int src = int(std::lround(v_[Source]));
        s << (src == 2 ? "listening for a note..." : "play a note");
    }
    readout_ = s;
}

// ---- painting ----

void ChordResonatorView::paint(juce::Graphics& g) {
    drawPlot(g);
    drawRuler(g);
    g.setColour(col::faint);
    g.setFont(uiFont(10.0f));
    g.drawText("Drag a bead: sideways = interval, up/down = level   |   double-click: add / remove", hint_.toNearestInt(), juce::Justification::centredLeft, true);
    // captions of the selector rows
    g.setColour(col::dim);
    g.setFont(uiFont(10.0f, true));
    auto c = controls_.toNearestInt();
    g.drawText("SOURCE", c.getX(), c.getY(), 44, 20, juce::Justification::centredLeft);
    g.drawText("DB/OCT", c.getX(), c.getY() + 24, 44, 20, juce::Justification::centredLeft);
}

void ChordResonatorView::drawPlot(juce::Graphics& g) {
    fillRounded(g, plot_, col::black.withAlpha(0.55f), 4.0f);
    g.saveState();
    g.reduceClipRegion(plot_.toNearestInt());
    const double boost = v_[Boost], q = v_[Q];
    const int stages = 2 * (std::clamp(int(std::lround(v_[Steep])), 0, 3) + 1);
    const int nf = activeFilters();
    const bool tonicOn = v_[Tonic] >= 0.5;

    // grid: a line per semitone, heavier on the octaves; dB lines every 12
    for (int s = kLoSemi; s <= kHiSemi; ++s) {
        const float x = xOfSemitone(s);
        g.setColour(s % 12 == 0 ? col::gridBar : col::grid.withAlpha(0.6f));
        g.drawVerticalLine(int(x), plot_.getY(), plot_.getBottom());
    }
    g.setFont(uiFont(9.0f));
    for (int db = -12; db <= 48; db += 12) {
        const float y = yOfDb(db);
        g.setColour(db == 0 ? col::gridBar : col::grid.withAlpha(0.6f));
        g.drawHorizontalLine(int(y), plot_.getX(), plot_.getRight());
        g.setColour(col::faint);
        g.drawText((db > 0 ? "+" : "") + juce::String(db), int(plot_.getX()) + 2, int(y) - 11, 26, 11, juce::Justification::centredLeft);
    }

    struct Res { double semis, db; juce::Colour c; bool on; };
    std::vector<Res> res;
    if (tonicOn) res.push_back({0.0, boost + v_[TonicLevel], col::text, true});
    for (int k = 0; k < nf; ++k) res.push_back({std::round(v_[size_t(I1 + k)]), boost + v_[size_t(L1 + k)], trackColour(size_t(k)), true});

    // sample positions: a regular grid plus extra points around every centre, so a narrow peak is never missed
    std::vector<double> xs;
    for (double s = kLoSemi - 0.5; s <= kHiSemi + 0.5 + 1e-9; s += 0.1) xs.push_back(s);
    const double bw = 12.0 / std::max(q, 1.0);                    // -3 dB bandwidth in semitones
    for (const auto& r : res)
        for (double m : {0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0}) { xs.push_back(r.semis + m * bw); xs.push_back(r.semis - m * bw); }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());

    auto xOfS = [&](double s) { return plot_.getX() + float((s - kLoSemi + 0.5) / kCells) * plot_.getWidth(); };
    const float baseY = plot_.getBottom();
    for (const auto& r : res) {
        juce::Path line, fill;
        bool first = true;
        for (double s : xs) {
            const double db = std::max(r.db + responseDb(s - r.semis, q, stages), kBottomDb);
            const float x = xOfS(s), y = yOfDb(db);
            if (first) { line.startNewSubPath(x, y); fill.startNewSubPath(x, baseY); fill.lineTo(x, y); first = false; }
            else { line.lineTo(x, y); fill.lineTo(x, y); }
        }
        fill.lineTo(xOfS(xs.back()), baseY);
        fill.closeSubPath();
        g.setColour(r.c.withAlpha(0.18f));
        g.fillPath(fill);
        g.setColour(r.c.withAlpha(0.85f));
        g.strokePath(line, juce::PathStrokeType(1.2f));
    }

    // the combined response (the original, lowered by the boost, plus every resonance), as an outline
    const double dry = std::pow(10.0, -0.17 * boost / 20.0);
    juce::Path sum;
    bool first = true;
    for (double s : xs) {
        double p = dry * dry;
        for (const auto& r : res) { const double a = std::pow(10.0, (r.db + responseDb(s - r.semis, q, stages)) / 20.0); p += a * a; }
        const float x = xOfS(s), y = yOfDb(std::max(10.0 * std::log10(p), kBottomDb));
        if (first) { sum.startNewSubPath(x, y); first = false; } else sum.lineTo(x, y);
    }
    g.setColour(col::text.withAlpha(0.55f));
    g.strokePath(sum, juce::PathStrokeType(1.0f));
    // the original
    {
        const float y = yOfDb(-0.17 * boost);
        const float dashes[] = {4.0f, 3.0f};
        g.setColour(col::dim.withAlpha(0.8f));
        g.drawDashedLine(juce::Line<float>(plot_.getX() + 28.0f, y, plot_.getRight() - 2.0f, y), dashes, 2, 1.0f);
        g.setFont(uiFont(9.0f));
        g.drawText("original", int(plot_.getRight()) - 52, int(y) + 1, 50, 10, juce::Justification::centredRight);
    }
    g.restoreState();

    // the tonic's line and diamond
    {
        const float x = xOfSemitone(0);
        g.setColour(col::text.withAlpha(tonic_ >= 0.0f ? 0.5f : 0.2f));
        g.drawVerticalLine(int(x), plot_.getY(), plot_.getBottom());
        const auto c = beadCentre(-1);
        juce::Path d;
        d.addQuadrilateral(c.x, c.y - 8, c.x + 6, c.y, c.x, c.y + 8, c.x - 6, c.y);
        g.setColour(tonicOn ? col::text : col::faint);
        if (tonicOn) g.fillPath(d); else g.strokePath(d, juce::PathStrokeType(1.2f));
        if (hover_ == -1 || drag_ == -1) { g.setColour(col::text); g.strokePath(d, juce::PathStrokeType(2.0f)); }
    }
    // the beads, labelled with their interval
    g.setFont(uiFont(10.0f, true));
    for (int k = 0; k < nf; ++k) {
        const auto c = beadCentre(k);
        const bool hot = hover_ == k || drag_ == k;
        const auto colr = trackColour(size_t(k));
        if (hot) { g.setColour(colr.withAlpha(0.3f)); g.fillEllipse(c.x - kBeadR - 4, c.y - kBeadR - 4, 2 * (kBeadR + 4), 2 * (kBeadR + 4)); }
        g.setColour(colr);
        g.fillEllipse(c.x - kBeadR, c.y - kBeadR, 2 * kBeadR, 2 * kBeadR);
        g.setColour(col::black);
        g.drawText(juce::String(k + 1), juce::Rectangle<float>(c.x - kBeadR, c.y - kBeadR, 2 * kBeadR, 2 * kBeadR).toNearestInt(), juce::Justification::centred);
        g.setColour(colr.brighter(0.3f));
        const auto name = intervalName(int(std::lround(v_[size_t(I1 + k)])));
        g.drawText(name, juce::Rectangle<float>(c.x - 26, c.y - kBeadR - 15, 52, 12).toNearestInt(), juce::Justification::centred);
    }
    // readout (top left) and the sounding chord (top right)
    g.setColour(col::text);
    g.setFont(uiFont(11.0f, true));
    g.drawText(readout_, juce::Rectangle<float>(plot_.getX() + 30, plot_.getY() + 3, plot_.getWidth() * 0.55f, 14).toNearestInt(), juce::Justification::centredLeft, true);
    if (tonic_ >= 0.0f) {
        juce::String chord;
        if (tonicOn) chord << noteName(std::round(tonic_)) << "  ";
        for (int k = 0; k < nf; ++k) chord << noteName(std::round(tonic_) + std::lround(v_[size_t(I1 + k)])) << "  ";
        g.setColour(col::dim);
        g.setFont(uiFont(11.0f));
        g.drawText(chord.trim(), juce::Rectangle<float>(plot_.getCentreX(), plot_.getY() + 3, plot_.getWidth() * 0.5f - 4, 14).toNearestInt(), juce::Justification::centredRight, true);
    }
}

void ChordResonatorView::drawRuler(juce::Graphics& g) {
    // one cell per semitone, shaded by how consonant the interval is; octaves labelled
    for (int s = kLoSemi; s <= kHiSemi; ++s) {
        const int m = ((s % 12) + 12) % 12;
        const float a = m == 0 ? 0.55f : (m == 3 || m == 4 || m == 7) ? 0.3f : (m == 5 || m == 8 || m == 9 || m == 10 || m == 11) ? 0.15f : 0.06f;
        const float x0 = xOfSemitone(s) - plot_.getWidth() / float(kCells) * 0.5f;
        auto cell = juce::Rectangle<float>(x0, ruler_.getY() + 1, plot_.getWidth() / float(kCells), 10.0f).reduced(0.5f, 0.0f);
        g.setColour(col::text.withAlpha(a));
        g.fillRect(cell);
        if (s == rulerHover_) { g.setColour(col::accent); g.drawRect(cell, 1.0f); }
    }
    g.setFont(uiFont(9.0f));
    g.setColour(col::dim);
    static const struct { int s; const char* t; } labels[] = {{-12, "-1 oct"}, {0, "tonic"}, {12, "+1 oct"}, {24, "+2 oct"}, {36, "+3 oct"}};
    for (const auto& l : labels) {
        const float x = xOfSemitone(l.s);
        auto r = juce::Rectangle<float>(x - 22, ruler_.getY() + 11, 44, 10);
        if (r.getX() < ruler_.getX()) r.setX(ruler_.getX());
        if (r.getRight() > ruler_.getRight()) r.setX(ruler_.getRight() - r.getWidth());
        g.drawText(l.t, r.toNearestInt(), juce::Justification::centred);
    }
}

// ---- mouse ----

void ChordResonatorView::mouseMove(const juce::MouseEvent& e) {
    const auto p = e.position;
    const int h = plot_.contains(p) ? hit(p) : -2;
    const int rh = ruler_.contains(p) ? semitoneAt(p.x) : -1000;
    if (h != hover_ || rh != rulerHover_) {
        hover_ = h; rulerHover_ = rh;
        if (rh > -1000) { readout_ = "Add a filter: " + intervalName(rh); }
        else updateReadout(h);
        setMouseCursor(h > -2 ? juce::MouseCursor::UpDownLeftRightResizeCursor : rh > -1000 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}
void ChordResonatorView::mouseExit(const juce::MouseEvent&) {
    if (drag_ != -2) return;
    hover_ = -2; rulerHover_ = -1000;
    updateReadout(-2);
    repaint();
}

void ChordResonatorView::mouseDown(const juce::MouseEvent& e) {
    const auto p = e.position;
    if (ruler_.contains(p) && !e.mods.isPopupMenu()) { addFilterAt(semitoneAt(p.x)); return; }
    const int h = plot_.contains(p) ? hit(p) : -2;
    if (h == -2) return;
    if (e.mods.isPopupMenu()) {
        if (h < 0) return;
        juce::PopupMenu m;
        m.addItem(1, "Remove filter", activeFilters() > 1);
        m.addItem(2, "Level to 0 dB");
        m.showMenuAsync(juce::PopupMenu::Options(), [this, h](int r) {
            if (r == 1) removeFilter(h);
            else if (r == 2) set(L1 + h, 0.0);
        });
        return;
    }
    drag_ = h;
    dragMoved_ = false;
    model_.beginGesture("chord resonator");
    if (h == -1 && v_[Tonic] < 0.5) set(Tonic, 1.0);       // a click on the ghost diamond switches the tonic on
    updateReadout(h);
    repaint();
}

void ChordResonatorView::mouseDrag(const juce::MouseEvent& e) {
    if (drag_ == -2) return;
    dragMoved_ = true;
    const juce::Point<float> p(std::clamp(e.position.x, plot_.getX(), plot_.getRight()), std::clamp(e.position.y, plot_.getY(), plot_.getBottom()));
    if (drag_ == -1) {
        set(TonicLevel, levelAt(p.y, v_[Boost], TonicLevel));
    } else if (drag_ < activeFilters()) {
        set(I1 + drag_, double(semitoneAt(p.x)));
        set(L1 + drag_, levelAt(p.y, v_[Boost], L1 + drag_));
    }
    updateReadout(drag_);
    repaint();
}

void ChordResonatorView::mouseUp(const juce::MouseEvent&) {
    if (drag_ == -2) return;
    drag_ = -2;
    model_.endGesture();
    repaint();
}

void ChordResonatorView::mouseDoubleClick(const juce::MouseEvent& e) {
    const auto p = e.position;
    if (!plot_.contains(p)) return;
    const int h = hit(p);
    if (h >= 0) removeFilter(h);
    else if (h == -1) { if (v_[Tonic] >= 0.5) set(Tonic, 0.0); }
    else addFilterAt(semitoneAt(p.x));
}

// ---- edits ----

void ChordResonatorView::set(int idx, double v) {
    const auto& s = info_.params[size_t(idx)];
    v = std::clamp(v, double(s.min), double(s.max));
    if (s.curve == Curve::Stepped) v = std::round(v);
    if (std::abs(v - v_[size_t(idx)]) < 1e-9) return;
    v_[size_t(idx)] = v;
    model_.apply(document::cmd::setParam(uid_, s.key, v));
}

void ChordResonatorView::setGroup(const std::string& label, const std::vector<std::pair<int, double>>& changes) {
    std::vector<document::Command> cs;
    for (auto [idx, v] : changes) cs.push_back(document::cmd::setParam(uid_, info_.params[size_t(idx)].key, v));
    model_.applyGroup(label, cs);
}

void ChordResonatorView::addFilterAt(int semis) {
    const int nf = activeFilters();
    if (nf >= kFilters) return;
    setGroup("add resonance", {{I1 + nf, double(std::clamp(semis, kLoSemi, kHiSemi))}, {L1 + nf, 0.0}, {Filters, double(nf + 1)}});
}

void ChordResonatorView::removeFilter(int k) {
    const int nf = activeFilters();
    if (nf <= 1 || k < 0 || k >= nf) return;
    std::vector<std::pair<int, double>> ch;
    for (int j = k; j + 1 < nf; ++j) { ch.push_back({I1 + j, v_[size_t(I1 + j + 1)]}); ch.push_back({L1 + j, v_[size_t(L1 + j + 1)]}); }
    ch.push_back({Filters, double(nf - 1)});
    setGroup("remove resonance", ch);
    hover_ = -2;
    updateReadout(-2);
}

}  // namespace ddaw::ui
