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
#include "dsp/Svf.h"
#include "engine/AudioClips.h"
#include "engine/Meters.h"
#include "engine/Metronome.h"
#include "engine/Modulation.h"
#include "engine/Scheduler.h"

namespace ddaw::engine {

constexpr int kMaxMeterTrackSlots = kMaxMeterTracks;

// Audio-rate modulation inputs of one device (B4): a buffer per A-rate parameter that has a route, handed to the
// device as ModInputs. Sized at build; the pointers are refreshed every chunk and are null where nothing modulates.
struct AModBufs {
    std::vector<const float*> ptrs;           // by A-rate ordinal
    std::vector<std::vector<float>> bufs;     // by A-rate ordinal; kMaxBlock floats where a route exists
    void init(size_t numAudioRate) { ptrs.assign(numAudioRate, nullptr); bufs.assign(numAudioRate, {}); }
    void ensure(size_t ordinal) { if (bufs[ordinal].empty()) bufs[ordinal].assign(size_t(kMaxBlock), 0.0f); }
    ModInputs inputs() const noexcept { return ModInputs{std::span<const float* const>(ptrs.data(), ptrs.size())}; }
};

// One audio-rate modulation route, resolved at build (ProjectSpec ARateSpec). Rendered into its target device's
// buffer once per chunk, after the source's own track has rendered.
struct AudioRoute {
    enum class Kind : uint8_t { Osc, Track };
    Kind kind = Kind::Osc;
    int shape = 0;
    bool follow = false;
    float hz = 220.0f, depth = 0.5f;
    float halfRange = 1.0f;          // (max - min) / 2 of the target parameter
    double phase = 0.0;              // Osc
    int src = -1;                    // Track: the source strip
    float atk = 0.0f, rel = 0.0f, env = 0.0f;   // follower coefficients and state
    int specIndex = 0;               // position in the track's ARateSpec list (live edits address routes by it)
    int dstTrack = 0;
    int dstFx = -1;                  // -1: the track's instrument, else the effect index
    size_t ordinal = 0;              // index among the target's A-rate parameters
};

// Delay compensation on one route (see Graph::finalize): the strip's signal is delayed by `samples` on its way to that destination.
struct PdcLine {
    dsp::DelayLine l, r;
    int samples = 0;
    void prepare(int d) { samples = d; if (d > 0) { l.prepare(d + 1); r.prepare(d + 1); } }   // read(d + 1) is d samples behind the sample just written
    void run(const float* inL, const float* inR, float* outL, float* outR, int n) noexcept {
        for (int k = 0; k < n; ++k) { l.write(inL[k]); r.write(inR[k]); outL[k] = l.read(samples + 1); outR[k] = r.read(samples + 1); }
    }
};

struct FxSlot {
    std::unique_ptr<EffectDevice> dev;
    dsp::Smoother out;  // linear gain from the device's `out` (dB)
    AModBufs mod;       // audio-rate inputs (empty when nothing is routed to this device)
    // Sidechain: the device's detector runs on another track's tap (after its effects, before pan, fader and mute: a muted track
    // still keys), high-passed at keyHpfHz when that is above 0. keySrc < 0: no key.
    int keySrc = -1;
    float keyHpfHz = 0.0f;
    dsp::Svf keyHp{dsp::SvfMode::Highpass};
    std::vector<float> keyBuf;
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
    AModBufs instMod;                        // audio-rate inputs of the instrument
    std::vector<uint32_t> aroutes;           // indices (into the graph's route list) of routes that target this track
    std::vector<float> tap;                  // mono mix after the effects, before pan/fader/mute; for routes that use this track as a source
    bool tapped = false;
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
    // Delay compensation per route (finalize): the output route (master or a bus), the A and B sends, and each bus send, so that
    // every path into a summing point (a bus input, the master) arrives with the same delay, send paths included.
    PdcLine pdcOut, pdcA, pdcB;
    std::vector<PdcLine> pdcSend;   // parallel to busSends

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
    PdcLine pdcOut;                 // return -> master
};

// A sidechain route: a note fired on `src` triggers the duck at (track, fx slot) or on the master chain.
struct DuckRoute {
    int src = 0;
    int track = -1;  // -1: master chain
    int fx = 0;
    int pitch = -1;  // -1: any; otherwise the pad index (pitch % 8) that triggers it
};

// A note's expression curves (MPE), as scheduled: points are (ticks after the note's start, value), by dimension
// 0 slide, 1 pressure, 2 pitch bend in semitones. Built with the graph; read-only on the audio thread.
struct ExprSeq {
    struct Pt { float t, v; };
    std::array<std::vector<Pt>, 3> dim;
    uint64_t uid = 0;   // the project note these belong to: a swap re-finds a sounding note's curves in the new graph by it
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
    // Expression curves of scheduled notes (B5). Returns the index NoteEv::expr refers to.
    int addExprSeq(ExprSeq s) { exprSeqs_.push_back(std::move(s)); return int(exprSeqs_.size()) - 1; }
    // Audio-rate routes (B4). The route's target buffers must already be sized (AModBufs::init / ensure).
    // Sources must render before their targets: the builder orders the tracks.
    uint32_t addAudioRoute(const AudioRoute& r);
    // Key a track's (dstTrack >= 0) or the master chain's (-1) effect `fx` from track `src`'s tap; `hpfHz` > 0 high-passes the key.
    void addKey(int dstTrack, size_t fx, int src, float hpfHz);
    size_t audioRouteCount() const noexcept { return aroutes_.size(); }
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
    void noteExpression(uint16_t track, uint32_t noteId, int dimension, float value) noexcept { if (track < tracks_.size() && tracks_[track].inst) tracks_[track].inst->noteExpression(noteId, dimension, value); }
    // Live input monitoring: `l`/`r` (n frames, valid for the next process() call, or null) are mixed
    // into every track that has monitoring on, ahead of its effects.
    void setMonitorInput(const float* l, const float* r) noexcept { monL_ = l; monR_ = r; }
    void setTrackMonitor(uint16_t track, bool on) noexcept { if (track < tracks_.size()) tracks_[track].monitorIn = on; }
    bool trackMonitor(int track) const noexcept { return track >= 0 && size_t(track) < tracks_.size() && tracks_[size_t(track)].monitorIn; }
    // Graph swap: keep monitoring switched on for the tracks that had it.
    void inheritMonitors(const Graph& old) noexcept { for (size_t i = 0; i < tracks_.size() && i < old.tracks_.size(); ++i) tracks_[i].monitorIn = old.tracks_[i].monitorIn; }
    void allNotesOff() noexcept;
    // A scheduled note with expression curves has just started at transport tick `startTick`: play them while it lasts
    // (`durTicks`). updateExpression(now) sends each playing note's curve values for the chunk about to render.
    void startExpression(uint16_t track, uint32_t noteId, double startTick, double durTicks, int seq) noexcept;
    void updateExpression(double nowTick) noexcept;
    void clearExpression() noexcept { nPlayers_ = 0; }
    int exprSeqCount() const noexcept { return int(exprSeqs_.size()); }
    int exprPlayerCount() const noexcept { return int(nPlayers_); }

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
    std::vector<AudioRoute> aroutes_;
    double invSr_ = 1.0 / 44100.0;
    void renderRoutes(TrackStrip& t, int n) noexcept;
    ModInputs inputsFor(FxSlot& f, int n) noexcept;   // the audio-rate buffers plus the filtered key
    dsp::Smoother masterGain_;
    SchedParams sched_;
    double launchQ_ = 1, loopStart_ = 0, loopEnd_ = 4 * 384;
    int tsTop_ = 4, tsBottom_ = 4;
    bool loopOn_ = false;
    std::vector<float> mixL_, mixR_, tmpL_, tmpR_, pdcL_, pdcR_, sdL_, sdR_;   // pdc*: the output route delayed; sd*: a send or return delayed
    std::array<Active, kMaxActive> active_{};
    size_t nActive_ = 0;
    struct ExprPlayer { uint16_t track; uint32_t noteId; double start, end; int seq; std::array<float, 3> last; std::array<uint32_t, 3> idx; uint64_t uid; };
    int findExprSeq(uint64_t uid) const noexcept;   // index of the sequence of project note `uid` (-1: none)
    std::vector<std::pair<uint64_t, int>> exprByUid_;   // sorted by uid; built by finalize()
    static constexpr size_t kMaxExprPlayers = 32;
    std::vector<ExprSeq> exprSeqs_;
    std::array<ExprPlayer, kMaxExprPlayers> players_{};
    size_t nPlayers_ = 0;
    std::array<Gate, kMaxGates> gates_{};
    size_t nGates_ = 0;
};

}  // namespace ddaw::engine
