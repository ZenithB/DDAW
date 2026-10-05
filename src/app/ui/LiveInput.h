#pragma once
// Live note input for the UI: every connected MIDI input, and the computer keyboard as a piano. Both feed
// the model (and so the engine's live-note path, which is polyphonic), so a chord is a chord.
//
// Keyboard map (two rows, like most DAWs): A W S E D F T G Y H U J K O L P ; = C C# D D# E F F# G G# A A# B C C# D D# E.
// Z / X move the octave down / up, C / V lower / raise the velocity. Holding Cmd / Ctrl / Alt disables the piano
// so shortcuts keep working.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <set>

#include "app/model/AppModel.h"

namespace ddaw::ui {

class LiveInput : public juce::MidiInputCallback, private juce::Timer {
public:
    explicit LiveInput(app::AppModel& m);
    ~LiveInput() override;

    // MIDI devices: every available input is opened; the list is re-checked every couple of seconds (hot plug).
    void refreshDevices();
    juce::StringArray deviceNames() const;
    int openCount() const { return int(open_.size()); }
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;   // MIDI thread

    // Controller values seen on the MIDI inputs, for bindings: poll from the UI thread; `emit` gets "midi:cc<N>" (0..1),
    // "midi:bend" and "midi:pressure" for each one that changed since the last poll.
    void pollControllers(const std::function<void(const std::string&, double)>& emit);

    // The computer keyboard.
    void setKeyboardEnabled(bool on);
    bool keyboardEnabled() const { return keys_; }
    // Call from a component's keyStateChanged(); returns true when the keys were used as a piano.
    bool keyStateChanged(juce::Component* focused);
    // Z / X / C / V handling; call from keyPressed(). True when handled.
    bool keyPressed(const juce::KeyPress&);
    int octave() const { return octave_; }
    float velocity() const { return velocity_; }
    // Note a key code plays, or -1 (public for tests).
    int pitchForKey(int keyCode) const;

private:
    void timerCallback() override { refreshDevices(); }
    void releaseAll();

    app::AppModel& model_;
    std::vector<std::unique_ptr<juce::MidiInput>> open_;
    std::set<int> held_;            // key codes currently sounding a note
    std::map<int, int> sounding_;   // key code -> pitch it started (an octave change must not strand a note)
    std::array<std::atomic<int>, 128> cc_;     // last value of each controller, -1 until seen (written by the MIDI thread)
    std::array<int, 128> ccSent_;
    std::atomic<int> bend_{-1}, pressure_{-1};
    // MPE: the note sounding on each member channel (1-16; -1 none) and the channel's latest expression, so the values a
    // controller sends before a note-on shape the note from its first sample.
    std::array<std::atomic<int>, 17> chanNote_;
    std::array<std::atomic<float>, 17> chanBend_, chanSlide_, chanPress_;
    // Registered-parameter state per channel (MIDI thread): the selected RPN and the data entry bytes
    std::array<int, 17> rpnMsb_, rpnLsb_, dataMsb_, dataLsb_;
    void handleRpnData(int ch);
    int bendSent_ = -1, pressureSent_ = -1;
    bool keys_ = false;
    int octave_ = 4;
    float velocity_ = 0.8f;
};

}  // namespace ddaw::ui
