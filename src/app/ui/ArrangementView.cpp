#include "app/ui/ArrangementView.h"

#include "app/ui/Dialogs.h"
#include "project/ProjectJson.h"

namespace ddaw::ui {

void ArrangementView::clampScroll() {
    const int contentH = kRulerH + int(model.project().tracks.size()) * kLaneH;
    scrollY_ = juce::jlimit(0, std::max(0, contentH - getHeight()), scrollY_);
    scale_.originTick = std::max(0.0, scale_.originTick);
}

juce::Rectangle<int> ArrangementView::clipRect(const std::string& key) const {
    const auto& p = model.project();
    auto it = p.arr.find(key);
    if (it == p.arr.end()) return {};
    int lane = -1;
    for (size_t i = 0; i < p.tracks.size(); ++i) if (p.tracks[i].id == it->second.trackId) lane = int(i);
    if (lane < 0) return {};
    const int x0 = kHeaderW + int(std::round(scale_.toX(it->second.start))), x1 = kHeaderW + int(std::round(scale_.toX(it->second.start + it->second.clip.len)));
    return {x0, laneTop(lane) + 4, std::max(3, x1 - x0), kLaneH - 8};
}

ArrangementView::Hit ArrangementView::clipAt(juce::Point<int> pt) const {
    Hit h;
    if (pt.x < kHeaderW || pt.y < kRulerH) return h;
    h.lane = laneOf(pt.y);
    for (auto it = model.project().arr.rbegin(); it != model.project().arr.rend(); ++it) {  // later keys draw on top
        const auto r = clipRect(it->first);
        if (r.contains(pt)) {
            h.key = it->first;
            h.part = pt.x >= r.getRight() - 7 ? Hit::RightEdge : Hit::Body;
            return h;
        }
    }
    return h;
}

const std::vector<float>& ArrangementView::peaksFor(const std::string& id) {
    auto it = peaks_.find(id);
    if (it != peaks_.end()) return it->second;
    std::vector<float> bins(512, 0.0f);
    if (auto buf = model.sampleBank()->get(id)) {
        const size_t n = buf->frames();
        for (size_t b = 0; b < bins.size() && n; ++b) {
            const size_t a = b * n / bins.size(), z = std::max(a + 1, (b + 1) * n / bins.size());
            float m = 0;
            for (size_t i = a; i < z && i < n; i += std::max<size_t>(1, (z - a) / 32)) m = std::max(m, std::abs(buf->l[i]));
            bins[b] = m;
        }
    }
    return peaks_.emplace(id, std::move(bins)).first->second;
}

void ArrangementView::drawClip(juce::Graphics& g, const std::string&, const project::ArrClip& ac, juce::Rectangle<int> r, juce::Colour c, bool selected, bool ghost) {
    auto rf = r.toFloat();
    const float alpha = ghost ? 0.55f : 1.0f;
    fillRounded(g, rf, c.withMultipliedBrightness(0.5f).withAlpha(alpha), 3.0f);
    auto head = rf.removeFromTop(14.0f);
    fillRounded(g, head, c.withAlpha(alpha * 0.9f), 3.0f);
    g.setColour(col::black.withAlpha(0.85f));
    g.setFont(uiFont(10.5f, true));
    const auto& clip = ac.clip;
    g.drawText(clip.audio ? juce::String(clip.audio->sampleName.empty() ? clip.audio->sampleId : clip.audio->sampleName) : juce::String(int(clip.notes.size())) + " notes",
               head.reduced(4, 0).toNearestInt(), juce::Justification::centredLeft, true);
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(r);
    auto body = rf.reduced(0, 2);
    if (clip.audio) {
        const auto& pk = peaksFor(clip.audio->sampleId);
        const auto buf = model.sampleBank()->get(clip.audio->sampleId);
        if (buf && buf->duration() > 0) {
            const double bpm = model.project().meta.bpm;
            g.setColour(c.brighter(0.3f).withAlpha(alpha));
            const double secPerPx = 60.0 / bpm / scale_.pxPerBeat;
            for (float x = 0; x < body.getWidth(); x += 2.0f) {
                double t = double(x) * secPerPx;
                if (clip.audio->loop != 0.0) t = std::fmod(t, buf->duration());
                if (t >= buf->duration()) break;
                const float a = pk[std::min(pk.size() - 1, size_t(t / buf->duration() * double(pk.size())))];
                const float h = std::max(1.0f, a * body.getHeight() * 0.5f);
                g.fillRect(body.getX() + x, body.getCentreY() - h, 1.6f, h * 2);
            }
        }
    } else if (!clip.notes.empty()) {
        int lo = 127, hi = 0;
        for (auto& n : clip.notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
        const double range = std::max(hi - lo, 7) + 1.0;
        g.setColour(col::black.withAlpha(0.5f * alpha));
        for (auto& n : clip.notes) {
            const float x = body.getX() + float(scale_.toX(n.startTicks) - scale_.toX(0));
            const float w = std::max(1.5f, float(n.durTicks / app::TimeScale::kTicksPerBeat * scale_.pxPerBeat));
            const float y = body.getBottom() - float((n.pitch - lo + 1) / range) * body.getHeight();
            g.fillRect(x, y, w, std::max(1.5f, body.getHeight() / float(range)));
        }
    }
    if (selected) {
        g.setColour(col::text);
        g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 3.0f, 1.5f);
    }
}

void ArrangementView::paint(juce::Graphics& g) {
    const auto& p = model.project();
    g.fillAll(col::bg);
    const int nT = int(p.tracks.size());
    const auto& sel = model.selection();
    const int gridW = getWidth() - kHeaderW;

    // ---- lanes and grid ----
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(kHeaderW, kRulerH, gridW, getHeight() - kRulerH);
        for (int t = 0; t < nT; ++t) {
            const int y = laneTop(t);
            g.setColour(t % 2 ? col::bg : col::bg.brighter(0.02f));
            g.fillRect(kHeaderW, y, gridW, kLaneH);
            g.setColour(col::line);
            g.drawHorizontalLine(y + kLaneH - 1, float(kHeaderW), float(getWidth()));
        }
        // vertical grid: beats, bars
        const double beatPx = scale_.pxPerBeat;
        const double firstBeat = std::floor(scale_.originTick / 96.0);
        const int per = p.meta.tsTop > 0 ? p.meta.tsTop : 4;
        for (double b = firstBeat; scale_.toX(b * 96.0) < gridW; b += 1.0) {
            const bool bar = int(b) % per == 0;
            if (!bar && beatPx < 14) continue;
            if (bar && beatPx * per < 16 && int(b / per) % 4 != 0) continue;
            const float x = float(kHeaderW + scale_.toX(b * 96.0));
            g.setColour(bar ? col::gridBar : col::grid);
            g.fillRect(x, float(kRulerH), 1.0f, float(getHeight() - kRulerH));
        }
        // clips
        for (const auto& [key, ac] : p.arr) {
            int lane = -1;
            for (int i = 0; i < nT; ++i) if (p.tracks[size_t(i)].id == ac.trackId) lane = i;
            if (lane < 0) continue;
            const bool ghosted = drag_ != Drag::None && dragMoved_ && key == dragKey_;
            auto r = clipRect(key);
            if (ghosted) {
                g.setColour(col::faint.withAlpha(0.4f));
                g.drawRoundedRectangle(r.toFloat(), 3.0f, 1.0f);
                continue;
            }
            drawClip(g, key, ac, r, trackColour(size_t(lane)), sel.clip.arrKey == key, false);
        }
        if (drag_ != Drag::None && dragMoved_ && p.arr.count(dragKey_)) {
            auto ac = p.arr.at(dragKey_);
            ac.start = previewStart_;
            ac.clip.len = previewLen_;
            const int lane = previewLane_ >= 0 ? previewLane_ : 0;
            const int x0 = kHeaderW + int(std::round(scale_.toX(ac.start))), x1 = kHeaderW + int(std::round(scale_.toX(ac.start + ac.clip.len)));
            drawClip(g, dragKey_, ac, {x0, laneTop(lane) + 4, std::max(3, x1 - x0), kLaneH - 8}, trackColour(size_t(lane)), true, true);
        }
        if (dropPos_.x >= 0) {
            g.setColour(col::accent);
            g.drawVerticalLine(dropPos_.x, float(kRulerH), float(getHeight()));
        }
        // the take being recorded: a red region growing from where capture started to the playhead
        if (model.recording().recording()) {
            const auto info = model.engine().captureInfo();
            const auto m = model.meters();
            if (info.active) {
                const double a = std::max(0.0, info.startTick), b = std::max(a, m.playheadTicks);
                for (int t = 0; t < nT; ++t)
                    if (model.recording().armed(p.tracks[size_t(t)].uid)) {
                        const float x0 = float(kHeaderW + scale_.toX(a)), x1 = float(kHeaderW + scale_.toX(b));
                        fillRounded(g, juce::Rectangle<float>(x0, float(laneTop(t) + 4), std::max(2.0f, x1 - x0), float(kLaneH - 8)), col::rec.withAlpha(0.35f), 3.0f);
                    }
            }
        }
        // cursor and playhead
        const float cx = float(kHeaderW + scale_.toX(model.cursorTicks()));
        g.setColour(col::dim.withAlpha(0.7f));
        g.fillRect(cx, float(kRulerH), 1.0f, float(getHeight() - kRulerH));
        const auto m = model.meters();
        if (m.playing && model.arrangementMode()) {
            const float px = float(kHeaderW + scale_.toX(m.playheadTicks));
            g.setColour(col::accent);
            g.fillRect(px, float(kRulerH), 1.5f, float(getHeight() - kRulerH));
        }
    }

    // ---- ruler ----
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(kHeaderW, 0, gridW, kRulerH);
        g.setColour(col::panel);
        g.fillRect(kHeaderW, 0, gridW, kRulerH);
        const auto& meta = p.meta;
        if (meta.loopEnd > meta.loopStart) {
            const float x0 = float(kHeaderW + scale_.toX(meta.loopStart)), x1 = float(kHeaderW + scale_.toX(meta.loopEnd));
            g.setColour((meta.loopOn ? col::queued : col::faint).withAlpha(0.35f));
            g.fillRect(x0, float(kRulerH - 10), x1 - x0, 10.0f);
        }
        if (drag_ == Drag::Loop) {
            const double a = std::min(loopA_, previewStart_), b = std::max(loopA_, previewStart_);
            g.setColour(col::queued.withAlpha(0.6f));
            g.fillRect(float(kHeaderW + scale_.toX(a)), float(kRulerH - 10), float(scale_.toX(b) - scale_.toX(a)), 10.0f);
        }
        const int per = p.meta.tsTop > 0 ? p.meta.tsTop : 4;
        g.setFont(monoFont(10.5f));
        for (double b = std::floor(scale_.originTick / 96.0); scale_.toX(b * 96.0) < gridW; b += 1.0) {
            const bool bar = int(b) % per == 0;
            const float x = float(kHeaderW + scale_.toX(b * 96.0));
            if (bar) {
                if (scale_.pxPerBeat * per < 30 && int(b / per) % 4 != 0) continue;
                g.setColour(col::line);
                g.fillRect(x, 4.0f, 1.0f, float(kRulerH - 4));
                g.setColour(col::dim);
                g.drawText(juce::String(int(b) / per + 1), int(x) + 4, 2, 40, 14, juce::Justification::centredLeft);
            } else if (scale_.pxPerBeat >= 14) {
                g.setColour(col::grid);
                g.fillRect(x, float(kRulerH - 8), 1.0f, 8.0f);
            }
        }
        const float cx = float(kHeaderW + scale_.toX(model.cursorTicks()));
        juce::Path tri;
        tri.addTriangle(cx - 5, 12, cx + 5, 12, cx, 22);
        g.setColour(col::dim);
        g.fillPath(tri);
    }
    g.setColour(col::line);
    g.drawHorizontalLine(kRulerH - 1, 0, float(getWidth()));

    // ---- headers ----
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(0, kRulerH, kHeaderW, getHeight() - kRulerH);
        g.setColour(col::panel);
        g.fillRect(0, kRulerH, kHeaderW, getHeight());
        for (int t = 0; t < nT; ++t) {
            const auto& tr = p.tracks[size_t(t)];
            const int y = laneTop(t);
            auto r = juce::Rectangle<float>(3.0f, float(y) + 2, kHeaderW - 6.0f, kLaneH - 4.0f);
            fillRounded(g, r, sel.track == tr.uid ? col::raised : col::panel2, 3.0f);
            g.setColour(trackColour(size_t(t)));
            g.fillRoundedRectangle(r.withWidth(4.0f), 2.0f);
            g.setColour(col::text);
            g.setFont(uiFont(12.5f, true));
            g.drawText(juce::String(tr.name), r.withTrimmedLeft(12).removeFromTop(24).toNearestInt(), juce::Justification::centredLeft, true);
            auto btn = r.withTrimmedLeft(12).removeFromBottom(26).reduced(0, 3);
            auto mute = btn.removeFromLeft(26.0f); btn.removeFromLeft(4);
            auto solo = btn.removeFromLeft(26.0f);
            fillRounded(g, mute, tr.mute ? col::queued : col::raised, 3.0f);
            fillRounded(g, solo, tr.solo ? col::accent : col::raised, 3.0f);
            g.setFont(uiFont(11.0f, true));
            g.setColour(tr.mute ? col::black : col::dim);
            g.drawText("M", mute.toNearestInt(), juce::Justification::centred);
            g.setColour(tr.solo ? col::black : col::dim);
            g.drawText("S", solo.toNearestInt(), juce::Justification::centred);
            if (tr.kind != project::TrackKind::Bus) {
                const bool on = model.recording().armed(tr.uid);
                auto arm = juce::Rectangle<float>(solo.getRight() + 4.0f, solo.getY(), 26.0f, solo.getHeight());
                fillRounded(g, arm, on ? col::rec : col::raised, 3.0f);
                g.setColour(on ? col::black : col::rec);
                g.fillEllipse(juce::Rectangle<float>(9, 9).withCentre(arm.getCentre()));
            }
        }
    }
    g.setColour(col::panel);
    g.fillRect(0, 0, kHeaderW, kRulerH);
    g.setColour(col::line);
    g.drawVerticalLine(kHeaderW - 1, 0, float(getHeight()));
    if (nT == 0) {
        g.setColour(col::dim);
        g.setFont(uiFont(14.0f));
        g.drawText("Add tracks in the Session view", getLocalBounds().withTrimmedTop(kRulerH), juce::Justification::centred);
    }
}

void ArrangementView::tick() {
    const auto m = model.meters();
    if (m.playing && (model.arrangementMode() || model.recording().recording())) {
        const double x = scale_.toX(m.playheadTicks);
        const double w = getWidth() - kHeaderW;
        if (x > w - 30 || x < 0) scale_.originTick = std::max(0.0, m.playheadTicks - 96.0);
    }
    if (m.playing != (lastPlayhead_ >= 0) || std::abs(m.playheadTicks - lastPlayhead_) > 0.5 || model.recording().recording()) {
        lastPlayhead_ = m.playing ? m.playheadTicks : -1;
        repaint();
    }
}

void ArrangementView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
        scale_.zoomAt(double(e.x - kHeaderW), std::exp(double(w.deltaY) * 1.5));
    } else {
        scale_.originTick = std::max(0.0, scale_.originTick - double(w.deltaX) * 800.0 / scale_.pxPerBeat * 4.0);
        scrollY_ -= int(w.deltaY * 500.0f);
    }
    clampScroll();
    repaint();
}

void ArrangementView::mouseDown(const juce::MouseEvent& e) {
    if (isShowing()) grabKeyboardFocus();
    const auto& p = model.project();
    dragMoved_ = false;
    drag_ = Drag::None;
    if (e.y < kRulerH && e.x >= kHeaderW) {
        const double t = std::max(0.0, app::edit::snapTicks(tickAt(e.x), e.mods.isShiftDown() ? 0.0 : app::autoGrid(scale_, 10.0)));
        if (e.mods.isShiftDown()) { drag_ = Drag::Loop; loopA_ = t; previewStart_ = t; }
        else { drag_ = Drag::Cursor; model.setCursor(t); if (model.meters().playing && model.arrangementMode()) model.play(true, t); }
        repaint();
        return;
    }
    if (e.x < kHeaderW && e.y >= kRulerH) {
        const int lane = laneOf(e.y);
        if (lane < 0 || lane >= int(p.tracks.size())) return;
        const auto& tr = p.tracks[size_t(lane)];
        const int by = laneTop(lane) + kLaneH - 2 - 26 + 3;
        if (e.y >= by && e.y < by + 20 && e.x >= 15 && e.x < 41) model.apply(document::cmd::setTrack(tr.uid, "mute", !tr.mute));
        else if (e.y >= by && e.y < by + 20 && e.x >= 45 && e.x < 71) model.apply(document::cmd::setTrack(tr.uid, "solo", !tr.solo));
        else if (e.y >= by && e.y < by + 20 && e.x >= 75 && e.x < 101 && tr.kind != project::TrackKind::Bus) model.recording().arm(tr.uid, !model.recording().armed(tr.uid));
        else model.selectTrack(tr.uid);
        return;
    }
    const auto h = clipAt(e.getPosition());
    if (h.part != Hit::None) {
        const auto& ac = p.arr.at(h.key);
        model.selectClip({0, "", h.key});
        if (e.mods.isPopupMenu()) {
            juce::PopupMenu m;
            m.addItem(1, "Delete");
            m.addItem(2, "Duplicate");
            m.showMenuAsync({}, [this, key = h.key](int r) {
                const auto& pr = model.project();
                auto it = pr.arr.find(key);
                if (it == pr.arr.end()) return;
                if (r == 1) model.apply(app::edit::removeArrClip(key));
                else if (r == 2) {
                    const auto* t = app::edit::findTrackById(pr, it->second.trackId);
                    auto c = app::edit::newArrClip(pr, t ? t->uid : 0, it->second.start + it->second.clip.len, it->second.clip.len);
                    c.args["arr"]["clip"] = project::clipToJson(it->second.clip);
                    model.apply(c);
                }
            });
            return;
        }
        drag_ = h.part == Hit::RightEdge ? Drag::Resize : Drag::Move;
        dragKey_ = h.key;
        dragOrigStart_ = previewStart_ = ac.start;
        dragOrigLen_ = previewLen_ = ac.clip.len;
        dragGrabOffset_ = tickAt(e.x) - ac.start;
        previewLane_ = h.lane;
    } else if (e.x >= kHeaderW) {
        const int lane = laneOf(e.y);
        if (lane >= 0 && lane < int(p.tracks.size())) { model.selectTrack(p.tracks[size_t(lane)].uid); model.selectClip({}); }
    }
    repaint();
}

void ArrangementView::mouseDrag(const juce::MouseEvent& e) {
    const auto& p = model.project();
    if (drag_ == Drag::Cursor && e.x >= kHeaderW) {
        model.setCursor(std::max(0.0, app::edit::snapTicks(tickAt(e.x), e.mods.isShiftDown() ? 0.0 : app::autoGrid(scale_, 10.0))));
    } else if (drag_ == Drag::Loop) {
        previewStart_ = std::max(0.0, app::edit::snapTicks(tickAt(e.x), app::autoGrid(scale_, 10.0)));
    } else if (drag_ == Drag::Move || drag_ == Drag::Resize) {
        if (e.getDistanceFromDragStart() < 3 && !dragMoved_) return;
        dragMoved_ = true;
        const double gr = grid(e);
        if (drag_ == Drag::Move) {
            previewStart_ = std::max(0.0, app::edit::snapTicks(tickAt(e.x) - dragGrabOffset_, gr));
            const int lane = juce::jlimit(0, std::max(0, int(p.tracks.size()) - 1), laneOf(e.y));
            previewLane_ = lane;
        } else {
            previewLen_ = std::max(gr > 0 ? gr : 6.0, app::edit::snapTicks(tickAt(e.x) - dragOrigStart_, gr));
        }
        // autoscroll at the edges
        if (e.x > getWidth() - 20) scale_.originTick += 24.0 / scale_.pxPerBeat * 96.0 * 0.5;
        else if (e.x < kHeaderW + 20 && scale_.originTick > 0) scale_.originTick = std::max(0.0, scale_.originTick - 12.0 / scale_.pxPerBeat * 96.0);
    }
    repaint();
}

void ArrangementView::mouseUp(const juce::MouseEvent&) {
    const auto& p = model.project();
    if (drag_ == Drag::Loop) {
        const double a = std::min(loopA_, previewStart_), b = std::max(loopA_, previewStart_);
        if (b - a >= 24.0)
            model.applyGroup("loop region", {document::cmd::setMeta("loopStart", a), document::cmd::setMeta("loopEnd", b), document::cmd::setMeta("loopOn", true)});
    } else if ((drag_ == Drag::Move || drag_ == Drag::Resize) && dragMoved_ && p.arr.count(dragKey_)) {
        try {
            if (drag_ == Drag::Move) {
                std::optional<project::Uid> to;
                if (previewLane_ >= 0 && previewLane_ < int(p.tracks.size())) {
                    const auto& dst = p.tracks[size_t(previewLane_)];
                    const auto& src = p.arr.at(dragKey_);
                    const bool audioClip = src.clip.audio.has_value();
                    if ((dst.kind == project::TrackKind::Audio) == audioClip && dst.kind != project::TrackKind::Bus) to = dst.uid;
                }
                model.apply(app::edit::moveArrClip(p, dragKey_, previewStart_, to));
            } else model.apply(app::edit::resizeArrClip(p, dragKey_, previewLen_));
        } catch (const std::exception&) {}
    }
    drag_ = Drag::None;
    dragMoved_ = false;
    repaint();
}

void ArrangementView::mouseDoubleClick(const juce::MouseEvent& e) {
    const auto& p = model.project();
    if (e.x < kHeaderW || e.y < kRulerH) return;
    const auto h = clipAt(e.getPosition());
    if (h.part != Hit::None) { model.selectClip({0, "", h.key}); return; }
    const int lane = laneOf(e.y);
    if (lane < 0 || lane >= int(p.tracks.size())) return;
    const auto& tr = p.tracks[size_t(lane)];
    if (tr.kind == project::TrackKind::Audio || tr.kind == project::TrackKind::Bus) return;
    const double start = std::max(0.0, app::edit::snapFloor(tickAt(e.x), 384.0));
    if (model.apply(app::edit::newArrClip(p, tr.uid, start))) {
        // newArrClip made the key from the pre-edit project; find the one we just added
        for (auto& [k, ac] : model.project().arr) if (ac.trackId == tr.id && ac.start == start) model.selectClip({0, "", k});
    }
}

bool ArrangementView::keyPressed(const juce::KeyPress& k) {
    const auto& sel = model.selection();
    if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && !sel.clip.arrKey.empty()) {
        model.apply(app::edit::removeArrClip(sel.clip.arrKey));
        return true;
    }
    if (k == juce::KeyPress::homeKey) { model.setCursor(0); scale_.originTick = 0; repaint(); return true; }
    return false;
}

void ArrangementView::itemDropped(const SourceDetails& d) {
    dropPos_ = {-1, -1};
    const auto& p = model.project();
    const int lane = laneOf(d.localPosition.y);
    repaint();
    if (lane < 0 || lane >= int(p.tracks.size()) || d.localPosition.x < kHeaderW) return;
    const auto& tr = p.tracks[size_t(lane)];
    if (tr.kind != project::TrackKind::Audio) return;
    const std::string id = d.description.toString().fromFirstOccurrenceOf("sample:", false, false).toStdString();
    const auto buf = model.sampleBank()->get(id);
    if (!buf) return;
    project::Clip c;
    c.len = std::max(1.0, std::ceil(buf->duration() * p.meta.bpm / 60.0)) * 96.0;
    c.audio = project::AudioClipData{};
    c.audio->sampleId = id;
    c.audio->sampleName = model.sampleName(id);
    const double start = std::max(0.0, app::edit::snapTicks(tickAt(d.localPosition.x), app::autoGrid(scale_, 10.0)));
    model.apply({"arrclip.set", {{"key", app::edit::uniqueArrKey(p)}, {"arr", {{"trackId", tr.id}, {"start", start}, {"clip", project::clipToJson(c)}}}}});
}

}  // namespace ddaw::ui
