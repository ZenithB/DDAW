#include "harness/FixtureRunner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

#include "engine/OfflineRender.h"
#include "harness/Metrics.h"
#include "harness/Wav.h"
#include "project/SynthyyImport.h"

namespace fs = std::filesystem;

namespace ddaw::harness {

const char* toString(Tier t) {
    switch (t) { case Tier::Tight: return "tight"; case Tier::Spectral: return "spectral"; case Tier::Baseline: return "baseline"; default: return "advisory"; }
}
const char* toString(Status s) {
    switch (s) {
        case Status::Pass: return "pass"; case Status::Fail: return "FAIL"; case Status::Advisory: return "advisory";
        case Status::Unported: return "unported"; case Status::NoGolden: return "no-golden"; default: return "ERROR";
    }
}

FixtureReport runFixture(const std::string& projectPath, const std::string& goldenPath, const RunOptions& opt) {
    FixtureReport rep;
    rep.name = fs::path(projectPath).stem().string();
    rep.tier = opt.tierFor ? opt.tierFor(rep.name) : Tier::Advisory;
    try {
        auto fx = project::importFixtureFile(projectPath);
        auto res = engine::renderFixture(fx, opt.sampleRate, opt.render);
        rep.levelDb = toDb(rms(toMono(res.l, res.r)));
        if (!opt.dumpDir.empty()) {
            Audio dump;
            dump.sampleRate = opt.sampleRate;
            // same convention as the goldens: a lead-in of silence before the render starts
            dump.l.assign(static_cast<size_t>(std::llround(opt.goldenLeadSec * opt.sampleRate)), 0.0f);
            dump.l.insert(dump.l.end(), res.l.begin(), res.l.end());
            dump.r = dump.l;
            std::copy(res.r.begin(), res.r.end(), dump.r.end() - static_cast<std::ptrdiff_t>(res.r.size()));
            fs::create_directories(opt.dumpDir);
            writeWavPcm16((fs::path(opt.dumpDir) / (rep.name + ".wav")).string(), dump);
        }

        // Hard assertions, always on.
        if (!allFinite(res.l) || !allFinite(res.r)) {
            rep.status = Status::Fail; rep.notes.push_back("non-finite samples"); return rep;
        }
        if (!res.complete()) {
            rep.status = Status::Unported; rep.notes = res.unsupported; return rep;
        }
        if (!fs::exists(goldenPath)) { rep.status = Status::NoGolden; return rep; }

        Audio g = readWav(goldenPath);
        if (std::abs(g.sampleRate - opt.sampleRate) > 0.5) {
            rep.status = Status::Error; rep.notes.push_back("golden sample rate differs"); return rep;
        }
        const size_t lead = static_cast<size_t>(std::llround(opt.goldenLeadSec * g.sampleRate));
        auto gl = skip(g.l, lead), gr = skip(g.r, lead);
        auto gm = toMono(gl, gr), rm = toMono(res.l, res.r);
        rep.goldenLevelDb = toDb(rms(gm));
        if (rep.goldenLevelDb > -50.0 && rep.levelDb < -140.0) {
            rep.status = Status::Fail; rep.notes.push_back("render is silent but golden is not"); return rep;
        }
        rep.rmsNullDb = rmsNullDb(rm, gm);
        rep.similarity = spectralSimilarity(rm, gm);

        const double dLevel = std::abs(rep.levelDb - rep.goldenLevelDb);
        switch (rep.tier) {
            case Tier::Tight:    rep.status = rep.rmsNullDb <= -60.0 ? Status::Pass : Status::Fail; break;
            case Tier::Spectral: rep.status = (rep.similarity >= 0.98 && dLevel <= 1.0) ? Status::Pass : Status::Fail; break;
            case Tier::Baseline: {
                auto it = opt.baselines.find(rep.name);
                if (it == opt.baselines.end()) { rep.status = Status::Advisory; rep.notes.push_back("no baseline entry"); break; }
                const auto& b = it->second;
                const double delta = rep.levelDb - rep.goldenLevelDb;
                const bool levelApplies = rep.goldenLevelDb > -60.0;  // a silent golden (fx-widen) says nothing about level
                const bool specOk = rep.similarity >= b.spectral - opt.baselineSpectralTol;
                const bool levelOk = !levelApplies || std::abs(delta - b.deltaRmsDb) <= opt.baselineLevelTolDb;
                rep.status = (specOk && levelOk) ? Status::Pass : Status::Fail;
                std::ostringstream o;
                o << "baseline: spectral " << b.spectral << " (got " << rep.similarity << "), level delta " << b.deltaRmsDb
                  << " dB (got " << delta << ")";
                if (rep.status == Status::Fail) rep.notes.push_back(o.str());
                break;
            }
            case Tier::Advisory: rep.status = Status::Advisory; break;
        }
        if (rep.status == Status::Fail) {
            std::ostringstream o;
            o << "rms-null " << rep.rmsNullDb << " dB, similarity " << rep.similarity << ", level diff " << dLevel << " dB";
            rep.notes.push_back(o.str());
        }
    } catch (const std::exception& e) {
        rep.status = Status::Error;
        rep.notes.push_back(e.what());
    }
    return rep;
}

std::vector<FixtureReport> runFixtures(const RunOptions& opt) {
    std::vector<fs::path> files;
    const fs::path projects = fs::path(opt.fixturesDir) / "projects";
    if (fs::exists(projects))
        for (auto& e : fs::directory_iterator(projects))
            if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::vector<FixtureReport> out;
    for (auto& f : files)
        out.push_back(runFixture(f.string(), (fs::path(opt.fixturesDir) / "golden" / (f.stem().string() + ".wav")).string(), opt));
    return out;
}

std::string formatTable(const std::vector<FixtureReport>& rs) {
    std::ostringstream o;
    char line[160];
    std::snprintf(line, sizeof line, "%-22s %-9s %-10s %10s %8s %9s\n", "fixture", "tier", "status", "null dB", "cosine", "level dB");
    o << line;
    for (auto& r : rs) {
        std::snprintf(line, sizeof line, "%-22s %-9s %-10s %10.1f %8.4f %9.1f\n", r.name.c_str(), toString(r.tier),
                      toString(r.status), r.rmsNullDb, r.similarity, r.levelDb);
        o << line;
        for (auto& n : r.notes) o << "    - " << n << "\n";
    }
    return o.str();
}

std::string toJson(const std::vector<FixtureReport>& rs) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& r : rs)
        arr.push_back({{"name", r.name}, {"tier", toString(r.tier)}, {"status", toString(r.status)},
                       {"rmsNullDb", r.rmsNullDb}, {"similarity", r.similarity}, {"levelDb", r.levelDb},
                       {"goldenLevelDb", r.goldenLevelDb}, {"notes", r.notes}});
    return arr.dump(2);
}

}  // namespace ddaw::harness
