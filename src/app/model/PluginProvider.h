#pragma once
// What the app model needs from the plugin host, as an interface so the model stays JUCE-free (and unit-testable with a
// fake). The implementation lives with the host (plugins/PluginProvider.*). All calls are made on the UI (message) thread.
#include <cstdint>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/Device.h"

namespace ddaw::app {

struct PluginEntry {
    std::string id, name, vendor, format, category;
    bool instrument = false;
};

class PluginProvider {
public:
    virtual ~PluginProvider() = default;
    // The plugins the last scan found (loaded from the cache at startup).
    virtual std::vector<PluginEntry> available() const = 0;
    virtual void scan(const std::function<void(const std::string&)>& progress) = 0;
    // The live instance behind a device (a project device is identified by its uid).
    virtual bool live(uint64_t uid) const = 0;
    // Make sure device `uid` has a live instance, created from `pluginId` and restored from `state` (base64). True when one was
    // created now; on failure false with `error` filled.
    virtual bool ensure(uint64_t uid, const std::string& pluginId, const std::string& state, std::string& error) = 0;
    // The plugin's parameters as engine controls (normalised 0..1) and their display names; empty when not live.
    virtual std::span<const ParamSpec> params(uint64_t uid) const = 0;
    virtual std::string paramName(uint64_t uid, size_t index) const = 0;
    virtual float value(uint64_t uid, size_t index) const = 0;
    virtual void setValue(uint64_t uid, size_t index, float v) = 0;
    // The plugin's state as base64 (empty: none, or not live).
    virtual std::string state(uint64_t uid) const = 0;
    // Open the plugin's own editor window; `onClosed` runs when the user closes it.
    virtual void showEditor(uint64_t uid, const std::function<void()>& onClosed) = 0;
    virtual void forget(uint64_t uid) = 0;
    // Drop every instance whose device is not in `keep` (a project was opened or created).
    virtual void retain(const std::set<uint64_t>& keep) = 0;
};

}  // namespace ddaw::app
