#pragma once
// Generic gates every device must pass (PLAN 6): well-formed ParamSpec table, finite and bounded
// output across random parameter sweeps, no allocation on the audio path, reset determinism.
// A device test calls these with its factory, then adds its own analytic checks (pitch, corner
// frequency, gain reduction, ...). Owned by the orchestrator; device tasks only include it.
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../AllocGuard.h"
#include "core/Constants.h"
#include "core/Device.h"

namespace ddaw::testkit {

constexpr double kSr = 44100.0;

inline ProcessContext ctx(double sr = kSr) { return {sr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4}; }

inline void checkParamTable(std::span<const ParamSpec> ps) {
    std::set<std::string> keys;
    for (size_t i = 0; i < ps.size(); ++i) {
        INFO("param " << ps[i].key);
        CHECK(ps[i].index == i);
        CHECK(ps[i].key != nullptr);
        CHECK(keys.insert(ps[i].key).second);                 // unique keys
        CHECK(ps[i].min <= ps[i].def);
        CHECK(ps[i].def <= ps[i].max);
        if (ps[i].curve == Curve::Stepped) CHECK(ps[i].smoothingMs == 0.0f);
        if (ps[i].curve == Curve::Exponential) CHECK(ps[i].min > 0.0f);
    }
}

inline bool finiteAndBounded(const std::vector<float>& v, float bound = 100.0f) {
    for (float x : v) if (!std::isfinite(x) || std::abs(x) > bound) return false;
    return true;
}

inline float randIn(std::mt19937& g, const ParamSpec& p) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    float v = p.min + u(g) * (p.max - p.min);
    if (p.curve == Curve::Stepped) v = std::round(v);
    return v;
}

// ---------------- effects ----------------

inline std::vector<float> noiseBlock(size_t n, unsigned seed, float amp = 0.3f) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> x(n);
    for (auto& v : x) v = amp * u(g);
    return x;
}

// Run `blocks` of `kMaxBlock` frames of a 220 Hz sine + noise through the device.
inline std::pair<std::vector<float>, std::vector<float>>
runEffect(EffectDevice& d, int blocks, double sr = kSr, float amp = 0.5f) {
    std::vector<float> outL, outR, l(kMaxBlock), r(kMaxBlock);
    size_t t = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < kMaxBlock; ++i, ++t) {
            l[size_t(i)] = amp * float(std::sin(2.0 * 3.14159265358979 * 220.0 * double(t) / sr)) + 0.05f * float(std::sin(0.37 * double(t)));
            r[size_t(i)] = 0.8f * l[size_t(i)];
        }
        d.process(l.data(), r.data(), kMaxBlock, ctx(sr), {});
        outL.insert(outL.end(), l.begin(), l.end());
        outR.insert(outR.end(), r.begin(), r.end());
    }
    return {outL, outR};
}

inline void checkEffectContract(const std::function<std::unique_ptr<EffectDevice>()>& make) {
    auto d = make();
    REQUIRE(d != nullptr);
    checkParamTable(d->params());
    d->prepare(kSr, kMaxBlock);

    SECTION("defaults: finite, bounded output") {
        auto [l, r] = runEffect(*d, 40);
        CHECK(finiteAndBounded(l));
        CHECK(finiteAndBounded(r));
    }
    SECTION("random parameter sweeps stay finite and bounded") {
        std::mt19937 g(1234);
        std::vector<float> l(kMaxBlock), r(kMaxBlock);
        for (int b = 0; b < 300; ++b) {
            for (auto& p : d->params()) d->setParam(p.index, randIn(g, p));
            for (int i = 0; i < kMaxBlock; ++i) l[size_t(i)] = r[size_t(i)] = 0.5f * float(std::sin(0.05 * double(b * kMaxBlock + i)));
            d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
            REQUIRE(finiteAndBounded(l));
            REQUIRE(finiteAndBounded(r));
        }
    }
    SECTION("no allocation on the audio path") {
        std::mt19937 g(99);
        std::vector<float> l(kMaxBlock, 0.3f), r(kMaxBlock, 0.3f);
        test::AllocGuard guard;
        for (int b = 0; b < 200; ++b) {
            if (b % 10 == 0) for (auto& p : d->params()) d->setParam(p.index, randIn(g, p));
            d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        }
        d->reset();
        CHECK(guard.count() == 0);
    }
    SECTION("reset makes the output repeatable") {
        auto first = runEffect(*d, 20);
        d->reset();
        auto second = runEffect(*d, 20);
        CHECK(first.first == second.first);
    }
    SECTION("a silent input stays finite (no denormal blow-ups or NaN)") {
        std::vector<float> l(kMaxBlock, 0.0f), r(kMaxBlock, 0.0f);
        for (int b = 0; b < 100; ++b) { d->process(l.data(), r.data(), kMaxBlock, ctx(), {}); REQUIRE(finiteAndBounded(l)); }
    }
}

// ---------------- instruments ----------------

// Play a short phrase through the device: overlapping notes, retriggers, off-by-id, an unknown id.
inline std::pair<std::vector<float>, std::vector<float>>
runPhrase(InstrumentDevice& d, int blocks, double sr = kSr) {
    std::vector<float> outL, outR, l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        if (b == 0) d.noteOn(60, 0.9f, 1);
        if (b == 10) d.noteOn(64, 0.7f, 2);          // overlaps note 1
        if (b == 20) d.noteOff(1);
        if (b == 30) d.noteOff(2);
        if (b == 35) d.noteOff(999);                  // unknown id: must be harmless
        if (b == 40) { d.noteOn(48, 1.0f, 3); d.noteOn(48, 0.5f, 4); }   // same pitch twice
        if (b == 60) { d.noteOff(3); d.noteOff(4); }
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(sr), {});
        outL.insert(outL.end(), l.begin(), l.end());
        outR.insert(outR.end(), r.begin(), r.end());
    }
    return {outL, outR};
}

inline void checkInstrumentContract(const std::function<std::unique_ptr<InstrumentDevice>()>& make) {
    auto d = make();
    REQUIRE(d != nullptr);
    checkParamTable(d->params());
    d->prepare(kSr, kMaxBlock);

    SECTION("a phrase is audible, finite and bounded") {
        auto [l, r] = runPhrase(*d, 120);
        CHECK(finiteAndBounded(l));
        double e = 0; for (float x : l) e += double(x) * x;
        CHECK(e > 1e-6);
    }
    SECTION("silent before any note, and additive (mixes into the buffer)") {
        std::vector<float> l(kMaxBlock, 0.25f), r(kMaxBlock, -0.25f);
        d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        for (float x : l) REQUIRE(x == 0.25f);
        for (float x : r) REQUIRE(x == -0.25f);
    }
    SECTION("random parameter sweeps while playing stay finite and bounded") {
        std::mt19937 g(4321);
        std::vector<float> l(kMaxBlock), r(kMaxBlock);
        for (int b = 0; b < 300; ++b) {
            if (b % 25 == 0) d->noteOn(uint8_t(40 + (b / 25) * 3), 0.8f, uint32_t(b + 1));
            if (b % 25 == 12) d->noteOff(uint32_t(b + 1 - 12));
            for (auto& p : d->params()) d->setParam(p.index, randIn(g, p));
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
            REQUIRE(finiteAndBounded(l));
            REQUIRE(finiteAndBounded(r));
        }
    }
    SECTION("no allocation on the audio path (noteOn, noteOff, setParam, process, reset)") {
        std::mt19937 g(7);
        std::vector<float> l(kMaxBlock), r(kMaxBlock);
        test::AllocGuard guard;
        for (int b = 0; b < 200; ++b) {
            if (b % 20 == 0) d->noteOn(uint8_t(36 + b % 30), 0.9f, uint32_t(b + 1));
            if (b % 20 == 10) d->noteOff(uint32_t(b - 9));
            if (b % 15 == 0) for (auto& p : d->params()) d->setParam(p.index, randIn(g, p));
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            d->process(l.data(), r.data(), kMaxBlock, ctx(), {});
        }
        d->reset();
        CHECK(guard.count() == 0);
    }
    SECTION("reset makes the phrase repeatable") {
        auto first = runPhrase(*d, 80);
        d->reset();
        auto second = runPhrase(*d, 80);
        CHECK(first.first == second.first);
    }
}

}  // namespace ddaw::testkit
