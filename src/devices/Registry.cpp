#include "devices/Registry.h"

#include <string>
#include <vector>

#include "devices/effects/stub_gain.h"
#include "devices/instruments/stub_tone.h"

namespace ddaw {
// Generated at configure time from every `make_<type>()` under devices/ (cmake/DeviceRegistry.cmake).
std::unique_ptr<InstrumentDevice> createBuiltinInstrument(std::string_view type);
std::unique_ptr<EffectDevice> createBuiltinEffect(std::string_view type);


namespace {
std::vector<std::pair<std::string, EffectFactory>>& extraEffects() {
    static std::vector<std::pair<std::string, EffectFactory>> v;
    return v;
}
}  // namespace

void registerEffect(std::string_view type, EffectFactory f) {
    for (auto& [t, fn] : extraEffects()) if (t == type) { fn = f; return; }
    extraEffects().emplace_back(std::string(type), f);
}

std::unique_ptr<EffectDevice> createEffect(std::string_view type) {
    if (type == "stubgain") return std::make_unique<StubGain>();
    if (auto d = createBuiltinEffect(type)) return d;
    for (auto& [t, fn] : extraEffects()) if (t == type) return fn();
    return nullptr;
}

namespace {
std::vector<std::pair<std::string, InstrumentFactory>>& extraInstruments() {
    static std::vector<std::pair<std::string, InstrumentFactory>> v;
    return v;
}
}  // namespace

void registerInstrument(std::string_view type, InstrumentFactory f) {
    for (auto& [t, fn] : extraInstruments()) if (t == type) { fn = f; return; }
    extraInstruments().emplace_back(std::string(type), f);
}

bool instrumentRegistered(std::string_view type) {
    for (auto& [t, fn] : extraInstruments()) if (t == type) return true;
    return false;
}

std::unique_ptr<InstrumentDevice> createInstrument(std::string_view type) {
    if (type == "stubtone") return std::make_unique<StubTone>();
    if (auto d = createBuiltinInstrument(type)) return d;
    for (auto& [t, fn] : extraInstruments()) if (t == type) return fn();
    return nullptr;
}

int findParam(std::span<const ParamSpec> specs, std::string_view key) {
    for (size_t i = 0; i < specs.size(); ++i)
        if (key == specs[i].key) return static_cast<int>(i);
    return -1;
}

}  // namespace ddaw
