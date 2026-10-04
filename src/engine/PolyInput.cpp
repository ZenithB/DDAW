#include "engine/PolyInput.h"

#include <chrono>
#include <cmath>

#include "dsp/StreamResampler.h"

namespace ddaw::engine {

constexpr int kHop = 512;   // 32 ms at 16 kHz

void PolyInput::start(const dsp::PolyPitchConfig& cfg) {
    if (running_.exchange(true)) return;
    stop_ = false;
    const double ratio = engine_.sampleRateHz() / 16000.0;
    latency_ = int(std::ceil((double(cfg.window) / 2.0 + double(kHop)) * ratio)) + 16 * int(std::ceil(ratio));   // measured: about 80 ms for a chord at -20 dBFS
    engine_.setPolyLatencyFrames(latency_);
    // discard whatever the ring holds from an earlier session
    std::vector<float> junk(4096), junk2(4096);
    while (engine_.polyTap().pop(junk.data(), junk2.data(), junk.size()) > 0) {}
    engine_.setPolyInputEnabled(true);
    thread_ = std::thread([this, cfg] { loop(cfg); });
}

void PolyInput::stop() {
    if (!running_.load()) return;
    engine_.setPolyInputEnabled(false);
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void PolyInput::loop(dsp::PolyPitchConfig cfg) {
    dsp::PolyPitch pitch;
    pitch.prepare(cfg);
    dsp::PolyNoteTracker tracker;
    tracker.configure(2, 3);
    dsp::StreamResampler rs;
    rs.prepare(engine_.sampleRateHz(), 16000.0);
    std::vector<float> hostL(4096), hostR(4096), win(size_t(cfg.window), 0.0f), ring(size_t(cfg.window) * 2, 0.0f);
    size_t written = 0, lastAnalysed = 0;
    std::vector<dsp::PolyNoteEvent> events;
    auto flush = [&](bool all) {
        for (int p = 0; p < 128; ++p) if (tracker.active(p) && all) engine_.liveNote(Engine::LiveKind::NoteOff, p);
    };
    while (!stop_.load(std::memory_order_acquire)) {
        const size_t got = engine_.polyTap().pop(hostL.data(), hostR.data(), hostL.size());
        if (got == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); continue; }
        rs.process(hostL.data(), int(got), [&](float y, double) {
            ring[written % ring.size()] = y;
            ++written;
            if (written >= size_t(cfg.window) && written - lastAnalysed >= size_t(kHop)) {
                lastAnalysed = written;
                for (size_t i = 0; i < size_t(cfg.window); ++i) win[i] = ring[(written - size_t(cfg.window) + i) % ring.size()];
                events.clear();
                tracker.update(pitch.estimate(win.data()), events);
                analysed_.fetch_add(1, std::memory_order_relaxed);
                for (const auto& ev : events) {
                    if (ev.on) { engine_.liveNote(Engine::LiveKind::NoteOn, ev.pitch, ev.velocity); started_.fetch_add(1, std::memory_order_relaxed); }
                    else engine_.liveNote(Engine::LiveKind::NoteOff, ev.pitch);
                }
            }
        });
    }
    flush(true);
}

}  // namespace ddaw::engine
