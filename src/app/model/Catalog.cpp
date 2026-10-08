#include "app/model/Catalog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "ddsp/DdspParams.h"
#include "devices/instruments/follow.h"
#include "devices/schema/Schema.generated.h"
#include "devices/schema/SchemaB4.h"
#include "devices/schema/SchemaB6.h"
#include "devices/schema/SchemaExt.h"
#include "dsp/Scales.h"

namespace ddaw::app {

namespace {

template <size_t N> std::span<const ParamSpec> sp(const ParamSpec (&a)[N]) { return std::span<const ParamSpec>(a, N); }
std::span<const ParamSpec> sp(std::span<const ParamSpec> s) { return s; }

std::vector<DeviceInfo> build() {
    using namespace schema;
    using C = Chain;
    return {
        {"poly", "Poly Synth", "Synth", C::Instrument, sp(kInstPoly)},
        {"mono", "Mono Synth", "Synth", C::Instrument, sp(kInstMono)},
        {"duo", "Duo", "Synth", C::Instrument, sp(kInstDuo)},
        {"fm", "FM", "Synth", C::Instrument, sp(kInstFm)},
        {"fmop", "FM Operators", "Synth", C::Instrument, sp(kInstFmop)},
        {"harmnoise", "Harmonic + Noise", "Synth", C::Instrument, sp(kInstHarmnoise)},
        {"subtractive", "Subtractive", "Synth", C::Instrument, sp(kInstSubtractive)},
        {"wavetable", "Wavetable", "Synth", C::Instrument, sp(kInstWavetable)},
        {"waveshaper", "Waveshaper", "Synth", C::Instrument, sp(kInstWaveshaper)},
        {"keys", "Keys", "Keys and Plucks", C::Instrument, sp(kInstKeys)},
        {"pluck", "Pluck", "Keys and Plucks", C::Instrument, sp(kInstPluck)},
        {"modal", "Modal", "Keys and Plucks", C::Instrument, sp(kInstModal)},
        {"drum", "Drum Machine", "Drums", C::Instrument, sp(kInstDrum)},
        {"perc", "Percussion", "Drums", C::Instrument, sp(kInstPerc)},
        {"ddsp", "DDSP Instrument", "Tracking", C::Instrument, sp(kInstDdsp)},
        {"follow", "Voice Follower", "Tracking", C::Instrument, sp(kInstFollow)},
        {"sampler", "Sampler", "Sampled", C::Instrument, sp(kInstSampler)},
        {"ksampler", "Key Sampler", "Sampled", C::Instrument, sp(kInstKsampler)},
        {"granular", "Granular", "Sampled", C::Instrument, sp(kInstGranular)},

        {"eq", "EQ Three", "EQ and Filter", C::Effect, sp(kFxEq)},
        {"eq7", "EQ Seven", "EQ and Filter", C::Effect, sp(kFxEq7)},
        {"filter", "Filter", "EQ and Filter", C::Effect, sp(kFxFilter)},
        {"autofilt", "Auto Filter", "EQ and Filter", C::Effect, sp(kFxAutofilt)},
        {"comp", "Compressor", "Dynamics", C::Effect, sp(kFxComp)},
        {"opto", "Opto Compressor", "Dynamics", C::Effect, sp(kFxOpto)},
        {"mbcomp", "Multiband Comp", "Dynamics", C::Effect, sp(kFxMbcomp)},
        {"gate", "Gate", "Dynamics", C::Effect, sp(kFxGate)},
        {"duck", "Ducker", "Dynamics", C::Effect, sp(kFxDuck)},
        {"delay", "Delay", "Time", C::Effect, sp(kFxDelay)},
        {"pingpong", "Ping Pong", "Time", C::Effect, sp(kFxPingpong)},
        {"reverb", "Reverb", "Time", C::Effect, sp(kFxReverb)},
        {"plate", "Plate", "Time", C::Effect, sp(kFxPlate)},
        {"chorus", "Chorus", "Modulation", C::Effect, sp(kFxChorus)},
        {"phaser", "Phaser", "Modulation", C::Effect, sp(kFxPhaser)},
        {"vib", "Vibrato", "Modulation", C::Effect, sp(kFxVib)},
        {"trem", "Tremolo", "Modulation", C::Effect, sp(kFxTrem)},
        {"autopan", "Auto Pan", "Modulation", C::Effect, sp(kFxAutopan)},
        {"dist", "Distortion", "Colour", C::Effect, sp(kFxDist)},
        {"crush", "Bit Crusher", "Colour", C::Effect, sp(kFxCrush)},
        {"cheby", "Chebyshev", "Colour", C::Effect, sp(kFxCheby)},
        {"widen", "Widener", "Utility", C::Effect, sp(kFxWiden)},
        {"shift", "Pitch Shift", "Pitch", C::Effect, sp(kFxShift)},
        {"autotune", "Auto-Tune", "Pitch", C::Effect, sp(kFxAutotuneKey)},

        {"scale", "Scale", "MIDI", C::MidiFx, sp(kMidiFxScale)},
        {"chord", "Chord", "MIDI", C::MidiFx, sp(kMidiFxChord)},
        {"arp", "Arpeggiator", "MIDI", C::MidiFx, sp(kMidiFxArp)},
        {"velo", "Velocity", "MIDI", C::MidiFx, sp(kMidiFxVelo)},
        {"rand", "Random", "MIDI", C::MidiFx, sp(kMidiFxRand)},
    };
}

}  // namespace

const std::vector<DeviceInfo>& deviceCatalog() {
    static const std::vector<DeviceInfo> c = build();
    return c;
}

const DeviceInfo* findDevice(Chain chain, std::string_view type) {
    for (const auto& d : deviceCatalog())
        if (d.chain == chain && d.type == type) return &d;
    return nullptr;
}

std::string paramLabel(std::string_view key) {
    static const char* const kAcronyms[] = {"lfo", "eq", "fm", "adsr", "hz", "db", "mix"};
    std::string out;
    std::string word;
    auto flush = [&] {
        if (word.empty()) return;
        std::string lower = word;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool acr = false;
        for (const char* a : kAcronyms) if (lower == a && lower != "mix") acr = true;
        if (acr) for (char& c : word) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        else word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        if (!out.empty()) out += ' ';
        out += word;
        word.clear();
    };
    for (size_t i = 0; i < key.size(); ++i) {
        const char c = key[i];
        if (c == '_') { flush(); continue; }
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool prevLower = i > 0 && std::islower(static_cast<unsigned char>(key[i - 1])) != 0;
        if (upper && prevLower) flush();
        word += c;
    }
    flush();
    return out;
}

std::string formatParam(const ParamSpec& s, double v) {
    // a key (autotune): note and scale names, with -1 meaning "the project's"
    if (s.min < 0.0f && (std::string_view(s.key) == "root" || std::string_view(s.key) == "scale")) {   // (-1 = project: only the key parameters go below 0)
        const int i = int(std::lround(v));
        if (i < 0) return "Project";
        return std::string_view(s.key) == "root" ? std::string(dsp::noteName(i)) : std::string(dsp::scales()[size_t(std::min(i, dsp::kScaleCount - 1))].name);
    }
    char buf[32];
    const double span = std::fabs(double(s.max) - double(s.min));
    if (s.curve == Curve::Stepped || (span >= 12 && std::fabs(v - std::round(v)) < 1e-9)) std::snprintf(buf, sizeof buf, "%d", int(std::lround(v)));
    else if (std::fabs(v) >= 1000) std::snprintf(buf, sizeof buf, "%.0f", v);
    else if (std::fabs(v) >= 100) std::snprintf(buf, sizeof buf, "%.1f", v);
    else if (std::fabs(v) >= 10) std::snprintf(buf, sizeof buf, "%.2f", v);
    else std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

double paramToUnit(const ParamSpec& s, double v) {
    const double lo = s.min, hi = s.max;
    if (hi <= lo) return 0.0;
    v = std::clamp(v, lo, hi);
    if (s.curve == Curve::Exponential && lo > 0.0) return std::log(v / lo) / std::log(hi / lo);
    return (v - lo) / (hi - lo);
}

double paramFromUnit(const ParamSpec& s, double u) {
    u = std::clamp(u, 0.0, 1.0);
    const double lo = s.min, hi = s.max;
    double v = (s.curve == Curve::Exponential && lo > 0.0) ? lo * std::pow(hi / lo, u) : lo + u * (hi - lo);
    if (s.curve == Curve::Stepped) v = std::round(v);
    return std::clamp(v, lo, hi);
}

namespace {
PluginProvider* gProvider = nullptr;
std::map<uint64_t, std::unique_ptr<DeviceInfo>> gPluginInfos;
}  // namespace

void setActivePluginProvider(PluginProvider* p) { gProvider = p; gPluginInfos.clear(); }

const DeviceInfo* deviceInfoFor(Chain chain, const project::DeviceSpec& d) {
    if (d.type != "plugin") return findDevice(chain, d.type);
    if (!gProvider || !gProvider->live(d.uid)) return nullptr;
    const auto params = gProvider->params(d.uid);
    auto& slot = gPluginInfos[d.uid];
    if (!slot || slot->params.data() != params.data() || slot->label != (d.pluginName.empty() ? "Plugin" : d.pluginName))
        slot = std::make_unique<DeviceInfo>(DeviceInfo{"plugin", d.pluginName.empty() ? "Plugin" : d.pluginName, "Plugins", chain, params});
    return slot.get();
}

std::string paramLabelFor(const project::DeviceSpec& d, std::string_view key) {
    if (d.type == "plugin" && gProvider && gProvider->live(d.uid)) {
        const auto params = gProvider->params(d.uid);
        for (size_t i = 0; i < params.size(); ++i) if (key == params[i].key) return gProvider->paramName(d.uid, i);
    }
    return paramLabel(key);
}

}  // namespace ddaw::app
