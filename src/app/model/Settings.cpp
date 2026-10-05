#include "app/model/Settings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace ddaw::app {

AppSettings loadSettings(const std::string& path) {
    AppSettings s;
    try {
        std::ifstream in(path);
        if (!in) return s;
        const auto j = nlohmann::json::parse(in);
        const auto& r = j.value("recording", nlohmann::json::object());
        s.recording.offsetMs = std::clamp(r.value("offsetMs", 0.0), -500.0, 500.0);
        s.recording.countInBars = std::clamp(r.value("countInBars", 0), 0, 2);
        s.recording.monitor = r.value("monitor", false);
        s.recording.recordCurves = r.value("recordCurves", false);
        s.inputMode = std::clamp(j.value("inputMode", 0), 0, 2);
        s.mpe = j.value("mpe", false);
        s.bendRange = std::clamp(j.value("bendRange", 2.0f), 1.0f, 96.0f);
        s.mpeRange = std::clamp(j.value("mpeRange", 48.0f), 1.0f, 96.0f);
        s.mpeLower = std::clamp(j.value("mpeLower", 15), 0, 15);
        s.mpeUpper = std::clamp(j.value("mpeUpper", 0), 0, 15 - s.mpeLower);
    } catch (const std::exception&) {
        return AppSettings{};   // damaged: start clean rather than half-applied
    }
    return s;
}

bool saveSettings(const std::string& path, const AppSettings& s) {
    try {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        nlohmann::json j = {{"recording", {{"offsetMs", s.recording.offsetMs}, {"countInBars", s.recording.countInBars}, {"monitor", s.recording.monitor}, {"recordCurves", s.recording.recordCurves}}},
                            {"inputMode", s.inputMode}, {"mpe", s.mpe}, {"bendRange", s.bendRange}, {"mpeRange", s.mpeRange}, {"mpeLower", s.mpeLower}, {"mpeUpper", s.mpeUpper}};
        std::ofstream out(path);
        out << j.dump(2);
        return bool(out);
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace ddaw::app
