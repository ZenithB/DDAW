// `follow` instrument: a voice that follows the performance tracker. Each PerformanceFrame sets the target
// pitch and the loudness; the voice glides to the pitch and follows the envelope, and closes when the input
// is unvoiced or below the gate. Without tracker frames (none for 100 ms) it is a plain monosynth played
// by notes, so it also works from a clip or a keyboard.
#include <algorithm>
#include <cmath>
#include <memory>

#include "devices/instruments/follow.h"
#include "dsp/Math.h"
#include "dsp/NoteExpr.h"
#include "dsp/PolyBlepOsc.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;
enum P : uint16_t { Wave_, Octave, Glide, Level, Gate, Release };

class FollowInst final : public InstrumentDevice {
public:
    FollowInst() { prepare(sr_, 128); }
    std::span<const ParamSpec> params() const override { return schema::kInstFollow; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 1.0));
        osc_.prepare(sr_);
        for (auto* s : {&level_, &gate_, &release_, &glide_}) s->prepare(sr_, 15.0f);
        level_.snap(0.8f); gate_.snap(-55.0f); release_.snap(120.0f); glide_.snap(30.0f);
        reset();
    }
    void setParam(uint16_t i, float v) override {
        switch (i) {
            case Wave_: { static const Wave w[4] = {Wave::Saw, Wave::Square, Wave::Triangle, Wave::Sine}; osc_.setWave(w[std::clamp(int(std::lround(v)), 0, 3)]); break; }
            case Octave: octave_ = std::clamp(int(std::lround(v)), -2, 2); break;
            case Glide: glide_.setTarget(std::clamp(v, 1.0f, 500.0f)); break;
            case Level: level_.setTarget(std::clamp(v, 0.0f, 1.5f)); break;
            case Gate: gate_.setTarget(std::clamp(v, -70.0f, -20.0f)); break;
            case Release: release_.setTarget(std::clamp(v, 5.0f, 1000.0f)); break;
            default: break;
        }
    }
    void performance(const PerformanceFrame& f) override {
        perfAge_ = 0;
        voiced_ = f.f0Hz > 0.0f && f.loudnessDb > gate_.current();
        if (voiced_) targetHz_ = f.f0Hz;
        targetAmp_ = voiced_ ? std::min(1.0f, f.envelope * 2.0f) : 0.0f;
    }
    void noteOn(uint8_t pitch, float velocity, uint32_t id) override { noteHz_ = midiHz(float(pitch)); noteAmp_ = velocity; noteHeld_ = true; noteId_ = id; ex_.clear(); }
    // MPE (note mode only; a tracked voice is the performance): bend (semitones) and pressure (up to +50% level).
    void noteExpression(uint32_t id, int dimension, float value) override { if (id == noteId_) ex_.set(dimension, value); }
    void noteOff(uint32_t) override { noteHeld_ = false; }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        const bool tracked = perfAge_ < int(0.1f * sr_);
        for (int i = 0; i < n; ++i) {
            const float lvl = level_.next(), glideMs = glide_.next(), relMs = release_.next();
            gate_.next();
            if (tracked) { ++perfAge_; } else { perfAge_ = std::min(perfAge_ + 1, 1 << 30); }
            const float hz = tracked ? targetHz_ : noteHz_;
            const float amp = (tracked ? targetAmp_ : (noteHeld_ ? noteAmp_ : 0.0f)) * lvl;
            if (hz > 0.0f) {
                if (curHz_ <= 0.0f) curHz_ = hz;
                const float g = 1.0f - std::exp(-1.0f / (0.001f * glideMs * sr_));
                curHz_ += (hz - curHz_) * g;
                ex_.step();
                osc_.setFreq(curHz_ * std::pow(2.0f, float(octave_)) * (tracked ? 1.0f : ex_.pitchFactor()));
            }
            // attack 5 ms, release by parameter
            const float a = amp > curAmp_ ? 1.0f - std::exp(-1.0f / (0.005f * sr_)) : 1.0f - std::exp(-1.0f / (0.001f * relMs * sr_));
            curAmp_ += (amp - curAmp_) * a;
            if (curAmp_ < 1e-5f && amp == 0.0f) { curAmp_ = 0.0f; continue; }
            const float s = osc_.next() * curAmp_ * 0.5f * (tracked ? 1.0f : ex_.gain());
            l[i] += s;
            r[i] += s;
        }
    }
    void reset() override {
        osc_.resetPhase(0.0f);
        voiced_ = false; noteHeld_ = false;
        targetHz_ = noteHz_ = curHz_ = 0.0f;
        targetAmp_ = noteAmp_ = curAmp_ = 0.0f;
        perfAge_ = 1 << 30;
        level_.snap(level_.target()); glide_.snap(glide_.target()); release_.snap(release_.target()); gate_.snap(gate_.target());
    }

private:
    float sr_ = 48000.0f;
    PolyBlepOsc osc_{Wave::Saw};
    Smoother level_, gate_, release_, glide_;
    int octave_ = 0, perfAge_ = 1 << 30;
    bool voiced_ = false, noteHeld_ = false;
    uint32_t noteId_ = 0;
    dsp::NoteExpr ex_;
    float targetHz_ = 0, noteHz_ = 0, curHz_ = 0, targetAmp_ = 0, noteAmp_ = 0, curAmp_ = 0;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_follow() { return std::make_unique<FollowInst>(); }

}  // namespace ddaw::devices
