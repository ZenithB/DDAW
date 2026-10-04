#pragma once
// The `ddsp` instrument (B3): Magenta's pretrained solo-instrument models (violin, flute, tenor sax, trumpet)
// as a playable device. It is played by the performance tracker (a voice or instrument in, the model's sound
// out - timbre transfer) or by notes (clips, a keyboard). Everything heavy runs on the device's own thread
// (PLAN 1: never on the audio thread): the network, the synthesis at the model rate (harmonic + noise +
// reverb, ddsp/Synth16k.h) and the conversion to the host rate. The audio thread only sends feature frames
// through a wait-free FIFO and plays finished samples from a ring. A late block never stalls it: the output
// fades; an offline render waits instead, so exports are deterministic.
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/Constants.h"
#include "core/Device.h"
#include "core/SpscFifo.h"
#include "core/SpscFloatRing.h"
#include "ddsp/Decoder.h"
#include "ddsp/DdspParams.h"
#include "ddsp/Synth16k.h"
#include "dsp/Smoother.h"
#include "dsp/StreamResampler.h"

namespace ddaw::ddsp {

// Where the exported model files (violin.ddspw, ...) live. Set once at startup; also read from the
// DDAW_MODELS environment variable and the per-user models folder when unset.
void setModelsDirectory(const std::string& dir);
std::string modelsDirectory();
const char* modelName(int index);          // "violin", "flute", "tenor_saxophone", "trumpet"
bool modelAvailable(int index);            // the .ddspw exists
// Register the `ddsp` device with the instrument registry. Call once at startup.
void registerDevices();

struct DdspStats {
    uint64_t framesDecoded = 0;   // control frames the inference thread produced
    uint64_t underruns = 0;       // audio blocks that ran out of synthesised samples (live only)
    bool modelReady = false;
    int model = -1;               // loaded model index
};

class DdspInstrument final : public InstrumentDevice {
public:
    DdspInstrument();
    ~DdspInstrument() override;
    std::span<const ParamSpec> params() const override { return schema::kInstDdsp; }
    void prepare(double sampleRate, int maxBlock) override;
    void setParam(uint16_t index, float value) override;
    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override;
    void noteOff(uint32_t noteId) override;
    void performance(const PerformanceFrame& f) override;
    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override;
    void reset() override;
    int latencySamples() const override { return latency_; }
    DdspStats stats() const;

private:
    enum P : uint16_t { Model, Transpose, Level, Reverb, Noise, Attack, Release, Vibrato };
    struct Feat { float ld, f0s, f0Hz; uint32_t gen; };
    struct Blk { uint32_t gen, count; };                    // a run of finished host-rate samples in audioQ_

    void inferenceLoop();
    bool loadModel(int index);                              // worker: decoder and reverb for a model
    void pushFeature(float ldDb, float f0Hz) noexcept;     // one 250 Hz frame, from the tracker or from notes
    void wake() noexcept;
    void noteFrame() noexcept;                              // the note-mode feature generator, once per hop

    // ---- the worker thread: decoder, synthesis (harmonic + noise + reverb), conversion to the host rate ----
    SpscFifo<Feat, 256> featQ_;                             // audio thread -> worker
    SpscFloatRing audioQ_;                                  // worker -> audio thread: host-rate samples ...
    SpscFifo<Blk, 512> blkQ_;                               // ... and, per run, the generation they belong to
    std::atomic<uint32_t> gen_{0};                          // bumped by the audio thread when a sound starts or is reset
    std::atomic<uint64_t> progress_{~uint64_t(0)};          // (generation << 32) | feature frames the worker has turned into audio
    std::atomic<int> wantModel_{0}, loadedModel_{-1};
    std::atomic<bool> stop_{false}, ready_{false}, reverbOn_{true};
    std::atomic<float> noiseGain_{1.0f};
    std::atomic<uint64_t> decoded_{0}, underruns_{0};
    std::thread thread_;
    // worker-owned
    std::unique_ptr<Decoder> decoder_;
    Synth16k synth_;
    std::unique_ptr<ReverbConv> reverb_;
    dsp::StreamResampler resampler_;
    uint32_t rng_ = 0x9E3779B9u;
    std::vector<float> outBuf_;
    float noise64_[kHopSamples] = {}, blk64_[kHopSamples] = {}, raw_[kDecoderOut] = {};

    // ---- the audio thread: features in, finished samples out ----
    double sr_ = 48000.0;
    double hopHost_ = 192.0, hopAccum_ = 0.0;
    int latency_ = 0;
    int silenceLeft_ = 0;                                   // zeros to play before the stream starts: the reported latency
    enum class State { Idle, Active } state_ = State::Idle;
    int silentRun_ = 0;
    uint32_t pushed_ = 0;                                   // feature frames sent this generation
    uint32_t curRemain_ = 0;                                // samples left in the run being played
    float mix_[kMaxBlock] = {};
    bool nextSample(float& o) noexcept;
    float lastOut_ = 0.0f;
    dsp::Smoother level_;
    int transpose_ = 0;
    float attackMs_ = 40.0f, releaseMs_ = 250.0f, vibratoCents_ = 12.0f;
    int64_t perfAge_ = int64_t(1) << 40;
    // note mode
    bool noteHeld_ = false;
    float noteHz_ = 0, curHz_ = 0, ldCur_ = -80.0f, ldTarget_ = -80.0f, vibPhase_ = 0;
};

}  // namespace ddaw::ddsp
