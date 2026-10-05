// `granular` instrument: port of sf-dsp/src/inst/granular.rs (makeGranular in the browser).
// Each held note runs a grain clock at `dens` grains/s; every tick spawns one grain: a `size`-second
// slice from `pos` (+- `spray`) of the sample in slot 0, pitched by the note (60 = reference, plus
// `pitch` +- `pjit` jitter), reversed with probability `rev`, through a triangular 0 -> 0.8 -> 0 gain
// window skewed by `shape`, a random stereo pan (+- `spread`, Web Audio StereoPanner law) and the
// voice's amp envelope (`attack` linear, `release` exponential with tau = release/3 after noteOff).
// Randomness is the Rust xorshift64* with its fixed seed, reseeded in reset() so renders repeat.
// Output gain 0.9. No sample (never set, or null) plays silence. Fixed pools: 10 voices (a note with
// all voices busy is dropped, like the browser) and 256 grains (a spawn with the pool exhausted is
// skipped). The Rust trigger(dur) path is gone: the scheduler sends noteOn and a later noteOff, so
// only the held/noteOff envelope path remains. Deviation: retriggering a held pitch is ignored as in
// the browser, but the extra note id is remembered so the voice releases once every id is off.
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/NoteExpr.h"

namespace ddaw::devices {
namespace {

// Index in schema::kInstGranular.
enum P : uint16_t { Size, Dens, Pos, Spray, Pitch, Pjit, Rev, Shape, Spread, Attack, Release };

constexpr int kMaxVoices = 10;    // browser device's MAX_VOICES
constexpr int kMaxGrains = 256;   // shared pool; spawns skip when exhausted
constexpr int kMaxIds = 4;        // note ids tracked per voice (same-pitch retriggers)
constexpr float kOutGain = 0.9f;
constexpr float kWinPeak = 0.8f;
constexpr float kLevelFloor = 1e-4f;
constexpr uint64_t kRngSeed = 0x9E3779B97F4A7C15ull;
constexpr double kInf = std::numeric_limits<double>::infinity();

// xorshift64*, the generator family the Rust scheduler uses.
struct XorShift {
    uint64_t s = kRngSeed;
    void seed(uint64_t v) { s = v == 0 ? kRngSeed : v; }
    uint64_t nextU64() {
        uint64_t x = s;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        s = x;
        return x * 0x2545F4914F6CDD1Dull;
    }
    float nextF32() { return static_cast<float>(nextU64() >> 40) / static_cast<float>(1u << 24); }  // [0, 1)
    float nextPm1() { return nextF32() * 2.0f - 1.0f; }                                              // [-1, 1)
};

// Web Audio StereoPannerNode stereo law (grains pan post-window).
inline void stereoPan(float& l, float& r, float pan) {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    if (p == 0.0f) return;
    const float x = p <= 0.0f ? p + 1.0f : p;
    const float gl = std::cos(x * 1.5707963267948966f), gr = std::sin(x * 1.5707963267948966f);
    if (p <= 0.0f) {
        l = l + r * gl;
        r = r * gr;
    } else {
        const float ol = l * gl;
        r = r + l * gr;
        l = ol;
    }
}

struct Voice {
    bool active = false;
    bool releasing = false;
    uint8_t pitch = 0;
    float vel = 0.0f;
    double elapsed = 0.0;    // engine samples since start
    double atk = 1.0;        // samples
    double rel = 1.0;        // samples
    double relEnd = kInf;    // elapsed when grain spawning stops (noteOff)
    float expCoeff = 0.0f;   // setTargetAtTime tau = rel / 3
    float curLevel = 0.0f;
    double spawnNext = 0.0;  // elapsed time of the next grain
    std::array<uint32_t, kMaxIds> ids{};
    int numIds = 0;
    dsp::NoteExpr ex;        // MPE: bend retunes the grains spawned from now on, pressure the level
};

struct Grain {
    bool active = false;
    uint8_t voice = 0;
    double pos = 0.0;    // source frame (signed step handles reverse)
    double step = 0.0;   // source frames per engine sample
    double t = 0.0;      // engine samples since grain start (negative for mid-block spawns)
    double len = 1.0;    // engine samples
    double peak = 0.5;   // engine samples to the window peak
    float pan = 0.0f;
};

class GranularInst final : public InstrumentDevice {
public:
    std::span<const ParamSpec> params() const override { return schema::kInstGranular; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        reset();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Size: size_ = v; break;
            case Dens: dens_ = v; break;
            case Pos: pos_ = std::clamp(v, 0.0f, 1.0f); break;
            case Spray: spray_ = std::clamp(v, 0.0f, 1.0f); break;
            case Pitch: pitch_ = v; break;
            case Pjit: pjit_ = std::max(v, 0.0f); break;
            case Rev: rev_ = std::clamp(v, 0.0f, 1.0f); break;
            case Shape: shape_ = v; break;
            case Spread: spread_ = std::clamp(v, 0.0f, 1.0f); break;
            case Attack: attack_ = std::max(v, 0.0f); break;
            case Release: release_ = std::max(v, 0.0f); break;
            default: break;
        }
    }

    // Builder thread, before the graph is published. Only slot 0 is used.
    void setSample(uint32_t slot, SamplePtr buf) override {
        if (slot != 0) return;
        for (auto& v : voices_) v.active = false;
        for (auto& g : grains_) g.active = false;
        buf_ = std::move(buf);
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override {
        if (!buf_) return;
        // browser: a held pitch ignores its retrigger; remember the id so its off still balances
        for (auto& v : voices_) {
            if (v.active && !v.releasing && v.pitch == pitch) {
                if (v.numIds < kMaxIds) v.ids[static_cast<size_t>(v.numIds++)] = noteId;
                return;
            }
        }
        int vi = -1;
        for (int i = 0; i < kMaxVoices; ++i)
            if (!voices_[static_cast<size_t>(i)].active) { vi = i; break; }
        if (vi < 0) return;  // browser device: at MAX_VOICES live voices new notes are dropped

        const double sr = sr_;
        const float atkS = std::max(attack_, 0.003f);
        const float relS = std::max(release_, 0.02f);
        Voice& v = voices_[static_cast<size_t>(vi)];
        v = Voice{};
        v.active = true;
        v.pitch = pitch;
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.atk = std::max(static_cast<double>(atkS) * sr, 1.0);
        v.rel = std::max(static_cast<double>(relS) * sr, 1.0);
        v.expCoeff = std::exp(-3.0f / (relS * std::max(sr_, 1.0f)));
        v.ids[0] = noteId;
        v.numIds = 1;
    }

    void noteExpression(uint32_t noteId, int dimension, float value) override {
        for (auto& v : voices_) {
            if (!v.active) continue;
            for (int k = 0; k < v.numIds; ++k) if (v.ids[static_cast<size_t>(k)] == noteId) { v.ex.set(dimension, value); break; }
        }
    }

    void noteOff(uint32_t noteId) override {
        for (auto& v : voices_) {
            if (!v.active || v.releasing) continue;
            for (int k = 0; k < v.numIds; ++k) {
                if (v.ids[static_cast<size_t>(k)] != noteId) continue;
                v.ids[static_cast<size_t>(k)] = v.ids[static_cast<size_t>(v.numIds - 1)];
                --v.numIds;
                if (v.numIds == 0) {
                    v.releasing = true;
                    v.relEnd = v.elapsed + v.rel;  // clock.stop(now + rel)
                }
                break;
            }
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        if (!buf_) return;
        int done = 0;
        while (done < n) {
            const int m = std::min(n - done, kMaxBlock);
            processBlock(l + done, r + done, m);
            done += m;
        }
    }

    void reset() override {
        for (auto& v : voices_) v = Voice{};
        for (auto& g : grains_) g.active = false;
        rng_.seed(kRngSeed);
    }

private:
    void spawnGrain(size_t vi, double delay) {
        const SampleBuf* buf = buf_.get();
        if (!buf) return;
        const size_t frames = buf->frames();
        if (frames == 0) return;
        Grain* g = nullptr;
        for (auto& gr : grains_)
            if (!gr.active) { g = &gr; break; }
        if (!g) return;  // pool exhausted: skip this grain, keep running

        const double srcSr = std::max(buf->sampleRate, 1.0f);
        const double bufDur = static_cast<double>(frames) / srcSr;
        const double engSr = std::max(sr_, 1.0f);

        const double size = std::clamp(size_, 0.02f, 0.5f);
        const float p = static_cast<float>(voices_[vi].pitch);
        const double rate = std::pow(2.0f, (p - 60.0f + pitch_ + voices_[vi].ex.bendS + rng_.nextPm1() * pjit_) / 12.0f);
        double offset = static_cast<double>(pos_) * bufDur + static_cast<double>(rng_.nextPm1()) * spray_ * bufDur * 0.5;
        const bool rev = rng_.nextF32() < rev_;
        if (rev) offset = bufDur - offset - size * rate;
        const double startSec = std::clamp(offset, 0.0, std::max(bufDur - 0.02, 0.0));
        double pos0, step;
        if (rev) {
            pos0 = (static_cast<double>(frames) - 1.0) - startSec * srcSr;
            step = -(rate * srcSr / engSr);
        } else {
            pos0 = startSec * srcSr;
            step = rate * srcSr / engSr;
        }
        const double shape = std::clamp(shape_, 0.03f, 0.97f);
        const double len = std::max(size * engSr, 2.0);
        g->active = true;
        g->voice = static_cast<uint8_t>(vi);
        g->pos = pos0;
        g->step = step;
        g->t = -delay;
        g->len = len;
        g->peak = std::clamp(len * shape, 1.0, len - 1.0);
        g->pan = rng_.nextPm1() * spread_;
    }

    // Advance a voice one sample. Returns false when the voice just died.
    static bool voiceLevel(Voice& v, float& out) {
        float lvl;
        if (v.releasing) {
            v.curLevel *= v.expCoeff;
            if (v.curLevel < kLevelFloor) return false;
            lvl = v.curLevel;
        } else if (v.elapsed < v.atk) {
            v.curLevel = v.vel * static_cast<float>(v.elapsed / v.atk);
            lvl = v.curLevel;
        } else {
            v.curLevel = v.vel;
            lvl = v.vel;
        }
        v.elapsed += 1.0;
        v.ex.step();
        out = lvl * v.ex.gain();
        return true;
    }

    void processBlock(float* l, float* r, int m) {
        const SampleBuf* buf = buf_.get();
        if (!buf) return;
        const double engSr = std::max(sr_, 1.0f);
        const double interval = engSr / static_cast<double>(std::clamp(dens_, 2.0f, 80.0f));

        // 1) voice envelopes for the block + grain spawning
        for (size_t vi = 0; vi < kMaxVoices; ++vi) {
            if (!voices_[vi].active) continue;
            for (;;) {
                const Voice& v = voices_[vi];
                const double due = v.spawnNext;
                if (due >= v.elapsed + m || due > v.relEnd) break;
                const double delay = std::max(due - v.elapsed, 0.0);
                voices_[vi].spawnNext = due + interval;
                spawnGrain(vi, delay);
            }
            float* lv = &levels_[vi * kMaxBlock];
            int diedAt = -1;
            for (int k = 0; k < m; ++k) {
                float lvl;
                if (voiceLevel(voices_[vi], lvl)) lv[k] = lvl;
                else { diedAt = k; break; }
            }
            if (diedAt >= 0) {
                for (int k = diedAt; k < m; ++k) lv[k] = 0.0f;
                voices_[vi].active = false;
                for (auto& g : grains_)
                    if (g.active && g.voice == vi) g.active = false;  // env is 0: cut the dead voice's grains
            }
        }

        // 2) grain-major accumulation
        for (auto& g : grains_) {
            if (!g.active) continue;
            const float* lv = &levels_[static_cast<size_t>(g.voice) * kMaxBlock];
            for (int k = 0; k < m; ++k) {
                g.t += 1.0;
                const double t = g.t;
                if (t < 0.0) continue;  // grain starts mid-block
                if (t >= g.len) { g.active = false; break; }
                const float win = t < g.peak ? kWinPeak * static_cast<float>(t / g.peak)
                                             : kWinPeak * static_cast<float>(1.0 - (t - g.peak) / (g.len - g.peak));
                float sl, sr;
                buf->readLin(g.pos, sl, sr);
                g.pos += g.step;
                float pl = sl * win, pr = sr * win;
                stereoPan(pl, pr, g.pan);
                const float lvl = lv[k] * kOutGain;
                l[k] += pl * lvl;
                r[k] += pr * lvl;
            }
        }
    }

    float sr_ = 44100.0f;
    SamplePtr buf_;
    // frozen schema params (the Rust device does not smooth them)
    float size_ = 0.12f, dens_ = 18.0f, pos_ = 0.15f, spray_ = 0.05f, pitch_ = 0.0f, pjit_ = 0.0f, rev_ = 0.0f,
          shape_ = 0.5f, spread_ = 0.6f, attack_ = 0.05f, release_ = 0.4f;
    std::array<Voice, kMaxVoices> voices_{};
    std::array<Grain, kMaxGrains> grains_{};
    std::array<float, kMaxVoices * kMaxBlock> levels_{};
    XorShift rng_;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_granular() { return std::make_unique<GranularInst>(); }

}  // namespace ddaw::devices
