// `sampler` instrument: port of sf-dsp/src/inst/sampler.rs (browser makeSampler()).
// ONE buffer (slot 0) keyed at C3 (MIDI 48) and played back pitch-relative: incoming pitch p plays
// the sample at rate 2^((p + tune - 48) / 12), scaled by srcRate / engineRate. One-shot per note: the
// sample plays from its head until it runs out or the release finishes. Envelope mirrors
// Tone.Sampler's fade (attack ramp, hold at 1, release on noteOff), output gain 0.9 mirrors the
// device's Tone.Gain(0.9). 12 fixed voices, oldest stolen, per-voice noteIds (a stale noteOff for a
// stolen voice is ignored). No sample loaded (never set, or null) -> silence.
// `tune`, `attack` and `release` are read at trigger time, as in the Rust device (Tone computes the
// playback rate and envelope when the note starts), so they are not smoothed.
// Deviation: the Rust trigger(dur) auto note-off is the scheduler's noteOff here.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/NoteExpr.h"
#include "dsp/Adsr.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Tune, Attack, Release };

constexpr int kMaxVoices = 12;
constexpr float kOutGain = 0.9f;
constexpr float kRootMidi = 48.0f;  // Tone.Sampler zone key: unpitched at C3

struct Voice {
    bool active = false;
    uint32_t noteId = 0;
    double pos = 0.0, step = 0.0;
    Adsr env;
    float vel = 0.0f;
    uint64_t age = 0;
    dsp::NoteExpr ex;   // MPE: bend scales the playback rate, pressure the level
};

class SamplerInst final : public InstrumentDevice {
public:
    std::span<const ParamSpec> params() const override { return schema::kInstSampler; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        for (auto& v : voices_) { v.active = false; v.env.prepare(sr_); }
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Tune: tune_ = v; break;
            case Attack: attack_ = std::max(v, 0.0f); break;
            case Release: release_ = std::max(v, 0.0f); break;
            default: break;
        }
    }

    void setSample(uint32_t slot, SamplePtr buf) override {
        if (slot != 0) return;
        for (auto& v : voices_) v.active = false;  // control path: nobody reads across the swap
        buf_ = std::move(buf);
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        const SampleBuf* b = buf_.get();
        if (!b || b->frames() == 0) return;
        const float semis = static_cast<float>(pitch) + tune_ - kRootMidi;
        const double rate = std::pow(2.0, static_cast<double>(semis) / 12.0);
        const double step = rate * static_cast<double>(b->sampleRate) / static_cast<double>(std::max(sr_, 1.0f));

        Voice* vp = nullptr;
        for (auto& v : voices_) if (!v.active) { vp = &v; break; }
        if (!vp) {
            vp = &voices_[0];
            for (auto& v : voices_) if (v.age < vp->age) vp = &v;  // steal the oldest
        }
        Voice& v = *vp;
        v.active = true;
        v.noteId = noteId;
        v.pos = 0.0;
        v.step = step;
        v.ex.clear();
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.age = ++age_;
        v.env.setAdsr(attack_, 0.0f, 1.0f, release_);  // Tone.Sampler fade: attack, hold, release
        v.env.reset();
        v.env.noteOn();
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_)
            if (v.active && v.noteId == noteId) v.env.noteOff();
    }

    void noteExpression(uint32_t noteId, int dimension, float value) override {
        for (auto& v : voices_) if (v.active && v.noteId == noteId) v.ex.set(dimension, value);
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        const SampleBuf* b = buf_.get();
        if (!b) return;
        const double frames = static_cast<double>(b->frames());
        if (frames <= 0.0) return;
        for (auto& v : voices_) {
            if (!v.active) continue;
            for (int k = 0; k < n; ++k) {
                const float e = v.env.next();
                if (!v.env.isActive() || v.pos >= frames) { v.active = false; break; }
                float sl, sr;
                b->readLin(v.pos, sl, sr);
                v.ex.step();
                const float g = e * v.vel * kOutGain * v.ex.gain();
                l[k] += sl * g;
                r[k] += sr * g;
                v.pos += v.ex.bendS == 0.0f ? v.step : v.step * double(v.ex.pitchFactor());
            }
        }
    }

    void reset() override {
        for (auto& v : voices_) { v.active = false; v.env.reset(); }
        age_ = 0;
    }

private:
    float sr_ = 44100.0f;
    SamplePtr buf_;
    float tune_ = 0.0f, attack_ = 0.005f, release_ = 0.4f;
    std::array<Voice, kMaxVoices> voices_;
    uint64_t age_ = 0;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_sampler() { return std::make_unique<SamplerInst>(); }

}  // namespace ddaw::devices
