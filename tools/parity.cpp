// ddaw_parity <fixturesDir> [--report out.json] [--strict] [--limiter brickwall|tone|bypass] [--no-trim] [--dump dir]
// Renders every fixture and prints the parity table (ARCH 13). Exit status is
// non-zero on any FAIL or ERROR; with --strict, unported fixtures also fail.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include "harness/FixtureRunner.h"

int main(int argc, char** argv) {
    using namespace ddaw::harness;
    if (argc < 2) { std::fprintf(stderr, "usage: ddaw_parity <fixturesDir> [--report out.json] [--strict] [--limiter brickwall|tone|bypass] [--no-trim] [--dump dir]\n"); return 2; }
    RunOptions opt;
    opt.fixturesDir = argv[1];
    std::string report;
    bool strict = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--strict")) strict = true;
        else if (!std::strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) opt.dumpDir = argv[++i];
        else if (!std::strcmp(argv[i], "--limiter") && i + 1 < argc) {
            const std::string m = argv[++i];
            using Mode = ddaw::engine::MasterLimiterConfig::Mode;
            opt.render.master.mode = m == "tone" ? Mode::ToneCompat : m == "bypass" ? Mode::Bypass : Mode::Brickwall;
        } else if (!std::strcmp(argv[i], "--no-trim")) opt.render.trimLatency = false;
    }
    // Tolerance tiers are declared per device with its port (ARCH 13); the CLI reads
    // them from <fixturesDir>/tiers.txt ("<name> <tight|spectral|baseline>" per line).
    std::ifstream tf(opt.fixturesDir + "/tiers.txt");
    std::string n, t;
    std::map<std::string, Tier> tiers;
    while (tf >> n >> t)
        tiers[n] = t == "tight" ? Tier::Tight : t == "spectral" ? Tier::Spectral : t == "baseline" ? Tier::Baseline : Tier::Advisory;
    // <fixturesDir>/baseline.txt: "<name> <spectral> <deltaRmsDb>" per line, from synthyy's parity ledger.
    std::ifstream bf(opt.fixturesDir + "/baseline.txt");
    double bs, bd;
    while (bf >> n >> bs >> bd) opt.baselines[n] = {bs, bd};
    // Default tier: gated against the Rust port's baseline when one exists for the fixture, else advisory.
    // (Fixtures whose devices are not ported yet report 'unported' before the tier is consulted.)
    opt.tierFor = [&](const std::string& name) {
        if (auto it = tiers.find(name); it != tiers.end()) return it->second;
        return opt.baselines.count(name) ? Tier::Baseline : Tier::Advisory;
    };

    auto reports = runFixtures(opt);
    std::fputs(formatTable(reports).c_str(), stdout);
    if (!report.empty()) std::ofstream(report) << toJson(reports);

    int bad = 0;
    for (auto& r : reports)
        if (r.status == Status::Fail || r.status == Status::Error || (strict && r.status == Status::Unported)) ++bad;
    std::printf("\n%zu fixtures, %d failing\n", reports.size(), bad);
    return bad ? 1 : 0;
}
