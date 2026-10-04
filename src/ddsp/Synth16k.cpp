#include "ddsp/Synth16k.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ddaw::ddsp {

// ---------------------------------------------------------------- NoiseFilter

void NoiseFilter::prepare() {
    cosTab_.resize(size_t(kBins) * kTaps);
    for (int k = 0; k < kBins; ++k)
        for (int n = 0; n < kTaps; ++n) cosTab_[size_t(k) * kTaps + size_t(n)] = float(std::cos(2.0 * std::numbers::pi * k * n / kTaps));
    // tf.signal.hann_window(128) (periodic), shifted to zero-phase form
    window_.resize(kTaps);
    for (int n = 0; n < kTaps; ++n) {
        const double h = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * n / kTaps);
        window_[size_t((n + kTaps / 2) % kTaps)] = float(h);   // fftshift
    }
    ir_.assign(kTaps, 0.0f);
    ola_.assign(256, 0.0f);
    reset();
}

void NoiseFilter::reset() {
    std::fill(ola_.begin(), ola_.end(), 0.0f);
    frame_ = 0;
    readBlock_ = 0;
}

void NoiseFilter::push(const float* raw, const float* noise) noexcept {
    // magnitudes -> zero-phase impulse response (irfft of a real spectrum), windowed, then in causal form
    float mag[kBins];
    for (int k = 0; k < kBins; ++k) mag[k] = expSigmoid(raw[k] - 5.0f);   // FilteredNoise.initial_bias = -5
    for (int n = 0; n < kTaps; ++n) {
        double acc = double(mag[0]) + double(mag[kBins - 1]) * ((n & 1) ? -1.0 : 1.0);
        for (int k = 1; k < kBins - 1; ++k) acc += 2.0 * double(mag[k]) * double(cosTab_[size_t(k) * kTaps + size_t(n)]);
        ir_[size_t(n)] = float(acc / kTaps) * window_[size_t(n)];
    }
    // convolve the block with the (fftshifted, causal) IR: tap j = ir[(j + 64) mod 128]
    const int base = frame_ * kHop;
    for (int i = 0; i < kHop; ++i) {
        const float x = noise[i];
        if (x == 0.0f) continue;
        for (int j = 0; j < kTaps; ++j) ola_[size_t((base + i + j) & 255)] += x * ir_[size_t((j + kTaps / 2) % kTaps)];
    }
    ++frame_;
}

void NoiseFilter::pop(float* out) noexcept {
    // output block g = ola[64g + 62 .. 64g + 125]  (the delay compensation crops the first 62 samples)
    const int start = readBlock_ * kHop + (kTaps - 1) / 2 - 1;
    for (int i = 0; i < kHop; ++i) {
        const size_t idx = size_t((start + i) & 255);
        out[i] = ola_[idx];
        ola_[idx] = 0.0f;
    }
    ++readBlock_;
}

// ---------------------------------------------------------------- ReverbConv

void ReverbConv::prepare(const float* ir, size_t length) {
    fft_.prepare(kN);
    parts_ = length ? (length + kB - 1) / kB : 0;
    H_.assign(parts_ * kBinsN * 2, 0.0);
    X_.assign(parts_ * kBinsN * 2, 0.0);
    prev_.assign(kB, 0.0);
    re_.assign(kN, 0.0);
    im_.assign(kN, 0.0);
    accRe_.assign(kBinsN, 0.0);
    accIm_.assign(kBinsN, 0.0);
    for (size_t p = 0; p < parts_; ++p) {
        std::fill(re_.begin(), re_.end(), 0.0);
        std::fill(im_.begin(), im_.end(), 0.0);
        for (int i = 0; i < kB; ++i) {
            const size_t k = p * kB + size_t(i);
            if (k < length) re_[size_t(i)] = (k == 0) ? 0.0 : double(ir[k]);   // the dry tap is masked
        }
        fft_.forward(re_.data(), im_.data());
        for (int b = 0; b < kBinsN; ++b) { H_[(p * kBinsN + size_t(b)) * 2] = re_[size_t(b)]; H_[(p * kBinsN + size_t(b)) * 2 + 1] = im_[size_t(b)]; }
    }
    head_ = 0;
}

void ReverbConv::reset() {
    std::fill(X_.begin(), X_.end(), 0.0);
    std::fill(prev_.begin(), prev_.end(), 0.0);
    head_ = 0;
}

void ReverbConv::process(const float* in, float* out) noexcept {
    if (!parts_) { if (out != in) std::copy(in, in + kB, out); return; }
    // frame = [previous block, this block]
    for (int i = 0; i < kB; ++i) { re_[size_t(i)] = prev_[size_t(i)]; re_[size_t(kB + i)] = double(in[i]); prev_[size_t(i)] = double(in[i]); }
    std::fill(im_.begin(), im_.end(), 0.0);
    fft_.forward(re_.data(), im_.data());
    head_ = (head_ + parts_ - 1) % parts_;   // newest spectrum goes at the head of the delay line
    for (int b = 0; b < kBinsN; ++b) { X_[(head_ * kBinsN + size_t(b)) * 2] = re_[size_t(b)]; X_[(head_ * kBinsN + size_t(b)) * 2 + 1] = im_[size_t(b)]; }
    std::fill(accRe_.begin(), accRe_.end(), 0.0);
    std::fill(accIm_.begin(), accIm_.end(), 0.0);
    for (size_t p = 0; p < parts_; ++p) {
        const size_t xi = (head_ + p) % parts_;   // spectrum of the block p blocks ago
        const double* h = &H_[p * kBinsN * 2];
        const double* x = &X_[xi * kBinsN * 2];
        for (int b = 0; b < kBinsN; ++b) {
            accRe_[size_t(b)] += h[2 * b] * x[2 * b] - h[2 * b + 1] * x[2 * b + 1];
            accIm_[size_t(b)] += h[2 * b] * x[2 * b + 1] + h[2 * b + 1] * x[2 * b];
        }
    }
    // inverse transform of the Hermitian spectrum: ifft(Y) = conj(fft(conj(Y))) / N
    for (int b = 0; b < kBinsN; ++b) { re_[size_t(b)] = accRe_[size_t(b)]; im_[size_t(b)] = -accIm_[size_t(b)]; }
    for (int b = 1; b < kN / 2; ++b) { re_[size_t(kN - b)] = accRe_[size_t(b)]; im_[size_t(kN - b)] = accIm_[size_t(b)]; }
    fft_.forward(re_.data(), im_.data());
    for (int i = 0; i < kB; ++i) out[i] = in[i] + float(re_[size_t(kB + i)] / kN);   // overlap-save: the last block is valid
}

// ---------------------------------------------------------------- Synth16k

void Synth16k::prepare(const float* ir, size_t irLen) {
    harm_.prepare(kModelSampleRate, kHopSamples);
    noise_.prepare();
    reverb_ = nullptr;
    if (ir && irLen) { ownReverb_.prepare(ir, irLen); reverb_ = &ownReverb_; }
    h_.assign(kHopSamples, 0.0f);
    n_.assign(kHopSamples, 0.0f);
    reset();
}

void Synth16k::reset() {
    harm_.reset();
    noise_.reset();
    if (reverb_) reverb_->reset();
    havePrev_ = false;
}

bool Synth16k::push(const float* raw, float f0Hz, const float* noise64, float* out) noexcept {
    makeHarmonicFrame(raw, f0Hz, kModelSampleRate, cur_);
    noise_.push(raw + kNumAmps + kNumHarmonics, noise64);
    bool produced = false;
    if (havePrev_) {
        harm_.renderInterval(prev_, cur_, h_.data());
        noise_.pop(n_.data());
        for (int i = 0; i < kHopSamples; ++i) out[i] = h_[size_t(i)] + noiseGain_ * n_[size_t(i)];
        if (reverbOn_ && reverb_ && reverb_->ready()) reverb_->process(out, out);
        produced = true;
    }
    prev_ = cur_;
    havePrev_ = true;
    return produced;
}

}  // namespace ddaw::ddsp
