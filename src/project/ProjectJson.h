#pragma once
// JSON (de)serialisation of the project model. The shape is synthyy's ProjectJSON plus stable `uid`
// fields, so a synthyy project parses unchanged (all uids 0) and a DDAW project is a superset.
#include <nlohmann/json.hpp>

#include "project/Project.h"

namespace ddaw::project {

// Throws std::runtime_error when the structure is unusable (no tracks array, ...). Unknown keys are ignored.
Project projectFromJson(const nlohmann::json& j);
nlohmann::json projectToJson(const Project& p);

// Per-object forms, used by the document's commands.
nlohmann::json trackToJson(const Track& t);
Track trackFromJson(const nlohmann::json& j);
nlohmann::json clipToJson(const Clip& c);
Clip clipFromJson(const nlohmann::json& j);
nlohmann::json deviceToJson(const DeviceSpec& d, bool isInstrument = false);
DeviceSpec deviceFromJson(const nlohmann::json& j);
nlohmann::json lfoToJson(const LfoSpec& l);
LfoSpec lfoFromJson(const nlohmann::json& j);
nlohmann::json macroToJson(const MacroSpec& m);
MacroSpec macroFromJson(const nlohmann::json& j);
nlohmann::json perfToJson(const PerfSpec& p);
PerfSpec perfFromJson(const nlohmann::json& j);
nlohmann::json pointsToJson(const std::vector<AutoPoint>& pts);
std::vector<AutoPoint> pointsFromJson(const nlohmann::json& j);

// Assign a uid (from `nextUid`, which advances) to every addressable object that lacks one. Returns the
// number of ids assigned. Existing uids are kept, so loading a saved project is stable.
size_t assignUids(Project& p, Uid& nextUid);
// The largest uid in use (0 when none), so a loader can resume the counter.
Uid maxUid(const Project& p);

}  // namespace ddaw::project
