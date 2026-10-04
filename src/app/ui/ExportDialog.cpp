#include "app/ui/ExportDialog.h"

namespace ddaw::ui {

ExportDialog::ExportDialog(app::AppModel& m, std::function<void(const juce::String&)> onDone)
    : model_(m), project_(m.document().snapshot()), bank_(m.sampleBank()), onDone_(std::move(onDone)) {
    opt_.sceneId = m.selection().scene;
    const bool hasArr = !project_->arr.empty();
    opt_.range = hasArr ? app::ExportOptions::Range::Arrangement : app::ExportOptions::Range::Scene;
    const int bd[3] = {16, 24, 32};
    const double sr[3] = {44100.0, 48000.0, 96000.0};
    using R = app::ExportOptions::Range;
    const R rg[3] = {R::Arrangement, R::LoopRegion, R::Scene};
    for (int i = 0; i < 3; ++i) {
        addAndMakeVisible(bits_[i]);
        addAndMakeVisible(rate_[i]);
        addAndMakeVisible(range_[i]);
        bits_[i].onClick = [this, i, bd] { opt_.bitDepth = bd[i]; refresh(); };
        rate_[i].onClick = [this, i, sr] { opt_.sampleRate = sr[i]; refresh(); };
        range_[i].onClick = [this, i, rg] { opt_.range = rg[i]; refresh(); };
    }
    for (juce::Component* c : std::initializer_list<juce::Component*>{&stems_, &mix_, &go_, &cancel_}) addAndMakeVisible(c);
    stems_.setToggleable(true);
    mix_.setToggleable(true);
    stems_.onToggle = [this](bool on) { opt_.stems = on; refresh(); };
    mix_.onToggle = [this](bool on) { opt_.mixdown = on; refresh(); };
    go_.onClick = [this] { if (!running_) chooseFile(); };
    cancel_.onClick = [this] { cancelFlag_ = true; if (!running_ && onDone_) onDone_(""); };
    refresh();
    setSize(460, 292);
    startTimerHz(15);
}

ExportDialog::~ExportDialog() {
    cancelFlag_ = true;
    if (worker_.joinable()) worker_.join();
}

void ExportDialog::refresh() {
    for (int i = 0; i < 3; ++i) {
        bits_[i].setOn(opt_.bitDepth == (i == 0 ? 16 : i == 1 ? 24 : 32));
        rate_[i].setOn(opt_.sampleRate == (i == 0 ? 44100.0 : i == 1 ? 48000.0 : 96000.0));
    }
    using R = app::ExportOptions::Range;
    range_[0].setOn(opt_.range == R::Arrangement);
    range_[1].setOn(opt_.range == R::LoopRegion);
    range_[2].setOn(opt_.range == R::Scene);
    stems_.setOn(opt_.stems);
    mix_.setOn(opt_.mixdown);
    repaint();
}

void ExportDialog::resized() {
    auto r = getLocalBounds().reduced(16);
    r.removeFromTop(24);
    auto row = [&](Chip* c, int n, int w) {
        auto rr = r.removeFromTop(26);
        rr.removeFromLeft(86);
        for (int i = 0; i < n; ++i) { c[i].setBounds(rr.removeFromLeft(w)); rr.removeFromLeft(6); }
        r.removeFromTop(10);
    };
    row(range_, 3, 100);
    row(rate_, 3, 80);
    row(bits_, 3, 100);
    auto rr = r.removeFromTop(26);
    rr.removeFromLeft(86);
    mix_.setBounds(rr.removeFromLeft(90)); rr.removeFromLeft(6);
    stems_.setBounds(rr.removeFromLeft(90));
    r.removeFromTop(34);
    auto btn = r.removeFromBottom(30);
    go_.setBounds(btn.removeFromRight(110));
    btn.removeFromRight(8);
    cancel_.setBounds(btn.removeFromRight(80));
}

void ExportDialog::paint(juce::Graphics& g) {
    g.fillAll(col::panel2);
    g.setColour(col::text);
    g.setFont(uiFont(13.0f, true));
    g.drawText("Export audio", 16, 8, 250, 20, juce::Justification::centredLeft);
    g.setColour(col::dim);
    g.setFont(uiFont(12.0f));
    g.drawText("Range", 16, 44, 80, 26, juce::Justification::centredLeft);
    g.drawText("Sample rate", 16, 80, 80, 26, juce::Justification::centredLeft);
    g.drawText("Format", 16, 116, 80, 26, juce::Justification::centredLeft);
    g.drawText("Files", 16, 152, 80, 26, juce::Justification::centredLeft);
    g.setFont(uiFont(11.0f));
    g.setColour(col::faint);
    g.drawFittedText("Stems are one file per track, through its own effects and the buses it sends to, without the master chain.",
                     juce::Rectangle<int>(16, 186, getWidth() - 32, 30), juce::Justification::topLeft, 2);
    if (running_) {
        auto bar = juce::Rectangle<float>(16.0f, float(getHeight()) - 76.0f, float(getWidth()) - 32.0f, 8.0f);
        g.setColour(col::black);
        g.fillRoundedRectangle(bar, 3.0f);
        g.setColour(col::accent);
        g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * float(fraction_.load())), 3.0f);
        const juce::ScopedLock sl(labelLock_);
        g.setColour(col::dim);
        g.drawText(label_, juce::Rectangle<int>(16, getHeight() - 66, getWidth() - 32, 16), juce::Justification::centredLeft);
    } else if (result_.isNotEmpty()) {
        g.setColour(col::text);
        g.drawFittedText(result_, juce::Rectangle<int>(16, getHeight() - 72, getWidth() - 32, 30), juce::Justification::topLeft, 2);
    }
}

void ExportDialog::chooseFile() {
    chooser_ = std::make_unique<juce::FileChooser>("Export audio", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(juce::String(model_.title()).trimCharactersAtEnd(" *") + ".wav"), "*.wav");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              start(f.getFileExtension().isEmpty() ? f.withFileExtension("wav") : f);
                          });
}

void ExportDialog::start(const juce::File& base) {
    if (running_) return;
    if (worker_.joinable()) worker_.join();
    cancelFlag_ = false;
    finishedFlag_ = false;
    fraction_ = 0.0;
    result_ = {};
    running_ = true;
    go_.setEnabled(false);
    worker_ = std::thread([this, base, proj = project_, bank = bank_, opt = opt_] {
        finished_ = exportProject(*proj, *bank, opt, base, &cancelFlag_, [this](double f, const juce::String& l) {
            fraction_ = f;
            const juce::ScopedLock sl(labelLock_);
            label_ = l;
        });
        finishedFlag_ = true;
    });
    repaint();
}

void ExportDialog::pump() {
    if (running_) repaint();
    if (finishedFlag_.exchange(false)) {
        if (worker_.joinable()) worker_.join();
        running_ = false;
        go_.setEnabled(true);
        result_ = finished_.message;
        repaint();
        if (onDone_) onDone_(finished_.message);
    }
}

}  // namespace ddaw::ui
