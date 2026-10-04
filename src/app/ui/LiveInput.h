#pragma once
// Live note input for the UI: every connected MIDI input, and the computer keyboard as a piano. Both feed
// the model (and so the engine's live-note path, which is polyphonic), so a chord is a chord.
//
// Keyboard map (two rows, like most DAWs): A W S E D F T G Y H U J K O L P ; = C C# D D# E F F# G G# A A# B C C# D D# E.
// Z / X move the octave down / up, C / V lower / raise the velocity. Holding Cmd / Ctrl / Alt disables the piano
// so shortcuts keep working.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
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
    bool keys_ = false;
    int octave_ = 4;
    float velocity_ = 0.8f;
};

}  // namespace ddaw::ui
