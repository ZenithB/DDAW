#pragma once
// Per-user application settings (not part of a project): recording options and the input channel mode.
// A missing or damaged file yields the defaults; saving never throws.
#include <string>

#include "app/model/Recording.h"

namespace ddaw::app {

struct AppSettings {
    RecordingSettings recording;
    int inputMode = 0;   // 0 stereo pair, 1 left, 2 right
    bool mpe = false;            // MIDI input: MPE member channels carry per-note expression (B5)
    float bendRange = 2.0f;      // semitones: a bend wheel / the MPE master channel
    float mpeRange = 48.0f;      // semitones: per-note bend on MPE member channels
    int mpeLower = 15, mpeUpper = 0;   // MPE zones: member channels in the lower (master 1) and upper (master 16) zone
};

AppSettings loadSettings(const std::string& path);
bool saveSettings(const std::string& path, const AppSettings& s);

}  // namespace ddaw::app
