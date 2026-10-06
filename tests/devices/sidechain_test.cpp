// The key input of the dynamics devices (comp, opto, mbcomp, gate): keyed by their own signal they behave exactly as unkeyed
// (bit for bit), a silent key means no detection, a loud key acts on a quiet signal, and the multiband compressor keys each band
// from the same band of the key.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;
using Catch::Approx;

namespace {

std::vector<float> tone(double hz, float amp, size_t n, double phase0 = 0.0) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = amp * float(std::sin(2.0 * std::numbers::pi * hz * double(i) / kSr + phase0));
    return x;
}

// Process `x` (copied, both channels) in blocks, with `key` (empty: none), and return the left output.
std::vector<float> run(EffectDevice& d, const std::vector<float>& x, const std::vector<float>& key = {}) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (size_t at = 0; at + kMaxBlock <= x.size(); at += kMaxBlock) {
        std::copy(x.begin() + long(at), x.begin() + long(at + kMaxBlock), l.begin());
        r = l;
        ModInputs mod;
        if (!key.empty()) { mod.keyL = key.data() + at; mod.keyR = key.data() + at; }
        d.process(l.data(), r.data(), kMaxBlock, ctx(), mod);
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}
double rms(const std::vector<float>& x, size_t from, size_t len) {
    double s = 0;
    for (size_t i = from; i < from + len && i < x.size(); ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(len));
}
std::unique_ptr<EffectDevice> make(const char* type) {
    auto d = createEffect(type);
    d->prepare(kSr, kMaxBlock);
    return d;
}
void set(EffectDevice& d, const char* key, float v) { d.setParam(uint16_t(findParam(d.params(), key)), v); }
}  // namespace

TEST_CASE("sidechain: every dynamics device says it takes a key, the others do not", "[sidechain][device]") {
    for (const char* t : {"comp", "opto", "mbcomp", "gate"}) { INFO(t); CHECK(createEffect(t)->keyable()); }
    for (const char* t : {"reverb", "delay", "eq", "filter", "chorus", "duck", "dist"}) { INFO(t); CHECK_FALSE(createEffect(t)->keyable()); }
}

TEST_CASE("sidechain: a device keyed by its own signal is bit-identical to the unkeyed device", "[sidechain][device]") {
    // a signal with loud and quiet parts, so the dynamics really move
    auto x = tone(220.0, 0.6f, 48000);
    const auto soft = tone(330.0, 0.004f, 48000, 1.0);
    for (size_t i = 12000; i < 30000; ++i) x[i] = soft[i];
    for (const char* t : {"comp", "opto", "mbcomp", "gate"}) {
        INFO(t);
        auto a = make(t), b = make(t);
        if (std::string(t) == "gate") for (auto* g : {a.get(), b.get()}) { set(*g, "thresh", -30.0f); set(*g, "hold", 0.0f); set(*g, "release", 0.02f); set(*g, "range", -60.0f); }
        a->reset(); b->reset();
        const auto plain = run(*a, x);
        const auto keyed = run(*b, x, x);
        REQUIRE(plain.size() == keyed.size());
        CHECK(plain == keyed);
        CHECK(rms(plain, 16000, 8000) != Approx(rms(x, 16000, 8000)).epsilon(0.02));        // the device did something to the quiet passage
    }
}

TEST_CASE("sidechain: a loud key compresses a quiet signal, a silent key leaves it alone", "[sidechain][device]") {
    const auto quiet = tone(220.0, 0.02f, 48000);                    // -34 dBFS: under the thresholds below
    const auto loud = tone(80.0, 0.9f, 48000);
    const std::vector<float> silent(48000, 0.0f);
    for (const char* t : {"comp", "opto"}) {
        INFO(t);
        auto d = make(t);
        if (std::string(t) == "comp") { set(*d, "thresh", -30.0f); set(*d, "ratio", 12.0f); set(*d, "attack", 0.003f); }
        else { set(*d, "reduction", 0.9f); set(*d, "mode", 1.0f); }
        d->reset();
        const auto own = run(*d, quiet);
        d->reset();
        const auto lowKey = run(*d, quiet, silent);
        d->reset();
        const auto hiKey = run(*d, quiet, loud);
        // silent key: no reduction (the signal comes out at least as loud as the free-running one, which compresses itself a little)
        CHECK(rms(lowKey, 20000, 8000) >= 0.95 * rms(own, 20000, 8000));
        // loud key: well below
        CHECK(rms(hiKey, 20000, 8000) < 0.5 * rms(lowKey, 20000, 8000));
    }
}

TEST_CASE("sidechain: the gate opens on its key, not on its own signal", "[sidechain][device]") {
    const auto sig = tone(220.0, 0.5f, 48000);                       // well above the threshold on its own
    const std::vector<float> silent(48000, 0.0f);
    const auto key = tone(1000.0, 0.5f, 48000);
    auto d = make("gate");
    set(*d, "thresh", -30.0f); set(*d, "range", -60.0f); set(*d, "attack", 0.002f); set(*d, "release", 0.02f); set(*d, "hold", 0.0f);
    d->reset();
    const auto own = run(*d, sig);
    d->reset();
    const auto shut = run(*d, sig, silent);
    d->reset();
    const auto open = run(*d, sig, key);
    CHECK(rms(own, 20000, 8000) > 0.3);
    CHECK(rms(shut, 20000, 8000) < 0.01 * rms(own, 20000, 8000));
    CHECK(rms(open, 20000, 8000) == Approx(rms(own, 20000, 8000)).epsilon(0.1));
    // listen: the output is the key (filtered by the detector's band), not the signal
    set(*d, "listen", 1.0f); d->reset();
    const auto heard = run(*d, sig, key);
    CHECK(rms(heard, 20000, 8000) > 0.2);
    CHECK(rms(heard, 20000, 8000) < 0.8);
}

TEST_CASE("sidechain: the multiband compressor reduces the bands its key excites", "[sidechain][device]") {
    // the signal has a low tone (100 Hz) and a high tone (4 kHz) of equal, quiet level; the key is loud at 100 Hz only
    auto sig = tone(100.0, 0.02f, 48000);
    const auto hi = tone(4000.0, 0.02f, 48000);
    for (size_t i = 0; i < sig.size(); ++i) sig[i] += hi[i];
    const auto key = tone(100.0, 0.9f, 48000);
    auto d = make("mbcomp");
    for (const char* k : {"thresh1", "thresh2", "thresh3"}) if (findParam(d->params(), k) >= 0) set(*d, k, -30.0f);
    for (const char* k : {"ratio1", "ratio2", "ratio3"}) if (findParam(d->params(), k) >= 0) set(*d, k, 12.0f);
    d->reset();
    const auto own = run(*d, sig);
    d->reset();
    const auto keyed = run(*d, sig, key);
    auto band = [](const std::vector<float>& x, double hz) {
        double re = 0, im = 0;
        for (size_t i = 20000; i < 28192; ++i) { const double ph = 2.0 * std::numbers::pi * hz * double(i) / kSr; re += x[i] * std::cos(ph); im -= x[i] * std::sin(ph); }
        return 2.0 * std::hypot(re, im) / 8192.0;
    };
    CHECK(band(keyed, 100.0) < 0.5 * band(own, 100.0));              // the key's band is pulled down
    CHECK(band(keyed, 4000.0) > 0.8 * band(own, 4000.0));            // the others are not
}
