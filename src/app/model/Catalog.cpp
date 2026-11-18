#include "app/model/Catalog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>

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
        {"poly", "Poly Synth", "Synth", C::Instrument, sp(kInstPolyExt)},
        {"mono", "Mono Synth", "Synth", C::Instrument, sp(kInstMonoExt)},
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
        {"autofilt", "Auto Filter", "EQ and Filter", C::Effect, sp(kFxAutofiltExt)},
        {"comp", "Compressor", "Dynamics", C::Effect, sp(kFxComp)},
        {"opto", "Opto Compressor", "Dynamics", C::Effect, sp(kFxOpto)},
        {"mbcomp", "Multiband Comp", "Dynamics", C::Effect, sp(kFxMbcomp)},
        {"gate", "Gate", "Dynamics", C::Effect, sp(kFxGate)},
        {"duck", "Ducker", "Dynamics", C::Effect, sp(kFxDuck)},
        {"delay", "Delay", "Time", C::Effect, sp(kFxDelayExt)},
        {"pingpong", "Ping Pong", "Time", C::Effect, sp(kFxPingpongExt)},
        {"reverb", "Reverb", "Time", C::Effect, sp(kFxReverbExt)},
        {"plate", "Plate", "Time", C::Effect, sp(kFxPlateExt)},
        {"chorus", "Chorus", "Modulation", C::Effect, sp(kFxChorusExt)},
        {"phaser", "Phaser", "Modulation", C::Effect, sp(kFxPhaserExt)},
        {"vib", "Vibrato", "Modulation", C::Effect, sp(kFxVibExt)},
        {"trem", "Tremolo", "Modulation", C::Effect, sp(kFxTremExt)},
        {"autopan", "Auto Pan", "Modulation", C::Effect, sp(kFxAutopanExt)},
        {"dist", "Distortion", "Colour", C::Effect, sp(kFxDist)},
        {"crush", "Bit Crusher", "Colour", C::Effect, sp(kFxCrush)},
        {"cheby", "Chebyshev", "Colour", C::Effect, sp(kFxCheby)},
        {"widen", "Widener", "Utility", C::Effect, sp(kFxWiden)},
        {"shift", "Pitch Shift", "Pitch", C::Effect, sp(kFxShift)},
        {"chordres", "Chord Resonator", "Pitch", C::Effect, sp(kFxChordres)},
        {"autotune", "Auto-Tune", "Pitch", C::Effect, sp(kFxAutotuneKey)},

        {"scale", "Scale", "MIDI", C::MidiFx, sp(kMidiFxScale)},
        {"chord", "Chord", "MIDI", C::MidiFx, sp(kMidiFxChord)},
        {"arp", "Arpeggiator", "MIDI", C::MidiFx, sp(kMidiFxArp)},
        {"velo", "Velocity", "MIDI", C::MidiFx, sp(kMidiFxVelo)},
        {"rand", "Random", "MIDI", C::MidiFx, sp(kMidiFxRand)},
    };
}

}  // namespace

// ---- value names ----
// What a stepped parameter's numbers mean, per device type and key (the label for min + i is names[i]). The catalog hands the UI a
// copy of each device's parameter table with these attached; the engine and the project keep the number.
namespace {

#define DDAW_NAMES(id, ...) const char* const id[] = {__VA_ARGS__}
DDAW_NAMES(kOff, "Off", "On");
DDAW_NAMES(kSlope3, "12 dB", "24 dB", "48 dB");
DDAW_NAMES(kSlope2, "12 dB", "24 dB");
DDAW_NAMES(kLfoShapes, "Sine", "Triangle", "Saw", "Square");
DDAW_NAMES(kLfoShapes7, "Sine", "Triangle", "Saw up", "Saw down", "Square", "S&H", "Random");
DDAW_NAMES(kPolyWave, "Saw", "Square", "Triangle", "Sine", "Fat saw", "Fat sq", "Fat tri");
DDAW_NAMES(kMonoWave, "Saw", "Square", "Triangle", "Sine");
DDAW_NAMES(kSubWave, "Saw", "Pulse", "Triangle", "Sine");
DDAW_NAMES(kShaperSrc, "Sine", "Triangle", "Saw");
DDAW_NAMES(kShaperShape, "Soft", "Fold", "Chebyshev", "Diode", "Clip", "Curve");
DDAW_NAMES(kBank, "Classic", "Vowel", "Metal", "Organ");
DDAW_NAMES(kAlgo, "Serial", "2 stacks", "Fan-in", "Fan-out", "Additive", "AM/Ring");
DDAW_NAMES(kModalModel, "String", "Bar", "Marimba", "Membrane", "Plate", "Bell", "Tube");
DDAW_NAMES(kExciter, "Impulse", "Noise", "Mallet");
DDAW_NAMES(kNoiseMode, "Low-pass", "Band-pass", "High-pass");
DDAW_NAMES(kFilterType, "Low-pass", "High-pass", "Band-pass");
DDAW_NAMES(kDelayTime, "1/32", "1/16", "1/8", "1/8 dot", "1/4", "1/2");
DDAW_NAMES(kDuckRate, "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16");
DDAW_NAMES(kRateMode, "Sync", "Low", "High");
DDAW_NAMES(kRateSync, "8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/8T", "1/16");
DDAW_NAMES(kOptoMode, "Comp", "Limit");
DDAW_NAMES(kMbMode, "Compress", "Expand");
DDAW_NAMES(kGateKey, "Stereo", "Left", "Right", "Mid", "Side");
DDAW_NAMES(kDet, "Peak", "RMS");
DDAW_NAMES(kTuneMode, "Key", "Chromatic");
DDAW_NAMES(kKeyRoot, "Project", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B");
DDAW_NAMES(kKeyScale, "Project", "Major", "Minor", "Dorian", "Mixolydian", "Pent maj", "Pent min", "Harm min", "Blues");
DDAW_NAMES(kPitchClass, "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B");
DDAW_NAMES(kDdspModel, "Violin", "Flute", "Tenor sax", "Trumpet");
DDAW_NAMES(kArpRate, "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32");
DDAW_NAMES(kArpMode, "Up", "Down", "Up-down", "Random");
DDAW_NAMES(kChordSource, "Auto", "Notes", "Audio");
DDAW_NAMES(kSlope4, "12 dB", "24 dB", "36 dB", "48 dB");
DDAW_NAMES(kInterval,
    "-Oct", "-M7", "-m7", "-M6", "-m6", "-P5", "-TT", "-P4", "-M3", "-m3", "-M2", "-m2", "Unison", "m2", "M2", "m3", "M3", "P4", "TT", "P5", "m6", "M6", "m7", "M7", "Oct", "m9", "M9", "m10", "M10", "P11", "TT+8", "P12", "m13", "M13", "m14", "M14", "2 Oct", "m2 +2 Oct", "M2 +2 Oct", "m3 +2 Oct", "M3 +2 Oct", "P4 +2 Oct", "TT +2 Oct", "P5 +2 Oct", "m6 +2 Oct", "M6 +2 Oct", "m7 +2 Oct", "M7 +2 Oct", "3 Oct");
DDAW_NAMES(kArpPat, "Octave", "5ths", "Scale", "Select");
#undef DDAW_NAMES

struct NameSet { const char* type; const char* key; const char* const* names; size_t n; };
template <size_t N> constexpr NameSet ns(const char* type, const char* key, const char* const (&a)[N]) { return {type, key, a, N}; }

const std::vector<NameSet>& nameSets() {
    static const std::vector<NameSet> v = [] {
        std::vector<NameSet> r = {
            ns("poly", "wave", kPolyWave), ns("poly", "slope", kSlope3), ns("poly", "lfoShape", kLfoShapes),
            ns("mono", "wave", kMonoWave), ns("mono", "slope", kSlope3), ns("mono", "lfoShape", kLfoShapes),
            ns("subtractive", "wave1", kSubWave), ns("subtractive", "wave2", kSubWave), ns("subtractive", "slope", kSlope2),
            ns("waveshaper", "src", kShaperSrc), ns("waveshaper", "shape", kShaperShape),
            ns("wavetable", "bank", kBank), ns("fmop", "algo", kAlgo),
            ns("modal", "model", kModalModel), ns("modal", "exciter", kExciter),
            ns("perc", "noiseMode", kNoiseMode), ns("perc", "gate", kOff),
            ns("ksampler", "root", kPitchClass),
            ns("ddsp", "model", kDdspModel), ns("ddsp", "reverb", kOff), ns("follow", "wave", kMonoWave),
            ns("filter", "ftype", kFilterType), ns("filter", "slope", kSlope3),
            ns("delay", "time", kDelayTime), ns("pingpong", "time", kDelayTime), ns("duck", "rate", kDuckRate),
            ns("opto", "mode", kOptoMode), ns("mbcomp", "mode", kMbMode),
            ns("gate", "key", kGateKey), ns("gate", "det", kDet), ns("gate", "listen", kOff), ns("gate", "inverse", kOff),
            ns("autotune", "mode", kTuneMode), ns("autotune", "root", kKeyRoot), ns("autotune", "scale", kKeyScale),
            ns("chordres", "source", kChordSource), ns("chordres", "tonic", kOff), ns("chordres", "steep", kSlope4),
            ns("arp", "rate", kArpRate), ns("arp", "mode", kArpMode), ns("arp", "pat", kArpPat),
        };
        for (const char* k : {"i1", "i2", "i3", "i4", "i5", "i6"}) r.push_back(ns("chordres", k, kInterval));
        for (const char* t : {"chorus", "phaser", "autofilt", "trem", "autopan", "vib"}) {
            r.push_back(ns(t, "shape", kLfoShapes7));
            r.push_back(ns(t, "rateMode", kRateMode));
            r.push_back(ns(t, "rateSync", kRateSync));
        }
        return r;
    }();
    return v;
}

// A copy of a device's parameter table with its value names attached (kept for the life of the program).
std::span<const ParamSpec> withNames(std::string_view type, std::span<const ParamSpec> params) {
    bool any = false;
    for (const auto& n : nameSets()) if (type == n.type) any = true;
    if (!any) return params;
    static std::deque<std::vector<ParamSpec>> store;
    store.emplace_back(params.begin(), params.end());
    for (auto& p : store.back())
        for (const auto& n : nameSets())
            if (type == n.type && std::string_view(p.key) == n.key) { p.names = n.names; p.nNames = uint8_t(n.n); }
    return store.back();
}

std::vector<DeviceInfo> buildNamed() {
    auto c = build();
    for (auto& d : c) d.params = withNames(d.type, d.params);
    return c;
}

}  // namespace

const std::vector<DeviceInfo>& deviceCatalog() {
    static const std::vector<DeviceInfo> c = buildNamed();
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
    // a stepped parameter with value names (a wave, a filter type, a tempo division): the word, not the number
    if (s.names && s.nNames) {
        const int i = int(std::lround(v)) - int(std::lround(s.min));
        if (i >= 0 && i < int(s.nNames)) return s.names[i];
    }
    char buf[32];
    // attack, release and hold times are stored in seconds and shown in milliseconds (a real parameter has a positive minimum: a plugin's
    // normalised 0..1 value that happens to be called "attack" is left alone)
    if (s.key && s.min > 0.0f && s.max <= 5.0f && s.curve != Curve::Stepped) {
        std::string k = s.key;
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const auto ends = [&](const char* suffix) { const std::string x = suffix; return k.size() >= x.size() && k.compare(k.size() - x.size(), x.size(), x) == 0; };
        if (ends("attack") || ends("release") || k == "hold") {
            const double ms = v * 1000.0;
            std::snprintf(buf, sizeof buf, ms < 10.0 ? "%.1f ms" : "%.0f ms", ms);
            return buf;
        }
    }
    // depth is shown as 0-100 whatever its range (a vibrato's 0..0.6 included); a phase is shown in degrees
    if (s.key && std::string(s.key) == "depth" && s.max > s.min) {
        std::snprintf(buf, sizeof buf, "%d", int(std::lround((v - s.min) / (double(s.max) - s.min) * 100.0)));
        return buf;
    }
    if (s.key && s.max == 360.0f && s.min == 0.0f && std::string(s.key).find("hase") != std::string::npos) {
        std::snprintf(buf, sizeof buf, "%d\xC2\xB0", int(std::lround(v)));
        return buf;
    }
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
