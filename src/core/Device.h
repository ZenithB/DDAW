#pragma once
// Device contract. Binding: docs/ARCH.md sections 3 and 4. Change the contract
// first, then this file.
#include <cstdint>

#include "core/Sample.h"
#include <memory>
#include <span>

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
};

struct ModInputs {
    // Indexed by A-rate ordinal (position among ParamSpecs with audioRate set,
    // in ParamSpec order). nullptr when unmodulated this block.
    std::span<const float* const> audioRate;
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
    // Sidechain trigger (duck): a source note fired. Called on the audio thread at a block boundary.
    virtual void  trigger() noexcept {}
    // The project routed a source track to this device (duck's `srcTrack`): switch to sidechain mode.
    virtual void  setSidechain(bool) noexcept {}
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
    // ADDITIVE: mix into l/r.
    virtual void process(float* l, float* r, int numFrames,
                         const ProcessContext&, const ModInputs&) = 0;
    virtual void reset() = 0;
    virtual int latencySamples() const { return 0; }
};

}  // namespace ddaw
