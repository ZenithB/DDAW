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

// The drum machines (the original `drum` and the synthesized kits): each has eight pads, in pitch order (note pitch modulo 8), and its
// own names for them. A pad's parameters (p0_tune, p3_decay...) are labelled with the pad's name ("Kick Tune", "Closed Hat Decay").
bool isDrumKit(std::string_view type);
const char* drumPadName(std::string_view type, int pitch);
inline const char* drumPadName(int pitch) { return drumPadName("drum", pitch); }
struct DrumKitInfo { const char* type; const char* label; };
const std::vector<DrumKitInfo>& drumKits();          // what the kit dropdown on a drum track lists

}  // namespace ddaw::app
