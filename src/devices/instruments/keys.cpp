// `keys` instrument ("Dream Keys"): port of sf-dsp/src/inst/keys.rs (Tone.PolySynth of AMSynth voices,
// maxPolyphony 12, modulation sine). Per voice:
//   out = carrier_sine(f) * ampEnv * vel * g * a2g(mod_sine(harm*f) * modEnv * vel * g),  a2g(x) = (x+1)/2
// with g = 10^(-10/20): Tone's ModulationSynth builds both inner Synths with volume -10 dB. The
// modulation envelope is the AMSynth default (A 0.5, D 0, S 1, R 0.5), not schema-addressable.
// Fixed 12-voice pool, same-pitch reuse first, then a free voice, then the oldest is stolen.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Adsr.h"
#include "dsp/Math.h"
#include "dsp/NoteExpr.h"
#include "dsp/PolyBlepOsc.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

// Index in schema::kInstKeys.
enum P : uint16_t { Harm, Attack, Decay, Sustain, Release };

constexpr int kNumVoices = 12;
constexpr float kSynthVolumeGain = 0.316227766f;  // 10^(-10/20)
constexpr float kModAttack = 0.5f, kModDecay = 0.0f, kModSustain = 1.0f, kModRelease = 0.5f;

struct Voice {
    PolyBlepOsc carrier{Wave::Sine};
    PolyBlepOsc modulator{Wave::Sine};
    Adsr ampEnv, modEnv;
    uint8_t pitch = 0;
    float freq = 440.0f, vel = 0.0f;
    bool active = false;
    uint64_t age = 0;
    uint32_t noteId = 0;
    NoteExpr ex;

    void prepare(float sr) {
        carrier.prepare(sr);
        modulator.prepare(sr);
        ampEnv.prepare(sr);
        modEnv.prepare(sr);
        modEnv.setAdsr(kModAttack, kModDecay, kModSustain, kModRelease);
        kill();
    }
    void kill() {
        ampEnv.reset();
        modEnv.reset();
        carrier.resetPhase(0.0f);  // deterministic restart after reset()
        modulator.resetPhase(0.0f);
        active = false;
    }
    void start(uint8_t p, float v, uint64_t a, uint32_t id) {
        pitch = p;
        freq = midiHz(static_cast<float>(p));
        vel = std::clamp(v, 0.0f, 1.0f);
        carrier.setFreq(freq);
        // envelopes re-attack from their current level (Tone semantics), oscillators stay phase-continuous
        ampEnv.noteOn();
        modEnv.noteOn();
        active = true;
        age = a;
        noteId = id;
        ex.clear();
    }
    void release() {
        ampEnv.noteOff();
        modEnv.noteOff();
    }
    void render(float* l, float* r, const float* harm, int n) {
        for (int i = 0; i < n; ++i) {
            if (!ampEnv.isActive()) { active = false; return; }
            ex.step();
            const float f = freq * ex.pitchFactor();
            carrier.setFreq(f);
            modulator.setFreq(harm[i] * f);
            const float m = modulator.next() * modEnv.next() * vel * kSynthVolumeGain;
            const float a2g = (m + 1.0f) * 0.5f;
            const float s = carrier.next() * ampEnv.next() * vel * kSynthVolumeGain * a2g * ex.gain();
            l[i] += s;
            r[i] += s;
        }
    }
};

class KeysInst final : public InstrumentDevice {
public:
    KeysInst() {
        harm_.prepare(sr_, 15.0f);
        harm_.snap(2.0f);
        for (auto& v : voices_) v.prepare(sr_);
        applyEnv();
    }

    std::span<const ParamSpec> params() const override { return schema::kInstKeys; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        harm_.prepare(sr_, 15.0f);
        harm_.snap(harm_.target());
        for (auto& v : voices_) v.prepare(sr_);
        applyEnv();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Harm: harm_.setTarget(std::clamp(v, 0.5f, 4.0f)); break;
            case Attack: attack_ = std::clamp(v, 0.001f, 2.0f); applyEnv(); break;
            case Decay: decay_ = std::clamp(v, 0.01f, 2.0f); applyEnv(); break;
            case Sustain: sustain_ = std::clamp(v, 0.0f, 1.0f); applyEnv(); break;
            case Release: release_ = std::clamp(v, 0.01f, 4.0f); applyEnv(); break;
            default: break;
        }
    }

    // MPE: bend (semitones) and pressure (up to +50% level); slide is unused.
    void noteExpression(uint32_t id, int dimension, float value) override {
        for (auto& v : voices_) if (v.active && v.noteId == id) v.ex.set(dimension, value);
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        voices_[allocVoice(pitch)].start(pitch, velocity, ++ageCounter_, noteId);
    }

    // Releases the voice(s) owned by this note id. A voice reused by a later note of the same pitch
    // carries the newer id, so the older note's off is superseded and ignored.
    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_)
            if (v.active && v.noteId == noteId) v.release();
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int off = 0; off < n; off += kMaxBlock) {
            const int c = std::min(kMaxBlock, n - off);
            harm_.fill(harmBuf_.data(), c);
            for (auto& v : voices_)
                if (v.active) v.render(l + off, r + off, harmBuf_.data(), c);
        }
    }

    void reset() override {
        for (auto& v : voices_) v.kill();
        harm_.snap(harm_.target());
        ageCounter_ = 0;
    }

private:
    void applyEnv() {
        for (auto& v : voices_) v.ampEnv.setAdsr(attack_, decay_, sustain_, release_);
    }

    // Same-pitch reuse first (PolySynth reuses the voice already playing the note), then a free voice,
    // then steal the oldest.
    size_t allocVoice(uint8_t pitch) const {
        int free = -1;
        size_t oldest = 0;
        uint64_t oldestAge = UINT64_MAX;
        for (size_t i = 0; i < voices_.size(); ++i) {
            const Voice& v = voices_[i];
            if (v.active) {
                if (v.pitch == pitch) return i;
                if (v.age < oldestAge) { oldestAge = v.age; oldest = i; }
            } else if (free < 0) {
                free = static_cast<int>(i);
            }
        }
        return free >= 0 ? static_cast<size_t>(free) : oldest;
    }

    float sr_ = 44100.0f;
    std::array<Voice, kNumVoices> voices_;
    Smoother harm_;
    float attack_ = 0.01f, decay_ = 0.4f, sustain_ = 0.5f, release_ = 0.8f;
    uint64_t ageCounter_ = 0;
    std::array<float, kMaxBlock> harmBuf_{};
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_keys() { return std::make_unique<KeysInst>(); }

}  // namespace ddaw::devices
