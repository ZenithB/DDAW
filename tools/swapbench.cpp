// ddaw_swapbench: what does a graph swap cost the audio thread, and where does it go?
// Plays the stress project (soak's: ~13 tracks, effects, buses, audio clips, sampler, tracker, monitoring, and the
// DDSP violin when the models are present) on a real-time thread at a 64-frame buffer, posts a freshly built graph
// every --every seconds, and reports per-callback CPU time for steady blocks and swap blocks, plus the engine's own
// phase timers (swap bookkeeping, the retiring graph's render, the whole chunk). Run in Release.
//   ddaw_swapbench [--seconds 20] [--every 0.5] [--block 64] [--no-ddsp] [--tracks N]
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "app/model/AppModel.h"
#include "app/model/Stress.h"
#include "devices/Registry.h"
#include "engine/GraphBuilder.h"
#ifdef DDAW_SWAPBENCH_DDSP
#include "ddsp/DdspInstrument.h"
#endif

using namespace ddaw;
using Clock = std::chrono::steady_clock;

static double cpuUs() { return double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)) / 1000.0; }

static bool makeRealtime(double periodUs, double computeUs) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double toAbs = 1e3 * double(tb.denom) / double(tb.numer);
    thread_time_constraint_policy_data_t p;
    p.period = uint32_t(periodUs * toAbs);
    p.computation = uint32_t(computeUs * toAbs);
    p.constraint = uint32_t(periodUs * toAbs);
    p.preemptible = 1;
    return thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&p), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

static double pct(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[size_t(q * double(v.size() - 1))];
}

int main(int argc, char** argv) {
    double seconds = 20, every = 0.5;
    int block = 64;
    bool ddsp = true, warm = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--every") every = std::atof(next().c_str());
        else if (a == "--block") block = std::atoi(next().c_str());
        else if (a == "--no-ddsp") ddsp = false;
        else if (a == "--warm") warm = true;   // warm each new graph on this (the builder) thread before posting it
    }
    const double sr = 48000.0, periodUs = 1e6 * block / sr;
    engine::Engine eng;
    eng.prepare(sr);
    app::AppModel model(eng, sr);
    model.service().stop();   // we drive the builds ourselves
#ifdef DDAW_SWAPBENCH_DDSP
    if (ddsp) ddsp::registerDevices();
#endif
    app::buildStressProject(model, sr);
    const auto proj = model.project();
    project::Fixture fx;
    fx.scope.kind = "live";
    fx.project = proj;
    std::printf("project: %zu tracks%s\n", proj.tracks.size(), ddaw::instrumentRegistered("ddsp") ? " (including the DDSP violin)" : "");

    uint32_t epoch = 1;
    {
        auto b = engine::buildGraph(fx, sr, epoch, model.sampleBank().get());
        eng.setInitialGraph(std::move(b.graph));
    }
    Cmd tempo; tempo.type = CmdType::SetTempo; tempo.setTempo = {118.0}; eng.commands().push(tempo);
    for (int t = 0; t < int(proj.tracks.size()); ++t) { Cmd c; c.type = CmdType::ClipLaunch; c.epoch = epoch; c.clipLaunch = {uint16_t(t), 0}; eng.commands().push(c); }
    Cmd play; play.type = CmdType::TransportPlay; play.transportPlay = {0, 0.0}; eng.commands().push(play);
    eng.setTrackerEnabled(true);

    const double profChunks = seconds * sr / 128.0;
    std::vector<double> steady, swapBlocks;
    std::atomic<bool> run{true};
    std::atomic<uint32_t> swapsSeen{0};
    std::thread audio([&] {
        makeRealtime(periodUs, periodUs * 0.5);
        const size_t nb = static_cast<size_t>(block);
        std::vector<float> l(nb), r(nb), in(nb);
        double ph = 0;
        auto next = Clock::now();
        uint64_t frame = 0;
        while (run.load()) {
            std::this_thread::sleep_until(next);
            next += std::chrono::microseconds(int64_t(periodUs));
            for (size_t i = 0; i < nb; ++i) { ph += 2.0 * 3.14159265 * 220.0 / sr; in[i] = 0.3f * float(std::sin(ph)); }
            const uint32_t before = eng.diagnostics().graphSwaps.load(std::memory_order_relaxed);
            const double c0 = cpuUs();
            eng.processIO(in.data(), in.data(), l.data(), r.data(), block);
            const double us = cpuUs() - c0;
            const bool swapBlock = eng.diagnostics().graphSwaps.load(std::memory_order_relaxed) != before;
            if (frame > uint64_t(2.0 * sr)) (swapBlock ? swapBlocks : steady).push_back(us);   // after the warm-up
            frame += uint64_t(block);
            if (swapBlock) swapsSeen++;
        }
    });

    const auto start = Clock::now();
    double nextSwap = every + 2.0;
    bool reset = false;
    while (std::chrono::duration<double>(Clock::now() - start).count() < seconds + 2.0) {
        const double t = std::chrono::duration<double>(Clock::now() - start).count();
        if (!reset && t > 2.0) { eng.diagnostics().resetTimings(); reset = true; }
        if (t >= nextSwap) {
            nextSwap += every;
            auto b = engine::buildGraph(fx, sr, ++epoch, model.sampleBank().get());
            std::unique_ptr<engine::Graph> g = std::move(b.graph);
            if (warm) g->warmUp();
            while (!eng.postGraph(g)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (auto old = eng.takeRetired()) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    run = false;
    audio.join();
    while (auto old = eng.takeRetired()) {}

    auto line = [&](const char* name, const std::vector<double>& v) {
        std::printf("%-12s n=%-7zu median %6.0f  p99 %6.0f  p99.9 %6.0f  max %6.0f us   (budget %.0f)\n", name, v.size(), pct(v, 0.5), pct(v, 0.99), pct(v, 0.999), pct(v, 1.0), periodUs);
    };
    line("steady", steady);
    line("swap blocks", swapBlocks);
#ifdef DDAW_GRAPH_PROFILE
    if (auto* g = eng.profileGraph()) {
        std::printf("per track (mean us / worst us per chunk):\n");
        for (int i = 0; i < g->trackCount(); ++i)
            std::printf("  %-10s inst %6.1f / %6.1f   fx %6.1f / %6.1f\n", proj.tracks[size_t(i)].name.c_str(), double(g->profInstNs[i]) / 1e3 / double(profChunks), double(g->profInstMax[i]) / 1e3,
                        double(g->profFxNs[i]) / 1e3 / double(profChunks), double(g->profFxMax[i]) / 1e3);
    }
#endif
    const auto& d = eng.diagnostics();
    std::printf("engine timers (high-water, us): chunk %.0f | swap chunk %.0f = bookkeeping %.0f + retiring graph %.0f + new graph\n",
                d.maxChunkNs.load() / 1e3, d.maxSwapChunkNs.load() / 1e3, d.maxSwapSetupNs.load() / 1e3, d.maxOldRenderNs.load() / 1e3);
    std::printf("input side (tracker, rings): %.0f us high-water\n", d.maxInputNs.load() / 1e3);
    std::printf("swap blocks / budget: %.0f%% worst, %.0f%% p99\n", 100.0 * pct(swapBlocks, 1.0) / periodUs, 100.0 * pct(swapBlocks, 0.99) / periodUs);
    return 0;
}
