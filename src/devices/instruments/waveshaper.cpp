// `waveshaper` instrument (B6): an oscillator (sine, triangle or band-limited saw) driven through a transfer function. The
// signal is u = drive * x + bias; shapes: 0 soft clip, 1 sine fold, 2 Chebyshev (T1 + h2 T2 + ... + h5 T5 of u clamped to
// +-1, so h_k puts exactly that share of the k-th harmonic into a full-amplitude sine), 3 diode (asymmetric soft clip), 4 hard
// clip, 5 a drawn curve through five heights (cubic Hermite). Soft, fold, diode and clip are scaled so a note keeps its level
// as the drive falls, and the drive itself falls over each note (dDecay to dSus) so the bright start settles, scaled by
// velocity. Everything is generated at four times the host rate and decimated once on the summed voices (the same 47/15-tap
// halfbands as fmop, whose delay is reported as latency), so the shapers' harmonics above the Nyquist do not fold back; a
// 3.5 Hz DC blocker takes the offset that bias and the diode make. A-rate ports: drive 0, bias 1, pitch 2. Eight voices.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/SchemaB6.h"
#include "dsp/Adsr.h"
#include "dsp/Decimator4.h"
#include "dsp/FastSin.h"
#include "dsp/Math.h"
#include "dsp/NoteExpr.h"
#include "dsp/Smoother.h"
#include "dsp/VoicePool.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Src, Shape, Drive, Bias, H2, H3, H4, H5, K1, K2, K3, K4, K5, DDecay, DSus, VelDrive, Attack, Decay, Sustain, Release, Pitch, Level, kNumP };
enum A : size_t { ADrive = 0, ABias = 1, APitch = 2 };

constexpr int kVoices = 8, kOs = 4;
constexpr float kOut = 0.5f;

inline float sat(float x) noexcept {   // soft clip, monotone, exactly +-1 at +-3
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
inline float polyBlep(float t, float dt) noexcept {
    if (t < dt) { const float x = t / dt; return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { const float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}

struct Voice {
    Adsr env;
    NoteExpr ex;
    float ph = 0.0f, dEnv = 1.0f;
    float freq = 440.0f, vel = 1.0f;
    uint8_t pitch = 69;
    bool held = false;
    uint64_t serial = 0;
    uint32_t id = 0;
    bool active() const { return env.isActive(); }
};

// Everything the shaper needs that does not depend on the voice.
struct Shaper {
    int shape = 0;
    float h[4] = {0, 0, 0, 0}, hNorm = 1.0f;
    float k[5] = {-0.9f, -0.5f, 0.0f, 0.5f, 0.9f};

    float curve(float u) const noexcept {   // cubic Hermite through k at -1, -.5, 0, .5, 1 with finite-difference slopes
        u = std::clamp(u, -1.0f, 1.0f);
        const float p = (u + 1.0f) * 2.0f;
        const int i = std::min(int(p), 3);
        const float t = p - float(i);
        const float y0 = k[i], y1 = k[i + 1];
        const float m0 = i == 0 ? (k[1] - k[0]) : 0.5f * (k[i + 1] - k[i - 1]);
        const float m1 = i == 3 ? (k[4] - k[3]) : 0.5f * (k[i + 2] - k[i]);
        const float t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * y1 + (t3 - t2) * m1;
    }
    float operator()(float u, float norm) const noexcept {
        switch (shape) {
            case 0: return norm * sat(u);
            case 1: return norm * fastSin2Pi(u * 0.25f);                          // sin(pi/2 * u)
            case 2: {
                const float x = std::clamp(u, -1.0f, 1.0f), x2 = 2.0f * x;
                const float t2 = x2 * x - 1.0f, t3 = x2 * t2 - x, t4 = x2 * t3 - t2, t5 = x2 * t4 - t3;
                return (x + h[0] * t2 + h[1] * t3 + h[2] * t4 + h[3] * t5) * hNorm;
            }
            case 3: return norm * (u > 0.0f ? sat(u) : 0.35f * sat(1.6f * u));
            case 4: return norm * std::clamp(u, -1.0f, 1.0f);
            default: return curve(u);
        }
    }
};

class Waveshaper final : public InstrumentDevice {
public:
    Waveshaper() {
        for (const auto& ps : schema::kInstWaveshaper) setParam(ps.index, ps.def);
        for (auto& s : s_) s.snap(s.target());
    }

    std::span<const ParamSpec> params() const override { return schema::kInstWaveshaper; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 8000.0));
        for (auto& s : s_) s.prepare(sr_, 15.0f);
        for (auto& v : voices_) { v.env.prepare(sr_); v.env.reset(); v.held = false; }
        applyAdsr();
        dcK_ = 1.0f - 6.2831853f * 3.5f / sr_;
        for (int i = 0; i < kNumP; ++i) setParam(uint16_t(i), ctl_[i]);
        for (auto& s : s_) s.snap(s.target());
    }

    void setParam(uint16_t i, float v) override {
        if (i >= kNumP) return;
        const ParamSpec& ps = schema::kInstWaveshaper[i];
        v = std::clamp(v, ps.min, ps.max);
        ctl_[i] = v;
        s_[i].setTarget(v);
        if (i >= Attack && i <= Release) applyAdsr();
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        Voice& v = voices_[size_t(pickVoice(voices_, pitch))];
        v.ph = 0.0f;
        v.dEnv = 1.0f;
        v.ex.clear();
        v.pitch = pitch;
        v.freq = midiHz(float(pitch));
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.serial = nextSerial_++;
        v.id = noteId;
        v.held = true;
        v.env.reset();
        v.env.noteOn();
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_) if (v.held && v.id == noteId) { v.held = false; v.env.noteOff(); }
    }

    // MPE: slide drives the shaper harder (up to +4), pressure raises the level by up to 50%, bend is in semitones.
    void noteExpression(uint32_t noteId, int dimension, float value) override {
        for (auto& v : voices_) if (v.active() && v.id == noteId) v.ex.set(dimension, value);
    }

    int latencySamples() const override { return dec_.latency(); }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs& mod) override {
        const float* aDrive = aBuf(mod, ADrive);
        const float* aBias = aBuf(mod, ABias);
        const float* aPit = aBuf(mod, APitch);
        const int src = int(std::lround(ctl_[Src]));
        Shaper sh;
        sh.shape = int(std::lround(ctl_[Shape]));
        const float hs = ctl_[H2] + ctl_[H3] + ctl_[H4] + ctl_[H5];
        sh.h[0] = ctl_[H2]; sh.h[1] = ctl_[H3]; sh.h[2] = ctl_[H4]; sh.h[3] = ctl_[H5];
        sh.hNorm = 1.0f / (1.0f + hs);
        for (int j = 0; j < 5; ++j) sh.k[j] = ctl_[K1 + j];
        const float dK = std::exp(-1.0f / (std::max(ctl_[DDecay], 0.001f) * sr_ * 0.3f));
        const float dSus = ctl_[DSus], velDrive = ctl_[VelDrive];
        const float invOs = 1.0f / (float(kOs) * sr_);
        for (int i = 0; i < n; ++i) {
            const float driveBase = s_[Drive].next(), biasBase = s_[Bias].next(), pitch = s_[Pitch].next(), level = s_[Level].next();
            const float driveNow = std::clamp(driveBase + (aDrive ? aDrive[i] : 0.0f), 0.0f, 12.0f);
            const float biasNow = std::clamp(biasBase + (aBias ? aBias[i] : 0.0f), -2.0f, 2.0f);
            const float semiNow = pitch + (aPit ? aPit[i] : 0.0f);
            if (!primed_) { drivePrev_ = driveNow; biasPrev_ = biasNow; semiPrev_ = semiNow; primed_ = true; }
            float hi[kOs] = {0, 0, 0, 0};
            for (auto& v : voices_) {
                if (!v.active()) continue;
                v.ex.step();
                const float a = v.env.next();
                v.dEnv = dSus + (v.dEnv - dSus) * dK;
                if (v.dEnv < 1e-6f) v.dEnv = 0.0f;
                const float velScale = 1.0f - velDrive * (1.0f - v.vel);
                const float gain = a * v.vel * kOut * v.ex.gain();
                const float dScale = v.dEnv * velScale;
                for (int j = 0; j < kOs; ++j) {
                    const float t = float(j + 1) * (1.0f / float(kOs));
                    const float d = (drivePrev_ + (driveNow - drivePrev_) * t) * dScale + 4.0f * v.ex.slideS;
                    const float bias = biasPrev_ + (biasNow - biasPrev_) * t;
                    const float semi = semiPrev_ + (semiNow - semiPrev_) * t;
                    const float pf = (semi == 0.0f ? 1.0f : std::exp2(semi * (1.0f / 12.0f))) * v.ex.pitchFactor();
                    const float inc = v.freq * pf * invOs;
                    float x;
                    switch (src) {
                        case 0: x = fastSin2Pi(v.ph); break;
                        case 1: x = v.ph < 0.25f ? 4.0f * v.ph : (v.ph < 0.75f ? 2.0f - 4.0f * v.ph : 4.0f * v.ph - 4.0f); break;
                        default: x = 2.0f * v.ph - 1.0f - polyBlep(v.ph, std::max(inc, 1e-9f)); break;
                    }
                    v.ph += inc;
                    v.ph -= v.ph >= 1.0f ? 1.0f : 0.0f;
                    // the level a plain oscillator would have: keeps a note's loudness as the drive falls
                    const float norm = sh.shape == 1 ? 1.0f / std::min(1.0f, 1.5708f * std::max(d, 0.02f))
                                       : sh.shape == 4 ? 1.0f / std::min(1.0f, std::max(d, 0.02f))
                                       : 1.0f / std::max(sat(std::max(d, 0.02f)), 0.02f);
                    hi[j] += gain * sh(x * d + bias, norm);
                }
            }
            drivePrev_ = driveNow; biasPrev_ = biasNow; semiPrev_ = semiNow;
            const float s = dec_.push(hi[0], hi[1], hi[2], hi[3]);
            const float o = (s - dcX_ + dcK_ * dcY_);
            dcX_ = s; dcY_ = o;
            l[i] += o * level;
            r[i] += o * level;
        }
    }

    void reset() override {
        for (auto& v : voices_) { v.env.reset(); v.held = false; v.serial = 0; v.id = 0; v.ex.clear(); }
        nextSerial_ = 1;
        dec_.reset();
        dcX_ = dcY_ = 0.0f;
        primed_ = false;
        for (auto& s : s_) s.snap(s.target());
    }

private:
    static const float* aBuf(const ModInputs& m, size_t ordinal) { return ordinal < m.audioRate.size() ? m.audioRate[ordinal] : nullptr; }
    void applyAdsr() { for (auto& v : voices_) v.env.setAdsr(ctl_[Attack], ctl_[Decay], ctl_[Sustain], ctl_[Release]); }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    std::array<Smoother, kNumP> s_;
    float ctl_[kNumP] = {};
    Decimator4 dec_{47, 15};
    float dcK_ = 0.9995f, dcX_ = 0.0f, dcY_ = 0.0f;
    float drivePrev_ = 0.0f, biasPrev_ = 0.0f, semiPrev_ = 0.0f;
    bool primed_ = false;
    uint64_t nextSerial_ = 1;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_waveshaper() { return std::make_unique<Waveshaper>(); }

}  // namespace ddaw::devices
