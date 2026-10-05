#pragma once
// Feeds gamepad and MIDI-controller values to the model's bindings (B5). Polls at 60 Hz on the UI thread: the gamepad
// through Apple's GameController framework, MIDI CCs / pitch bend / pressure from LiveInput's tables. A value is sent
// only when it changed.
#include <juce_events/juce_events.h>
#include <map>
#include <string>

#include "app/model/AppModel.h"
#include "app/platform/Gamepad.h"
#include "app/ui/LiveInput.h"

namespace ddaw::ui {

class ControllerInput : private juce::Timer {
public:
    ControllerInput(app::AppModel& m, LiveInput& live);
    ~ControllerInput() override;
    int pulses() const { return pulses_; }   // how many times a learned control was acknowledged (tests)
    bool padConnected() const { return !pads_.empty(); }
    size_t padCount() const { return pads_.size(); }
    const std::string& padName(size_t i = 0) const { static const std::string none; return i < pads_.size() ? pads_[i].name : none; }
    // One poll now (the timer does this; tests call it directly with a stubbed state).
    void pollOnce();
    // Send a value as if a control had moved (tests, on-screen controls).
    void feed(const std::string& source, double value01);
    // For tests: read this state (re-read at every poll) instead of the device; null restores the device.
    void setPadOverride(const app::GamepadState* s) { override_ = s; overrideAll_ = nullptr; }
    void setPadsOverride(const std::vector<app::GamepadState>* s) { overrideAll_ = s; override_ = nullptr; }

private:
    void timerCallback() override { pollOnce(); }
    void sendIfChanged(const std::string& source, double v);
    void deliver(const std::string& source, double v);   // to the model; a pad pulses when its control has just been learned

    app::AppModel& model_;
    LiveInput& live_;
    app::GamepadInput gamepad_;
    std::vector<app::GamepadState> pads_;
    const app::GamepadState* override_ = nullptr;
    const std::vector<app::GamepadState>* overrideAll_ = nullptr;
    std::map<std::string, double> last_;
    int pulses_ = 0;
};

}  // namespace ddaw::ui
