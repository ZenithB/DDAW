#pragma once
// Importer for synthyy fixtures (`{ name, scope, project }`) and bare ProjectJSON.
#include <string>

#include "project/Project.h"

namespace ddaw::project {

// Throws std::runtime_error on unreadable or structurally invalid input.
// Unknown keys are ignored here; features the renderer cannot yet honour are
// reported by `unsupportedFeatures()`, never silently dropped.
Fixture importFixtureFile(const std::string& path);
Fixture importFixtureJson(const std::string& jsonText);

}  // namespace ddaw::project
