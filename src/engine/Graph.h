#pragma once
// The audio graph (ARCH 10): tracks and buses, sends, returns, master effects. One class serves live
// playback and offline render. Port of sf-engine graph.rs (render_chunk), mirroring synthyy's
// engine-tone buildTrack/wireFx/rewireBuses semantics:
//
//   instrument -> inst out gain -> [fx -> out gain ...] -> pan -> fader*mute -> master
//                                                                 '-> A / B / per-bus send taps
//   Buses render after tracks in topological order. A bus->bus edge that would close a cycle is
//   routed through a short delay (a back edge) so the loop regenerates instead of being dropped; the
//   built-in Feedback bus (send marker F) does the same with its own output.
//   Master: sum -> master gain -> master fx chain (the Engine then applies the limiter).
//
// Everything is allocated at build time. process() performs no allocation, locking or throwing.
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/Cmd.h"
#include "core/Constants.h"
#include "core/Device.h"
#include "dsp/DelayLine.h"
#include "dsp/Smoother.h"
#include "engine/AudioClips.h"
#include "engine/Meters.h"
#include "engine/Metronome.h"
#include "engine/Modulation.h"
#include "engine/Scheduler.h"

namespace ddaw::engine {

constexpr int kMaxMeterTrackSlots = kMaxMeterTracks;

struct FxSlot {
    std::unique_ptr<EffectDevice> dev;
    dsp::Smoother out;  // linear gain from the device's `out` (dB)
};

struct TrackStrip {
    enum class Out { Master, Bus, Delayed };
    struct BusSend { int target; dsp::Smoother level; };
    struct Feedback {
        dsp::DelayLine dl, dr;
        dsp::Smoother gain;
        int delaySamples = 0;
    };

    std::string id;
    bool isBus = false;
    bool audible = true;
    bool monitorIn = false;                  // the live input is mixed in ahead of the effects
    std::unique_ptr<InstrumentDevice> inst;  // null on buses
    dsp::Smoother instOut;
    std::vector<FxSlot> fx;
    dsp::Smoother pan, fader, muteGain, sendA, sendB;
    std::vector<BusSend> busSends;
    Out outKind = Out::Master;
    int outTarget = -1;  // bus index (Out::Bus) or back-edge index (Out::Delayed)
    std::unique_ptr<Feedback> fb;
    TrackSched sched;
    AudioPlayer audio;              // audio clips (audio tracks)
    std::vector<float> bufL, bufR;  // input accumulation (buses, and the inputs of any track routed into one)
    dsp::DelayLine pdcL, pdcR;      // delay compensation at the strip output
    int pdcSamples = 0;

    int latencySamples() const;
};

// A bus->bus edge that closes a cycle. The target renders BEFORE the source: the source writes into
// the delay each block and the target reads what was written `delaySamples` ago.
struct BackEdge {
    int target = 0;
    dsp::DelayLine dl, dr;
    int delaySamples = 0;
};

// A built-in effect return, used when the project has no A/B send buses (`returns` in the project).
struct ReturnNode {
    std::unique_ptr<EffectDevice> dev;
    dsp::Smoother gain;
    std::vector<float> bufL, bufR;
};

// A sidechain route: a note fired on `src` triggers the duck at (track, fx slot) or on the master chain.
struct DuckRoute {
    int src = 0;
    int track = -1;  // -1: master chain
    int fx = 0;
    int pitch = -1;  // -1: any; otherwise the pad index (pitch % 8) that triggers it
};

class Graph {
public:
    Graph(uint32_t epoch, double sampleRate);

    uint32_t epoch() const noexcept { return epoch_; }
    double   sampleRate() const noexcept { return sr_; }
    int      trackCount() const noexcept { return static_cast<int>(tracks_.size()); }
    // Longest path through the graph (track chain + the bus chain it feeds + master effects); every
    // shorter path is delayed to match (ARCH 10). Sends are not compensated.
    int      latencySamples() const noexcept { return latency_; }

    // ---- build side (builder thread, may allocate) ----
    TrackStrip& addTrack();
    TrackStrip& track(int i) { return tracks_[static_cast<size_t>(i)]; }
    FxSlot& addMasterFx();
    size_t masterFxCount() const noexcept { return masterFx_.size(); }
    ReturnNode& addReturn();
    BackEdge& addBackEdge(int target, int delaySamples);
    void setOrder(std::vector<int> order) { order_ = std::move(order); }
    void setSendBuses(int a, int b) { sendABus_ = a; sendBBus_ = b; }
    void addDuck(const DuckRoute& d) { ducks_.push_back(d); }
    void setMasterGainDb(float db);
    void setTiming(const SchedParams& sp, double launchQ, int tsTop, int tsBottom) {
        sched_ = sp; launchQ_ = launchQ; tsTop_ = tsTop; tsBottom_ = tsBottom;
    }
    void setLoop(bool on, double start, double end) { loopOn_ = on; loopStart_ = start; loopEnd_ = end; }
    void setModulation(std::unique_ptr<ModState> m) { mod_ = std::move(m); }
    void finalize();  // computes delay compensation; call once after the last add*

    // ---- audio side (real-time safe) ----
    // Writes the master-gained, master-fx-processed mix to l/r (n <= kMaxBlock). The Engine limits it.
    void process(float* l, float* r, int n, const ProcessContext& ctx, MeterBank* meters, float meterDecay,
                 Metronome* metro = nullptr) noexcept;
    // Builder thread, before the graph is posted: render a few silent chunks so every device's memory is touched
    // and its code has run once, then reset all devices to their initial state. A graph that goes live "cold"
    // pays for first touch on the audio thread, in the very chunk that also renders the graph it replaces.
    void warmUp(int chunks = 6);
#ifdef DDAW_GRAPH_PROFILE
    // Development builds only: nanoseconds spent per track (instrument, effects) - high-water mark per chunk.
    uint64_t profInstNs[128] = {}, profFxNs[128] = {}, profInstMax[128] = {}, profFxMax[128] = {};
#endif
    void setParam(const ParamAddr& a, float value) noexcept;
    void noteOn(uint16_t track, uint8_t pitch, float vel, uint32_t noteId) noexcept;
    void noteOff(uint16_t track, uint32_t noteId) noexcept;
    void performance(uint16_t track, const PerformanceFrame& f) noexcept;
    // Live input monitoring: `l`/`r` (n frames, valid for the next process() call, or null) are mixed
    // into every track that has monitoring on, ahead of its effects.
    void setMonitorInput(const float* l, const float* r) noexcept { monL_ = l; monR_ = r; }
    void setTrackMonitor(uint16_t track, bool on) noexcept { if (track < tracks_.size()) tracks_[track].monitorIn = on; }
    bool trackMonitor(int track) const noexcept { return track >= 0 && size_t(track) < tracks_.size() && tracks_[size_t(track)].monitorIn; }
    // Graph swap: keep monitoring switched on for the tracks that had it.
    void inheritMonitors(const Graph& old) noexcept { for (size_t i = 0; i < tracks_.size() && i < old.tracks_.size(); ++i) tracks_[i].monitorIn = old.tracks_[i].monitorIn; }
    void allNotesOff() noexcept;

    // ---- scheduling (driven by the Engine) ----
    const SchedParams& schedParams() const noexcept { return sched_; }
    TrackSched& sched(int track) { return tracks_[static_cast<size_t>(track)].sched; }
    bool loopOn() const noexcept { return loopOn_; }
    double loopStart() const noexcept { return loopStart_; }
    double loopEnd() const noexcept { return loopEnd_; }
    double barTicks() const noexcept { return kPpq * 4.0 * tsTop_ / tsBottom_; }
    double beatTicks() const noexcept { return kPpq * 4.0 / tsBottom_; }
    // Next clip-launch boundary in ticks (engine-tone nextBoundaryTicks).
    double nextBoundaryTicks(double curTicks) const noexcept;
    void relocateAll(TransportMode mode, double now) noexcept;
    // Graph swap: re-trigger the notes that are sounding in `old` (same track index, same ids, same
    // scheduled end) so a sustained note does not fall silent when the graph is rebuilt under it.
    void inheritNotes(const Graph& old) noexcept;
    // Evaluate automation, LFOs and macros at `now` and push the values through setParam.
    void applyModulation(double now, bool playing, TransportMode mode, int frames, const PerformanceFrame* perf = nullptr) noexcept {
        if (mod_) mod_->apply(*this, now, playing, mode, frames, perf);
    }
    // A note fired on `src`: kick every duck listening to that track / pad.
    void fireDucks(int src, uint8_t pitch) noexcept;

    // Gate-off queue: a scheduled note ends at an absolute transport frame.
    bool scheduleGate(int64_t frame, uint16_t track, uint32_t noteId) noexcept;
    int64_t nextGateFrame() const noexcept { return nGates_ ? gates_[0].frame : INT64_MAX; }
    void fireGatesUpTo(int64_t frame) noexcept;
    void clearGates() noexcept { nGates_ = 0; }

private:
    const float* monL_ = nullptr;
    const float* monR_ = nullptr;
    struct Gate { int64_t frame; uint16_t track; uint32_t noteId; };
    static constexpr size_t kMaxActive = 512, kMaxGates = 2048;
    struct Active { uint16_t track; uint32_t noteId; uint8_t pitch; float vel; };

    uint32_t epoch_;
    double   sr_;
    int      latency_ = 0;
    std::vector<TrackStrip> tracks_;
    std::vector<int> order_;                  // render order: tracks in document order, then buses topologically
    std::vector<BackEdge> backEdges_;
    std::vector<ReturnNode> returns_;
    std::vector<FxSlot> masterFx_;
    int sendABus_ = -1, sendBBus_ = -1;
    std::vector<DuckRoute> ducks_;
    std::unique_ptr<ModState> mod_;
    dsp::Smoother masterGain_;
    SchedParams sched_;
    double launchQ_ = 1, loopStart_ = 0, loopEnd_ = 4 * 384;
    int tsTop_ = 4, tsBottom_ = 4;
    bool loopOn_ = false;
    std::vector<float> mixL_, mixR_, tmpL_, tmpR_;
    std::array<Active, kMaxActive> active_{};
    size_t nActive_ = 0;
    std::array<Gate, kMaxGates> gates_{};
    size_t nGates_ = 0;
};

}  // namespace ddaw::engine
