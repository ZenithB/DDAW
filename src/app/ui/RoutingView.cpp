#include "app/ui/RoutingView.h"

#include <cmath>

namespace ddaw::ui {

RoutingView::RoutingView(app::AppModel& m) : View(m) { rebuild(); }

void RoutingView::rebuild() {
    const auto& p = model.project();
    rows_.clear(); cols_.clear();
    for (auto& t : p.tracks) if (t.kind != project::TrackKind::Bus) rows_.push_back(t.uid);
    nInputRows_ = rows_.size();
    for (auto& t : p.tracks) if (t.kind == project::TrackKind::Bus) { rows_.push_back(t.uid); cols_.push_back(t.uid); }
    cols_.push_back(0);                                                        // the master
}

void RoutingView::refresh(app::ModelEvent) { rebuild(); clampScroll(); repaint(); }

int RoutingView::rowY(size_t r) const { return kHeadH + int(r) * kCellH + (r >= nInputRows_ && nInputRows_ > 0 && rows_.size() > nInputRows_ ? kGap : 0) - scrollY_; }
int RoutingView::colX(size_t c) const { return kLabelW + int(c) * kCellW + (c + 1 == cols_.size() && cols_.size() > 1 ? kGap : 0) - scrollX_; }

void RoutingView::clampScroll() {
    const int contentH = int(rows_.size()) * kCellH + (nInputRows_ > 0 && rows_.size() > nInputRows_ ? kGap : 0), viewH = std::max(0, getHeight() - kHeadH - kFootH);
    const int contentW = int(cols_.size()) * kCellW + (cols_.size() > 1 ? kGap : 0), viewW = std::max(0, getWidth() - kLabelW);
    scrollY_ = std::clamp(scrollY_, 0, std::max(0, contentH - viewH));
    scrollX_ = std::clamp(scrollX_, 0, std::max(0, contentW - viewW));
}

juce::Rectangle<int> RoutingView::cellRect(project::Uid src, project::Uid dst) const {
    size_t r = rows_.size(), c = cols_.size();
    for (size_t i = 0; i < rows_.size(); ++i) if (rows_[i] == src) r = i;
    for (size_t i = 0; i < cols_.size(); ++i) if (cols_[i] == dst) c = i;
    if (r == rows_.size() || c == cols_.size()) return {};
    return {colX(c), rowY(r), kCellW, kCellH};
}
juce::Rectangle<int> RoutingView::cornerRect(project::Uid src, project::Uid dst) const {
    const auto r = cellRect(src, dst);
    return r.isEmpty() ? r : juce::Rectangle<int>(r.getRight() - 18, r.getY() + 2, 16, 16);
}

bool RoutingView::sendAllowed(project::Uid src, project::Uid dst) const {
    if (dst == 0 || src == dst) return false;
    for (auto* x : app::edit::sendTargets(model.project(), src)) if (x->uid == dst) return true;
    return false;
}
bool RoutingView::outputAllowed(project::Uid src, project::Uid dst) const {
    const auto* s = app::edit::findTrack(model.project(), src);
    if (!s) return false;
    if (s->kind != project::TrackKind::Bus) return dst == 0;                  // a track feeds the master and reaches buses by sends
    if (dst == 0) return true;
    for (auto* x : app::edit::outputTargets(model.project(), src)) if (x->uid == dst) return true;
    return false;
}
double RoutingView::sendLevel(project::Uid src, project::Uid dst) const {
    const auto& p = model.project();
    const auto* s = app::edit::findTrack(p, src);
    const auto* d = app::edit::findTrack(p, dst);
    if (!s || !d) return 0.0;
    const auto it = s->sends.find(d->id);
    return it == s->sends.end() ? 0.0 : it->second;
}
bool RoutingView::isOutput(project::Uid src, project::Uid dst) const {
    const auto& p = model.project();
    const auto* s = app::edit::findTrack(p, src);
    if (!s) return false;
    if (s->kind != project::TrackKind::Bus) return dst == 0;
    const bool toMaster = s->output.empty() || s->output == "master" || !app::edit::findTrackById(p, s->output);
    if (dst == 0) return toMaster;
    const auto* d = app::edit::findTrack(p, dst);
    return d && !toMaster && s->output == d->id;
}

RoutingView::Cell RoutingView::cellAt(juce::Point<int> pt) const {
    Cell c;
    for (size_t r = 0; r < rows_.size(); ++r) for (size_t k = 0; k < cols_.size(); ++k) {
        const juce::Rectangle<int> cell(colX(k), rowY(r), kCellW, kCellH);
        if (!cell.contains(pt)) continue;
        c.src = rows_[r]; c.dst = cols_[k]; c.valid = true;
        c.corner = cornerRect(c.src, c.dst).expanded(2).contains(pt);
        return c;
    }
    return c;
}

void RoutingView::describe(const Cell& c) {
    hoverText_.clear();
    if (!c.valid) return;
    const auto& p = model.project();
    const auto* s = app::edit::findTrack(p, c.src);
    const auto* d = app::edit::findTrack(p, c.dst);
    if (!s) return;
    const juce::String from = s->name, to = c.dst == 0 ? juce::String("Master") : juce::String(d ? d->name : "?");
    if (c.dst == 0) hoverText_ = from + " outputs " + (isOutput(c.src, 0) ? "to the Master" : "elsewhere") + (outputAllowed(c.src, 0) && s->kind == project::TrackKind::Bus ? "   (click the diamond to route it to the Master)" : "");
    else if (c.src == c.dst) hoverText_ = from + ": a bus cannot send to itself";
    else if (!sendAllowed(c.src, c.dst) && !outputAllowed(c.src, c.dst)) hoverText_ = to + " already feeds " + from + ": a route here would be a loop";
    else {
        const double lv = sendLevel(c.src, c.dst);
        hoverText_ = from + " > " + to + ":  send " + juce::String(lv, 2) + (lv > 0 ? "  (" + juce::String(20.0 * std::log10(lv), 1) + " dB)" : juce::String(""));
        if (s->kind == project::TrackKind::Bus) hoverText_ << (isOutput(c.src, c.dst) ? "   -   its output goes here" : outputAllowed(c.src, c.dst) ? "   -   click the diamond to make this its output" : "");
    }
    // the path of the row under the pointer: where the sound goes
    juce::String path;
    for (auto& [bus, level] : s->sends) if (level > 0) { const auto* b = app::edit::findTrackById(p, bus); path << (path.isEmpty() ? "" : ", ") << (b ? b->name : bus) << " " << juce::String(level, 2); }
    const auto* o = s->output.empty() || s->output == "master" ? nullptr : app::edit::findTrackById(p, s->output);
    hoverText_ << "     |  " << s->name << (path.isEmpty() ? "" : " sends to " + path + ";") << " main output " << (o && s->kind == project::TrackKind::Bus ? o->name : juce::String("Master"));
}

void RoutingView::mouseMove(const juce::MouseEvent& e) {
    const auto c = cellAt(e.getPosition());
    if (c.valid != hover_.valid || c.src != hover_.src || c.dst != hover_.dst || c.corner != hover_.corner) { hover_ = c; describe(c); repaint(); }
}

void RoutingView::mouseDown(const juce::MouseEvent& e) {
    const auto c = cellAt(e.getPosition());
    drag_ = {};
    dragged_ = false;
    if (!c.valid) return;
    const auto& p = model.project();
    const auto* s = app::edit::findTrack(p, c.src);
    if (!s) return;
    if (e.mods.isPopupMenu()) {                                                        // clear the send
        if (c.dst != 0 && sendLevel(c.src, c.dst) > 0.0) model.apply(document::cmd::setBusSend(c.src, app::edit::findTrack(p, c.dst)->id, 0.0));
        return;
    }
    if ((c.corner || c.dst == 0) && s->kind == project::TrackKind::Bus) {              // the output marker (or the master column)
        if (outputAllowed(c.src, c.dst)) {
            const std::string target = c.dst == 0 ? "master" : app::edit::findTrack(p, c.dst)->id;
            model.apply(document::cmd::setTrack(c.src, "output", target));
        }
        return;
    }
    if (c.dst == 0 || !sendAllowed(c.src, c.dst)) return;
    drag_ = c;
    dragStart_ = sendLevel(c.src, c.dst);
    dragY_ = e.position.y;
    model.beginGesture("send");
}

void RoutingView::mouseDrag(const juce::MouseEvent& e) {
    if (!drag_.valid) return;
    if (!dragged_ && std::abs(e.position.y - dragY_) < 3.0f) return;
    dragged_ = true;
    const double v = std::clamp(std::round((dragStart_ + double(dragY_ - e.position.y) / 80.0) * 100.0) / 100.0, 0.0, 1.0);
    if (const auto* d = app::edit::findTrack(model.project(), drag_.dst)) model.apply(document::cmd::setBusSend(drag_.src, d->id, v));
}

void RoutingView::mouseUp(const juce::MouseEvent&) {
    if (!drag_.valid) return;
    if (!dragged_) {                                                                   // a click: on or off
        if (const auto* d = app::edit::findTrack(model.project(), drag_.dst)) model.apply(document::cmd::setBusSend(drag_.src, d->id, dragStart_ > 0.0 ? 0.0 : 0.6));
    }
    model.endGesture();
    drag_ = {};
}

void RoutingView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {       // scrolls; it never edits a send
    scrollX_ -= int(w.deltaX * 600.0f);
    scrollY_ -= int(w.deltaY * 600.0f);
    clampScroll();
    repaint();
}

void RoutingView::tick() {
    const auto m = model.meters();
    const auto& p = model.project();
    bool changed = false;
    for (size_t i = 0; i < p.tracks.size() && i < size_t(engine::kMaxMeterTracks); ++i) {
        const float pk = m.trackPeak[i];
        auto& old = peak_[p.tracks[i].uid];
        if (std::abs(pk - old) > 0.01f) { old = pk; changed = true; }
    }
    if (changed) repaint();
}

void RoutingView::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    const auto& p = model.project();
    g.setFont(uiFont(10.5f));
    g.setColour(col::dim);
    g.drawText("FROM  (down)     TO  (across)", juce::Rectangle<int>(10, 8, kLabelW - 14, 16), juce::Justification::centredLeft);
    g.setColour(col::faint);
    g.setFont(uiFont(9.5f));
    g.drawText("dial = send level", juce::Rectangle<int>(10, 28, kLabelW - 14, 12), juce::Justification::centredLeft);
    g.drawText("diamond = main output", juce::Rectangle<int>(10, 41, kLabelW - 14, 12), juce::Justification::centredLeft);
    // column heads
    for (size_t c = 0; c < cols_.size(); ++c) {
        const auto* t = cols_[c] ? app::edit::findTrack(p, cols_[c]) : nullptr;
        const juce::Rectangle<int> head(colX(c), 6, kCellW, kHeadH - 10);
        fillRounded(g, head.toFloat().reduced(1, 0), cols_[c] ? col::panel2 : col::accent.withAlpha(0.25f), 4.0f);
        g.setColour(cols_[c] ? col::meterMid : col::accent);
        g.fillRoundedRectangle(head.toFloat().reduced(1, 0).removeFromTop(4.0f), 2.0f);
        g.setColour(col::text);
        g.setFont(uiFont(11.5f, true));
        g.drawText(t ? juce::String(t->name) : juce::String("Master"), head.reduced(3, 6), juce::Justification::centred, true);
        if (!hover_.valid || hover_.dst != cols_[c]) continue;
        g.setColour(col::accent.withAlpha(0.15f));
        g.fillRect(juce::Rectangle<int>(colX(c), kHeadH, kCellW, rowY(rows_.size() - 1) + kCellH - kHeadH));
    }
    // rows (they scroll under the column heads and above the foot)
    g.saveState();
    g.reduceClipRegion(0, kHeadH, getWidth(), std::max(0, getHeight() - kHeadH - kFootH));
    for (size_t r = 0; r < rows_.size(); ++r) {
        const auto* s = app::edit::findTrack(p, rows_[r]);
        if (!s) continue;
        const int y = rowY(r);
        const bool bus = s->kind == project::TrackKind::Bus;
        size_t pi = 0;
        for (size_t i = 0; i < p.tracks.size(); ++i) if (p.tracks[i].uid == s->uid) pi = i;
        const auto colr = bus ? col::meterMid : trackColour(pi);
        fillRounded(g, juce::Rectangle<float>(6.0f, float(y) + 1, kLabelW - 12.0f, kCellH - 2.0f), hover_.valid && hover_.src == s->uid ? col::raised : col::panel2, 4.0f);
        g.setColour(colr);
        g.fillRoundedRectangle(6.0f, float(y) + 1, 4.0f, kCellH - 2.0f, 2.0f);
        g.setColour(col::text);
        g.setFont(uiFont(12.0f, true));
        g.drawText(juce::String(s->name), juce::Rectangle<int>(16, y, kLabelW - 26, kCellH), juce::Justification::centredLeft, true);
        const float pk = peak_.count(s->uid) ? peak_.at(s->uid) : 0.0f;
        for (size_t c = 0; c < cols_.size(); ++c) {
            const auto cell = juce::Rectangle<int>(colX(c), y, kCellW, kCellH).reduced(1, 1).toFloat();
            const project::Uid dst = cols_[c];
            const bool allowSend = sendAllowed(s->uid, dst), allowOut = outputAllowed(s->uid, dst);
            const bool diag = s->uid == dst, master = dst == 0;
            const double lv = sendLevel(s->uid, dst);
            const bool out = isOutput(s->uid, dst);
            const bool hot = hover_.valid && hover_.src == s->uid && hover_.dst == dst;
            fillRounded(g, cell, hot ? col::raised : col::panel, 3.0f);
            if (diag || (!allowSend && !allowOut && !master)) {                         // a loop (or itself): hatched, dead
                g.setColour(col::faint.withAlpha(0.35f));
                for (float x = cell.getX() - cell.getHeight(); x < cell.getRight(); x += 7.0f) g.drawLine(x, cell.getBottom(), x + cell.getHeight(), cell.getY(), 1.0f);
                continue;
            }
            if (lv > 0.0) {                                                              // the dial, and a glow for the signal passing through
                const auto d = juce::Rectangle<float>(22.0f, 22.0f).withCentre({cell.getCentreX() - 6.0f, cell.getCentreY()});
                g.setColour(col::black.withAlpha(0.6f));
                g.fillEllipse(d);
                juce::Path pie;
                pie.addPieSegment(d.reduced(2.0f), -juce::MathConstants<float>::pi * 0.5f, -juce::MathConstants<float>::pi * 0.5f + float(lv) * juce::MathConstants<float>::twoPi, 0.55f);
                g.setColour(colr.withAlpha(0.95f));
                g.fillPath(pie);
                const float glow = std::clamp(pk * float(lv) * 3.0f, 0.0f, 1.0f);
                if (glow > 0.02f) { g.setColour(colr.withAlpha(0.35f * glow)); g.fillRoundedRectangle(cell, 3.0f); }
                g.setColour(col::text);
                g.setFont(monoFont(9.5f));
                g.drawText(juce::String(lv, 2), juce::Rectangle<float>(cell.getCentreX() + 6.0f, cell.getY(), cell.getWidth() / 2 - 8.0f, cell.getHeight()).toNearestInt(), juce::Justification::centredLeft);
            } else if (allowSend && hot) {
                g.setColour(col::dim.withAlpha(0.7f));
                g.setFont(uiFont(10.0f));
                g.drawText("click", cell.toNearestInt(), juce::Justification::centred);
            }
            if (allowOut || out) {                                                       // the output marker
                const auto dm = cornerRect(s->uid, dst).toFloat().reduced(2.0f);
                juce::Path diamond;
                diamond.addQuadrilateral(dm.getCentreX(), dm.getY(), dm.getRight(), dm.getCentreY(), dm.getCentreX(), dm.getBottom(), dm.getX(), dm.getCentreY());
                if (out) {
                    g.setColour(master ? col::accent : colr);
                    g.fillPath(diamond);
                    if (master) { const float glow = std::clamp(pk * 2.0f, 0.0f, 1.0f); g.setColour(col::accent.withAlpha(0.3f * glow)); g.fillRoundedRectangle(cell, 3.0f); }
                } else { g.setColour(col::faint); g.strokePath(diamond, juce::PathStrokeType(1.0f)); }
            }
        }
    }
    g.restoreState();
    // the foot: what is under the pointer
    g.setColour(col::panel);
    g.fillRect(0, getHeight() - kFootH, getWidth(), kFootH);
    g.setColour(col::text.withAlpha(0.85f));
    g.setFont(uiFont(11.5f));
    g.drawText(hoverText_.isEmpty() ? juce::String("Point at a cell. Down the side: where a sound comes from. Across the top: where it can go.") : hoverText_,
               juce::Rectangle<int>(10, getHeight() - kFootH, getWidth() - 20, kFootH), juce::Justification::centredLeft, true);
}

}  // namespace ddaw::ui
