#include "engine/Engine.h"

#include "core/Denormals.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>

namespace ddaw::engine {

namespace {
inline void raiseTo(std::atomic<uint64_t>& a, uint64_t v) noexcept {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
inline uint64_t nsSince(std::chrono::steady_clock::time_point t) noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
}
}  // namespace

Engine::Engine() = default;

Engine::~Engine() {
    delete cur_;
    delete old_;
    for (int i = 0; i < nPendingRetire_; ++i) delete pendingRetire_[i];
    Graph* g;
    while (incoming_.pop(g)) delete g;
    while (retired_.pop(g)) delete g;
}

int Engine::masterLatency() const noexcept {
    switch (master_.mode) {
        case MasterLimiterConfig::Mode::Brickwall: return limiter_.latencySamples();
        case MasterLimiterConfig::Mode::ToneCompat: return toneLimiter_.latencySamples();
        default: return 0;
    }
}

void Engine::prepare(double sr, const MasterLimiterConfig& master) {
    sr_ = sr;
    master_ = master;
    transport_.prepare(sr);
    limiter_.prepare(sr, master.ceilingDb);
    toneLimiter_.configureNative(-1.0f, 20.0f, 0.003f, 0.010f);  // Tone.Limiter(-1)
    toneLimiter_.prepare(sr);
    metro_.prepare(sr);
    toneAmp_.prepare(sr, 10.0f);
    toneAmp_.snap(0.0f);
    tap_.prepare(size_t(sr * 30.0));
    polyTap_.prepare(size_t(sr * 4.0));
    polyMix_.assign(4096, 0.0f);
    tracker_.prepare(sr);
    trackerLatency_.store(tracker_.latencySamples());
    inRingL_.assign(1 << 15, 0.0f);
    inRingR_.assign(1 << 15, 0.0f);
    monoScratch_.assign(4096, 0.0f);
    monChunkL_.assign(kMaxBlock, 0.0f);
    monChunkR_.assign(kMaxBlock, 0.0f);
    monRead_ = 0;
    inputFrames_.store(0);
    trackerRunning_ = false;   // 30 s of input headroom for the recorder thread
    oldL_.assign(kMaxBlock, 0.0f);
    oldR_.assign(kMaxBlock, 0.0f);
    pendL_.assign(kMaxBlock, 0.0f);
    pendR_.assign(kMaxBlock, 0.0f);
    pendLen_ = pendPos_ = 0;
    latency_.store(masterLatency() + (cur_ ? cur_->latencySamples() : 0), std::memory_order_relaxed);
}

void Engine::setInitialGraph(std::unique_ptr<Graph> g) {
    delete cur_;
    cur_ = g.release();
    if (cur_) liveEpoch_.store(cur_->epoch(), std::memory_order_release);
    latency_.store(masterLatency() + (cur_ ? cur_->latencySamples() : 0), std::memory_order_relaxed);
}

bool Engine::postGraph(std::unique_ptr<Graph>& g) {
    if (!incoming_.push(g.get())) return false;
    g.release();
    return true;
}

std::unique_ptr<Graph> Engine::takeRetired() {
    Graph* g = nullptr;
    if (!retired_.pop(g)) return nullptr;
    return std::unique_ptr<Graph>(g);
}

void Engine::retire(Graph* g) noexcept {
    if (retired_.push(g)) return;
    if (nPendingRetire_ < 8) { pendingRetire_[nPendingRetire_++] = g; diag_.retireBacklog.store(uint32_t(nPendingRetire_), std::memory_order_relaxed); }
    // Else: eight graphs are already waiting for the builder to collect them. A graph is only ever
    // posted after the previous one was collected in practice; if this is hit the graph is leaked
    // rather than deleted on the audio thread.
}

void Engine::flushRetired() noexcept {
    while (nPendingRetire_ > 0 && retired_.push(pendingRetire_[nPendingRetire_ - 1])) --nPendingRetire_;
    diag_.retireBacklog.store(uint32_t(nPendingRetire_), std::memory_order_relaxed);
}

void Engine::pollIncomingGraph() noexcept {
    Graph* g = nullptr;
    while (incoming_.pop(g)) {
        if (old_) retire(old_);  // two swaps in one callback: the first fade never ran
        old_ = cur_;
        cur_ = g;
        if (old_) {  // clips launched in the outgoing graph keep playing in the new one (same track index)
            const int common = std::min(old_->trackCount(), cur_->trackCount());
            for (int i = 0; i < common; ++i)
                if (auto scene = old_->sched(i).launchedScene()) cur_->sched(i).launch(*scene, old_->sched(i).anchor());
        }
        if (transport_.playing()) cur_->relocateAll(transport_.mode(), transport_.positionTicks());
        if (old_) cur_->inheritNotes(*old_);
        if (old_) cur_->inheritMonitors(*old_);   // sustained notes keep sounding across the rebuild
        liveEpoch_.store(cur_->epoch(), std::memory_order_release);
        latency_.store(masterLatency() + cur_->latencySamples(), std::memory_order_relaxed);
        meters_.clearTracks(cur_->trackCount());
        diag_.graphSwaps.fetch_add(1, std::memory_order_relaxed);
    }
}

void Engine::drainCommands() noexcept {
    Cmd c;
    while (commands_.pop(c)) {
        if (isGraphAddressed(c.type) && (!cur_ || c.epoch != cur_->epoch())) {
            diag_.staleCommandsDropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        switch (c.type) {
            case CmdType::SetParam:      cur_->setParam(c.setParam.addr, c.setParam.value); break;
            case CmdType::NoteOn:
                cur_->noteOn(c.noteOn.track, c.noteOn.pitch, c.noteOn.vel, c.noteOn.noteId);
                cur_->fireDucks(c.noteOn.track, c.noteOn.pitch);
                break;
            case CmdType::NoteOff:       cur_->noteOff(c.noteOff.track, c.noteOff.noteId); break;
            case CmdType::Performance:   cur_->performance(c.performance.track, c.performance.frame); break;
            case CmdType::NoteExpression: break;  // MPE routing arrives with B5
            case CmdType::ClipLaunch: {
                if (c.clipLaunch.track >= cur_->trackCount()) break;
                // While stopped, launches anchor at 0 and wait for TransportPlay, so a scene's batch of
                // launches lands together. While playing they wait for the next launch boundary.
                const double anchor = transport_.playing() ? cur_->nextBoundaryTicks(transport_.positionTicks()) : 0.0;
                auto& sch = cur_->sched(c.clipLaunch.track);
                sch.launch(c.clipLaunch.clip, anchor);
                sch.relocate(TransportMode::Session, transport_.positionTicks());
                break;
            }
            case CmdType::ClipStop:
                if (c.clipStop.track < cur_->trackCount()) cur_->sched(c.clipStop.track).stopClip();
                break;
            case CmdType::TransportPlay: {
                const auto mode = c.transportPlay.mode == 1 ? TransportMode::Arrangement : TransportMode::Session;
                rng_ = XorShift(0x5F3F9A1B2C4D6E7Full);
                transport_.loopEnabled = !loopOverrideOff_ && mode == TransportMode::Arrangement && cur_ && cur_->loopOn();
                if (cur_) {
                    transport_.loopStart = cur_->loopStart();
                    transport_.loopEnd = std::max(cur_->loopStart() + 384.0, cur_->loopEnd());
                }
                transport_.play(c.transportPlay.fromTicks, mode);
                if (cur_) cur_->relocateAll(mode, transport_.positionTicks());
                resetMetronome();
                break;
            }
            case CmdType::TransportStop:
                transport_.stop();
                if (cur_) cur_->allNotesOff();
                break;
            case CmdType::SetTempo:      transport_.setTempo(c.setTempo.bpm); break;
            case CmdType::Tracking:      trackTarget_ = c.tracking.track < cur_->trackCount() ? c.tracking.track : -1; break;
            case CmdType::LiveTrack:     liveTrack_.store(c.liveTrack.track < cur_->trackCount() ? c.liveTrack.track : -1, std::memory_order_relaxed); break;
            case CmdType::MonitorInput:  cur_->setTrackMonitor(c.monitorInput.track, c.monitorInput.on != 0); break;
            case CmdType::TrackerConfig: {
                dsp::TrackerConfig tc = tracker_.config();
                tc.minHz = c.trackerConfig.minHz; tc.maxHz = c.trackerConfig.maxHz;
                tc.yinThreshold = c.trackerConfig.threshold; tc.voicedConfidence = c.trackerConfig.voicedConfidence;
                tracker_.setConfig(tc);
                trackerLatency_.store(tracker_.latencySamples(), std::memory_order_relaxed);
                break;
            }
            case CmdType::MetronomeOn:   metroOn_ = true; break;
            case CmdType::MetronomeOff:  metroOn_ = false; break;
            case CmdType::TestTone:
                toneOn_ = c.testTone.on != 0;
                toneInc_ = 2.0 * std::numbers::pi * double(c.testTone.freqHz) / sr_;
                toneAmp_.setTarget(toneOn_ ? std::pow(10.0f, c.testTone.levelDb / 20.0f) : 0.0f);
                break;
        }
    }
}

void Engine::resetMetronome() noexcept {
    if (!cur_) return;
    const double bt = cur_->beatTicks();
    nextMetroTick_ = std::ceil(transport_.positionTicks() / bt - 1e-9) * bt;  // the next beat at or after the playhead
}

void Engine::renderChunk(float* l, float* r, int n) noexcept {
    const ProcessContext ctx{sr_, transport_.positionTicks(), transport_.bpm(), transport_.playing(),
                             transport_.loopEnabled, transport_.loopStart, transport_.loopEnd, 4, 4,
                             transport_.mode() == TransportMode::Arrangement, offline_};
    const float decay = MeterBank::decay(n, sr_);

    if (cur_) {
        // block-rate modulation (automation, LFOs, macros) lands before the chunk renders
        perfAgeFrames_ = std::min<int64_t>(perfAgeFrames_ + n, int64_t(1) << 40);
        const bool perfFresh = trackerOn_.load(std::memory_order_relaxed) && perfAgeFrames_ < int64_t(0.1 * sr_);   // tracker live and not stale
        cur_->applyModulation(ctx.positionTicks, transport_.playing(), transport_.mode(), n, perfFresh ? &lastPerf_ : nullptr);
        cur_->process(l, r, n, ctx, &meters_, decay, metroOn_ ? &metro_ : nullptr);
    }
    else { std::fill_n(l, n, 0.0f); std::fill_n(r, n, 0.0f); }

    if (old_) {  // graph swap: linear crossfade over the first kSwapFade frames of this chunk, then the old graph is retired
        // (the retiring graph is rendered only for the fade, not the whole chunk: that render is pure overhead on the swap block)
        const int nf = std::min(n, kSwapFade);
        const auto tOld = std::chrono::steady_clock::now();
        old_->process(oldL_.data(), oldR_.data(), nf, ctx, nullptr, decay);
        raiseTo(diag_.maxOldRenderNs, nsSince(tOld));
        for (int i = 0; i < nf; ++i) {
            const float t = float(i + 1) / float(nf);
            l[i] = l[i] * t + oldL_[size_t(i)] * (1.0f - t);
            r[i] = r[i] * t + oldR_[size_t(i)] * (1.0f - t);
        }
        retire(old_);
        old_ = nullptr;
    }

    if (toneOn_ || !toneAmp_.settled() || toneAmp_.current() > 0.0f) {
        for (int i = 0; i < n; ++i) {
            const float s = toneAmp_.next() * static_cast<float>(std::sin(tonePhase_));
            tonePhase_ += toneInc_;
            if (tonePhase_ >= 2.0 * std::numbers::pi) tonePhase_ -= 2.0 * std::numbers::pi;
            l[i] += s; r[i] += s;
        }
    }

    float grDb = 0.0f;
    switch (master_.mode) {
        case MasterLimiterConfig::Mode::Brickwall: limiter_.process(l, r, n); grDb = limiter_.gainReductionDb(); break;
        case MasterLimiterConfig::Mode::ToneCompat: toneLimiter_.process(l, r, n); grDb = toneLimiter_.grDb(); break;
        case MasterLimiterConfig::Mode::Bypass: break;
    }
    float pk, rm;
    MeterBank::peakRms(l, r, n, pk, rm);
    meters_.setMaster(pk, rm, grDb, decay);
}

// Fire everything due at the current frame and shorten `chunk` so the next event lands on a chunk
// boundary (sample-accurate scheduling). Mirrors sf-engine's process() event loop.
void Engine::fireDueEvents(int& chunk) noexcept {
    const TransportMode mode = transport_.mode();
    const double now = transport_.positionTicks();
    const int64_t nowFrame = transport_.nowFrame();
    const double fpt = transport_.framesPerTick();
    const SchedParams& sp = cur_->schedParams();
    int64_t nearest = INT64_MAX;

    for (int ti = 0; ti < cur_->trackCount(); ++ti) {
        auto& sch = cur_->sched(ti);
        for (;;) {
            const auto fire = sch.peek(mode, now, sp, rng_);
            if (!fire) break;
            const int64_t until = transport_.framesUntilTick(*fire);
            if (until > 0) { nearest = std::min(nearest, until); break; }
            const auto ev = sch.takeDue(*fire);
            if (ev && ev->audible) {
                const uint32_t id = ++noteCounter_;
                cur_->noteOn(static_cast<uint16_t>(ti), ev->pitch, ev->vel, id);
                // The note ends after its duration (at least 20 ms), counted in frames from now.
                const double gateFrames = std::max(ev->durTicks * fpt, 0.02 * sr_);
                cur_->scheduleGate(nowFrame + static_cast<int64_t>(gateFrames), static_cast<uint16_t>(ti), id);
                cur_->fireDucks(ti, ev->pitch);
            }
        }
    }
    if (metroOn_) {  // a click on every beat of the meter, accented on bar starts
        const double bt = cur_->beatTicks(), bar = cur_->barTicks();
        if (transport_.framesUntilTick(nextMetroTick_) <= 0) {
            const double beat = std::round(nextMetroTick_ / bt);
            metro_.trigger(std::abs(std::fmod(beat * bt, bar)) < 1e-6);
            nextMetroTick_ += bt;
        }
        nearest = std::min(nearest, std::max<int64_t>(transport_.framesUntilTick(nextMetroTick_), 1));
    }
    cur_->fireGatesUpTo(nowFrame);
    if (cur_->nextGateFrame() != INT64_MAX) nearest = std::min(nearest, std::max<int64_t>(cur_->nextGateFrame() - nowFrame, 1));
    if (transport_.loopEnabled && mode == TransportMode::Arrangement)  // loop wrap is a boundary too
        nearest = std::min(nearest, std::max<int64_t>(transport_.framesUntilTick(transport_.loopEnd), 1));
    if (nearest != INT64_MAX) chunk = static_cast<int>(std::min<int64_t>(chunk, std::max<int64_t>(nearest, 1)));
}

// Render the next chunk of the internal fixed grid into the pending buffer.
void Engine::renderNextChunk() noexcept {
    const auto tStart = std::chrono::steady_clock::now();
    flushRetired();
    const bool hadOld = old_ != nullptr;
    pollIncomingGraph();
    const bool swapped = old_ != nullptr && !hadOld;
    drainCommands();
    drainLiveNotes();
    // Chunks end on the absolute kMaxBlock grid (or earlier at an event), whatever the callback size.
    int chunk = kMaxBlock - static_cast<int>(transport_.nowFrame() % kMaxBlock);
    if (cur_ && transport_.playing()) fireDueEvents(chunk);
    if (perfQCount_ > 0) {   // the oldest waiting tracked frame goes to the target's instrument before the chunk renders
        if (cur_ && trackTarget_ >= 0) cur_->performance(static_cast<uint16_t>(trackTarget_), perfQueue_[perfQHead_]);
        perfQHead_ = (perfQHead_ + 1) % 16;
        --perfQCount_;
    }
    {   // live input for monitoring through track effects: delayed by one grid chunk so it always exists
        const uint64_t have = inputFrames_.load(std::memory_order_relaxed);
        for (int j = 0; j < chunk; ++j) {
            const int64_t src = int64_t(monRead_) + j - kMaxBlock;
            const bool ok = src >= 0 && uint64_t(src) < have;
            monChunkL_[size_t(j)] = ok ? inRingL_[size_t(src) & (inRingL_.size() - 1)] : 0.0f;
            monChunkR_[size_t(j)] = ok ? inRingR_[size_t(src) & (inRingR_.size() - 1)] : 0.0f;
        }
        monRead_ += uint64_t(chunk);
        if (cur_) cur_->setMonitorInput(monChunkL_.data(), monChunkR_.data());
        if (old_) old_->setMonitorInput(monChunkL_.data(), monChunkR_.data());
    }
    if (swapped) raiseTo(diag_.maxSwapSetupNs, nsSince(tStart));
    const auto tRender = std::chrono::steady_clock::now();
    renderChunk(pendL_.data(), pendR_.data(), chunk);
    raiseTo(diag_.maxChunkNs, nsSince(tStart));
    if (swapped) raiseTo(diag_.maxSwapChunkNs, nsSince(tStart));
    (void)tRender;
    transport_.advance(chunk);
    if (transport_.wrapIfNeeded() && cur_) { cur_->relocateAll(transport_.mode(), transport_.positionTicks()); resetMetronome(); }
    pendLen_ = chunk;
    pendPos_ = 0;
}

void Engine::process(float* l, float* r, int n) noexcept {
    const ScopedFlushDenormals ftz;
    int done = 0;
    while (done < n) {
        if (pendPos_ >= pendLen_) renderNextChunk();
        const int take = std::min(n - done, pendLen_ - pendPos_);
        std::memcpy(l + done, pendL_.data() + pendPos_, size_t(take) * sizeof(float));
        std::memcpy(r + done, pendR_.data() + pendPos_, size_t(take) * sizeof(float));
        pendPos_ += take;
        done += take;
    }
    meters_.setTransport(transport_.positionTicks(), transport_.playing());
    if (cur_) {
        const int nt = std::min(cur_->trackCount(), kMaxMeterTracks);
        for (int i = 0; i < nt; ++i) {
            const auto sc = cur_->sched(i).launchedScene();
            meters_.setTrackScene(i, sc ? static_cast<int>(*sc) : -1, cur_->sched(i).anchor());
        }
        for (int i = nt; i < kMaxMeterTracks; ++i) meters_.setTrackScene(i, -1, 0.0);
    }
}

void Engine::liveNote(LiveKind kind, int pitch, float velocity) noexcept {
    while (liveLock_.test_and_set(std::memory_order_acquire)) {}   // producers only; held for one FIFO push
    liveQ_.push({kind, static_cast<uint8_t>(std::clamp(pitch, 0, 127)), velocity});
    liveLock_.clear(std::memory_order_release);
}

void Engine::liveRelease(int pitch) noexcept {
    const int tr = liveTrack_.load(std::memory_order_relaxed);
    if (liveId_[pitch] == 0) return;
    if (cur_ && tr >= 0 && tr < cur_->trackCount()) cur_->noteOff(static_cast<uint16_t>(tr), liveId_[pitch]);
    noteRecQ_.push({transport_.positionTicks(), liveId_[pitch], static_cast<uint16_t>(std::max(tr, 0)), static_cast<uint8_t>(pitch), 0, 0.0f});
    liveId_[pitch] = 0;
    sustained_[pitch] = false;
}

void Engine::drainLiveNotes() noexcept {
    LiveIn n;
    while (liveQ_.pop(n)) {
        const int tr = liveTrack_.load(std::memory_order_relaxed);
        switch (n.kind) {
            case LiveKind::NoteOn: {
                if (!cur_ || tr < 0 || tr >= cur_->trackCount()) break;
                const int p = n.pitch;
                if (liveId_[p]) liveRelease(p);               // the same key struck again: end the old note first
                const uint32_t id = 0x40000000u | (++liveCounter_ & 0x3FFFFFFFu);
                liveId_[p] = id;
                sustained_[p] = false;
                cur_->noteOn(static_cast<uint16_t>(tr), n.pitch, n.vel, id);
                cur_->fireDucks(tr, n.pitch);
                noteRecQ_.push({transport_.positionTicks(), id, static_cast<uint16_t>(tr), n.pitch, 1, n.vel});
                break;
            }
            case LiveKind::NoteOff:
                if (sustain_) sustained_[n.pitch] = true; else liveRelease(n.pitch);
                break;
            case LiveKind::SustainDown: sustain_ = true; break;
            case LiveKind::SustainUp:
                sustain_ = false;
                for (int p = 0; p < 128; ++p) if (sustained_[p]) liveRelease(p);
                break;
            case LiveKind::AllOff:
                sustain_ = false;
                for (int p = 0; p < 128; ++p) liveRelease(p);
                break;
        }
    }
}

Engine::CaptureInfo Engine::captureInfo() const noexcept {
    CaptureInfo i;
    i.active = captureActive_.load(std::memory_order_acquire);
    const uint64_t b = captureStartBits_.load(std::memory_order_relaxed);
    std::memcpy(&i.startTick, &b, 8);
    i.frames = captureFrames_.load(std::memory_order_relaxed);
    i.dropped = tap_.dropped();
    return i;
}

PerformanceFrame Engine::latestPerformance() const noexcept {
    PerformanceFrame f;
    f.f0Hz = perfF0_.load(std::memory_order_relaxed);
    f.loudnessDb = perfLd_.load(std::memory_order_relaxed);
    f.confidence = perfConf_.load(std::memory_order_relaxed);
    f.envelope = perfEnv_.load(std::memory_order_relaxed);
    return f;
}

void Engine::feedTracker(const float* inL, const float* inR, int n, uint64_t base) noexcept {
    const bool want = trackerOn_.load(std::memory_order_acquire) && inL;
    if (want && !trackerRunning_) { tracker_.reset(); trackerOrigin_ = base; trackerRunning_ = true; }
    if (!want) { trackerRunning_ = false; return; }
    dsp::TimedFrame tf[8];
    for (int off = 0; off < n;) {
        const int m = std::min<int>(n - off, int(monoScratch_.size()));
        for (int i = 0; i < m; ++i) monoScratch_[size_t(i)] = 0.5f * (inL[off + i] + inR[off + i]);
        const int got = tracker_.push(monoScratch_.data(), m, tf, 8);
        for (int g = 0; g < got; ++g) {
            const TimedPerf tp{tf[g].frame, double(trackerOrigin_) + tf[g].inputFrame};
            if (perfQCount_ == 16) { perfQHead_ = (perfQHead_ + 1) % 16; --perfQCount_; perfDropped_.fetch_add(1, std::memory_order_relaxed); }   // a burst beyond 16: drop the oldest
            perfQueue_[(perfQHead_ + perfQCount_++) % 16] = tp.frame;
            lastPerf_ = tp.frame;
            perfAgeFrames_ = 0;
            perfF0_.store(tp.frame.f0Hz, std::memory_order_relaxed);
            perfLd_.store(tp.frame.loudnessDb, std::memory_order_relaxed);
            perfConf_.store(tp.frame.confidence, std::memory_order_relaxed);
            perfEnv_.store(tp.frame.envelope, std::memory_order_relaxed);
            perfCount_.fetch_add(1, std::memory_order_relaxed);
            if (!perfFifo_.push(tp)) perfDropped_.fetch_add(1, std::memory_order_relaxed);
        }
        off += m;
    }
}

void Engine::processIO(const float* inL, const float* inR, float* l, float* r, int n) noexcept {
    if (!inL) inL = inR;
    if (!inR) inR = inL;
    const auto tIn = std::chrono::steady_clock::now();
    const uint64_t base = inputFrames_.load(std::memory_order_relaxed);
    // the input timeline: store it for monitoring (silence when there is no input), then analyse it
    {
        const size_t mask = inRingL_.size() - 1;
        for (int i = 0; i < n; ++i) {
            inRingL_[size_t(base + uint64_t(i)) & mask] = inL ? inL[i] : 0.0f;
            inRingR_[size_t(base + uint64_t(i)) & mask] = inR ? inR[i] : 0.0f;
        }
        inputFrames_.store(base + uint64_t(n), std::memory_order_release);
    }
    feedTracker(inL, inR, n, base);
    if (inL && polyOn_.load(std::memory_order_acquire)) {   // a mono copy for the polyphonic analysis thread
        for (int off = 0; off < n;) {
            const int m = std::min<int>(n - off, int(polyMix_.size()));
            for (int i = 0; i < m; ++i) polyMix_[size_t(i)] = 0.5f * (inL[off + i] + inR[off + i]);
            polyTap_.push(polyMix_.data(), polyMix_.data(), size_t(m));
            off += m;
        }
    }
    raiseTo(diag_.maxInputNs, nsSince(tIn));
    process(l, r, n);
    if (!inL) inputPeak_.store(0.0f, std::memory_order_relaxed);
    if (inL) {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) pk = std::max({pk, std::abs(inL[i]), std::abs(inR[i])});
        const float prev = inputPeak_.load(std::memory_order_relaxed);
        inputPeak_.store(std::max(pk, prev * MeterBank::decay(n, sr_)), std::memory_order_relaxed);
    }

    const bool want = captureReq_.load(std::memory_order_acquire);
    if (want && !capturing_ && transport_.playing() && (inL || !captureAudio_.load(std::memory_order_relaxed))) {
        // Absolute stream frame of the first sample of THIS callback: the frames rendered so far minus what
        // still waits in the pending buffer, minus this callback. Playback began at the anchor frame, which
        // may fall inside this callback (the first `skip` frames are before it) and may be before tick 0
        // (a count-in), so the start tick is read off the transport's own anchor.
        const int64_t cbFrame = transport_.nowFrame() - int64_t(pendLen_ - pendPos_) - n;
        const int64_t from = std::max(cbFrame, transport_.anchorFrame());
        const int skip = int(from - cbFrame);
        const double start = transport_.tickAtFrame(from);
        std::uint64_t b; std::memcpy(&b, &start, 8);
        captureStartBits_.store(b, std::memory_order_relaxed);
        captureStartInput_.store(base + uint64_t(skip), std::memory_order_relaxed);
        captureFrames_.store(0, std::memory_order_relaxed);
        tap_.clearDropped();
        capturing_ = true;
        captureActive_.store(true, std::memory_order_release);
        if (skip < n) {
            if (captureAudio_.load(std::memory_order_relaxed)) tap_.push(inL + skip, inR + skip, size_t(n - skip));
            captureFrames_.fetch_add(uint64_t(n - skip), std::memory_order_relaxed);
        }
        return;
    }
    if (capturing_) {
        if (!want || (!inL && captureAudio_.load(std::memory_order_relaxed))) {
            capturing_ = false;
            captureActive_.store(false, std::memory_order_release);
        } else {
            if (captureAudio_.load(std::memory_order_relaxed)) tap_.push(inL, inR, size_t(n));
            captureFrames_.fetch_add(uint64_t(n), std::memory_order_relaxed);
        }
    }
}

}  // namespace ddaw::engine
