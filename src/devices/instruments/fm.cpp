// `fm` instrument: port of sf-dsp/src/inst/fm.rs (Tone.PolySynth(Tone.FMSynth, 12)).
// Each voice is a sine carrier frequency-modulated by a sine modulator at freq*harm; the peak
// deviation is freq*modIdx scaled by a fixed modulation envelope (0.005/0.4/0.6/0.4). The amp
// envelope is the schema ADSR, velocity scales both envelopes, and both internal synths carry Tone's
// fixed -10 dB voice volume. Fixed pool of 12 voices; a noteOn on a still-held pitch reuses that
// voice, else a free voice, else the oldest is stolen. noteIds are tracked per voice.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Adsr.h"
#include "dsp/Math.h"
#include "dsp/PolyBlepOsc.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

// Index in schema::kInstFm.
enum P : uint16_t { Harm, ModIdx, Attack, Decay, Sustain, Release };

constexpr int kVoices = 12;
constexpr float kSynthVolumeGain = 0.31622777f;  // 10^(-10/20): Tone.ModulationSynth voice volume
constexpr float kModAttack = 0.005f, kModDecay = 0.4f, kModSustain = 0.6f, kModRelease = 0.4f;

struct Voice {
    // The carrier keeps a raw phase accumulator: through-zero FM needs a signed instantaneous
    // frequency, which PolyBlepOsc clamps away.
    PolyBlepOsc modulator{Wave::Sine};
    float carrierPhase = 0.0f;
    Adsr ampEnv, modEnv;
    float freq = 440.0f, vel = 1.0f;
    uint8_t pitch = 69;
    bool held = false;
    uint64_t serial = 0;
    uint32_t id = 0;

    Voice() { modEnv.setAdsr(kModAttack, kModDecay, kModSustain, kModRelease); }

    void prepare(float sr) {
        modulator.prepare(sr);
        ampEnv.prepare(sr);
        modEnv.prepare(sr);
        modEnv.setAdsr(kModAttack, kModDecay, kModSustain, kModRelease);
        carrierPhase = 0.0f;
        held = false;
    }

    bool active() const { return ampEnv.isActive(); }

    void release() {
        held = false;
        ampEnv.noteOff();
        modEnv.noteOff();
    }

    float render(float harm, float modIdx, float invSr) {
        modulator.setFreq(freq * harm);
        // The modulator is a full Tone.Synth: osc * env * velocity * volume(-10 dB).
        const float m = modulator.next() * modEnv.next() * vel * kSynthVolumeGain;
        const float step = std::clamp(freq * (1.0f + modIdx * m) * invSr, -0.5f, 0.5f);
        const float s = std::sin(carrierPhase * static_cast<float>(2.0 * std::numbers::pi));
        carrierPhase += step;
        carrierPhase -= std::floor(carrierPhase);
        return s * ampEnv.next() * vel * kSynthVolumeGain;
    }
};

class FmInst final : public InstrumentDevice {
public:
    FmInst() {
        harm_.snap(3.0f);
        modIdx_.snap(10.0f);
        applyAdsr();
    }

    std::span<const ParamSpec> params() const override { return schema::kInstFm; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        harm_.prepare(sr_, 15.0f);
        modIdx_.prepare(sr_, 15.0f);
        for (auto& v : voices_) v.prepare(sr_);
        applyAdsr();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Harm: harm_.setTarget(std::clamp(v, 0.25f, 8.0f)); break;
            case ModIdx: modIdx_.setTarget(std::clamp(v, 0.5f, 40.0f)); break;
            // envelope edits apply to all voices, live ones included (synth.set in makeFm)
            case Attack: attack_ = std::clamp(v, 0.001f, 2.0f); applyAdsr(); break;
            case Decay: decay_ = std::clamp(v, 0.01f, 2.0f); applyAdsr(); break;
            case Sustain: sustain_ = std::clamp(v, 0.0f, 1.0f); applyAdsr(); break;
            case Release: release_ = std::clamp(v, 0.01f, 4.0f); applyAdsr(); break;
            default: break;
        }
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        const uint64_t serial = nextSerial_++;
        int same = -1, freeV = -1, oldest = 0;
        uint64_t oldestSerial = UINT64_MAX;
        for (int i = 0; i < kVoices; ++i) {
            const Voice& v = voices_[static_cast<size_t>(i)];
            if (v.held && v.pitch == pitch) { same = i; break; }
            if (!v.active() && freeV < 0) freeV = i;
            if (v.serial < oldestSerial) { oldestSerial = v.serial; oldest = i; }
        }
        const int idx = same >= 0 ? same : (freeV >= 0 ? freeV : oldest);
        Voice& v = voices_[static_cast<size_t>(idx)];
        v.pitch = pitch;
        v.freq = midiHz(static_cast<float>(pitch));
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.serial = serial;
        v.id = noteId;
        v.held = true;
        v.ampEnv.noteOn();
        v.modEnv.noteOn();
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_)
            if (v.held && v.id == noteId) v.release();
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        const float invSr = 1.0f / sr_;
        for (int i = 0; i < n; ++i) {
            const float harm = harm_.next();
            const float modIdx = modIdx_.next();
            float s = 0.0f;
            for (auto& v : voices_) {
                if (!v.active()) continue;
                s += v.render(harm, modIdx, invSr);
            }
            l[i] += s;
            r[i] += s;
        }
    }

    void reset() override {
        for (auto& v : voices_) {
            v.ampEnv.reset();
            v.modEnv.reset();
            v.held = false;
            v.carrierPhase = 0.0f;
            v.modulator.resetPhase(0.0f);
            v.serial = 0;
            v.id = 0;
        }
        nextSerial_ = 1;
        harm_.snap(harm_.target());
        modIdx_.snap(modIdx_.target());
    }

private:
    void applyAdsr() {
        for (auto& v : voices_) v.ampEnv.setAdsr(attack_, decay_, sustain_, release_);
    }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    Smoother harm_, modIdx_;
    float attack_ = 0.005f, decay_ = 0.3f, sustain_ = 0.4f, release_ = 0.6f;
    uint64_t nextSerial_ = 1;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_fm() { return std::make_unique<FmInst>(); }

}  // namespace ddaw::devices
