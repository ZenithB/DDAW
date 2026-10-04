#pragma once
// Command lane messages (ARCH 9). Trivially copyable, <= 32 bytes, numeric
// addresses only: no string ever crosses into the audio thread.
#include <cstdint>

#include "core/Device.h"
#include "core/SpscFifo.h"

namespace ddaw {

constexpr uint16_t kTargetMaster = 0xFFFF;  // master strip instead of a track
constexpr uint8_t  kSlotInst     = 0;       // instrument
constexpr uint8_t  kSlotFx0      = 1;       // fx device i is kSlotFx0 + i
constexpr uint8_t  kSlotMixer    = 0xFF;    // gain, pan, sends
constexpr uint8_t  kSlotMacro    = 0xFE;    // a macro's value (param = macro index)
constexpr uint8_t  kSlotLfo      = 0xFD;    // an LFO field (param = lfoIndex * 4 + field)
constexpr uint16_t kLfoDepth = 0, kLfoHz = 1, kLfoPhase = 2;
constexpr uint16_t kParamOut     = 0xFFFF;  // device output gain (dB), not a ParamSpec

// Mixer-slot parameter indices (slot kSlotMixer).
constexpr uint16_t kMixGain = 0;  // dB; with target kTargetMaster it is the master gain
constexpr uint16_t kMixPan  = 1;  // -1..1
constexpr uint16_t kMixSendA = 2;       // 0..1, post-fader send to the A bus / first return
constexpr uint16_t kMixSendB = 3;       // 0..1, B bus / second return
constexpr uint16_t kMixBusSend0 = 16;   // + index of the track's per-bus send list
constexpr uint16_t kMaxBusSends = 16;
constexpr uint16_t kMixFeedback = kMixBusSend0 + kMaxBusSends;  // feedback bus self-send level

struct ParamAddr { uint16_t target; uint8_t slot; uint16_t param; };

enum class CmdType : uint8_t {
    SetParam, NoteOn, NoteOff, NoteExpression, Performance,
    ClipLaunch, ClipStop, TransportPlay, TransportStop, SetTempo,
    MetronomeOn, MetronomeOff, TestTone,
    Tracking, MonitorInput, TrackerConfig, LiveTrack
};

// Graph-addressed commands (everything but transport/tempo/metronome) carry the
// epoch of the graph their addresses were resolved against. The audio thread
// drops a graph-addressed command whose epoch is not the live graph's (ARCH 7).
struct SetParamP    { ParamAddr addr; float value; };
struct NoteOnP      { uint16_t track; uint8_t pitch; float vel; uint32_t noteId; };
struct NoteOffP     { uint16_t track; uint32_t noteId; };
struct NoteExprP    { uint16_t track; uint32_t noteId; int8_t dim; float value; };
struct PerformanceP { uint16_t track; PerformanceFrame frame; };
struct ClipLaunchP  { uint16_t track; uint16_t clip; };
struct ClipStopP    { uint16_t track; };
struct TransportPlayP { uint8_t mode; double fromTicks; };  // mode: 0 Session, 1 Arrangement
struct SetTempoP    { double bpm; };
struct TrackingP    { int16_t track; };                           // the track whose instrument receives performance frames (< 0: none)
struct LiveTrackP   { int16_t track; };                           // the track that live (MIDI / keyboard) notes play (< 0: none)
struct MonitorInputP { uint16_t track; uint8_t on; };            // route the live input through this track's effect chain
struct TrackerConfigP { float minHz, maxHz, threshold, voicedConfidence; };   // engine-level
struct TestToneP    { float freqHz; float levelDb; uint8_t on; };  // diagnostic, master bus, engine-level

struct Cmd {
    CmdType  type{};
    uint32_t epoch = 0;
    union {
        SetParamP     setParam;
        NoteOnP       noteOn;
        NoteOffP      noteOff;
        NoteExprP     noteExpr;
        PerformanceP  performance;
        ClipLaunchP   clipLaunch;
        ClipStopP     clipStop;
        TransportPlayP transportPlay;
        SetTempoP     setTempo;
        TestToneP     testTone;
        TrackingP     tracking;
        LiveTrackP    liveTrack;
        MonitorInputP monitorInput;
        TrackerConfigP trackerConfig;
    };
    Cmd() : setParam{} {}
};

static_assert(sizeof(Cmd) <= 32, "Cmd must stay within 32 bytes");
static_assert(std::is_trivially_copyable_v<Cmd>);

constexpr size_t kCommandFifoCapacity = 4096;
using CommandFifo = SpscFifo<Cmd, kCommandFifoCapacity>;

constexpr bool isGraphAddressed(CmdType t) {
    return !(t == CmdType::TransportPlay || t == CmdType::TransportStop || t == CmdType::SetTempo ||
             t == CmdType::MetronomeOn || t == CmdType::MetronomeOff || t == CmdType::TestTone || t == CmdType::TrackerConfig);
}

}  // namespace ddaw
