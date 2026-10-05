#pragma once
// A stand-in plugin host for the model and UI tests: devices become "live" when asked, with two parameters, and the tests
// can read back what the app asked of it.
#include <map>
#include <set>
#include <string>
#include <vector>

#include "app/model/PluginProvider.h"

namespace ddaw::testing {

struct FakeProvider : app::PluginProvider {
    std::map<size_t, float> set;                           // setValue calls by parameter index
    int editorsShown = 0;
    std::map<uint64_t, std::string> liveIds, states;
    std::set<std::string> unloadable;                     // plugin ids that fail to load
    int ensureCalls = 0, scans = 0;
    std::vector<ParamSpec> specs{{0, "p_cutoff", 0, 1, 0.25f, Curve::Linear, 0, false}, {1, "p_res", 0, 1, 0.5f, Curve::Linear, 0, false}};
    std::vector<app::PluginEntry> available() const override {
        return {{"AudioUnit#x#Fake", "Fake", "Acme", "AudioUnit", "Effect", false}, {"AudioUnit#x#FakeSynth", "FakeSynth", "Acme", "AudioUnit", "Instrument", true}};
    }
    void scan(const std::function<void(const std::string&)>&) override { ++scans; }
    bool live(uint64_t uid) const override { return liveIds.count(uid) != 0; }
    bool ensure(uint64_t uid, const std::string& id, const std::string& state, std::string& error) override {
        ++ensureCalls;
        if (unloadable.count(id)) { error = "cannot load"; return false; }
        liveIds[uid] = id;
        if (!state.empty()) states[uid] = state;
        return true;
    }
    std::span<const ParamSpec> params(uint64_t uid) const override { return live(uid) ? std::span<const ParamSpec>(specs) : std::span<const ParamSpec>(); }
    std::string paramName(uint64_t, size_t i) const override { return i == 0 ? "Cutoff Freq" : "Resonance"; }
    float value(uint64_t, size_t i) const override { return specs[i].def; }
    void setValue(uint64_t, size_t i, float v) override { set[i] = v; }
    std::string state(uint64_t uid) const override { auto it = states.find(uid); return it == states.end() ? std::string() : it->second; }
    void showEditor(uint64_t, const std::function<void()>&) override { ++editorsShown; }
    void forget(uint64_t uid) override { liveIds.erase(uid); states.erase(uid); }
    void retain(const std::set<uint64_t>& keep) override {
        for (auto it = liveIds.begin(); it != liveIds.end();) { if (!keep.count(it->first)) { states.erase(it->first); it = liveIds.erase(it); } else ++it; }
    }
};

}  // namespace ddaw::testing
