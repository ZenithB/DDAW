#include "app/ui/TransportBar.h"

#include "app/model/Timeline.h"
#include "dsp/Scales.h"
#include "app/ui/RecordOptions.h"

namespace ddaw::ui {

TransportBar::TransportBar(app::AppModel& m) : View(m) {
    for (Chip* c : {&play_, &stop_, &rec_, &recOpt_, &metro_, &loop_, &mode_, &undo_, &redo_, &audio_}) addAndMakeVisible(c);
    addAndMakeVisible(bpm_);
    addAndMakeVisible(swing_);
    addAndMakeVisible(key_);
    key_.onClick = [this] {
        const auto& meta = model.project().meta;
        const int root = ((int(std::lround(meta.root)) % 12) + 12) % 12, scale = dsp::scaleIndex(meta.scale);
        juce::PopupMenu rootMenu, scaleMenu, menu;
        for (int i = 0; i < 12; ++i) rootMenu.addItem(100 + i, dsp::noteName(i), true, i == root);
        for (int i = 0; i < dsp::kScaleCount; ++i) scaleMenu.addItem(200 + i, dsp::scales()[size_t(i)].name, true, i == scale);
        menu.addSubMenu("Root", rootMenu);
        menu.addSubMenu("Scale", scaleMenu);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&key_), [this](int r) {
            if (r >= 100 && r < 112) model.apply(document::cmd::setMeta("root", double(r - 100)));
            else if (r >= 200 && r < 200 + dsp::kScaleCount) model.apply(document::cmd::setMeta("scale", std::string(dsp::scales()[size_t(r - 200)].id)));
        });
    };
    metro_.setToggleable(true);
    loop_.setToggleable(true);
    mode_.setToggleable(true);
    mode_.setOnColour(col::accent);
    loop_.setOnColour(col::queued);
    play_.onClick = [this] {
        model.togglePlay();
    };
    stop_.onClick = [this] { model.stop(); model.stopAllClips(); };
    rec_.onClick = [this] {
        auto& r = model.recording();
        std::string msg;
        if (r.recording()) model.stop();
        else if (!r.start(msg)) { model.setStatus(msg); model.notify(app::ModelEvent::Recording); }
    };
    recOpt_.onClick = [this] { showRecordOptions(); };
    metro_.onToggle = [this](bool on) { model.setMetronome(on); };
    loop_.onToggle = [this](bool on) {
        auto& meta = model.project().meta;
        std::vector<document::Command> cs{document::cmd::setMeta("loopOn", on)};
        if (on && meta.loopEnd <= meta.loopStart) { cs.push_back(document::cmd::setMeta("loopStart", 0.0)); cs.push_back(document::cmd::setMeta("loopEnd", 384.0 * 4)); }
        model.applyGroup("loop", cs);
    };
    mode_.onToggle = [this](bool arr) {
        const bool was = model.meters().playing;
        model.setArrangementMode(arr);
        if (was) model.play(arr, arr ? model.cursorTicks() : 0.0);
        model.notify(app::ModelEvent::Transport);
    };
    undo_.onClick = [this] { model.undo(); };
    redo_.onClick = [this] { model.redo(); };
    audio_.onClick = [this] { if (onAudioSettings) onAudioSettings(); };
    bpm_.onBegin = [this] { model.beginGesture("tempo"); };
    bpm_.onChange = [this](double v) { model.apply(document::cmd::setMeta("bpm", v)); };
    bpm_.onEnd = [this] { model.endGesture(); };
    swing_.onBegin = [this] { model.beginGesture("swing"); };
    swing_.onChange = [this](double v) { model.apply(document::cmd::setMeta("swing", v / 100.0)); };
    swing_.onEnd = [this] { model.endGesture(); };
    refresh(app::ModelEvent::Document);
}

void TransportBar::resized() {
    auto r = getLocalBounds().reduced(10, 9);
    auto place = [&](juce::Component& c, int w, int gap = 6) { c.setBounds(r.removeFromLeft(w)); r.removeFromLeft(gap); };
    place(play_, 64);
    place(stop_, 54, 6);
    place(rec_, 50, 2);
    place(recOpt_, 24, 14);
    place(bpm_, 96);
    place(swing_, 74, 8);
    place(key_, 130, 14);
    place(metro_, 58);
    place(loop_, 50);
    place(mode_, 46, 18);
    place(undo_, 52);
    place(redo_, 52, 18);
    audio_.setBounds(r.removeFromRight(64));
    r.removeFromRight(10);
    meterRect_ = r.removeFromRight(110).toFloat();
    inMeterRect_ = meterRect_.withHeight(5.0f).translated(0, 29).withTrimmedRight(10).withTrimmedLeft(16);
    r.removeFromRight(14);
    posRect_ = r.removeFromLeft(150);
    r.removeFromLeft(16);
    titleRect_ = r;
}

void TransportBar::paint(juce::Graphics& g) {
    g.fillAll(col::panel);
    g.setColour(col::line);
    g.drawHorizontalLine(getHeight() - 1, 0, float(getWidth()));
    // position readout
    const auto pr = posRect_;
    fillRounded(g, pr.toFloat(), col::black, 3.0f);
    g.setColour(col::accent);
    g.setFont(monoFont(18.0f));
    g.drawText(pos_, pr, juce::Justification::centred);
    if (model.recording().anyArmed() || model.recording().recording()) {
        g.setColour(col::black);
        g.fillRoundedRectangle(inMeterRect_, 2.0f);
        const float idb = 20.0f * std::log10(std::max(inputPeak_, 1e-6f));
        g.setColour(idb > -1.0f ? col::meterHigh : col::rec.withAlpha(0.85f));
        g.fillRoundedRectangle(inMeterRect_.withWidth(inMeterRect_.getWidth() * juce::jlimit(0.0f, 1.0f, (idb + 60.0f) / 60.0f)), 2.0f);
        g.setColour(col::dim);
        g.setFont(monoFont(9.5f));
        g.drawText("IN", juce::Rectangle<float>(inMeterRect_.getX() - 16, inMeterRect_.getY() - 4, 14, 12).toNearestInt(), juce::Justification::centredLeft);
    }
    g.setColour(model.recording().recording() ? col::rec : col::dim);
    g.setFont(uiFont(12.0f, model.recording().recording()));
    g.drawText(recText_.isNotEmpty() ? recText_ : title_, titleRect_, juce::Justification::centredLeft, true);
    // master meter
    const auto mr = meterRect_.withHeight(8.0f).withTrimmedRight(10).translated(0, 3);
    g.setColour(col::black);
    g.fillRoundedRectangle(mr, 2.0f);
    const float db = 20.0f * std::log10(std::max(masterPeak_, 1e-6f));
    const float u = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 66.0f);
    g.setColour(db > -1.0f ? col::meterHigh : db > -12.0f ? col::meterMid : col::meterLow);
    g.fillRoundedRectangle(mr.withWidth(mr.getWidth() * u), 2.0f);
    g.setColour(col::dim);
    g.setFont(monoFont(10.5f));
    g.drawText(juce::String(db <= -60.0f ? "-inf" : juce::String(db, 1)) + " dB", mr.translated(0, 11).toNearestInt(), juce::Justification::centredLeft);
}

void TransportBar::showRecordOptions() {
    auto box = std::make_unique<RecordOptions>(model);
    juce::CallOutBox::launchAsynchronously(std::move(box), recOpt_.getScreenBounds(), nullptr);
}

void TransportBar::refresh(app::ModelEvent) {
    const auto& meta = model.project().meta;
    bpm_.setValue(meta.bpm);
    swing_.setValue(meta.swing * 100.0);
    key_.setText(juce::String(dsp::noteName(int(std::lround(meta.root)))) + " " + dsp::scales()[size_t(dsp::scaleIndex(meta.scale))].name);
    loop_.setOn(meta.loopOn);
    mode_.setOn(model.arrangementMode());
    mode_.setText(model.arrangementMode() ? "ARR" : "SES");
    metro_.setOn(model.metronome());
    undo_.setEnabled(model.canUndo());
    redo_.setEnabled(model.canRedo());
    undo_.setAlpha(model.canUndo() ? 1.0f : 0.4f);
    redo_.setAlpha(model.canRedo() ? 1.0f : 0.4f);
    title_ = model.title();
    repaint();
}

void TransportBar::tick() {
    const auto m = model.meters();
    const juce::String pos = app::barBeatLabel(m.playheadTicks, model.project().meta.tsTop);
    const bool changed = pos != pos_ || std::abs(m.masterPeak - masterPeak_) > 1e-3f || m.playing != play_.isOn();
    pos_ = pos;
    masterPeak_ = m.masterPeak;
    grDb_ = m.limiterGrDb;
    play_.setOn(m.playing);
    rec_.setOn(model.recording().recording());
    rec_.setAlpha(model.recording().anyArmed() || model.recording().recording() ? 1.0f : 0.55f);
    inputPeak_ = model.engine().inputPeak();
    juce::String rt;
    if (model.recording().recording()) rt = juce::String::formatted("REC  %.1f s", model.recording().recordedSeconds());
    const bool recChanged = rt != recText_;
    recText_ = rt;
    if (recChanged || model.recording().recording() || model.recording().anyArmed()) repaint();
    if (changed) repaint();
}

}  // namespace ddaw::ui
