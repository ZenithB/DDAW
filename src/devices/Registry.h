#pragma once
// Device registry: maps synthyy type strings to factories. This is a shared
// file owned by the orchestrator; device tasks never edit it (they report the
// line to add).
#include <memory>
#include <string_view>

#include "core/Device.h"

namespace ddaw {

std::unique_ptr<EffectDevice>     createEffect(std::string_view type);      // nullptr if unknown
std::unique_ptr<InstrumentDevice> createInstrument(std::string_view type);  // nullptr if unknown

// Devices that live outside ddaw_core (the DDSP instrument needs RTNeural) register a factory at startup,
// before any graph is built. Not thread-safe against concurrent createInstrument calls.
using InstrumentFactory = std::unique_ptr<InstrumentDevice> (*)();
void registerInstrument(std::string_view type, InstrumentFactory f);
bool instrumentRegistered(std::string_view type);
using EffectFactory = std::unique_ptr<EffectDevice> (*)();
void registerEffect(std::string_view type, EffectFactory f);

// Index of the ParamSpec with this key, or -1.
int findParam(std::span<const ParamSpec> specs, std::string_view key);

}  // namespace ddaw
