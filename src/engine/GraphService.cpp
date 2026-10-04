#include "engine/GraphService.h"

#include <chrono>

namespace ddaw::engine {

namespace {
using Clock = std::chrono::steady_clock;

BuildResult buildLive(const project::Project& p, double sr, uint32_t epoch, const project::SampleBank* bank) {
    project::Fixture fx;
    fx.name = "live";
    fx.scope.kind = "live";
    fx.project = p;
    return buildGraph(fx, sr, epoch, bank);
}
}  // namespace

GraphService::GraphService(Engine& engine, double sr) : engine_(engine), sr_(sr) {}

GraphService::~GraphService() { stop(); }

void GraphService::setSampleBank(std::shared_ptr<const project::SampleBank> bank) {
    std::lock_guard<std::mutex> lk(m_);
    bank_ = std::move(bank);
}

void GraphService::buildNow(const project::Project& p) {
    std::shared_ptr<const project::SampleBank> bank;
    uint32_t epoch;
    {
        std::lock_guard<std::mutex> lk(m_);
        bank = bank_;
        epoch = ++epochCounter_;
    }
    BuildResult b = buildLive(p, sampleRate(), epoch, bank.get());
    auto resolver = std::make_shared<const ParamResolver>(std::move(b.resolver));
    engine_.setInitialGraph(std::move(b.graph));
    std::lock_guard<std::mutex> lk(m_);
    resolver_ = std::move(resolver);
    publishedEpoch_ = epoch;
    ++stats_.builds;
    cv_.notify_all();
}

void GraphService::start() {
    if (running_.exchange(true)) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void GraphService::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    collectRetired();
}

void GraphService::submit(std::shared_ptr<const project::Project> snapshot) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (pending_) ++stats_.coalesced;
        pending_ = std::move(snapshot);
        ++stats_.submitted;
        busy_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
}

bool GraphService::waitForEpoch(uint32_t epoch, int timeoutMs) const {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return publishedEpoch_ >= epoch; });
}

uint32_t GraphService::publishedEpoch() const { std::lock_guard<std::mutex> lk(m_); return publishedEpoch_; }

std::shared_ptr<const ParamResolver> GraphService::resolver() const { std::lock_guard<std::mutex> lk(m_); return resolver_; }

ServiceStats GraphService::stats() const { std::lock_guard<std::mutex> lk(m_); return stats_; }

bool GraphService::pushParam(const std::string& key, float value) {
    std::shared_ptr<const ParamResolver> r;
    uint32_t epoch;
    {
        std::lock_guard<std::mutex> lk(m_);
        r = resolver_;
        epoch = publishedEpoch_;
    }
    ParamAddr a;
    if (!r || !r->resolve(key, a)) return false;
    Cmd c;
    c.type = CmdType::SetParam;
    c.epoch = epoch;
    c.setParam = {a, value};
    return engine_.commands().push(c);
}

void GraphService::collectRetired() {
    while (auto old = engine_.takeRetired()) { /* destroyed here, off the audio thread */ }
}

void GraphService::run() {
    for (;;) {
        std::shared_ptr<const project::Project> snap;
        std::shared_ptr<const project::SampleBank> bank;
        uint32_t epoch = 0;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(25), [&] { return stop_ || pending_; });
            if (stop_) return;
            if (pending_) {
                snap = std::move(pending_);
                pending_.reset();
                bank = bank_;
                epoch = ++epochCounter_;
            }
        }
        collectRetired();
        if (!snap) continue;

        const auto t0 = Clock::now();
        std::unique_ptr<Graph> graph;
        std::shared_ptr<const ParamResolver> resolver;
        try {
            BuildResult b = buildLive(*snap, sampleRate(), epoch, bank.get());
            graph = std::move(b.graph);
            resolver = std::make_shared<const ParamResolver>(std::move(b.resolver));
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(m_);
            ++stats_.failures;
            stats_.lastError = e.what();
            if (!pending_) busy_.store(false, std::memory_order_release);
            continue;  // the previous graph stays live
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

        // Hand it over. The lane has room for a few graphs; if the Engine is not running it fills up and we wait.
        bool posted = false;
        while (!stop_) {
            if (engine_.postGraph(graph)) { posted = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            collectRetired();
        }
        if (!posted) return;

        // Wait for the audio thread to adopt it, then publish the matching resolver. Until then producers
        // keep using the previous resolver, whose epoch is still the live one.
        while (!stop_ && engine_.liveEpoch() != epoch) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            collectRetired();
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            if (engine_.liveEpoch() == epoch) {
                resolver_ = std::move(resolver);
                publishedEpoch_ = epoch;
            }
            ++stats_.builds;
            stats_.lastBuildMs = ms;
            if (!pending_) busy_.store(false, std::memory_order_release);
            cv_.notify_all();
        }
        collectRetired();
    }
}

}  // namespace ddaw::engine
