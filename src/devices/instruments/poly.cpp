// `poly` instrument: port of sf-dsp/src/inst/poly.rs (Tone.PolySynth(Tone.Synth, 16) -> one shared
// lowpass). Fixed pool of 16 voices, oldest-voice stealing, noteIds tracked per voice. Each voice is
// one oscillator + amp ADSR; the "fat" waves (4..6) layer 3 unison oscillators detuned
// [-spread/2, 0, +spread/2] cents at -9.3 dB each, phases i/3 (Tone.FatOscillator). The shared filter
// is a cascade of 1/2/4 SVF stages (rolloff 12/24/48 dB by `slope`, Q from `res` in dB), with the
// built-in instrument LFO summed into the cutoff as +-cutoff*amt. Smoothers advance per sample but the
// filter/LFO are updated at control rate (every 16 samples), exactly like the Rust device.
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

// Index in schema::kInstPoly.
enum P : uint16_t { Wave_, Spread, Cutoff, Res, Slope, Attack, Decay, Sustain, Release, LfoShape, LfoRate, LfoAmt };

constexpr int kVoices = 16;
constexpr int kUnison = 3;
constexpr int kCtrl = 16;                       // control-rate interval (samples)
constexpr float kFatGain = 0.3428f;             // -6 - count*1.1 dB
constexpr std::array<int, 3> kStageCount = {1, 2, 4};
constexpr std::array<uint32_t, 4> kLfoShapeMap = {0, 1, 2, 4};  // sine, triangle, saw up, square
constexpr float kSilence = 1e-7f;               // filter ringing below this (-140 dB) after the last voice ends is dropped

struct Voice {
    std::array<PolyBlepOsc, kUnison> oscs{PolyBlepOsc(Wave::Saw), PolyBlepOsc(Wave::Saw), PolyBlepOsc(Wave::Saw)};
    Adsr env;
    uint8_t pitch = 0;
    float vel = 0.0f;
    uint64_t serial = 0;
    uint32_t id = 0;
    bool gated = false;
    bool inUse = false;
};

class PolyInst final : public InstrumentDevice {
public:
    PolyInst() {
        spread_.snap(18.0f);
        cutoff_.snap(7000.0f);
        res_.snap(0.7f);
        lfoRate_.snap(2.0f);
        lfoAmt_.snap(0.0f);
        cutoff_.setTimeMs(30.0f);  // JS ramps cutoff and LFO depth with rampTo(..., 0.03)
        lfoAmt_.setTimeMs(30.0f);
    }

    std::span<const ParamSpec> params() const override { return schema::kInstPoly; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        for (auto& v : voices_) {
            for (auto& o : v.oscs) o.prepare(sr_);
            v.env.prepare(sr_);
            v.env.reset();
            v.inUse = false;
            v.gated = false;
        }
        for (auto& s : stages_) s.prepare(sr_);
        for (Smoother* s : {&spread_, &res_, &lfoRate_}) { s->prepare(sr_, 15.0f); s->snap(s->target()); }
        for (Smoother* s : {&cutoff_, &lfoAmt_}) { s->prepare(sr_, 30.0f); s->snap(s->target()); }
        lfoPhase_ = 0.0;
        ringing_ = false;
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Wave_: wave_ = std::clamp(static_cast<int>(v), 0, 6); break;
            case Spread: spread_.setTarget(std::clamp(v, 0.0f, 60.0f)); break;
            case Cutoff: cutoff_.setTarget(std::max(v, 1.0f)); break;
            case Res: res_.setTarget(std::clamp(v, 0.0f, 10.0f)); break;
            case Slope: slope_ = std::clamp(static_cast<int>(v), 0, 2); break;
            case LfoShape: lfoShape_ = std::clamp(static_cast<int>(v), 0, 3); break;
            case LfoRate: lfoRate_.setTarget(std::max(v, 0.0f)); break;
            case LfoAmt: lfoAmt_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            case Attack: attack_ = std::max(v, 0.0f); syncEnv(); break;
            case Decay: decay_ = std::max(v, 0.0f); syncEnv(); break;
            case Sustain: sustain_ = std::clamp(v, 0.0f, 1.0f); syncEnv(); break;
            case Release: release_ = std::max(v, 0.0f); syncEnv(); break;
            default: break;
        }
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        // free voice first, else steal the oldest (smallest serial)
        int idx = -1;
        for (int i = 0; i < kVoices; ++i) if (!voices_[static_cast<size_t>(i)].inUse) { idx = i; break; }
        if (idx < 0) {
            uint64_t best = UINT64_MAX;
            idx = 0;
            for (int i = 0; i < kVoices; ++i) {
                if (voices_[static_cast<size_t>(i)].serial < best) { best = voices_[static_cast<size_t>(i)].serial; idx = i; }
            }
        }
        ++serial_;
        const bool fat = wave_ >= 4;
        Voice& v = voices_[static_cast<size_t>(idx)];
        v.pitch = pitch;
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.serial = serial_;
        v.id = noteId;
        v.gated = true;
        v.inUse = true;
        for (int i = 0; i < kUnison; ++i)
            v.oscs[static_cast<size_t>(i)].resetPhase(fat ? static_cast<float>(i) / static_cast<float>(kUnison) : 0.0f);
        // attack ramps from the current level, so steals do not click
        v.env.setAdsr(attack_, decay_, sustain_, release_);
        v.env.noteOn();
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_) {
            if (v.inUse && v.gated && v.id == noteId) {
                v.env.noteOff();
                v.gated = false;
            }
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        int done = 0;
        while (done < n) {
            const int m = std::min(n - done, kCtrl);

            float spread = spread_.current(), cutoff = cutoff_.current(), res = res_.current();
            float rate = lfoRate_.current(), amt = lfoAmt_.current();
            for (int k = 0; k < m; ++k) {
                spread = spread_.next();
                cutoff = cutoff_.next();
                res = res_.next();
                rate = lfoRate_.next();
                amt = lfoAmt_.next();
            }

            // the LFO is gated like makeInstLfo: stopped while lfoAmt ~ 0
            const bool lfoOn = amt > 0.001f;
            const float lfoV = lfoOn ? lfoShapeValue(kLfoShapeMap[static_cast<size_t>(lfoShape_)], lfoPhase_) : 0.0f;
            if (lfoOn) lfoPhase_ += static_cast<double>(rate) * m / static_cast<double>(sr_);

            bool anyActive = false;
            for (const auto& v : voices_) if (v.inUse) { anyActive = true; break; }
            if (!anyActive && !ringing_) {
                done += m;
                continue;
            }

            const float eff = cutoff + cutoff * amt * lfoV;
            const float q = dbToLin(res);
            const int nSt = kStageCount[static_cast<size_t>(slope_)];
            for (int s = 0; s < nSt; ++s) stages_[static_cast<size_t>(s)].setCutoffQ(eff, q);

            std::array<float, kCtrl> scratch;
            scratch.fill(0.0f);

            const Wave wave = waveFromIndex(wave_);
            const bool fat = wave_ >= 4;
            const float det = std::pow(2.0f, spread * 0.5f / 1200.0f);
            const int count = fat ? kUnison : 1;
            const float gain = fat ? kFatGain : 1.0f;
            for (auto& v : voices_) {
                if (!v.inUse) continue;
                const float base = midiHz(static_cast<float>(v.pitch));
                for (int i = 0; i < count; ++i) {
                    auto& o = v.oscs[static_cast<size_t>(i)];
                    o.setWave(wave);
                    float f = base;
                    if (fat) f = i == 0 ? base / det : (i == 1 ? base : base * det);
                    o.setFreq(f);
                }
                for (int k = 0; k < m; ++k) {
                    const float e = v.env.next();
                    if (e != 0.0f) {
                        float x = 0.0f;
                        for (int i = 0; i < count; ++i) x += v.oscs[static_cast<size_t>(i)].next();
                        scratch[static_cast<size_t>(k)] += x * gain * e * v.vel;
                    }
                }
                if (!v.env.isActive()) v.inUse = false;
            }

            for (int s = 0; s < nSt; ++s) stages_[static_cast<size_t>(s)].process(scratch.data(), m);
            for (int k = 0; k < m; ++k) {
                l[done + k] += scratch[static_cast<size_t>(k)];
                r[done + k] += scratch[static_cast<size_t>(k)];
            }

            if (anyActive) {
                ringing_ = true;
            } else {
                // last voice has ended: let the shared filter ring out, then drop its state
                float peak = 0.0f;
                for (int k = 0; k < m; ++k) peak = std::max(peak, std::abs(scratch[static_cast<size_t>(k)]));
                if (peak < kSilence) {
                    for (auto& s : stages_) s.reset();
                    ringing_ = false;
                }
            }
            done += m;
        }
    }

    void reset() override {
        for (auto& v : voices_) {
            v.env.reset();
            v.inUse = false;
            v.gated = false;
            v.serial = 0;
            v.id = 0;
            for (auto& o : v.oscs) o.resetPhase(0.0f);
        }
        serial_ = 0;
        for (auto& s : stages_) s.reset();
        for (Smoother* s : {&spread_, &cutoff_, &res_, &lfoRate_, &lfoAmt_}) s->snap(s->target());
        lfoPhase_ = 0.0;
        ringing_ = false;
    }

private:
    // envelope keys apply to live voices too (Tone synth.set does)
    void syncEnv() {
        for (auto& v : voices_) if (v.inUse) v.env.setAdsr(attack_, decay_, sustain_, release_);
    }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    uint64_t serial_ = 0;
    int wave_ = 0, slope_ = 0, lfoShape_ = 0;
    Smoother spread_, cutoff_, res_, lfoRate_, lfoAmt_;
    float attack_ = 0.01f, decay_ = 0.15f, sustain_ = 0.6f, release_ = 0.4f;
    double lfoPhase_ = 0.0;
    std::array<Svf, 4> stages_{Svf(SvfMode::Lowpass), Svf(SvfMode::Lowpass), Svf(SvfMode::Lowpass), Svf(SvfMode::Lowpass)};
    bool ringing_ = false;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_poly() { return std::make_unique<PolyInst>(); }

}  // namespace ddaw::devices
