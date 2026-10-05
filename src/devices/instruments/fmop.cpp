// `fmop` instrument (B4): a four-operator FM/AM synth with audio-rate modulation ports.
//
// Wiring (`algo`), operator k has output o_k = sin(phase_k + modulation) and level l_k; modulation depth is
// l_modulator * index * indexEnvelope radians. Operator 4 can feed back on itself (`fb`).
//   0  4 -> 3 -> 2 -> 1                      out = l1*o1                       (serial FM)
//   1  (4 -> 3) + (2 -> 1)                   out = (l1*o1 + l3*o3) / 2         (two stacks)
//   2  (4, 3, 2) -> 1                        out = l1*o1                       (fan-in)
//   3  4 -> (1, 2, 3)                        out = (l1*o1 + l2*o2 + l3*o3) / 3 (fan-out)
//   4  additive                              out = (l1*o1 + l2*o2 + l3*o3 + l4*o4) / 4
//   5  AM: o1 scaled by (1 - l2 + l2*o2), o3 by (1 - l4 + l4*o4)   out = (l1*o1' + l3*o3') / 2   (ring mod at l = 1)
//
// Three parameters accept audio-rate modulation through ModInputs (ordinals: index 0, pitch 1, amp 2): the buffers
// are added to the (smoothed) control value per sample, bypassing the smoother. Everything is generated at four
// times the host rate and decimated once on the summed voices (dsp/Decimator4.h, 47/15-tap halfbands), so FM
// sidebands above the Nyquist are removed instead of folding back; the decimator's delay is reported as latency.
// Fixed pool of 8 voices; a noteOn on a still-held pitch reuses that voice, else a free one, else the oldest.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/SchemaB4.h"
#include "dsp/Adsr.h"
#include "dsp/Decimator4.h"
#include "dsp/FastSin.h"
#include "dsp/Math.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Algo, R1, R2, R3, R4, L1, L2, L3, L4, Index, Pitch, Amp, Fb, Attack, Decay, Sustain, Release, IDecay, ISus, VelIdx };
enum A : int { ARIndex = 0, ARPitch = 1, ARAmp = 2 };

constexpr int kVoices = 8;
constexpr int kOs = 4;                                  // oversampling factor
constexpr float kInvTwoPi = float(1.0 / (2.0 * std::numbers::pi));
constexpr float kOutGain = 0.35f;
constexpr float kExprSmooth = 0.012f;                    // per sample: about 2 ms at 44.1 kHz

struct Voice {
    Adsr ampEnv, idxEnv;
    float ph[4] = {0, 0, 0, 0};
    float fb1 = 0.0f, fb2 = 0.0f;                       // operator 4's last two outputs (feedback)
    float freq = 440.0f, vel = 1.0f;
    float bend = 0.0f, slide = 0.0f, pressure = 0.0f;       // per-note expression, as set ...
    float bendS = 0.0f, slideS = 0.0f, pressureS = 0.0f;    // ... and as heard (smoothed, so controller steps do not zipper)
    uint8_t pitch = 69;
    bool held = false;
    uint64_t serial = 0;
    uint32_t id = 0;

    void prepare(float sr) { ampEnv.prepare(sr); idxEnv.prepare(sr); clear(); }
    void clear() { for (float& p : ph) p = 0.0f; fb1 = fb2 = 0.0f; held = false; ampEnv.reset(); idxEnv.reset(); bend = slide = pressure = bendS = slideS = pressureS = 0.0f; }
    bool active() const { return ampEnv.isActive(); }
    void release() { held = false; ampEnv.noteOff(); idxEnv.noteOff(); }
};

// Everything a sample needs that does not depend on the voice.
struct Frame {
    int algo;
    float ratio[4], level[4];
    float indexBase;       // control-rate index
    float fbAmt;
};

class FmopInst final : public InstrumentDevice {
public:
    FmopInst() {
        for (const auto& ps : schema::kInstFmop) setParam(ps.index, ps.def);
        snapAll();
    }

    std::span<const ParamSpec> params() const override { return schema::kInstFmop; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 8000.0));
        for (auto* s : {&r_[0], &r_[1], &r_[2], &r_[3], &l_[0], &l_[1], &l_[2], &l_[3], &index_, &pitch_, &amp_, &fb_}) s->prepare(sr_, 15.0f);
        for (auto& v : voices_) v.prepare(sr_);
        applyAdsr();
        for (const auto& ps : schema::kInstFmop) setParam(ps.index, ctl_[ps.index]);
        snapAll();
    }

    void setParam(uint16_t i, float v) override {
        if (i >= schema::kInstFmop[0].index + std::size(schema::kInstFmop)) return;
        const ParamSpec& ps = schema::kInstFmop[i];
        v = std::clamp(v, ps.min, ps.max);
        ctl_[i] = v;
        switch (i) {
            case Algo: algo_ = int(std::lround(v)); break;
            case R1: case R2: case R3: case R4: r_[i - R1].setTarget(v); break;
            case L1: case L2: case L3: case L4: l_[i - L1].setTarget(v); break;
            case Index: index_.setTarget(v); break;
            case Pitch: pitch_.setTarget(v); break;
            case Amp: amp_.setTarget(v); break;
            case Fb: fb_.setTarget(v); break;
            case Attack: case Decay: case Sustain: case Release: applyAdsr(); break;
            case IDecay: case ISus: applyAdsr(); break;
            case VelIdx: velIdx_ = v; break;
            default: break;
        }
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        const uint64_t serial = nextSerial_++;
        int same = -1, freeV = -1, oldest = 0;
        uint64_t oldestSerial = UINT64_MAX;
        for (int i = 0; i < kVoices; ++i) {
            const Voice& v = voices_[size_t(i)];
            if (v.held && v.pitch == pitch) { same = i; break; }
            if (!v.active() && freeV < 0) freeV = i;
            if (v.serial < oldestSerial) { oldestSerial = v.serial; oldest = i; }
        }
        const int idx = same >= 0 ? same : (freeV >= 0 ? freeV : oldest);
        Voice& v = voices_[size_t(idx)];
        v.clear();
        v.pitch = pitch;
        v.freq = midiHz(float(pitch));
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.serial = serial;
        v.id = noteId;
        v.held = true;
        v.ampEnv.noteOn();
        v.idxEnv.noteOn();
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_) if (v.held && v.id == noteId) v.release();
    }

    // MPE: slide brightens (the modulation index grows by up to 3x), pressure raises the level by up to 50%, bend is in semitones.
    void noteExpression(uint32_t noteId, int dimension, float value) override {
        for (auto& v : voices_) {
            if (!v.active() || v.id != noteId) continue;
            if (dimension == 0) v.slide = std::clamp(value, 0.0f, 1.0f);
            else if (dimension == 1) v.pressure = std::clamp(value, 0.0f, 1.0f);
            else if (dimension == 2) v.bend = std::clamp(value, -96.0f, 96.0f);
        }
    }

    int latencySamples() const override { return dec_.latency(); }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs& mod) override {
        const float* aIdx = aBuf(mod, ARIndex);
        const float* aPit = aBuf(mod, ARPitch);
        const float* aAmp = aBuf(mod, ARAmp);
        const float invOs = 1.0f / (float(kOs) * sr_);
        for (int i = 0; i < n; ++i) {
            // ---- control values for this sample (smoothed) and the audio-rate additions ----
            Frame fr;
            fr.algo = algo_;
            for (int k = 0; k < 4; ++k) { fr.ratio[k] = r_[k].next(); fr.level[k] = l_[k].next(); }
            fr.indexBase = index_.next();
            fr.fbAmt = fb_.next();
            const float pitchSmooth = pitch_.next();
            const float ampSmooth = amp_.next();
            // values at the end of this sample; the four sub-samples interpolate from the previous one
            const float idxNow = std::max(fr.indexBase + (aIdx ? aIdx[i] : 0.0f), 0.0f);
            const float semiNow = pitchSmooth + (aPit ? aPit[i] : 0.0f);
            const float ampNow = std::clamp(ampSmooth + (aAmp ? aAmp[i] : 0.0f), 0.0f, 2.0f);
            if (!primed_) { idxPrev_ = idxNow; semiPrev_ = semiNow; ampPrev_ = ampNow; primed_ = true; }

            float hi[kOs] = {0, 0, 0, 0};
            for (auto& v : voices_) {
                if (!v.active()) continue;
                const float aenv = v.ampEnv.next();
                glide(v.bendS, v.bend);
                glide(v.slideS, v.slide);
                glide(v.pressureS, v.pressure);
                const float ienv = v.idxEnv.next() * (1.0f + 2.0f * v.slideS);
                const float velScale = 1.0f - velIdx_ * (1.0f - v.vel);
                const float gain = aenv * v.vel * kOutGain * (1.0f + 0.5f * v.pressureS);
                for (int j = 0; j < kOs; ++j) {
                    const float t = float(j + 1) * (1.0f / float(kOs));
                    const float idx = (idxPrev_ + (idxNow - idxPrev_) * t) * ienv * velScale;
                    const float semi = semiPrev_ + (semiNow - semiPrev_) * t + v.bendS;
                    const float am = ampPrev_ + (ampNow - ampPrev_) * t;
                    const float pf = semi == 0.0f ? 1.0f : std::exp2(semi * (1.0f / 12.0f));
                    hi[j] += gain * am * sample(v, fr, idx, v.freq * pf * invOs);
                }
            }
            idxPrev_ = idxNow; semiPrev_ = semiNow; ampPrev_ = ampNow;
            const float s = dec_.push(hi[0], hi[1], hi[2], hi[3]);
            l[i] += s;
            r[i] += s;
        }
    }

    void reset() override {
        for (auto& v : voices_) { v.clear(); v.serial = 0; v.id = 0; }
        nextSerial_ = 1;
        dec_.reset();
        primed_ = false;
        snapAll();
    }

private:
    static const float* aBuf(const ModInputs& m, int ordinal) {
        return size_t(ordinal) < m.audioRate.size() ? m.audioRate[size_t(ordinal)] : nullptr;
    }

    // One step of the expression smoother; settles exactly (a residue of 1e-6 would keep the pitch path on).
    static void glide(float& s, float target) {
        s += (target - s) * kExprSmooth;
        if (std::abs(target - s) < 1e-6f) s = target;
    }

    // One oversampled sample of one voice.
    static float sample(Voice& v, const Frame& f, float idx, float incBase) {
        const float* r = f.ratio;
        const float* l = f.level;
        float inc[4];
        for (int k = 0; k < 4; ++k) inc[k] = std::min(incBase * r[k], 0.5f);
        const float md = idx * kInvTwoPi;               // radians -> cycles
        float o1, o2, o3, o4, out;
        // operator 4 (the top of every stack) with self-feedback
        o4 = fastSin2Pi(v.ph[3] + f.fbAmt * 0.5f * (v.fb1 + v.fb2) * 0.5f);
        v.fb2 = v.fb1;
        v.fb1 = o4;
        switch (f.algo) {
            case 0:
                o3 = fastSin2Pi(v.ph[2] + md * l[3] * o4);
                o2 = fastSin2Pi(v.ph[1] + md * l[2] * o3);
                o1 = fastSin2Pi(v.ph[0] + md * l[1] * o2);
                out = l[0] * o1;
                break;
            case 1:
                o3 = fastSin2Pi(v.ph[2] + md * l[3] * o4);
                o2 = fastSin2Pi(v.ph[1]);
                o1 = fastSin2Pi(v.ph[0] + md * l[1] * o2);
                out = 0.5f * (l[0] * o1 + l[2] * o3);
                break;
            case 2:
                o3 = fastSin2Pi(v.ph[2]);
                o2 = fastSin2Pi(v.ph[1]);
                o1 = fastSin2Pi(v.ph[0] + md * (l[1] * o2 + l[2] * o3 + l[3] * o4));
                out = l[0] * o1;
                break;
            case 3:
                o3 = fastSin2Pi(v.ph[2] + md * l[3] * o4);
                o2 = fastSin2Pi(v.ph[1] + md * l[3] * o4);
                o1 = fastSin2Pi(v.ph[0] + md * l[3] * o4);
                out = (l[0] * o1 + l[1] * o2 + l[2] * o3) * (1.0f / 3.0f);
                break;
            case 4:
                o3 = fastSin2Pi(v.ph[2]);
                o2 = fastSin2Pi(v.ph[1]);
                o1 = fastSin2Pi(v.ph[0]);
                out = (l[0] * o1 + l[1] * o2 + l[2] * o3 + l[3] * o4) * 0.25f;
                break;
            default:   // 5: amplitude modulation / ring modulation
                o3 = fastSin2Pi(v.ph[2]);
                o2 = fastSin2Pi(v.ph[1]);
                o1 = fastSin2Pi(v.ph[0]);
                out = 0.5f * (l[0] * o1 * (1.0f - l[1] + l[1] * o2) + l[2] * o3 * (1.0f - l[3] + l[3] * o4));
                break;
        }
        for (int k = 0; k < 4; ++k) { v.ph[k] += inc[k]; v.ph[k] -= std::floor(v.ph[k]); }
        return out;
    }

    void applyAdsr() {
        for (auto& v : voices_) {
            v.ampEnv.setAdsr(ctl_[Attack], ctl_[Decay], ctl_[Sustain], ctl_[Release]);
            v.idxEnv.setAdsr(0.002f, ctl_[IDecay], ctl_[ISus], ctl_[Release]);
        }
    }
    void snapAll() {
        for (auto* s : {&r_[0], &r_[1], &r_[2], &r_[3], &l_[0], &l_[1], &l_[2], &l_[3], &index_, &pitch_, &amp_, &fb_}) s->snap(s->target());
    }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    Smoother r_[4], l_[4], index_, pitch_, amp_, fb_;
    Decimator4 dec_{47, 15};
    float ctl_[20] = {};
    int algo_ = 0;
    float velIdx_ = 0.5f;
    float idxPrev_ = 0.0f, semiPrev_ = 0.0f, ampPrev_ = 1.0f;
    bool primed_ = false;
    uint64_t nextSerial_ = 1;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_fmop() { return std::make_unique<FmopInst>(); }

}  // namespace ddaw::devices
