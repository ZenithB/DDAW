// ddaw_b1 {verify|bench} <instrument> [modelsDir]
//   verify: C++ decoder + harmonic synth vs the Python/TF reference (tools/b1/ddsp_ref.py).
//   bench : per-frame inference time and audio->inference->audio handoff latency.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

#include "core/SpscFifo.h"
#include "ddsp/Decoder.h"
#include "ddsp/HarmonicSynth.h"
#include "harness/Metrics.h"

using namespace ddaw;
using namespace ddaw::ddsp;
using Clock = std::chrono::steady_clock;

namespace {

std::vector<float> readBin(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<float> v(static_cast<size_t>(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * 4));
    return v;
}

double maxAbsDiff(const float* a, const float* b, size_t n) {
    double m = 0;
    for (size_t i = 0; i < n; ++i) m = std::max(m, std::abs(double(a[i]) - double(b[i])));
    return m;
}
double rmsDiff(const float* a, const float* b, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; ++i) { double d = double(a[i]) - double(b[i]); s += d * d; }
    return std::sqrt(s / double(n));
}

struct Inputs {
    std::vector<float> f0, ld, f0s, lds, raw, harm;
    size_t frames = 0;
};

Inputs loadRef(const std::string& dir) {
    Inputs in;
    in.f0 = readBin(dir + "/f0_hz.bin");     in.ld = readBin(dir + "/loudness_db.bin");
    in.f0s = readBin(dir + "/f0_scaled.bin"); in.lds = readBin(dir + "/ld_scaled.bin");
    in.raw = readBin(dir + "/decoder_raw.bin"); in.harm = readBin(dir + "/harmonic_signal.bin");
    in.frames = in.f0.size();
    return in;
}

// Synthesise a whole clip from per-frame raw controls (streaming, one frame of lookahead).
std::vector<float> synthClip(const std::vector<float>& raw, const std::vector<float>& f0) {
    const size_t n = f0.size();
    std::vector<HarmonicFrame> fr(n);
    for (size_t i = 0; i < n; ++i) makeHarmonicFrame(&raw[i * kDecoderOut], f0[i], kModelSampleRate, fr[i]);
    HarmonicSynth syn;
    syn.prepare(kModelSampleRate, kHopSamples);
    std::vector<float> out(n * kHopSamples);
    for (size_t f = 0; f < n; ++f) syn.renderInterval(fr[f], fr[std::min(f + 1, n - 1)], &out[f * kHopSamples]);
    return out;
}

int verify(const std::string& inst, const std::string& models) {
    Decoder dec;
    dec.load(models + "/export/" + inst + ".ddspw");
    Inputs in = loadRef(models + "/ref/" + inst);

    std::vector<float> f0s(in.frames), lds(in.frames);
    for (size_t i = 0; i < in.frames; ++i) { f0s[i] = scaleF0Hz(in.f0[i]); lds[i] = scaleLoudnessDb(in.ld[i]); }

    std::vector<float> raw(in.frames * kDecoderOut);
    dec.reset();
    for (size_t i = 0; i < in.frames; ++i) dec.step(lds[i], f0s[i], &raw[i * kDecoderOut]);

    auto group = [&](const char* name, int off, int len) {
        double mx = 0, ss = 0;
        for (size_t i = 0; i < in.frames; ++i) {
            const float* a = &raw[i * kDecoderOut + off]; const float* b = &in.raw[i * kDecoderOut + off];
            mx = std::max(mx, maxAbsDiff(a, b, size_t(len)));
            ss += rmsDiff(a, b, size_t(len)) * rmsDiff(a, b, size_t(len));
        }
        std::printf("  %-22s max|err| %.3e   rms err %.3e\n", name, mx, std::sqrt(ss / double(in.frames)));
        return mx;
    };
    std::printf("[%s] %zu frames\n", inst.c_str(), in.frames);
    std::printf("input scaling   : f0 max|err| %.2e, loudness max|err| %.2e\n",
                maxAbsDiff(f0s.data(), in.f0s.data(), in.frames), maxAbsDiff(lds.data(), in.lds.data(), in.frames));
    std::printf("decoder output vs TF (raw, pre-scale):\n");
    double e1 = group("amps", 0, kNumAmps), e2 = group("harmonic_distribution", kNumAmps, kNumHarmonics),
           e3 = group("noise_magnitudes", kNumAmps + kNumHarmonics, kNumNoise);
    const double decMax = std::max({e1, e2, e3});

    // Synth isolated: reference controls in, so only the C++ synth is under test.
    auto isolated = synthClip(in.raw, in.f0);
    // End to end: C++ decoder -> C++ synth.
    auto e2e = synthClip(raw, in.f0);
    {   // Dump the C++ synth output for tools/b1/harmonic_f64.py to compare against.
        const std::string od = models + "/out/" + inst;
        (void)std::system(("mkdir -p '" + od + "'").c_str());
        std::ofstream(od + "/harmonic_cpp.bin", std::ios::binary).write(reinterpret_cast<const char*>(isolated.data()), std::streamsize(isolated.size() * 4));
    }
    const double nullIso = harness::rmsNullDb(isolated, in.harm), nullE2e = harness::rmsNullDb(e2e, in.harm);
    std::printf("harmonic audio vs TF (16 kHz, %zu samples):\n", in.harm.size());
    std::printf("  synth only (TF controls)   rms null %.1f dB   max|err| %.2e\n", nullIso,
                maxAbsDiff(isolated.data(), in.harm.data(), in.harm.size()));
    std::printf("  decoder + synth (C++)      rms null %.1f dB   max|err| %.2e\n", nullE2e,
                maxAbsDiff(e2e.data(), in.harm.data(), in.harm.size()));
    std::printf("  reference level %.1f dBFS\n", harness::toDb(harness::rms(in.harm)));

    const bool ok = nullE2e <= -50.0 && decMax <= 5e-4;
    std::printf("%s (gates: end-to-end null vs TF <= -50 dB, decoder max|err| <= 5e-4; synth vs float64 spec is gated in harmonic_f64.py)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

struct Request { uint32_t id; float ld, f0; int64_t pushedNs; };
struct Result  { uint32_t id; int64_t pushedNs; int64_t doneNs; float raw0; };

int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

void percentiles(std::vector<double>& v, const char* label, const char* unit) {
    std::sort(v.begin(), v.end());
    auto p = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
    double mean = 0; for (double x : v) mean += x; mean /= double(v.size());
    std::printf("  %-26s mean %8.1f  p50 %8.1f  p99 %8.1f  max %8.1f %s\n", label, mean, p(0.5), p(0.99), v.back(), unit);
}

// QoS for the latency-critical threads (macOS). DDAW_QOS=0 disables it, to measure the difference.
void boostThread() {
#ifdef __APPLE__
    const char* e = std::getenv("DDAW_QOS");
    if (e && std::string(e) == "0") return;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

int bench(const std::string& inst, const std::string& models) {
    boostThread();
    Decoder dec;
    dec.load(models + "/export/" + inst + ".ddspw");
    Inputs in = loadRef(models + "/ref/" + inst);
    std::vector<float> out(kDecoderOut);

    // 1. Pure per-frame inference time, single thread.
    dec.reset();
    for (int i = 0; i < 250; ++i) dec.step(scaleLoudnessDb(in.ld[size_t(i)]), scaleF0Hz(in.f0[size_t(i)]), out.data());  // warm-up
    std::vector<double> us;
    for (int pass = 0; pass < 4; ++pass)
        for (size_t i = 0; i < in.frames; ++i) {
            const auto t0 = Clock::now();
            dec.step(scaleLoudnessDb(in.ld[i]), scaleF0Hz(in.f0[i]), out.data());
            us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        }
    std::printf("[%s] per-frame inference (budget 4000 us per frame at 250 fps)\n", inst.c_str());
    std::vector<double> sorted = us;
    percentiles(sorted, "step()", "us");
    const double mean = [&] { double s = 0; for (double x : us) s += x; return s / double(us.size()); }();
    std::printf("  real-time factor (mean/budget): %.3f  -> %.1f%% of one core\n", mean / 4000.0, 100.0 * mean / 4000.0);

    // 2. Handoff: "audio thread" posts a frame every 4 ms, inference thread wakes, decodes, posts the result.
    static SpscFifo<Request, 64> req;
    static SpscFifo<Result, 64> res;
    static std::atomic<uint32_t> bell{0};
    static std::atomic<bool> stop{false};
    const bool spin = std::getenv("DDAW_SPIN") && std::string(std::getenv("DDAW_SPIN")) == "1";
    std::printf("inference thread wait: %s\n", spin ? "spin (keeps the core hot)" : "blocking (futex wake)");
    dec.reset();
    std::thread worker([&] {
        boostThread();
        uint32_t seen = 0;
        std::vector<float> o(kDecoderOut);
        while (!stop.load(std::memory_order_acquire)) {
            if (spin) { while (bell.load(std::memory_order_acquire) == seen && !stop.load(std::memory_order_acquire)) {} }
            else      bell.wait(seen, std::memory_order_acquire);
            seen = bell.load(std::memory_order_acquire);
            Request r;
            while (req.pop(r)) {
                dec.step(r.ld, r.f0, o.data());
                res.push({r.id, r.pushedNs, nowNs(), o[0]});
            }
        }
    });
    constexpr int kFrames = 4000;  // 16 s at 250 fps
    std::vector<double> ready, visible;
    int misses = 0;
    auto next = Clock::now();
    for (uint32_t i = 0; i < kFrames; ++i) {
        next += std::chrono::microseconds(4000);
        std::this_thread::sleep_until(next);
        const size_t k = i % in.frames;
        req.push({i, scaleLoudnessDb(in.ld[k]), scaleF0Hz(in.f0[k]), nowNs()});
        bell.fetch_add(1, std::memory_order_release);
        bell.notify_one();
        Result r;
        while (!res.pop(r)) {}  // spin: measures the floor without poll quantisation
        visible.push_back(double(nowNs() - r.pushedNs) / 1000.0);
        ready.push_back(double(r.doneNs - r.pushedNs) / 1000.0);  // worker-side: wake + decode
        if (ready.back() > 4000.0) ++misses;
    }
    stop.store(true, std::memory_order_release);
    bell.fetch_add(1, std::memory_order_release);
    bell.notify_one();
    worker.join();
    std::printf("handoff, %d frames posted every 4 ms (audio post -> inference thread wake -> decode -> result):\n", kFrames);
    percentiles(ready, "ready (worker clock)", "us");
    percentiles(visible, "visible (spin poll)", "us");
    std::printf("  frames later than one 4 ms hop: %d of %d (%.2f%%)\n", misses, kFrames, 100.0 * misses / kFrames);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: ddaw_b1 {verify|bench} <instrument> [modelsDir]\n"); return 2; }
    const std::string cmd = argv[1], inst = argv[2];
    const std::string models = argc > 3 ? argv[3] : std::string(DDAW_MODELS_DIR);
    try {
        if (cmd == "verify") return verify(inst, models);
        if (cmd == "bench") return bench(inst, models);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
