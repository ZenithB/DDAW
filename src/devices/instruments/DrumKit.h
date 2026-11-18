#pragma once
// The engine shared by the synthesized drum machines (ARCH 29): eight pads, each a small synthesis voice, with the same three controls
// per pad as the original `drum` (p{i}_tune in semitones, p{i}_decay in seconds, p{i}_level in dB), a pitch selecting the pad (pitch mod
// 8), per-pad sample override (the bank's samples replace a pad's synthesized sound, as on `drum`), and an optional "digital" stage
// (bit depth and sample-and-hold rate) for the sampled machines of the eighties. A machine file defines its eight voices from the
// building blocks below and hands the engine a parameter table and the voices; one device per file.
//
// Not a port: these are original synthesis models of how those machines sound (an analogue kick that is a sine with a falling pitch,
// a snare of two tones and noise, a hat of six detuned squares, a clap of noise bursts), not copies of any machine's circuit or samples.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <span>

#include "core/Device.h"
#include "core/Sample.h"
#include "dsp/Math.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

namespace ddaw::devices::drumkit {

constexpr int kPads = 8;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

struct Noise {
    uint32_t s = 0x9E3779B9u;
    float white() noexcept {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
};

// Exponential decay: reaches -60 dB after `seconds`.
struct Decay {
    float v = 0.0f, k = 0.0f;
    void trig(float seconds, float sr, float start = 1.0f) noexcept { v = start; k = std::exp(-6.9078f / (std::max(seconds, 0.002f) * sr)); }
    float tick() noexcept { const float o = v; v *= k; if (v < 1e-5f) v = 0.0f; return o; }
    bool on() const noexcept { return v > 1e-4f; }
    void kill() noexcept { v = 0.0f; }
};

// A sine whose pitch falls exponentially from f0 to f1 (an analogue drum's pitch envelope).
struct SweepSine {
    double ph = 0.0;
    float f = 0.0f, f1 = 0.0f, coef = 1.0f, sr = 44100.0f;
    void trig(float f0, float fEnd, float tau, float rate) noexcept { sr = rate; f = f0; f1 = fEnd; coef = std::exp(-1.0f / (std::max(tau, 0.0005f) * sr)); ph = 0.0; }
    float tick() noexcept {
        ph += kTwoPi * double(f) / double(sr);
        if (ph > kTwoPi) ph -= kTwoPi;
        f = f1 + (f - f1) * coef;
        return float(std::sin(ph));
    }
};

// Six detuned square oscillators summed: the metallic part of hats and cymbals.
struct Metal {
    static constexpr std::array<float, 6> kRatios = {205.3f, 304.4f, 369.4f, 522.7f, 540.0f, 800.0f};
    std::array<double, 6> ph{};
    float scale = 1.0f, sr = 44100.0f;
    void set(float freqScale, float rate) noexcept { scale = freqScale; sr = rate; }
    float tick() noexcept {
        float s = 0.0f;
        for (size_t i = 0; i < 6; ++i) {
            ph[i] += double(kRatios[i] * scale) / double(sr);
            if (ph[i] >= 1.0) ph[i] -= 1.0;
            s += ph[i] < 0.5 ? 1.0f : -1.0f;
        }
        return s * (1.0f / 6.0f);
    }
};

// One pad's voice. tick() is mono. `tune` is in semitones, `decay` is the pad's decay control in seconds, `vel` is 0..1.
class PadVoice {
public:
    virtual ~PadVoice() = default;
    virtual void prepare(float sr) = 0;
    virtual void trigger(float vel, float tune, float decay) = 0;
    virtual float tick() noexcept = 0;
    virtual bool active() const noexcept = 0;
    virtual void reset() = 0;
    virtual void seed(uint32_t) {}                        // the noise sequence a pad starts every reset() from (different per pad)
};

inline float semis(float s) noexcept { return std::exp2(s * (1.0f / 12.0f)); }

// ---- the voices ----

struct KickCfg { float f0, f1, pitchTau, click, clickHz, drive; };
class Kick final : public PadVoice {
public:
    explicit Kick(KickCfg c) : c_(c) {}
    void prepare(float sr) override { sr_ = sr; filt_.prepare(sr); }
    void trigger(float vel, float tune, float decay) override {
        const float m = semis(tune);
        osc_.trig(c_.f0 * m, c_.f1 * m, c_.pitchTau, sr_);
        amp_.trig(decay, sr_);
        click_.trig(0.006f, sr_);
        filt_.setCutoffQ(c_.clickHz, 0.9f);
        vel_ = vel;
    }
    float tick() noexcept override {
        if (!amp_.on()) { amp_.kill(); return 0.0f; }
        const float body = osc_.tick() * amp_.tick();
        const float click = filt_.processSample(rng_.white()) * click_.tick() * c_.click;
        const float x = (body + click) * c_.drive;
        return std::tanh(x) / std::tanh(c_.drive) * 0.9f * vel_;
    }
    bool active() const noexcept override { return amp_.on(); }
    void reset() override { amp_.kill(); click_.kill(); filt_.reset(); rng_.s = seed_; }
    void seed(uint32_t s) override { seed_ = rng_.s = s | 1u; }

private:
    uint32_t seed_ = 0x9E3779B9u;
    KickCfg c_;
    SweepSine osc_;
    Decay amp_, click_;
    Noise rng_;
    dsp::Svf filt_{dsp::SvfMode::Bandpass};
    float sr_ = 44100.0f, vel_ = 0.0f;
};

struct SnareCfg { float f1, f2, toneMix, toneDecay, noiseHz, noiseQ, noiseMix; dsp::SvfMode noiseMode; };
class Snare final : public PadVoice {
public:
    explicit Snare(SnareCfg c) : c_(c) {}
    void prepare(float sr) override { sr_ = sr; filt_.prepare(sr); filt_.setMode(c_.noiseMode); }
    void trigger(float vel, float tune, float decay) override {
        const float m = semis(tune);
        a_.trig(c_.f1 * m * 1.25f, c_.f1 * m, 0.012f, sr_);
        b_.trig(c_.f2 * m * 1.2f, c_.f2 * m, 0.012f, sr_);
        tone_.trig(std::min(c_.toneDecay, decay), sr_);
        noise_.trig(decay, sr_);
        filt_.setCutoffQ(c_.noiseHz * m, c_.noiseQ);
        vel_ = vel;
    }
    float tick() noexcept override {
        if (!noise_.on() && !tone_.on()) return 0.0f;
        const float t = (a_.tick() * 0.6f + b_.tick() * 0.4f) * tone_.tick() * c_.toneMix;
        float n = filt_.processSample(rng_.white());
        if (c_.noiseMode == dsp::SvfMode::Bandpass) n /= c_.noiseQ;
        return (t + n * noise_.tick() * c_.noiseMix) * 0.85f * vel_;
    }
    bool active() const noexcept override { return noise_.on() || tone_.on(); }
    void reset() override { tone_.kill(); noise_.kill(); filt_.reset(); rng_.s = seed_; }
    void seed(uint32_t s) override { seed_ = rng_.s = s | 1u; }

private:
    uint32_t seed_ = 0x9E3779B9u;
    SnareCfg c_;
    SweepSine a_, b_;
    Decay tone_, noise_;
    Noise rng_;
    dsp::Svf filt_;
    float sr_ = 44100.0f, vel_ = 0.0f;
};

// Noise bursts through a band-pass: a clap (a few quick bursts, then a tail) or a tambourine (higher, tighter).
struct ClapCfg { float hz, q, gap, burstDecay, tailFrac; int bursts; };
class Clap final : public PadVoice {
public:
    explicit Clap(ClapCfg c) : c_(c) {}
    void prepare(float sr) override { sr_ = sr; filt_.prepare(sr); }
    void trigger(float vel, float tune, float decay) override {
        filt_.setCutoffQ(c_.hz * semis(tune), c_.q);
        decay_ = decay;
        fired_ = 0;
        wait_ = 0;
        vel_ = vel;
        env_.kill();
    }
    float tick() noexcept override {
        if (fired_ < c_.bursts && wait_ == 0) {
            const bool last = fired_ == c_.bursts - 1;
            env_.trig(last ? std::max(decay_ * c_.tailFrac, 0.02f) : c_.burstDecay, sr_);
            ++fired_;
            wait_ = uint32_t(c_.gap * sr_);
        } else if (wait_ > 0) --wait_;
        if (!env_.on()) { if (fired_ >= c_.bursts) env_.kill(); return 0.0f; }
        return filt_.processSample(rng_.white()) / c_.q * env_.tick() * 0.95f * vel_;
    }
    bool active() const noexcept override { return env_.on() || fired_ < c_.bursts; }
    void reset() override { env_.kill(); fired_ = c_.bursts; filt_.reset(); rng_.s = seed_; }
    void seed(uint32_t s) override { seed_ = rng_.s = s | 1u; }

private:
    uint32_t seed_ = 0x9E3779B9u;
    ClapCfg c_;
    Decay env_;
    Noise rng_;
    dsp::Svf filt_{dsp::SvfMode::Bandpass};
    uint32_t wait_ = 0;
    int fired_ = 0;
    float decay_ = 0.2f, sr_ = 44100.0f, vel_ = 0.0f;
};

// Metal and noise through a high-pass: closed and open hats, cymbals.
struct HatCfg { float metalMix, noiseMix, hpHz, freqScale, bpHz; };
class Hat final : public PadVoice {
public:
    explicit Hat(HatCfg c) : c_(c) {}
    void prepare(float sr) override { sr_ = sr; hp_.prepare(sr); bp_.prepare(sr); }
    void trigger(float vel, float tune, float decay) override {
        const float m = semis(tune);
        metal_.set(c_.freqScale * m, sr_);
        hp_.setCutoffQ(c_.hpHz * m, 0.7f);
        bp_.setCutoffQ(c_.bpHz * m, 0.9f);
        env_.trig(decay, sr_);
        vel_ = vel;
    }
    float tick() noexcept override {
        if (!env_.on()) { env_.kill(); return 0.0f; }
        const float x = metal_.tick() * c_.metalMix + rng_.white() * c_.noiseMix;
        const float y = hp_.processSample(x) + bp_.processSample(x) * 0.5f;
        return y * env_.tick() * 0.8f * vel_;
    }
    bool active() const noexcept override { return env_.on(); }
    void reset() override { env_.kill(); hp_.reset(); bp_.reset(); rng_.s = seed_; metal_.ph.fill(0.0); }
    void seed(uint32_t s) override { seed_ = rng_.s = s | 1u; }

private:
    uint32_t seed_ = 0x9E3779B9u;
    HatCfg c_;
    Metal metal_;
    Decay env_;
    Noise rng_;
    dsp::Svf hp_{dsp::SvfMode::Highpass}, bp_{dsp::SvfMode::Bandpass};
    float sr_ = 44100.0f, vel_ = 0.0f;
};

struct TomCfg { float f0, f1, pitchTau, click; };
class Tom final : public PadVoice {
public:
    explicit Tom(TomCfg c) : c_(c) {}
    void prepare(float sr) override { sr_ = sr; }
    void trigger(float vel, float tune, float decay) override {
        const float m = semis(tune);
        osc_.trig(c_.f0 * m, c_.f1 * m, c_.pitchTau, sr_);
        amp_.trig(decay, sr_);
        click_.trig(0.004f, sr_);
        vel_ = vel;
    }
    float tick() noexcept override {
        if (!amp_.on()) { amp_.kill(); return 0.0f; }
        return (osc_.tick() * amp_.tick() + rng_.white() * click_.tick() * c_.click) * 0.9f * vel_;
    }
    bool active() const noexcept override { return amp_.on(); }
    void reset() override { amp_.kill(); click_.kill(); rng_.s = seed_; }
    void seed(uint32_t s) override { seed_ = rng_.s = s | 1u; }

private:
    uint32_t seed_ = 0x9E3779B9u;
    TomCfg c_;
    SweepSine osc_;
    Decay amp_, click_;
    Noise rng_;
    float sr_ = 44100.0f, vel_ = 0.0f;
};

// Two tones through a band-pass: a cowbell (two squares) or a rim shot (two short sines).
struct TwoToneCfg { float f1, f2, bpHz, bpQ; bool square; };
class TwoTone final : public PadVoice {
public:
    explicit TwoTone(TwoToneCfg c) : c_(c) {}
    void prepare(float sr) override { sr_ = sr; bp_.prepare(sr); }
    void trigger(float vel, float tune, float decay) override {
        const float m = semis(tune);
        s1_ = float(c_.f1 * m); s2_ = float(c_.f2 * m);
        bp_.setCutoffQ(c_.bpHz * m, c_.bpQ);
        env_.trig(decay, sr_);
        vel_ = vel;
    }
    float tick() noexcept override {
        if (!env_.on()) { env_.kill(); return 0.0f; }
        p1_ += double(s1_) / double(sr_); p2_ += double(s2_) / double(sr_);
        p1_ -= std::floor(p1_); p2_ -= std::floor(p2_);
        const float a = c_.square ? (p1_ < 0.5 ? 1.0f : -1.0f) : float(std::sin(kTwoPi * p1_));
        const float b = c_.square ? (p2_ < 0.5 ? 1.0f : -1.0f) : float(std::sin(kTwoPi * p2_));
        return bp_.processSample((a + b) * 0.5f) / c_.bpQ * env_.tick() * 1.6f * vel_;
    }
    bool active() const noexcept override { return env_.on(); }
    void reset() override { env_.kill(); bp_.reset(); p1_ = p2_ = 0.0; }

private:
    TwoToneCfg c_;
    Decay env_;
    dsp::Svf bp_{dsp::SvfMode::Bandpass};
    double p1_ = 0.0, p2_ = 0.0;
    float s1_ = 0.0f, s2_ = 0.0f, sr_ = 44100.0f, vel_ = 0.0f;
};

// ---- the engine ----

struct Digital { int bits = 0; float holdHz = 0.0f; };       // bits 0: off (a clean analogue machine)

class Machine final : public InstrumentDevice {
public:
    using Factory = std::unique_ptr<PadVoice> (*)();
    Machine(std::span<const ParamSpec> table, const std::array<Factory, kPads>& voices, Digital digital = {}) : table_(table), digital_(digital) {
        for (size_t i = 0; i < pads_.size(); ++i) {
            pads_[i].voice = voices[i]();
            pads_[i].voice->seed(0x9E3779B9u * uint32_t(i + 1));
            pads_[i].decay = table_[i * 3 + 1].def;
            pads_[i].levelSm.prepare(sr_, 15.0f);
            pads_[i].levelSm.snap(1.0f);
        }
    }
    std::span<const ParamSpec> params() const override { return table_; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 1.0));
        for (auto& p : pads_) { p.voice->prepare(sr_); p.levelSm.prepare(sr_, 15.0f); p.levelSm.snap(p.levelSm.target()); p.voice->reset(); p.sampleActive = false; }
        holdStep_ = digital_.holdHz > 0.0f ? digital_.holdHz / sr_ : 1.0f;
    }
    void setSample(uint32_t slot, SamplePtr buf) override {
        if (slot >= pads_.size()) return;
        pads_[slot].sample = std::move(buf);
        pads_[slot].voice->reset();
        pads_[slot].sampleActive = false;
    }
    void setParam(uint16_t index, float v) override {
        const size_t i = index / 3;
        if (i >= pads_.size()) return;
        Pad& p = pads_[i];
        switch (index % 3) {
            case 0: p.tune = std::clamp(v, -12.0f, 12.0f); break;
            case 1: p.decay = std::clamp(v, 0.03f, 2.0f); break;
            default: p.levelSm.setTarget(dsp::dbToLin(std::clamp(v, -24.0f, 6.0f))); break;
        }
    }
    void noteOn(uint8_t pitch, float velocity, uint32_t) override {
        Pad& p = pads_[pitch % kPads];
        const float vel = std::clamp(velocity, 0.0f, 1.0f);
        if (const SampleBuf* b = p.sample.get()) {                     // a loaded sample plays instead of the synthesized sound
            p.sampleActive = true;
            p.pos = 0.0;
            p.rate = double(b->sampleRate) / double(sr_) * std::pow(2.0, double(p.tune) / 12.0);
            p.sampleVel = vel;
            return;
        }
        p.voice->trigger(vel, p.tune, p.decay);
    }
    void noteOff(uint32_t) override {}                                 // one-shots ring out

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int i = 0; i < n; ++i) {
            float mixL = 0.0f, mixR = 0.0f;
            for (auto& p : pads_) {
                const float g = p.levelSm.next();
                if (p.sampleActive) {
                    const SampleBuf* b = p.sample.get();
                    if (!b || p.pos >= double(b->frames())) { p.sampleActive = false; continue; }
                    float sl, sr2;
                    b->readLin(p.pos, sl, sr2);
                    p.pos += p.rate;
                    mixL += sl * g * p.sampleVel; mixR += sr2 * g * p.sampleVel;
                    continue;
                }
                const float s = p.voice->tick() * g;
                mixL += s; mixR += s;
            }
            if (digital_.bits > 0) {                                  // the sampled machines: coarse amplitude and a held sample rate
                holdPhase_ += holdStep_;
                if (holdPhase_ >= 1.0f) { holdPhase_ -= 1.0f; const float q = float(1 << (digital_.bits - 1)); heldL_ = std::round(std::clamp(mixL, -1.0f, 1.0f) * q) / q; heldR_ = std::round(std::clamp(mixR, -1.0f, 1.0f) * q) / q; }
                mixL = heldL_; mixR = heldR_;
            }
            l[i] += mixL; r[i] += mixR;
        }
    }
    void reset() override {
        for (auto& p : pads_) { p.voice->reset(); p.levelSm.snap(p.levelSm.target()); p.sampleActive = false; }
        holdPhase_ = 0.0f; heldL_ = heldR_ = 0.0f;
    }

private:
    struct Pad {
        std::unique_ptr<PadVoice> voice;
        float tune = 0.0f, decay = 0.3f, sampleVel = 1.0f;
        dsp::Smoother levelSm;
        SamplePtr sample;
        bool sampleActive = false;
        double pos = 0.0, rate = 1.0;
    };
    std::span<const ParamSpec> table_;
    Digital digital_;
    std::array<Pad, kPads> pads_;
    float sr_ = 44100.0f, holdStep_ = 1.0f, holdPhase_ = 0.0f, heldL_ = 0.0f, heldR_ = 0.0f;
};

}  // namespace ddaw::devices::drumkit
