#pragma once
// Names for controller sources and binding targets (B5), and how they read to a person.
#include <string>

namespace ddaw::app {

// target strings of a project::ControlBinding
inline std::string morphTarget(const std::string& trackId, int index, char axis) { return "morph:" + trackId + ":" + std::to_string(index) + ":" + axis; }
inline std::string macroTarget(const std::string& trackId, int index) { return "macro:" + trackId + ":" + std::to_string(index); }

// "pad:lx" -> "Left stick X", "midi:cc74" -> "MIDI CC 74"
std::string describeSource(const std::string& source);

}  // namespace ddaw::app
