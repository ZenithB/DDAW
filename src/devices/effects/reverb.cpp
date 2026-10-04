// `reverb` effect: port of sf-dsp/src/fx/reverb.rs. 8-line FDN with an orthogonal Hadamard feedback
// matrix and a fixed 20 ms pre-delay; per-line gains lose 60 dB in `size` seconds. No in-loop damping
// (the browser IR is undamped noise). Level staging follows the browser's offline reverb: a LINEAR
// dry/wet mix and a wet tap gain derived from the Web Audio ConvolverNode normalisation
// (0.00125 / rms(IR)). All buffers are sized in prepare().
// Schema: size 0.2..10 s (def 2.2), mix 0..1 (def 0.3).
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/DelayLine.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Size, Mix };

constexpr int kLines = 8;
constexpr float kLineMs[kLines] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.5f, 61.7f, 68.3f};
constexpr float kPreDelaySec = 0.02f;
constexpr float kGainCalibration = 0.00125f;
constexpr float kGainCalibrationSr = 44100.0f;
constexpr float kSixLn10 = 13.815511f;  // 6*ln(10)
constexpr float kSizeMin = 0.2f, kSizeMax = 10.0f;

class ReverbFx final : public EffectDevice {
public:
    ReverbFx() { prepare(44100.0, 128); }

    std::span<const ParamSpec> params() const override { return schema::kFxReverb; }

    void prepare(double sr, int) override {
        sr_ = std::max(static_cast<float>(sr), 1.0f);
        for (int k = 0; k < kLines; ++k) {
            delaySamps_[size_t(k)] = std::max(static_cast<int>(kLineMs[k] * 1e-3f * sr_), 1);
            lines_[size_t(k)].prepare(delaySamps_[size_t(k)] + 1);
        }
        preSamps_ = std::max(static_cast<int>(kPreDelaySec * sr_), 1);
        preL_.prepare(preSamps_ + 1);
        preR_.prepare(preSamps_ + 1);
        for (auto& g : fbGain_) g.prepare(sr_, 15.0f);
        tapGain_.prepare(sr_, 15.0f);
        mix_.prepare(sr_, 15.0f);
        updateFbTargets();
        reset();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Size: size_ = std::clamp(v, kSizeMin, kSizeMax); updateFbTargets(); break;
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int i = 0; i < n; ++i) {
            const float dryL = l[i], dryR = r[i];

            preL_.write(dryL);
            preR_.write(dryR);
            const float inL = preL_.read(preSamps_);
            const float inR = preR_.read(preSamps_);

            std::array<float, kLines> v;
            for (size_t k = 0; k < kLines; ++k) v[k] = lines_[k].read(delaySamps_[k]);

            // decorrelated stereo taps: orthogonal sign patterns over the lines
            const float tap = tapGain_.next();
            const float wetL = tap * (v[0] - v[1] + v[2] - v[3] + v[4] - v[5] + v[6] - v[7]);
            const float wetR = tap * (v[0] + v[1] - v[2] - v[3] + v[4] + v[5] - v[6] - v[7]);

            hadamard8(v);
            for (size_t k = 0; k < kLines; ++k) {
                const float g = fbGain_[k].next();
                const float inj = (k & 1) == 0 ? inL : inR;
                lines_[k].write(v[k] * g + inj);
            }

            // LINEAR dry/wet (the golden path is Gain(mix) wet + Gain(1-mix) dry)
            const float m = mix_.next();
            l[i] = dryL * (1.0f - m) + wetL * m;
            r[i] = dryR * (1.0f - m) + wetR * m;
        }
    }

    void reset() override {
        for (auto& line : lines_) line.clear();
        preL_.clear();
        preR_.clear();
        for (auto& g : fbGain_) g.snap(g.target());
        tapGain_.snap(tapGain_.target());
        mix_.snap(mix_.target());
    }

private:
    // Per-line loop gain for a 60 dB decay over `size` seconds, plus the wet tap gain that matches
    // the browser's convolver-normalised wet level (tap^2 = 6 ln10 * cal^2 / sum(1/d_k)).
    void updateFbTargets() {
        const float t60 = std::clamp(size_, kSizeMin, kSizeMax);
        float invDSum = 0.0f;
        for (size_t k = 0; k < kLines; ++k) {
            const float dSec = static_cast<float>(delaySamps_[k]) / sr_;
            fbGain_[k].setTarget(std::pow(10.0f, -3.0f * dSec / t60));
            invDSum += 1.0f / static_cast<float>(delaySamps_[k]);
        }
        const float cal = kGainCalibration * kGainCalibrationSr / sr_;
        const float tapSq = kSixLn10 * cal * cal / invDSum;
        tapGain_.setTarget(std::sqrt(std::max(tapSq, 0.0f)));
    }

    // In-place 8-point Hadamard butterfly, normalised to be orthogonal.
    static void hadamard8(std::array<float, kLines>& v) {
        for (size_t stride : {size_t(1), size_t(2), size_t(4)}) {
            for (size_t i = 0; i < kLines; i += stride * 2) {
                for (size_t j = i; j < i + stride; ++j) {
                    const float a = v[j], b = v[j + stride];
                    v[j] = a + b;
                    v[j + stride] = a - b;
                }
            }
        }
        constexpr float kNorm = 0.35355339f;  // 1/sqrt(8)
        for (auto& x : v) x *= kNorm;
    }

    float sr_ = 44100.0f;
    std::array<DelayLine, kLines> lines_;
    std::array<int, kLines> delaySamps_{};
    std::array<Smoother, kLines> fbGain_;
    Smoother tapGain_, mix_;
    DelayLine preL_, preR_;
    int preSamps_ = 1;
    float size_ = 2.2f;
};

}  // namespace

std::unique_ptr<EffectDevice> make_reverb() { return std::make_unique<ReverbFx>(); }

}  // namespace ddaw::devices
