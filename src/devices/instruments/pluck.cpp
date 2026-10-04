// `pluck` instrument: port of sf-dsp/src/inst/pluck.rs (Karplus-Strong string, Tone.PluckSynth).
// Per voice: pink-noise burst of one period -> one-pole lowpass at `dampen` -> feedback comb tuned to
// the note (y[n] = x[n] + res*y[n-D], output y[n-D]) -> 0.9 output gain. noteOff is a no-op (the
// browser device lets strings ring out). Fixed pool of 4 voices allocated round-robin, exactly like the
// browser (pool[i++ % 4]), which steals the oldest string once all four are in use. Delay lines are
// sized in prepare() for the lowest MIDI pitch.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Math.h"
#include "dsp/OnePole.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

// Index in schema::kInstPluck.
enum P : uint16_t { Dampen, Res };

constexpr int kNumVoices = 4;           // browser pool size
constexpr float kOutGain = 0.9f;        // new Tone.Gain(0.9)
constexpr float kSilenceFloor = 1.0e-6f;

size_t nextPow2(size_t v) {
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

struct Voice {
    std::vector<float> buf;
    size_t mask = 0, writePos = 0;
    float delay = 1.0f;
    uint32_t burstLeft = 0, quietRun = 0;
    float vel = 0.0f;
    bool active = false;
    OnePole lp{OnePoleMode::Lowpass};
    float lastCutoff = -1.0f;
    uint32_t rng = 1, seed = 1;
    float b0 = 0, b1 = 0, b2 = 0;

    void init(size_t maxDelay, uint32_t s) {
        seed = rng = s;
        resize(maxDelay);
    }
    void resize(size_t maxDelay) {
        const size_t len = nextPow2(std::max<size_t>(maxDelay, 8));
        if (len != buf.size()) buf.assign(len, 0.0f);  // prepare() may allocate; process() never does
        mask = len - 1;
    }
    void prepare(float sr, size_t maxDelay) {
        resize(maxDelay);
        lp.prepare(sr);
        clear();
    }
    void clear() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        writePos = 0;
        burstLeft = 0;
        quietRun = 0;
        active = false;
        lp.reset();
        lastCutoff = -1.0f;
        b0 = b1 = b2 = 0.0f;
        rng = seed;  // deterministic restart after reset()
    }
    void start(float delaySamples, float v) {
        // steal in place: silence the old string so the new note starts clean
        std::fill(buf.begin(), buf.end(), 0.0f);
        lp.reset();
        delay = std::clamp(delaySamples, 1.0f, static_cast<float>(buf.size() - 1));
        burstLeft = static_cast<uint32_t>(delay) + 1;  // noise burst of one period (attackNoise = 1)
        quietRun = 0;
        vel = std::clamp(v, 0.0f, 1.0f);
        active = true;
    }
    float pink() {
        uint32_t x = rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng = x;
        const float white = static_cast<float>(x) * (2.0f / 4294967296.0f) - 1.0f;
        // Kellet "economy" pink approximation, scaled to stay within +-1
        b0 = 0.99765f * b0 + white * 0.099046f;
        b1 = 0.963f * b1 + white * 0.2965164f;
        b2 = 0.57f * b2 + white * 1.0526913f;
        return (b0 + b1 + b2 + white * 0.1848f) * 0.28f;
    }
    // One Karplus-Strong step: the pre-gain voice output. Once the burst is spent and a full loop of
    // consecutive output samples sits below the silence floor the string is retired.
    float tick(float res, float dampenHz) {
        float x = 0.0f;
        if (burstLeft > 0) {
            --burstLeft;
            x = pink() * vel;
        }
        if (dampenHz != lastCutoff) { lp.setCutoff(dampenHz); lastCutoff = dampenHz; }
        const float filtered = lp.processSample(x);
        const float d = delay;
        const size_t di = static_cast<size_t>(d);
        const float frac = d - static_cast<float>(di);
        const size_t len = buf.size();
        const size_t i0 = (writePos + len - di) & mask;
        const size_t i1 = (i0 + len - 1) & mask;
        const float s0 = buf[i0], s1 = buf[i1];
        const float delayed = s0 + (s1 - s0) * frac;
        buf[writePos] = filtered + delayed * res;
        writePos = (writePos + 1) & mask;
        if (burstLeft == 0) {
            if (std::abs(delayed) < kSilenceFloor) {
                ++quietRun;
                if (static_cast<float>(quietRun) > delay + 2.0f) active = false;
            } else {
                quietRun = 0;
            }
        }
        return delayed;
    }
};

class PluckInst final : public InstrumentDevice {
public:
    PluckInst() {
        dampen_.prepare(sr_, 15.0f);
        res_.prepare(sr_, 15.0f);
        dampen_.snap(4000.0f);
        res_.snap(0.93f);
        for (size_t i = 0; i < voices_.size(); ++i) {
            voices_[i].init(maxDelay(sr_), 0x9e3779b9u * static_cast<uint32_t>(i + 1));
            voices_[i].prepare(sr_, maxDelay(sr_));
        }
    }

    std::span<const ParamSpec> params() const override { return schema::kInstPluck; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        const size_t md = maxDelay(sr_);
        for (auto& v : voices_) v.prepare(sr_, md);
        dampen_.prepare(sr_, 15.0f);
        res_.prepare(sr_, 15.0f);
        dampen_.snap(dampen_.target());
        res_.snap(res_.target());
        next_ = 0;
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Dampen: dampen_.setTarget(std::clamp(v, 10.0f, sr_ * 0.45f)); break;
            case Res: res_.setTarget(std::clamp(v, 0.0f, 0.9999f)); break;
            default: break;
        }
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t) override {
        const float delay = sr_ / midiHz(static_cast<float>(pitch));
        voices_[next_].start(delay, velocity);
        next_ = (next_ + 1) % kNumVoices;
    }

    // Browser device: noteOff is a no-op; plucks ring out on feedback decay alone.
    void noteOff(uint32_t) override {}

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        bool any = false;
        for (const auto& v : voices_) any = any || v.active;
        if (!any) {
            for (int i = 0; i < n; ++i) { dampen_.next(); res_.next(); }  // keep the param ramps moving
            return;
        }
        for (int i = 0; i < n; ++i) {
            const float dampen = dampen_.next();
            const float res = res_.next();
            float sum = 0.0f;
            for (auto& v : voices_)
                if (v.active) sum += v.tick(res, dampen);
            const float out = sum * kOutGain;
            l[i] += out;
            r[i] += out;
        }
    }

    void reset() override {
        for (auto& v : voices_) v.clear();
        next_ = 0;
        dampen_.snap(dampen_.target());
        res_.snap(res_.target());
    }

private:
    // Longest loop to hold: MIDI 0 = 8.176 Hz -> sr / 8.176 < sr / 8.
    static size_t maxDelay(float sr) { return static_cast<size_t>(sr * 0.125f) + 4; }

    float sr_ = 44100.0f;
    std::array<Voice, kNumVoices> voices_;
    size_t next_ = 0;
    Smoother dampen_, res_;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_pluck() { return std::make_unique<PluckInst>(); }

}  // namespace ddaw::devices
