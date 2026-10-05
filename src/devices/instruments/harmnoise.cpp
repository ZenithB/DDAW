// `harmnoise` instrument (B6): the DDSP signal model - a bank of harmonic partials plus filtered noise - controlled by hand
// instead of by a network. Every partial's level comes from a handful of smooth controls (spectral tilt, odd/even balance,
// a Gaussian formant bump on a log-frequency axis, a partial count with a soft edge) and the partial frequencies from an
// inharmonic stretch f_k = k f0 sqrt(1 + B k^2). Partials that would pass the Nyquist are faded out per block (exact masking,
// as in the neural synth), so a high note never folds back. The noise is white noise through a band-pass whose level is
// normalised, so `noise` means the same loudness whatever the colour. Loudness is normalised too: moving the tilt changes
// the timbre, not the level. Fixed pool of 8 voices.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/SchemaB6.h"
#include "dsp/Adsr.h"
#include "dsp/FastSin.h"
#include "dsp/Math.h"
#include "dsp/NoteExpr.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"
#include "dsp/VoicePool.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Harmonics, Tilt, OddEven, Stretch, Formant, FWidth, FGain, Noise, NoiseHz, NoiseQ, NoiseTrack, Chiff, VibRate, VibAmt, Attack, Decay, Sustain, Release, Level, kNumP };

constexpr int kVoices = 8;
constexpr int kMaxH = 48;
constexpr float kPartialRms = 0.35f;     // the loudness of a full-level note before velocity and level

struct Voice {
    Adsr env;
    NoteExpr ex;
    Svf noiseFilt{SvfMode::Bandpass};
    std::array<float, kMaxH> ph{}, g{}, dg{};
    float freq = 440.0f, vel = 1.0f, vibPh = 0.0f, chiffEnv = 0.0f, nGain = 0.0f;
    uint32_t rng = 1;
    uint8_t pitch = 69;
    bool held = false, fresh = true;
    uint64_t serial = 0;
    uint32_t id = 0;
    bool active() const { return env.isActive(); }
};

class HarmNoise final : public InstrumentDevice {
public:
    HarmNoise() {
        for (const auto& ps : schema::kInstHarmnoise) setParam(ps.index, ps.def);
        for (auto& s : s_) s.snap(s.target());
    }

    std::span<const ParamSpec> params() const override { return schema::kInstHarmnoise; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 8000.0));
        for (auto& s : s_) s.prepare(sr_, 15.0f);
        for (auto& v : voices_) { v.env.prepare(sr_); v.noiseFilt.prepare(sr_); v.env.reset(); v.held = false; }
        applyAdsr();
        for (int i = 0; i < kNumP; ++i) setParam(uint16_t(i), ctl_[i]);
        for (auto& s : s_) s.snap(s.target());
    }

    void setParam(uint16_t i, float v) override {
        if (i >= kNumP) return;
        const ParamSpec& ps = schema::kInstHarmnoise[i];
        v = std::clamp(v, ps.min, ps.max);
        ctl_[i] = v;
        s_[i].setTarget(v);
        if (i >= Attack && i <= Release) applyAdsr();
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        const int idx = pickVoice(voices_, pitch);
        Voice& v = voices_[size_t(idx)];
        v.ph.fill(0.0f); v.g.fill(0.0f); v.dg.fill(0.0f);
        v.noiseFilt.reset();
        v.ex.clear();
        v.pitch = pitch;
        v.freq = midiHz(float(pitch));
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.serial = nextSerial_++;
        v.id = noteId;
        v.held = true;
        v.fresh = true;
        v.vibPh = 0.0f;
        v.chiffEnv = 1.0f;
        v.rng = 0x9E3779B9u * (noteId + 1u) + 12345u;
        v.env.reset();
        v.env.noteOn();
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_) if (v.held && v.id == noteId) { v.held = false; v.env.noteOff(); }
    }

    // MPE: slide brightens (the tilt rises by up to 8 dB/octave), pressure raises the level by up to 50%, bend is in semitones.
    void noteExpression(uint32_t noteId, int dimension, float value) override {
        for (auto& v : voices_) if (v.active() && v.id == noteId) v.ex.set(dimension, value);
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int done = 0; done < n; done += kMaxBlock) block(l + done, r + done, std::min(kMaxBlock, n - done));
    }

    void reset() override {
        for (auto& v : voices_) { v.env.reset(); v.held = false; v.serial = 0; v.id = 0; v.ex.clear(); v.noiseFilt.reset(); v.ph.fill(0.0f); v.g.fill(0.0f); }
        nextSerial_ = 1;
        for (auto& s : s_) s.snap(s.target());
    }

private:
    static float adv(Smoother& s, int n) { for (int i = 0; i < n; ++i) s.next(); return s.current(); }

    void applyAdsr() {
        for (auto& v : voices_) v.env.setAdsr(ctl_[Attack], ctl_[Decay], ctl_[Sustain], ctl_[Release]);
    }

    void block(float* l, float* r, int n) {
        // block-rate controls: the smoothed value at the end of the block
        const float harm = adv(s_[Harmonics], n), tilt = adv(s_[Tilt], n), oddEven = adv(s_[OddEven], n), B = adv(s_[Stretch], n);
        const float formant = adv(s_[Formant], n), fWidth = adv(s_[FWidth], n), fGain = adv(s_[FGain], n);
        const float noise = adv(s_[Noise], n), noiseHz = adv(s_[NoiseHz], n), noiseQ = adv(s_[NoiseQ], n), track = adv(s_[NoiseTrack], n);
        const float chiff = adv(s_[Chiff], n), vibRate = adv(s_[VibRate], n), vibAmt = adv(s_[VibAmt], n);
        float lvl[kMaxBlock];
        for (int i = 0; i < n; ++i) lvl[i] = s_[Level].next();

        const int top = std::min(kMaxH, int(std::ceil(harm)));
        float ratio[kMaxH + 1];
        for (int k = 1; k <= kMaxH; ++k) ratio[k] = float(k) * std::sqrt(1.0f + B * float(k) * float(k));
        const float vibMax = std::exp2(vibAmt * (1.0f / 1200.0f));
        const float invSr = 1.0f / sr_;
        const float chiffK = std::exp(-1.0f / (0.05f * sr_));
        const float vibInc = vibRate * invSr;
        const float bumpPeak = std::pow(10.0f, fGain * 0.05f);

        for (auto& v : voices_) {
            if (!v.active()) continue;
            // partial gains for the end of this block
            const float f0 = v.freq * v.ex.pitchFactor();
            const float tiltEff = std::min(tilt + 8.0f * v.ex.slideS, 6.0f);
            const float slopeExp = tiltEff * (1.0f / 6.0206f);                  // amplitude ~ k^slopeExp
            float amp[kMaxH + 1], sumSq = 0.0f;
            for (int k = 1; k <= top; ++k) {
                float a = std::pow(float(k), slopeExp);
                a *= (k & 1) ? (oddEven < 0.0f ? 1.0f + oddEven : 1.0f) : (oddEven > 0.0f ? 1.0f - oddEven : 1.0f);
                if (fGain > 0.0f) {
                    const float x = std::log2(std::max(ratio[k] * f0, 1.0f) / formant) / fWidth;
                    a *= 1.0f + (bumpPeak - 1.0f) * std::exp(-0.5f * x * x);
                }
                a *= std::clamp(harm - float(k) + 1.0f, 0.0f, 1.0f);              // soft edge on the last partial
                amp[k] = a;
                sumSq += a * a;
            }
            const float norm = sumSq > 1e-12f ? kPartialRms * 1.41421356f / std::sqrt(sumSq) : 0.0f;
            const float fMax = f0 * vibMax * invSr;
            float tgt[kMaxH];
            int act = 0;   // partials that sound this block: the ones with a target and the ones still fading out
            for (int k = 1; k <= kMaxH; ++k) {
                float t = 0.0f;
                if (k <= top) {
                    // fade from 0.42 to 0.47 of the sample rate; nothing is synthesised above it
                    const float mask = std::clamp((0.47f - ratio[k] * fMax) * (1.0f / 0.05f), 0.0f, 1.0f);
                    t = amp[k] * norm * mask;
                }
                tgt[k - 1] = t;
                if (v.fresh) v.g[size_t(k - 1)] = t;
                v.dg[size_t(k - 1)] = (t - v.g[size_t(k - 1)]) * (1.0f / float(n));
                if (t > 0.0f || std::abs(v.g[size_t(k - 1)]) > 1e-7f) act = k;
            }
            v.fresh = false;

            // noise band and level
            const float nf = std::clamp(noiseHz * std::pow(std::max(f0, 1.0f) * (1.0f / 261.63f), track), 60.0f, sr_ * 0.45f);
            v.noiseFilt.setCutoffQ(nf, noiseQ);
            // unit-peak band-pass (the filter peaks at Q) with a flat input: rms = sqrt(1/3 * pi/2 * (nf / Q) / (sr / 2))
            const float rms = std::sqrt((1.0f / 3.0f) * 1.5707963f * (nf / noiseQ) / (sr_ * 0.5f));
            const float nNorm = std::min(kPartialRms / std::max(rms, 1e-4f), 40.0f) / noiseQ;

            for (int i = 0; i < n; ++i) {
                v.ex.step();
                const float env = v.env.next();
                v.vibPh += vibInc; v.vibPh -= v.vibPh >= 1.0f ? 1.0f : 0.0f;
                const float vib = vibAmt > 0.0f ? std::exp2(vibAmt * (1.0f / 1200.0f) * fastSin2Pi(v.vibPh)) : 1.0f;
                const float inc0 = v.freq * v.ex.pitchFactor() * vib * invSr;
                float s = 0.0f;
                for (int k = 0; k < act; ++k) {
                    float& p = v.ph[size_t(k)];
                    p += std::min(ratio[k + 1] * inc0, 0.49f);
                    p -= p >= 1.0f ? 1.0f : 0.0f;
                    v.g[size_t(k)] += v.dg[size_t(k)];
                    s += v.g[size_t(k)] * fastSin2Pi(p);
                }
                v.rng ^= v.rng << 13; v.rng ^= v.rng >> 17; v.rng ^= v.rng << 5;
                const float w = float(int32_t(v.rng)) * (1.0f / 2147483648.0f);
                const float nz = v.noiseFilt.processSample(w) * nNorm * (noise + chiff * v.chiffEnv);
                v.chiffEnv = v.chiffEnv < 1e-6f ? 0.0f : v.chiffEnv * chiffK;   // no denormals while the note sustains
                const float o = (s + nz) * env * v.vel * lvl[i] * v.ex.gain();
                l[i] += o;
                r[i] += o;
            }
            for (int k = 0; k < act; ++k) if (tgt[k] == 0.0f) v.g[size_t(k)] = 0.0f;   // faded out: stop paying for it
        }
    }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    std::array<Smoother, kNumP> s_;
    float ctl_[kNumP] = {};
    uint64_t nextSerial_ = 1;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_harmnoise() { return std::make_unique<HarmNoise>(); }

}  // namespace ddaw::devices
