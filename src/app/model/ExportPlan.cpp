#include "app/model/ExportPlan.h"

#include <algorithm>
#include <cctype>

namespace ddaw::app {

std::string safeFileName(const std::string& s) {
    std::string out;
    bool lastDash = false;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '_' || c == '.') { out += char(c); lastDash = false; }
        else if (!lastDash && !out.empty()) { out += '-'; lastDash = true; }
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) out.pop_back();
    return out.empty() ? "track" : out;
}

std::vector<ExportJob> planExport(const project::Project& p, const ExportOptions& o, std::string& error) {
    std::vector<ExportJob> jobs;
    error.clear();
    if (!o.mixdown && !o.stems) { error = "choose mixdown, stems or both"; return jobs; }

    project::Scope scope;
    using R = ExportOptions::Range;
    R range = o.range;
    if (range == R::Auto) range = p.arr.empty() ? R::Scene : R::Arrangement;
    if (range == R::Arrangement) {
        if (p.arr.empty()) { error = "the arrangement is empty"; return jobs; }
        scope.kind = "arr";
    } else if (range == R::LoopRegion) {
        if (p.meta.loopEnd <= p.meta.loopStart) { error = "no loop region is set"; return jobs; }
        scope.kind = "loop";
    } else {
        if (p.scenes.empty()) { error = "there are no scenes to export"; return jobs; }
        scope.kind = "scene";
        scope.sceneId = o.sceneId.empty() ? p.scenes.front() : o.sceneId;
        if (std::find(p.scenes.begin(), p.scenes.end(), scope.sceneId) == p.scenes.end()) { error = "no scene '" + scope.sceneId + "'"; return jobs; }
    }
    if (p.tracks.empty()) { error = "there are no tracks"; return jobs; }

    if (o.mixdown) {
        ExportJob j;
        j.label = "Mixdown";
        j.fixture.scope = scope;
        j.fixture.project = p;
        jobs.push_back(std::move(j));
    }
    if (o.stems) {
        std::vector<std::string> used;
        for (size_t i = 0; i < p.tracks.size(); ++i) {
            if (p.tracks[i].kind == project::TrackKind::Bus) continue;   // a bus has no source of its own: it is inside the stems that feed it
            ExportJob j;
            std::string name = safeFileName(p.tracks[i].name.empty() ? p.tracks[i].id : p.tracks[i].name);
            for (int n = 2; std::find(used.begin(), used.end(), name) != used.end(); ++n) name = safeFileName(p.tracks[i].name) + "-" + std::to_string(n);
            used.push_back(name);
            j.suffix = "-" + name;
            j.label = "Stem: " + p.tracks[i].name;
            j.fixture.scope = scope;
            j.fixture.project = p;
            for (size_t k = 0; k < p.tracks.size(); ++k) {
                auto& t = j.fixture.project.tracks[k];
                t.solo = false;
                if (k != i && t.kind != project::TrackKind::Bus) t.mute = true;
            }
            j.fixture.project.tracks[i].mute = false;
            j.fixture.project.masterFx.clear();
            j.fixture.project.meta.masterGainDb = p.meta.masterGainDb;
            j.render.master.mode = engine::MasterLimiterConfig::Mode::Bypass;
            jobs.push_back(std::move(j));
        }
        if (jobs.empty()) error = "there are no tracks to make stems from";
    }
    return jobs;
}

}  // namespace ddaw::app
