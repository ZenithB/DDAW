#include "app/ui/BrowserPanel.h"

#include "project/ProjectJson.h"

namespace ddaw::ui {

BrowserPanel::BrowserPanel(app::AppModel& m) : View(m) {
    addAndMakeVisible(search_);
    search_.setTextToShowWhenEmpty("Search devices", col::faint);
    search_.setFont(uiFont(12.5f));
    search_.setColour(juce::TextEditor::backgroundColourId, col::black);
    search_.setColour(juce::TextEditor::textColourId, col::text);
    search_.setColour(juce::TextEditor::outlineColourId, col::line);
    search_.onTextChange = [this] { rebuild(); repaint(); };
    rebuild();
}

void BrowserPanel::resized() { search_.setBounds(6, 6, getWidth() - 12, 24); }

void BrowserPanel::refresh(app::ModelEvent e) {
    if (e == app::ModelEvent::File || e == app::ModelEvent::Document) { rebuild(); repaint(); }
}

void BrowserPanel::rebuild() {
    rows_.clear();
    const juce::String q = search_.getText().trim().toLowerCase();
    auto section = [&](const char* title, app::Chain chain) {
        std::vector<Row> items;
        juce::String lastCat;
        std::vector<Row> body;
        for (const auto& d : app::deviceCatalog()) {
            if (d.chain != chain) continue;
            if (q.isNotEmpty() && !juce::String(d.label).toLowerCase().contains(q) && !juce::String(d.category).toLowerCase().contains(q) && !juce::String(d.type).contains(q)) continue;
            Row r;
            r.kind = Row::Device; r.label = d.label; r.chain = chain; r.key = d.type;
            body.push_back(r);
        }
        if (body.empty()) return;
        Row h; h.kind = Row::Header; h.label = title; h.open = !closed_.count(title) || q.isNotEmpty();
        rows_.push_back(h);
        if (h.open) rows_.insert(rows_.end(), body.begin(), body.end());
    };
    section("Instruments", app::Chain::Instrument);
    section("Effects", app::Chain::Effect);
    section("MIDI Effects", app::Chain::MidiFx);
    // samples
    {
        Row h; h.kind = Row::Header; h.label = "Samples"; h.open = !closed_.count("Samples");
        rows_.push_back(h);
        if (h.open) {
            Row imp; imp.kind = Row::Import; imp.label = "Import sample...";
            rows_.push_back(imp);
            for (const auto& id : model.sampleIds()) {
                if (q.isNotEmpty() && !juce::String(id).toLowerCase().contains(q)) continue;
                Row r; r.kind = Row::Sample; r.label = model.sampleName(id); r.key = id;
                rows_.push_back(r);
            }
        }
    }
}

void BrowserPanel::paint(juce::Graphics& g) {
    g.fillAll(col::panel);
    g.setColour(col::line);
    g.drawVerticalLine(getWidth() - 1, 0, float(getHeight()));
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(0, 34, getWidth() - 1, getHeight() - 34);
    int y = 34 - scroll_;
    for (size_t i = 0; i < rows_.size(); ++i, y += kRowH) {
        const auto& r = rows_[i];
        if (y + kRowH < 34 || y > getHeight()) continue;
        auto rr = juce::Rectangle<int>(0, y, getWidth() - 1, kRowH);
        if (r.kind == Row::Header) {
            g.setColour(col::panel2);
            g.fillRect(rr);
            g.setColour(col::dim);
            g.setFont(uiFont(11.0f, true));
            g.drawText(juce::String(r.open ? juce::CharPointer_UTF8("\xe2\x96\xbe  ") : juce::CharPointer_UTF8("\xe2\x96\xb8  ")) + r.label.toUpperCase(), rr.reduced(8, 0), juce::Justification::centredLeft);
        } else {
            if (int(i) == pressed_) { g.setColour(col::raised); g.fillRect(rr); }
            g.setColour(r.kind == Row::Import ? col::accent : col::text);
            g.setFont(uiFont(12.5f));
            g.drawText(r.label, rr.withTrimmedLeft(22).withTrimmedRight(6), juce::Justification::centredLeft, true);
        }
    }
}

void BrowserPanel::mouseDown(const juce::MouseEvent& e) {
    if (e.y < 34) return;
    const int i = rowAt(e.y);
    if (i < 0 || i >= int(rows_.size())) { pressed_ = -1; return; }
    pressed_ = i;
    auto& r = rows_[size_t(i)];
    if (r.kind == Row::Header) {
        if (closed_.count(r.label)) closed_.erase(r.label); else closed_.insert(r.label);
        rebuild();
        pressed_ = -1;
    } else if (r.kind == Row::Import) {
        if (onImportSample) onImportSample();
    }
    repaint();
}

void BrowserPanel::mouseDrag(const juce::MouseEvent& e) {
    if (pressed_ < 0 || pressed_ >= int(rows_.size()) || e.getDistanceFromDragStart() < 6) return;
    const auto& r = rows_[size_t(pressed_)];
    juce::String desc;
    if (r.kind == Row::Device) desc = juce::String("device:") + (r.chain == app::Chain::Instrument ? "inst" : r.chain == app::Chain::MidiFx ? "midifx" : "fx") + ":" + r.key;
    else if (r.kind == Row::Sample) desc = "sample:" + juce::String(r.key);
    else return;
    if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor(this))
        if (!c->isDragAndDropActive()) c->startDragging(desc, this);
}

void BrowserPanel::mouseDoubleClick(const juce::MouseEvent& e) {
    if (e.y < 34) return;
    const int i = rowAt(e.y);
    if (i >= 0 && i < int(rows_.size())) activate(rows_[size_t(i)]);
}

void BrowserPanel::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    scroll_ = juce::jlimit(0, std::max(0, int(rows_.size()) * kRowH - (getHeight() - 34)), scroll_ - int(w.deltaY * 500.0f));
    repaint();
}

bool BrowserPanel::activate(const Row& r) {
    const auto& p = model.project();
    const auto& sel = model.selection();
    const auto* track = app::edit::findTrack(p, sel.track);
    if (r.kind == Row::Device) {
        if (r.chain == app::Chain::Instrument) {
            if (!track || track->kind == project::TrackKind::Audio) return false;
            return model.apply(app::edit::setInstrument(p, track->uid, r.key));
        }
        if (!track) return false;
        return model.apply(app::edit::addDevice(p, track->uid, r.chain == app::Chain::MidiFx ? "midifx" : "fx", r.key));
    }
    if (r.kind == Row::Sample) {
        if (!track) return false;
        if (track->kind == project::TrackKind::Audio) {
            if (p.scenes.empty()) return false;
            const std::string scene = sel.scene.empty() ? p.scenes.front() : sel.scene;
            const auto buf = model.sampleBank()->get(r.key);
            if (!buf) return false;
            project::Clip c;
            c.len = std::max(1.0, std::ceil(buf->duration() * p.meta.bpm / 60.0)) * 96.0;
            c.audio = project::AudioClipData{};
            c.audio->sampleId = r.key;
            c.audio->sampleName = model.sampleName(r.key);
            return model.apply({"clip.set", {{"track", track->uid}, {"scene", scene}, {"clip", project::clipToJson(c)}}});
        }
        // a sampled instrument on the selected track takes it as its main sample
        if (track->inst.type == "sampler" || track->inst.type == "ksampler" || track->inst.type == "granular") {
            auto d = track->inst;
            d.sampleId = r.key;
            d.sampleName = model.sampleName(r.key);
            return model.apply({"inst.set", {{"track", track->uid}, {"device", project::deviceToJson(d, true)}}});
        }
    }
    return false;
}

}  // namespace ddaw::ui
