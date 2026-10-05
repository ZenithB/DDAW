// `subtractive` instrument (B6): the classic oscillators-filter-envelope voice. Two PolyBLEP oscillators (saw, pulse with a
// width, triangle, sine) with a semitone/cent offset on the second, a sub-oscillator one octave down and noise feed a
// resonant ladder filter (12 or 24 dB/octave) whose cutoff follows its own ADSR, the key and the velocity; then the amplitude
// ADSR. The filter coefficients are refreshed every 16 samples, or every sample while cutoff is being modulated at audio
// rate. A-rate ports: cutoff 0 and pitch 1 (added to the control value per sample, bypassing the smoother). Eight voices.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/SchemaB6.h"
#include "dsp/Adsr.h"
#include "dsp/FastSin.h"
#include "dsp/Ladder.h"
#include "dsp/Math.h"
#include "dsp/NoteExpr.h"
#include "dsp/Smoother.h"
#include "dsp/VoicePool.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Wave1, Wave2, Pw, Semi2, Detune2, Mix2, Sub, Noise, Cutoff, Res, Slope, Drive, Keytrack, EnvAmt, FAttack, FDecay, FSustain, FRelease, VelFilt, Attack, Decay, Sustain, Release, Pitch, Level, kNumP };
enum A : size_t { ACutoff = 0, APitch = 1 };

constexpr int kVoices = 8;
constexpr int kCtrl = 16;

inline float polyBlep(float t, float dt) noexcept {
    if (t < dt) { const float x = t / dt; return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { const float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}

// One band-limited oscillator: 0 saw, 1 pulse (width pw), 2 triangle (leaky-integrated square), 3 sine.
struct Osc {
    float ph = 0.0f, tri = 0.0f;
    void reset(float phase) { ph = phase; tri = 0.0f; }
    float leak = 0.9996f;
    float next(int wave, float inc, float pw) noexcept {
        const float dt = std::max(inc, 1e-9f);
        float out;
        switch (wave) {
            case 0: out = 2.0f * ph - 1.0f - polyBlep(ph, dt); break;
            case 1: {
                float t2 = ph + (1.0f - pw); t2 -= std::floor(t2);
                out = (ph < pw ? 1.0f : -1.0f) + polyBlep(ph, dt) - polyBlep(t2, dt);
                break;
            }
            case 2: {
                float t2 = ph + 0.5f; t2 -= std::floor(t2);
                const float s = (ph < 0.5f ? 1.0f : -1.0f) + polyBlep(ph, dt) - polyBlep(t2, dt);
                tri = 4.0f * dt * s + leak * tri;   // integrate the square (unit peak); a 3 Hz leak keeps the DC from drifting
                out = tri;
                break;
            }
            default: out = fastSin2Pi(ph); break;
        }
        ph += inc;
        ph -= ph >= 1.0f ? 1.0f : 0.0f;
        return out;
    }
};

struct Voice {
    Osc o1, o2, sub;
    Adsr aenv, fenv;
    Ladder lad;
    NoteExpr ex;
    float freq = 440.0f, vel = 1.0f;
    uint32_t rng = 1;
    uint8_t pitch = 69;
    bool held = false;
    uint64_t serial = 0;
    uint32_t id = 0;
    bool active() const { return aenv.isActive(); }
};

class Subtractive final : public InstrumentDevice {
public:
    Subtractive() {
        for (const auto& ps : schema::kInstSubtractive) setParam(ps.index, ps.def);
        for (auto& s : s_) s.snap(s.target());
    }

    std::span<const ParamSpec> params() const override { return schema::kInstSubtractive; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 8000.0));
        for (auto& s : s_) s.prepare(sr_, 15.0f);
        const float leak = 1.0f - 6.2831853f * 3.0f / sr_;
        for (auto& v : voices_) { v.o1.leak = v.o2.leak = v.sub.leak = leak; v.aenv.prepare(sr_); v.fenv.prepare(sr_); v.lad.prepare(sr_); v.aenv.reset(); v.fenv.reset(); v.held = false; }
        applyAdsr();
        for (int i = 0; i < kNumP; ++i) setParam(uint16_t(i), ctl_[i]);
        for (auto& s : s_) s.snap(s.target());
    }

    void setParam(uint16_t i, float v) override {
        if (i >= kNumP) return;
        const ParamSpec& ps = schema::kInstSubtractive[i];
        v = std::clamp(v, ps.min, ps.max);
        ctl_[i] = v;
        s_[i].setTarget(v);
        if (i >= FAttack && i <= FRelease) applyAdsr();
        if (i >= Attack && i <= Release) applyAdsr();
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        Voice& v = voices_[size_t(pickVoice(voices_, pitch))];
        v.o1.reset(0.0f); v.o2.reset(0.25f); v.sub.reset(0.0f);
        v.lad.reset();
        v.ex.clear();
        v.pitch = pitch;
        v.freq = midiHz(float(pitch));
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.serial = nextSerial_++;
        v.id = noteId;
        v.held = true;
        v.rng = 0x85EBCA6Bu * (noteId + 1u) + 777u;
        v.aenv.reset(); v.fenv.reset();
        v.aenv.noteOn(); v.fenv.noteOn();
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_) if (v.held && v.id == noteId) { v.held = false; v.aenv.noteOff(); v.fenv.noteOff(); }
    }

    // MPE: slide opens the filter (up to two octaves), pressure raises the level by up to 50%, bend is in semitones.
    void noteExpression(uint32_t noteId, int dimension, float value) override {
        for (auto& v : voices_) if (v.active() && v.id == noteId) v.ex.set(dimension, value);
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs& mod) override {
        const float* aCut = aBuf(mod, ACutoff);
        const float* aPit = aBuf(mod, APitch);
        const float invSr = 1.0f / sr_;
        for (int i = 0; i < n; ++i) {
            const int wave1 = int(std::lround(ctl_[Wave1])), wave2 = int(std::lround(ctl_[Wave2]));
            const float pw = s_[Pw].next();
            const float semi2 = s_[Semi2].next(), det2 = s_[Detune2].next();
            const float mix2 = s_[Mix2].next(), sub = s_[Sub].next(), noise = s_[Noise].next();
            const float cutoff = s_[Cutoff].next(), res = s_[Res].next(), drive = s_[Drive].next();
            const float pitch = s_[Pitch].next(), level = s_[Level].next();
            const float keytrack = s_[Keytrack].next(), envAmt = s_[EnvAmt].next(), velFilt = s_[VelFilt].next();
            const float cutNow = std::clamp(cutoff + (aCut ? aCut[i] : 0.0f), 40.0f, 16000.0f);
            const float semiNow = pitch + (aPit ? aPit[i] : 0.0f);
            const float ratio2 = std::exp2((semi2 + det2 * 0.01f) * (1.0f / 12.0f));
            // The ladder's saturator is gentle only for small signals: the oscillators go in at a quarter of their level (clean to
            // about -45 dB distortion) and drive raises that up to twelve-fold, into audible saturation. The makeup is the
            // saturator's own gain at that level, so a full-scale oscillator comes out near full scale at any drive.
            const float inGain = 0.25f * std::pow(12.0f, drive);
            const float gs = std::min(inGain, 3.0f);
            const float makeup = (1.0f + 1.2f * res) * (27.0f + 9.0f * gs * gs) / (gs * (27.0f + gs * gs));
            const bool slope24 = ctl_[Slope] >= 0.5f;
            const bool refresh = aCut != nullptr || (ctrlCount_ & (kCtrl - 1)) == 0;
            ++ctrlCount_;
            float acc = 0.0f;
            for (auto& v : voices_) {
                if (!v.active()) continue;
                v.ex.step();
                const float a = v.aenv.next();
                const float fe = v.fenv.next();
                const float pf = std::exp2(semiNow * (1.0f / 12.0f)) * v.ex.pitchFactor();
                const float inc1 = v.freq * pf * invSr;
                float x = v.o1.next(wave1, inc1, pw);
                if (mix2 > 0.0f) x = (x + mix2 * v.o2.next(wave2, inc1 * ratio2, pw)) * (1.0f / (1.0f + mix2));
                else v.o2.next(wave2, inc1 * ratio2, pw);
                if (sub > 0.0f) x += sub * 0.5f * v.sub.next(1, inc1 * 0.5f, 0.5f);
                if (noise > 0.0f) {
                    v.rng ^= v.rng << 13; v.rng ^= v.rng >> 17; v.rng ^= v.rng << 5;
                    x += noise * 0.5f * float(int32_t(v.rng)) * (1.0f / 2147483648.0f);
                }
                if (refresh) {
                    const float oct = 6.0f * envAmt * fe + keytrack * (float(v.pitch) - 60.0f) * (1.0f / 12.0f) + 2.0f * velFilt * (2.0f * v.vel - 1.0f) + 2.0f * v.ex.slideS;
                    v.lad.setMode(slope24 ? LadderMode::Db24 : LadderMode::Db12);
                    v.lad.setCutoffRes(cutNow * std::exp2(oct), res);
                }
                acc += v.lad.processSample(x * inGain) * makeup * a * v.vel * v.ex.gain();
            }
            acc *= level;
            l[i] += acc;
            r[i] += acc;
        }
    }

    void reset() override {
        for (auto& v : voices_) { v.aenv.reset(); v.fenv.reset(); v.held = false; v.serial = 0; v.id = 0; v.ex.clear(); v.lad.reset(); }
        nextSerial_ = 1;
        ctrlCount_ = 0;
        for (auto& s : s_) s.snap(s.target());
    }

private:
    static const float* aBuf(const ModInputs& m, size_t ordinal) { return ordinal < m.audioRate.size() ? m.audioRate[ordinal] : nullptr; }

    void applyAdsr() {
        for (auto& v : voices_) {
            v.aenv.setAdsr(ctl_[Attack], ctl_[Decay], ctl_[Sustain], ctl_[Release]);
            v.fenv.setAdsr(ctl_[FAttack], ctl_[FDecay], ctl_[FSustain], ctl_[FRelease]);
        }
    }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    std::array<Smoother, kNumP> s_;
    float ctl_[kNumP] = {};
    uint64_t nextSerial_ = 1;
    uint32_t ctrlCount_ = 0;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_subtractive() { return std::make_unique<Subtractive>(); }

}  // namespace ddaw::devices
