#include "document/NativeFormat.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "project/ProjectJson.h"

namespace ddaw::document {

using json = nlohmann::json;
namespace fs = std::filesystem;

void MigrationChain::run(json& doc, int from, int to, std::vector<std::string>& notes) const {
    if (from > to) throw std::runtime_error("project was saved by a newer DDAW (format " + std::to_string(from) +
                                             ", this build reads up to " + std::to_string(to) + ")");
    for (int v = from; v < to; ++v) {
        auto it = steps_.find(v);
        if (it == steps_.end()) throw std::runtime_error("no migration from project format " + std::to_string(v));
        it->second(doc, notes);
    }
}

const MigrationChain& MigrationChain::builtin() {
    static const MigrationChain chain = [] {
        MigrationChain c;
        // 0 -> 1: a synthyy ProjectJSON (no "ddaw" header) becomes a DDAW project: wrap it and assign uids.
        c.add(0, [](json& doc, std::vector<std::string>& notes) {
            if (!doc.contains("project")) doc = json{{"project", doc}};  // a bare ProjectJSON
            notes.push_back("imported a synthyy project (format 0): stable ids assigned");
        });
        return c;
    }();
    return chain;
}

namespace {

json header(project::Uid nextUid, const json& samples) {
    json h = {{"format", kFormatName}, {"version", kFormatVersion}, {"nextUid", nextUid}, {"generator", "DDAW"}};
    if (!samples.empty()) h["samples"] = samples;
    return h;
}

LoadedProject finishLoad(json doc, const MigrationChain& chain) {
    LoadedProject out;
    int version = 0;
    if (doc.is_object() && doc.contains("ddaw")) {
        const auto& h = doc["ddaw"];
        if (h.value("format", "") != kFormatName) throw std::runtime_error("not a DDAW project (unknown format tag)");
        version = h.value("version", 0);
    } else if (!(doc.is_object() && (doc.contains("tracks") || doc.contains("project")))) {
        throw std::runtime_error("not a DDAW or synthyy project");
    }
    out.fileVersion = version;
    out.migrated = version != kFormatVersion;
    chain.run(doc, version, kFormatVersion, out.notes);

    const json& body = doc.contains("project") ? doc["project"] : doc;
    out.project = project::projectFromJson(body);
    project::Uid next = doc.contains("ddaw") ? doc["ddaw"].value("nextUid", project::Uid{1}) : project::Uid{1};
    next = std::max<project::Uid>({next, project::maxUid(out.project) + 1, 1});  // never reuse a live id
    const size_t assigned = project::assignUids(out.project, next);
    if (assigned && version == kFormatVersion) out.notes.push_back("assigned " + std::to_string(assigned) + " missing ids");
    out.nextUid = next;
    if (doc.contains("ddaw") && doc["ddaw"].contains("samples"))
        for (auto& [id, v] : doc["ddaw"]["samples"].items()) { (void)v; out.sampleIds.push_back(id); }
    return out;
}

std::string safeFileName(const std::string& id) {
    std::string s;
    for (char c : id) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return s.empty() ? "sample" : s;
}

}  // namespace

std::string saveProjectJson(const project::Project& p, project::Uid nextUid) {
    project::Project copy = p;
    project::assignUids(copy, nextUid);  // a project never leaves memory with unassigned ids
    json doc = {{"ddaw", header(nextUid, json::object())}, {"project", project::projectToJson(copy)}};
    return doc.dump(1);
}

LoadedProject loadProjectJson(const std::string& text, const MigrationChain& chain) {
    json doc;
    try { doc = json::parse(text); }
    catch (const std::exception& e) { throw std::runtime_error(std::string("project file is not valid JSON: ") + e.what()); }
    return finishLoad(std::move(doc), chain);
}

void saveProjectPackage(const std::string& dir, const project::Project& p, project::Uid nextUid,
                        const project::SampleBank& bank, const std::map<std::string, std::string>& names) {
    project::Project copy = p;
    project::assignUids(copy, nextUid);
    fs::create_directories(fs::path(dir) / "samples");

    // every sample id the project references
    std::vector<std::string> ids;
    auto want = [&](const std::string& id) { if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id); };
    for (auto& t : copy.tracks) { want(t.inst.sampleId); for (auto& [k, v] : t.inst.padSamples) want(v); }
    auto clip = [&](const project::Clip& c) { if (c.audio) want(c.audio->sampleId); };
    for (auto& [k, c] : copy.clips) clip(c);
    for (auto& [k, a] : copy.arr) clip(a.clip);

    json samples = json::object();
    for (const auto& id : ids) {
        json entry = {{"file", ""}};
        if (auto it = names.find(id); it != names.end()) entry["name"] = it->second;
        if (auto buf = bank.get(id)) {
            const std::string file = "samples/" + safeFileName(id) + ".wav";
            project::writeWavFloat32((fs::path(dir) / file).string(), *buf);
            entry["file"] = file;
        }
        samples[id] = entry;
    }
    std::ofstream f(fs::path(dir) / "project.json");
    if (!f) throw std::runtime_error("cannot write " + dir + "/project.json");
    json doc = {{"ddaw", header(nextUid, samples)}, {"project", project::projectToJson(copy)}};
    f << doc.dump(1);
}

LoadedProject loadProjectPackage(const std::string& dir, project::SampleBank& bank) {
    std::ifstream f(fs::path(dir) / "project.json");
    if (!f) throw std::runtime_error("no project.json in " + dir);
    std::stringstream ss;
    ss << f.rdbuf();
    json doc;
    try { doc = json::parse(ss.str()); }
    catch (const std::exception& e) { throw std::runtime_error(std::string("project.json is not valid JSON: ") + e.what()); }
    json samples = doc.contains("ddaw") && doc["ddaw"].contains("samples") ? doc["ddaw"]["samples"] : json::object();
    LoadedProject out = finishLoad(std::move(doc), MigrationChain::builtin());
    for (auto& [id, entry] : samples.items()) {
        const std::string file = entry.value("file", "");
        if (file.empty()) { out.notes.push_back("sample '" + id + "' has no file in the package"); continue; }
        try { bank.put(id, project::loadWavSample((fs::path(dir) / file).string())); }
        catch (const std::exception& e) { out.notes.push_back("sample '" + id + "' could not be read: " + e.what()); }
    }
    return out;
}

}  // namespace ddaw::document
