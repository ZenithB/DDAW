#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <tuple>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {

constexpr double kTwoPi = 6.283185307179586;

SamplePtr sineBuf(float sr, float hz, float secs, bool stereo = false) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = sr;
    const size_t n = size_t(sr * secs);
    b->l.resize(n);
    for (size_t i = 0; i < n; ++i) b->l[i] = float(std::sin(kTwoPi * double(hz) * double(i) / double(sr)));
    if (stereo) b->r = b->l;
    return b;
}

SamplePtr constBuf(float v, float secs, float sr = 44100.0f, float vr = -1.0f) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = sr;
    b->l.assign(size_t(sr * secs), v);
    if (vr >= 0.0f) b->r.assign(b->l.size(), vr);
    return b;
}

SamplePtr rampBuf(float secs, float sr = 44100.0f) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = sr;
    const size_t n = size_t(sr * secs);
    b->l.resize(n);
    for (size_t i = 0; i < n; ++i) b->l[i] = float(i) / float(n);
    return b;
}

// First half `lo`, second half `hi`.
SamplePtr stepBuf(float lo, float hi, float secs, float sr = 44100.0f) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = sr;
    const size_t n = size_t(sr * secs);
    b->l.resize(n);
    for (size_t i = 0; i < n; ++i) b->l[i] = i < n / 2 ? lo : hi;
    return b;
}

std::unique_ptr<InstrumentDevice> makeBare() { return createInstrument("granular"); }

std::unique_ptr<InstrumentDevice> makeLoaded() {
    auto d = createInstrument("granular");
    d->setSample(0, sineBuf(44100.0f, 330.0f, 2.0f));
    return d;
}

int idx(InstrumentDevice& d, const char* key) { return findParam(d.params(), key); }
void set(InstrumentDevice& d, const char* k, float v) { d.setParam(uint16_t(idx(d, k)), v); }

struct Out { std::vector<float> l, r; };

Out renderLR(InstrumentDevice& d, int blocks, double sr = kSr) {
    Out o;
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(sr), {});
        o.l.insert(o.l.end(), l.begin(), l.end());
        o.r.insert(o.r.end(), r.begin(), r.end());
    }
    return o;
}
std::vector<float> render(InstrumentDevice& d, int blocks, double sr = kSr) { return renderLR(d, blocks, sr).l; }
int blocksFor(double secs, double sr = kSr) { return int(std::ceil(secs * sr / kMaxBlock)); }

struct Run { size_t start, len; };

// Contiguous non-silent segments (grains that do not overlap leave zero gaps between them).
std::vector<Run> runsOf(const std::vector<float>& x, float thr = 1e-7f) {
    std::vector<Run> runs;
    size_t i = 0;
    while (i < x.size()) {
        if (std::abs(x[i]) > thr) {
            size_t j = i;
            while (j < x.size() && std::abs(x[j]) > thr) ++j;
            runs.push_back({i, j - i});
            i = j;
        } else {
            ++i;
        }
    }
    return runs;
}

// Frequency of a segment from its zero crossings.
double runHz(const std::vector<float>& x, const Run& r, double sr = kSr) {
    int zc = 0;
    for (size_t i = r.start + 1; i < r.start + r.len; ++i) zc += (x[i - 1] >= 0) != (x[i] >= 0);
    return 0.5 * zc / (double(r.len) / sr);
}

// Common setup: isolated grains (dens 2 -> one grain per 0.5 s), no randomness, centred pan.
void isolated(InstrumentDevice& d, float size = 0.2f) {
    set(d, "size", size); set(d, "dens", 2); set(d, "spray", 0); set(d, "pjit", 0); set(d, "rev", 0);
    set(d, "spread", 0); set(d, "pos", 0.1f); set(d, "attack", 0.001f); set(d, "shape", 0.5f);
}

// Median grain frequency of a sustained note rendered from `buf` at engine rate `sr`.
double playedHz(SamplePtr buf, uint8_t note, float pitchParam = 0.0f, double sr = kSr) {
    auto d = makeBare(); d->prepare(sr, kMaxBlock);
    d->setSample(0, std::move(buf));
    isolated(*d); set(*d, "pitch", pitchParam);
    d->noteOn(note, 1.0f, 1);
    auto x = render(*d, blocksFor(2.0, sr), sr);
    auto runs = runsOf(x);
    REQUIRE(runs.size() >= 3);
    std::vector<double> hz;
    for (size_t i = 1; i + 1 < runs.size(); ++i) hz.push_back(runHz(x, runs[i], sr));
    std::sort(hz.begin(), hz.end());
    return hz[hz.size() / 2];
}

double peakAbs(const std::vector<float>& x, size_t a = 0, size_t b = size_t(-1)) {
    double p = 0;
    for (size_t i = a; i < std::min(b, x.size()); ++i) p = std::max(p, double(std::abs(x[i])));
    return p;
}
}  // namespace

// The kit's phrase section needs sound, so the contract runs against a device with a sample loaded.
TEST_CASE("granular: device contract (sample loaded)", "[device][granular]") { checkInstrumentContract(makeLoaded); }

TEST_CASE("granular: no sample plays silence, never crashes", "[device][granular]") {
    auto d = makeBare(); d->prepare(kSr, kMaxBlock);
    auto silent = [&](const char* what) {
        INFO(what);
        std::vector<float> l(kMaxBlock, 0.25f), r(kMaxBlock, -0.25f);
        d->noteOn(60, 1.0f, 1);
        d->noteOn(64, 0.5f, 2);
        for (int b = 0; b < 100; ++b) d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        d->noteOff(1); d->noteOff(2);
        for (int b = 0; b < 20; ++b) d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        for (float x : l) REQUIRE(x == 0.25f);
        for (float x : r) REQUIRE(x == -0.25f);
    };
    silent("never set");
    d->setSample(0, nullptr);
    silent("null sample");
    d->setSample(0, std::make_shared<SampleBuf>());
    silent("empty sample");
    d->setSample(1, sineBuf(44100.0f, 440.0f, 1.0f));
    silent("slot 1 is ignored");
    d->setSample(0, sineBuf(44100.0f, 440.0f, 1.0f));
    d->setSample(0, nullptr);
    silent("sample removed again");
}

TEST_CASE("granular: sample loaded before prepare survives it; a phrase sounds", "[device][granular]") {
    auto d = makeLoaded(); d->prepare(kSr, kMaxBlock);
    d->noteOn(60, 1.0f, 1);
    CHECK(harness::rms(render(*d, 100)) > 0.01);
}

TEST_CASE("granular: pitch follows the note, the pitch param and the buffer's own rate", "[device][granular]") {
    SECTION("44.1 kHz buffer, 440 Hz sine") {
        CHECK(playedHz(sineBuf(44100.0f, 440.0f, 2.0f), 60) == Catch::Approx(440.0).epsilon(0.015));
        CHECK(playedHz(sineBuf(44100.0f, 440.0f, 2.0f), 72) == Catch::Approx(880.0).epsilon(0.015));
        CHECK(playedHz(sineBuf(44100.0f, 440.0f, 2.0f), 48) == Catch::Approx(220.0).epsilon(0.015));
        CHECK(playedHz(sineBuf(44100.0f, 440.0f, 2.0f), 60, 12.0f) == Catch::Approx(880.0).epsilon(0.015));
        CHECK(playedHz(sineBuf(44100.0f, 440.0f, 2.0f), 67) == Catch::Approx(440.0 * std::pow(2.0, 7.0 / 12.0)).epsilon(0.015));
    }
    SECTION("22.05 kHz buffer resampled to the 44.1 kHz engine") {
        CHECK(playedHz(sineBuf(22050.0f, 440.0f, 2.0f), 60) == Catch::Approx(440.0).epsilon(0.015));
        CHECK(playedHz(sineBuf(22050.0f, 440.0f, 2.0f), 72) == Catch::Approx(880.0).epsilon(0.015));
    }
    SECTION("48 kHz buffer resampled to the 44.1 kHz engine") {
        CHECK(playedHz(sineBuf(48000.0f, 440.0f, 2.0f), 60) == Catch::Approx(440.0).epsilon(0.015));
        CHECK(playedHz(sineBuf(48000.0f, 440.0f, 2.0f), 55) == Catch::Approx(440.0 * std::pow(2.0, -5.0 / 12.0)).epsilon(0.015));
    }
    SECTION("44.1 kHz buffer on a 48 kHz engine") {
        CHECK(playedHz(sineBuf(44100.0f, 440.0f, 2.0f), 60, 0.0f, 48000.0) == Catch::Approx(440.0).epsilon(0.015));
    }
    SECTION("stereo buffer plays at the same pitch") {
        CHECK(playedHz(sineBuf(44100.0f, 440.0f, 2.0f, true), 60) == Catch::Approx(440.0).epsilon(0.015));
    }
}

TEST_CASE("granular: pitch jitter spreads grain pitches, zero jitter does not", "[device][granular]") {
    auto spreadOf = [](float pjit) {
        auto d = makeBare(); d->prepare(kSr, kMaxBlock);
        d->setSample(0, sineBuf(44100.0f, 440.0f, 2.0f));
        isolated(*d); set(*d, "pjit", pjit); set(*d, "dens", 4); set(*d, "size", 0.1f);
        d->noteOn(60, 1.0f, 1);
        auto x = render(*d, blocksFor(2.0));
        auto runs = runsOf(x);
        REQUIRE(runs.size() >= 4);
        double lo = 1e9, hi = 0;
        for (size_t i = 1; i + 1 < runs.size(); ++i) { const double h = runHz(x, runs[i]); lo = std::min(lo, h); hi = std::max(hi, h); }
        return hi / lo;
    };
    CHECK(spreadOf(0.0f) < 1.02);
    CHECK(spreadOf(12.0f) > 1.15);
}

TEST_CASE("granular: grain size and shape set the window", "[device][granular]") {
    auto d = makeBare(); d->prepare(kSr, kMaxBlock);
    d->setSample(0, constBuf(1.0f, 4.0f));
    isolated(*d, 0.1f); set(*d, "pos", 0.5f);
    SECTION("size gives the grain length") {
        d->noteOn(60, 1.0f, 1);
        auto x = render(*d, blocksFor(1.8));
        auto runs = runsOf(x);
        REQUIRE(runs.size() >= 3);
        for (size_t i = 1; i < runs.size(); ++i) CHECK(double(runs[i].len) == Catch::Approx(0.1 * kSr).margin(4));
        // grain period = 1 / dens = 0.5 s
        CHECK(double(runs[2].start - runs[1].start) == Catch::Approx(0.5 * kSr).margin(3));
    }
    SECTION("window peak is 0.8 * velocity * 0.9 and sits at shape * length") {
        for (float shape : {0.25f, 0.5f, 0.75f}) {
            INFO("shape " << shape);
            d->reset();
            set(*d, "shape", shape);
            d->noteOn(60, 1.0f, 1);
            auto x = render(*d, blocksFor(1.2));
            auto runs = runsOf(x);
            REQUIRE(runs.size() >= 2);
            const Run g = runs[1];                       // past the attack ramp
            size_t am = g.start;
            for (size_t i = g.start; i < g.start + g.len; ++i) if (x[i] > x[am]) am = i;
            CHECK(double(x[am]) == Catch::Approx(0.72).margin(0.01));
            CHECK(double(am - g.start) / double(g.len) == Catch::Approx(double(shape)).margin(0.02));
        }
    }
    SECTION("velocity scales the level") {
        d->noteOn(60, 0.5f, 1);
        auto x = render(*d, blocksFor(1.2));
        CHECK(peakAbs(x, size_t(0.4 * kSr)) == Catch::Approx(0.36).margin(0.01));
    }
    SECTION("size is clamped to 20 ms .. 500 ms") {
        set(*d, "size", 0.0f);
        d->noteOn(60, 1.0f, 1);
        auto x = render(*d, blocksFor(1.2));
        auto runs = runsOf(x);
        REQUIRE(runs.size() >= 2);
        CHECK(double(runs[1].len) == Catch::Approx(0.02 * kSr).margin(4));
    }
}

TEST_CASE("granular: density sets how many grains start per second", "[device][granular]") {
    auto count = [](float dens, double secs) {
        auto d = makeBare(); d->prepare(kSr, kMaxBlock);
        d->setSample(0, constBuf(1.0f, 4.0f));
        isolated(*d, 0.02f); set(*d, "dens", dens); set(*d, "pos", 0.5f);
        d->noteOn(60, 1.0f, 1);
        return int(runsOf(render(*d, blocksFor(secs))).size());
    };
    CHECK(std::abs(count(10.0f, 2.0) - 20) <= 1);
    CHECK(std::abs(count(40.0f, 2.0) - 80) <= 2);
    CHECK(std::abs(count(5.0f, 3.0) - 15) <= 1);
    CHECK(std::abs(count(1.0f, 2.0) - 4) <= 1);   // clamped up to 2 grains/s
}

TEST_CASE("granular: position picks the source region, spray scatters it", "[device][granular]") {
    // First half of the source is 0.25, second half 1.0; tiny grains stay inside one half.
    auto peaksAt = [](float pos, float spray) {
        auto d = makeBare(); d->prepare(kSr, kMaxBlock);
        d->setSample(0, stepBuf(0.25f, 1.0f, 2.0f));
        isolated(*d, 0.02f); set(*d, "pos", pos); set(*d, "spray", spray); set(*d, "dens", 10);
        d->noteOn(60, 1.0f, 1);
        auto x = render(*d, blocksFor(3.0));
        std::vector<double> peaks;
        auto runs = runsOf(x);
        for (size_t i = 1; i + 1 < runs.size(); ++i) {   // the last grain may be cut by the end of the render
            double p = 0; for (size_t k = runs[i].start; k < runs[i].start + runs[i].len; ++k) p = std::max(p, double(x[k]));
            peaks.push_back(p);
        }
        return peaks;
    };
    for (double p : peaksAt(0.25f, 0.0f)) CHECK(p == Catch::Approx(0.18).margin(0.005));   // 0.72 * 0.25
    for (double p : peaksAt(0.75f, 0.0f)) CHECK(p == Catch::Approx(0.72).margin(0.005));
    auto scattered = peaksAt(0.5f, 1.0f);
    REQUIRE(scattered.size() > 10);
    const double mn = *std::min_element(scattered.begin(), scattered.end());
    const double mx = *std::max_element(scattered.begin(), scattered.end());
    CHECK(mn < 0.25);
    CHECK(mx > 0.6);
}

TEST_CASE("granular: reverse plays the grain backwards", "[device][granular]") {
    // On a rising ramp a forward grain is rising (positive correlation with time about its centre),
    // a reversed grain falls.
    auto tilt = [](float rev) {
        auto d = makeBare(); d->prepare(kSr, kMaxBlock);
        d->setSample(0, rampBuf(2.0f));
        isolated(*d, 0.1f); set(*d, "rev", rev); set(*d, "pos", 0.5f); set(*d, "dens", 4);
        d->noteOn(60, 1.0f, 1);
        auto x = render(*d, blocksFor(2.0));
        auto runs = runsOf(x);
        REQUIRE(runs.size() >= 3);
        double sum = 0; int pos = 0, neg = 0;
        for (size_t i = 1; i + 1 < runs.size(); ++i) {
            double c = 0, mid = double(runs[i].len) / 2;
            for (size_t k = 0; k < runs[i].len; ++k) c += (double(k) - mid) * double(x[runs[i].start + k]);
            sum += c; (c > 0 ? pos : neg)++;
        }
        return std::make_tuple(sum, pos, neg);
    };
    auto [fwd, fp, fn] = tilt(0.0f);
    auto [bwd, bp, bn] = tilt(1.0f);
    CHECK(fwd > 0); CHECK(fn == 0);
    CHECK(bwd < 0); CHECK(bp == 0);
}

TEST_CASE("granular: spread pans grains, mono and stereo buffers", "[device][granular]") {
    auto d = makeBare(); d->prepare(kSr, kMaxBlock);
    SECTION("spread 0: mono buffer is identical on both sides") {
        d->setSample(0, constBuf(1.0f, 2.0f));
        isolated(*d, 0.05f); set(*d, "dens", 20);
        d->noteOn(60, 1.0f, 1);
        auto o = renderLR(*d, blocksFor(1.0));
        CHECK(harness::rms(o.l) > 0.01);
        CHECK(o.l == o.r);
    }
    SECTION("spread 1: grains land at different pans") {
        d->setSample(0, constBuf(1.0f, 2.0f));
        isolated(*d, 0.02f); set(*d, "dens", 10); set(*d, "spread", 1);
        d->noteOn(60, 1.0f, 1);
        auto o = renderLR(*d, blocksFor(3.0));
        auto runs = runsOf(o.l, 1e-7f);
        double minB = 1e9, maxB = -1e9;
        for (size_t i = 1; i < runs.size(); ++i) {
            double el = 0, er = 0;
            for (size_t k = runs[i].start; k < runs[i].start + runs[i].len; ++k) { el += double(o.l[k]) * o.l[k]; er += double(o.r[k]) * o.r[k]; }
            const double bal = (el - er) / (el + er + 1e-30);
            minB = std::min(minB, bal); maxB = std::max(maxB, bal);
        }
        CHECK(minB < -0.3);
        CHECK(maxB > 0.3);
    }
    SECTION("stereo buffer keeps its channels (left only in, left only out)") {
        d->setSample(0, constBuf(1.0f, 2.0f, 44100.0f, 0.0f));
        isolated(*d, 0.05f); set(*d, "dens", 20);
        d->noteOn(60, 1.0f, 1);
        auto o = renderLR(*d, blocksFor(1.0));
        CHECK(harness::rms(o.l) > 0.01);
        CHECK(peakAbs(o.r) == 0.0);
    }
    SECTION("stereo buffer with centred grains keeps L/R levels apart") {
        d->setSample(0, constBuf(1.0f, 2.0f, 44100.0f, 0.5f));
        isolated(*d, 0.05f); set(*d, "dens", 20);
        d->noteOn(60, 1.0f, 1);
        auto o = renderLR(*d, blocksFor(1.0));
        CHECK(peakAbs(o.l) == Catch::Approx(2.0 * peakAbs(o.r)).epsilon(0.01));
    }
}

TEST_CASE("granular: envelope attack and exponential release", "[device][granular]") {
    auto d = makeBare(); d->prepare(kSr, kMaxBlock);
    d->setSample(0, constBuf(1.0f, 4.0f));
    set(*d, "spray", 0); set(*d, "spread", 0); set(*d, "pos", 0.5f);
    set(*d, "size", 0.5f); set(*d, "dens", 20);
    SECTION("attack ramps the level") {
        set(*d, "attack", 1.0f);
        d->noteOn(60, 1.0f, 1);
        auto x = render(*d, blocksFor(1.5));
        const double early = peakAbs(x, 0, size_t(0.1 * kSr));
        const double late = peakAbs(x, size_t(1.2 * kSr));
        CHECK(early < 0.2 * late);
    }
    SECTION("noteOff fades out after the release, and only the matching id counts") {
        set(*d, "attack", 0.005f); set(*d, "release", 0.1f);
        d->noteOn(60, 1.0f, 1);
        render(*d, blocksFor(0.3));
        d->noteOff(77);                                  // unknown id: harmless
        CHECK(peakAbs(render(*d, blocksFor(0.05))) > 0.3);
        d->noteOff(1);
        const auto mid = render(*d, blocksFor(0.05));    // still audible just after the off
        CHECK(peakAbs(mid) > 0.01);
        // exponential: the tail dies by ~3.1 x release (+ block rounding)
        auto tail = render(*d, blocksFor(0.5));
        CHECK(peakAbs(tail, size_t(0.45 * kSr)) == 0.0);
        // monotone-ish decay: later peaks are lower
        CHECK(peakAbs(tail, size_t(0.2 * kSr), size_t(0.3 * kSr)) < peakAbs(mid));
    }
    SECTION("a same-pitch retrigger is absorbed and its id still balances the off") {
        set(*d, "release", 0.05f);
        d->noteOn(60, 1.0f, 1);
        d->noteOn(60, 1.0f, 2);                          // browser: ignored (still one voice)
        render(*d, blocksFor(0.2));
        d->noteOff(1);
        CHECK(harness::rms(render(*d, blocksFor(0.1))) > 0.05);   // id 2 still holds the voice
        d->noteOff(2);
        render(*d, blocksFor(0.4));
        CHECK(peakAbs(render(*d, 10)) == 0.0);
    }
}

TEST_CASE("granular: voice and grain pools are fixed", "[device][granular]") {
    auto d = makeBare(); d->prepare(kSr, kMaxBlock);
    d->setSample(0, constBuf(1.0f, 4.0f));
    isolated(*d, 0.1f); set(*d, "pos", 0.5f); set(*d, "attack", 0.001f); set(*d, "release", 0.05f);
    auto peakWith = [&](int notes) {
        d->reset();
        for (int i = 0; i < notes; ++i) d->noteOn(uint8_t(40 + i), 1.0f, uint32_t(i + 1));
        auto x = render(*d, blocksFor(1.0));
        return peakAbs(x, size_t(0.4 * kSr));            // the second grain of every voice (all aligned)
    };
    const double one = peakWith(1);
    CHECK(one == Catch::Approx(0.72).margin(0.01));
    CHECK(peakWith(10) == Catch::Approx(10.0 * one).epsilon(0.02));
    CHECK(peakWith(14) == Catch::Approx(10.0 * one).epsilon(0.02));   // notes beyond 10 voices are dropped

    SECTION("a freed voice can play again") {
        d->reset();
        for (int i = 0; i < 10; ++i) d->noteOn(uint8_t(40 + i), 1.0f, uint32_t(i + 1));
        for (int i = 0; i < 10; ++i) d->noteOff(uint32_t(i + 1));
        render(*d, blocksFor(0.6));
        d->noteOn(60, 1.0f, 100);
        CHECK(peakAbs(render(*d, blocksFor(0.2))) > 0.1);
    }
    SECTION("an exhausted grain pool skips spawns: finite, bounded by 256 grains, no allocation") {
        set(*d, "dens", 80); set(*d, "size", 0.5f); set(*d, "release", 4.0f);
        d->reset();
        for (int i = 0; i < 10; ++i) d->noteOn(uint8_t(40 + i), 1.0f, uint32_t(i + 1));
        std::vector<float> l(kMaxBlock), r(kMaxBlock);
        test::AllocGuard guard;
        double peak = 0;
        for (int b = 0; b < blocksFor(1.5); ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
            REQUIRE(finiteAndBounded(l, 256.0f));
            peak = std::max(peak, peakAbs(l));
        }
        CHECK(guard.count() == 0);
        CHECK(peak > 5.0);                               // heavily layered
        CHECK(peak <= 256.0 * 0.72 + 0.01);              // never more than a full pool
    }
}

TEST_CASE("granular: reset makes a random cloud repeatable, and the RNG is reseeded", "[device][granular]") {
    auto d = makeBare(); d->prepare(kSr, kMaxBlock);
    d->setSample(0, sineBuf(44100.0f, 330.0f, 2.0f));
    set(*d, "spray", 0.5f); set(*d, "pjit", 4.0f); set(*d, "rev", 0.5f); set(*d, "spread", 1.0f); set(*d, "dens", 30);
    auto run = [&] {
        d->reset();
        d->noteOn(60, 1.0f, 1);
        d->noteOn(67, 0.7f, 2);
        auto o = renderLR(*d, blocksFor(0.7));
        d->noteOff(1); d->noteOff(2);
        auto t = renderLR(*d, blocksFor(0.3));
        o.l.insert(o.l.end(), t.l.begin(), t.l.end());
        o.r.insert(o.r.end(), t.r.begin(), t.r.end());
        return o;
    };
    auto a = run();
    auto b = run();
    CHECK(harness::rms(a.l) > 0.01);
    CHECK(a.l == b.l);
    CHECK(a.r == b.r);
    // a second instance with the same setup produces the same cloud
    auto d2 = makeBare(); d2->prepare(kSr, kMaxBlock);
    d2->setSample(0, sineBuf(44100.0f, 330.0f, 2.0f));
    for (auto [k, v] : std::vector<std::pair<const char*, float>>{{"spray", 0.5f}, {"pjit", 4.0f}, {"rev", 0.5f}, {"spread", 1.0f}, {"dens", 30.0f}})
        set(*d2, k, v);
    d2->reset();
    d2->noteOn(60, 1.0f, 1);
    d2->noteOn(67, 0.7f, 2);
    CHECK(renderLR(*d2, blocksFor(0.7)).l == std::vector<float>(a.l.begin(), a.l.begin() + long(blocksFor(0.7)) * kMaxBlock));
}

TEST_CASE("granular: arbitrary block sizes, sweeps with a sample, extremes stay bounded", "[device][granular]") {
    auto d = makeLoaded(); d->prepare(kSr, kMaxBlock);
    d->noteOn(60, 1.0f, 1);
    std::vector<float> l(1000, 0.0f), r(1000, 0.0f);
    d->process(l.data(), r.data(), 1000, ctx(), {});     // bigger than kMaxBlock: processed in chunks
    CHECK(finiteAndBounded(l));
    CHECK(harness::rms(l) > 0.0);
    for (auto [k, v] : std::vector<std::pair<const char*, float>>{{"size", 0.5f}, {"dens", 80.0f}, {"pos", 1.0f}, {"spray", 1.0f}, {"pitch", 24.0f}, {"pjit", 12.0f}, {"rev", 1.0f}, {"shape", 0.0f}, {"spread", 1.0f}})
        set(*d, k, v);
    d->noteOn(84, 1.0f, 2);
    d->noteOn(36, 1.0f, 3);
    auto x = renderLR(*d, blocksFor(1.0));
    CHECK(finiteAndBounded(x.l));
    CHECK(finiteAndBounded(x.r));
    d->noteOff(2);
    auto y = render(*d, blocksFor(0.5));
    CHECK(finiteAndBounded(y));
}

TEST_CASE("granular: no allocation with a sample loaded (noteOn, noteOff, process, reset)", "[device][granular]") {
    auto d = makeLoaded(); d->prepare(kSr, kMaxBlock);
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    set(*d, "dens", 60); set(*d, "spray", 0.8f); set(*d, "pjit", 5.0f); set(*d, "rev", 0.5f);
    test::AllocGuard guard;
    for (int b = 0; b < 400; ++b) {
        if (b % 10 == 0) d->noteOn(uint8_t(40 + b % 30), 0.8f, uint32_t(b + 1));
        if (b % 10 == 7) d->noteOff(uint32_t(b - 6));
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
    }
    d->reset();
    CHECK(guard.count() == 0);
}
