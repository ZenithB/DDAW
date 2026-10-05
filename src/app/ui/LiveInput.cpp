#include "app/ui/LiveInput.h"

#include <algorithm>

namespace ddaw::ui {

namespace {
// key -> semitones above the octave's C
const std::pair<int, int> kMap[] = {{'a', 0}, {'w', 1}, {'s', 2}, {'e', 3}, {'d', 4}, {'f', 5}, {'t', 6}, {'g', 7}, {'y', 8}, {'h', 9},
                                    {'u', 10}, {'j', 11}, {'k', 12}, {'o', 13}, {'l', 14}, {'p', 15}, {';', 16}};
}

LiveInput::LiveInput(app::AppModel& m) : model_(m) {
    for (auto& c : cc_) c = -1;
    rpnMsb_.fill(127); rpnLsb_.fill(127); dataMsb_.fill(0); dataLsb_.fill(0);
    for (size_t i = 0; i < chanNote_.size(); ++i) { chanNote_[i] = -1; chanBend_[i] = 0.0f; chanSlide_[i] = 0.0f; chanPress_[i] = 0.0f; }
    ccSent_.fill(-1);
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
    const int ch = std::clamp(msg.getChannel(), 1, 16);
    const bool member = model_.mpeMember(ch);          // an MPE member channel: one note at a time, with its own expression
    if (msg.isNoteOn()) {
        const int p = msg.getNoteNumber();
        if (member) {   // the channel's current expression first, so the note starts the way the performer is holding it
            chanNote_[size_t(ch)] = p;
            model_.noteExpression(p, 2, chanBend_[size_t(ch)]);
            model_.noteExpression(p, 0, chanSlide_[size_t(ch)]);
            model_.noteExpression(p, 1, chanPress_[size_t(ch)]);
        }
        model_.noteOn(p, msg.getFloatVelocity());
    } else if (msg.isNoteOff()) {
        if (member) chanNote_[size_t(ch)] = -1;
        model_.noteOff(msg.getNoteNumber());
    } else if (msg.isPitchWheel()) {
        const float norm = float(msg.getPitchWheelValue() - 8192) / 8192.0f;
        bend_ = msg.getPitchWheelValue();
        if (member) {
            const float semis = norm * model_.mpeRange();
            chanBend_[size_t(ch)] = semis;
            if (const int p = chanNote_[size_t(ch)]; p >= 0) model_.noteExpression(p, 2, semis);
        } else {
            model_.bend(norm * model_.bendRange());   // a bend wheel, or the MPE master channel: every note
        }
    } else if (msg.isChannelPressure()) {
        pressure_ = msg.getChannelPressureValue();
        if (member) {
            const float v = float(msg.getChannelPressureValue()) / 127.0f;
            chanPress_[size_t(ch)] = v;
            if (const int p = chanNote_[size_t(ch)]; p >= 0) model_.noteExpression(p, 1, v);
        }
    } else if (msg.isAftertouch()) {   // polyphonic key pressure: one note's pressure
        model_.noteExpression(msg.getNoteNumber(), 1, float(msg.getAfterTouchValue()) / 127.0f);
    } else if (msg.isController()) {
        const int cc = msg.getControllerNumber();
        cc_[size_t(cc & 127)] = msg.getControllerValue();
        if (cc == 101) { rpnMsb_[size_t(ch)] = msg.getControllerValue(); dataLsb_[size_t(ch)] = 0; }
        else if (cc == 100) { rpnLsb_[size_t(ch)] = msg.getControllerValue(); dataLsb_[size_t(ch)] = 0; }
        else if (cc == 6) { dataMsb_[size_t(ch)] = msg.getControllerValue(); dataLsb_[size_t(ch)] = 0; handleRpnData(ch); }
        else if (cc == 38) { dataLsb_[size_t(ch)] = msg.getControllerValue(); handleRpnData(ch); }
        else if (cc == 64) model_.sustain(msg.getControllerValue() >= 64);
        else if (cc == 74 && member) {   // slide
            const float v = float(msg.getControllerValue()) / 127.0f;
            chanSlide_[size_t(ch)] = v;
            if (const int p = chanNote_[size_t(ch)]; p >= 0) model_.noteExpression(p, 0, v);
        }
    } else if (msg.isAllNotesOff() || msg.isAllSoundOff()) {
        model_.allNotesOff();
    }
}

// Data entry for the selected registered parameter: pitch bend sensitivity (RPN 0,0: semitones + cents) sets the member or
// master range, and the MPE configuration message (RPN 0,6 on a master channel: the zone's number of member channels).
void LiveInput::handleRpnData(int ch) {
    const int msb = rpnMsb_[size_t(ch)], lsb = rpnLsb_[size_t(ch)];
    if (msb == 0 && lsb == 0) {
        const float semis = float(dataMsb_[size_t(ch)]) + float(dataLsb_[size_t(ch)]) / 100.0f;
        if (semis >= 1.0f) model_.midiSetBendRange(model_.mpeMember(ch), semis);
    } else if (msb == 0 && lsb == 6 && (ch == 1 || ch == 16)) {
        model_.midiConfigureMpe(ch, dataMsb_[size_t(ch)]);
    }
}

void LiveInput::pollControllers(const std::function<void(const std::string&, double)>& emit) {
    for (size_t n = 0; n < cc_.size(); ++n) {
        const int v = cc_[n].load();
        if (v >= 0 && v != ccSent_[n]) { ccSent_[n] = v; emit("midi:cc" + std::to_string(n), double(v) / 127.0); }
    }
    if (const int b = bend_.load(); b >= 0 && b != bendSent_) { bendSent_ = b; emit("midi:bend", double(b) / 16383.0); }
    if (const int p = pressure_.load(); p >= 0 && p != pressureSent_) { pressureSent_ = p; emit("midi:pressure", double(p) / 127.0); }
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
