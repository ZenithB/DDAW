// ddaw_soak: a long run of the real engine under load, for the A6 exit check (PLAN 6: "a 30-minute soak at
// a 64-frame buffer").
//
//   ddaw_soak [--minutes 30] [--block 64] [--rate 48000] [--realtime] [--seed N] [--report out.json]
//
// An "audio thread" runs the Engine in `block`-frame callbacks, with the project's graph kept live by the
// builder thread, while the main thread plays a musician with a short attention span: it launches scenes,
// starts and stops, changes tempo, tweaks parameters, adds and removes tracks, effects and notes, swaps
// instruments, moves clips, undoes and redoes, and saves and reloads the project. Default mode runs the
// audio faster than real time (the minutes are of audio); --realtime paces it against the clock and also
// counts callbacks that woke up late.
//
// Checks (a violation is a non-zero exit): no allocation on the audio thread, no NaN/inf, output within
// full scale, no failed builds, no growth in resident memory after warm-up, no retired graphs left behind,
// and the slowest callback inside its buffer's time budget.
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

#include "../tests/AllocGuard.h"
#include "app/model/AppModel.h"
#include "app/model/Controllers.h"
#include "app/model/Catalog.h"
#include "app/model/Stress.h"
#ifdef DDAW_SOAK_DDSP
#include "ddsp/DdspInstrument.h"
#endif
#include "project/ProjectJson.h"
#include "project/SampleBank.h"

using namespace ddaw;
using Clock = std::chrono::steady_clock;

namespace {

// Ask the kernel for the scheduling class CoreAudio gives a device callback (time-constraint policy: a
// guaranteed slice every period), so the paced run measures the engine, not a normal thread's scheduling.
bool makeRealtime(double periodUs, double computeUs) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double toAbs = 1e3 * double(tb.denom) / double(tb.numer);   // microseconds -> mach absolute units
    thread_time_constraint_policy_data_t p;
    p.period = uint32_t(periodUs * toAbs);
    p.computation = uint32_t(computeUs * toAbs);
    p.constraint = uint32_t(periodUs * toAbs);
    p.preemptible = 1;
    return thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&p), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

size_t residentBytes() {
    mach_task_basic_info info;
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &n) != KERN_SUCCESS) return 0;
    return info.resident_size;
}

struct Stats {
    std::atomic<uint64_t> blocks{0}, nanBlocks{0}, overFull{0}, late{0}, overBudget{0}, allocs{0};
    std::atomic<double> maxUs{0}, sumUs{0}, maxPeak{0}, maxCpuUs{0};
    std::atomic<double> maxSwapCpuUs{0}, maxSteadyCpuUs{0};   // CPU time of the slowest swap block / non-swap block
    std::atomic<uint64_t> overBudgetCpu{0};   // over budget in CPU time (the engine really was too slow), not just wall time
    std::atomic<uint64_t> hist[64]{};   // log2(us) buckets, for the p99.9 estimate
};

double percentile(const Stats& s, double q) {
    uint64_t total = 0;
    for (auto& h : s.hist) total += h.load();
    if (!total) return 0;
    uint64_t acc = 0;
    for (int i = 0; i < 64; ++i) {
        acc += s.hist[i].load();
        if (double(acc) >= q * double(total)) return std::pow(2.0, i + 1);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    double minutes = 30;
    int block = 64;
    double sr = 48000;
    bool realtime = false;
    uint32_t seed = 12345;
    std::string report;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--minutes") minutes = std::atof(next().c_str());
        else if (a == "--block") block = std::atoi(next().c_str());
        else if (a == "--rate") sr = std::atof(next().c_str());
        else if (a == "--realtime") realtime = true;
        else if (a == "--seed") seed = uint32_t(std::atol(next().c_str()));
        else if (a == "--report") report = next();
    }
#ifndef NDEBUG
    std::printf("NOTE: this is an unoptimised (Debug) build. Timings are meaningless and are not checked; use a Release build for the budget checks.\n");
    constexpr bool kTimed = false;
#else
    constexpr bool kTimed = true;
#endif
    const uint64_t totalFrames = uint64_t(minutes * 60.0 * sr);
    const double periodUs = 1e6 * block / sr;

    engine::Engine eng;
    eng.prepare(sr);
    app::AppModel model(eng, sr);
    // the audio thread comes first: the builder waits for the engine to swap each graph in
    Stats st;
    std::atomic<bool> run{true};
    std::atomic<uint64_t> framesDone{0};
    std::thread audio([&] {
        const size_t nb = static_cast<size_t>(block);
        std::vector<float> l(nb), r(nb), in(nb);
        double vph = 0;
        uint32_t noiseState = 12345;
        if (realtime && !makeRealtime(periodUs, periodUs * 0.5)) std::printf("note: could not get a real-time thread policy\n");
        test::AllocGuard guard;
        auto next = Clock::now();
        int slow = 0;
        while (run.load(std::memory_order_acquire) && framesDone.load() < totalFrames) {
            if (realtime) {
                std::this_thread::sleep_until(next);
                const auto woke = Clock::now();
                if (std::chrono::duration<double, std::micro>(woke - next).count() > periodUs) st.late++;
                next += std::chrono::microseconds(int64_t(periodUs));
            }
            const auto t0 = Clock::now();
            const uint64_t c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
            const uint32_t swapsBefore = eng.diagnostics().graphSwaps.load(std::memory_order_relaxed);
            {   // the "microphone": a wandering voice with breaths of silence and bursts of noise
                const uint64_t f0 = framesDone.load();
                for (int i = 0; i < block; ++i) {
                    const double t = double(f0 + uint64_t(i)) / sr;
                    const double seg = std::fmod(t, 7.0);
                    float v = 0.0f;
                    if (seg < 5.0) {                           // voiced: a slow melody with vibrato, swelling
                        const double hz = 196.0 * std::pow(2.0, 0.5 * std::sin(2.0 * 3.14159265 * 0.3 * t) + 0.02 * std::sin(2.0 * 3.14159265 * 5.5 * t));
                        vph += 2.0 * 3.14159265 * hz / sr;
                        v = 0.35f * float(std::sin(vph) + 0.4 * std::sin(2.0 * vph)) * float(0.6 + 0.4 * std::sin(2.0 * 3.14159265 * 0.7 * t));
                    } else if (seg < 6.0) {                    // a breath of noise
                        noiseState = noiseState * 1664525u + 1013904223u;
                        v = 0.1f * (float(noiseState >> 9) / 4194304.0f - 1.0f);
                    }                                          // then a second of silence
                    in[size_t(i)] = v;
                }
            }
            eng.processIO(in.data(), in.data(), l.data(), r.data(), block);
            const double cpuUs = double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0) / 1000.0;
            const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            const bool swapBlock = eng.diagnostics().graphSwaps.load(std::memory_order_relaxed) != swapsBefore;
            if (swapBlock) { if (cpuUs > st.maxSwapCpuUs.load()) st.maxSwapCpuUs = cpuUs; }
            else if (cpuUs > st.maxSteadyCpuUs.load()) st.maxSteadyCpuUs = cpuUs;
            if (cpuUs > periodUs) st.overBudgetCpu++;
            if (cpuUs > st.maxCpuUs.load()) st.maxCpuUs = cpuUs;
            if (us > periodUs && slow < 12) {   // keep the first few offenders: when, how long on the wall and on the CPU
                std::printf("  slow block at %.2f min of audio: wall %.0f us, cpu %.0f us%s\n", double(framesDone.load()) / 48000.0 / 60.0, us, cpuUs, swapBlock ? " (graph swap)" : "");
                ++slow;
            }
            float pk = 0;
            bool bad = false;
            for (int i = 0; i < block; ++i) {
                const float a = std::abs(l[size_t(i)]), b = std::abs(r[size_t(i)]);
                if (!std::isfinite(a) || !std::isfinite(b)) bad = true;
                pk = std::max({pk, a, b});
            }
            if (bad) st.nanBlocks++;
            if (pk > 1.0f) st.overFull++;
            if (pk > st.maxPeak.load()) st.maxPeak = pk;
            if (us > periodUs) st.overBudget++;
            if (us > st.maxUs.load()) st.maxUs = us;
            st.sumUs = st.sumUs.load() + us;
            st.hist[std::min(63, int(std::log2(std::max(us, 1.0))))]++;
            st.blocks++;
            framesDone += uint64_t(block);
        }
        st.allocs = guard.count();
    });


#ifdef DDAW_SOAK_DDSP
    ddsp::registerDevices();
#endif
    app::buildStressProject(model, sr);
    while (model.service().busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // ---- the musician ----
    std::mt19937 rng(seed);
    auto pick = [&](size_t n) { return n ? size_t(rng() % n) : 0; };
    auto uni = [&] { return double(rng() % 100000) / 100000.0; };
    uint64_t actions = 0, rejected = 0, reloads = 0;
    auto nextReload = uint64_t(180 * sr);
    const auto pkg = (std::filesystem::temp_directory_path() / "ddaw_soak.ddaw").string();
    size_t rssWarm = 0, rssPeak = 0;
    uint64_t nextReport = uint64_t(60 * sr);
    const auto wall0 = Clock::now();
    bool arrMode = false;

    while (framesDone.load() < totalFrames) {
        const auto& p = model.project();
        const size_t nT = p.tracks.size();
        bool ok = true;
        switch (rng() % 27) {
            case 0: if (!p.scenes.empty()) model.launchScene(p.scenes[pick(p.scenes.size())]); break;
            case 1: model.stopAllClips(); break;
            case 2: arrMode = !arrMode; model.play(arrMode, 0.0); break;
            case 3: model.stop(); break;
            case 4: ok = model.apply(document::cmd::setMeta("bpm", 80.0 + 80.0 * uni())); break;
            case 5: if (nT) ok = model.apply(document::cmd::setTrack(p.tracks[pick(nT)].uid, "mute", rng() % 2 == 0)); break;
            case 6: if (nT) ok = model.apply(document::cmd::setTrack(p.tracks[pick(nT)].uid, "gain", -30.0 * uni())); break;
            case 7: case 8: case 9: {   // a random parameter of a random device
                if (!nT) break;
                const auto& t = p.tracks[pick(nT)];
                const project::DeviceSpec* d = nullptr;
                app::Chain chain = app::Chain::Effect;
                if (!t.fx.empty() && rng() % 2) d = &t.fx[pick(t.fx.size())];
                else if (!t.inst.type.empty() && t.kind != project::TrackKind::Bus) { d = &t.inst; chain = app::Chain::Instrument; }
                if (!d || d->uid == 0) break;
                const auto* info = app::findDevice(chain, d->type);
                if (!info || info->params.empty()) break;
                const auto& spec = info->params[pick(info->params.size())];
                ok = model.apply(document::cmd::setParam(d->uid, spec.key, app::paramFromUnit(spec, uni())));
                break;
            }
            case 10: if (nT) { const auto& t = p.tracks[pick(nT)]; if (t.fx.size() < 5 && t.kind != project::TrackKind::Bus) {
                        static const char* fxs[] = {"reverb", "delay", "chorus", "comp", "filter", "phaser", "dist", "eq", "widen", "gate"};
                        ok = model.apply(app::edit::addDevice(p, t.uid, "fx", fxs[pick(10)])); } } break;
            case 11: if (nT) { const auto& t = p.tracks[pick(nT)]; if (!t.fx.empty()) ok = model.apply(app::edit::removeDevice(t.fx[pick(t.fx.size())].uid)); } break;
            case 12: if (nT < 14) { using K = project::TrackKind; const K ks[] = {K::Synth, K::Drum, K::Audio, K::Bus}; ok = model.apply(app::edit::addTrack(p, ks[pick(4)])); } break;
            case 13: if (nT > 4) ok = model.apply(app::edit::removeTrack(p.tracks[pick(nT)].uid)); break;
            case 14: if (nT) {   // a note in a random existing clip
                const auto& t = p.tracks[pick(nT)];
                for (auto& s : p.scenes) if (p.clips.count(t.id + "|" + s) && !p.clips.at(t.id + "|" + s).audio) {
                    ok = model.apply(app::edit::addNote({t.uid, s, ""}, int(36 + pick(48)), 24.0 * double(pick(16)), 24.0));
                    break;
                } } break;
            case 15: model.undo(); break;
            case 16: model.redo(); break;
            case 17: if (!p.arr.empty()) { auto it = p.arr.begin(); std::advance(it, long(pick(p.arr.size())));
                         try { ok = model.apply(app::edit::moveArrClip(p, it->first, 96.0 * double(pick(40)))); } catch (...) { ok = false; } } break;
            case 18: if (nT) { const auto& t = p.tracks[pick(nT)]; if (t.kind == project::TrackKind::Synth) {
                         static const char* is[] = {"poly", "mono", "duo", "fm", "keys", "pluck", "sampler", "fmop", "harmnoise", "subtractive", "wavetable", "waveshaper", "modal", "perc"};
                         const std::string ty = is[pick(14)];
                         if (ty != "sampler") ok = model.apply(app::edit::setInstrument(p, t.uid, ty)); } } break;
            case 19: ok = model.apply(document::cmd::setMeta("loopOn", rng() % 2 == 0)); break;
            case 20: model.setMetronome(rng() % 2 == 0); break;
            case 21: if (nT) {   // an audio-rate route on a random FM Operators track: an oscillator or another track's audio
                const auto& t = p.tracks[pick(nT)];
                if (t.inst.type == "fmop" && t.arate.size() < 4) {
                    static const char* ports[] = {"index", "pitch", "amp"};
                    project::ARateSpec a;
                    a.id = "r" + std::to_string(rng() % 100000);
                    a.source = rng() % 2 ? "osc" : "track";
                    a.shape = int(rng() % 4); a.hz = 0.5 + 2000.0 * uni(); a.follow = rng() % 2 == 0; a.depth = 2.0 * uni() - 1.0;
                    a.srcTrack = p.tracks[pick(nT)].id;
                    a.target = {"inst", "inst", ports[pick(3)]};
                    ok = model.apply({"arate.insert", {{"track", t.uid}, {"index", t.arate.size()}, {"arate", project::arateToJson(a)}}});
                } } break;
            case 23: {   // a controller moves: a stick, a trigger, a CC (bound ones drive morph sticks live, the rest are ignored)
                static const char* srcs[] = {"pad:lx", "pad:ly", "pad:rx", "midi:cc74", "midi:bend", "pad:rt"};
                model.controllerInput(srcs[pick(6)], uni());
                break;
            }
            case 24: if (nT) {   // a morph map on a random FM Operators track: add, move the stick, remove
                const auto& t = p.tracks[pick(nT)];
                if (t.inst.type == "fmop") {
                    if (t.morph.empty()) {
                        project::MorphSpec m; m.name = "m"; m.method = rng() % 2 ? "rbf" : "idw";
                        m.targets = {{"inst", "inst", "index"}, {"inst", "inst", "l2"}};
                        m.anchors = {{"a", 0.2, 0.2, {0.2, 0.3}}, {"b", 0.8, 0.7, {0.8, 0.9}}};
                        ok = model.apply({"morph.insert", {{"track", t.uid}, {"index", 0}, {"morph", project::morphToJson(m)}}});
                    } else if (rng() % 4 == 0) {
                        ok = model.apply({"morph.remove", {{"track", t.uid}, {"index", 0}}});
                    } else {
                        ok = model.apply({"morph.pos", {{"track", t.uid}, {"index", 0}, {"x", uni()}, {"y", uni()}}});
                    }
                } } break;
            case 25: {   // live playing with expression, as an MPE controller would: bend and pressure before and after the note-on
                const int pit = int(36 + pick(48));
                model.noteExpression(pit, 2, float((uni() - 0.5) * 24.0));
                model.noteOn(pit, 0.3f + 0.6f * float(uni()));
                model.noteExpression(pit, 1, float(uni()));
                model.noteExpression(pit, 0, float(uni()));
                if (rng() % 3 == 0) model.bend(float((uni() - 0.5) * 4.0));
                if (rng() % 2) model.noteOff(pit);
                break;
            }
            case 26: model.allNotesOff(); model.bend(0.0f); break;
            case 22: if (nT) {   // retune or remove a route (live fields do not rebuild the graph)
                const auto& t = p.tracks[pick(nT)];
                if (!t.arate.empty()) {
                    const size_t i = pick(t.arate.size());
                    const int what = int(rng() % 3);
                    if (what == 0) ok = model.apply({"arate.remove", {{"track", t.uid}, {"index", i}}});
                    else ok = model.apply({"arate.field", {{"track", t.uid}, {"index", i}, {"field", what == 1 ? "depth" : "hz"}, {"value", what == 1 ? 2.0 * uni() - 1.0 : 0.5 + 3000.0 * uni()}}});
                } } break;
        }
        ++actions;
        if (!ok) ++rejected;

        model.tick();   // keeps the engine's track indices in step with the rebuilds
        const uint64_t f = framesDone.load();
        if (f >= nextReload) {   // save and reload the whole project, mid-flight
            std::string e2;
            std::filesystem::remove_all(pkg);
            if (model.save(pkg, e2) && model.open(pkg, e2)) ++reloads; else std::fprintf(stderr, "reload failed: %s\n", e2.c_str());
            nextReload += uint64_t(180 * sr);
        }
        if (f >= nextReport) {
            const size_t rss = residentBytes();
            if (nextReport == uint64_t(60 * sr)) rssWarm = rss;     // after the first minute everything should have settled
            rssPeak = std::max(rssPeak, rss);
            const auto svc = model.service().stats();
            std::printf("[%4.0f min audio | %5.0f s wall] blocks %llu | rss %.0f MB | actions %llu (%llu rejected) | builds %llu (%llu coalesced, %llu failed) | max block %.0f us | allocs %llu\n",
                        double(f) / sr / 60.0, std::chrono::duration<double>(Clock::now() - wall0).count(), (unsigned long long)st.blocks.load(), double(rss) / 1048576.0,
                        (unsigned long long)actions, (unsigned long long)rejected, (unsigned long long)svc.builds, (unsigned long long)svc.coalesced, (unsigned long long)svc.failures,
                        st.maxUs.load(), (unsigned long long)st.allocs.load());
            std::fflush(stdout);
            nextReport += uint64_t(60 * sr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(realtime ? 120 : 8));
    }
    run = false;
    audio.join();
    model.stop();
    while (model.service().busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // let the builder collect the last retired graph

    const auto svc = model.service().stats();
    const size_t rssEnd = residentBytes();
    const double growthMB = rssWarm ? (double(std::max(rssEnd, rssPeak)) - double(rssWarm)) / 1048576.0 : 0.0;
    const uint32_t backlog = eng.diagnostics().retireBacklog.load();
    const double meanUs = st.blocks ? st.sumUs.load() / double(st.blocks) : 0;

    std::printf("\nblocks %llu (%d frames, %.1f min of audio) in %.0f s | mean %.1f us, p99.9 <= %.0f us, max %.0f us, budget %.0f us (%.1f%% worst)\n",
                (unsigned long long)st.blocks.load(), block, double(framesDone.load()) / sr / 60.0, std::chrono::duration<double>(Clock::now() - wall0).count(), meanUs,
                percentile(st, 0.999), st.maxUs.load(), periodUs, 100.0 * st.maxUs.load() / periodUs);
    std::printf("slowest callback: %.0f us of wall time but only %.0f us of CPU time | %llu callbacks over budget in wall time, %llu in CPU time\n", st.maxUs.load(), st.maxCpuUs.load(),
                (unsigned long long)st.overBudget.load(), (unsigned long long)st.overBudgetCpu.load());
    std::printf("CPU time of the slowest block: %.0f us when a graph swapped in, %.0f us otherwise\n", st.maxSwapCpuUs.load(), st.maxSteadyCpuUs.load());
    {
        const auto& d = eng.diagnostics();
        std::printf("engine timers (high-water, us): chunk %.0f | swap chunk %.0f = bookkeeping %.0f + retiring graph %.0f + new graph\n", double(d.maxChunkNs.load()) / 1e3,
                    double(d.maxSwapChunkNs.load()) / 1e3, double(d.maxSwapSetupNs.load()) / 1e3, double(d.maxOldRenderNs.load()) / 1e3);
    }
    std::printf("tracker: %llu frames, %llu dropped from the queue (the model drains it)\n", (unsigned long long)eng.performanceFrameCount(), (unsigned long long)eng.performanceFramesDropped());
    std::printf("actions %llu (%llu rejected as invalid) | reloads %llu | builds %llu, coalesced %llu, failed %llu | graph swaps %u | stale commands dropped %u\n",
                (unsigned long long)actions, (unsigned long long)rejected, (unsigned long long)reloads, (unsigned long long)svc.builds, (unsigned long long)svc.coalesced,
                (unsigned long long)svc.failures, eng.diagnostics().graphSwaps.load(), eng.diagnostics().staleCommandsDropped.load());
    std::printf("audio-thread allocations %llu | NaN/inf blocks %llu | blocks over full scale %llu (max peak %.3f) | over budget %llu | late wakeups %llu%s\n",
                (unsigned long long)st.allocs.load(), (unsigned long long)st.nanBlocks.load(), (unsigned long long)st.overFull.load(), st.maxPeak.load(),
                (unsigned long long)st.overBudget.load(), (unsigned long long)st.late.load(), realtime ? "" : " (not measured: accelerated)");
    std::printf("resident memory: %.0f MB after the first minute, peak %.0f MB, end %.0f MB (growth %.0f MB) | retired-graph backlog %u\n",
                double(rssWarm) / 1048576.0, double(rssPeak) / 1048576.0, double(rssEnd) / 1048576.0, growthMB, backlog);

    std::vector<std::string> fails;
    if (st.allocs.load()) fails.push_back("allocation on the audio thread");
    if (st.nanBlocks.load()) fails.push_back("NaN or inf in the output");
    if (st.overFull.load()) fails.push_back("output above full scale");
    if (svc.failures) fails.push_back("failed graph builds: " + svc.lastError);
    if (kTimed && st.overBudgetCpu.load()) fails.push_back("a callback needed more CPU time than its buffer lasts");
    if (kTimed && realtime && st.overBudget.load()) fails.push_back("a callback missed its deadline");
    if (kTimed && realtime && st.late.load() > 5) fails.push_back("late wakeups (system scheduling, not necessarily the engine)");
    if (growthMB > 64.0) fails.push_back("resident memory grew by more than 64 MB after warm-up");
    if (backlog) fails.push_back("retired graphs were left behind");
    for (auto& f : fails) std::printf("FAIL: %s\n", f.c_str());
    if (fails.empty()) std::printf("PASS\n");
    if (!report.empty()) {
        if (FILE* f = std::fopen(report.c_str(), "w")) {
            std::fprintf(f, "{\"minutes\":%.1f,\"block\":%d,\"realtime\":%s,\"blocks\":%llu,\"maxUs\":%.1f,\"p999Us\":%.0f,\"allocs\":%llu,\"nan\":%llu,\"failedBuilds\":%llu,\"rssGrowthMB\":%.1f,\"pass\":%s}\n",
                         minutes, block, realtime ? "true" : "false", (unsigned long long)st.blocks.load(), st.maxUs.load(), percentile(st, 0.999),
                         (unsigned long long)st.allocs.load(), (unsigned long long)st.nanBlocks.load(), (unsigned long long)svc.failures, growthMB, fails.empty() ? "true" : "false");
            std::fclose(f);
        }
    }
    std::filesystem::remove_all(pkg);
    return fails.empty() ? 0 : 1;
}
