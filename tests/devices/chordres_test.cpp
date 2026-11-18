#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
#include <numbers>
#include <random>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "devices/schema/SchemaExt.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
enum P : uint16_t { Source, Filters, Tonic, TonicLevel, I1, I2, I3, I4, I5, I6, L1, L2, L3, L4, L5, L6, Boost, Q, Steep, Excite, Sat, Mix, Glide, Thresh };

std::unique_ptr<EffectDevice> make() { return createEffect("chordres"); }

using Setup = std::vector<std::pair<uint16_t, float>>;
std::unique_ptr<EffectDevice> made(const Setup& s) {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    for (auto [i, v] : s) d->setParam(i, v);
    d->reset();                       // reset snaps the smoothers to the set values
    return d;
}

// Feed `gen(t)` (t = sample index since `t0`) through the device in blocks of `block`; append the left output.
size_t feed(EffectDevice& d, std::vector<float>& out, size_t t0, double seconds, const std::function<float(size_t)>& gen, int block = kMaxBlock) {
    const size_t total = size_t(seconds * kSr) / size_t(block) * size_t(block);
    std::vector<float> l(static_cast<size_t>(block), 0.0f), r(static_cast<size_t>(block), 0.0f);
    for (size_t t = 0; t < total; t += size_t(block)) {
        for (int i = 0; i < block; ++i) l[size_t(i)] = r[size_t(i)] = gen(t0 + t + size_t(i));
        d.process(l.data(), r.data(), block, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return t0 + total;
}

std::function<float(size_t)> noise(float amp, unsigned seed = 7) {
    auto g = std::make_shared<std::mt19937>(seed);
    return [g, amp](size_t) { return amp * std::uniform_real_distribution<float>(-1.0f, 1.0f)(*g); };
}
std::function<float(size_t)> sine(double hz, float amp) {
    return [=](size_t t) { return amp * float(std::sin(2.0 * std::numbers::pi * hz * double(t) / kSr)); };
}

double dftMag(const float* x, size_t n, double hz) {
    double re = 0, im = 0;
    const double w = 2.0 * std::numbers::pi * hz / kSr;
    for (size_t i = 0; i < n; ++i) { re += double(x[i]) * std::cos(w * double(i)); im += double(x[i]) * std::sin(w * double(i)); }
    return std::sqrt(re * re + im * im);
}
// Band-limited magnitude: the rms of single-bin DFT values over f +- w Hz (one bin of noise is a Rayleigh draw).
double bandMag(const float* x, size_t n, double hz, double w = 4.0) {
    double s = 0; int k = 0;
    for (double f = hz - w; f <= hz + w + 1e-9; f += 1.0, ++k) { const double m = dftMag(x, n, f); s += m * m; }
    return std::sqrt(s / k);
}
double peakHz(const std::vector<float>& x, size_t n, double lo, double hi, double step = 1.0) {
    const float* tail = x.data() + x.size() - n;
    double best = lo, bm = -1;
    for (double f = lo; f <= hi; f += step) { const double m = dftMag(tail, n, f); if (m > bm) { bm = m; best = f; } }
    return best;
}
double tailRms(const std::vector<float>& x, size_t n) { return harness::rms(std::span<const float>(x).subspan(x.size() - n)); }
double hzOf(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }
double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }
constexpr size_t kTail = 22050;
}  // namespace

TEST_CASE("chordres: device contract", "[device][chordres]") { checkEffectContract(make); }

TEST_CASE("chordres: the schema has no stepped parameter outside its range and defaults are sane", "[device][chordres]") {
    checkParamTable(make()->params());
    CHECK(make()->wantsNotes());
    CHECK(make()->hasInfo());
}

TEST_CASE("chordres: a filter rings at its interval above the played note (equal temperament)", "[device][chordres]") {
    // A3 (220 Hz) played; one filter a perfect fifth above (7 semitones, 329.63 Hz); white noise in.
    auto d = made({{Source, 1}, {Filters, 1}, {I1, 7}, {Boost, 12}, {Q, 40}, {Excite, 0}});
    d->noteOn(57, 1.0f, 1);
    std::vector<float> out;
    feed(*d, out, 0, 2.0, noise(0.1f));
    const double p = peakHz(out, kTail, 290.0, 370.0);
    CHECK(std::abs(p - hzOf(57 + 7)) < 6.0);
    // and not at the tonic, which is off by default
    const float* tail = out.data() + out.size() - kTail;
    CHECK(dftMag(tail, kTail, hzOf(64)) > 4.0 * dftMag(tail, kTail, hzOf(57)));
}

TEST_CASE("chordres: six filters ring at six intervals, and `filters` limits how many", "[device][chordres]") {
    const int semis[6] = {4, 7, 11, 12, 16, 19};
    for (int nf : {1, 3, 6}) {
        INFO("filters " << nf);
        auto d = made({{Source, 1}, {Filters, float(nf)}, {Boost, 12}, {Q, 60}, {Excite, 0}});
        d->noteOn(48, 1.0f, 1);                                          // C3 = 130.81 Hz
        std::vector<float> out;
        feed(*d, out, 0, 2.5, noise(0.1f));
        const float* tail = out.data() + out.size() - kTail;
        // a quiet reference between resonances
        const double between = bandMag(tail, kTail, hzOf(48 + 5.5));
        for (int k = 0; k < 6; ++k) {
            const double m = bandMag(tail, kTail, hzOf(48 + semis[k]));
            if (k < nf) CHECK(m > 1.7 * between);
        }
        // a filter that is not active leaves its frequency at the noise floor of the dry path
        if (nf < 6) CHECK(bandMag(tail, kTail, hzOf(48 + semis[5])) < 1.4 * between);
    }
}

TEST_CASE("chordres: the tonic is an optional resonance", "[device][chordres]") {
    for (float on : {0.0f, 1.0f}) {
        auto d = made({{Source, 1}, {Filters, 1}, {I1, 12}, {Tonic, on}, {TonicLevel, 0}, {Boost, 12}, {Q, 60}, {Excite, 0}});
        d->noteOn(57, 1.0f, 1);
        std::vector<float> out;
        feed(*d, out, 0, 2.0, noise(0.1f));
        const float* tail = out.data() + out.size() - kTail;
        const double atTonic = dftMag(tail, kTail, 220.0), around = dftMag(tail, kTail, 245.0);
        if (on > 0.5f) CHECK(atTonic > 3.0 * around); else CHECK(atTonic < 2.0 * around);
    }
}

TEST_CASE("chordres: a new note retunes the filters, gliding", "[device][chordres]") {
    auto d = made({{Source, 1}, {Filters, 1}, {I1, 7}, {Boost, 12}, {Q, 40}, {Excite, 0}, {Glide, 25}});
    d->noteOn(57, 1.0f, 1);
    std::vector<float> out;
    auto g = noise(0.1f);
    size_t t = feed(*d, out, 0, 1.5, g);
    CHECK(std::abs(peakHz(out, kTail, 290.0, 370.0) - hzOf(64)) < 6.0);
    CHECK(std::abs(d->info() - 57.0f) < 0.01f);
    d->noteOff(1);
    d->noteOn(60, 1.0f, 2);
    // mid-glide the tonic is between the two
    std::vector<float> mid;
    feed(*d, mid, t, 0.01, g);
    CHECK(d->info() > 57.0f);
    CHECK(d->info() < 60.0f);
    out.clear();
    feed(*d, out, t + size_t(0.01 * kSr), 1.5, g);
    CHECK(std::abs(d->info() - 60.0f) < 0.05f);
    CHECK(std::abs(peakHz(out, kTail, 370.0, 420.0) - hzOf(67)) < 7.0);
}

TEST_CASE("chordres: releasing a note keeps the resonance on the last tonic; releasing one of two returns to the one held", "[device][chordres]") {
    auto d = made({{Source, 1}, {Filters, 1}, {I1, 7}, {Boost, 12}});
    d->noteOn(57, 1.0f, 1);
    d->noteOn(60, 1.0f, 2);
    d->noteOff(2);
    std::vector<float> out;
    feed(*d, out, 0, 0.5, noise(0.1f));
    CHECK(std::abs(d->info() - 57.0f) < 0.05f);
    d->noteOff(1);
    feed(*d, out, 0, 0.2, noise(0.1f));
    CHECK(std::abs(d->info() - 57.0f) < 0.05f);
}

TEST_CASE("chordres: no tonic, no resonance", "[device][chordres]") {
    auto d = made({{Source, 1}, {Boost, 12}, {Excite, 0}, {Mix, 1}});
    std::vector<float> out;
    feed(*d, out, 0, 0.3, sine(300.0, 0.2f));
    CHECK(d->info() < 0.0f);
    // the original, lowered by the boost headroom
    CHECK(db(tailRms(out, 4096)) - db(0.2 / std::sqrt(2.0)) > -3.0);
}

TEST_CASE("chordres: audio source reads the pitch with the FFT", "[device][chordres]") {
    auto harmonic = [](double hz) {
        return [=](size_t t) {
            const double w = 2.0 * std::numbers::pi * hz * double(t) / kSr;
            return float(0.15 * std::sin(w) + 0.3 * std::sin(2 * w) + 0.1 * std::sin(3 * w));   // a strong 2nd: the octave must not win
        };
    };
    for (double midi : {45.0, 57.0, 64.0, 69.0}) {
        INFO("midi " << midi);
        auto d = made({{Source, 2}, {Filters, 1}, {I1, 7}});
        std::vector<float> out;
        feed(*d, out, 0, 1.0, harmonic(hzOf(midi)));
        CHECK(std::abs(d->info() - float(midi)) < 0.25f);
    }
}

TEST_CASE("chordres: audio source holds the tonic through silence and a low level", "[device][chordres]") {
    auto d = made({{Source, 2}, {Filters, 1}, {I1, 7}, {Thresh, -55}});
    std::vector<float> out;
    size_t t = feed(*d, out, 0, 1.0, sine(hzOf(57), 0.3f));
    CHECK(std::abs(d->info() - 57.0f) < 0.25f);
    t = feed(*d, out, t, 1.0, [](size_t) { return 0.0f; });
    CHECK(std::abs(d->info() - 57.0f) < 0.25f);
    feed(*d, out, t, 1.0, noise(1e-4f));                               // -80 dB hiss: below the threshold
    CHECK(std::abs(d->info() - 57.0f) < 0.25f);
}

TEST_CASE("chordres: Auto follows the notes while they play and the audio otherwise", "[device][chordres]") {
    auto d = made({{Source, 0}, {Filters, 1}, {I1, 7}});
    std::vector<float> out;
    size_t t = feed(*d, out, 0, 1.0, sine(hzOf(64), 0.3f));            // no notes: the audio says E4
    CHECK(std::abs(d->info() - 64.0f) < 0.25f);
    d->noteOn(50, 1.0f, 1);                                            // a note: it wins, whatever the audio says
    feed(*d, out, t, 0.6, sine(hzOf(64), 0.3f));
    CHECK(std::abs(d->info() - 50.0f) < 0.1f);
}

TEST_CASE("chordres: more boost lowers the original and compresses each resonance harder", "[device][chordres]") {
    // the original, away from every resonance: lowered by up to ~6 dB
    auto lone = [&](float boost) {
        auto d = made({{Source, 1}, {Filters, 1}, {I1, 7}, {Boost, boost}, {Q, 100}, {Excite, 0}});
        d->noteOn(57, 1.0f, 1);
        std::vector<float> out;
        feed(*d, out, 0, 1.0, sine(2000.0, 0.1f));
        return db(tailRms(out, 8192));
    };
    CHECK(lone(0) - lone(36) > 4.5);
    CHECK(lone(0) - lone(36) < 7.5);
    // a resonance driven on its centre grows by less than the boost
    auto driven = [&](float boost) {
        auto d = made({{Source, 1}, {Filters, 1}, {I1, 7}, {Boost, boost}, {Q, 40}, {Excite, 0}});
        d->noteOn(57, 1.0f, 1);
        std::vector<float> out;
        feed(*d, out, 0, 1.5, sine(hzOf(64), 0.05f));
        return db(tailRms(out, 8192));
    };
    const double growth = driven(36) - driven(12);
    CHECK(growth > 2.0);
    CHECK(growth < 18.0);                                              // 24 dB of boost, compressed
}

TEST_CASE("chordres: the output never clips at full boost, six filters, hot input", "[device][chordres]") {
    for (float boost : {12.0f, 36.0f}) {
        auto d = made({{Source, 1}, {Filters, 6}, {Tonic, 1}, {Boost, boost}, {Q, 200}, {Excite, 1}, {Sat, 0}});
        d->noteOn(48, 1.0f, 1);
        std::vector<float> out;
        feed(*d, out, 0, 2.0, noise(0.8f));
        float peak = 0;
        for (float v : out) { REQUIRE(std::isfinite(v)); peak = std::max(peak, std::abs(v)); }
        INFO("boost " << boost << " peak " << peak);
        // the wet sum is held under 1; the lowered original adds at most 0.8 * 10^(-6/20)
        CHECK(peak < 1.0f + 0.01f);
    }
}

TEST_CASE("chordres: each resonance has unity gain at its centre and a -3 dB bandwidth of f / Q at every steepness", "[device][chordres]") {
    // boost 0 and no exciter: the output is the original plus the resonance, so the resonance alone is out - in
    auto wetGain = [&](float steep, float q, double hz) {
        auto d = made({{Source, 1}, {Filters, 1}, {I1, 12}, {Boost, 0}, {Q, q}, {Steep, steep}, {Excite, 0}, {Mix, 1}});
        d->noteOn(57, 1.0f, 1);
        std::vector<float> out, in;
        auto g = sine(hz, 0.05f);
        feed(*d, out, 0, 1.5, g);
        for (size_t i = 0; i < out.size(); ++i) in.push_back(g(i));
        double s = 0;
        for (size_t i = out.size() - 8192; i < out.size(); ++i) { const double w = double(out[i]) - double(in[i]); s += w * w; }
        double si = 0;
        for (size_t i = in.size() - 8192; i < in.size(); ++i) si += double(in[i]) * double(in[i]);
        return std::sqrt(s / si);
    };
    const double f0 = 440.0;
    for (float steep : {0.0f, 1.0f, 2.0f, 3.0f}) {
        INFO("steep " << steep);
        CHECK(std::abs(wetGain(steep, 20, f0) - 1.0) < 0.05);
        const double hi = f0 * (std::sqrt(1.0 + 1.0 / (4.0 * 20 * 20)) + 1.0 / (2.0 * 20));
        CHECK(std::abs(wetGain(steep, 20, hi) - std::sqrt(0.5)) < 0.06);
    }
}

TEST_CASE("chordres: steepness narrows the skirts, Q narrows the peak", "[device][chordres]") {
    auto wetGain = [&](float steep, float q, double hz) {
        auto d = made({{Source, 1}, {Filters, 1}, {I1, 12}, {Boost, 0}, {Q, q}, {Steep, steep}, {Excite, 0}, {Mix, 1}});
        d->noteOn(57, 1.0f, 1);
        std::vector<float> out, in;
        auto g = sine(hz, 0.05f);
        feed(*d, out, 0, 1.5, g);
        for (size_t i = 0; i < out.size(); ++i) in.push_back(g(i));
        double s = 0, si = 0;
        for (size_t i = out.size() - 8192; i < out.size(); ++i) { const double w = double(out[i]) - double(in[i]); s += w * w; si += double(in[i]) * double(in[i]); }
        return std::sqrt(s / si);
    };
    // a quarter-octave off the centre: the steeper the skirt, the less gets through
    CHECK(wetGain(3, 10, 523.0) < 0.5 * wetGain(0, 10, 523.0));
    CHECK(wetGain(1, 10, 523.0) < wetGain(0, 10, 523.0));
    CHECK(wetGain(2, 10, 523.0) < wetGain(1, 10, 523.0));
    // a higher Q is narrower
    CHECK(wetGain(1, 80, 460.0) < 0.5 * wetGain(1, 10, 460.0));
}

TEST_CASE("chordres: the exciter gives the filters harmonics to ring on", "[device][chordres]") {
    // a pure 220 Hz sine has nothing at 440 Hz; with the exciter its second harmonic is there for the filter on the octave
    auto at440 = [&](float excite) {
        auto d = made({{Source, 1}, {Filters, 1}, {I1, 12}, {Boost, 12}, {Q, 40}, {Excite, excite}});
        d->noteOn(57, 1.0f, 1);
        std::vector<float> out;
        feed(*d, out, 0, 1.5, sine(220.0, 0.3f));
        return dftMag(out.data() + out.size() - kTail, kTail, 440.0);
    };
    CHECK(at440(0.6f) > 20.0 * at440(0.0f));
}

TEST_CASE("chordres: mix 0 and sat 0 are bit-exact passes", "[device][chordres]") {
    auto input = noise(0.4f, 3);
    std::vector<float> in;
    for (size_t i = 0; i < 20000; ++i) in.push_back(input(i));
    {   // mix 0: the effect is the original
        auto d = made({{Source, 1}, {Filters, 6}, {Boost, 24}, {Sat, 0.7f}, {Mix, 0}});
        d->noteOn(50, 1.0f, 1);
        std::vector<float> out;
        size_t k = 0;
        feed(*d, out, 0, 0.4, [&](size_t) { return in[k++]; });
        // the final stage still follows `sat`; with sat > 0 it is the only thing that differs
        auto e = made({{Source, 1}, {Filters, 6}, {Boost, 24}, {Sat, 0.0f}, {Mix, 0}});
        e->noteOn(50, 1.0f, 1);
        std::vector<float> out0;
        k = 0;
        feed(*e, out0, 0, 0.4, [&](size_t) { return in[k++]; });
        for (size_t i = 0; i < out0.size(); ++i) REQUIRE(out0[i] == in[i]);
    }
    {   // sat 0 changes nothing against the same patch's resonated sound; sat > 0 does
        auto run = [&](float sat) {
            auto d = made({{Source, 1}, {Filters, 3}, {Boost, 12}, {Sat, sat}, {Mix, 1}});
            d->noteOn(50, 1.0f, 1);
            std::vector<float> out;
            size_t k = 0;
            feed(*d, out, 0, 0.4, [&](size_t) { return in[k++]; });
            return out;
        };
        const auto a = run(0.0f), b = run(0.8f);
        double diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff += std::abs(double(a[i]) - double(b[i]));
        CHECK(diff > 1.0);
    }
}

TEST_CASE("chordres: the final saturation is tasteful: unity for small signals, bounded for loud ones", "[device][chordres]") {
    auto run = [&](float sat, float amp) {
        auto d = made({{Source, 1}, {Filters, 1}, {Boost, 0}, {Sat, sat}, {Mix, 0}});
        std::vector<float> out;
        feed(*d, out, 0, 0.6, sine(300.0, amp));
        return out;
    };
    const auto clean = run(0.0f, 0.01f), quiet = run(1.0f, 0.01f);
    CHECK(std::abs(db(tailRms(quiet, 8192)) - db(tailRms(clean, 8192))) < 1.5);      // small signals: about unity gain
    const auto loud = run(1.0f, 1.0f);
    float peak = 0;
    for (float v : loud) { REQUIRE(std::isfinite(v)); peak = std::max(peak, std::abs(v)); }
    CHECK(peak < 1.2f);
    CHECK(tailRms(loud, 8192) < tailRms(run(0.0f, 1.0f), 8192) * 1.01);              // it compresses: never louder than the clean one
}

TEST_CASE("chordres: block size does not change the sound", "[device][chordres]") {
    auto run = [&](int block) {
        auto d = made({{Source, 1}, {Filters, 4}, {Tonic, 1}, {Boost, 20}, {Sat, 0.3f}, {Excite, 0.5f}});
        d->noteOn(52, 1.0f, 1);
        std::vector<float> out;
        feed(*d, out, 0, 0.5, noise(0.3f, 11), block);
        return out;
    };
    const auto a = run(128), b = run(64), c = run(32);
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) REQUIRE(a[i] == b[i]);
    for (size_t i = 0; i < std::min(a.size(), c.size()); ++i) REQUIRE(a[i] == c[i]);
}

TEST_CASE("chordres: runs without allocating", "[device][chordres][noalloc]") {
    for (float source : {1.0f, 2.0f}) {
        auto d = made({{Source, source}, {Filters, 6}, {Tonic, 1}, {Boost, 24}, {Sat, 0.5f}});
        d->noteOn(50, 1.0f, 1);
        std::vector<float> l(kMaxBlock, 0.1f), r(kMaxBlock, 0.1f);
        test::AllocGuard guard;
        for (int b = 0; b < 100; ++b) {
            for (int i = 0; i < kMaxBlock; ++i) l[size_t(i)] = r[size_t(i)] = 0.3f * float(std::sin(0.05 * double(b * kMaxBlock + i)));
            if (b == 40) d->noteOn(55, 1.0f, 2);
            d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        }
        CHECK(guard.count() == 0);
    }
}

TEST_CASE("chordres: other sample rates tune the same", "[device][chordres]") {
    for (double sr : {48000.0, 96000.0}) {
        auto d = createEffect("chordres");
        d->prepare(sr, kMaxBlock);
        d->setParam(Source, 1); d->setParam(Filters, 1); d->setParam(I1, 7); d->setParam(Boost, 12); d->setParam(Q, 40); d->setParam(Excite, 0);
        d->reset();
        d->noteOn(57, 1.0f, 1);
        std::vector<float> l(kMaxBlock), r(kMaxBlock), out;
        std::mt19937 g(5);
        for (size_t t = 0; t < size_t(2.0 * sr); t += kMaxBlock) {
            for (int i = 0; i < kMaxBlock; ++i) l[size_t(i)] = r[size_t(i)] = 0.1f * std::uniform_real_distribution<float>(-1.0f, 1.0f)(g);
            d->process(l.data(), r.data(), kMaxBlock, ctx(sr), {});
            out.insert(out.end(), l.begin(), l.end());
        }
        const float* tail = out.data() + out.size() - 24000;
        double best = 0, bm = -1;
        for (double f = 300.0; f <= 360.0; f += 1.0) {
            double re = 0, im = 0;
            const double w = 2.0 * std::numbers::pi * f / sr;
            for (size_t i = 0; i < 24000; ++i) { re += double(tail[i]) * std::cos(w * double(i)); im += double(tail[i]) * std::sin(w * double(i)); }
            if (std::hypot(re, im) > bm) { bm = std::hypot(re, im); best = f; }
        }
        CHECK(std::abs(best - hzOf(64)) < 6.0);
    }
}
