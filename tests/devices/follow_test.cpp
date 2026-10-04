#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "DeviceTestKit.h"
#include "devices/Registry.h"

using namespace ddaw;
using namespace ddaw::testkit;
using Catch::Approx;

namespace {
std::unique_ptr<InstrumentDevice> make() { return createInstrument("follow"); }

std::vector<float> render(InstrumentDevice& d, int blocks) {
    std::vector<float> out, l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), {});
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}
double freqOf(const std::vector<float>& x, double sr) {
    std::vector<double> t;
    for (size_t i = 1; i < x.size(); ++i) if (x[i - 1] < 0.0f && x[i] >= 0.0f) t.push_back(double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]));
    return t.size() < 3 ? 0.0 : double(t.size() - 1) * sr / (t.back() - t.front());
}
double rms(const std::vector<float>& x, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b; ++i) s += double(x[i]) * x[i]; return std::sqrt(s / double(b - a)); }
}  // namespace

TEST_CASE("follow: device contract", "[device][follow]") { checkInstrumentContract(make); }

TEST_CASE("follow: sings the tracked pitch and follows the envelope", "[device][follow]") {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    d->setParam(uint16_t(findParam(d->params(), "wave")), 3.0f);   // sine: easy to measure
    PerformanceFrame f{330.0f, 0.95f, -20.0f, 0.4f};
    std::vector<float> all;
    for (int b = 0; b < 160; ++b) { d->performance(f); const auto o = render(*d, 1); all.insert(all.end(), o.begin(), o.end()); }
    const std::vector<float> tail(all.end() - 4096, all.end());
    CHECK(freqOf(tail, kSr) == Approx(330.0).epsilon(0.01));
    CHECK(rms(tail, 0, tail.size()) > 0.05);
    // unvoiced: the voice releases to silence (an exponential release: set a short one to measure it)
    d->setParam(uint16_t(findParam(d->params(), "release")), 20.0f);
    f.f0Hz = 0.0f;
    std::vector<float> after;
    for (int b = 0; b < 200; ++b) { d->performance(f); const auto o = render(*d, 1); after.insert(after.end(), o.begin(), o.end()); }
    CHECK(rms(after, after.size() - 2048, after.size()) < 1e-4);
    // below the gate: also silent
    f.f0Hz = 330.0f; f.loudnessDb = -75.0f;
    std::vector<float> gated;
    for (int b = 0; b < 200; ++b) { d->performance(f); const auto o = render(*d, 1); gated.insert(gated.end(), o.begin(), o.end()); }
    CHECK(rms(gated, gated.size() - 2048, gated.size()) < 1e-4);
}

TEST_CASE("follow: octave transposes, glide slides, notes play it when nothing is tracked", "[device][follow]") {
    auto d = make();
    d->prepare(kSr, kMaxBlock);
    d->setParam(uint16_t(findParam(d->params(), "wave")), 3.0f);
    d->setParam(uint16_t(findParam(d->params(), "octave")), 1.0f);
    PerformanceFrame f{220.0f, 0.95f, -20.0f, 0.4f};
    std::vector<float> all;
    for (int b = 0; b < 160; ++b) { d->performance(f); const auto o = render(*d, 1); all.insert(all.end(), o.begin(), o.end()); }
    CHECK(freqOf(std::vector<float>(all.end() - 4096, all.end()), kSr) == Approx(440.0).epsilon(0.01));
    // no frames for a while: a note takes over
    d->reset();
    d->noteOn(57, 0.8f, 1);   // A3 = 220 Hz, an octave up = 440
    const auto o = render(*d, 160);
    CHECK(freqOf(std::vector<float>(o.end() - 4096, o.end()), kSr) == Approx(440.0).epsilon(0.01));
}
