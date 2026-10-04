#pragma once
// Per-user application settings (not part of a project): recording options and the input channel mode.
// A missing or damaged file yields the defaults; saving never throws.
#include <string>

#include "app/model/Recording.h"

namespace ddaw::app {

struct AppSettings {
    RecordingSettings recording;
    int inputMode = 0;   // 0 stereo pair, 1 left, 2 right
};

AppSettings loadSettings(const std::string& path);
bool saveSettings(const std::string& path, const AppSettings& s);

}  // namespace ddaw::app
