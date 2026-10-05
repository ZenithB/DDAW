#include "app/ui/MainComponent.h"

namespace ddaw::ui {

MainComponent::MainComponent(app::AppModel& m)
    : model_(m), live_(m), controllers_(m, live_), transport_(m), browser_(m), session_(m), arrangement_(m), mixer_(m), clip_(m), devices_(m), input_(m, live_), mod_(m), arate_(m), morph_(m, &controllers_) {
    for (juce::Component* c : std::initializer_list<juce::Component*>{&transport_, &browser_, &session_, &arrangement_, &mixer_, &clip_, &devices_, &input_, &mod_, &arate_, &morph_})
        addAndMakeVisible(c);
    transport_.onAudioSettings = [this] { if (onAudioSettings) onAudioSettings(); };
    listener_ = model_.addListener([this](app::ModelEvent e) { onModel(e); });
    showMain(MainTab::Session);
    showDetail(DetailTab::Devices);
    setWantsKeyboardFocus(true);
    startTimerHz(30);
    onModel(app::ModelEvent::Document);
}

MainComponent::~MainComponent() { model_.removeListener(listener_); }

void MainComponent::onModel(app::ModelEvent e) {
    transport_.refresh(e);
    browser_.refresh(e);
    session_.refresh(e);
    arrangement_.refresh(e);
    mixer_.refresh(e);
    clip_.refresh(e);
    devices_.refresh(e);
    input_.refresh(e);
    mod_.refresh(e);
    arate_.refresh(e);
    morph_.refresh(e);
    // opening a clip brings its editor forward
    if (e == app::ModelEvent::Selection && model_.selection().clip.valid() && detailTab_ != DetailTab::Clip) showDetail(DetailTab::Clip);
    if (e == app::ModelEvent::Transport && model_.arrangementMode() && model_.meters().playing && mainTab_ == MainTab::Session) {}
    if (e == app::ModelEvent::Recording || e == app::ModelEvent::Document) { if (!model_.status().empty()) { status_ = model_.status(); } }
    repaint();
}

void MainComponent::timerCallback() {
    transport_.tick();
    if (mainTab_ == MainTab::Session) session_.tick();
    if (mainTab_ == MainTab::Arrangement) arrangement_.tick();
    if (mainTab_ == MainTab::Mixer) mixer_.tick();
    if (detailTab_ == DetailTab::Clip) clip_.tick();
    if (detailTab_ == DetailTab::Input) input_.tick();
    if (detailTab_ == DetailTab::Modulation) mod_.tick();
    model_.tick();
}

void MainComponent::showMain(MainTab t) {
    mainTab_ = t;
    session_.setVisible(t == MainTab::Session);
    arrangement_.setVisible(t == MainTab::Arrangement);
    mixer_.setVisible(t == MainTab::Mixer);
    repaint();
}

void MainComponent::showDetail(DetailTab t) {
    detailTab_ = t;
    clip_.setVisible(t == DetailTab::Clip);
    devices_.setVisible(t == DetailTab::Devices);
    input_.setVisible(t == DetailTab::Input);
    mod_.setVisible(t == DetailTab::Modulation);
    arate_.setVisible(t == DetailTab::AudioRate);
    morph_.setVisible(t == DetailTab::Morph);
    repaint();
}

void MainComponent::resized() {
    auto r = getLocalBounds();
    transport_.setBounds(r.removeFromTop(kTransportH));
    r.removeFromBottom(kStatusH);
    browser_.setBounds(r.removeFromLeft(kBrowserW));
    detailH_ = juce::jlimit(120, std::max(120, r.getHeight() - 160), detailH_);
    auto detail = r.removeFromBottom(detailH_);
    splitRect_ = detail.removeFromTop(6);
    detailStrip_ = detail.removeFromTop(kTabH);
    clip_.setBounds(detail);
    devices_.setBounds(detail);
    input_.setBounds(detail);
    mod_.setBounds(detail);
    arate_.setBounds(detail);
    morph_.setBounds(detail);
    mainStrip_ = r.removeFromTop(kTabH);
    session_.setBounds(r);
    arrangement_.setBounds(r);
    mixer_.setBounds(r);
}

void MainComponent::paintTabs(juce::Graphics& g, juce::Rectangle<int> strip, const std::vector<juce::String>& names, int active) {
    g.setColour(col::panel);
    g.fillRect(strip);
    int x = strip.getX() + 6;
    g.setFont(uiFont(12.5f, true));
    for (int i = 0; i < int(names.size()); ++i) {
        const int w = 18 + int(textWidth(uiFont(12.5f, true), names[size_t(i)]));
        auto tr = juce::Rectangle<int>(x, strip.getY() + 4, w, strip.getHeight() - 4);
        if (i == active) { fillRounded(g, tr.toFloat().withTrimmedBottom(-4), col::bg, 4.0f); g.setColour(col::text); }
        else g.setColour(col::dim);
        g.drawText(names[size_t(i)], tr, juce::Justification::centred);
        x += w + 2;
    }
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(col::bg);
    paintTabs(g, mainStrip_, {"Session", "Arrangement", "Mixer"}, int(mainTab_));
    paintTabs(g, detailStrip_, {"Clip", "Devices", "Input", "Modulation", "Audio-rate", "Morph"}, int(detailTab_));
    // detail strip: selected track / clip summary on the right
    {
        const auto& p = model_.project();
        const auto* t = app::edit::findTrack(p, model_.selection().track);
        g.setColour(col::dim);
        g.setFont(uiFont(12.0f));
        g.drawText(t ? juce::String(t->name) : juce::String("no track selected"), detailStrip_.withTrimmedRight(12), juce::Justification::centredRight);
    }
    g.setColour(col::line);
    g.fillRect(splitRect_.withHeight(1).withY(splitRect_.getY() + 2));
    g.setColour(col::panel);
    g.fillRect(juce::Rectangle<int>(0, getHeight() - kStatusH, getWidth(), kStatusH));
    g.setColour(col::dim);
    g.setFont(uiFont(11.5f));
    g.drawText(status_, juce::Rectangle<int>(10, getHeight() - kStatusH, getWidth() - 20, kStatusH), juce::Justification::centredLeft);
}

void MainComponent::mouseDown(const juce::MouseEvent& e) {
    if (splitRect_.expanded(0, 3).contains(e.getPosition())) { draggingSplit_ = true; return; }
    auto hitTabs = [&](juce::Rectangle<int> strip, const std::vector<juce::String>& names) {
        int x = strip.getX() + 6;
        for (int i = 0; i < int(names.size()); ++i) {
            const int w = 18 + int(textWidth(uiFont(12.5f, true), names[size_t(i)]));
            if (juce::Rectangle<int>(x, strip.getY(), w, strip.getHeight()).contains(e.getPosition())) return i;
            x += w + 2;
        }
        return -1;
    };
    if (int i = hitTabs(mainStrip_, {"Session", "Arrangement", "Mixer"}); i >= 0) {
        showMain(MainTab(i));
        if (MainTab(i) != MainTab::Mixer) { model_.setArrangementMode(MainTab(i) == MainTab::Arrangement); model_.notify(app::ModelEvent::Transport); }
    } else if (int j = hitTabs(detailStrip_, {"Clip", "Devices", "Input", "Modulation", "Audio-rate", "Morph"}); j >= 0) showDetail(DetailTab(j));
}

void MainComponent::mouseDrag(const juce::MouseEvent& e) {
    if (!draggingSplit_) return;
    detailH_ = getHeight() - kStatusH - e.y;
    resized();
}
void MainComponent::mouseUp(const juce::MouseEvent&) { draggingSplit_ = false; }

bool MainComponent::keyStateChanged(bool) { return live_.keyStateChanged(juce::Component::getCurrentlyFocusedComponent()); }

bool MainComponent::keyPressed(const juce::KeyPress& k) {
    if (live_.keyPressed(k)) return true;
    if (k == juce::KeyPress::spaceKey) { model_.togglePlay(); return true; }
    if (k == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) { model_.undo(); return true; }
    if (k == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { model_.redo(); return true; }
    if (k == juce::KeyPress::tabKey) { showMain(mainTab_ == MainTab::Session ? MainTab::Arrangement : MainTab::Session); model_.setArrangementMode(mainTab_ == MainTab::Arrangement); model_.notify(app::ModelEvent::Transport); return true; }
    return false;
}

}  // namespace ddaw::ui
