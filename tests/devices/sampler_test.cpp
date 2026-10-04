#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
constexpr double kPi = 3.14159265358979323846;

SamplePtr sine(double srcRate, double hz, double secs, bool stereo = false, double hzR = 0.0) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = float(srcRate);
    const size_t n = size_t(srcRate * secs);
    b->l.resize(n);
    for (size_t i = 0; i < n; ++i) b->l[i] = float(std::sin(2.0 * kPi * hz * double(i) / srcRate));
    if (stereo) {
        b->r.resize(n);
        for (size_t i = 0; i < n; ++i) b->r[i] = float(std::sin(2.0 * kPi * hzR * double(i) / srcRate));
    }
    return b;
}
SamplePtr constant(double srcRate, double secs, float v = 1.0f) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = float(srcRate);
    b->l.assign(size_t(srcRate * secs), v);
    return b;
}

std::unique_ptr<InstrumentDevice> makeBare() { return createInstrument("sampler"); }
std::unique_ptr<InstrumentDevice> makeWith(SamplePtr s) {
    auto d = makeBare();
    d->setSample(0, std::move(s));
    return d;
}
void set(InstrumentDevice& d, const char* k, float v) { d.setParam(uint16_t(findParam(d.params(), k)), v); }

struct Out { std::vector<float> l, r; };
Out render(InstrumentDevice& d, double secs, double sr = kSr) {
    Out o;
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    const int blocks = int(secs * sr / kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(sr), {});
        o.l.insert(o.l.end(), l.begin(), l.end());
        o.r.insert(o.r.end(), r.begin(), r.end());
    }
    return o;
}
std::vector<float> slice(const std::vector<float>& x, double a, double b, double sr = kSr) {
    return std::vector<float>(x.begin() + long(a * sr), x.begin() + long(std::min(b * sr, double(x.size()))));
}
// Frequency from interpolated rising zero crossings (precise to a small fraction of a percent).
double freqOf(const std::vector<float>& x, double sr) {
    std::vector<double> t;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) t.push_back(double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]));
    if (t.size() < 3) return 0.0;
    return double(t.size() - 1) * sr / (t.back() - t.front());
}
double peak(const std::vector<float>& x) { double m = 0; for (float v : x) m = std::max(m, double(std::abs(v))); return m; }
}  // namespace

TEST_CASE("sampler: device contract (procedural sample loaded)", "[device][sampler]") {
    checkInstrumentContract([] { return makeWith(sine(kSr, 220.0, 2.0)); });
}

TEST_CASE("sampler: a missing sample is silent and additive, never crashes", "[device][sampler]") {
    for (int mode = 0; mode < 3; ++mode) {
        auto d = makeBare(); d->prepare(kSr, kMaxBlock);
        if (mode == 1) d->setSample(0, nullptr);
        if (mode == 2) { d->setSample(0, sine(kSr, 220.0, 1.0)); d->setSample(0, nullptr); }
        d->noteOn(48, 1.0f, 1); d->noteOn(60, 1.0f, 2); d->noteOff(1);
        std::vector<float> l(kMaxBlock, 0.25f), r(kMaxBlock, -0.25f);
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        for (float x : l) REQUIRE(x == 0.25f);
        for (float x : r) REQUIRE(x == -0.25f);
        d->reset();
    }
    // an empty buffer and an out-of-range slot are harmless too
    auto d = makeBare(); d->prepare(kSr, kMaxBlock);
    d->setSample(0, std::make_shared<SampleBuf>());
    d->setSample(5, sine(kSr, 220.0, 1.0));
    d->noteOn(48, 1.0f, 1);
    CHECK(peak(render(*d, 0.1).l) == 0.0);
}

TEST_CASE("sampler: no allocation in noteOn/noteOff/setParam/process with a sample", "[device][sampler]") {
    auto d = makeWith(sine(kSr, 220.0, 1.0)); d->prepare(kSr, kMaxBlock);
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    test::AllocGuard guard;
    for (int b = 0; b < 300; ++b) {
        if (b % 5 == 0) d->noteOn(uint8_t(30 + b % 40), 0.9f, uint32_t(b + 1));
        if (b % 5 == 3) d->noteOff(uint32_t(b - 2));
        if (b % 7 == 0) { set(*d, "tune", float(b % 24)); set(*d, "attack", 0.01f); set(*d, "release", 0.2f); }
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
    }
    d->reset();
    CHECK(guard.count() == 0);
}

TEST_CASE("sampler: pitch maps to playback rate, root key is C3 (MIDI 48)", "[device][sampler]") {
    auto freq = [](uint8_t pitch, float tune) {
        auto d = makeWith(sine(kSr, 100.0, 4.0)); d->prepare(kSr, kMaxBlock);
        set(*d, "attack", 0.001f); set(*d, "tune", tune);
        d->noteOn(pitch, 1.0f, 1);
        return freqOf(slice(render(*d, 1.0).l, 0.05, 1.0), kSr);
    };
    CHECK(freq(48, 0) == Catch::Approx(100.0).epsilon(0.003));          // root: natural
    CHECK(freq(60, 0) == Catch::Approx(200.0).epsilon(0.003));          // +12 st
    CHECK(freq(36, 0) == Catch::Approx(50.0).epsilon(0.003));           // -12 st
    CHECK(freq(55, 0) == Catch::Approx(100.0 * std::pow(2.0, 7.0 / 12.0)).epsilon(0.003));
}

TEST_CASE("sampler: tune shifts the rate in semitones", "[device][sampler]") {
    auto freq = [](float tune) {
        auto d = makeWith(sine(kSr, 100.0, 4.0)); d->prepare(kSr, kMaxBlock);
        set(*d, "attack", 0.001f); set(*d, "tune", tune);
        d->noteOn(48, 1.0f, 1);
        return freqOf(slice(render(*d, 1.0).l, 0.05, 1.0), kSr);
    };
    CHECK(freq(12.0f) == Catch::Approx(200.0).epsilon(0.003));
    CHECK(freq(-12.0f) == Catch::Approx(50.0).epsilon(0.003));
    CHECK(freq(7.0f) == Catch::Approx(100.0 * std::pow(2.0, 7.0 / 12.0)).epsilon(0.003));
    CHECK(freq(-0.5f) == Catch::Approx(100.0 * std::pow(2.0, -0.5 / 12.0)).epsilon(0.003));
}

TEST_CASE("sampler: resampling from 22.05 kHz and 48 kHz sources keeps the real-time pitch", "[device][sampler]") {
    for (double engine : {44100.0, 48000.0}) {
        for (double src : {22050.0, 48000.0}) {
            INFO("engine " << engine << " source " << src);
            auto d = makeWith(sine(src, 100.0, 3.0)); d->prepare(engine, kMaxBlock);
            set(*d, "attack", 0.001f);
            d->noteOn(48, 1.0f, 1);
            auto x = render(*d, 1.0, engine);
            CHECK(freqOf(slice(x.l, 0.05, 1.0, engine), engine) == Catch::Approx(100.0).epsilon(0.003));
            // and the one-shot length is real time: a 0.5 s source ends after 0.5 s whatever the rates
            auto e = makeWith(constant(src, 0.5)); e->prepare(engine, kMaxBlock);
            set(*e, "attack", 0.001f);
            e->noteOn(48, 1.0f, 1);
            auto y = render(*e, 1.0, engine);
            CHECK(peak(slice(y.l, 0.45, 0.49, engine)) > 0.5);
            CHECK(peak(slice(y.l, 0.52, 1.0, engine)) == 0.0);
        }
    }
}

TEST_CASE("sampler: mono source plays on both sides, stereo source keeps its image", "[device][sampler]") {
    auto m = makeWith(sine(kSr, 100.0, 2.0)); m->prepare(kSr, kMaxBlock);
    m->noteOn(48, 1.0f, 1);
    auto mo = render(*m, 0.3);
    CHECK(mo.l == mo.r);
    CHECK(peak(mo.l) > 0.5);

    auto s = makeWith(sine(kSr, 100.0, 2.0, true, 200.0)); s->prepare(kSr, kMaxBlock);
    set(*s, "attack", 0.001f);
    s->noteOn(48, 1.0f, 1);
    auto so = render(*s, 1.0);
    CHECK(freqOf(slice(so.l, 0.05, 1.0), kSr) == Catch::Approx(100.0).epsilon(0.003));
    CHECK(freqOf(slice(so.r, 0.05, 1.0), kSr) == Catch::Approx(200.0).epsilon(0.003));
}

TEST_CASE("sampler: attack ramps in, velocity scales, output gain is 0.9", "[device][sampler]") {
    auto d = makeWith(constant(kSr, 3.0)); d->prepare(kSr, kMaxBlock);
    set(*d, "attack", 0.2f);
    d->noteOn(48, 1.0f, 1);
    auto x = render(*d, 0.6).l;
    auto at = [&](double t) { return double(x[size_t(t * kSr)]); };
    CHECK(at(0.1) == Catch::Approx(0.45).margin(0.02));                  // halfway up the 0.2 s ramp
    CHECK(at(0.15) < at(0.19));
    CHECK(at(0.4) == Catch::Approx(0.9).margin(0.005));                  // full level = 0.9 * vel
    CHECK(at(0.5) == Catch::Approx(0.9).margin(0.005));                  // held, no decay

    auto h = makeWith(constant(kSr, 3.0)); h->prepare(kSr, kMaxBlock);
    set(*h, "attack", 0.001f);
    h->noteOn(48, 0.5f, 1);
    CHECK(render(*h, 0.2).l.back() == Catch::Approx(0.45).margin(0.005));
}

TEST_CASE("sampler: noteOff releases over `release`, then the voice is free", "[device][sampler]") {
    auto d = makeWith(constant(kSr, 3.0)); d->prepare(kSr, kMaxBlock);
    set(*d, "attack", 0.001f); set(*d, "release", 0.2f);
    d->noteOn(48, 1.0f, 1);
    auto a = render(*d, 0.5).l;
    CHECK(a.back() == Catch::Approx(0.9).margin(0.005));                 // sustains while held
    d->noteOff(1);
    auto r = render(*d, 1.0).l;
    CHECK(r[size_t(0.1 * kSr)] < 0.5);                                   // mid-release, falling
    CHECK(r[size_t(0.1 * kSr)] > 0.01);
    CHECK(r[size_t(0.02 * kSr)] > r[size_t(0.1 * kSr)]);
    CHECK(peak(slice(r, 0.3, 1.0)) == 0.0);                              // over after > release
    // a longer release rings longer
    auto e = makeWith(constant(kSr, 3.0)); e->prepare(kSr, kMaxBlock);
    set(*e, "attack", 0.001f); set(*e, "release", 1.5f);
    e->noteOn(48, 1.0f, 1);
    render(*e, 0.2);
    e->noteOff(1);
    CHECK(peak(slice(render(*e, 0.8).l, 0.6, 0.8)) > 0.01);
}

TEST_CASE("sampler: one-shot ends with the sample even while the note is held", "[device][sampler]") {
    auto d = makeWith(sine(kSr, 220.0, 0.2)); d->prepare(kSr, kMaxBlock);
    set(*d, "attack", 0.001f);
    d->noteOn(48, 1.0f, 1);                                              // never released
    auto x = render(*d, 1.0).l;
    CHECK(harness::rms(slice(x, 0.0, 0.15)) > 0.1);
    CHECK(peak(slice(x, 0.25, 1.0)) == 0.0);
    // a higher pitch finishes the same sample sooner
    auto u = makeWith(sine(kSr, 220.0, 0.4)); u->prepare(kSr, kMaxBlock);
    set(*u, "attack", 0.001f);
    u->noteOn(60, 1.0f, 1);
    auto y = render(*u, 0.5).l;
    CHECK(peak(slice(y, 0.15, 0.19)) > 0.5);
    CHECK(peak(slice(y, 0.23, 0.5)) == 0.0);
}

TEST_CASE("sampler: 12 voices, oldest stolen, a stale noteOff is ignored", "[device][sampler]") {
    auto d = makeWith(constant(kSr, 10.0)); d->prepare(kSr, kMaxBlock);
    set(*d, "attack", 0.001f); set(*d, "release", 0.01f);
    for (uint32_t id = 1; id <= 12; ++id) { d->noteOn(48, 1.0f, id); render(*d, 0.01); }
    auto level = [&] { return double(render(*d, 0.05).l.back()); };
    CHECK(level() == Catch::Approx(12 * 0.9).margin(0.01));
    d->noteOn(48, 1.0f, 13);                                             // steals id 1 (the oldest)
    CHECK(level() == Catch::Approx(12 * 0.9).margin(0.01));              // still 12 voices
    d->noteOff(1);                                                       // stale: id 1 no longer owns a voice
    CHECK(level() == Catch::Approx(12 * 0.9).margin(0.01));
    d->noteOff(2);                                                       // id 2 survived and releases
    CHECK(level() == Catch::Approx(11 * 0.9).margin(0.01));
    d->noteOff(13);
    CHECK(level() == Catch::Approx(10 * 0.9).margin(0.01));
    d->noteOff(999);                                                     // unknown id: harmless
    CHECK(level() == Catch::Approx(10 * 0.9).margin(0.01));
}

TEST_CASE("sampler: attack, release and tune are read at note start", "[device][sampler]") {
    auto d = makeWith(sine(kSr, 100.0, 4.0)); d->prepare(kSr, kMaxBlock);
    set(*d, "attack", 0.001f);
    d->noteOn(48, 1.0f, 1);
    render(*d, 0.1);
    set(*d, "tune", 12.0f);                                              // a sounding note keeps its rate
    CHECK(freqOf(slice(render(*d, 1.0).l, 0.05, 1.0), kSr) == Catch::Approx(100.0).epsilon(0.003));
}

TEST_CASE("sampler: reset silences voices, the sample survives a reset and a prepare", "[device][sampler]") {
    auto d = makeWith(sine(kSr, 100.0, 2.0)); d->prepare(kSr, kMaxBlock);
    d->noteOn(48, 1.0f, 1);
    render(*d, 0.1);
    d->reset();
    CHECK(peak(render(*d, 0.1).l) == 0.0);
    d->prepare(kSr, kMaxBlock);                                          // the builder prepares before setSample, but a re-prepare must not drop it
    d->noteOn(48, 1.0f, 2);
    CHECK(peak(render(*d, 0.1).l) > 0.1);
}
