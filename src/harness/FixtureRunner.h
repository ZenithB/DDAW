#pragma once
// Golden-fixture parity runner (ARCH 13). Imports fixtures/projects/*.json,
// renders each through the engine, compares against fixtures/golden/<name>.wav.
#include <functional>
#include "engine/OfflineRender.h"
#include <map>
#include <string>
#include <vector>

namespace ddaw::harness {

enum class Tier { Tight, Spectral, Baseline, Advisory };
enum class Status { Pass, Fail, Advisory, Unported, NoGolden, Error };

const char* toString(Tier);
const char* toString(Status);

struct FixtureReport {
    std::string name;
    Tier   tier = Tier::Advisory;
    Status status = Status::Error;
    double rmsNullDb = 0, similarity = 0, levelDb = -160, goldenLevelDb = -160;
    std::vector<std::string> notes;  // unsupported features, failure reasons
};

// Scores the Rust port reached against the browser goldens (synthyy's parity ledger). A ported
// device must stay within tolerance of these, not of an exact null: the goldens are Tone.js
// composites with no phase alignment (synthyy docs/PARITY.md).
struct Baseline {
    double spectral = 0;    // mean log-magnitude cosine
    double deltaRmsDb = 0;  // rendered RMS minus golden RMS, dB
};

struct RunOptions {
    std::string fixturesDir;           // contains projects/ and golden/
    double      sampleRate = 44100.0;
    double      goldenLeadSec = 0.02;  // synthyy goldens start transport inside the buffer
    engine::RenderOptions render;      // master limiter mode and latency trimming
    std::string dumpDir;               // when set, write each render as <name>.wav here (inspection)
    std::function<Tier(const std::string&)> tierFor;  // default: Advisory
    std::map<std::string, Baseline> baselines;        // for Tier::Baseline
    double baselineSpectralTol = 0.02;                // allowed drop below the baseline
    double baselineLevelTolDb = 1.0;                  // allowed level difference from the baseline's
};

std::vector<FixtureReport> runFixtures(const RunOptions&);
FixtureReport runFixture(const std::string& projectPath, const std::string& goldenPath, const RunOptions&);

std::string formatTable(const std::vector<FixtureReport>&);
std::string toJson(const std::vector<FixtureReport>&);

}  // namespace ddaw::harness
