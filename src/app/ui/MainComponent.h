#pragma once
#include "app/ui/ArrangementView.h"
#include "app/ui/AudioRatePanel.h"
#include "app/ui/BrowserPanel.h"
#include "app/ui/ClipEditor.h"
#include "app/ui/ControllerInput.h"
#include "app/ui/DeviceChainView.h"
#include "app/ui/LiveInput.h"
#include "app/ui/MixerView.h"
#include "app/ui/ModulationPanel.h"
#include "app/ui/MorphPanel.h"
#include "app/ui/SessionView.h"
#include "app/ui/TrackingPanel.h"
#include "app/ui/TransportBar.h"

namespace ddaw::ui {

// The whole window: transport on top, browser at the left, the main view (session / arrangement /
// mixer) over a detail pane (clip editor / devices), and a status line.
class MainComponent : public juce::Component, public juce::DragAndDropContainer, private juce::Timer {
public:
    explicit MainComponent(app::AppModel& model);
    ~MainComponent() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;
    LiveInput& liveInput() { return live_; }
    ControllerInput& controllers() { return controllers_; }

    enum class MainTab { Session, Arrangement, Mixer };
    enum class DetailTab { Clip, Devices, Input, Modulation, AudioRate, Morph };
    void showMain(MainTab);
    void showDetail(DetailTab);
    MainTab mainTab() const { return mainTab_; }

    // Hooks the application sets (file dialogs and audio settings need app-level objects).
    std::function<void()> onAudioSettings;
    void setStatus(const juce::String& s) { status_ = s; repaint(); }
    // The app polls this for the window-level commands (file menu).
    TransportBar& transport() { return transport_; }
    void tick() { timerCallback(); }
    BrowserPanel& browser() { return browser_; }

private:
    void timerCallback() override;
    void onModel(app::ModelEvent);
    void paintTabs(juce::Graphics&, juce::Rectangle<int> strip, const std::vector<juce::String>& names, int active);

    app::AppModel& model_;
    int listener_ = 0;
    LiveInput live_;   // before the panels that show it
    ControllerInput controllers_;
    TransportBar transport_;
    BrowserPanel browser_;
    SessionView session_;
    ArrangementView arrangement_;
    MixerView mixer_;
    ClipEditor clip_;
    DeviceChainView devices_;
    TrackingPanel input_;
    ModulationPanel mod_;
    AudioRatePanel arate_;
    MorphPanel morph_;
    MainTab mainTab_ = MainTab::Session;
    DetailTab detailTab_ = DetailTab::Devices;
    int detailH_ = 300;
    bool draggingSplit_ = false;
    juce::Rectangle<int> mainStrip_, detailStrip_, splitRect_;
    juce::String status_;
    static constexpr int kTransportH = 48, kBrowserW = 214, kTabH = 28, kStatusH = 22;
};

}  // namespace ddaw::ui
