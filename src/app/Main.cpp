#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AppController.h"
#include "app/AudioHost.h"
#include "app/model/Demo.h"
#include "app/model/Settings.h"
#ifdef DDAW_HAVE_DDSP
#include "ddsp/DdspInstrument.h"
#endif

namespace {

using namespace ddaw;

class MainWindow : public juce::DocumentWindow {
public:
    MainWindow(app::AppModel& model, app::AudioHost& host, std::function<void()> quit)
        : DocumentWindow("DDAW", ui::col::bg, DocumentWindow::allButtons), model_(model) {
        setUsingNativeTitleBar(true);
        auto* main = new ui::MainComponent(model);
        setContentOwned(main, true);
        controller_ = std::make_unique<app::AppController>(model, *main, &host, std::move(quit));
        listener_ = model.addListener([this](app::ModelEvent) { setName(juce::String(model_.title()) + " - DDAW"); });
        setName(juce::String(model.title()) + " - DDAW");
        setResizable(true, true);
        setResizeLimits(1000, 640, 4000, 3000);
        centreWithSize(1440, 900);
        setVisible(true);
        main->grabKeyboardFocus();
    }
    ~MainWindow() override { model_.removeListener(listener_); }
    void closeButtonPressed() override { controller_->confirmDiscard([] { juce::JUCEApplication::getInstance()->quit(); }); }

private:
    app::AppModel& model_;
    std::unique_ptr<app::AppController> controller_;
    int listener_ = 0;
};

class DdawApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise(const juce::String& args) override {
#ifdef DDAW_HAVE_DDSP
        ddsp::registerDevices();   // the DDSP instrument: models are looked up in $DDAW_MODELS, the user's models folder, then the source tree
#endif
        // The device decides the sample rate; the engine starts with an empty graph and the model
        // (builder thread) submits the project as soon as it exists.
        host_ = std::make_unique<app::AudioHost>([](double rate) { return std::make_unique<engine::Graph>(1, rate); });
        double sr = 48000.0;
        if (auto err = host_->start(); err.isNotEmpty()) {
            juce::Logger::writeToLog("audio: " + err);
            host_->engine().prepare(sr);
        } else if (host_->stats().sampleRate > 0) sr = host_->stats().sampleRate;
        model_ = std::make_unique<app::AppModel>(host_->engine(), sr);
        wireRecording();
        const auto path = args.unquoted().trim();
        std::string err;
        if (path.isNotEmpty() && juce::File(path).exists()) { if (!model_->open(path.toStdString(), err)) juce::Logger::writeToLog("open: " + juce::String(err)); }
        else app::buildDemo(*model_);
        window_ = std::make_unique<MainWindow>(*model_, *host_, [] { juce::JUCEApplication::getInstance()->quit(); });
    }
    void shutdown() override {
        saveSettingsNow();
        window_.reset();
        host_->stop();
        model_.reset();
        host_.reset();
    }
    void systemRequestedQuit() override { quit(); }

private:
    juce::String settingsPath() const {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("DDAW").getChildFile("settings.json").getFullPathName();
    }
    void saveSettingsNow() {
        if (!model_ || !host_) return;
        app::AppSettings s;
        s.recording = model_->recording().settings();
        s.inputMode = int(host_->inputMode());
        s.mpe = model_->mpe();
        s.bendRange = model_->bendRange();
        s.mpeRange = model_->mpeRange();
        s.mpeLower = model_->mpeLowerMembers();
        s.mpeUpper = model_->mpeUpperMembers();
        app::saveSettings(settingsPath().toStdString(), s);
    }
    // Connects the recording workflow to the audio device, restores the saved options, and keeps the
    // project playable when the device restarts (a new rate, or the input being opened).
    void wireRecording() {
        const auto saved = app::loadSettings(settingsPath().toStdString());
        model_->recording().settings() = saved.recording;
        host_->setInputMode(app::AudioHost::InputMode(saved.inputMode));
        model_->setBendRange(saved.bendRange);
        model_->setMpeRange(saved.mpeRange);
        model_->setMpeZones(saved.mpeLower, saved.mpeUpper);
        model_->setMpe(saved.mpe);
        app::RecordingEnv env;
        env.openInput = [this]() -> std::string { return host_->setInputEnabled(true).toStdString(); };
        env.inputLatencyFrames = [this] { return host_->inputLatencyFrames(); };
        env.outputLatencyFrames = [this] { return host_->outputLatencyFrames(); };
        env.inputChannels = [this] {   // 0: no input is open; 2: a stereo pair is recorded; 1: one channel as mono
            const int n = host_->inputChannels();
            return n <= 0 ? 0 : (host_->inputMode() == app::AudioHost::InputMode::Stereo && n >= 2 ? 2 : 1);
        };
        env.sampleRate = [this] { return host_->stats().sampleRate > 0 ? host_->stats().sampleRate : 48000.0; };
        env.inputMode = [this] { return int(host_->inputMode()); };
        env.setInputMode = [this](int m) { host_->setInputMode(app::AudioHost::InputMode(m)); };
        env.deviceInfo = [this]() -> std::string {
            const auto s = host_->stats();
            char b[200];
            std::snprintf(b, sizeof b, "Device latency: in %d + out %d frames (%.1f ms at %.0f Hz). Prefer a wired or built-in interface; Bluetooth misreports.",
                          host_->inputLatencyFrames(), host_->outputLatencyFrames(),
                          s.sampleRate > 0 ? 1000.0 * (host_->inputLatencyFrames() + host_->outputLatencyFrames()) / s.sampleRate : 0.0, s.sampleRate);
            return b;
        };
        model_->recording().setEnv(env);
        model_->recording().setMonitor(saved.recording.monitor);
        host_->onDeviceStarted = [this](double rate) { if (model_) model_->setSampleRate(rate); };
        model_->addListener([this](app::ModelEvent e) { if (e == app::ModelEvent::Recording) saveSettingsNow(); });
    }

    std::unique_ptr<app::AudioHost> host_;
    std::unique_ptr<app::AppModel> model_;
    std::unique_ptr<MainWindow> window_;
};

}  // namespace

START_JUCE_APPLICATION(DdawApplication)
