// Per-device CPU cost: every built-in effect and instrument at its default parameters, 128-frame chunks (the engine's
// grid), 48 kHz. Instruments play a six-note chord. Prints mean / p99 / worst microseconds per chunk against the 2667 us
// a chunk lasts. For finding the devices that dominate the audio thread's worst case (see ddaw_swapbench, ddaw_soak).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/Constants.h"
#include "devices/Registry.h"

using namespace ddaw;
using Clock = std::chrono::steady_clock;

namespace {
struct Result { std::string name; double mean, p99, worst; };

template <class F> Result measure(const std::string& name, F&& chunk) {
    std::vector<double> us;
    for (int i = 0; i < 400; ++i) chunk(i);   // warm up
    for (int i = 0; i < 3000; ++i) {
        const auto t0 = Clock::now();
        chunk(i);
        us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    }
    std::sort(us.begin(), us.end());
    double sum = 0; for (double v : us) sum += v;
    return {name, sum / double(us.size()), us[size_t(double(us.size()) * 0.99)], us.back()};
}
}  // namespace

int main() {
    const double sr = 48000.0;
    const ProcessContext ctx{sr, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    std::vector<Result> res;
    const char* insts[] = {"drum", "duo", "fm", "fmop", "follow", "granular", "harmnoise", "keys", "ksampler", "modal", "mono", "perc", "pluck", "poly", "sampler", "subtractive", "waveshaper", "wavetable"};
    const char* fxs[] = {"autofilt", "autopan", "autotune", "cheby", "chorus", "comp", "crush", "delay", "dist", "duck", "eq", "eq7", "filter", "gate",
                         "mbcomp", "opto", "phaser", "pingpong", "plate", "reverb", "shift", "trem", "vib", "widen"};
    for (const char* t : insts) {
        auto d = createInstrument(t);
        if (!d) continue;
        d->prepare(sr, kMaxBlock);
        static const uint8_t chord[] = {48, 55, 60, 64, 67, 72};
        uint32_t id = 0;
        for (uint8_t p : chord) d->noteOn(p, 0.8f, ++id);
        res.push_back(measure(std::string("inst ") + t, [&](int i) {
            if (i % 200 == 199) { for (uint32_t k = 1; k <= id; ++k) d->noteOff(k); id = 0; for (uint8_t p : chord) d->noteOn(p, 0.8f, ++id); }
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            d->process(l.data(), r.data(), kMaxBlock, ctx, {});
        }));
    }
    for (const char* t : fxs) {
        auto d = createEffect(t);
        if (!d) continue;
        d->prepare(sr, kMaxBlock);
        res.push_back(measure(std::string("fx   ") + t, [&](int i) {
            for (int k = 0; k < kMaxBlock; ++k) { const float x = 0.4f * std::sin(0.031f * float(i * kMaxBlock + k)) + 0.1f * std::sin(0.77f * float(k)); l[size_t(k)] = x; r[size_t(k)] = 0.9f * x; }
            d->process(l.data(), r.data(), kMaxBlock, ctx, {});
        }));
    }
    std::sort(res.begin(), res.end(), [](const Result& a, const Result& b) { return a.worst > b.worst; });
    std::printf("%-18s %9s %9s %9s   (a chunk lasts %.0f us)\n", "device", "mean us", "p99 us", "worst us", double(kMaxBlock) / sr * 1e6);
    for (auto& x : res) std::printf("%-18s %9.1f %9.1f %9.1f\n", x.name.c_str(), x.mean, x.p99, x.worst);
    return 0;
}
