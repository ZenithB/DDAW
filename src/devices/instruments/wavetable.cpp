// `wavetable` instrument (B6): four built-in banks of eight single-cycle frames - classic shapes (sine to pulse), vowels
// (three formant bumps), metallic combs, and organ/digital spectra - each stored as 2048-sample cycles in seven mip levels
// that keep 64, 32, 16, 8, 4, 2 or 1 harmonics, so a note reads the level whose highest partial stays under 0.47 of the
// sample rate (no aliasing from the table itself; the interpolation images sit below -50 dB). `pos` morphs between
// neighbouring frames by crossfading two interpolated reads. The tables are built once, on first use, from harmonic
// recipes; nothing is loaded from disk. Per voice: up to five unison oscillators with detune and stereo spread, a sine sub, a
// state-variable low-pass (per channel, key tracked), a position envelope and the amplitude ADSR. A-rate ports: pos 0,
// pitch 1. Eight voices.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

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

constexpr int kBanks = 4, kFrames = 8, kLevels = 7, kSize = 2048, kMaxHarm = 64;
constexpr int kLevelHarm[kLevels] = {64, 32, 16, 8, 4, 2, 1};

// ---- the tables ----------------------------------------------------------------------------------------------------------

struct Recipe {
    std::array<float, kMaxHarm + 1> amp{}, phase{};   // by harmonic number (1..64): amplitude and phase in cycles
};

uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
float hash01(uint32_t x) { return float(hash32(x) & 0xFFFFFF) * (1.0f / 16777216.0f); }

Recipe makeRecipe(int bank, int frame) {
    Recipe r;
    const auto pi = float(std::numbers::pi);
    auto setAll = [&](auto&& fa, auto&& fp) { for (int k = 1; k <= kMaxHarm; ++k) { r.amp[size_t(k)] = fa(k); r.phase[size_t(k)] = fp(k); } };
    switch (bank) {
        case 0:   // classic: sine, triangle, saw, buzzy saw, square, 25% and 12.5% pulse, impulse train
            switch (frame) {
                case 0: setAll([](int k) { return k == 1 ? 1.0f : 0.0f; }, [](int) { return 0.0f; }); break;
                case 1: setAll([](int k) { return (k & 1) ? 1.0f / float(k * k) : 0.0f; }, [](int k) { return ((k >> 1) & 1) ? 0.5f : 0.0f; }); break;
                case 2: setAll([](int k) { return 1.0f / float(k); }, [](int) { return 0.0f; }); break;
                case 3: setAll([](int k) { return 1.0f / std::sqrt(float(k)); }, [](int) { return 0.0f; }); break;
                case 4: setAll([](int k) { return (k & 1) ? 1.0f / float(k) : 0.0f; }, [](int) { return 0.0f; }); break;
                case 5: setAll([&](int k) { return std::abs(std::sin(pi * 0.25f * float(k))) / float(k); }, [](int k) { return 0.125f * float(k); }); break;
                case 6: setAll([&](int k) { return std::abs(std::sin(pi * 0.125f * float(k))) / float(k); }, [](int k) { return 0.0625f * float(k); }); break;
                default: setAll([](int) { return 1.0f; }, [](int) { return 0.0f; }); break;
            }
            break;
        case 1: {   // vowels: three formant bumps over a falling glottal slope, for a 110 Hz reference voice
            static const float kF[8][3] = {{800, 1200, 2600}, {400, 2000, 2600}, {300, 2300, 3000}, {450, 800, 2700}, {325, 700, 2500}, {700, 1700, 2500}, {400, 1500, 2400}, {300, 1600, 2300}};
            static const float kG[3] = {1.0f, 0.6f, 0.35f};
            for (int k = 1; k <= kMaxHarm; ++k) {
                const float f = 110.0f * float(k);
                float a = 0.0f;
                for (int j = 0; j < 3; ++j) { const float F = kF[frame][j], sg = 0.12f * F; a += kG[j] * std::exp(-0.5f * (f - F) * (f - F) / (sg * sg)); }
                r.amp[size_t(k)] = a / std::pow(float(k), 0.4f) + 0.01f / float(k);
                r.phase[size_t(k)] = 0.0f;
            }
            break;
        }
        case 2: {   // metallic combs: partials spaced every (frame + 1) harmonics from a frame-dependent start, with fixed random phases
            const int step = frame + 1, start = 1 + frame / 2;
            for (int k = 1; k <= kMaxHarm; ++k) {
                const bool on = k >= start && (k - start) % step == 0;
                r.amp[size_t(k)] = (on ? 1.0f : 0.04f) / std::pow(float(k), 0.6f);
                r.phase[size_t(k)] = hash01(uint32_t(k * 977 + frame * 31 + 5));
            }
            break;
        }
        default: {  // organ drawbars and digital spectra
            static const float kBars[7][8] = {{1, 0, 0, 0, 0, 0, 0, 0}, {1, 0.6f, 0, 0, 0, 0, 0, 0}, {1, 0.7f, 0.5f, 0, 0, 0, 0, 0}, {1, 0.8f, 0.7f, 0.6f, 0, 0.5f, 0, 0.4f},
                                              {1, 0.2f, 0.9f, 0.1f, 0.6f, 0.1f, 0.3f, 0.1f}, {0.7f, 1, 0.8f, 0.9f, 0.5f, 0.7f, 0.4f, 0.5f}, {1, 0.5f, 0.45f, 0.35f, 0.3f, 0.25f, 0.2f, 0.18f}};
            if (frame < 7) {
                for (int k = 1; k <= 8; ++k) r.amp[size_t(k)] = kBars[frame][k - 1];
                if (frame == 3) { r.amp[10] = 0.3f; r.amp[12] = 0.25f; r.amp[16] = 0.2f; }
            } else {
                for (int k = 1; k <= kMaxHarm; ++k) { r.amp[size_t(k)] = (0.2f + hash01(uint32_t(k * 131 + 9))) / std::pow(float(k), 0.5f); r.phase[size_t(k)] = hash01(uint32_t(k * 71 + 3)); }
            }
            break;
        }
    }
    return r;
}

struct Tables {
    std::vector<float> data;    // [bank][frame][level][kSize + 1]
    Tables() {
        data.assign(size_t(kBanks) * kFrames * kLevels * (kSize + 1), 0.0f);
        std::vector<float> sinTab(kSize);
        for (int i = 0; i < kSize; ++i) sinTab[size_t(i)] = float(std::sin(2.0 * std::numbers::pi * double(i) / double(kSize)));
        for (int b = 0; b < kBanks; ++b)
            for (int f = 0; f < kFrames; ++f) {
                const Recipe rc = makeRecipe(b, f);
                float scale = 1.0f;
                for (int L = 0; L < kLevels; ++L) {
                    float* t = at(b, f, L);
                    for (int k = 1; k <= kLevelHarm[L]; ++k) {
                        const float a = rc.amp[size_t(k)];
                        if (a == 0.0f) continue;
                        const int ph = int(std::lround(rc.phase[size_t(k)] * float(kSize)));
                        for (int i = 0; i < kSize; ++i) t[i] += a * sinTab[size_t((k * i + ph) & (kSize - 1))];
                    }
                    if (L == 0) {   // loudness from the full-bandwidth table: rms 0.35, peak at most 1.5, the same for every level
                        double pk = 1e-9, sq = 0.0;
                        for (int i = 0; i < kSize; ++i) { pk = std::max(pk, double(std::abs(t[i]))); sq += double(t[i]) * t[i]; }
                        const double rms = std::sqrt(sq / kSize);
                        scale = float(std::min(0.35 / std::max(rms, 1e-9), 1.5 / pk));
                    }
                    for (int i = 0; i < kSize; ++i) t[i] *= scale;
                    t[kSize] = t[0];
                }
            }
    }
    float* at(int bank, int frame, int level) { return data.data() + ((size_t(bank) * kFrames + size_t(frame)) * kLevels + size_t(level)) * (kSize + 1); }
    const float* at(int bank, int frame, int level) const { return data.data() + ((size_t(bank) * kFrames + size_t(frame)) * kLevels + size_t(level)) * (kSize + 1); }
};

const Tables& tables() { static const Tables t; return t; }

inline float readTable(const float* t, float phase) noexcept {
    const float p = phase * float(kSize);
    const int i = int(p);
    const float f = p - float(i);
    return t[i] + f * (t[i + 1] - t[i]);
}

// ---- the device ----------------------------------------------------------------------------------------------------------

enum P : uint16_t { Bank, Pos, PosEnv, PosDecay, Unison, Detune, Spread, Sub, Cutoff, Res, Keytrack, Attack, Decay, Sustain, Release, Pitch, Level, kNumP };
enum A : size_t { APos = 0, APitch = 1 };

constexpr int kVoices = 8, kMaxUni = 5, kCtrl = 16;

struct Voice {
    std::array<float, kMaxUni> ph{};
    float subPh = 0.0f, posEnv = 0.0f;
    Adsr env;
    NoteExpr ex;
    Svf lpL{SvfMode::Lowpass}, lpR{SvfMode::Lowpass};
    float freq = 440.0f, vel = 1.0f;
    uint8_t pitch = 69;
    bool held = false;
    uint64_t serial = 0;
    uint32_t id = 0;
    bool active() const { return env.isActive(); }
};

class Wavetable final : public InstrumentDevice {
public:
    Wavetable() {
        (void)tables();   // build the tables here, on the builder thread, never in process()
        for (const auto& ps : schema::kInstWavetable) setParam(ps.index, ps.def);
        for (auto& s : s_) s.snap(s.target());
    }

    std::span<const ParamSpec> params() const override { return schema::kInstWavetable; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 8000.0));
        for (auto& s : s_) s.prepare(sr_, 15.0f);
        for (auto& v : voices_) { v.env.prepare(sr_); v.lpL.prepare(sr_); v.lpR.prepare(sr_); v.env.reset(); v.held = false; }
        applyAdsr();
        for (int i = 0; i < kNumP; ++i) setParam(uint16_t(i), ctl_[i]);
        for (auto& s : s_) s.snap(s.target());
    }

    void setParam(uint16_t i, float v) override {
        if (i >= kNumP) return;
        const ParamSpec& ps = schema::kInstWavetable[i];
        v = std::clamp(v, ps.min, ps.max);
        ctl_[i] = v;
        s_[i].setTarget(v);
        if (i >= Attack && i <= Release) applyAdsr();
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        Voice& v = voices_[size_t(pickVoice(voices_, pitch))];
        for (int j = 0; j < kMaxUni; ++j) v.ph[size_t(j)] = 0.173f * float(j);
        v.subPh = 0.0f;
        v.posEnv = 1.0f;
        v.lpL.reset(); v.lpR.reset();
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

    // MPE: slide moves the table position (up to +0.5), pressure raises the level by up to 50%, bend is in semitones.
    void noteExpression(uint32_t noteId, int dimension, float value) override {
        for (auto& v : voices_) if (v.active() && v.id == noteId) v.ex.set(dimension, value);
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs& mod) override {
        const float* aPos = aBuf(mod, APos);
        const float* aPit = aBuf(mod, APitch);
        const Tables& tb = tables();
        const float invSr = 1.0f / sr_;
        const int bank = std::clamp(int(std::lround(ctl_[Bank])), 0, kBanks - 1);
        const int uni = std::clamp(int(std::lround(ctl_[Unison])), 1, kMaxUni);
        for (int i = 0; i < n; ++i) {
            const float pos = s_[Pos].next(), posEnvAmt = s_[PosEnv].next(), detune = s_[Detune].next(), spread = s_[Spread].next();
            const float sub = s_[Sub].next(), cutoff = s_[Cutoff].next(), res = s_[Res].next(), keytrack = s_[Keytrack].next();
            const float pitch = s_[Pitch].next(), level = s_[Level].next();
            const float posDecay = s_[PosDecay].next();
            const float posK = std::exp(-1.0f / (std::max(posDecay, 0.001f) * sr_ * 0.3f));
            const float semi = pitch + (aPit ? aPit[i] : 0.0f);
            const float pfBase = semi == 0.0f ? 1.0f : std::exp2(semi * (1.0f / 12.0f));
            const bool refresh = (ctrlCount_++ & (kCtrl - 1)) == 0;
            // unison: detune ratios and pan gains for this sample (the settings are smooth, the loops are tiny)
            float ratio[kMaxUni], gl[kMaxUni], gr[kMaxUni];
            for (int j = 0; j < uni; ++j) {
                const float u = uni > 1 ? (2.0f * float(j) / float(uni - 1) - 1.0f) : 0.0f;
                ratio[j] = u == 0.0f || detune == 0.0f ? 1.0f : std::exp2(u * detune * (1.0f / 1200.0f));
                const auto [pl, pr] = panGains(u * spread);
                const float g = 1.0f / std::sqrt(float(uni));
                gl[j] = pl * g * 1.41421356f; gr[j] = pr * g * 1.41421356f;
            }
            float accL = 0.0f, accR = 0.0f;
            for (auto& v : voices_) {
                if (!v.active()) continue;
                v.ex.step();
                const float a = v.env.next();
                v.posEnv = v.posEnv < 1e-6f ? 0.0f : v.posEnv * posK;
                const float p = std::clamp(pos + (aPos ? aPos[i] : 0.0f) + posEnvAmt * v.posEnv + 0.5f * v.ex.slideS, 0.0f, 1.0f);
                const float fp = p * float(kFrames - 1);
                const int f0 = std::min(int(fp), kFrames - 2);
                const float ff = fp - float(f0);
                const float f = v.freq * pfBase * v.ex.pitchFactor();
                const float fMax = f * ratio[uni - 1] * invSr;
                int lv = kLevels - 1;                              // the largest table whose top partial stays under 0.47 of the rate
                for (int L = 0; L < kLevels; ++L) if (float(kLevelHarm[L]) * fMax < 0.47f) { lv = L; break; }
                const bool silent = fMax >= 0.47f;
                const float* ta = tb.at(bank, f0, lv);
                const float* tbb = tb.at(bank, f0 + 1, lv);
                float sl = 0.0f, sr = 0.0f;
                for (int j = 0; j < uni; ++j) {
                    float& ph = v.ph[size_t(j)];
                    const float s = silent ? 0.0f : readTable(ta, ph) * (1.0f - ff) + readTable(tbb, ph) * ff;
                    ph += f * ratio[j] * invSr;
                    ph -= ph >= 1.0f ? 1.0f : 0.0f;
                    sl += s * gl[j]; sr += s * gr[j];
                }
                if (sub > 0.0f) {
                    const float s = sub * 0.5f * fastSin2Pi(v.subPh);
                    v.subPh += f * 0.5f * invSr; v.subPh -= v.subPh >= 1.0f ? 1.0f : 0.0f;
                    sl += s; sr += s;
                }
                const float cut = cutoff * std::exp2(keytrack * (float(v.pitch) - 60.0f) * (1.0f / 12.0f));
                if (cut < sr_ * 0.45f) {
                    if (refresh) { const float q = 0.7071f + 9.0f * res * res; v.lpL.setCutoffQ(cut, q); v.lpR.setCutoffQ(cut, q); }
                    sl = v.lpL.processSample(sl); sr = v.lpR.processSample(sr);
                }
                const float g = a * v.vel * v.ex.gain();
                accL += sl * g; accR += sr * g;
            }
            l[i] += accL * level;
            r[i] += accR * level;
        }
    }

    void reset() override {
        for (auto& v : voices_) { v.env.reset(); v.held = false; v.serial = 0; v.id = 0; v.ex.clear(); v.lpL.reset(); v.lpR.reset(); }
        nextSerial_ = 1;
        ctrlCount_ = 0;
        for (auto& s : s_) s.snap(s.target());
    }

private:
    static const float* aBuf(const ModInputs& m, size_t ordinal) { return ordinal < m.audioRate.size() ? m.audioRate[ordinal] : nullptr; }
    void applyAdsr() { for (auto& v : voices_) v.env.setAdsr(ctl_[Attack], ctl_[Decay], ctl_[Sustain], ctl_[Release]); }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    std::array<Smoother, kNumP> s_;
    float ctl_[kNumP] = {};
    uint64_t nextSerial_ = 1;
    uint32_t ctrlCount_ = 0;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_wavetable() { return std::make_unique<Wavetable>(); }

}  // namespace ddaw::devices
