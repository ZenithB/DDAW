#pragma once
// The builder thread (ARCH 7): turns project snapshots into graphs off the audio thread, hands them
// to the Engine, collects the retired ones, and publishes the resolver that matches the live graph.
//
//   any thread  submit(snapshot)       latest wins; bursts of edits coalesce into one build
//   UI thread   pushParam(key, v)      resolve with the live epoch's resolver and push a SetParam command
//   service     build -> postGraph -> wait for the Engine to swap -> publish resolver -> delete the old graph
//
// A graph is never deleted on the audio thread. A command stamped with an epoch that is no longer live
// is dropped by the Engine (ARCH 7), so a parameter edit racing a swap cannot land on the wrong graph;
// callers that need the edit to survive a swap submit a fresh snapshot (see document::Session).
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "engine/Engine.h"
#include "engine/GraphBuilder.h"
#include "project/Project.h"
#include "project/SampleBank.h"

namespace ddaw::engine {

struct ServiceStats {
    uint64_t submitted = 0, builds = 0, failures = 0, coalesced = 0;
    double lastBuildMs = 0;
    std::string lastError;
};

class GraphService {
public:
    GraphService(Engine& engine, double sampleRate);
    ~GraphService();
    GraphService(const GraphService&) = delete;
    GraphService& operator=(const GraphService&) = delete;

    void setSampleBank(std::shared_ptr<const project::SampleBank> bank);
    // The device's rate changed (the Engine was re-prepared): later builds use it. Submit a snapshot afterwards.
    void setSampleRate(double sr) { sr_.store(sr, std::memory_order_release); }
    double sampleRate() const { return sr_.load(std::memory_order_acquire); }

    // Synchronous first build, installed directly. Call BEFORE audio starts. Throws on a bad project.
    void buildNow(const project::Project& p);

    // Start the thread. Idempotent.
    void start();
    void stop();

    // Any thread. The newest snapshot wins; older unbuilt ones are dropped (counted as coalesced).
    void submit(std::shared_ptr<const project::Project> snapshot);

    // True from submit() until the Engine has swapped to a graph built from a snapshot at least this new.
    bool busy() const { return busy_.load(std::memory_order_acquire); }

    // Block until the published graph has epoch >= `epoch` (tests, shutdown), or the timeout passes.
    bool waitForEpoch(uint32_t epoch, int timeoutMs) const;
    uint32_t publishedEpoch() const;
    std::shared_ptr<const ParamResolver> resolver() const;

    // UI thread only (the command lane has a single producer). Resolves `key` against the live
    // graph's resolver and pushes a SetParam stamped with that graph's epoch. False if the key is unknown
    // (a bypassed or missing device, a track that does not exist) or the lane is full.
    bool pushParam(const std::string& key, float value);

    ServiceStats stats() const;

private:
    void run();
    void collectRetired();

    Engine& engine_;
    std::atomic<double> sr_;
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    std::shared_ptr<const project::Project> pending_;
    std::shared_ptr<const project::SampleBank> bank_;
    std::shared_ptr<const ParamResolver> resolver_;
    uint32_t publishedEpoch_ = 0;
    uint32_t epochCounter_ = 0;
    ServiceStats stats_;
    std::atomic<bool> busy_{false}, stop_{false}, running_{false};
    std::thread thread_;
};

}  // namespace ddaw::engine
