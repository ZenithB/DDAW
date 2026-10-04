#include "ddsp/Decoder.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <RTNeural/RTNeural.h>

#include "ddsp/Weights.h"

namespace ddaw::ddsp {

namespace {
// RTNeural's STL backend takes float arrays; its XSIMD (NEON) backend takes arrays of SIMD
// vectors and keeps its outputs as SIMD vectors. This adapter hides the difference: callers
// pass aligned, zero-padded float buffers and read outputs as floats.
#if defined(RTNEURAL_USE_XSIMD)
using V = xsimd::simd_type<float>;
constexpr int kVS = int(V::size);
template <int N> constexpr int vlen = (N + kVS - 1) / kVS;
template <int In, class L> inline void fwd(L& l, const float* in) noexcept {
    l.forward(reinterpret_cast<const V(&)[vlen<In>]>(*in));
}
#else
constexpr int kVS = 1;
template <int In, class L> inline void fwd(L& l, const float* in) noexcept {
    l.forward(reinterpret_cast<const float(&)[In]>(*in));
}
#endif
template <class L> inline const float* outsOf(const L& l) noexcept { return reinterpret_cast<const float*>(l.outs); }
}  // namespace

float scaleLoudnessDb(float db) { return db / 80.0f + 1.0f; }

float scaleF0Hz(float hz) {
    if (hz <= 0.0f) return 0.0f;
    return (12.0f * (std::log2(hz) - std::log2(440.0f)) + 69.0f) / 127.0f;
}

namespace {

constexpr float kLayerNormEps = 1e-3f;  // tf.keras LayerNormalization default
constexpr float kLeakyAlpha   = 0.2f;   // tf.nn.leaky_relu default

// LayerNormalization over the feature axis, then LeakyReLU, in place.
// RTNeural has no LayerNorm or LeakyReLU, so these two are ours.
template <int N>
struct NormAct {
    float gamma[N]{}, beta[N]{};
    void apply(float* x) const noexcept {
        float mean = 0.0f;
        for (int i = 0; i < N; ++i) mean += x[i];
        mean *= 1.0f / float(N);
        float var = 0.0f;
        for (int i = 0; i < N; ++i) { const float d = x[i] - mean; var += d * d; }
        const float inv = 1.0f / std::sqrt(var * (1.0f / float(N)) + kLayerNormEps);
        for (int i = 0; i < N; ++i) {
            const float y = (x[i] - mean) * inv * gamma[i] + beta[i];
            x[i] = y >= 0.0f ? y : kLeakyAlpha * y;
        }
    }
};

template <class Dense>
void setDense(Dense& d, const Tensor& W, const Tensor& b) {
    constexpr int in = Dense::in_size, out = Dense::out_size;
    if (W.dims.size() != 2 || int(W.dims[0]) != out || int(W.dims[1]) != in || int(b.size()) != out)
        throw std::runtime_error("dense weight shape mismatch");
    std::vector<std::vector<float>> w(out, std::vector<float>(in));
    for (int o = 0; o < out; ++o) std::memcpy(w[o].data(), &W.data[size_t(o) * in], sizeof(float) * in);
    d.setWeights(w);
    d.setBias(b.data.data());
}

std::vector<std::vector<float>> rows(const Tensor& t, size_t r, size_t c) {
    if (t.size() != r * c) throw std::runtime_error("tensor shape mismatch");
    std::vector<std::vector<float>> m(r, std::vector<float>(c));
    for (size_t i = 0; i < r; ++i) std::memcpy(m[i].data(), &t.data[i * c], sizeof(float) * c);
    return m;
}

template <int N>
void setNorm(NormAct<N>& n, const Tensor& g, const Tensor& b) {
    if (g.size() != N || b.size() != N) throw std::runtime_error("layernorm shape mismatch");
    std::memcpy(n.gamma, g.data.data(), sizeof n.gamma);
    std::memcpy(n.beta, b.data.data(), sizeof n.beta);
}

}  // namespace

struct Decoder::Impl {
    static constexpr int H = kHidden;

    // One FC stack: Dense -> LayerNorm -> LeakyReLU, three times.
    template <int In>
    struct Stack {
        RTNeural::DenseT<float, In, H> d0;
        RTNeural::DenseT<float, H, H>  d1, d2;
        NormAct<H> n0, n1, n2;
    };

    Stack<1>   ld, f0;
    Stack<3 * H> out;  // takes [ld, f0, gru]
    RTNeural::GRULayerT<float, 2 * H, H> gru;
    RTNeural::DenseT<float, H, kDecoderOut> denseOut;

    alignas(64) float ldOut[H], f0Out[H], gruIn[2 * H], outIn[3 * H], x[H];
    bool loaded = false;

};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;
bool Decoder::loaded() const { return impl_->loaded; }

void Decoder::load(const std::string& path) {
    const TensorMap t = loadWeights(path);
    auto at = [&](const std::string& k) -> const Tensor& {
        auto it = t.find(k);
        if (it == t.end()) throw std::runtime_error("missing tensor " + k + " in " + path);
        return it->second;
    };
    auto& m = *impl_;
    auto loadStack = [&](auto& s, const std::string& p) {
        setDense(s.d0, at(p + ".0.W"), at(p + ".0.b")); setNorm(s.n0, at(p + ".0.gamma"), at(p + ".0.beta"));
        setDense(s.d1, at(p + ".1.W"), at(p + ".1.b")); setNorm(s.n1, at(p + ".1.gamma"), at(p + ".1.beta"));
        setDense(s.d2, at(p + ".2.W"), at(p + ".2.b")); setNorm(s.n2, at(p + ".2.gamma"), at(p + ".2.beta"));
    };
    loadStack(m.ld, "ld");
    loadStack(m.f0, "f0");
    loadStack(m.out, "out");
    constexpr size_t H = kHidden;
    m.gru.setWVals(rows(at("gru.W"), 2 * H, 3 * H));
    m.gru.setUVals(rows(at("gru.U"), H, 3 * H));
    m.gru.setBVals(rows(at("gru.b"), 2, 3 * H));
    setDense(m.denseOut, at("dense_out.W"), at("dense_out.b"));
    m.loaded = true;
    reset();
}

void Decoder::reset() noexcept {
    impl_->gru.reset();
}

void Decoder::step(float ldScaled, float f0Scaled, float* out) noexcept {
    auto& m = *impl_;
    constexpr int H = kHidden;
    alignas(64) float ldIn[16] = {ldScaled}, f0In[16] = {f0Scaled};  // zero-padded to a SIMD multiple

    fwd<1>(m.ld.d0, ldIn);
    std::memcpy(m.ldOut, outsOf(m.ld.d0), sizeof m.ldOut); m.ld.n0.apply(m.ldOut);
    fwd<H>(m.ld.d1, m.ldOut);
    std::memcpy(m.ldOut, outsOf(m.ld.d1), sizeof m.ldOut); m.ld.n1.apply(m.ldOut);
    fwd<H>(m.ld.d2, m.ldOut);
    std::memcpy(m.ldOut, outsOf(m.ld.d2), sizeof m.ldOut); m.ld.n2.apply(m.ldOut);

    fwd<1>(m.f0.d0, f0In);
    std::memcpy(m.f0Out, outsOf(m.f0.d0), sizeof m.f0Out); m.f0.n0.apply(m.f0Out);
    fwd<H>(m.f0.d1, m.f0Out);
    std::memcpy(m.f0Out, outsOf(m.f0.d1), sizeof m.f0Out); m.f0.n1.apply(m.f0Out);
    fwd<H>(m.f0.d2, m.f0Out);
    std::memcpy(m.f0Out, outsOf(m.f0.d2), sizeof m.f0Out); m.f0.n2.apply(m.f0Out);

    std::memcpy(m.gruIn, m.ldOut, sizeof m.ldOut);
    std::memcpy(m.gruIn + H, m.f0Out, sizeof m.f0Out);
    fwd<2 * H>(m.gru, m.gruIn);

    std::memcpy(m.outIn, m.ldOut, sizeof m.ldOut);
    std::memcpy(m.outIn + H, m.f0Out, sizeof m.f0Out);
    std::memcpy(m.outIn + 2 * H, outsOf(m.gru), sizeof(float) * H);

    fwd<3 * H>(m.out.d0, m.outIn);
    std::memcpy(m.x, outsOf(m.out.d0), sizeof m.x);
    m.out.n0.apply(m.x);
    fwd<H>(m.out.d1, m.x);
    std::memcpy(m.x, outsOf(m.out.d1), sizeof m.x); m.out.n1.apply(m.x);
    fwd<H>(m.out.d2, m.x);
    std::memcpy(m.x, outsOf(m.out.d2), sizeof m.x); m.out.n2.apply(m.x);

    fwd<H>(m.denseOut, m.x);
    std::memcpy(out, outsOf(m.denseOut), sizeof(float) * kDecoderOut);
}

}  // namespace ddaw::ddsp
