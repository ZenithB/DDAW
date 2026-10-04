// `mono` instrument: port of sf-dsp/src/inst/mono.rs (Tone.MonoSynth topology).
// One PolyBLEP oscillator -> lowpass (biquad-cascade rolloff 12/24/48 dB via `slope`, Q from `res`
// in dB like a Web Audio lowpass) -> amp ADSR. The cutoff is driven by a fixed filter envelope
// (A 0.004, D 0.18, S 0.4, R 0.3) mapped freq = base + (base*2^envAmt - base) * e^2, plus the
// built-in instrument LFO summed into the cutoff as +-cutoff*amt. `glide` is Tone's portamento: an
// exponential pitch ramp when a note is retriggered while the previous one is still sounding.
#include <array>
#include <cmath>
#include <memory>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Adsr.h"
#include "dsp/Lfo.h"
#include "dsp/Math.h"
#include "dsp/PolyBlepOsc.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

// Index in schema::kInstMono.
enum P : uint16_t { Wave_, Cutoff, Res, Slope, EnvAmt, Glide, Attack, Decay, Sustain, Release, LfoShape, LfoRate, LfoAmt };

constexpr std::array<int, 3> kSlopeStages = {1, 2, 4};  // Tone SLOPE_ROLLOFFS = [-12, -24, -48]
constexpr int kMaxStages = 4;
constexpr std::array<uint32_t, 4> kLfoShapeMap = {0, 1, 2, 4};  // sine, triangle, saw up, square

class MonoInst final : public InstrumentDevice {
public:
    MonoInst() {
        vel_.snap(1.0f);
        filtEnv_.setAdsr(0.004f, 0.18f, 0.4f, 0.3f);  // makeMono's fixed filter envelope
        syncAmpEnv();
    }

    std::span<const ParamSpec> params() const override { return schema::kInstMono; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        osc_.prepare(sr_);
        for (auto& f : filt_) f.prepare(sr_);
        ampEnv_.prepare(sr_);
        filtEnv_.prepare(sr_);
        lfo_.prepare(sr_);
        vel_.prepare(sr_, 2.0f);
        lastFilterHz_ = -1.0f;
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Wave_: osc_.setWave(waveFromIndex(static_cast<int>(v))); break;
            case Cutoff: cutoff_ = std::max(v, 1.0f); lastFilterHz_ = -1.0f; break;
            case Res: res_ = v; lastFilterHz_ = -1.0f; break;
            case Slope: {
                const int stages = kSlopeStages[static_cast<size_t>(std::clamp(static_cast<int>(v), 0, 2))];
                if (stages != stages_) { stages_ = stages; lastFilterHz_ = -1.0f; }
                break;
            }
            case EnvAmt: envAmt_ = v; break;
            case Glide: glideS_ = std::max(v, 0.0f); break;
            case Attack: attack_ = std::max(v, 0.0f); syncAmpEnv(); break;
            case Decay: decay_ = std::max(v, 0.0f); syncAmpEnv(); break;
            case Sustain: sustain_ = std::clamp(v, 0.0f, 1.0f); syncAmpEnv(); break;
            case Release: release_ = std::max(v, 0.0f); syncAmpEnv(); break;
            case LfoShape: lfo_.setShape(kLfoShapeMap[static_cast<size_t>(std::clamp(static_cast<int>(v), 0, 3))]); break;
            case LfoRate: lfo_.setFreq(v); break;
            case LfoAmt: lfoAmt_ = std::clamp(v, 0.0f, 1.0f); break;
            default: break;
        }
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        current_ = noteId;
        startNote(pitch, velocity);
    }

    // Monophonic: only the sounding note's own release counts. An older note's off, arriving after
    // a legato retrigger, is superseded (the Rust scheduler path overwrites the gate the same way).
    void noteOff(uint32_t noteId) override {
        if (noteId != current_) return;
        ampEnv_.noteOff();
        filtEnv_.noteOff();
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        if (!active_) return;
        const float base = cutoff_;
        const float span = base * (std::pow(2.0f, envAmt_) - 1.0f);
        for (int i = 0; i < n; ++i) {
            // portamento glide in log2-frequency (Tone's exponential ramp)
            if (glideStep_ != 0.0f) {
                freqLog2_ += glideStep_;
                if ((glideStep_ > 0.0f && freqLog2_ >= freqTargetLog2_) || (glideStep_ < 0.0f && freqLog2_ <= freqTargetLog2_)) {
                    freqLog2_ = freqTargetLog2_;
                    glideStep_ = 0.0f;
                }
            }
            osc_.setFreq(std::exp2(freqLog2_));

            // filter cutoff: envelope (exponent 2) + LFO offset (+-cutoff*amt)
            const float e = filtEnv_.next();
            const float lfoV = lfo_.next();
            float hz = base + span * e * e;
            if (lfoAmt_ > 0.001f) hz += lfoV * base * lfoAmt_;
            setFilterHz(std::max(hz, 10.0f));

            float s = osc_.next();
            for (int f = 0; f < stages_; ++f) s = filt_[static_cast<size_t>(f)].processSample(s);
            s *= ampEnv_.next() * vel_.next();
            l[i] += s;
            r[i] += s;

            if (!ampEnv_.isActive()) {
                active_ = false;
                for (int f = 0; f < stages_; ++f) filt_[static_cast<size_t>(f)].reset();
                return;
            }
        }
    }

    void reset() override {
        ampEnv_.reset();
        filtEnv_.reset();
        for (auto& f : filt_) f.reset();
        osc_.resetPhase(0.0f);
        active_ = false;
        glideStep_ = 0.0f;
        lastFilterHz_ = -1.0f;
        current_ = 0;
    }

private:
    void syncAmpEnv() { ampEnv_.setAdsr(attack_, decay_, sustain_, release_); }

    // Q from `res`: Web Audio lowpass Q is in dB (Tone.Filter passes it raw).
    float qLinear() const { return std::max(std::pow(10.0f, res_ * 0.05f), 0.05f); }

    void setFilterHz(float hz) {
        // quantised guard: only re-derive coefficients when the target moved
        if (std::abs(hz - lastFilterHz_) > lastFilterHz_ * 1e-3f + 0.01f) {
            lastFilterHz_ = hz;
            const float q = qLinear();
            const float c = std::clamp(hz, 10.0f, sr_ * 0.45f);
            for (int f = 0; f < stages_; ++f) filt_[static_cast<size_t>(f)].setCutoffQ(c, q);
        }
    }

    void startNote(uint8_t pitch, float velocity) {
        const float target = std::log2(midiHz(static_cast<float>(pitch)));
        // Tone.Monophonic portamento: glide when the previous note still sounds
        const bool audible = active_ && ampEnv_.value() > 0.05f;
        if (audible && glideS_ > 1e-4f) {
            freqTargetLog2_ = target;
            const float n = std::max(glideS_ * sr_, 1.0f);
            glideStep_ = (target - freqLog2_) / n;
        } else {
            freqLog2_ = freqTargetLog2_ = target;
            glideStep_ = 0.0f;
        }
        const float v = std::clamp(velocity, 0.0f, 1.0f);
        vel_.setTarget(v);
        if (!active_) vel_.snap(v);
        ampEnv_.noteOn();
        filtEnv_.noteOn();
        active_ = true;
    }

    float sr_ = 44100.0f;
    PolyBlepOsc osc_{Wave::Saw};
    std::array<Svf, kMaxStages> filt_{Svf(SvfMode::Lowpass), Svf(SvfMode::Lowpass), Svf(SvfMode::Lowpass), Svf(SvfMode::Lowpass)};
    int stages_ = kSlopeStages[1];  // schema default slope = 1 (24 dB)
    Adsr ampEnv_, filtEnv_;
    Lfo lfo_;

    float cutoff_ = 900.0f, res_ = 2.0f, envAmt_ = 2.5f, glideS_ = 0.02f, lfoAmt_ = 0.0f;
    float attack_ = 0.003f, decay_ = 0.2f, sustain_ = 0.45f, release_ = 0.25f;

    Smoother vel_;
    float freqLog2_ = 0.0f, freqTargetLog2_ = 0.0f, glideStep_ = 0.0f;
    bool active_ = false;
    float lastFilterHz_ = -1.0f;
    uint32_t current_ = 0;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_mono() { return std::make_unique<MonoInst>(); }

}  // namespace ddaw::devices
