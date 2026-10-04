// `drum` instrument: 8 procedural pads (kick, snare, clap, closed/open hat, lo tom, perc, crash). Port of
// sf-dsp/src/inst/drum.rs (makeDrum()).
//  - membrane pads (kick @ MIDI 36, lo tom @ 48): sine with an exponential pitch drop, starting at
//    hz * 6 and ramping to hz over `pitchDecay` (Tone.MembraneSynth setNote).
//  - noise pads: white noise through a bandpass (snare 1800 Hz Q 0.8, clap 1100 Hz Q 1) or highpass
//    (closed hat 9 kHz, open hat 8 kHz, crash 6.5 kHz on pink noise); `tune` retunes the filter live.
//  - clap: three bursts (t, +12 ms, +26 ms at 0.7/0.85/1.0 x vel) on one noise voice.
//  - perc: short triangle blip at 1100 Hz.
// Envelopes are a 1 ms attack with exponential decay to 0, so a pad always rings out and noteOff is a
// no-op. One mono voice per pad (a retrigger steals that pad's own voice); pitch selects pad = pitch % 8.
// Params are p{i}_{tune,decay,level}, table order i*3 + {0,1,2}.
// Sample pads (setSample(slot 0-7), from the sample bank): a pad with an injected sample plays that
// sample INSTEAD of its synthesized sound (browser samplePad()): one-shot from the head, the FULL
// sample always plays (`decay` is a no-op), `tune` repitches by 2^(tune/12) read at trigger time,
// `level` is the pad gain, velocity scales it, 1 ms attack. The source rate is folded into the read
// step (srcRate / engineRate). The sample shares the pad's mono voice, so a retrigger restarts it.
// Pads without a sample keep the synthesized sound; setSample(slot, null) restores it. Slots >= 8
// are ignored. A mono source is read on both sides, a stereo source keeps its image.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "core/Device.h"
#include "core/Sample.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Math.h"
#include "dsp/PolyBlepOsc.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

constexpr int kPadCount = 8;
constexpr float kAttackS = 0.001f;
constexpr float kEnvExpMult = 6.2f;  // exp decay reaches ~-54 dB at the nominal decay time
constexpr float kEnvFloor = 1e-4f;   // about -80 dB: the voice is over
constexpr float kMembraneOctaves = 6.0f;
constexpr std::array<float, 3> kClapVels = {0.7f, 0.85f, 1.0f};
constexpr std::array<float, 2> kClapGapsS = {0.012f, 0.014f};

enum class Kind { Membrane, Noise, Blip };

struct PadKind {
    Kind kind;
    float baseMidi;    // membrane
    float pitchDecay;  // membrane
    float centerHz;    // noise centre / cutoff, blip base frequency
    SvfMode mode;
    float q;
    bool pink, clap;
};

constexpr std::array<PadKind, kPadCount> kPadKinds = {{
    {Kind::Membrane, 36.0f, 0.05f, 0.0f, SvfMode::Lowpass, 1.0f, false, false},      // kick
    {Kind::Noise, 0.0f, 0.0f, 1800.0f, SvfMode::Bandpass, 0.8f, false, false},      // snare
    {Kind::Noise, 0.0f, 0.0f, 1100.0f, SvfMode::Bandpass, 1.0f, false, true},       // clap
    {Kind::Noise, 0.0f, 0.0f, 9000.0f, SvfMode::Highpass, 1.0f, false, false},      // closed hat
    {Kind::Noise, 0.0f, 0.0f, 8000.0f, SvfMode::Highpass, 1.0f, false, false},      // open hat
    {Kind::Membrane, 48.0f, 0.03f, 0.0f, SvfMode::Lowpass, 1.0f, false, false},      // lo tom
    {Kind::Blip, 0.0f, 0.0f, 1100.0f, SvfMode::Lowpass, 1.0f, false, false},         // perc
    {Kind::Noise, 0.0f, 0.0f, 6500.0f, SvfMode::Highpass, 1.0f, true, false},       // crash
}};

constexpr std::array<float, kPadCount> kPadDecayDef = {0.42f, 0.18f, 0.28f, 0.06f, 0.4f, 0.32f, 0.12f, 1.3f};

enum class Stage { Idle, Attack, Decay };

struct Voice {
    Stage stage = Stage::Idle;
    float env = 0.0f, attackStep = 0.0f, attackTarget = 0.0f, decayCoeff = 0.0f, vel = 0.0f;
    PolyBlepOsc osc{Wave::Sine};
    float freq = 0.0f, freqEnd = 0.0f, sweepRatio = 1.0f;
    uint32_t sweepLeft = 0;
    uint32_t rng = 1, seed = 1;
    std::array<float, 3> pinkState{};
    Svf filt{SvfMode::Lowpass};
    uint8_t burstsFired = 3;  // 3 => none pending
    uint32_t nextBurstIn = 0;
    // sample playback (browser samplePad); pos / rate are in SOURCE frames
    bool sampleActive = false;
    double pos = 0.0, rate = 1.0;

    void init(const PadKind& k, int idx) {
        const Wave w = k.kind == Kind::Blip ? Wave::Triangle : Wave::Sine;
        osc = PolyBlepOsc(w);
        filt = Svf(k.kind == Kind::Noise ? k.mode : SvfMode::Lowpass);
        seed = rng = (0x9E3779B9u * static_cast<uint32_t>(idx + 1)) | 1u;
    }

    void reset() {
        stage = Stage::Idle;
        env = 0.0f;
        sweepLeft = 0;
        burstsFired = 3;
        nextBurstIn = 0;
        sampleActive = false;
        pos = 0.0;
        pinkState = {};
        filt.reset();
        osc.resetPhase(0.0f);
        rng = seed;  // deterministic restart after reset()
    }

    // Linear 1 ms attack from the CURRENT level (Tone retrigger semantics: clap bursts must not click).
    void startAttack(float target, float sr) {
        const float n = std::max(kAttackS * sr, 1.0f);
        attackTarget = target;
        attackStep = (target - env) / n;
        stage = Stage::Attack;
    }

    float envNext() {
        switch (stage) {
            case Stage::Idle: return 0.0f;
            case Stage::Attack: {
                env += attackStep;
                const bool done = attackStep >= 0.0f ? env >= attackTarget : env <= attackTarget;
                if (done) { env = attackTarget; stage = Stage::Decay; }
                return env;
            }
            case Stage::Decay:
                env *= decayCoeff;
                if (env < kEnvFloor) { env = 0.0f; stage = Stage::Idle; }
                return env;
        }
        return 0.0f;
    }

    // xorshift32 white noise in [-1, 1)
    float white() {
        uint32_t x = rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng = x;
        return static_cast<float>(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    // Paul Kellet economy pink filter (crash uses Tone's pink noise)
    float pinkNext(float w) {
        pinkState[0] = 0.99765f * pinkState[0] + w * 0.099046f;
        pinkState[1] = 0.963f * pinkState[1] + w * 0.2965164f;
        pinkState[2] = 0.57f * pinkState[2] + w * 1.0526913f;
        return (pinkState[0] + pinkState[1] + pinkState[2] + w * 0.1848f) * 0.25f;
    }
};

struct Pad {
    PadKind kind{};
    float tune = 0.0f, decay = 0.2f;
    Smoother tuneSm, levelSm;
    SamplePtr sample;  // injected one-shot override (null: synthesize)
    Voice voice;
};

class DrumInst final : public InstrumentDevice {
public:
    DrumInst() {
        for (int i = 0; i < kPadCount; ++i) {
            Pad& p = pads_[static_cast<size_t>(i)];
            p.kind = kPadKinds[static_cast<size_t>(i)];
            p.decay = kPadDecayDef[static_cast<size_t>(i)];
            p.tuneSm.prepare(sr_, 15.0f);
            p.levelSm.prepare(sr_, 15.0f);
            p.tuneSm.snap(0.0f);
            p.levelSm.snap(1.0f);  // 0 dB default
            p.voice.init(p.kind, i);
        }
    }

    std::span<const ParamSpec> params() const override { return schema::kInstDrum; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        for (auto& p : pads_) {
            p.tuneSm.prepare(sr_, 15.0f);
            p.levelSm.prepare(sr_, 15.0f);
            p.voice.osc.prepare(sr_);
            p.voice.filt.prepare(sr_);
            p.voice.reset();
        }
    }

    // Control path (builder thread, before the graph is published). Silences the pad so a ringing voice
    // never reads across the swap; the audio thread only reads the pointer.
    void setSample(uint32_t slot, SamplePtr buf) override {
        if (slot >= pads_.size()) return;
        Pad& p = pads_[slot];
        p.sample = std::move(buf);
        p.voice.reset();
    }

    void setParam(uint16_t index, float v) override {
        const size_t padIdx = index / 3;
        if (padIdx >= pads_.size()) return;
        Pad& p = pads_[padIdx];
        switch (index % 3) {
            case 0: p.tune = std::clamp(v, -12.0f, 12.0f); p.tuneSm.setTarget(p.tune); break;
            case 1: p.decay = std::clamp(v, 0.03f, 2.0f); break;
            case 2: p.levelSm.setTarget(dbToLin(std::clamp(v, -24.0f, 6.0f))); break;
            default: break;
        }
    }

    void noteOn(uint8_t pitch, float velocity, uint32_t) override { trig(pitch, velocity); }

    // One-shot pads ring out on their own decay (browser noteOff is a no-op).
    void noteOff(uint32_t) override {}

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        const float sr = sr_;
        // block rate: retune ringing noise filters from the smoothed tune
        for (auto& p : pads_) {
            if (p.kind.kind == Kind::Noise && !p.sample && p.voice.stage != Stage::Idle) {
                const float cutoff = p.kind.centerHz * std::pow(2.0f, p.tuneSm.current() / 12.0f);
                p.voice.filt.setCutoffQ(cutoff, p.kind.q);
            }
        }
        for (int i = 0; i < n; ++i) {
            float mix = 0.0f, mixR = 0.0f;
            for (auto& p : pads_) {
                p.tuneSm.next();
                const float gain = p.levelSm.next();
                Voice& v = p.voice;
                // pending clap bursts retrigger the envelope mid-decay
                if (v.burstsFired < 3) {
                    if (v.nextBurstIn == 0) {
                        const size_t k = v.burstsFired;
                        v.startAttack(v.vel * kClapVels[k], sr);
                        ++v.burstsFired;
                        if (k < kClapGapsS.size()) v.nextBurstIn = static_cast<uint32_t>(kClapGapsS[k] * sr);
                    } else {
                        --v.nextBurstIn;
                    }
                }
                if (v.stage == Stage::Idle) continue;
                if (v.sampleActive) {
                    // one-shot read; the voice ends at the buffer end (decay never gates it)
                    const SampleBuf* b = p.sample.get();
                    if (!b || v.pos >= static_cast<double>(b->frames())) {
                        v.env = 0.0f;
                        v.stage = Stage::Idle;
                        v.sampleActive = false;
                        continue;
                    }
                    float sl, sr2;
                    b->readLin(v.pos, sl, sr2);
                    v.pos += v.rate;
                    const float g = v.envNext() * gain;
                    mix += sl * g;
                    mixR += sr2 * g;
                    continue;
                }
                float m;
                switch (p.kind.kind) {
                    case Kind::Membrane:
                        if (v.sweepLeft > 0) {
                            v.freq *= v.sweepRatio;
                            if (--v.sweepLeft == 0) v.freq = v.freqEnd;
                            v.osc.setFreq(v.freq);
                        }
                        m = v.osc.next();
                        break;
                    case Kind::Blip: m = v.osc.next(); break;
                    case Kind::Noise: default: {
                        const float w = v.white();
                        const float x = p.kind.pink ? v.pinkNext(w) : w;
                        const float y = v.filt.processSample(x);
                        // SVF band output peaks at Q; a biquad bandpass peaks at 1: normalise so q shapes width
                        m = p.kind.mode == SvfMode::Bandpass ? y / p.kind.q : y;
                        break;
                    }
                }
                const float synth = m * v.envNext() * gain;
                mix += synth;
                mixR += synth;
            }
            l[i] += mix;
            r[i] += mixR;
        }
    }

    void reset() override {
        for (auto& p : pads_) {
            p.voice.reset();
            p.tuneSm.snap(p.tuneSm.target());
            p.levelSm.snap(p.levelSm.target());
        }
    }

private:
    void trig(uint8_t pitch, float velocity) {
        const float sr = sr_;
        Pad& pad = pads_[pitch % kPadCount];
        Voice& v = pad.voice;
        v.vel = std::clamp(velocity, 0.0f, 1.0f);
        v.decayCoeff = std::exp(-kEnvExpMult / (pad.decay * sr));
        v.burstsFired = 3;
        if (const SampleBuf* b = pad.sample.get()) {
            // browser samplePad: one-shot from the top, tune -> playbackRate 2^(tune/12) read at trigger
            // time; decay coeff 1.0 holds the envelope at vel after the 1 ms attack.
            v.sampleActive = true;
            v.pos = 0.0;
            v.rate = static_cast<double>(b->sampleRate) / static_cast<double>(sr) * std::pow(2.0, static_cast<double>(pad.tune) / 12.0);
            v.decayCoeff = 1.0f;
            v.startAttack(v.vel, sr);
            return;
        }
        v.sampleActive = false;
        switch (pad.kind.kind) {
            case Kind::Membrane: {
                const float base = midiHz(pad.kind.baseMidi + pad.tune);
                const float n = std::max(pad.kind.pitchDecay * sr, 1.0f);
                v.freq = base * kMembraneOctaves;
                v.freqEnd = base;
                v.sweepLeft = static_cast<uint32_t>(n);
                v.sweepRatio = std::pow(1.0f / kMembraneOctaves, 1.0f / n);
                v.osc.setFreq(v.freq);
                v.startAttack(v.vel, sr);
                break;
            }
            case Kind::Blip:
                v.osc.setFreq(pad.kind.centerHz * std::pow(2.0f, pad.tune / 12.0f));
                v.startAttack(v.vel, sr);
                break;
            case Kind::Noise: {
                const float cutoff = pad.kind.centerHz * std::pow(2.0f, pad.tuneSm.current() / 12.0f);
                v.filt.setCutoffQ(cutoff, pad.kind.q);
                if (pad.kind.clap) {
                    v.burstsFired = 1;
                    v.nextBurstIn = static_cast<uint32_t>(kClapGapsS[0] * sr);
                    v.startAttack(v.vel * kClapVels[0], sr);
                } else {
                    v.startAttack(v.vel, sr);
                }
                break;
            }
        }
    }

    float sr_ = 44100.0f;
    std::array<Pad, kPadCount> pads_;
};

}  // namespace

std::unique_ptr<InstrumentDevice> make_drum() { return std::make_unique<DrumInst>(); }

}  // namespace ddaw::devices
