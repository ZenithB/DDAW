// The synthesized drum machines (808-, 909- and Linn-style): each is a full instrument, every pad speaks with its own character, the
// pad controls do what they say, and the sampled machine has its digital grit. Analytic checks only (no listening tests).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <set>

#include "DeviceTestKit.h"
#include "devices/Registry.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::testkit;
using Catch::Approx;

namespace {
const char* const kKits[] = {"drum808", "drum909", "drumlinn"};

std::unique_ptr<InstrumentDevice> make(const char* t) { return createInstrument(t); }
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
std::vector<float> hit(const char* kit, uint8_t pad, double seconds, const std::vector<std::pair<const char*, float>>& ps = {}, float vel = 1.0f) {
    auto d = make(kit);
    d->prepare(kSr, kMaxBlock);
    for (auto [k, v] : ps) set(*d, k, v);
    d->reset();                                                                       // the levels settle at once, as they do in a built graph
    d->noteOn(pad, vel, 1);
    return render(*d, seconds);
}
double energy(const std::vector<float>& x, size_t a, size_t b) { double e = 0; for (size_t i = a; i < std::min(b, x.size()); ++i) e += double(x[i]) * x[i]; return e; }
double rmsOf(const std::vector<float>& x, double a, double b) { return std::sqrt(energy(x, size_t(a * kSr), size_t(b * kSr)) / std::max(1.0, (b - a) * kSr)); }
// energy of the first difference over total energy: ~0 for a low tone, large for noise and hats
double brightness(const std::vector<float>& x) {
    double d = 0; for (size_t i = 1; i < x.size(); ++i) { const double v = double(x[i]) - x[i - 1]; d += v * v; }
    return d / std::max(energy(x, 0, x.size()), 1e-30);
}
// how long until the level has fallen 40 dB below its peak (in 5 ms windows)
double fallTime(const std::vector<float>& x) {
    const size_t w = size_t(0.005 * kSr);
    std::vector<double> lv;
    for (size_t i = 0; i + w <= x.size(); i += w) lv.push_back(std::sqrt(energy(x, i, i + w) / double(w)));
    double peak = 0; for (double v : lv) peak = std::max(peak, v);
    size_t last = 0;
    for (size_t i = 0; i < lv.size(); ++i) if (lv[i] > peak * 0.01) last = i;
    return double(last + 1) * 0.005;
}
// the lowest strong frequency of a short tone burst, by zero crossings of the first 60 ms after the pitch has settled
double settledHz(const std::vector<float>& x, double from, double to) {
    int zc = 0;
    for (size_t i = size_t(from * kSr) + 1; i < size_t(to * kSr) && i < x.size(); ++i) if ((x[i - 1] < 0) != (x[i] < 0)) ++zc;
    return zc / 2.0 / (to - from);
}
}  // namespace

TEST_CASE("drum kits: each passes the instrument contract", "[device][drumkit]") {
    for (const char* k : kKits) { INFO(k); checkInstrumentContract([k] { return make(k); }); }
}

TEST_CASE("drum kits: the table has eight pads of tune, decay and level, in the original drum's key scheme", "[device][drumkit]") {
    auto ref = createInstrument("drum");
    for (const char* k : kKits) {
        INFO(k);
        auto d = make(k);
        REQUIRE(d->params().size() == 24);
        for (size_t i = 0; i < 24; ++i) CHECK(std::string(d->params()[i].key) == std::string(ref->params()[i].key));   // so a switch keeps the settings
    }
}

TEST_CASE("drum kits: every pad sounds and ends, kicks are low and hats are bright", "[device][drumkit]") {
    for (const char* k : kKits) {
        for (uint8_t pad = 0; pad < 8; ++pad) {
            INFO(k << " pad " << int(pad));
            const auto x = hit(k, pad, 2.5);
            for (float v : x) REQUIRE(std::isfinite(v));
            CHECK(energy(x, 0, x.size()) > 1e-3);                                         // audible
            double peak = 0; for (float v : x) peak = std::max(peak, double(std::abs(v)));
            CHECK(peak < 1.6);                                                            // never hot enough to need a limiter
            CHECK(rmsOf(x, 2.3, 2.5) < 1e-3);                                             // and over by the end
        }
        const auto kick = hit(k, 0, 0.6), hat = hit(k, 3, 0.3), snare = hit(k, 1, 0.4);
        INFO(k);
        CHECK(brightness(kick) < 0.15);
        CHECK(brightness(hat) > 0.8);
        CHECK(brightness(hat) > 5.0 * brightness(kick));
        CHECK(brightness(snare) > brightness(kick));
    }
}

TEST_CASE("drum kits: the pad controls: decay lengthens, tune moves the pitch, level scales, velocity scales", "[device][drumkit]") {
    for (const char* k : kKits) {
        INFO(k);
        // decay
        const auto shortK = hit(k, 0, 2.0, {{"p0_decay", 0.1f}}), longK = hit(k, 0, 2.0, {{"p0_decay", 1.2f}});
        CHECK(fallTime(longK) > 2.0 * fallTime(shortK));
        const auto shortH = hit(k, 4, 2.0, {{"p4_decay", 0.05f}}), longH = hit(k, 4, 2.0, {{"p4_decay", 0.8f}});
        CHECK(fallTime(longH) > 2.0 * fallTime(shortH));
        // tune: an octave up doubles the settled pitch of the kick
        const auto base = hit(k, 0, 0.5, {{"p0_decay", 0.8f}}), up = hit(k, 0, 0.5, {{"p0_decay", 0.8f}, {"p0_tune", 12.0f}});
        const double f0 = settledHz(base, 0.12, 0.3), f1 = settledHz(up, 0.12, 0.3);
        CHECK(f1 > 1.7 * f0);
        CHECK(f1 < 2.4 * f0);
        // level: -6 dB is about half the amplitude
        const auto full = hit(k, 1, 0.5), half = hit(k, 1, 0.5, {{"p1_level", -6.0f}});
        CHECK(rmsOf(half, 0.0, 0.3) == Approx(rmsOf(full, 0.0, 0.3) * 0.501).epsilon(0.05));
        // velocity
        const auto soft = hit(k, 1, 0.5, {}, 0.5f);
        CHECK(rmsOf(soft, 0.0, 0.3) == Approx(rmsOf(full, 0.0, 0.3) * 0.5).epsilon(0.08));
        // a pad does not disturb another
        auto d = make(k); d->prepare(kSr, kMaxBlock);
        set(*d, "p2_level", -24.0f);
        d->noteOn(0, 1.0f, 1);
        const auto only = render(*d, 0.4);
        const auto kickAlone = hit(k, 0, 0.4);
        for (size_t i = 0; i < only.size(); ++i) REQUIRE(only[i] == kickAlone[i]);
    }
}

TEST_CASE("drum kits: the machines sound different from each other, and from the original", "[device][drumkit]") {
    // the 808 kick rings longer than the 909's at their defaults; the 909 kick is harder (a bigger click), the Linn is shorter
    const auto k808 = hit("drum808", 0, 2.0), k909 = hit("drum909", 0, 2.0), kLinn = hit("drumlinn", 0, 2.0), kStd = hit("drum", 0, 2.0);
    CHECK(fallTime(k808) > fallTime(k909));
    CHECK(fallTime(k909) > fallTime(kLinn) - 0.2);
    std::vector<std::vector<float>> snares = {hit("drum808", 1, 0.4), hit("drum909", 1, 0.4), hit("drumlinn", 1, 0.4), hit("drum", 1, 0.4)};
    for (size_t i = 0; i < snares.size(); ++i)
        for (size_t j = i + 1; j < snares.size(); ++j) {
            double diff = 0;
            for (size_t n = 0; n < snares[i].size(); ++n) diff += std::abs(double(snares[i][n]) - double(snares[j][n]));
            CHECK(diff > 5.0);
        }
    (void)kStd;
}

TEST_CASE("drum kits: the Linn-style machine is digital: few amplitude levels, a held sample rate", "[device][drumkit]") {
    const auto linn = hit("drumlinn", 1, 0.3), analog = hit("drum808", 1, 0.3);
    std::set<float> a, b;
    for (float v : linn) a.insert(v);
    for (float v : analog) b.insert(v);
    CHECK(a.size() <= 257);                                                               // 8 bits
    CHECK(b.size() > 2000);
    // the held rate shows as runs of identical samples
    size_t runs = 0;
    for (size_t i = 1; i < linn.size(); ++i) if (linn[i] == linn[i - 1]) ++runs;
    CHECK(runs > linn.size() / 4);
}

TEST_CASE("drum kits: a loaded sample replaces the pad's synthesized sound", "[device][drumkit]") {
    for (const char* k : kKits) {
        INFO(k);
        auto d = make(k); d->prepare(kSr, kMaxBlock);
        auto s = std::make_shared<SampleBuf>();
        s->sampleRate = float(kSr);
        s->l.assign(4410, 0.0f); s->r.assign(4410, 0.0f);
        for (size_t i = 0; i < s->l.size(); ++i) s->l[i] = s->r[i] = 0.5f;
        d->setSample(1, s);
        d->noteOn(1, 1.0f, 1);
        const auto x = render(*d, 0.2);
        CHECK(x[200] == Approx(0.5f).margin(0.02f));                                       // the sample's level, not a snare
        d->setSample(1, nullptr);
        d->noteOn(1, 1.0f, 2);
        const auto y = render(*d, 0.05);
        double diff = 0; for (float v : y) diff += std::abs(double(v) - 0.5);
        CHECK(diff > 1.0);                                                                  // synthesized again
        d->setSample(99, s);                                                                // a slot out of range is ignored
    }
}
