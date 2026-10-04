#pragma once
// Builds a Graph from a project snapshot on the builder thread (ARCH 7). Everything it cannot yet
// honour is reported in `unsupported`, never silently dropped.
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/Graph.h"
#include "engine/Scheduler.h"
#include "project/Project.h"
#include "project/SampleBank.h"

namespace ddaw::engine {

// String key -> numeric address, valid only for the epoch of the graph it was built with.
// Grammar (ARCH 9, as synthyy's CommandResolver): "<trackId>|inst|<key>", "<trackId>|<fxId>|<key>"
// (fxId = the device's id, else its type; key "out" is the device output gain), "<trackId>|mix|<gain|pan|
// sendA|sendB>", "<trackId>|send|<busId>", "master|gain", "master|<fxId>|<key>".
class ParamResolver {
public:
    // Scene and track ids resolve to the indices commands carry (ClipLaunch, NoteOn, ...).
    void insertScene(std::string id, uint16_t idx) { scenes_.emplace(std::move(id), idx); }
    void insertTrack(std::string id, uint16_t idx) { tracks_.emplace(std::move(id), idx); }
    bool scene(const std::string& id, uint16_t& out) const { auto it = scenes_.find(id); if (it == scenes_.end()) return false; out = it->second; return true; }
    bool track(const std::string& id, uint16_t& out) const { auto it = tracks_.find(id); if (it == tracks_.end()) return false; out = it->second; return true; }

    void insert(std::string key, ParamAddr a) { map_.emplace(std::move(key), a); }
    bool resolve(const std::string& key, ParamAddr& out) const {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        out = it->second;
        return true;
    }
    size_t size() const { return map_.size(); }

private:
    std::unordered_map<std::string, ParamAddr> map_;
    std::unordered_map<std::string, uint16_t> scenes_, tracks_;
};

struct BuildResult {
    std::unique_ptr<Graph> graph;
    ParamResolver resolver;
    std::vector<std::string> unsupported;
    // The render scope (synthyy render.ts scopeBounds): where it starts, how long it is, and which mode.
    TransportMode mode = TransportMode::Session;
    double fromTicks = 0;
    double lengthTicks = 0;
    int sceneIndex = -1;  // scene scope: the scene to launch on every track
    bool complete() const { return unsupported.empty(); }
};

// Throws std::runtime_error for unusable input (no tracks, unknown scene, empty arrangement).
// Scope kind "live" builds an interactive graph (no render length, no scene required).
// `bank` supplies decoded samples (injected into sampler-style instruments by id; a missing id plays silent).
BuildResult buildGraph(const project::Fixture& fx, double sampleRate, uint32_t epoch, const project::SampleBank* bank = nullptr);

}  // namespace ddaw::engine
