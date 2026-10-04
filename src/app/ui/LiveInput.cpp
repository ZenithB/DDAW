#include "app/ui/LiveInput.h"

#include <algorithm>

namespace ddaw::ui {

namespace {
// key -> semitones above the octave's C
const std::pair<int, int> kMap[] = {{'a', 0}, {'w', 1}, {'s', 2}, {'e', 3}, {'d', 4}, {'f', 5}, {'t', 6}, {'g', 7}, {'y', 8}, {'h', 9},
                                    {'u', 10}, {'j', 11}, {'k', 12}, {'o', 13}, {'l', 14}, {'p', 15}, {';', 16}};
}

LiveInput::LiveInput(app::AppModel& m) : model_(m) {
    refreshDevices();
    startTimer(2500);
}

LiveInput::~LiveInput() {
    stopTimer();
    for (auto& d : open_) d->stop();
    releaseAll();
}

void LiveInput::refreshDevices() {
    const auto avail = juce::MidiInput::getAvailableDevices();
    // drop the ones that went away
    open_.erase(std::remove_if(open_.begin(), open_.end(), [&](const std::unique_ptr<juce::MidiInput>& d) {
        for (const auto& a : avail) if (a.identifier == d->getIdentifier()) return false;
        d->stop();
        return true;
    }), open_.end());
    for (const auto& a : avail) {
        bool have = false;
        for (const auto& d : open_) have |= d->getIdentifier() == a.identifier;
        if (have) continue;
        if (auto in = juce::MidiInput::openDevice(a.identifier, this)) { in->start(); open_.push_back(std::move(in)); }
    }
}

juce::StringArray LiveInput::deviceNames() const {
    juce::StringArray n;
    for (const auto& d : open_) n.add(d->getName());
    return n;
}

void LiveInput::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& msg) {
    // runs on the MIDI thread: the model's note calls only touch the engine's thread-safe live queue
    if (msg.isNoteOn()) model_.noteOn(msg.getNoteNumber(), msg.getFloatVelocity());
    else if (msg.isNoteOff()) model_.noteOff(msg.getNoteNumber());
    else if (msg.isController() && msg.getControllerNumber() == 64) model_.sustain(msg.getControllerValue() >= 64);
    else if (msg.isAllNotesOff() || msg.isAllSoundOff()) model_.allNotesOff();
}

int LiveInput::pitchForKey(int keyCode) const {
    for (const auto& [k, off] : kMap) if (k == keyCode) return std::clamp(12 * (octave_ + 1) + off, 0, 127);
    return -1;
}

void LiveInput::setKeyboardEnabled(bool on) {
    keys_ = on;
    if (!on) releaseAll();
}

void LiveInput::releaseAll() {
    for (const auto& [code, pitch] : sounding_) model_.noteOff(pitch);
    sounding_.clear();
    held_.clear();
}

bool LiveInput::keyStateChanged(juce::Component* focused) {
    if (!keys_) return false;
    if (dynamic_cast<juce::TextEditor*>(focused) != nullptr) { releaseAll(); return false; }
    const auto mods = juce::ModifierKeys::currentModifiers;
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown()) { releaseAll(); return false; }
    bool used = false;
    for (const auto& [code, off] : kMap) {
        const bool down = juce::KeyPress::isKeyCurrentlyDown(code);
        const bool was = sounding_.count(code) != 0;
        if (down && !was) { const int p = pitchForKey(code); model_.noteOn(p, velocity_); sounding_[code] = p; used = true; }
        else if (!down && was) { model_.noteOff(sounding_[code]); sounding_.erase(code); used = true; }
    }
    return used;
}

bool LiveInput::keyPressed(const juce::KeyPress& k) {
    if (!keys_ || k.getModifiers().isAnyModifierKeyDown()) return false;
    int c = k.getKeyCode();                                   // JUCE gives letters as lower-case key codes; accept either case
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c < 32 || c > 126) c = k.getTextCharacter();
    if (c == 'z') { octave_ = std::max(-1, octave_ - 1); return true; }
    if (c == 'x') { octave_ = std::min(8, octave_ + 1); return true; }
    if (c == 'c') { velocity_ = std::max(0.1f, velocity_ - 16.0f / 127.0f); return true; }
    if (c == 'v') { velocity_ = std::min(1.0f, velocity_ + 16.0f / 127.0f); return true; }
    for (const auto& [code, off] : kMap) if (code == c) return true;   // a piano key: consumed (keyStateChanged plays it)
    return false;
}

}  // namespace ddaw::ui
