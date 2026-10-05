// `modal` instrument (B6): modal synthesis. A struck object is a bank of damped resonances; each mode is a two-pole
// resonator y[n] = 2 R cos(w) y[n-1] - R^2 y[n-2] + a sin(w) x[n] with its own frequency ratio, level and decay.
//   models    0 string (k), 1 free bar (Euler-Bernoulli, 1 : 2.756 : 5.404 : 8.933 ...), 2 marimba bar (1 : 4 : 10 ...),
//             3 circular membrane (Bessel zeros), 4 plate (m^2 + 1.7 n^2), 5 bell (minor-third), 6 closed tube (odd k)
//   `inharm`  stretches the ratios, r -> r^(1 + inharm)
//   levels    the strike position weights mode n by |sin(pi pos n)| (exact nulls for the string; the others keep a floor), and `tone`
//             tilts the levels with frequency (r^(-0.7 (1 - tone)))
//   decay     the fundamental rings for T60 = decay seconds, mode k for decay * r_k^(-1.5 damp); the release shortens every
//             mode's T60 to release * r_k^(-1.5 damp) when the note ends
//   exciter   an impulse, a noise burst, or a half-sine mallet pulse whose width (0.3 to 8 ms) falls with `hard` and with velocity,
//             normalised to unit area so a soft strike is quieter at the top, not overall
// The strike (model, modes, inharm, decay, damp, pos, tone, exciter, hard, spread) is read when the note starts; a sounding
// note keeps it. Modes above 0.47 of the sample rate are left out. Pitch bend retunes the modes block by block (the
// coefficients are recomputed only when the bend moves). Stereo spread alternates the modes between the channels. Eight voices.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/SchemaB6.h"
#include "dsp/Math.h"
#include "dsp/NoteExpr.h"
#include "dsp/Smoother.h"
#include "dsp/VoicePool.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Model, Modes, Inharm, Decay, Damp, Pos, Tone, Exciter, Hard, Release, Spread, Level, kNumP };

constexpr int kVoices = 8, kMaxModes = 24;
constexpr float kT60Ln = 6.9077553f;   // ln(1000): 60 dB

// Frequency ratios of the modes of each model, fundamental = 1.
struct Ratios {
    float r[7][kMaxModes];
    Ratios() {
        for (int k = 0; k < kMaxModes; ++k) r[0][k] = float(k + 1);
        static const float kBar[5] = {1.0f, 2.7565f, 5.4039f, 8.9330f, 13.3443f};
        for (int k = 0; k < kMaxModes; ++k) {
            if (k < 5) r[1][k] = kBar[k];
            else { const double b = (double(k) + 1.5) * std::numbers::pi; r[1][k] = float(b * b / (4.73004 * 4.73004)); }
        }
        static const float kMarimba[4] = {1.0f, 4.0f, 10.0f, 17.6f};
        for (int k = 0; k < kMaxModes; ++k) r[2][k] = k < 4 ? kMarimba[k] : r[1][k] * 1.9f;
        static const float kDrum[kMaxModes] = {1.000f, 1.593f, 2.135f, 2.295f, 2.653f, 2.917f, 3.155f, 3.500f, 3.598f, 3.652f, 4.060f, 4.154f,
                                               4.230f, 4.601f, 4.723f, 4.831f, 5.131f, 5.276f, 5.412f, 5.540f, 5.652f, 5.987f, 6.151f, 6.202f};
        for (int k = 0; k < kMaxModes; ++k) r[3][k] = kDrum[k];
        // plate: the lowest 24 values of m^2 + 1.7 n^2, scaled to start at 1
        float v[64]; int n = 0;
        for (int m = 1; m <= 7; ++m) for (int q = 1; q <= 7; ++q) v[n++] = float(m * m) + 1.7f * float(q * q);
        std::sort(v, v + n);
        for (int k = 0; k < kMaxModes; ++k) r[4][k] = v[k] / v[0];
        static const float kBell[kMaxModes] = {1, 2, 2.4f, 3, 4, 5, 6, 6.4f, 8, 9.6f, 10.7f, 12, 13.3f, 14.4f, 16, 17.6f, 19.2f, 20.8f, 22.4f, 24, 25.6f, 27.2f, 28.8f, 30.4f};
        for (int k = 0; k < kMaxModes; ++k) r[5][k] = kBell[k];
        for (int k = 0; k < kMaxModes; ++k) r[6][k] = float(2 * k + 1);
    }
};
const Ratios& ratios() { static const Ratios r; return r; }

struct Mode {
    float y1 = 0.0f, y2 = 0.0f, c1 = 0.0f, c2 = 0.0f, b = 0.0f;
    float ratio = 1.0f, amp = 0.0f, t60 = 1.0f, t60Rel = 1.0f, gl = 1.0f, gr = 1.0f;
};

struct Voice {
    std::array<Mode, kMaxModes> m;
    int nModes = 0;
    NoteExpr ex;
    float freq = 440.0f, vel = 1.0f, lastBend = 1e9f;
    int excKind = 2;
    float excW = 1.0f, excLp = 1.0f, excState = 0.0f;
    int excN = 0;
    uint32_t rng = 1;
    uint8_t pitch = 69;
    bool held = false, alive = false, releasing = false;
    uint64_t serial = 0;
    uint32_t id = 0;
    bool active() const { return alive; }
};

class Modal final : public InstrumentDevice {
public:
    Modal() {
        (void)ratios();
        for (const auto& ps : schema::kInstModal) setParam(ps.index, ps.def);
        for (auto& s : s_) s.snap(s.target());
    }

    std::span<const ParamSpec> params() const override { return schema::kInstModal; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 8000.0));
        for (auto& s : s_) s.prepare(sr_, 15.0f);
        for (auto& v : voices_) { v.alive = false; v.held = false; }
        for (int i = 0; i < kNumP; ++i) setParam(uint16_t(i), ctl_[i]);
        for (auto& s : s_) s.snap(s.target());
    }

    void setParam(uint16_t i, float v) override {
        if (i >= kNumP) return;
        const ParamSpec& ps = schema::kInstModal[i];
        v = std::clamp(v, ps.min, ps.max);
        ctl_[i] = v;
        s_[i].setTarget(v);
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        Voice& v = voices_[size_t(pickVoice(voices_, pitch))];
        v.ex.clear();
        v.pitch = pitch;
        v.freq = midiHz(float(pitch));
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.serial = nextSerial_++;
        v.id = noteId;
        v.held = true;
        v.alive = true;
        v.releasing = false;
        v.lastBend = 1e9f;
        v.rng = 0xC2B2AE35u * (noteId + 1u) + 99u;
        strike(v);
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_) {
            if (!v.held || v.id != noteId) continue;
            v.held = false;
            v.releasing = true;
            const float rel = ctl_[Release], damp = ctl_[Damp];
            for (int k = 0; k < v.nModes; ++k) {
                Mode& m = v.m[size_t(k)];
                m.t60Rel = std::min(m.t60, rel * std::pow(m.ratio, -1.5f * damp));
            }
            v.lastBend = 1e9f;   // recompute the radii
        }
    }

    // MPE: pressure raises the level by up to 50%, bend retunes the modes (semitones). Slide has no meaning for a struck object.
    void noteExpression(uint32_t noteId, int dimension, float value) override {
        for (auto& v : voices_) if (v.alive && v.id == noteId) v.ex.set(dimension, value);
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int done = 0; done < n; done += kMaxBlock) block(l + done, r + done, std::min(kMaxBlock, n - done));
    }

    void reset() override {
        for (auto& v : voices_) { v.alive = false; v.held = false; v.releasing = false; v.serial = 0; v.id = 0; v.ex.clear(); for (auto& m : v.m) m.y1 = m.y2 = 0.0f; }
        nextSerial_ = 1;
        for (auto& s : s_) s.snap(s.target());
    }

private:
    // Read the strike parameters and set up the modes of a voice.
    void strike(Voice& v) {
        const int model = std::clamp(int(std::lround(ctl_[Model])), 0, 6);
        const int nModes = std::clamp(int(std::lround(ctl_[Modes])), 2, kMaxModes);
        const float inharm = ctl_[Inharm], decay = ctl_[Decay], damp = ctl_[Damp], pos = ctl_[Pos], tone = ctl_[Tone], spread = ctl_[Spread];
        const bool string = model == 0;
        float sumSq = 0.0f;
        v.nModes = nModes;
        for (int k = 0; k < nModes; ++k) {
            Mode& m = v.m[size_t(k)];
            m.y1 = m.y2 = 0.0f;
            m.ratio = std::pow(ratios().r[model][k], 1.0f + inharm);
            const float w = std::abs(std::sin(float(std::numbers::pi) * pos * float(k + 1)));
            float a = (string ? w : 0.2f + 0.8f * w) * std::pow(m.ratio, -0.7f * (1.0f - tone));
            if (v.freq * m.ratio > 0.47f * sr_) a = 0.0f;                  // above the Nyquist: left out
            m.amp = a;
            sumSq += a * a;
            m.t60 = std::max(decay * std::pow(m.ratio, -1.5f * damp), 0.01f);
            m.t60Rel = m.t60;
            const float theta = float(std::numbers::pi) * 0.25f * (1.0f + ((k & 1) ? spread : -spread));   // alternate the channels
            m.gl = std::cos(theta) * 1.41421356f;
            m.gr = std::sin(theta) * 1.41421356f;
        }
        const float norm = sumSq > 1e-12f ? 0.9f / std::sqrt(sumSq) : 0.0f;
        for (int k = 0; k < nModes; ++k) v.m[size_t(k)].amp *= norm;
        // the exciter
        const int kind = std::clamp(int(std::lround(ctl_[Exciter])), 0, 2);
        const float hard = std::clamp(ctl_[Hard] + 0.3f * (v.vel - 0.5f), 0.0f, 1.0f);
        v.excKind = kind;
        v.excN = 0;
        v.excState = 0.0f;
        if (kind == 2) v.excW = std::max(1.0f, (0.3f + 7.7f * (1.0f - hard)) * 0.001f * sr_);
        else if (kind == 1) { v.excW = std::max(4.0f, (0.5f + 12.0f * (1.0f - hard)) * 0.001f * sr_); v.excLp = std::clamp(0.05f + 0.95f * hard * hard, 0.02f, 1.0f); }
        else v.excW = 1.0f;
    }

    // Per-block coefficients of a voice: radii (release) and frequencies (bend).
    static void coefficients(Voice& v, float sr, float bendFactor) {
        for (int k = 0; k < v.nModes; ++k) {
            Mode& m = v.m[size_t(k)];
            const float f = v.freq * m.ratio * bendFactor;
            if (m.amp == 0.0f || f >= 0.47f * sr) { m.c1 = 0.0f; m.c2 = 0.0f; m.b = 0.0f; continue; }
            const float w = 2.0f * float(std::numbers::pi) * f / sr;
            const float R = std::exp(-kT60Ln / (m.t60Rel * sr));
            m.c1 = 2.0f * R * std::cos(w);
            m.c2 = R * R;
            m.b = m.amp * std::sin(w);
        }
    }

    float excitation(Voice& v) {
        const int n = v.excN++;
        switch (v.excKind) {
            case 0: return n == 0 ? v.vel : 0.0f;
            case 1: {
                if (float(n) >= v.excW) return 0.0f;
                v.rng ^= v.rng << 13; v.rng ^= v.rng >> 17; v.rng ^= v.rng << 5;
                const float w = float(int32_t(v.rng)) * (1.0f / 2147483648.0f);
                const float env = std::exp(-4.0f * float(n) / v.excW);
                v.excState += v.excLp * (w * env - v.excState);
                return v.excState * v.vel * (3.0f / std::sqrt(v.excW));
            }
            default:
                if (float(n) >= v.excW) return 0.0f;
                return std::sin(float(std::numbers::pi) * (float(n) + 0.5f) / v.excW) * (float(std::numbers::pi) * 0.5f / v.excW) * v.vel;
        }
    }

    void block(float* l, float* r, int n) {
        float lvl[kMaxBlock];
        for (int i = 0; i < n; ++i) lvl[i] = s_[Level].next();
        for (auto& v : voices_) {
            if (!v.alive) continue;
            for (int i = 0; i < n; ++i) v.ex.step();
            if (v.ex.bendS != v.lastBend) { coefficients(v, sr_, v.ex.pitchFactor()); v.lastBend = v.ex.bendS; }
            const float g = v.ex.gain();
            float peak = 0.0f;
            for (int i = 0; i < n; ++i) {
                const float x = v.excN < 100000 ? excitation(v) : 0.0f;
                float sl = 0.0f, sr2 = 0.0f;
                for (int k = 0; k < v.nModes; ++k) {
                    Mode& m = v.m[size_t(k)];
                    const float y = m.c1 * m.y1 - m.c2 * m.y2 + m.b * x;
                    m.y2 = m.y1; m.y1 = y;
                    sl += m.gl * y; sr2 += m.gr * y;
                }
                l[i] += sl * g * lvl[i];
                r[i] += sr2 * g * lvl[i];
                peak = std::max(peak, std::max(std::abs(sl), std::abs(sr2)));
            }
            for (int k = 0; k < v.nModes; ++k) { Mode& m = v.m[size_t(k)]; if (std::abs(m.y1) < 1e-15f && std::abs(m.y2) < 1e-15f) m.y1 = m.y2 = 0.0f; }   // a mode that died early must not run on denormals
            // done when the exciter is over and nothing audible is left (about -140 dB)
            if (peak < 1e-7f && float(v.excN) > v.excW + 1.0f) { v.alive = false; v.held = false; for (auto& m : v.m) m.y1 = m.y2 = 0.0f; }
        }
    }

    float sr_ = 44100.0f;
    std::array<Voice, kVoices> voices_;
    std::array<Smoother, kNumP> s_;
    float ctl_[kNumP] = {};
    uint64_t nextSerial_ = 1;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_modal() { return std::make_unique<Modal>(); }

}  // namespace ddaw::devices
