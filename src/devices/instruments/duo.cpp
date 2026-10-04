// `duo` instrument: port of sf-dsp/src/inst/duo.rs (Tone.DuoSynth as wired by makeDuo()).
// Per voice: two sawtooth layers (the second at `harm` x the base frequency) detuned by a shared
// vibrato LFO of +-50 cents * vibAmt at vibRate, one lowpass (Q 1, 12 dB) driven by DuoSynth's
// default filter envelope (0.01/0/1/0.5, 200 Hz base + 3 octaves through env^2), and the schema ADSR
// on the amp. Both layers sum at unity. Fixed pool of 6 voices, oldest stolen; a noteOn on a pitch
// that is still held reuses that voice (as the Rust device does). noteIds are tracked per voice.
#include <algorithm>
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

// Index in schema::kInstDuo.
enum P : uint16_t { Harm, VibAmt, VibRate, Attack, Decay, Sustain, Release };

constexpr int kVoices = 6;
constexpr float kSawStartPhase = 0.5f;  // Web Audio's native sawtooth starts mid-rise
constexpr float kFiltBaseHz = 200.0f;
constexpr float kFiltTopHz = 1600.0f;
constexpr float kFiltQ = 1.0f;
constexpr float kVibRangeCents = 50.0f;

struct Voice {
    PolyBlepOsc osc0{Wave::Saw}, osc1{Wave::Saw};
    Svf filt{SvfMode::Lowpass};
    Adsr amp, fenv;
    float freq = 440.0f, vel = 0.0f, lastCutoff = 0.0f;
    uint8_t pitch = 0;
    uint64_t age = 0;
    uint32_t id = 0;
    bool released = false;

    Voice() { fenv.setAdsr(0.01f, 0.0f, 1.0f, 0.5f); }

    void prepare(float sr) {
        osc0.prepare(sr);
        osc1.prepare(sr);
        filt.prepare(sr);
        amp.prepare(sr);
        fenv.prepare(sr);
        fenv.setAdsr(0.01f, 0.0f, 1.0f, 0.5f);
        lastCutoff = 0.0f;
    }

    void start(uint8_t p, float v, uint64_t a, uint32_t noteId, const std::array<float, 4>& adsr) {
        pitch = p;
        freq = midiHz(static_cast<float>(p));
        vel = std::clamp(v, 0.0f, 1.0f);
        age = a;
        id = noteId;
        released = false;
        amp.setAdsr(adsr[0], adsr[1], adsr[2], adsr[3]);
        osc0.resetPhase(kSawStartPhase);
        osc1.resetPhase(kSawStartPhase);
        amp.noteOn();
        fenv.noteOn();
    }

    void release() {
        released = true;
        amp.noteOff();
        fenv.noteOff();
    }

    float sample(float harm, float vib) {
        if (!amp.isActive()) return 0.0f;
        // Tone.FrequencyEnvelope: linear scale(base, base*2^oct) after env^2
        const float fe = fenv.next();
        const float cutoff = kFiltBaseHz + (kFiltTopHz - kFiltBaseHz) * fe * fe;
        if (std::abs(cutoff - lastCutoff) > lastCutoff * 0.005f) {
            filt.setCutoffQ(cutoff, kFiltQ);
            lastCutoff = cutoff;
        }
        osc0.setFreq(freq * vib);
        osc1.setFreq(freq * harm * vib);
        const float x = osc0.next() + osc1.next();
        return filt.processSample(x) * amp.next() * vel;
    }
};

class DuoInst final : public InstrumentDevice {
public:
    DuoInst() {
        harm_.snap(1.5f);
        vibAmt_.snap(0.12f);
        vibRate_.snap(4.5f);
    }

    std::span<const ParamSpec> params() const override { return schema::kInstDuo; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        for (auto& v : voices_) v.prepare(sr_);
        for (Smoother* s : {&harm_, &vibAmt_, &vibRate_}) { s->prepare(sr_, 15.0f); s->snap(s->target()); }
        applyAdsr();
        vibPhase_ = 0.0;
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Harm: harm_.setTarget(std::clamp(v, 0.5f, 3.0f)); break;
            case VibAmt: vibAmt_.setTarget(std::clamp(v, 0.0f, 0.6f)); break;
            case VibRate: vibRate_.setTarget(std::clamp(v, 0.5f, 10.0f)); break;
            case Attack: adsr_[0] = std::max(v, 0.0f); applyAdsr(); break;
            case Decay: adsr_[1] = std::max(v, 0.0f); applyAdsr(); break;
            case Sustain: adsr_[2] = std::clamp(v, 0.0f, 1.0f); applyAdsr(); break;
            case Release: adsr_[3] = std::max(v, 0.0f); applyAdsr(); break;
            default: break;
        }
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        ++counter_;
        // reuse the held voice on this pitch, else a free voice, else steal the oldest
        int idx = -1;
        for (int i = 0; i < kVoices; ++i) {
            const Voice& v = voices_[static_cast<size_t>(i)];
            if (v.amp.isActive() && !v.released && v.pitch == pitch) { idx = i; break; }
        }
        if (idx < 0)
            for (int i = 0; i < kVoices; ++i) if (!voices_[static_cast<size_t>(i)].amp.isActive()) { idx = i; break; }
        if (idx < 0) {
            uint64_t best = UINT64_MAX;
            idx = 0;
            for (int i = 0; i < kVoices; ++i)
                if (voices_[static_cast<size_t>(i)].age < best) { best = voices_[static_cast<size_t>(i)].age; idx = i; }
        }
        voices_[static_cast<size_t>(idx)].start(pitch, velocity, counter_, noteId, adsr_);
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_)
            if (v.amp.isActive() && !v.released && v.id == noteId) v.release();
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        bool any = false;
        for (const auto& v : voices_) if (v.amp.isActive()) { any = true; break; }
        if (!any) {
            // keep smoothers and the vibrato phase moving while silent
            for (int i = 0; i < n; ++i) { harm_.next(); vibAmt_.next(); advanceVibrato(); }
            return;
        }
        for (int i = 0; i < n; ++i) {
            const float harm = harm_.next();
            const float amt = vibAmt_.next();
            const double phase = advanceVibrato();
            const float cents = lfoShapeValue(0, phase) * kVibRangeCents * amt;
            const float vib = std::pow(2.0f, cents / 1200.0f);
            float sum = 0.0f;
            for (auto& v : voices_) sum += v.sample(harm, vib);
            l[i] += sum;
            r[i] += sum;
        }
    }

    void reset() override {
        for (auto& v : voices_) {
            v.amp.reset();
            v.fenv.reset();
            v.filt.reset();
            v.released = false;
            v.lastCutoff = 0.0f;
            v.age = 0;
            v.id = 0;
        }
        counter_ = 0;
        vibPhase_ = 0.0;
        for (Smoother* s : {&harm_, &vibAmt_, &vibRate_}) s->snap(s->target());
    }

private:
    void applyAdsr() {
        for (auto& v : voices_) v.amp.setAdsr(adsr_[0], adsr_[1], adsr_[2], adsr_[3]);
    }

    double advanceVibrato() {
        const float rate = vibRate_.next();
        vibPhase_ += static_cast<double>(rate / sr_);
        if (vibPhase_ >= 1.0) vibPhase_ -= 1.0;
        return vibPhase_;
    }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    Smoother harm_, vibAmt_, vibRate_;
    std::array<float, 4> adsr_{0.02f, 0.3f, 0.6f, 0.6f};
    double vibPhase_ = 0.0;
    uint64_t counter_ = 0;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_duo() { return std::make_unique<DuoInst>(); }

}  // namespace ddaw::devices
