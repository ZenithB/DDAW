// `perc` instrument (B6): a hybrid percussion voice - the transient-plus-noise-plus-sinusoids decomposition of drum sounds,
// with every part a plain control:
//   body    a sine whose pitch starts `sweep` octaves above `tune` and glides down (time constant sweepTime), decaying over
//           bodyDecay seconds (T60); `track` says how much the note moves it (1: a chromatic tom, 0: the same drum on every key)
//   noise   white noise through a low-, band- or high-pass (noiseHz, noiseQ, noiseMode), decaying over noiseDecay seconds;
//           `clap` re-strikes it that many extra times, 9 ms apart, each strike but the last dying in 10 ms
//   metal   the classic cluster of six square oscillators (205 to 800 Hz, scaled by metalTune and the note) through a 6 kHz
//           high-pass - cymbals and hats
//   click   a 2 ms burst of noise for the beater
//   drive   a soft clipper over the sum
// Kick, snare, toms, hats, claps and cowbell-ish clangs are settings of one voice. Velocity scales the level and opens the noise
// filter; pressure adds up to 50% level, slide opens the noise filter by up to two octaves, bend moves the body and the metal.
// With gate on, a note-off damps the voice in `release` seconds; with it off the hit rings out. Eight voices; each ends (and
// stops costing anything) when every part is below -80 dB.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/SchemaB6.h"
#include "dsp/FastSin.h"
#include "dsp/Math.h"
#include "dsp/NoteExpr.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"
#include "dsp/VoicePool.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Tune, Track, Sweep, SweepTime, Body, BodyDecay, Noise, NoiseMode, NoiseHz, NoiseQ, NoiseDecay, Clap, Metal, MetalTune, MetalDecay, Click, Drive, Gate, Release, Level, kNumP };

constexpr int kVoices = 8, kCtrl = 16;
constexpr float kT60Ln = 6.9077553f;
constexpr float kMetalHz[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};

inline float sat(float x) noexcept {
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
inline float polyBlepSquare(float ph, float dt) noexcept {
    auto blep = [](float t, float d) { if (t < d) { const float x = t / d; return x + x - x * x - 1.0f; } if (t > 1.0f - d) { const float x = (t - 1.0f) / d; return x * x + x + x + 1.0f; } return 0.0f; };
    float t2 = ph + 0.5f; t2 -= std::floor(t2);
    return (ph < 0.5f ? 1.0f : -1.0f) + blep(ph, dt) - blep(t2, dt);
}

struct Voice {
    float bodyPh = 0.0f, sweepEnv = 1.0f, bodyEnv = 1.0f;
    float noiseEnv = 1.0f, noiseK = 0.99f, metalEnv = 1.0f, clickEnv = 1.0f, relGain = 1.0f;
    std::array<float, 6> metalPh{};
    Svf noiseF{SvfMode::Bandpass}, metalF{SvfMode::Highpass};
    NoteExpr ex;
    int t = 0, burstsLeft = 0, nextBurst = 0;
    uint32_t rng = 1;
    float vel = 1.0f;
    uint8_t pitch = 60;
    bool held = false, alive = false, releasing = false;
    uint64_t serial = 0;
    uint32_t id = 0;
    bool active() const { return alive; }
};

class Perc final : public InstrumentDevice {
public:
    Perc() {
        for (const auto& ps : schema::kInstPerc) setParam(ps.index, ps.def);
        for (auto& s : s_) s.snap(s.target());
    }

    std::span<const ParamSpec> params() const override { return schema::kInstPerc; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 8000.0));
        for (auto& s : s_) s.prepare(sr_, 15.0f);
        for (auto& v : voices_) { v.noiseF.prepare(sr_); v.metalF.prepare(sr_); v.metalF.setCutoffQ(6000.0f, 0.7071f); v.alive = false; v.held = false; }
        for (int i = 0; i < kNumP; ++i) setParam(uint16_t(i), ctl_[i]);
        for (auto& s : s_) s.snap(s.target());
    }

    void setParam(uint16_t i, float v) override {
        if (i >= kNumP) return;
        const ParamSpec& ps = schema::kInstPerc[i];
        v = std::clamp(v, ps.min, ps.max);
        ctl_[i] = v;
        s_[i].setTarget(v);
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        Voice& v = voices_[size_t(pickVoice(voices_, pitch))];
        v.bodyPh = 0.0f; v.sweepEnv = 1.0f; v.bodyEnv = 1.0f; v.noiseEnv = 1.0f; v.metalEnv = 1.0f; v.clickEnv = 1.0f; v.relGain = 1.0f;
        for (int k = 0; k < 6; ++k) v.metalPh[size_t(k)] = 0.13f * float(k);
        v.noiseF.reset(); v.metalF.reset();
        v.metalF.setCutoffQ(6000.0f, 0.7071f);
        v.ex.clear();
        v.t = 0;
        v.burstsLeft = int(std::lround(ctl_[Clap]));
        v.nextBurst = int(0.009f * sr_);
        v.noiseK = std::exp(-kT60Ln / (std::max(v.burstsLeft > 0 ? 0.01f : ctl_[NoiseDecay], 0.001f) * sr_));
        v.pitch = pitch;
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.serial = nextSerial_++;
        v.id = noteId;
        v.held = true; v.alive = true; v.releasing = false;
        v.rng = 0x27D4EB2Fu * (noteId + 1u) + 4242u;
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_) {
            if (!v.held || v.id != noteId) continue;
            v.held = false;
            if (ctl_[Gate] >= 0.5f) v.releasing = true;
        }
    }

    void noteExpression(uint32_t noteId, int dimension, float value) override {
        for (auto& v : voices_) if (v.alive && v.id == noteId) v.ex.set(dimension, value);
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int done = 0; done < n; done += kMaxBlock) block(l + done, r + done, std::min(kMaxBlock, n - done));
    }

    void reset() override {
        for (auto& v : voices_) { v.alive = false; v.held = false; v.releasing = false; v.serial = 0; v.id = 0; v.ex.clear(); v.noiseF.reset(); v.metalF.reset(); }
        nextSerial_ = 1;
        for (auto& s : s_) s.snap(s.target());
    }

private:
    void block(float* l, float* r, int n) {
        const float invSr = 1.0f / sr_;
        const float bodyK = std::exp(-kT60Ln / (ctl_[BodyDecay] * sr_));
        const float sweepK = std::exp(-1.0f / (ctl_[SweepTime] * sr_));
        const float metalK = std::exp(-kT60Ln / (ctl_[MetalDecay] * sr_));
        const float clickK = std::exp(-1.0f / (0.0007f * sr_));
        const float relK = std::exp(-kT60Ln / (ctl_[Release] * sr_));
        const float noiseDecayK = std::exp(-kT60Ln / (ctl_[NoiseDecay] * sr_));
        const float strikeK = std::exp(-kT60Ln / (0.01f * sr_));
        const int mode = int(std::lround(ctl_[NoiseMode]));
        float tune[kMaxBlock], track[kMaxBlock], sweep[kMaxBlock], body[kMaxBlock], noise[kMaxBlock], nHz[kMaxBlock], nQ[kMaxBlock], metal[kMaxBlock], mTune[kMaxBlock], click[kMaxBlock], drive[kMaxBlock], level[kMaxBlock];
        for (int i = 0; i < n; ++i) {
            tune[i] = s_[Tune].next(); track[i] = s_[Track].next(); sweep[i] = s_[Sweep].next(); body[i] = s_[Body].next();
            noise[i] = s_[Noise].next(); nHz[i] = s_[NoiseHz].next(); nQ[i] = s_[NoiseQ].next(); metal[i] = s_[Metal].next(); mTune[i] = s_[MetalTune].next();
            click[i] = s_[Click].next(); drive[i] = s_[Drive].next(); level[i] = s_[Level].next();
        }
        for (auto& v : voices_) {
            if (!v.alive) continue;
            const float pitchRatio = std::exp2((float(v.pitch) - 60.0f) * (1.0f / 12.0f));
            float quiet = 0.0f;
            for (int i = 0; i < n; ++i) {
                v.ex.step();
                const float pf = v.ex.pitchFactor();
                // body
                const float trk = std::pow(pitchRatio, track[i]);
                const float f0 = std::min(tune[i] * trk * std::exp2(sweep[i] * v.sweepEnv) * pf, 0.45f * sr_);
                v.bodyPh += f0 * invSr; v.bodyPh -= std::floor(v.bodyPh);
                const float b = fastSin2Pi(v.bodyPh) * v.bodyEnv * body[i];
                v.sweepEnv = v.sweepEnv < 1e-6f ? 0.0f : v.sweepEnv * sweepK;
                v.bodyEnv *= bodyK;
                // noise (with clap re-strikes)
                if (v.burstsLeft > 0 && v.t >= v.nextBurst) {
                    --v.burstsLeft;
                    v.nextBurst += int(0.009f * sr_);
                    v.noiseEnv = 1.0f;
                    v.noiseK = v.burstsLeft > 0 ? strikeK : noiseDecayK;
                }
                if ((v.t & (kCtrl - 1)) == 0) {
                    const float cut = nHz[i] * (0.6f + 0.8f * v.vel) * std::exp2(2.0f * v.ex.slideS);
                    v.noiseF.setMode(mode == 0 ? SvfMode::Lowpass : mode == 1 ? SvfMode::Bandpass : SvfMode::Highpass);
                    v.noiseF.setCutoffQ(std::clamp(cut, 60.0f, 0.45f * sr_), nQ[i]);
                }
                v.rng ^= v.rng << 13; v.rng ^= v.rng >> 17; v.rng ^= v.rng << 5;
                const float w = float(int32_t(v.rng)) * (1.0f / 2147483648.0f);
                float nz = v.noiseF.processSample(w);
                if (mode == 1) nz *= 1.0f / nQ[i];
                nz *= v.noiseEnv * noise[i];
                v.noiseEnv *= v.noiseK;
                // metal
                float m = 0.0f;
                if (metal[i] > 0.0f) {
                    const float base = mTune[i] * trk * pf * invSr;
                    for (int k = 0; k < 6; ++k) {
                        const float inc = std::min(kMetalHz[k] * base, 0.45f);
                        m += polyBlepSquare(v.metalPh[size_t(k)], std::max(inc, 1e-9f));
                        v.metalPh[size_t(k)] += inc; v.metalPh[size_t(k)] -= v.metalPh[size_t(k)] >= 1.0f ? 1.0f : 0.0f;
                    }
                    m = v.metalF.processSample(m * (1.0f / 6.0f)) * v.metalEnv * metal[i] * 3.0f;   // little of a square cluster's energy is above 6 kHz: make it up
                }
                v.metalEnv *= metalK;
                // click
                const float c = click[i] > 0.0f ? w * v.clickEnv * click[i] : 0.0f;
                v.clickEnv *= clickK;
                float y = b + nz + m + c;
                if (drive[i] > 1e-3f) {
                    const float g = 1.0f + 8.0f * drive[i];
                    y += drive[i] * (sat(g * y) / sat(g) - y);
                }
                if (v.releasing) v.relGain *= relK;
                const float o = y * v.vel * v.ex.gain() * v.relGain * level[i];
                l[i] += o; r[i] += o;
                ++v.t;
            }
            // finished: every part below -80 dB, no strikes left (or damped away)
            quiet = std::max({v.bodyEnv * ctl_[Body], v.noiseEnv * ctl_[Noise], v.metalEnv * ctl_[Metal], v.clickEnv * ctl_[Click]});
            if ((v.burstsLeft == 0 && quiet < 1e-4f) || v.relGain < 1e-4f) v.alive = false;
        }
    }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    std::array<Smoother, kNumP> s_;
    float ctl_[kNumP] = {};
    uint64_t nextSerial_ = 1;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_perc() { return std::make_unique<Perc>(); }

}  // namespace ddaw::devices
