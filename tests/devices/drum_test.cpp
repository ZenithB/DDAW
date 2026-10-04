#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("drum"); }
void set(InstrumentDevice& d, const char* k, float v) { d.setParam(uint16_t(findParam(d.params(), k)), v); }

std::vector<float> render(InstrumentDevice& d, double seconds) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    const int blocks = int(seconds * kSr / kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

std::vector<float> hit(uint8_t pad, double seconds, float vel = 1.0f) {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->noteOn(pad, vel, 1);
    return render(*d, seconds);
}

std::vector<float> slice(const std::vector<float>& x, double a, double b) {
    return std::vector<float>(x.begin() + long(a * kSr), x.begin() + long(std::min(b * kSr, double(x.size()))));
}
double energy(const std::vector<float>& x) { double e = 0; for (float v : x) e += double(v) * v; return e; }
// Energy of the first difference over total energy: ~0 for low-frequency content, large for noise/hats.
double brightness(const std::vector<float>& x) {
    double d = 0; for (size_t i = 1; i < x.size(); ++i) { const double v = double(x[i]) - x[i - 1]; d += v * v; }
    return d / std::max(energy(x), 1e-30);
}
int zeroCrossings(const std::vector<float>& x) {
    int z = 0; for (size_t i = 1; i < x.size(); ++i) z += (x[i - 1] >= 0) != (x[i] >= 0);
    return z;
}
}  // namespace

TEST_CASE("drum: device contract", "[device][drum]") { checkInstrumentContract(make); }

TEST_CASE("drum: every pad sounds, then decays to silence", "[device][drum]") {
    for (uint8_t pad = 0; pad < 8; ++pad) {
        auto x = hit(pad, 4.0);
        INFO("pad " << int(pad));
        CHECK(harness::rms(slice(x, 0.0, 0.3)) > 1e-3);
        CHECK(harness::rms(slice(x, 3.5, 4.0)) == 0.0);
    }
}

TEST_CASE("drum: onset energy decays", "[device][drum]") {
    for (uint8_t pad = 0; pad < 8; ++pad) {
        auto x = hit(pad, 2.0);
        INFO("pad " << int(pad));
        CHECK(energy(slice(x, 0.0, 0.1)) > 4.0 * energy(slice(x, 0.4, 0.5)));
    }
}

TEST_CASE("drum: pad timbre follows the pitch (kick low, hats and crash bright)", "[device][drum]") {
    const double kick = brightness(slice(hit(0, 1.0), 0.06, 1.0));
    const double snare = brightness(hit(1, 0.5));
    const double closedHat = brightness(hit(3, 0.5));
    const double openHat = brightness(hit(4, 0.5));
    const double crash = brightness(hit(7, 1.0));
    CHECK(kick < 0.05);                          // sine body at ~65 Hz
    CHECK(snare > 20.0 * kick);
    CHECK(closedHat > snare);                    // 9 kHz highpass vs 1.8 kHz bandpass
    CHECK(openHat > snare);
    CHECK(crash > snare);
    CHECK(hit(1, 0.2) != hit(3, 0.2));           // different pads, different sound
    // pitch wraps modulo 8: MIDI 36 (= pad 4) is the open hat, MIDI 40 and 48 hit pad 0
    CHECK(hit(12, 0.1) == hit(4, 0.1));
}

TEST_CASE("drum: kick is a sine near 65 Hz after its pitch drop; tune moves it", "[device][drum]") {
    auto crossings = [](float tune) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "p0_tune", tune);
        render(*d, 0.1);                                     // settle the tune smoother
        d->noteOn(0, 1.0f, 1);
        auto x = render(*d, 0.4);
        return zeroCrossings(slice(x, 0.06, 0.4));
    };
    // MIDI 36 = 65.4 Hz: 2 * 65.4 * 0.34 s ~ 44 crossings
    CHECK(crossings(0.0f) == Catch::Approx(44.0).margin(6));
    CHECK(crossings(12.0f) > 1.8 * crossings(0.0f));
}

TEST_CASE("drum: decay and level params", "[device][drum]") {
    auto tailEnergy = [](float decay) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "p1_decay", decay);
        d->noteOn(1, 1.0f, 1);
        return energy(slice(render(*d, 1.0), 0.3, 1.0));
    };
    CHECK(tailEnergy(1.0f) > 100.0 * std::max(tailEnergy(0.05f), 1e-12));

    auto level = [](float db) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        set(*d, "p1_level", db);
        d->reset();
        d->noteOn(1, 1.0f, 1);
        return harness::rms(render(*d, 0.3));
    };
    CHECK(level(6.0f) / level(-24.0f) == Catch::Approx(std::pow(10.0, 30.0 / 20.0)).epsilon(0.05));
}

TEST_CASE("drum: clap fires three bursts, noteOff is a no-op", "[device][drum]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    set(*d, "p2_decay", 0.03f);                              // short decay so the bursts separate
    d->noteOn(2, 1.0f, 1);
    d->noteOff(1);
    auto x = render(*d, 0.12);
    // peak of each burst window: bursts land at 0, 12 and 26 ms with velocities 0.7 / 0.85 / 1.0
    auto peak = [&](double a, double b) { double m = 0; for (float v : slice(x, a, b)) m = std::max(m, double(std::abs(v))); return m; };
    CHECK(peak(0.0, 0.011) > 0.05);
    CHECK(peak(0.013, 0.025) > 0.05);
    CHECK(peak(0.027, 0.040) > 0.05);
    CHECK(energy(slice(x, 0.09, 0.12)) < 0.01 * energy(slice(x, 0.0, 0.05)));
}

TEST_CASE("drum: velocity scales the hit", "[device][drum]") {
    CHECK(harness::rms(hit(0, 0.3, 1.0f)) > 1.9 * harness::rms(hit(0, 0.3, 0.5f)));
}

// ---------------- sample pads (setSample slots 0-7) ----------------
namespace {
constexpr double kPi = 3.14159265358979323846;

SamplePtr sineBuf(double srcRate, double hz, double secs, bool stereo = false, double hzR = 0.0, float amp = 1.0f) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = float(srcRate);
    const size_t n = size_t(srcRate * secs);
    b->l.resize(n);
    for (size_t i = 0; i < n; ++i) b->l[i] = amp * float(std::sin(2.0 * kPi * hz * double(i) / srcRate));
    if (stereo) {
        b->r.resize(n);
        for (size_t i = 0; i < n; ++i) b->r[i] = amp * float(std::sin(2.0 * kPi * hzR * double(i) / srcRate));
    }
    return b;
}
SamplePtr constBuf(double srcRate, double secs, float v = 0.5f) {
    auto b = std::make_shared<SampleBuf>();
    b->sampleRate = float(srcRate);
    b->l.assign(size_t(srcRate * secs), v);
    return b;
}
double peakOf(const std::vector<float>& x) { double m = 0; for (float v : x) m = std::max(m, double(std::abs(v))); return m; }
double freqOf(const std::vector<float>& x, double sr) {
    std::vector<double> t;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) t.push_back(double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]));
    if (t.size() < 3) return 0.0;
    return double(t.size() - 1) * sr / (t.back() - t.front());
}
struct Out2 { std::vector<float> l, r; };
Out2 renderLR(InstrumentDevice& d, double seconds, double sr = kSr) {
    Out2 o;
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    const int blocks = int(seconds * sr / kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(sr), {});
        o.l.insert(o.l.end(), l.begin(), l.end());
        o.r.insert(o.r.end(), r.begin(), r.end());
    }
    return o;
}
}  // namespace

TEST_CASE("drum: a sample pad plays the sample instead of the synthesized sound", "[device][drum][sample]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setSample(1, sineBuf(22050.0, 440.0, 0.5));           // snare pad; synth would be noise
    d->noteOn(1, 1.0f, 1);
    auto x = render(*d, 1.0);
    CHECK(peakOf(slice(x, 0.01, 0.4)) == Catch::Approx(1.0).margin(0.03));
    CHECK(freqOf(slice(x, 0.01, 0.45), kSr) == Catch::Approx(440.0).epsilon(0.005));
    // one-shot: the 0.5 s sample (real time, whatever its rate) ends and the pad falls silent
    CHECK(peakOf(slice(x, 0.52, 1.0)) == 0.0);
    // decay is a no-op: the snare's 0.18 s default decay does not gate the sample
    CHECK(harness::rms(slice(x, 0.3, 0.45)) > 0.5 * harness::rms(slice(x, 0.05, 0.2)));
    // `decay` param changes nothing
    auto e = make(); e->prepare(kSr, kMaxBlock);
    e->setSample(1, sineBuf(22050.0, 440.0, 0.5));
    set(*e, "p1_decay", 0.03f);
    e->noteOn(1, 1.0f, 1);
    CHECK(render(*e, 1.0) == x);
}

TEST_CASE("drum: sample pad sample-rate handling (22.05 kHz and 48 kHz sources, 44.1 and 48 kHz engines)", "[device][drum][sample]") {
    for (double engine : {44100.0, 48000.0}) {
        for (double src : {22050.0, 48000.0}) {
            INFO("engine " << engine << " source " << src);
            auto d = make(); d->prepare(engine, kMaxBlock);
            d->setSample(4, sineBuf(src, 300.0, 0.5));
            d->noteOn(4, 1.0f, 1);
            auto x = renderLR(*d, 1.0, engine).l;
            auto sl = [&](double a, double b) { return std::vector<float>(x.begin() + long(a * engine), x.begin() + std::min(long(b * engine), long(x.size()))); };  // whole blocks only: clamp to what was rendered
            CHECK(freqOf(sl(0.01, 0.45), engine) == Catch::Approx(300.0).epsilon(0.005));
            CHECK(peakOf(sl(0.44, 0.49)) > 0.5);
            CHECK(peakOf(sl(0.52, 1.0)) == 0.0);
        }
    }
}

TEST_CASE("drum: sample pad tune repitches by 2^(tune/12), level is the pad gain, velocity scales", "[device][drum][sample]") {
    auto play = [](float tune, float levelDb, float vel) {
        auto d = make(); d->prepare(kSr, kMaxBlock);
        d->setSample(0, sineBuf(kSr, 200.0, 1.0));
        set(*d, "p0_tune", tune); set(*d, "p0_level", levelDb);
        render(*d, 0.1);                                      // settle the smoothers
        d->noteOn(0, vel, 1);
        return render(*d, 1.2);
    };
    auto base = play(0.0f, 0.0f, 1.0f);
    CHECK(freqOf(slice(base, 0.01, 0.9), kSr) == Catch::Approx(200.0).epsilon(0.005));
    auto up = play(12.0f, 0.0f, 1.0f);
    CHECK(freqOf(slice(up, 0.01, 0.45), kSr) == Catch::Approx(400.0).epsilon(0.005));
    CHECK(peakOf(slice(up, 0.55, 1.2)) == 0.0);               // twice the rate: the 1 s sample ends at 0.5 s
    auto down = play(-12.0f, 0.0f, 1.0f);
    CHECK(freqOf(slice(down, 0.01, 1.0), kSr) == Catch::Approx(100.0).epsilon(0.005));
    CHECK(peakOf(slice(down, 1.0, 1.2)) > 0.5);               // half the rate: still sounding past 1 s
    CHECK(peakOf(play(0.0f, 6.0f, 1.0f)) / peakOf(base) == Catch::Approx(std::pow(10.0, 6.0 / 20.0)).epsilon(0.02));
    CHECK(peakOf(play(0.0f, -24.0f, 1.0f)) / peakOf(base) == Catch::Approx(std::pow(10.0, -24.0 / 20.0)).epsilon(0.02));
    CHECK(peakOf(play(0.0f, 0.0f, 0.5f)) / peakOf(base) == Catch::Approx(0.5).epsilon(0.02));
    CHECK(peakOf(base) == Catch::Approx(1.0).margin(0.02));   // unity at 0 dB, full velocity
}

TEST_CASE("drum: sample pad has a 1 ms attack, a retrigger restarts the sample", "[device][drum][sample]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setSample(2, constBuf(kSr, 1.0, 0.8f));
    d->noteOn(2, 1.0f, 1);
    auto x = render(*d, 0.3);
    CHECK(x[0] < 0.1);                                        // ramping in, not a click
    CHECK(x[size_t(0.005 * kSr)] == Catch::Approx(0.8).margin(0.01));
    // retrigger mid-sample: the sample restarts, so it ends one trigger-offset later, not at 1 s
    auto r = make(); r->prepare(kSr, kMaxBlock);
    r->setSample(2, constBuf(kSr, 0.5, 0.8f));
    r->noteOn(2, 1.0f, 1);
    render(*r, 0.3);
    r->noteOn(2, 1.0f, 2);
    auto y = render(*r, 0.6);
    CHECK(peakOf(slice(y, 0.4, 0.49)) > 0.5);                 // would be over at 0.5 s without the restart
    CHECK(peakOf(slice(y, 0.52, 0.6)) == 0.0);
    r->noteOff(2);                                            // noteOff stays a no-op
}

TEST_CASE("drum: pitch selects the sample pad modulo 8; other pads keep synthesizing", "[device][drum][sample]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setSample(1, sineBuf(kSr, 440.0, 0.5));
    for (uint8_t pitch : {uint8_t(1), uint8_t(9), uint8_t(25), uint8_t(121)}) {
        d->reset();
        d->noteOn(pitch, 1.0f, 1);
        auto x = render(*d, 0.2);
        INFO("pitch " << int(pitch));
        CHECK(freqOf(slice(x, 0.01, 0.2), kSr) == Catch::Approx(440.0).epsilon(0.005));
    }
    // the kick (pad 0) is still the membrane synth: identical to a drum with no sample at all
    auto ref = make(); ref->prepare(kSr, kMaxBlock);
    d->reset();
    d->noteOn(0, 1.0f, 1);
    ref->noteOn(0, 1.0f, 1);
    CHECK(render(*d, 0.5) == render(*ref, 0.5));
    // a sample pad and a synth pad sound together
    auto both = make(); both->prepare(kSr, kMaxBlock);
    both->setSample(1, constBuf(kSr, 1.0, 0.5f));
    both->noteOn(1, 1.0f, 1);
    both->noteOn(3, 1.0f, 2);
    auto solo = make(); solo->prepare(kSr, kMaxBlock);
    solo->setSample(1, constBuf(kSr, 1.0, 0.5f));
    solo->noteOn(1, 1.0f, 1);
    CHECK(render(*both, 0.2) != render(*solo, 0.2));
}

TEST_CASE("drum: clearing a sample restores the synth, null and bad slots are harmless", "[device][drum][sample]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setSample(1, sineBuf(kSr, 440.0, 0.5));
    d->setSample(1, nullptr);
    d->setSample(8, sineBuf(kSr, 440.0, 0.5));                // out of range: ignored
    d->setSample(99, nullptr);
    d->setSample(3, std::make_shared<SampleBuf>());           // empty buffer: silent, no crash
    d->noteOn(3, 1.0f, 1);
    CHECK(peakOf(render(*d, 0.2)) == 0.0);
    auto ref = make(); ref->prepare(kSr, kMaxBlock);
    d->reset(); d->noteOn(1, 1.0f, 2); ref->noteOn(1, 1.0f, 2);
    CHECK(render(*d, 0.3) == render(*ref, 0.3));              // the synthesized snare is back, bit for bit
    // setSample silences a ringing pad so nothing reads across the swap
    d->setSample(0, sineBuf(kSr, 440.0, 2.0));
    d->noteOn(0, 1.0f, 3);
    render(*d, 0.05);
    d->setSample(0, sineBuf(kSr, 220.0, 2.0));
    CHECK(peakOf(render(*d, 0.2)) == 0.0);
}

TEST_CASE("drum: a stereo sample keeps its image, a mono sample plays on both sides", "[device][drum][sample]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    d->setSample(5, sineBuf(kSr, 150.0, 1.0, true, 300.0));
    d->setSample(6, sineBuf(kSr, 150.0, 1.0));
    d->noteOn(5, 1.0f, 1);
    auto s = renderLR(*d, 0.5);
    CHECK(freqOf(slice(s.l, 0.01, 0.5), kSr) == Catch::Approx(150.0).epsilon(0.005));
    CHECK(freqOf(slice(s.r, 0.01, 0.5), kSr) == Catch::Approx(300.0).epsilon(0.005));
    d->reset();
    d->noteOn(6, 1.0f, 2);
    auto m = renderLR(*d, 0.3);
    CHECK(m.l == m.r);
    CHECK(peakOf(m.l) > 0.5);
}

TEST_CASE("drum: sample pads do not allocate on the audio path", "[device][drum][sample]") {
    auto d = make(); d->prepare(kSr, kMaxBlock);
    for (uint32_t s = 0; s < 8; s += 2) d->setSample(s, sineBuf(22050.0, 200.0 + 50.0 * s, 0.4, true, 300.0));
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    test::AllocGuard guard;
    for (int b = 0; b < 300; ++b) {
        if (b % 6 == 0) d->noteOn(uint8_t(b % 11), 0.9f, uint32_t(b + 1));
        if (b % 6 == 3) d->noteOff(uint32_t(b - 2));
        if (b % 17 == 0) { set(*d, "p0_tune", float(b % 12)); set(*d, "p2_level", -3.0f); }
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
    }
    d->reset();
    CHECK(guard.count() == 0);
}

TEST_CASE("drum: device contract with sample pads loaded", "[device][drum][sample]") {
    checkInstrumentContract([] {
        auto d = make();
        for (uint32_t s = 0; s < 8; ++s) if (s % 2 == 0) d->setSample(s, sineBuf(22050.0, 180.0 + 20.0 * s, 0.7, s == 4, 90.0));
        return d;
    });
}
