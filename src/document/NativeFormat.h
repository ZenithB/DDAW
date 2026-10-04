#pragma once
// DDAW's native project format: a versioned JSON document and a package directory.
//
//   MyProject.ddaw/
//     project.json     {"ddaw": {"format":"ddaw-project","version":1,"nextUid":N,"samples":{...}}, "project": {...}}
//     samples/<id>.wav decoded samples as 32-bit float WAV (lossless, referenced by sample id)
//
// "project" is synthyy's ProjectJSON plus stable `uid`s (project/ProjectJson.h). A file without the
// "ddaw" header is a synthyy project (format version 0): it is migrated on load, which assigns uids.
// Older versions migrate through a chain of small steps; a file from a newer DDAW is refused rather
// than silently misread.
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "project/Project.h"
#include "project/SampleBank.h"

namespace ddaw::document {

constexpr int kFormatVersion = 1;
constexpr const char* kFormatName = "ddaw-project";

struct LoadedProject {
    project::Project project;
    project::Uid nextUid = 1;
    int fileVersion = 0;                 // the version the file was written with
    bool migrated = false;
    std::vector<std::string> sampleIds;  // samples the package declares (package loads only)
    std::vector<std::string> notes;      // human-readable migration / repair notes
};

// An ordered chain of migrations: step `from` upgrades a document from version `from` to `from + 1`.
class MigrationChain {
public:
    using Step = std::function<void(nlohmann::json& doc, std::vector<std::string>& notes)>;
    void add(int from, Step step) { steps_[from] = std::move(step); }
    // Throws std::runtime_error if a step is missing or `from` is newer than `to`.
    void run(nlohmann::json& doc, int from, int to, std::vector<std::string>& notes) const;
    static const MigrationChain& builtin();

private:
    std::map<int, Step> steps_;
};

// Single-file JSON. Throws std::runtime_error on malformed or unsupported input.
std::string saveProjectJson(const project::Project& p, project::Uid nextUid);
LoadedProject loadProjectJson(const std::string& text, const MigrationChain& chain = MigrationChain::builtin());

// Package directory with sample files. `bank` supplies the samples to write (ids not in the bank are
// listed but have no file); on load the declared samples are decoded into `bank`.
void saveProjectPackage(const std::string& dir, const project::Project& p, project::Uid nextUid,
                        const project::SampleBank& bank, const std::map<std::string, std::string>& sampleNames = {});
LoadedProject loadProjectPackage(const std::string& dir, project::SampleBank& bank);

}  // namespace ddaw::document
