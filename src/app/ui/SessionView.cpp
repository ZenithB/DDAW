#include "app/ui/SessionView.h"

#include "app/ui/Dialogs.h"
#include "project/ProjectJson.h"

namespace ddaw::ui {

using Kind = SessionView::Hit::Kind;

juce::Rectangle<int> SessionView::headerRect(int t) const { return {kSceneW + t * kTrackW - scrollX_, 0, kTrackW, kHeaderH}; }
juce::Rectangle<int> SessionView::slotRect(int t, int s) const { return {kSceneW + t * kTrackW - scrollX_, kHeaderH + s * kRowH - scrollY_, kTrackW, kRowH}; }

void SessionView::clampScroll() {
    const int contentW = kSceneW + trackCount() * kTrackW + kAddW, contentH = kHeaderH + (sceneCount() + 1) * kRowH;
    scrollX_ = juce::jlimit(0, std::max(0, contentW - getWidth()), scrollX_);
    scrollY_ = juce::jlimit(0, std::max(0, contentH - getHeight()), scrollY_);
}

void SessionView::refresh(app::ModelEvent) { clampScroll(); repaint(); }

void SessionView::tick() {
    const auto m = model.meters();
    const int n = trackCount();
    std::vector<int> now(size_t(n), -1);
    std::vector<bool> q(size_t(n), false);
    for (int i = 0; i < n; ++i) {
        now[size_t(i)] = m.trackScene[i];
        q[size_t(i)] = m.trackScene[i] >= 0 && m.trackAnchor[i] > m.playheadTicks + 1.0 && m.playing;
    }
    if (now != playing_ || q != queued_ || (std::any_of(q.begin(), q.end(), [](bool b) { return b; }))) {
        playing_ = std::move(now);
        queued_ = std::move(q);
        repaint();
    }
}

void SessionView::paint(juce::Graphics& g) {
    const auto& p = model.project();
    g.fillAll(col::bg);
    const int nT = trackCount(), nS = sceneCount();
    const auto& sel = model.selection();
    const bool flash = (juce::Time::getMillisecondCounter() / 250) % 2 == 0;

    // ---- grid ----
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(kSceneW, kHeaderH, getWidth() - kSceneW, getHeight() - kHeaderH);
        for (int s = 0; s < nS; ++s)
            for (int t = 0; t < nT; ++t) {
                const auto& tr = p.tracks[size_t(t)];
                auto r = slotRect(t, s);
                if (r.getRight() < kSceneW || r.getX() > getWidth() || r.getBottom() < kHeaderH || r.getY() > getHeight()) continue;
                auto inner = r.reduced(2, 2).toFloat();
                const auto it = p.clips.find(tr.id + "|" + p.scenes[size_t(s)]);
                const bool hasClip = it != p.clips.end();
                const bool isPlaying = size_t(t) < playing_.size() && playing_[size_t(t)] == s;
                const bool isQueued = isPlaying && size_t(t) < queued_.size() && queued_[size_t(t)];
                const auto colr = trackColour(size_t(t));
                if (hasClip) {
                    juce::Colour fill = colr.withMultipliedBrightness(0.55f).withMultipliedSaturation(0.8f);
                    if (isPlaying) fill = isQueued ? col::queued.withMultipliedBrightness(0.7f) : colr.withMultipliedBrightness(0.95f);
                    fillRounded(g, inner, fill, 3.0f);
                    // content preview
                    const auto& clip = it->second;
                    auto body = inner.withTrimmedLeft(22).reduced(3, 3);
                    if (clip.audio) {
                        g.setColour(col::black.withAlpha(0.55f));
                        g.setFont(uiFont(11.0f));
                        g.drawText(clip.audio->sampleName.empty() ? juce::String(clip.audio->sampleId) : juce::String(clip.audio->sampleName), body.toNearestInt(), juce::Justification::centredLeft, true);
                    } else if (!clip.notes.empty()) {
                        int lo = 127, hi = 0;
                        for (auto& n : clip.notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
                        const double range = std::max(hi - lo, 7) + 1.0;
                        g.setColour(col::black.withAlpha(0.45f));
                        for (auto& n : clip.notes) {
                            const float x = body.getX() + float(n.startTicks / clip.len) * body.getWidth();
                            const float w = std::max(1.5f, float(n.durTicks / clip.len) * body.getWidth());
                            const float y = body.getBottom() - float((n.pitch - lo + 1) / range) * body.getHeight();
                            g.fillRect(x, y, std::min(w, body.getRight() - x), std::max(1.5f, body.getHeight() / float(range)));
                        }
                    }
                    // launch triangle
                    juce::Path tri;
                    const float cx = inner.getX() + 11, cy = inner.getCentreY();
                    tri.addTriangle(cx - 4, cy - 6, cx - 4, cy + 6, cx + 6, cy);
                    g.setColour(isPlaying && !isQueued ? col::black : (isQueued && flash ? col::black : col::black.withAlpha(0.7f)));
                    if (isPlaying && !isQueued) { g.setColour(col::black); }
                    g.fillPath(tri);
                } else {
                    fillRounded(g, inner, col::panel, 3.0f);
                    if (size_t(t) < playing_.size() && playing_[size_t(t)] >= 0) {  // stop square on empty slots of a playing track
                        g.setColour(col::faint);
                        g.fillRect(juce::Rectangle<float>(8, 8).withCentre({inner.getX() + 11, inner.getCentreY()}));
                    }
                }
                if (sel.track == tr.uid && sel.scene == p.scenes[size_t(s)]) {
                    g.setColour(col::text.withAlpha(0.9f));
                    g.drawRoundedRectangle(inner.expanded(0.5f), 3.0f, 1.5f);
                }
                if (dropSlot_.x == t && dropSlot_.y == s) {
                    g.setColour(col::accent);
                    g.drawRoundedRectangle(inner.expanded(0.5f), 3.0f, 2.0f);
                }
            }
    }

    // ---- scene column ----
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(0, kHeaderH, kSceneW, getHeight() - kHeaderH);
        g.setColour(col::panel);
        g.fillRect(0, kHeaderH, kSceneW, getHeight());
        for (int s = 0; s < nS; ++s) {
            const int y = kHeaderH + s * kRowH - scrollY_;
            auto r = juce::Rectangle<float>(4.0f, float(y) + 2, kSceneW - 8.0f, kRowH - 4.0f);
            fillRounded(g, r, col::panel2, 3.0f);
            juce::Path tri;
            tri.addTriangle(r.getX() + 8, r.getCentreY() - 6, r.getX() + 8, r.getCentreY() + 6, r.getX() + 18, r.getCentreY());
            g.setColour(col::play);
            g.fillPath(tri);
            g.setColour(col::text);
            g.setFont(uiFont(12.0f));
            g.drawText(juce::String(p.scenes[size_t(s)]), r.withTrimmedLeft(26).toNearestInt(), juce::Justification::centredLeft, true);
        }
        const int y = kHeaderH + nS * kRowH - scrollY_;
        auto r = juce::Rectangle<float>(4.0f, float(y) + 2, kSceneW - 8.0f, kRowH - 4.0f);
        g.setColour(col::faint);
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
        g.setFont(uiFont(12.0f));
        g.drawText("+ Scene", r.toNearestInt(), juce::Justification::centred);
    }

    // ---- track headers ----
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(kSceneW, 0, getWidth() - kSceneW, kHeaderH);
        g.setColour(col::panel);
        g.fillRect(0, 0, getWidth(), kHeaderH);
        for (int t = 0; t < nT; ++t) {
            const auto& tr = p.tracks[size_t(t)];
            auto r = headerRect(t).reduced(2, 4).toFloat();
            const auto c = trackColour(size_t(t));
            fillRounded(g, r, sel.track == tr.uid ? col::raised.brighter(0.1f) : col::panel2, 3.0f);
            g.setColour(c);
            g.fillRoundedRectangle(r.removeFromTop(4.0f), 2.0f);
            g.setColour(col::text);
            g.setFont(uiFont(12.5f, true));
            g.drawText(juce::String(tr.name), r.removeFromTop(20.0f).reduced(6, 0).toNearestInt(), juce::Justification::centredLeft, true);
            g.setColour(col::dim);
            g.setFont(uiFont(10.5f));
            const char* kind = tr.kind == project::TrackKind::Drum ? "drum" : tr.kind == project::TrackKind::Audio ? "audio" : tr.kind == project::TrackKind::Bus ? "bus" : tr.inst.type.c_str();
            g.drawText(kind, r.removeFromTop(14.0f).reduced(6, 0).toNearestInt(), juce::Justification::centredLeft, true);
            auto btn = r.reduced(4, 2);
            auto button = [&](int i) { return juce::Rectangle<float>(btn.getX() + i * 28.0f, btn.getY(), 24.0f, btn.getHeight()); };
            const auto stop = button(0), mute = button(1), solo = button(2), arm = button(3);
            fillRounded(g, stop, col::raised, 3.0f);
            g.setColour(col::dim);
            g.fillRect(juce::Rectangle<float>(8, 8).withCentre(stop.getCentre()));
            fillRounded(g, mute, tr.mute ? col::queued : col::raised, 3.0f);
            fillRounded(g, solo, tr.solo ? col::accent : col::raised, 3.0f);
            g.setFont(uiFont(11.0f, true));
            g.setColour(tr.mute ? col::black : col::dim);
            g.drawText("M", mute.toNearestInt(), juce::Justification::centred);
            g.setColour(tr.solo ? col::black : col::dim);
            g.drawText("S", solo.toNearestInt(), juce::Justification::centred);
            if (tr.kind != project::TrackKind::Bus) {
                const bool on = model.recording().armed(tr.uid);
                fillRounded(g, arm, on ? col::rec : col::raised, 3.0f);
                g.setColour(on ? col::black : col::rec);
                g.fillEllipse(juce::Rectangle<float>(9, 9).withCentre(arm.getCentre()));
            }
        }
        auto add = juce::Rectangle<float>(float(kSceneW + nT * kTrackW - scrollX_ + 4), 8.0f, kAddW - 8.0f, kHeaderH - 16.0f);
        g.setColour(col::faint);
        g.drawRoundedRectangle(add, 3.0f, 1.0f);
        g.setFont(uiFont(16.0f));
        g.drawText("+", add.toNearestInt(), juce::Justification::centred);
    }
    g.setColour(col::panel);
    g.fillRect(0, 0, kSceneW, kHeaderH);
    g.setColour(col::line);
    g.drawVerticalLine(kSceneW - 1, 0, float(getHeight()));
    g.drawHorizontalLine(kHeaderH - 1, 0, float(getWidth()));
    if (nT == 0) {
        g.setColour(col::dim);
        g.setFont(uiFont(14.0f));
        g.drawText("Add a track to begin  (+)", getLocalBounds().withTrimmedTop(kHeaderH), juce::Justification::centred);
    }
}

SessionView::Hit SessionView::hitAt(juce::Point<int> pt) const {
    const int nT = trackCount(), nS = sceneCount();
    const auto& p = model.project();
    Hit h;
    if (pt.y < kHeaderH) {
        if (pt.x < kSceneW) return h;
        for (int t = 0; t < nT; ++t) {
            auto r = headerRect(t).reduced(2, 4);
            if (!r.contains(pt)) continue;
            h.track = t;
            h.kind = Kind::TrackHeader;
            const int by = r.getBottom() - 24, bx = r.getX() + 4;
            if (pt.y >= by) {
                const int i = (pt.x - bx) / 28;
                if (pt.x >= bx && (pt.x - bx) % 28 < 24) {
                    if (i == 0) h.kind = Kind::TrackStop;
                    else if (i == 1) h.kind = Kind::Mute;
                    else if (i == 2) h.kind = Kind::Solo;
                    else if (i == 3 && p.tracks[size_t(t)].kind != project::TrackKind::Bus) h.kind = Kind::Arm;
                }
            }
            return h;
        }
        if (juce::Rectangle<int>(kSceneW + nT * kTrackW - scrollX_, 0, kAddW, kHeaderH).contains(pt)) h.kind = Kind::AddTrack;
        return h;
    }
    if (pt.x < kSceneW) {
        const int s = (pt.y - kHeaderH + scrollY_) / kRowH;
        if (pt.y - kHeaderH + scrollY_ < 0) return h;
        if (s < nS) { h.scene = s; h.kind = pt.x < 28 ? Kind::SceneLaunch : Kind::SceneHeader; }
        else if (s == nS) h.kind = Kind::AddScene;
        return h;
    }
    const int t = (pt.x - kSceneW + scrollX_) / kTrackW, s = (pt.y - kHeaderH + scrollY_) / kRowH;
    if (pt.x - kSceneW + scrollX_ >= 0 && t < nT && s < nS && pt.y - kHeaderH + scrollY_ >= 0) {
        h.track = t; h.scene = s;
        h.kind = pt.x - slotRect(t, s).getX() < 24 ? Kind::SlotLaunch : Kind::Slot;
    }
    return h;
}

void SessionView::mouseDown(const juce::MouseEvent& e) {
    if (isShowing()) grabKeyboardFocus();
    const auto h = hitAt(e.getPosition());
    const auto& p = model.project();
    if (e.mods.isPopupMenu()) { contextMenu(h); return; }
    switch (h.kind) {
        case Kind::AddTrack: showAddTrackMenu(); break;
        case Kind::AddScene: model.apply(app::edit::addScene(p)); break;
        case Kind::SceneLaunch: model.launchScene(p.scenes[size_t(h.scene)]); break;
        case Kind::TrackStop: model.stopTrack(p.tracks[size_t(h.track)].id); break;
        case Kind::Mute: model.apply(document::cmd::setTrack(p.tracks[size_t(h.track)].uid, "mute", !p.tracks[size_t(h.track)].mute)); break;
        case Kind::Arm: model.recording().arm(p.tracks[size_t(h.track)].uid, !model.recording().armed(p.tracks[size_t(h.track)].uid)); break;
        case Kind::Solo: model.apply(document::cmd::setTrack(p.tracks[size_t(h.track)].uid, "solo", !p.tracks[size_t(h.track)].solo)); break;
        case Kind::TrackHeader: model.selectTrack(p.tracks[size_t(h.track)].uid); break;
        case Kind::SlotLaunch: {
            const auto& tr = p.tracks[size_t(h.track)];
            const auto& sc = p.scenes[size_t(h.scene)];
            if (p.clips.count(tr.id + "|" + sc)) model.launchClip(tr.id, sc); else model.stopTrack(tr.id);
            model.selectClip({tr.uid, sc, ""});
            break;
        }
        case Kind::Slot: {
            const auto& tr = p.tracks[size_t(h.track)];
            const auto& sc = p.scenes[size_t(h.scene)];
            if (p.clips.count(tr.id + "|" + sc)) model.selectClip({tr.uid, sc, ""});
            else model.selectSlot(tr.uid, sc);   // an empty slot: remembered so the browser can fill it
            break;
        }
        default: break;
    }
    repaint();
}

void SessionView::mouseDoubleClick(const juce::MouseEvent& e) {
    const auto h = hitAt(e.getPosition());
    const auto& p = model.project();
    if (h.kind == Kind::Slot) {
        const auto& tr = p.tracks[size_t(h.track)];
        const auto& sc = p.scenes[size_t(h.scene)];
        if (tr.kind == project::TrackKind::Audio || tr.kind == project::TrackKind::Bus) return;
        if (!p.clips.count(tr.id + "|" + sc)) model.apply(app::edit::newSessionClip(tr.uid, sc));
        model.selectClip({tr.uid, sc, ""});
    } else if (h.kind == Kind::TrackHeader) {
        const auto uid = p.tracks[size_t(h.track)].uid;
        promptText("Rename track", p.tracks[size_t(h.track)].name, [this, uid](juce::String n) { if (n.isNotEmpty()) model.apply(document::cmd::setTrack(uid, "name", n.toStdString())); });
    }
}

void SessionView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    scrollX_ -= int(w.deltaX * 600.0f);
    scrollY_ -= int(w.deltaY * 600.0f);
    clampScroll();
    repaint();
}

bool SessionView::keyPressed(const juce::KeyPress& k) {
    const auto& p = model.project();
    const auto& sel = model.selection();
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) {
        if (sel.track && !sel.scene.empty() && sel.clip.valid() && sel.clip.arrKey.empty()) { model.apply(app::edit::clearSessionClip(sel.track, sel.scene)); return true; }
    }
    if (k == juce::KeyPress::returnKey && sel.track && !sel.scene.empty()) {
        const auto* t = app::edit::findTrack(p, sel.track);
        if (t && p.clips.count(t->id + "|" + sel.scene)) { model.launchClip(t->id, sel.scene); return true; }
    }
    return false;
}

void SessionView::showAddTrackMenu() {
    juce::PopupMenu m;
    m.addItem(1, "Synth track");
    m.addItem(2, "Drum track");
    m.addItem(3, "Audio track");
    m.addItem(4, "Bus");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int r) {
        using K = project::TrackKind;
        const K kinds[] = {K::Synth, K::Drum, K::Audio, K::Bus};
        if (r >= 1 && r <= 4) {
            model.apply(app::edit::addTrack(model.project(), kinds[r - 1]));
            model.selectTrack(model.project().tracks.back().uid);
        }
    });
}

void SessionView::contextMenu(const Hit& h) {
    const auto& p = model.project();
    juce::PopupMenu m;
    if (h.kind == Kind::TrackHeader || h.kind == Kind::Mute || h.kind == Kind::Solo || h.kind == Kind::TrackStop) {
        const auto uid = p.tracks[size_t(h.track)].uid;
        m.addItem(1, "Rename...");
        m.addItem(2, "Move left", h.track > 0);
        m.addItem(3, "Move right", h.track + 1 < trackCount());
        m.addSeparator();
        m.addItem(4, "Delete track");
        m.showMenuAsync({}, [this, uid, h](int r) {
            const auto& pr = model.project();
            if (r == 1) promptText("Rename track", pr.tracks[size_t(h.track)].name, [this, uid](juce::String n) { if (n.isNotEmpty()) model.apply(document::cmd::setTrack(uid, "name", n.toStdString())); });
            else if (r == 2) model.apply({"track.move", {{"uid", uid}, {"index", h.track - 1}}});
            else if (r == 3) model.apply({"track.move", {{"uid", uid}, {"index", h.track + 1}}});
            else if (r == 4) model.apply(app::edit::removeTrack(uid));
        });
    } else if (h.kind == Kind::Slot || h.kind == Kind::SlotLaunch) {
        const auto& tr = p.tracks[size_t(h.track)];
        const auto sc = p.scenes[size_t(h.scene)];
        const bool has = p.clips.count(tr.id + "|" + sc) != 0;
        const bool noteTrack = tr.kind == project::TrackKind::Synth || tr.kind == project::TrackKind::Drum;
        m.addItem(1, "Create clip", !has && noteTrack);
        m.addItem(2, "Delete clip", has);
        m.addItem(3, "Duplicate to next scene", has && h.scene + 1 < sceneCount());
        m.showMenuAsync({}, [this, uid = tr.uid, sc, h](int r) {
            const auto& pr = model.project();
            if (r == 1) model.apply(app::edit::newSessionClip(uid, sc));
            else if (r == 2) model.apply(app::edit::clearSessionClip(uid, sc));
            else if (r == 3) {
                const auto* t = app::edit::findTrack(pr, uid);
                if (!t) return;
                auto it = pr.clips.find(t->id + "|" + sc);
                if (it != pr.clips.end())
                    model.apply({"clip.set", {{"track", uid}, {"scene", pr.scenes[size_t(h.scene + 1)]}, {"clip", project::clipToJson(it->second)}}});
            }
        });
    } else if (h.kind == Kind::SceneHeader || h.kind == Kind::SceneLaunch) {
        const auto id = p.scenes[size_t(h.scene)];
        m.addItem(1, "Delete scene");
        m.showMenuAsync({}, [this, id](int r) { if (r == 1) model.apply(app::edit::removeScene(id)); });
    }
}

bool SessionView::isInterestedInDragSource(const SourceDetails& d) { return d.description.toString().startsWith("sample:"); }
void SessionView::itemDragMove(const SourceDetails& d) {
    const auto h = hitAt(d.localPosition);
    const juce::Point<int> slot = h.kind == Kind::Slot || h.kind == Kind::SlotLaunch ? juce::Point<int>(h.track, h.scene) : juce::Point<int>(-1, -1);
    if (slot != dropSlot_) { dropSlot_ = slot; repaint(); }
}
void SessionView::itemDropped(const SourceDetails& d) {
    const auto h = hitAt(d.localPosition);
    dropSlot_ = {-1, -1};
    repaint();
    if (h.kind != Kind::Slot && h.kind != Kind::SlotLaunch) return;
    const auto& p = model.project();
    const auto& tr = p.tracks[size_t(h.track)];
    if (tr.kind != project::TrackKind::Audio) return;
    const std::string id = d.description.toString().fromFirstOccurrenceOf("sample:", false, false).toStdString();
    const auto buf = model.sampleBank()->get(id);
    if (!buf) return;
    project::Clip c;
    c.len = std::max(1.0, std::ceil(buf->duration() * p.meta.bpm / 60.0)) * 96.0;  // whole beats
    c.audio = project::AudioClipData{};
    c.audio->sampleId = id;
    c.audio->sampleName = model.sampleName(id);
    model.apply({"clip.set", {{"track", tr.uid}, {"scene", p.scenes[size_t(h.scene)]}, {"clip", project::clipToJson(c)}}});
}

}  // namespace ddaw::ui
