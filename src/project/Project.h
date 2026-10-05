#pragma once
// In-memory project model. Shaped after synthyy's ProjectJSON so imported projects map one to one;
// the native DDAW format (A4) will extend it. Audio clips, follow actions and sample references are
// not modelled yet (A4).
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ddaw::project {

using Uid = uint64_t;  // document-scoped stable object id (0 = not yet assigned)

// One point of a note's expression curve (MPE): `t` ticks after the note starts; `v` is semitones for bend, 0..1 for slide
// and pressure. Between points the value is interpolated linearly; before the first it is the first.
struct ExprPoint { double t = 0, v = 0; };

struct Note {
    Uid    uid = 0;
    int    pitch = 60;
    double startTicks = 0, durTicks = 0;
    double velocity = 1.0;
    double probability = 1.0;
    std::vector<ExprPoint> bend, slide, pressure;   // per-note expression curves, sorted by t (empty: none)
};

struct AutoPoint { double t = 0, v = 0; };
// Keys are "dest|fxId|pkey" (clip envelopes and track lanes); values are normalised 0..1.
using AutoMap = std::map<std::string, std::vector<AutoPoint>>;

// An audio clip (audio tracks): a sample region played from a launch or arrangement position.
struct AudioClipData {
    std::string sampleId, sampleName;
    double gainDb = 0, pitch = 0, rev = 0, loop = 0, fadeIn = 0, fadeOut = 0;
    std::optional<double> offset, dur, cents, xfade;
};

struct Clip {
    double len = 384;  // ticks
    std::optional<AudioClipData> audio;
    std::vector<Note> notes;
    AutoMap env;       // clip envelopes (session: looped over the clip; arrangement: under the playhead)
};

struct ArrClip {
    Clip clip;
    std::string trackId;
    double start = 0;  // ticks
};

// An instrument, an insert effect, a MIDI effect, a master effect.
struct DeviceSpec {
    Uid uid = 0;
    std::string id;    // addressed by "fxId" in modulation keys; empty falls back to "fx<i>"
    std::string type;
    bool on = true;
    std::map<std::string, double> params;
    std::optional<double> outDb;        // device output gain (dB)
    std::string srcTrack;               // duck: the sidechain source track id
    std::optional<double> srcPitch;     // duck: restrict the trigger to this pitch
    // Instruments only: the sample bank ids (sampler, ksampler, granular; drum pad overrides by slot).
    std::string sampleId, sampleName;
    std::map<std::string, std::string> padSamples, padNames;
};

struct ModTarget { std::string dest, fxId, pkey; };

struct LfoSpec {
    std::string id;
    bool on = true;
    int shape = 0;
    bool sync = false;
    double rate = 5;   // sync division index
    double hz = 1;     // free rate
    double depth = 0.5, phase = 0;
    std::string dest, fxId, pkey;       // legacy single target, folded into `targets`
    std::vector<ModTarget> targets;
};

struct MacroSpec {
    std::string name;
    double value = 0;
    std::vector<ModTarget> targets;
};

// A performance route (B2): one tracked quantity of the live input, mapped over [srcMin, srcMax] to 0..1
// (srcMin == srcMax == 0: the source's default range), driving parameters like an automation lane does.
// source: "f0" | "loudness" | "confidence" | "envelope".
struct PerfSpec {
    std::string source = "f0";
    double srcMin = 0, srcMax = 0;
    bool on = true;
    bool record = false;     // write this route's curve into the targets' automation lanes while recording
    std::vector<ModTarget> targets;
};

// An audio-rate modulation route (B4): a per-sample source drives an A-rate parameter (ParamSpec::audioRate) of a
// device on this track. source "osc": a free-running oscillator (shape 0 sine, 1 triangle, 2 saw, 3 square; `hz`).
// source "track": the audio of another (non-bus) track after its effects and before its fader and mute, raw or
// through an envelope follower. The
// modulation added to the parameter is source * depth * (max - min) / 2, in the parameter's own units.
struct ARateSpec {
    std::string id;
    bool on = true;
    std::string source = "osc";
    int shape = 0;
    double hz = 220;
    std::string srcTrack;               // source "track": the id of that track
    bool follow = false;
    double attackMs = 5, releaseMs = 80;
    double depth = 0.5;                 // -1..1
    ModTarget target;                   // dest "inst" or an effect id, pkey an A-rate parameter
};

// A morph map (B5): the parameters `targets` are driven from a point (x, y) on a 2D field. Each anchor holds a
// position and one value per target (0..1 of the parameter's range; log for exponential parameters); the stick
// position blends them (dsp/MorphMap.h), each target shaped by its own response exponent.
struct MorphAnchor {
    std::string name;
    double x = 0.5, y = 0.5;
    std::vector<double> values;          // one per target
};
struct MorphSpec {
    std::string name;
    bool on = true;
    double x = 0.5, y = 0.5;             // the stick
    std::string method = "idw";          // "idw" | "rbf"
    double power = 2.0;                  // idw
    double width = 0.35;                 // rbf
    std::vector<ModTarget> targets;
    std::vector<double> curves;          // per target response exponent (a missing entry is 1)
    std::vector<MorphAnchor> anchors;
};

enum class TrackKind { Synth, Drum, Audio, Bus };
enum class SendBus { None, A, B, F };

struct Track {
    Uid uid = 0;
    std::string id, name;
    TrackKind kind = TrackKind::Synth;
    DeviceSpec inst;
    std::vector<DeviceSpec> fx;
    std::vector<DeviceSpec> midifx;
    double gainDb = 0, pan = 0, sendA = 0, sendB = 0;
    bool mute = false, solo = false;
    std::string output;                    // bus routing: "" / "master" or a bus track id
    std::map<std::string, double> sends;   // bus track id -> level
    SendBus send = SendBus::None;          // built-in send-bus marker (a bus track acting as A / B / feedback)
    std::vector<LfoSpec> lfos;
    std::vector<MacroSpec> macros;
    std::vector<PerfSpec> perf;            // performance routes (B2)
    std::vector<ARateSpec> arate;          // audio-rate modulation routes (B4)
    std::vector<MorphSpec> morph;          // morph maps (B5)
    AutoMap autoLanes;                     // arrangement automation lanes (absolute ticks)
};

struct Meta {
    std::string title;
    double bpm = 120, swing = 0, humanize = 0, masterGainDb = 0;
    std::string swingSubdivision = "16n";
    double root = 9;
    std::string scale = "minor";
    double launchQ = 1;                    // launch quantisation in bars (0 = immediate)
    bool loopOn = false;
    double loopStart = 0, loopEnd = 0;
    int tsTop = 4, tsBottom = 4;
};

struct Return {
    std::string id, name, fxType;
    std::map<std::string, double> params;
    double gainDb = 0;
};

// A controller binding (B5): one input of a gamepad or MIDI controller drives one live control.
// source: "pad:lx" "pad:ly" "pad:rx" "pad:ry" (sticks, 0..1 with 0.5 at rest), "pad:lt" "pad:rt" (triggers), "pad:a" ... (buttons),
//         "midi:cc<N>" (any channel), "midi:bend", "midi:pressure".
// target: "morph:<trackId>:<index>:x" | ":y" (a morph map's stick), "macro:<trackId>:<index>" (a macro's value).
// The input (0..1, flipped when `invert`) is mapped linearly onto [min, max] of the target's own 0..1 range.
struct ControlBinding {
    std::string source, target;
    double min = 0, max = 1;
    bool invert = false;
};

struct Project {
    Meta meta;
    std::vector<Track> tracks;
    std::vector<std::string> scenes;
    std::map<std::string, Clip> clips;     // key "trackId|sceneId"
    std::map<std::string, ArrClip> arr;    // arrangement clips
    std::vector<Return> returns;
    std::vector<DeviceSpec> masterFx;
    AutoMap masterAuto;
    std::vector<ControlBinding> bindings;  // controller bindings (B5)
};

struct Scope {
    std::string kind = "scene";  // scene | arr | loop
    std::string sceneId;
};

struct Fixture {
    std::string name;
    Scope scope;
    Project project;
};

}  // namespace ddaw::project
