#pragma once
// What the UI can offer: every device type with its category, display name and parameter schema, taken
// from the generated schema tables (no device is instantiated). JUCE-free, so it is unit tested.
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/model/PluginProvider.h"
#include "core/Device.h"
#include "project/Project.h"

namespace ddaw::app {

enum class Chain { Instrument, Effect, MidiFx };

struct DeviceInfo {
    std::string type;
    std::string label;
    std::string category;
    Chain chain;
    std::span<const ParamSpec> params;
};

const std::vector<DeviceInfo>& deviceCatalog();
const DeviceInfo* findDevice(Chain chain, std::string_view type);

// Hosted plugins (L1): their parameters are not in the static catalog but in the live host. The app sets the provider once; the
// two lookups below then work for every device, catalog or plugin. UI thread only.
void setActivePluginProvider(PluginProvider* p);
const DeviceInfo* deviceInfoFor(Chain chain, const project::DeviceSpec& d);       // null: unknown, or a plugin that is not loaded
std::string paramLabelFor(const project::DeviceSpec& d, std::string_view key);     // the plugin's own parameter name, else paramLabel(key)

// "lfoShape" -> "LFO Shape", "vibAmt" -> "Vib Amt": a readable label from a schema key.
std::string paramLabel(std::string_view key);
// A value as the UI shows it: sensible digits for the span of the range.
std::string formatParam(const ParamSpec& spec, double value);
// Normalised position 0..1 of a value on its curve, and back.
double paramToUnit(const ParamSpec& spec, double value);
double paramFromUnit(const ParamSpec& spec, double unit);

// The drum machine's eight pads, in pitch order (note pitch modulo 8).
inline const char* drumPadName(int pitch) {
    static const char* n[] = {"Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Lo Tom", "Perc", "Crash"};
    return n[((pitch % 8) + 8) % 8];
}

}  // namespace ddaw::app
