#include "dsp/PolyPitch.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ddaw::dsp {

namespace {
constexpr double kRate = 16000.0;

struct Peak { double hz, amp; };
double midiOf(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
}  // namespace

void PolyPitch::prepare(const PolyPitchConfig& cfg) {
    cfg_ = cfg;
    fftSize_ = 4 * cfg.window;   // zero padding: 4x finer bins for interpolation
    fft_.prepare(fftSize_);
    win_.resize(size_t(cfg.window));
    for (int i = 0; i < cfg.window; ++i) win_[size_t(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / cfg.window);
    re_.assign(size_t(fftSize_), 0.0);
    im_.assign(size_t(fftSize_), 0.0);
}

std::vector<PolyCandidate> PolyPitch::estimate(const float* x) {
    std::fill(re_.begin(), re_.end(), 0.0);
    std::fill(im_.begin(), im_.end(), 0.0);
    for (int i = 0; i < cfg_.window; ++i) re_[size_t(i)] = double(x[i]) * win_[size_t(i)];
    fft_.forward(re_.data(), im_.data());
    const int bins = fftSize_ / 2;
    std::vector<double> mag(static_cast<size_t>(bins));
    double top = 0;
    for (int k = 0; k < bins; ++k) { mag[size_t(k)] = std::hypot(re_[size_t(k)], im_[size_t(k)]); top = std::max(top, mag[size_t(k)]); }
    std::vector<PolyCandidate> out;
    if (top < 1e-6) return out;
    {   // noise gate: a tonal frame has peaks tens of dB above the typical bin; white noise tops out a few dB above it
        std::vector<double> sorted(mag);
        std::nth_element(sorted.begin(), sorted.begin() + bins / 2, sorted.end());
        if (top < 10.0 * sorted[size_t(bins / 2)]) return out;
    }

    // ---- peaks, with parabolic interpolation on the log magnitude ----
    const double hzPerBin = kRate / fftSize_, floorAmp = top * std::pow(10.0, cfg_.floorDb / 20.0);
    double totalPower = 0;
    for (int k = 0; k < bins; ++k) totalPower += mag[size_t(k)] * mag[size_t(k)];
    std::vector<Peak> peaks;
    for (int k = 2; k < bins - 2; ++k) {
        const double m = mag[size_t(k)];
        if (m < floorAmp || m <= mag[size_t(k - 1)] || m < mag[size_t(k + 1)]) continue;
        const double a = std::log(mag[size_t(k - 1)] + 1e-12), b = std::log(m + 1e-12), c = std::log(mag[size_t(k + 1)] + 1e-12);
        const double den = a - 2.0 * b + c;
        const double d = std::abs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
        peaks.push_back({(double(k) + std::clamp(d, -0.5, 0.5)) * hzPerBin, std::exp(b - 0.25 * (a - c) * d)});
    }
    if (peaks.empty()) return out;
    const double hannMainlobe = 2.0 * kRate / cfg_.window;     // Hz between a peak and its first null
    (void)hannMainlobe;

    // ---- candidate fundamentals: each peak as the 1st..4th partial ----
    std::vector<double> candHz;
    for (const auto& p : peaks)
        for (int h = 1; h <= 4; ++h) {
            const double f = p.hz / h;
            if (f >= cfg_.minHz && f <= cfg_.maxHz) candHz.push_back(f);
        }
    std::sort(candHz.begin(), candHz.end());
    // merge hypotheses closer than 25 cents
    std::vector<double> cands;
    for (double f : candHz) if (cands.empty() || std::abs(1200.0 * std::log2(f / cands.back())) > 25.0) cands.push_back(f);

    std::vector<double> amp(peaks.size(), 0.0);
    for (size_t i = 0; i < peaks.size(); ++i) amp[i] = peaks[i].amp / top;

    const bool cleanTone = peaks.size() <= 3;   // a bare sine (a flute, a test tone) has no harmonics to count
    auto score = [&](double f0, std::vector<std::pair<size_t, double>>* used) {
        double s = 0, tot = 0;
        int missingLow = 0, matchedLow = 0;
        bool fundamental = false;
        for (int h = 1; h <= cfg_.harmonics; ++h) {
            const double target = f0 * h;
            if (target > kRate * 0.5 - 100.0) break;
            const double tol = std::max(0.015 * target, 1.5 * kRate / cfg_.window);   // 1.5% or a bin and a half
            int best = -1;
            double bestA = 0;
            for (size_t i = 0; i < peaks.size(); ++i)
                if (std::abs(peaks[i].hz - target) <= tol && amp[i] > bestA) { bestA = amp[i]; best = int(i); }
            const double w = 1.0 / std::sqrt(double(h));
            tot += w;
            if (best >= 0 && bestA >= 0.02) {
                s += w * std::sqrt(bestA);
                if (used) used->push_back({size_t(best), bestA});
                if (h <= 6) ++matchedLow;
                if (h == 1) fundamental = true;
            } else if (h <= 4) ++missingLow;
        }
        // the fundamental must be audible (a subharmonic is not a note), and a real note shows several of its
        // low partials, unless the whole signal is one clean tone
        if (!fundamental || (!cleanTone && matchedLow < 3)) return 0.0;
        return tot > 0 ? s / (tot * (1.0 + 0.5 * missingLow)) : 0.0;
    };

    // ---- iterate: best hypothesis, then weaken what it explains ----
    double firstScore = 0;
    for (int voice = 0; voice < cfg_.maxVoices; ++voice) {
        double bestS = 0, bestF = 0;
        for (double f : cands) {
            const double s = score(f, nullptr);
            if (s > bestS) { bestS = s; bestF = f; }
        }
        if (bestS <= 0.0) break;
        if (voice == 0) firstScore = bestS;
        if (bestS < cfg_.minSalience * firstScore || bestS < 0.12) break;
        std::vector<std::pair<size_t, double>> used;
        score(bestF, &used);
        {   // tonality gate: a note's partials carry a real share of the frame's energy (noise spreads it over everything)
            std::vector<uint8_t> seen(size_t(bins), 0);
            double e = 0;
            for (auto& [i, a] : used) {
                const int k0 = int(std::lround(peaks[i].hz / hzPerBin));
                for (int k = std::max(0, k0 - 8); k <= std::min(bins - 1, k0 + 8); ++k) if (!seen[size_t(k)]) { seen[size_t(k)] = 1; e += mag[size_t(k)] * mag[size_t(k)]; }
            }
            if (e < 0.06 * totalPower && voice > 0) {   // later voices may be quiet, but not noise
                for (auto it = cands.begin(); it != cands.end();) it = std::abs(1200.0 * std::log2(*it / bestF)) < 30.0 ? cands.erase(it) : it + 1;
                continue;
            }
            if (e < 0.05 * totalPower) break;            // the first voice: if even it is not tonal, the frame is noise
        }
        double level = 0;
        for (auto& [i, a] : used) { level = std::max(level, a); }
        // refine the pitch from the matched partials (amplitude-weighted ratio to the harmonic number)
        double num = 0, den = 0;
        for (auto& [i, a] : used) {
            const int h = std::max(1, int(std::lround(peaks[i].hz / bestF)));
            num += a * peaks[i].hz / h;
            den += a;
        }
        const double refined = den > 0 ? num / den : bestF;
        PolyCandidate c;
        c.hz = float(refined);
        c.midi = float(midiOf(refined));
        c.salience = float(bestS / firstScore);
        c.level = float(level * top * 4.0 / cfg_.window);   // a sine of amplitude A peaks at A*N/4 under a Hann window: back to amplitude
        // a duplicate of a note already found (within a quarter tone) adds nothing
        bool dup = false;
        for (auto& o : out) dup |= std::abs(o.midi - c.midi) < 0.5f;
        if (!dup) out.push_back(c);
        for (auto& [i, a] : used) amp[i] *= 0.12;           // what this note explains is mostly spoken for; a shared partial (octave, fifth) keeps a little
        for (auto it = cands.begin(); it != cands.end();) it = std::abs(1200.0 * std::log2(*it / bestF)) < 30.0 ? cands.erase(it) : it + 1;
    }
    return out;
}

void PolyNoteTracker::reset() { for (auto& s : state_) s = S{}; }

void PolyNoteTracker::update(const std::vector<PolyCandidate>& found, std::vector<PolyNoteEvent>& events) {
    bool now[128] = {};
    float lv[128] = {};
    for (const auto& c : found) {
        const int p = int(std::lround(c.midi));
        if (p < 0 || p > 127 || std::abs(c.midi - float(p)) > 0.45f) continue;   // within 45 cents of a semitone
        now[p] = true;
        lv[p] = std::max(lv[p], c.level);
    }
    for (int p = 0; p < 128; ++p) {
        S& s = state_[p];
        if (now[p]) {
            s.missing = 0;
            s.level = std::max(s.level * 0.5f, lv[p]);
            if (!s.active && ++s.seen >= confirm_) {
                s.active = true;
                events.push_back({p, true, std::clamp(0.2f + 0.8f * (20.0f * std::log10(std::max(s.level, 1e-6f)) + 40.0f) / 34.0f, 0.15f, 1.0f)});   // -40 dB -> soft, -6 dB -> full
            }
        } else {
            s.seen = 0;
            if (s.active && ++s.missing >= release_) { s.active = false; s.missing = 0; s.level = 0; events.push_back({p, false, 0.0f}); }
        }
    }
}

}  // namespace ddaw::dsp
