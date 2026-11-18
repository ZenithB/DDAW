// `chordres` effect (Chord Resonator): turns the note being played into a chord with steep resonant filters.
//
// The tonic. With `source` Notes the tonic is the last note the track's instrument was told to play (the graph passes every note of
// the track to effects that ask, EffectDevice::wantsNotes): exact and with no delay. With Audio it is read from the sound: a 4096-sample
// Hann-windowed FFT (zero-padded to 8192) every 1024 samples, candidate peaks refined by parabolic interpolation and ranked by how
// much of a harmonic series sits on them (so an octave up does not win), accepted when two readings in a row agree, and held when the
// level falls below `thresh`. That has the delay of its window (about 90 ms), the price of an audio track. Auto uses the notes while the
// track has had one in the last 4 s, else the audio. Either way the tonic glides to a new value over `glide` ms (in semitones).
//
// The resonators. Up to six filters (`filters`, at the equal-tempered intervals i1..i6 above the tonic, -12..+36 semitones) and
// optionally one on the tonic itself. Each is a cascade of 2, 4, 6 or 8 two-pole band-pass sections (`steep`: 12, 24, 36, 48 dB/oct
// skirts) with unit gain at the centre, the per-section Q chosen so the -3 dB bandwidth of the cascade is f / Q whatever the steepness.
// They are state-variable filters in double precision, retuned smoothly every 16 samples. They work on the mono sum and their output
// goes to both channels.
//
// What they ring on. `excite` adds light harmonic distortion ahead of the filters (a soft 2nd and 3rd order shaper, DC removed): a
// pure tone has nothing at a major third above it, the shaper's harmonics give the filters something to boost.
//
// Boost and headroom. `boost` (dB, plus each filter's own level) raises each resonance. The more it is raised, (1) each resonance
// is compressed harder (threshold -6 - 0.1 boost dB, ratio 2 to 8, soft knee), so a loud resonance cannot run away, (2) the original
// is lowered by up to 6 dB, and (3) a soft ceiling rounds off the sum above 0.85 (blended in over the first 3 dB of boost).
// `mix` crossfades between the original and all of that; at 0 the effect is a bit-exact bypass.
//
// `sat`: a final saturation-and-compression stage, off (and bit-exact) at 0. A program-dependent compressor (threshold -16 dB, ratio up
// to 3.5:1, attack 8 ms, release 120 ms, only 65% of the reduction applied) into an asymmetric tanh saturator whose small-signal gain is 1, blended in
// as `sat` rises.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/SchemaExt.h"
#include "dsp/Fft.h"
#include "dsp/Math.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Source, Filters, Tonic, TonicLevel, I1, I2, I3, I4, I5, I6, L1, L2, L3, L4, L5, L6, Boost, Q, Steep, Excite, Sat, Mix, Glide, Thresh, kNumP };

constexpr int kFilters = 6, kSlots = kFilters + 1, kMaxStages = 8, kChunk = 16;
constexpr int kWin = 4096, kFft = 8192, kHop = 1024, kRing = 8192;
constexpr float kNoteWindowSec = 4.0f;       // Auto: notes count as the source for this long after the last one
constexpr float kLoHz = 55.0f, kHiHz = 2000.0f;

struct Resonator {
    double a1 = 1, a2 = 0, a3 = 0, k = 1;
    std::array<double, kMaxStages> ic1{}, ic2{};
    float env = 0.0f, gain = 1.0f;    // the per-resonance compressor
    bool wasOn = false;
    void clear() { ic1.fill(0.0); ic2.fill(0.0); env = 0.0f; gain = 1.0f; }
    void tune(double g, double kk) { k = kk; a1 = 1.0 / (1.0 + g * (g + kk)); a2 = g * a1; a3 = g * a2; }
    // Unit-gain band-pass cascade of n sections.
    double run(double x, int n) noexcept {
        for (int s = 0; s < n; ++s) {
            const double v3 = x - ic2[size_t(s)];
            const double v1 = a1 * ic1[size_t(s)] + a2 * v3;
            const double v2 = ic2[size_t(s)] + a2 * ic1[size_t(s)] + a3 * v3;
            ic1[size_t(s)] = 2.0 * v1 - ic1[size_t(s)];
            ic2[size_t(s)] = 2.0 * v2 - ic2[size_t(s)];
            x = v1 * k;
        }
        return x;
    }
};

inline float dbOf(float lin) noexcept { return 20.0f * std::log10(std::max(lin, 1e-9f)); }
inline float linOf(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
// soft ceiling: transparent below 0.85, rounded above, never past 1
inline float ceilingOf(float x) noexcept {
    const float a = std::abs(x);
    return a < 0.85f ? x : std::copysign(0.85f + 0.15f * std::tanh((a - 0.85f) * (1.0f / 0.15f)), x);
}
// Soft-knee compressor gain reduction (dB, >= 0) for a level `over` dB above the threshold.
inline float reductionOf(float over, float ratio, float knee) noexcept {
    if (over <= -0.5f * knee) return 0.0f;
    const float slope = 1.0f - 1.0f / ratio;
    if (over >= 0.5f * knee) return over * slope;
    const float t = over + 0.5f * knee;
    return slope * t * t / (2.0f * knee);
}

class ChordRes final : public EffectDevice {
public:
    ChordRes() {
        for (const auto& ps : schema::kFxChordres) setParam(ps.index, ps.def);
        snapAll();
    }

    std::span<const ParamSpec> params() const override { return schema::kFxChordres; }

    void prepare(double sr, int) override {
        sr_ = float(std::max(sr, 8000.0));
        fft_.prepare(kFft);
        hann_.resize(size_t(kWin));
        for (int i = 0; i < kWin; ++i) hann_[size_t(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(kWin));
        for (auto& s : sm_) s.prepare(sr_, 15.0f);
        for (int i = 0; i < kNumP; ++i) setParam(uint16_t(i), ctl_[i]);
        compAtt_ = std::exp(-1.0f / (0.0005f * sr_));
        compRel_ = std::exp(-1.0f / (0.02f * sr_));
        peakRel_ = std::exp(-1.0f / (0.08f * sr_));
        satAtt_ = std::exp(-1.0f / (0.008f * sr_));
        satRel_ = std::exp(-1.0f / (0.12f * sr_));
        dcK_ = 1.0f - 6.2831853f * 20.0f / sr_;
        reset();
    }

    void setParam(uint16_t i, float v) override {
        if (i >= kNumP) return;
        const ParamSpec& ps = schema::kFxChordres[i];
        v = std::clamp(v, ps.min, ps.max);
        ctl_[i] = v;
        if (ps.curve == Curve::Stepped) return;      // read straight from ctl_ in process()
        sm_[i].setTarget(v);
    }

    bool wantsNotes() const noexcept override { return true; }
    void noteOn(uint8_t pitch, float, uint32_t id) override {
        noteAge_ = 0;
        for (int k = 0; k < nHeld_; ++k) if (held_[size_t(k)].id == id) { for (int j = k; j + 1 < nHeld_; ++j) held_[size_t(j)] = held_[size_t(j + 1)]; --nHeld_; break; }
        if (nHeld_ == kHeld) { for (int j = 0; j + 1 < nHeld_; ++j) held_[size_t(j)] = held_[size_t(j + 1)]; --nHeld_; }
        held_[size_t(nHeld_++)] = {id, pitch};
        noteTonic_ = float(pitch);
    }
    void noteOff(uint32_t id) override {
        for (int k = 0; k < nHeld_; ++k)
            if (held_[size_t(k)].id == id) {
                for (int j = k; j + 1 < nHeld_; ++j) held_[size_t(j)] = held_[size_t(j + 1)];
                --nHeld_;
                break;
            }
        if (nHeld_ > 0) noteTonic_ = float(held_[size_t(nHeld_ - 1)].pitch);    // back to the note still held; else keep ringing on the last
    }

    bool hasInfo() const noexcept override { return true; }
    float info() const noexcept override { return midiCur_; }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int at = 0; at < n; at += kChunk) chunk(l + at, r + at, std::min(kChunk, n - at));
    }

    void reset() override {
        for (auto& rs : res_) rs.clear();
        ring_.fill(0.0f);
        ringPos_ = 0; sinceDetect_ = 0;
        nHeld_ = 0; noteTonic_ = -1.0f; noteAge_ = 1 << 30;
        audioTonic_ = -1.0f; lastCand_ = -1.0f; agree_ = 0;
        midiCur_ = -1.0f;
        dcX_ = dcY_ = 0.0f;
        satEnv_ = 0.0f;
        snapAll();
    }

private:
    struct Held { uint32_t id; uint8_t pitch; };
    static constexpr int kHeld = 16;

    void snapAll() { for (auto& s : sm_) s.snap(s.target()); }
    static float adv(Smoother& s, int n) { for (int i = 0; i < n; ++i) s.next(); return s.current(); }

    // ---- pitch from audio ----
    void detect() {
        const int start = (ringPos_ + kRing - kWin) & (kRing - 1);
        double e = 0.0;
        for (int i = 0; i < kWin; ++i) { const double x = double(ring_[size_t((start + i) & (kRing - 1))]); re_[size_t(i)] = x * hann_[size_t(i)]; im_[size_t(i)] = 0.0; e += x * x; }
        const float rmsDb = dbOf(float(std::sqrt(e / kWin)));
        if (rmsDb < ctl_[Thresh]) { agree_ = 0; return; }                // too quiet: hold the last tonic
        for (int i = kWin; i < kFft; ++i) { re_[size_t(i)] = 0.0; im_[size_t(i)] = 0.0; }
        fft_.forward(re_.data(), im_.data());
        const double binHz = double(sr_) / double(kFft);
        const int lo = std::max(2, int(kLoHz / binHz)), hi = std::min(kFft / 2 - 2, int(kHiHz / binHz));
        double gmax = 1e-12;
        for (int k = 1; k < kFft / 2; ++k) { mag_[size_t(k)] = std::hypot(re_[size_t(k)], im_[size_t(k)]); }
        for (int k = lo; k <= hi; ++k) gmax = std::max(gmax, mag_[size_t(k)]);
        // candidates: local maxima above a fraction of the strongest peak
        struct Cand { double bin, score; };
        std::array<Cand, 12> cands{};
        int nc = 0;
        for (int k = lo + 1; k < hi && nc < 12; ++k) {
            const double m = mag_[size_t(k)];
            if (m < 0.12 * gmax || m < mag_[size_t(k - 1)] || m <= mag_[size_t(k + 1)]) continue;
            const double a = std::log(mag_[size_t(k - 1)] + 1e-12), b = std::log(m + 1e-12), c = std::log(mag_[size_t(k + 1)] + 1e-12);
            const double den = a - 2.0 * b + c;
            const double off = std::abs(den) < 1e-12 ? 0.0 : std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
            cands[size_t(nc++)] = {double(k) + off, 0.0};
        }
        if (nc == 0) { agree_ = 0; return; }
        static constexpr double w[6] = {1.0, 0.8, 0.7, 0.6, 0.5, 0.4};
        double best = 0.0;
        for (int c = 0; c < nc; ++c) {
            double s = 0.0;
            for (int h = 1; h <= 6; ++h) {
                const int kc = int(std::lround(cands[size_t(c)].bin * h));
                if (kc + 2 >= kFft / 2) break;
                double m = 0.0;
                for (int d = -2; d <= 2; ++d) m = std::max(m, mag_[size_t(kc + d)]);
                s += w[h - 1] * m;
            }
            cands[size_t(c)].score = s;
            best = std::max(best, s);
        }
        double bin = 0.0;
        for (int c = 0; c < nc; ++c) if (cands[size_t(c)].score >= 0.75 * best) { bin = cands[size_t(c)].bin; break; }   // the lowest well-supported one
        const float midi = 69.0f + 12.0f * std::log2(float(bin * binHz) / 440.0f);
        if (lastCand_ >= 0.0f && std::abs(midi - lastCand_) < 0.6f) ++agree_; else agree_ = 1;
        lastCand_ = midi;
        if (agree_ >= 2) audioTonic_ = midi;                              // two readings in a row agree: a note, not a glitch
    }

    void chunk(float* l, float* r, int n) {
        const int stepped = int(std::lround(ctl_[Source]));
        const int nf = std::clamp(int(std::lround(ctl_[Filters])), 1, kFilters);
        const bool tonicOn = ctl_[Tonic] >= 0.5f;
        const int stages = 2 * (std::clamp(int(std::lround(ctl_[Steep])), 0, 3) + 1);
        // smoothed controls (block rate)
        const float boost = adv(sm_[Boost], n), q = adv(sm_[Q], n), excite = adv(sm_[Excite], n), glideMs = adv(sm_[Glide], n);
        const float tonicLevel = adv(sm_[TonicLevel], n);
        float lev[kFilters];
        for (int j = 0; j < kFilters; ++j) lev[j] = adv(sm_[size_t(L1 + j)], n);
        adv(sm_[Thresh], n);

        // the audio the pitch tracker reads
        for (int i = 0; i < n; ++i) { ring_[size_t(ringPos_)] = 0.5f * (l[i] + r[i]); ringPos_ = (ringPos_ + 1) & (kRing - 1); }
        noteAge_ = std::min(noteAge_ + n, 1 << 30);
        const bool useNotes = stepped == 1 || (stepped == 0 && float(noteAge_) < kNoteWindowSec * sr_);
        if (!useNotes) {
            sinceDetect_ += n;
            if (sinceDetect_ >= kHop) { sinceDetect_ = 0; detect(); }
        }
        const float target = useNotes ? noteTonic_ : audioTonic_;
        if (target >= 0.0f) {
            if (midiCur_ < 0.0f) midiCur_ = target;                          // the first note: no glide from nowhere
            else midiCur_ += (target - midiCur_) * (1.0f - std::exp(-float(n) / (std::max(glideMs, 1.0f) * 0.001f * sr_)));
        }
        const bool have = midiCur_ >= 0.0f;
        const double tonicHz = have ? 440.0 * std::pow(2.0, (double(midiCur_) - 69.0) / 12.0) : 0.0;

        // tune the resonators
        const double nyq = 0.45 * double(sr_);
        const double sec = std::sqrt(std::pow(2.0, 1.0 / double(stages)) - 1.0);
        const double kk = 1.0 / std::max(double(q) * sec, 0.3);
        bool on[kSlots];
        float gain[kSlots];
        for (int s = 0; s < kSlots; ++s) {
            double f = 0.0;
            if (s == 0) { on[0] = have && tonicOn; f = tonicHz; gain[0] = linOf(boost + tonicLevel); }
            else {
                const int semi = int(std::lround(ctl_[I1 + (s - 1)]));
                f = tonicHz * std::pow(2.0, double(semi) / 12.0);
                on[s] = have && s <= nf;
                gain[s] = linOf(boost + lev[s - 1]);
            }
            if (f < 20.0 || f > nyq) on[s] = false;
            if (on[s]) {
                res_[size_t(s)].tune(std::tan(std::numbers::pi * f / double(sr_)), kk);
                if (!res_[size_t(s)].wasOn) res_[size_t(s)].clear();
            }
            res_[size_t(s)].wasOn = on[s];
        }
        const float thr = linOf(-6.0f - 0.1f * boost);
        const float ratio = 2.0f + 6.0f * std::clamp(boost / 36.0f, 0.0f, 1.0f);
        const float dryGain = linOf(-0.17f * boost);
        const float ceilW = std::clamp(boost / 3.0f, 0.0f, 1.0f);

        for (int i = 0; i < n; ++i) {
            const float inL = l[i], inR = r[i];
            const float mix = sm_[Mix].next();
            float wet = 0.0f;
            if (mix > 0.0f && have) {
                const float x = 0.5f * (inL + inR);
                // the exciter: a soft 2nd and 3rd order shaper, scaled by `excite`, DC removed
                const float u = std::clamp(x * (1.0f + 2.0f * excite), -1.0f, 1.0f);
                const float h = excite * (0.55f * u * u + 0.4f * u * u * u);
                const float hd = h - dcX_ + dcK_ * dcY_;
                dcX_ = h; dcY_ = hd;
                const double rin = double(x + hd);
                for (int s = 0; s < kSlots; ++s) {
                    if (!on[s]) continue;
                    auto& rs = res_[size_t(s)];
                    float y = float(rs.run(rin, stages)) * gain[s];
                    // compress this resonance: instant peak, soft knee, smoothed gain
                    const float a = std::abs(y);
                    rs.env = a > rs.env ? a : rs.env * peakRel_;
                    const float red = reductionOf(dbOf(rs.env / thr), ratio, 6.0f);
                    const float gt = linOf(-red);
                    rs.gain = gt < rs.gain ? gt + (rs.gain - gt) * compAtt_ : gt + (rs.gain - gt) * compRel_;
                    wet += y * rs.gain;
                }
            }
            float procL = inL * dryGain + wet, procR = inR * dryGain + wet;
            if (ceilW > 0.0f) { procL += ceilW * (ceilingOf(procL) - procL); procR += ceilW * (ceilingOf(procR) - procR); }
            float outL = inL + mix * (procL - inL), outR = inR + mix * (procR - inR);
            // the final saturation and compression
            const float sat = sm_[Sat].next();
            if (sat > 1e-6f) saturate(outL, outR, sat);
            l[i] = outL; r[i] = outR;
        }
    }

    void saturate(float& l, float& r, float sat) {
        // program-dependent compression (threshold -16 dB, up to 3.5:1), partial makeup
        const float lvl = std::max(std::abs(l), std::abs(r));
        satEnv_ = lvl > satEnv_ ? lvl + (satEnv_ - lvl) * satAtt_ : lvl + (satEnv_ - lvl) * satRel_;
        const float ratio = 1.0f + 2.5f * sat;
        const float red = reductionOf(dbOf(satEnv_) + 16.0f, ratio, 8.0f);
        const float g = linOf(-0.65f * red);                                    // under the threshold: unity; above it the level is only partly taken back
        // asymmetric tanh with unity small-signal gain
        const float d = 1.0f + 3.5f * sat, b = 0.15f * sat;
        const float tb = std::tanh(b), norm = 1.0f / (d * (1.0f - tb * tb));
        const float w = std::min(1.0f, sat * 3.0f);
        const auto stage = [&](float x) { const float c = x * g; return (std::tanh(d * c + b) - tb) * norm; };
        l += w * (stage(l) - l);
        r += w * (stage(r) - r);
    }

    float sr_ = 44100.0f;
    std::array<Smoother, kNumP> sm_;
    float ctl_[kNumP] = {};
    std::array<Resonator, kSlots> res_;
    // tonic
    std::array<Held, kHeld> held_{};
    int nHeld_ = 0, noteAge_ = 1 << 30;
    float noteTonic_ = -1.0f, audioTonic_ = -1.0f, midiCur_ = -1.0f, lastCand_ = -1.0f;
    int agree_ = 0;
    // pitch tracker
    dsp::Fft fft_;
    std::vector<double> hann_;
    std::array<float, kRing> ring_{};
    std::array<double, kFft> re_{}, im_{}, mag_{};
    int ringPos_ = 0, sinceDetect_ = 0;
    // dynamics
    float compAtt_ = 0, compRel_ = 0, peakRel_ = 0, satAtt_ = 0, satRel_ = 0, dcK_ = 0.999f;
    float dcX_ = 0, dcY_ = 0, satEnv_ = 0;
};

}  // namespace

std::unique_ptr<EffectDevice> make_chordres() { return std::make_unique<ChordRes>(); }

}  // namespace ddaw::devices
