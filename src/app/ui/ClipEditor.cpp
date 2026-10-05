#include "app/ui/ClipEditor.h"

#include <algorithm>
#include <cstdio>

#include "project/ProjectJson.h"

namespace ddaw::ui {

namespace {
const ParamSpec kGainSpec{0, "gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, false};
const ParamSpec kPitchSpec{0, "pitch", -24.0f, 24.0f, 0.0f, Curve::Stepped, 0, false};
}  // namespace

ClipEditor::ClipEditor(app::AppModel& m) : View(m), gainKnob_(kGainSpec, "Gain dB"), pitchKnob_(kPitchSpec, "Pitch") {
    setWantsKeyboardFocus(true);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&gridChip_, &lenBox_, &gainKnob_, &pitchKnob_, &loopChip_, &revChip_, &velChip_, &slideChip_, &pressChip_, &bendChip_, &drawChip_, &clearChip_}) addAndMakeVisible(c);
    velChip_.onClick = [this] { setLane(Lane::Velocity); };
    slideChip_.onClick = [this] { setLane(Lane::Slide); };
    pressChip_.onClick = [this] { setLane(Lane::Pressure); };
    bendChip_.onClick = [this] { setLane(Lane::Bend); };
    drawChip_.setToggleable(true);
    drawChip_.onToggle = [this](bool on) { draw_ = on; };
    clearChip_.onClick = [this] { clearCurves(); };
    velChip_.setOn(true);
    loopChip_.setToggleable(true);
    revChip_.setToggleable(true);
    gridChip_.onClick = [this] { cycleGrid(); };
    lenBox_.onChange = [this](double bars) {
        if (!ref_.valid()) return;
        try { model.apply(app::edit::setClipLength(ref_, model.project(), bars * 384.0)); } catch (const std::exception&) {}
    };
    gainKnob_.onChange = [this](double v) { commitAudio([v](project::AudioClipData& a) { a.gainDb = v; }); };
    pitchKnob_.onChange = [this](double v) { commitAudio([v](project::AudioClipData& a) { a.pitch = v; }); };
    loopChip_.onToggle = [this](bool on) { commitAudio([on](project::AudioClipData& a) { a.loop = on ? 1.0 : 0.0; }); };
    revChip_.onToggle = [this](bool on) { commitAudio([on](project::AudioClipData& a) { a.rev = on ? 1.0 : 0.0; }); };
    refresh(app::ModelEvent::Selection);
}

project::Uid ClipEditor::trackUid() const {
    const auto& p = model.project();
    if (ref_.arrKey.empty()) return ref_.track;
    auto it = p.arr.find(ref_.arrKey);
    const auto* t = it == p.arr.end() ? nullptr : app::edit::findTrackById(p, it->second.trackId);
    return t ? t->uid : 0;
}

const project::Clip* ClipEditor::clip() const { return ref_.valid() ? app::edit::findClip(model.project(), ref_) : nullptr; }

void ClipEditor::commitAudio(const std::function<void(project::AudioClipData&)>& f) {
    const auto* c = clip();
    if (!c || !c->audio) return;
    project::Clip n = *c;
    f(*n.audio);
    if (!ref_.arrKey.empty()) {
        const auto& ac = model.project().arr.at(ref_.arrKey);
        model.apply({"arrclip.set", {{"key", ref_.arrKey}, {"arr", {{"trackId", ac.trackId}, {"start", ac.start}, {"clip", project::clipToJson(n)}}}}});
    } else {
        model.apply({"clip.set", {{"track", ref_.track}, {"scene", ref_.scene}, {"clip", project::clipToJson(n)}}});
    }
}

void ClipEditor::cycleGrid() {
    static const double g[] = {0, 96, 48, 24, 12};
    static const char* n[] = {"Auto", "1/4", "1/8", "1/16", "1/32"};
    size_t i = 0;
    for (size_t k = 0; k < std::size(g); ++k) if (g[k] == gridTicks_) i = k;
    i = (i + 1) % std::size(g);
    gridTicks_ = g[i];
    gridChip_.setText(juce::String("Grid: ") + n[i]);
    repaint();
}

void ClipEditor::refresh(app::ModelEvent e) {
    const auto& sel = model.selection();
    const bool changedRef = !(ref_ == sel.clip);
    ref_ = sel.clip;
    const auto* c = clip();
    const auto* tr = ref_.arrKey.empty() ? app::edit::findTrack(model.project(), ref_.track) : nullptr;
    const project::Track* track = tr;
    if (!track && !ref_.arrKey.empty()) {
        auto it = model.project().arr.find(ref_.arrKey);
        if (it != model.project().arr.end()) track = app::edit::findTrackById(model.project(), it->second.trackId);
    }
    drum_ = track && track->kind == project::TrackKind::Drum;
    const bool audio = c && c->audio;
    for (juce::Component* k : std::initializer_list<juce::Component*>{&gainKnob_, &pitchKnob_, &loopChip_, &revChip_}) k->setVisible(audio);
    gridChip_.setVisible(c && !audio);
    const bool notes = c && !audio;
    velChip_.setVisible(notes);
    for (juce::Component* k : std::initializer_list<juce::Component*>{&slideChip_, &pressChip_, &bendChip_}) k->setVisible(notes && !drum_);   // drum pads take no expression
    drawChip_.setVisible(notes && !drum_ && lane_ != Lane::Velocity);
    clearChip_.setVisible(notes && !drum_ && lane_ != Lane::Velocity);
    if (drum_ && lane_ != Lane::Velocity) setLane(Lane::Velocity);
    lenBox_.setVisible(c != nullptr);
    if (c) {
        lenBox_.setValue(c->len / 384.0);
        if (audio) {
            gainKnob_.setValue(c->audio->gainDb);
            pitchKnob_.setValue(c->audio->pitch);
            loopChip_.setOn(c->audio->loop != 0.0);
            revChip_.setOn(c->audio->rev != 0.0);
        }
        // drop selected ids that no longer exist
        std::set<project::Uid> keep;
        for (auto& n : c->notes) if (sel_.count(n.uid)) keep.insert(n.uid);
        sel_ = std::move(keep);
    } else sel_.clear();
    if (changedRef) { sel_.clear(); selPtNote_ = 0; exprIdx_ = -1; if (c) centreOnNotes(); }
    if (exprIdx_ >= 0) {   // the selected point must still exist
        const auto* sn = noteById(selPtNote_);
        if (!sn || size_t(exprIdx_) >= curveOf(*sn).size()) { selPtNote_ = 0; exprIdx_ = -1; }
    }
    (void)e;
    repaint();
}

void ClipEditor::centreOnNotes() {
    const auto* c = clip();
    scale_.originTick = 0;
    if (!c || drum_) { scrollY_ = 0; if (!c) return; }
    if (c->notes.empty()) { scrollY_ = double(127 - 72) * rowH(); return; }
    int lo = 127, hi = 0;
    for (auto& n : c->notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
    const double mid = (lo + hi) / 2.0;
    scrollY_ = drum_ ? 0.0 : std::max(0.0, (127 - mid) * rowH() - gridRect().getHeight() / 2.0);
    // fit the clip horizontally when it is short
    const double w = std::max(200, gridRect().getWidth());
    scale_.pxPerBeat = std::clamp(w / (c->len / 96.0) * 0.95, 12.0, 200.0);
}

void ClipEditor::resized() {
    auto top = getLocalBounds().removeFromTop(kTopH).reduced(6, 4);
    top.removeFromLeft(220);
    gridChip_.setBounds(top.removeFromLeft(92));
    top.removeFromLeft(8);
    lenBox_.setBounds(top.removeFromLeft(96));
    top.removeFromLeft(14);
    auto lanes = top;   // note clips show the lane chips where audio clips show LOOP / REV
    loopChip_.setBounds(top.removeFromLeft(54));
    top.removeFromLeft(6);
    revChip_.setBounds(top.removeFromLeft(48));
    velChip_.setBounds(lanes.removeFromLeft(40));
    lanes.removeFromLeft(4);
    slideChip_.setBounds(lanes.removeFromLeft(48));
    lanes.removeFromLeft(4);
    pressChip_.setBounds(lanes.removeFromLeft(48));
    lanes.removeFromLeft(4);
    bendChip_.setBounds(lanes.removeFromLeft(46));
    lanes.removeFromLeft(10);
    drawChip_.setBounds(lanes.removeFromLeft(46));
    lanes.removeFromLeft(4);
    clearChip_.setBounds(lanes.removeFromLeft(46));
    layoutAudio();
}

void ClipEditor::layoutAudio() {
    auto r = juce::Rectangle<int>(20, kTopH + 14, 64, 76);
    gainKnob_.setBounds(r);
    pitchKnob_.setBounds(r.translated(76, 0));
}

juce::Rectangle<int> ClipEditor::gridRect() const { return {kKeysW, kTopH, std::max(0, getWidth() - kKeysW), std::max(0, getHeight() - kTopH - laneH())}; }
juce::Rectangle<int> ClipEditor::laneRect() const { return {kKeysW, gridRect().getBottom(), std::max(0, getWidth() - kKeysW), laneH()}; }
int ClipEditor::rowTop(int pitch) const { return gridRect().getY() + int(std::round((topPitch() - pitch) * rowH() - scrollY_)); }
int ClipEditor::pitchAt(int y) const { return topPitch() - int(std::floor((y - gridRect().getY() + scrollY_) / rowH())); }

juce::Rectangle<int> ClipEditor::noteRect(const project::Note& n) const {
    const int x0 = kKeysW + int(std::round(scale_.toX(n.startTicks)));
    const int x1 = kKeysW + int(std::round(scale_.toX(n.startTicks + n.durTicks)));
    return {x0, rowTop(n.pitch), std::max(4, x1 - x0 - 1), int(rowH()) - 1};
}

ClipEditor::NoteHit ClipEditor::noteAt(juce::Point<int> p) const {
    const auto* c = clip();
    NoteHit h;
    if (!c) return h;
    for (auto it = c->notes.rbegin(); it != c->notes.rend(); ++it) {
        const auto r = noteRect(*it);
        if (r.contains(p)) { h.uid = it->uid; h.edge = p.x >= r.getRight() - 6 && r.getWidth() > 12; return h; }
    }
    return h;
}

void ClipEditor::selectAll() {
    sel_.clear();
    if (const auto* c = clip()) for (auto& n : c->notes) sel_.insert(n.uid);
    repaint();
}

void ClipEditor::tick() {
    const auto* c = clip();
    double pos = -1;
    const auto m = model.meters();
    if (c && m.playing) {
        const auto& p = model.project();
        if (!ref_.arrKey.empty()) {
            auto it = p.arr.find(ref_.arrKey);
            if (it != p.arr.end() && model.arrangementMode() && m.playheadTicks >= it->second.start && m.playheadTicks < it->second.start + c->len) pos = m.playheadTicks - it->second.start;
        } else {
            for (size_t i = 0; i < p.tracks.size(); ++i)
                if (p.tracks[i].uid == ref_.track && m.trackScene[i] >= 0 && size_t(m.trackScene[i]) < p.scenes.size() && p.scenes[size_t(m.trackScene[i])] == ref_.scene && m.playheadTicks >= m.trackAnchor[i])
                    pos = std::fmod(m.playheadTicks - m.trackAnchor[i], std::max(c->len, 1.0));
        }
    }
    if (std::abs(pos - playPos_) > 0.4) { playPos_ = pos; repaint(); }
}

void ClipEditor::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    const auto* c = clip();
    g.setColour(col::panel);
    g.fillRect(0, 0, getWidth(), kTopH);
    g.setColour(col::line);
    g.drawHorizontalLine(kTopH - 1, 0, float(getWidth()));
    if (!c) {
        g.setColour(col::dim);
        g.setFont(uiFont(14.0f));
        g.drawText("Double-click a clip slot to create a clip, or select a clip to edit it", getLocalBounds().withTrimmedTop(kTopH), juce::Justification::centred);
        return;
    }
    const auto& p = model.project();
    const project::Track* track = nullptr;
    if (ref_.arrKey.empty()) track = app::edit::findTrack(p, ref_.track);
    else if (auto it = p.arr.find(ref_.arrKey); it != p.arr.end()) track = app::edit::findTrackById(p, it->second.trackId);
    size_t ti = 0;
    for (size_t i = 0; i < p.tracks.size(); ++i) if (track && p.tracks[i].uid == track->uid) ti = i;
    const auto colr = trackColour(ti);
    g.setColour(col::text);
    g.setFont(uiFont(12.5f, true));
    g.drawText(juce::String(track ? track->name : "") + (ref_.arrKey.empty() ? "  /  " + juce::String(ref_.scene) : "  /  arrangement"), juce::Rectangle<int>(10, 0, 210, kTopH), juce::Justification::centredLeft, true);

    if (c->audio) {
        const auto buf = model.sampleBank()->get(c->audio->sampleId);
        auto area = juce::Rectangle<int>(200, kTopH + 10, getWidth() - 220, getHeight() - kTopH - 20);
        fillRounded(g, area.toFloat(), col::black, 4.0f);
        g.setColour(col::dim);
        g.setFont(uiFont(12.0f));
        g.drawText(juce::String(c->audio->sampleName.empty() ? c->audio->sampleId : c->audio->sampleName) + (buf ? juce::String::formatted("   %.2f s  %.0f Hz", buf->duration(), double(buf->sampleRate)) : juce::String("   (sample missing)")),
                   area.removeFromTop(20).reduced(8, 0), juce::Justification::centredLeft);
        if (buf && buf->frames() > 0) {
            g.setColour(colr);
            const int w = area.getWidth() - 16;
            for (int x = 0; x < w; ++x) {
                const size_t a = size_t(double(x) / w * double(buf->frames())), z = std::max(a + 1, size_t(double(x + 1) / w * double(buf->frames())));
                float mx = 0;
                for (size_t i = a; i < z && i < buf->frames(); i += std::max<size_t>(1, (z - a) / 24)) mx = std::max(mx, std::abs(buf->l[i]));
                const float h = std::max(1.0f, mx * float(area.getHeight()) * 0.45f);
                g.fillRect(float(area.getX() + 8 + x), float(area.getCentreY()) - h, 1.0f, h * 2);
            }
        }
        return;
    }

    const auto gr = gridRect();
    juce::Graphics::ScopedSaveState ss(g);
    {
        juce::Graphics::ScopedSaveState s2(g);
        g.reduceClipRegion(gr);
        // rows
        const int lo = std::max(0, pitchAt(gr.getBottom())), hi = std::min(topPitch(), pitchAt(gr.getY()) + 1);
        for (int pitch = lo; pitch <= hi; ++pitch) {
            const int y = rowTop(pitch);
            const bool black = !drum_ && app::isBlackKey(pitch);
            g.setColour(black ? col::bg.darker(0.15f) : col::bg.brighter(0.025f));
            g.fillRect(gr.getX(), y, gr.getWidth(), int(rowH()));
            g.setColour(pitch % 12 == 0 && !drum_ ? col::gridBar : col::grid);
            g.fillRect(gr.getX(), y + int(rowH()) - 1, gr.getWidth(), 1);
        }
        // columns
        const double step = grid();
        const double firstTick = std::floor(scale_.originTick / step) * step;
        for (double t = firstTick; scale_.toX(t) < gr.getWidth(); t += step) {
            const long tl = long(std::round(t));
            const bool bar = tl % 384 == 0, beat = tl % 96 == 0;
            g.setColour(bar ? col::gridBar.brighter(0.25f) : beat ? col::gridBar : col::grid);
            g.fillRect(kKeysW + int(std::round(scale_.toX(t))), gr.getY(), 1, gr.getHeight());
        }
        // region after the clip end is dimmed
        const int endX = kKeysW + int(std::round(scale_.toX(c->len)));
        g.setColour(col::black.withAlpha(0.45f));
        g.fillRect(std::max(endX, gr.getX()), gr.getY(), std::max(0, gr.getRight() - endX), gr.getHeight());
        g.setColour(col::accent.withAlpha(0.8f));
        g.fillRect(endX, gr.getY(), 1, gr.getHeight());

        // notes (with the drag preview applied to the selection)
        for (const auto& n : c->notes) {
            project::Note d = n;
            if (sel_.count(n.uid) && moved_) {
                if (mode_ == Mode::Move) { d.startTicks = std::max(0.0, n.startTicks + dTicks_); d.pitch = std::clamp(n.pitch + dPitch_, drum_ ? 0 : 0, drum_ ? 7 : 127); }
                else if (mode_ == Mode::Resize) d.durTicks = std::max(6.0, n.durTicks + dDur_);
            }
            if (mode_ == Mode::Velocity && n.uid == velUid_) d.velocity = velValue_;
            const bool hasExpr = !n.bend.empty() || !n.slide.empty() || !n.pressure.empty();
            auto r = noteRect(d);
            if (!r.intersects(gr)) continue;
            const bool s = sel_.count(n.uid) != 0;
            const float v = float(d.velocity);
            auto fill = colr.withMultipliedBrightness(0.55f + 0.5f * v);
            fillRounded(g, r.toFloat(), fill, 2.0f);
            if (s) { g.setColour(col::text); g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 2.0f, 1.5f); }
            else { g.setColour(col::black.withAlpha(0.5f)); g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 2.0f, 1.0f); }
            if (hasExpr && r.getWidth() >= 8) { g.setColour(col::text.withAlpha(0.85f)); g.fillRect(r.getX() + 2, r.getBottom() - 3, std::min(r.getWidth() - 4, 10), 2); }   // carries expression curves
        }
        if (mode_ == Mode::Marquee) {
            g.setColour(col::accent.withAlpha(0.18f));
            g.fillRect(marquee_);
            g.setColour(col::accent);
            g.drawRect(marquee_, 1);
        }
        if (playPos_ >= 0) {
            g.setColour(col::accent);
            g.fillRect(kKeysW + int(std::round(scale_.toX(playPos_))), gr.getY(), 2, gr.getHeight());
        }
    }

    // keys
    {
        juce::Graphics::ScopedSaveState s3(g);
        g.reduceClipRegion(0, gr.getY(), kKeysW, gr.getHeight());
        const int lo = std::max(0, pitchAt(gr.getBottom())), hi = std::min(topPitch(), pitchAt(gr.getY()) + 1);
        for (int pitch = lo; pitch <= hi; ++pitch) {
            auto kr = juce::Rectangle<int>(0, rowTop(pitch), kKeysW - 1, int(rowH()));
            const bool black = !drum_ && app::isBlackKey(pitch);
            g.setColour(pitch == keyDown_ ? col::accent : black ? col::black : col::text.withMultipliedBrightness(0.82f));
            g.fillRect(kr.withTrimmedBottom(1).withTrimmedRight(black ? 14 : 0));
            if (drum_) { g.setColour(col::black); g.setFont(uiFont(11.0f)); g.drawText(app::drumPadName(pitch), kr.reduced(4, 0), juce::Justification::centredLeft); }
            else if (pitch % 12 == 0) { g.setColour(col::black); g.setFont(uiFont(10.0f)); g.drawText(app::noteName(pitch), kr.reduced(4, 0), juce::Justification::centredRight); }
        }
    }
    g.setColour(col::line);
    g.drawVerticalLine(kKeysW - 1, float(gr.getY()), float(gr.getBottom()));

    // the lane under the notes
    {
        const auto vr = laneRect();
        g.setColour(col::panel);
        g.fillRect(juce::Rectangle<int>(0, vr.getY(), getWidth(), vr.getHeight()));
        g.setColour(col::line);
        g.drawHorizontalLine(vr.getY(), 0, float(getWidth()));
        g.setColour(col::dim);
        g.setFont(uiFont(10.5f));
        static const char* names[] = {"Velocity", "Slide", "Pressure", "Pitch bend"};
        g.drawText(names[int(lane_)], juce::Rectangle<int>(4, vr.getY(), kKeysW - 8, 18), juce::Justification::centredLeft);
        if (lane_ == Lane::Velocity) {
            juce::Graphics::ScopedSaveState s4(g);
            g.reduceClipRegion(vr);
            for (const auto& n : c->notes) {
                const double vel = (mode_ == Mode::Velocity && n.uid == velUid_) ? velValue_ : n.velocity;
                const int x = kKeysW + int(std::round(scale_.toX(n.startTicks)));
                const int h = int(vel * (kVelH - 8));
                g.setColour(sel_.count(n.uid) ? col::text : colr);
                g.fillRect(x, vr.getBottom() - 4 - h, 3, h);
                g.fillEllipse(float(x) - 2.0f, float(vr.getBottom() - 4 - h) - 3.0f, 7.0f, 7.0f);
            }
        } else drawLane(g, *c, colr);
    }
}

// ---- expression lanes (slide, pressure, pitch bend): per-note curves, edited with points ----

const std::vector<project::ExprPoint>& ClipEditor::curveOf(const project::Note& n) const {
    static const std::vector<project::ExprPoint> none;
    switch (lane_) {
        case Lane::Slide: return n.slide;
        case Lane::Pressure: return n.pressure;
        case Lane::Bend: return n.bend;
        default: return none;
    }
}
std::vector<project::ExprPoint>& ClipEditor::curveOf(project::Note& n) const {
    return const_cast<std::vector<project::ExprPoint>&>(curveOf(static_cast<const project::Note&>(n)));
}

const project::Note* ClipEditor::noteById(project::Uid uid) const {
    const auto* c = clip();
    if (!c || !uid) return nullptr;
    for (auto& n : c->notes) if (n.uid == uid) return &n;
    return nullptr;
}

double ClipEditor::bendSpan() const {
    double m = 0;
    if (const auto* c = clip()) for (auto& n : c->notes) for (auto& p : n.bend) m = std::max(m, std::abs(p.v));
    return m <= 12.0 ? 12.0 : m <= 24.0 ? 24.0 : m <= 48.0 ? 48.0 : 96.0;
}

int ClipEditor::laneY(double value) const {
    const auto r = laneRect();
    const double f = (std::clamp(value, laneMin(), laneMax()) - laneMin()) / (laneMax() - laneMin());
    return r.getBottom() - 8 - int(std::round(f * (r.getHeight() - 16)));
}
double ClipEditor::laneValue(int y) const {
    const auto r = laneRect();
    const double f = std::clamp(double(r.getBottom() - 8 - y) / double(r.getHeight() - 16), 0.0, 1.0);
    return laneMin() + f * (laneMax() - laneMin());
}
int ClipEditor::laneX(const project::Note& n, double tRel) const { return kKeysW + int(std::round(scale_.toX(n.startTicks + tRel))); }

project::Uid ClipEditor::noteAtTime(double tick) const {
    const auto* c = clip();
    project::Uid pick = 0;
    if (!c) return 0;
    for (auto& n : c->notes) {
        if (tick < n.startTicks - 1e-9 || tick > n.startTicks + n.durTicks + 1e-9) continue;
        if (sel_.count(n.uid)) return n.uid;   // a selected note wins over the ones it overlaps
        pick = n.uid;                          // else the last one (drawn on top)
    }
    return pick;
}

ClipEditor::PointHit ClipEditor::pointAt(juce::Point<int> p) const {
    const auto* c = clip();
    PointHit best;
    if (!c) return best;
    double bd = 1e9;
    for (auto& n : c->notes) {
        const auto& pts = curveOf(n);
        for (size_t i = 0; i < pts.size(); ++i) {
            const double dx = laneX(n, pts[i].t) - p.x, dy = laneY(pts[i].v) - p.y;
            const double d = std::hypot(dx, dy) - (sel_.count(n.uid) ? 1.5 : 0.0);   // ties go to the selected note
            if (std::abs(dx) <= 7 && std::abs(dy) <= 7 && d < bd) { bd = d; best = {n.uid, int(i)}; }
        }
    }
    return best;
}

void ClipEditor::setLane(Lane l) {
    lane_ = l;
    velChip_.setOn(l == Lane::Velocity);
    slideChip_.setOn(l == Lane::Slide);
    pressChip_.setOn(l == Lane::Pressure);
    bendChip_.setOn(l == Lane::Bend);
    drawChip_.setVisible(l != Lane::Velocity && !drum_ && clip() != nullptr);
    clearChip_.setVisible(l != Lane::Velocity && !drum_ && clip() != nullptr);
    mode_ = Mode::None;
    selPtNote_ = 0;
    exprIdx_ = -1;
    resized();
    repaint();
}

void ClipEditor::setDraw(bool on) { draw_ = on; drawChip_.setOn(on); }

namespace {
// Snap a lane value: whole hundredths for slide and pressure, tenths of a semitone for bend with a pull to zero; Shift = free.
double snapLane(double v, bool bend, double span, bool free) {
    if (free) return v;
    if (bend) return std::abs(v) < span * 0.03 ? 0.0 : std::round(v * 10.0) / 10.0;
    return std::round(v * 100.0) / 100.0;
}
}  // namespace

void ClipEditor::exprDown(const juce::MouseEvent& e) {
    const auto* c = clip();
    if (!c) return;
    const bool bend = lane_ == Lane::Bend;
    const auto hit = pointAt(e.getPosition());
    auto select = [&](project::Uid uid) {
        if (!sel_.count(uid)) { if (!e.mods.isShiftDown()) sel_.clear(); sel_.insert(uid); }
    };
    if (hit.note) {
        if (e.mods.isPopupMenu() || e.mods.isAltDown()) { deletePoint(hit.note, hit.index); return; }
        select(hit.note);
        work_ = *noteById(hit.note);
        selPtNote_ = hit.note;
        exprIdx_ = hit.index;
        mode_ = Mode::ExprPoint;
        exprDirty_ = false;
        repaint();
        return;
    }
    const double tick = scale_.toTick(double(e.x - kKeysW));
    const auto uid = noteAtTime(tick);
    exprIdx_ = -1;
    selPtNote_ = 0;
    if (!uid) { repaint(); return; }
    select(uid);
    work_ = *noteById(uid);
    const double t = std::round(std::clamp(tick - work_.startTicks, 0.0, work_.durTicks));
    const double v = snapLane(laneValue(e.y), bend, bendSpan(), e.mods.isShiftDown());
    auto& pts = curveOf(work_);
    if (draw_) {
        mode_ = Mode::ExprDraw;
        drawLastT_ = t;
        pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const project::ExprPoint& q) { return std::abs(q.t - t) < 1.0; }), pts.end());
        pts.insert(std::lower_bound(pts.begin(), pts.end(), t, [](const project::ExprPoint& q, double tt) { return q.t < tt; }), {t, v});
    } else {
        mode_ = Mode::ExprPoint;
        const auto it = pts.insert(std::lower_bound(pts.begin(), pts.end(), t, [](const project::ExprPoint& q, double tt) { return q.t < tt; }), {t, v});
        exprIdx_ = int(it - pts.begin());
        selPtNote_ = uid;
    }
    exprDirty_ = true;
    repaint();
}

void ClipEditor::exprDrag(const juce::MouseEvent& e) {
    if (work_.uid == 0) return;
    if (e.getDistanceFromDragStart() < 3 && !exprDirty_) return;
    const bool bend = lane_ == Lane::Bend;
    auto& pts = curveOf(work_);
    const double t = std::round(std::clamp(scale_.toTick(double(e.x - kKeysW)) - work_.startTicks, 0.0, work_.durTicks));
    const double v = snapLane(laneValue(e.y), bend, bendSpan(), e.mods.isShiftDown());
    if (mode_ == Mode::ExprPoint && exprIdx_ >= 0 && size_t(exprIdx_) < pts.size()) {
        const size_t i = size_t(exprIdx_);
        const double lo = i > 0 ? pts[i - 1].t + 1.0 : 0.0, hi = i + 1 < pts.size() ? pts[i + 1].t - 1.0 : work_.durTicks;
        pts[i] = {std::clamp(t, lo, std::max(lo, hi)), v};
        exprDirty_ = true;
    } else if (mode_ == Mode::ExprDraw) {
        const double gap = std::max(2.0, 3.0 * 96.0 / scale_.pxPerBeat);   // about three pixels between points
        if (std::abs(t - drawLastT_) < gap) return;
        const double a = std::min(t, drawLastT_), b = std::max(t, drawLastT_);
        pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const project::ExprPoint& q) { return q.t >= a - 1e-9 && q.t <= b + 1e-9 && std::abs(q.t - drawLastT_) > 1e-9; }), pts.end());
        pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const project::ExprPoint& q) { return std::abs(q.t - t) < 1.0; }), pts.end());
        pts.insert(std::lower_bound(pts.begin(), pts.end(), t, [](const project::ExprPoint& q, double tt) { return q.t < tt; }), {t, v});
        drawLastT_ = t;
        exprDirty_ = true;
    }
    repaint();
}

void ClipEditor::exprUp() {
    if (exprDirty_ && work_.uid != 0) {
        static const char* lbl[] = {"", "edit slide curve", "edit pressure curve", "edit bend curve"};
        commitNotes(lbl[int(lane_)], {work_});
    }
    work_ = project::Note{};
    exprDirty_ = false;
}

void ClipEditor::deletePoint(project::Uid uid, int index) {
    const auto* n = noteById(uid);
    if (!n) return;
    project::Note w = *n;
    auto& pts = curveOf(w);
    if (index < 0 || size_t(index) >= pts.size()) return;
    pts.erase(pts.begin() + index);
    exprIdx_ = -1;
    selPtNote_ = 0;
    commitNotes("delete curve point", {w});
}

void ClipEditor::clearCurves() {
    const auto* c = clip();
    if (!c || lane_ == Lane::Velocity) return;
    std::vector<project::Note> out;
    for (auto n : c->notes) {
        if (!sel_.count(n.uid) || curveOf(n).empty()) continue;
        curveOf(n).clear();
        out.push_back(n);
    }
    exprIdx_ = -1;
    selPtNote_ = 0;
    commitNotes("clear curves", out);
}

void ClipEditor::drawLane(juce::Graphics& g, const project::Clip& c, juce::Colour colr) {
    const auto vr = laneRect();
    juce::Graphics::ScopedSaveState s4(g);
    g.reduceClipRegion(vr);
    const bool bend = lane_ == Lane::Bend;
    // value guides: the neutral line (zero bend / zero) and quarters
    g.setFont(uiFont(9.5f));
    for (int i = 0; i <= 4; ++i) {
        const double v = laneMin() + (laneMax() - laneMin()) * i / 4.0;
        const int y = laneY(v);
        g.setColour((bend && i == 2) ? col::gridBar.brighter(0.3f) : col::grid);
        g.fillRect(vr.getX(), y, vr.getWidth(), 1);
        if (i == 0 || i == 4 || (bend && i == 2)) {
            g.setColour(col::dim);
            char b[16];
            std::snprintf(b, sizeof b, bend ? "%+.0f" : "%.0f", v);
            g.drawText(b, juce::Rectangle<int>(kKeysW - 30, y - 6, 26, 12), juce::Justification::centredRight);
        }
    }
    for (const auto& n0 : c.notes) {
        const bool live = work_.uid == n0.uid && (mode_ == Mode::ExprPoint || mode_ == Mode::ExprDraw);
        const project::Note& n = live ? work_ : n0;
        const bool s = sel_.count(n.uid) != 0;
        const int x0 = laneX(n, 0), x1 = laneX(n, n.durTicks);
        if (x1 < vr.getX() || x0 > vr.getRight()) continue;
        if (s) {   // where a click lands on this note
            g.setColour(col::accent.withAlpha(0.09f));
            g.fillRect(x0, vr.getY(), std::max(1, x1 - x0), vr.getHeight());
            g.setColour(col::accent.withAlpha(0.5f));
            g.fillRect(x0, vr.getY(), 1, vr.getHeight());
            g.fillRect(x1, vr.getY(), 1, vr.getHeight());
        }
        const auto& pts = curveOf(n);
        if (pts.empty()) {
            if (s) {   // nothing recorded or drawn: the note plays with neutral expression
                g.setColour(col::dim.withAlpha(0.6f));
                const int y = laneY(bend ? 0.0 : laneMin());
                for (int x = x0; x < x1; x += 8) g.fillRect(x, y, 4, 1);
            }
            continue;
        }
        juce::Path path;
        path.startNewSubPath(float(x0), float(laneY(pts.front().v)));
        for (auto& p : pts) path.lineTo(float(laneX(n, p.t)), float(laneY(p.v)));
        path.lineTo(float(x1), float(laneY(pts.back().v)));
        g.setColour(s ? col::text : colr.withAlpha(0.7f));
        g.strokePath(path, juce::PathStrokeType(s ? 1.8f : 1.2f));
        if (s) {
            for (size_t i = 0; i < pts.size(); ++i) {
                const bool picked = n.uid == selPtNote_ && int(i) == exprIdx_;
                const float r = picked ? 4.5f : 3.5f;
                const float px = float(laneX(n, pts[i].t)), py = float(laneY(pts[i].v));
                g.setColour(picked ? col::accent : colr);
                g.fillEllipse(px - r, py - r, 2 * r, 2 * r);
                g.setColour(col::black.withAlpha(0.6f));
                g.drawEllipse(px - r, py - r, 2 * r, 2 * r, 1.0f);
            }
        }
    }
    // readout of the point being dragged
    if ((mode_ == Mode::ExprPoint) && work_.uid && exprIdx_ >= 0 && size_t(exprIdx_) < curveOf(work_).size()) {
        const auto& p = curveOf(work_)[size_t(exprIdx_)];
        char b[48];
        if (bend) std::snprintf(b, sizeof b, "%+.2f st  @ %d", p.v, int(p.t)); else std::snprintf(b, sizeof b, "%.2f  @ %d", p.v, int(p.t));
        g.setColour(col::text);
        g.setFont(uiFont(11.0f, true));
        g.drawText(b, juce::Rectangle<int>(vr.getRight() - 150, vr.getY() + 2, 146, 14), juce::Justification::centredRight);
    }
}

void ClipEditor::mouseDown(const juce::MouseEvent& e) {
    if (isShowing()) grabKeyboardFocus();
    const auto* c = clip();
    if (!c || c->audio) return;
    downPos_ = e.getPosition();
    moved_ = false;
    mode_ = Mode::None;
    const auto gr = gridRect();
    if (e.x < kKeysW && e.y >= gr.getY() && e.y < gr.getBottom()) {
        mode_ = Mode::Key;
        keyDown_ = juce::jlimit(0, topPitch(), pitchAt(e.y));
        model.audition(trackUid(), keyDown_, true);
        repaint();
        return;
    }
    if (e.y >= gr.getBottom() && e.x >= kKeysW && lane_ != Lane::Velocity) { exprDown(e); return; }
    if (e.y >= gr.getBottom() && e.x >= kKeysW) {
        // pick the note whose velocity bar is nearest
        double best = 9;
        project::Uid pick = 0;
        for (auto& n : c->notes) {
            const double d = std::abs(kKeysW + scale_.toX(n.startTicks) + 1 - e.x);
            if (d < best) { best = d; pick = n.uid; }
        }
        if (pick) {
            mode_ = Mode::Velocity;
            velUid_ = pick;
            velValue_ = juce::jlimit(0.02, 1.0, double(gr.getBottom() + kVelH - 4 - e.y) / (kVelH - 8));
            repaint();
        }
        return;
    }
    if (!gr.contains(e.getPosition())) return;
    exprIdx_ = -1;
    selPtNote_ = 0;
    const auto h = noteAt(e.getPosition());
    grabTick_ = scale_.toTick(double(e.x - kKeysW));
    grabPitch_ = pitchAt(e.y);
    if (h.uid) {
        if (e.mods.isShiftDown()) { if (sel_.count(h.uid)) sel_.erase(h.uid); else sel_.insert(h.uid); }
        else if (!sel_.count(h.uid)) { sel_.clear(); sel_.insert(h.uid); }
        mode_ = h.edge ? Mode::Resize : Mode::Move;
        // hear what you grab
        if (!drum_) { /* no audition on grab: it is noisy while editing */ }
    } else {
        if (!e.mods.isShiftDown()) sel_.clear();
        mode_ = Mode::Marquee;
        marquee_ = {downPos_, downPos_};
    }
    repaint();
}

void ClipEditor::mouseDrag(const juce::MouseEvent& e) {
    const auto* c = clip();
    if (!c || mode_ == Mode::None || mode_ == Mode::Key) return;
    if (mode_ == Mode::ExprPoint || mode_ == Mode::ExprDraw) { exprDrag(e); return; }
    if (mode_ == Mode::Velocity) {
        velValue_ = juce::jlimit(0.02, 1.0, double(gridRect().getBottom() + kVelH - 4 - e.y) / (kVelH - 8));
        repaint();
        return;
    }
    if (e.getDistanceFromDragStart() < 3 && !moved_) return;
    moved_ = true;
    const double g = e.mods.isShiftDown() ? 0.0 : grid();
    const double tickNow = scale_.toTick(double(e.x - kKeysW));
    if (mode_ == Mode::Move) {
        // snap the delta so the earliest selected note lands on the grid
        double first = 1e18;
        for (auto& n : c->notes) if (sel_.count(n.uid)) first = std::min(first, n.startTicks);
        const double target = app::edit::snapTicks(first + (tickNow - grabTick_), g);
        dTicks_ = std::max(-first, target - first);
        dPitch_ = pitchAt(e.y) - grabPitch_;
    } else if (mode_ == Mode::Resize) {
        double firstEnd = 1e18, start = 0;
        for (auto& n : c->notes) if (sel_.count(n.uid) && n.startTicks + n.durTicks < firstEnd) { firstEnd = n.startTicks + n.durTicks; start = n.startTicks; }
        const double target = std::max(g > 0 ? g : 6.0, app::edit::snapTicks(tickNow - start, g));
        dDur_ = target - (firstEnd - start);
    } else if (mode_ == Mode::Marquee) {
        marquee_ = juce::Rectangle<int>(downPos_, e.getPosition()).getIntersection(gridRect());
        sel_.clear();
        for (auto& n : c->notes) if (marquee_.intersects(noteRect(n))) sel_.insert(n.uid);
    }
    repaint();
}

void ClipEditor::mouseUp(const juce::MouseEvent&) {
    const auto* c = clip();
    if (mode_ == Mode::Key) {
        model.audition(trackUid(), keyDown_, false);
        keyDown_ = -1;
    } else if (mode_ == Mode::ExprPoint || mode_ == Mode::ExprDraw) {
        exprUp();
    } else if (c && mode_ == Mode::Velocity) {
        auto n = *std::find_if(c->notes.begin(), c->notes.end(), [&](const project::Note& x) { return x.uid == velUid_; });
        n.velocity = velValue_;
        commitNotes("velocity", {n});
    } else if (c && moved_ && (mode_ == Mode::Move || mode_ == Mode::Resize)) {
        std::vector<project::Note> out;
        for (auto n : c->notes) {
            if (!sel_.count(n.uid)) continue;
            if (mode_ == Mode::Move) { n.startTicks = std::max(0.0, n.startTicks + dTicks_); n.pitch = std::clamp(n.pitch + dPitch_, 0, drum_ ? 7 : 127); }
            else n.durTicks = std::max(6.0, n.durTicks + dDur_);
            out.push_back(n);
        }
        commitNotes(mode_ == Mode::Move ? "move notes" : "resize notes", out);
    }
    mode_ = Mode::None;
    moved_ = false;
    dTicks_ = dDur_ = 0;
    dPitch_ = 0;
    repaint();
}

void ClipEditor::commitNotes(const std::string& label, const std::vector<project::Note>& edited) {
    std::vector<document::Command> cs;
    for (auto& n : edited) cs.push_back(app::edit::editNote(ref_, n));
    if (!cs.empty()) model.applyGroup(label, cs);
}

void ClipEditor::mouseDoubleClick(const juce::MouseEvent& e) {
    const auto* c = clip();
    if (!c || c->audio || !gridRect().contains(e.getPosition())) return;
    if (noteAt(e.getPosition()).uid) return;
    const double g = grid();
    const double start = std::max(0.0, app::edit::snapFloor(scale_.toTick(double(e.x - kKeysW)), g));
    const int pitch = std::clamp(pitchAt(e.y), 0, drum_ ? 7 : 127);
    const double dur = drum_ ? std::max(g, 24.0) : g;
    if (model.apply(app::edit::addNote(ref_, pitch, start, dur))) {
        sel_.clear();
        if (const auto* nc = clip()) sel_.insert(nc->notes.back().uid);
        if (const auto uid = trackUid()) {
            model.audition(uid, pitch, true);
            juce::Timer::callAfterDelay(160, [this, uid, pitch] { model.audition(uid, pitch, false); });
        }
    }
}

void ClipEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (e.mods.isCommandDown() || e.mods.isCtrlDown()) scale_.zoomAt(double(e.x - kKeysW), std::exp(double(w.deltaY) * 1.5), 8.0, 800.0);
    else {
        scale_.originTick = std::max(0.0, scale_.originTick - double(w.deltaX) * 800.0 / scale_.pxPerBeat * 4.0);
        scrollY_ = juce::jlimit(0.0, std::max(0.0, (topPitch() + 1) * rowH() - gridRect().getHeight()), scrollY_ - double(w.deltaY) * 400.0);
    }
    repaint();
}

void ClipEditor::deleteSelected() {
    const auto* c = clip();
    if (!c || sel_.empty()) return;
    std::vector<document::Command> cs;
    for (auto& n : c->notes) if (sel_.count(n.uid)) cs.push_back(app::edit::removeNote(ref_, n.uid));
    sel_.clear();
    model.applyGroup("delete notes", cs);
}

void ClipEditor::duplicateSelected() {
    const auto* c = clip();
    if (!c || sel_.empty()) return;
    double lo = 1e18, hi = 0;
    for (auto& n : c->notes) if (sel_.count(n.uid)) { lo = std::min(lo, n.startTicks); hi = std::max(hi, n.startTicks + n.durTicks); }
    const double shift = std::ceil((hi - lo) / grid()) * grid();
    std::vector<document::Command> cs;
    for (auto& n : c->notes) if (sel_.count(n.uid)) { auto d = n; d.startTicks += shift; cs.push_back(app::edit::addNoteCopy(ref_, d)); }
    if (model.applyGroup("duplicate notes", cs)) {
        const auto* nc = clip();
        sel_.clear();
        for (size_t i = nc->notes.size() - cs.size(); i < nc->notes.size(); ++i) sel_.insert(nc->notes[i].uid);
    }
}

void ClipEditor::nudge(int dPitch, double dTicks) {
    const auto* c = clip();
    if (!c) return;
    std::vector<project::Note> out;
    for (auto n : c->notes) {
        if (!sel_.count(n.uid)) continue;
        n.pitch = std::clamp(n.pitch + dPitch, 0, drum_ ? 7 : 127);
        n.startTicks = std::max(0.0, n.startTicks + dTicks);
        out.push_back(n);
    }
    commitNotes("nudge notes", out);
}

bool ClipEditor::keyPressed(const juce::KeyPress& k) {
    if (!clip()) return false;
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) {
        if (lane_ != Lane::Velocity && exprIdx_ >= 0 && selPtNote_) deletePoint(selPtNote_, exprIdx_);   // a picked curve point goes before the notes do
        else deleteSelected();
        return true;
    }
    if (k == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)) { selectAll(); return true; }
    if (k == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)) { duplicateSelected(); return true; }
    const int oct = k.getModifiers().isShiftDown() ? 12 : 1;
    if (k.isKeyCode(juce::KeyPress::upKey)) { nudge(drum_ ? 1 : oct, 0); return true; }
    if (k.isKeyCode(juce::KeyPress::downKey)) { nudge(drum_ ? -1 : -oct, 0); return true; }
    if (k.isKeyCode(juce::KeyPress::leftKey)) { nudge(0, -grid()); return true; }
    if (k.isKeyCode(juce::KeyPress::rightKey)) { nudge(0, grid()); return true; }
    return false;
}

}  // namespace ddaw::ui
