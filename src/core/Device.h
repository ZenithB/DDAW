#pragma once
// Device contract. Binding: docs/ARCH.md sections 3 and 4. Change the contract
// first, then this file.
#include <cstdint>

#include "core/Sample.h"
#include <memory>
#include <span>
#include <string_view>

namespace ddaw {

enum class Curve { Linear, Exponential, Stepped };

struct ParamSpec {
    uint16_t    index;        // numeric address used on the audio thread
    const char* key;          // stable string key (save format, automation)
    float       min, max, def;
    Curve       curve;
    float       smoothingMs;  // control-rate smoothing; 0 for stepped params
    bool        audioRate;    // true: accepts a per-sample modulation buffer
};

struct ProcessContext {
    double  sampleRate;
    double  positionTicks;    // at the first frame of this block
    double  bpm;
    bool    playing;
    bool    looping;
    double  loopStartTicks;
    double  loopEndTicks;
    uint8_t tsTop, tsBottom;
    bool    arrangement = false;  // arrangement mode (else session)
    bool    offline = false;      // an offline render: devices with background threads may wait for them
    bool    warmup = false;       // the builder is touching the graph's memory before it goes live: devices that share state
                                  // with the running graph (hosted plugins) must not process
};

struct ModInputs {
    // Indexed by A-rate ordinal (position among ParamSpecs with audioRate set,
    // in ParamSpec order). nullptr when unmodulated this block.
    std::span<const float* const> audioRate;
    // Sidechain (dynamics devices): another track's audio for the detector, n frames, mono sources on both pointers. Null when the
    // device has no sidechain source this block: it detects on its own input as always. The processed signal is never the key.
    const float* keyL = nullptr;
    const float* keyR = nullptr;
};

struct PerformanceFrame {
    float f0Hz;        // 0 when unvoiced
    float confidence;  // 0..1
    float loudnessDb;  // matched to Magenta features
    float envelope;    // follower output, 0..1
};

class EffectDevice {
public:
    virtual ~EffectDevice() = default;
    virtual std::span<const ParamSpec> params() const = 0;
    virtual void prepare(double sampleRate, int maxBlock) = 0;  // may allocate
    virtual void setParam(uint16_t index, float value) = 0;     // block boundary only
    virtual void process(float* l, float* r, int numFrames,
                         const ProcessContext&, const ModInputs&) = 0;
    virtual void reset() = 0;
    virtual int   latencySamples() const { return 0; }
    virtual float gainReductionDb() const { return 0.0f; }
    // Dynamics devices show how hard they are working: reductionBands() meters (1, or 3 for the multiband compressor), each
    // reductionDb(band) <= 0 dB. The graph publishes them to the meter bank once per block when hasReductionMeter().
    virtual bool  hasReductionMeter() const noexcept { return false; }
    virtual int   reductionBands() const noexcept { return 1; }
    virtual float reductionDb(int /*band*/) const noexcept { return gainReductionDb(); }
    // Sidechain trigger (duck): a source note fired. Called on the audio thread at a block boundary.
    virtual void  trigger() noexcept {}
    // The project routed a source track to this device (duck's `srcTrack`): switch to sidechain mode.
    virtual void  setSidechain(bool) noexcept {}
    // True for dynamics processors that can run their detector on a key signal (ModInputs::keyL/keyR).
    virtual bool  keyable() const noexcept { return false; }
    // Control path (builder thread, before prepare): the project's key - root pitch class 0..11 (9 = A) and scale index (dsp/Scales.h) -
    // for devices that follow it (autotune). Called on every effect; most ignore it.
    virtual void  setProjectKey(int /*root*/, int /*scale*/) {}
    // Control path (builder thread, before prepare): the device's project identity - its uid and, for a hosted plugin, which
    // plugin it is (DeviceSpec::plugin). Devices that need neither ignore it.
    virtual void  bind(uint64_t /*deviceUid*/, std::string_view /*pluginId*/) {}
};

class InstrumentDevice {
public:
    virtual ~InstrumentDevice() = default;
    virtual std::span<const ParamSpec> params() const = 0;
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    virtual void setParam(uint16_t index, float value) = 0;
    virtual void noteOn(uint8_t pitch, float velocity, uint32_t noteId) = 0;
    virtual void noteOff(uint32_t noteId) = 0;
    // Per-note expression for a sounding note (MPE): dimension 0 slide / Y (0..1), 1 pressure (0..1), 2 pitch bend in
    // SEMITONES (signed, already scaled by the controller's bend range). A value replaces the previous one; 0 is "neutral".
    virtual void noteExpression(uint32_t, int, float) {}
    virtual void performance(const PerformanceFrame&) {}  // note-less control
    // Control path (builder thread, before the graph is published): inject a decoded sample. Slot 0 is
    // the instrument's main sample; drum pads use slots 0-7. Non-sampled instruments ignore it.
    virtual void setSample(uint32_t /*slot*/, SamplePtr /*buf*/) {}
    // Control path (builder thread, before prepare): the device's project identity, as for effects.
    virtual void bind(uint64_t /*deviceUid*/, std::string_view /*pluginId*/) {}
    // ADDITIVE: mix into l/r.
    virtual void process(float* l, float* r, int numFrames,
                         const ProcessContext&, const ModInputs&) = 0;
    virtual void reset() = 0;
    virtual int latencySamples() const { return 0; }
};

}  // namespace ddaw
