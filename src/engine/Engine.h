#pragma once
// The engine: everything the audio callback runs (ARCH 1, 7, 9, 10).
//
//   audio thread : Engine::process()  - drains commands, swaps graphs, splits blocks at scheduled
//                                       events, renders, applies test tone, limiter and meters
//   builder      : postGraph() / takeRetired() hand whole graphs across by FIFO
//   any producer : commands() - the SPSC command lane (single producer at a time)
//
// Nothing on the audio side allocates, locks or throws. Graphs are only ever deleted by the thread
// that calls takeRetired().
#include <atomic>
#include <memory>
#include <vector>

#include "core/Cmd.h"
#include "dsp/CompKernel.h"
#include "dsp/Limiter.h"
#include "dsp/Smoother.h"
#include "dsp/Tracker.h"
#include "engine/Graph.h"
#include "engine/InputTap.h"
#include "engine/Meters.h"
#include "engine/Metronome.h"
#include "engine/NoteEvent.h"
#include "engine/Transport.h"

namespace ddaw::engine {

// Which master limiter runs after the mix.
//   Brickwall : lookahead limiter, never exceeds the ceiling (the default for DDAW projects)
//   ToneCompat: the browser's Tone.Limiter(-1): a soft-knee compressor kernel with a 6 ms pre-delay.
//               Needed to reproduce synthyy's browser goldens, which were rendered through it.
//   Bypass    : no limiting (inspection only)
struct MasterLimiterConfig {
    enum class Mode { Brickwall, ToneCompat, Bypass } mode = Mode::Brickwall;
    float ceilingDb = -1.0f;  // Brickwall only
};

// Frames over which a graph swap crossfades (the old graph renders only this much of the swap chunk).
constexpr int kSwapFade = 64;

struct EngineDiagnostics {
    std::atomic<uint32_t> staleCommandsDropped{0};  // epoch did not match the live graph
    std::atomic<uint32_t> graphSwaps{0};
    std::atomic<uint32_t> retireBacklog{0};         // retired graphs waiting for FIFO space
    // Timing of the audio thread's own work (nanoseconds, high-water marks since reset; a clock read per chunk).
    std::atomic<uint64_t> maxChunkNs{0};            // one rendered chunk (a callback renders at most one)
    std::atomic<uint64_t> maxSwapChunkNs{0};        // a chunk in which a graph swapped in (renders old and new)
    std::atomic<uint64_t> maxSwapSetupNs{0};        // the swap bookkeeping before that chunk renders
    std::atomic<uint64_t> maxOldRenderNs{0};        // the retiring graph's share of a swap chunk
    std::atomic<uint64_t> maxInputNs{0};            // processIO's input side: ring copies, tracker, poly tap
    void resetTimings() { maxChunkNs = 0; maxSwapChunkNs = 0; maxSwapSetupNs = 0; maxOldRenderNs = 0; maxInputNs = 0; }
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Non-real-time. Call before audio starts (or again to change the sample rate, which also
    // invalidates any live graph: rebuild and post a new one).
    void prepare(double sampleRate, const MasterLimiterConfig& master = {});
    void prepare(double sampleRate, float brickwallCeilingDb) { prepare(sampleRate, MasterLimiterConfig{MasterLimiterConfig::Mode::Brickwall, brickwallCeilingDb}); }
    // Non-real-time and only while audio is not running: install the first graph directly.
    void setInitialGraph(std::unique_ptr<Graph> g);

    // Builder thread.
    bool postGraph(std::unique_ptr<Graph>& g);    // false when the lane is full (g keeps ownership)
    std::unique_ptr<Graph> takeRetired();         // nullptr when none
    uint32_t liveEpoch() const noexcept { return liveEpoch_.load(std::memory_order_acquire); }

    CommandFifo& commands() noexcept { return commands_; }
#ifdef DDAW_GRAPH_PROFILE
    Graph* profileGraph() noexcept { return cur_; }   // development builds: read after the audio thread has stopped
#endif
    const MeterBank& meters() const noexcept { return meters_; }
    EngineDiagnostics& diagnostics() noexcept { return diag_; }

    // Offline renders play through: the arrangement loop region is ignored (synthyy's loop_override_off).
    void setLoopOverrideOff(bool off) noexcept { loopOverrideOff_ = off; }
    // An offline render is not paced by a clock: devices that run work on other threads (the DDSP instrument)
    // wait for it instead of risking an underrun, so the result is deterministic.
    void setOffline(bool on) noexcept { offline_ = on; }

    // Live graph latency plus the master limiter's latency. Offline render trims this many leading samples.
    int latencySamples() const noexcept { return latency_.load(std::memory_order_relaxed); }
    double sampleRateHz() const noexcept { return sr_; }

    // Audio thread. Any n >= 0; l and r must be distinct buffers.
    //
    // The engine renders internally on a fixed grid: chunks end at multiples of kMaxBlock of the
    // absolute frame counter, or earlier at a scheduled event, never at a callback boundary. Samples
    // are handed out of an internal buffer, so the output is bit-identical whatever the callback size
    // (live at 32 or 64 frames, offline at 128). Commands, graph swaps and modulation take effect at
    // chunk starts, so a control change lands within one chunk (at most kMaxBlock frames, 2.7 ms).
    void process(float* l, float* r, int n) noexcept;

    // Audio thread, with the device input (either pointer may be null: no input). Renders like process(),
    // then monitors the input, meters it, and feeds the capture ring while a take is running.
    void processIO(const float* inL, const float* inR, float* l, float* r, int n) noexcept;

    // ---- input capture (control side) ----
    // Ask the audio thread to start / stop capturing. A take starts at the first callback in which the
    // transport is playing; captureInfo().startTick is then the timeline position of the first captured
    // frame (exact for a constant tempo), and `skip` leading frames were dropped when playback began
    // mid-callback. Stopping is acknowledged by captureInfo().active turning false.
    // `audio` false: only note where the take starts (performance-curve recording needs no sample data).
    void requestCapture(bool on, bool audio = true) noexcept { captureAudio_.store(audio, std::memory_order_relaxed); captureReq_.store(on, std::memory_order_release); }
    struct CaptureInfo { bool active = false; double startTick = 0; uint64_t frames = 0; uint64_t dropped = 0; };
    CaptureInfo captureInfo() const noexcept;
    InputTap& inputTap() noexcept { return tap_; }
    // Engine-wide count of input frames seen by processIO (the input timeline), and the index of the first
    // frame of the running take on it.
    uint64_t inputFrames() const noexcept { return inputFrames_.load(std::memory_order_relaxed); }
    uint64_t captureStartInputFrame() const noexcept { return captureStartInput_.load(std::memory_order_relaxed); }

    // ---- live note input (MIDI, computer keyboard) ----
    // Any thread may call these (a producer lock serialises them; the audio side never waits). Notes play the
    // instrument of the live target track (a LiveTrack command) at the next chunk boundary. Polyphony is the
    // instrument's own. The sustain pedal holds released notes until it is lifted.
    enum class LiveKind : uint8_t { NoteOn, NoteOff, SustainDown, SustainUp, AllOff, Expression, BendAll };
    void liveNote(LiveKind kind, int pitch = 0, float velocity = 0.8f) noexcept;
    // Per-note expression (MPE) for the live note on key `pitch`: dimension 0 slide (0..1), 1 pressure (0..1), 2 pitch bend in
    // SEMITONES (signed). Remembered per key, so a controller that sends it before the note-on (MPE does) still shapes the
    // note's start; a note that is not sounding just keeps the value for its next strike.
    void liveExpression(int pitch, int dimension, float value) noexcept;
    // Pitch bend for every live note on the target track (a bend wheel, or an MPE master channel), in semitones; it adds to a
    // note's own bend and applies to notes struck later.
    void liveBend(float semitones) noexcept;
    // Every live note the engine played, with its place on the timeline, for recording. Single consumer.
    // on: 1 note on, 0 note off, 2 an expression value (`dim`: 0 slide, 1 pressure, 2 bend in semitones; `velocity` carries the
    // value). Expression is echoed when it changes by a clear amount, and the values a note starts with are echoed with its note-on.
    struct NoteRecord { double tick; uint32_t id; uint16_t track; uint8_t pitch; uint8_t on; float velocity; uint8_t dim = 0; };
    bool popNoteRecord(NoteRecord& out) noexcept { return noteRecQ_.pop(out); }
    int liveTrack() const noexcept { return liveTrack_.load(std::memory_order_relaxed); }

    // ---- polyphonic audio input (several notes from the live signal) ----
    // While on, processIO also feeds a mono copy of the input to a ring that PolyInput (a service thread)
    // analyses. The notes it finds arrive through liveNote(), like a keyboard's.
    void setPolyInputEnabled(bool on) noexcept { polyOn_.store(on, std::memory_order_release); }
    bool polyInputEnabled() const noexcept { return polyOn_.load(std::memory_order_acquire); }
    InputTap& polyTap() noexcept { return polyTap_; }
    // How late a detected note is (host frames from the sound to the note-on): set by the service, read by recording.
    void setPolyLatencyFrames(int f) noexcept { polyLatency_.store(f, std::memory_order_relaxed); }
    int polyLatencyFrames() const noexcept { return polyLatency_.load(std::memory_order_relaxed); }

    // ---- performance tracking (B2) ----
    // Run the tracker on the input (pitch, loudness, envelope, confidence at 250 frames per second). The
    // latest frame is delivered to the instrument of the tracking target track (set with a Tracking
    // command) before each chunk renders, and every frame is queued for recording and display.
    void setTrackerEnabled(bool on) noexcept { trackerOn_.store(on, std::memory_order_release); }
    bool trackerEnabled() const noexcept { return trackerOn_.load(std::memory_order_acquire); }
    // Frames in order of their moment, with `inputFrame` on the engine's input timeline. Single consumer.
    struct TimedPerf { PerformanceFrame frame; double inputFrame; };
    bool popPerformance(TimedPerf& out) noexcept { return perfFifo_.pop(out); }
    PerformanceFrame latestPerformance() const noexcept;
    uint64_t performanceFrameCount() const noexcept { return perfCount_.load(std::memory_order_relaxed); }
    uint64_t performanceFramesDropped() const noexcept { return perfDropped_.load(std::memory_order_relaxed); }
    int trackerLatencySamples() const noexcept { return trackerLatency_.load(std::memory_order_relaxed); }
    // Delay of the live-monitoring path through a track's effects (the fixed render grid).
    static constexpr int monitorLatencyFrames() noexcept { return kMaxBlock; }
    float inputPeak() const noexcept { return inputPeak_.load(std::memory_order_relaxed); }

private:
    int masterLatency() const noexcept;
    void drainCommands() noexcept;
    void pollIncomingGraph() noexcept;
    void retire(Graph* g) noexcept;
    void flushRetired() noexcept;
    void renderChunk(float* l, float* r, int n) noexcept;
    void fireDueEvents(int& chunk) noexcept;
    void renderNextChunk() noexcept;

    double sr_ = 48000.0;
    Transport transport_;
    Graph* cur_ = nullptr;   // audio thread owns after setInitialGraph / swap
    Graph* old_ = nullptr;   // fading out during the swap chunk
    MasterLimiterConfig master_;
    dsp::Limiter limiter_;
    dsp::CompKernel toneLimiter_;
    MeterBank meters_;
    EngineDiagnostics diag_;

    // test tone (engine-level, diagnostic)
    bool   toneOn_ = false;
    double tonePhase_ = 0.0, toneInc_ = 0.0;
    dsp::Smoother toneAmp_;

    CommandFifo commands_;
    SpscFifo<Graph*, 8> incoming_, retired_;
    Graph* pendingRetire_[8] = {};
    int    nPendingRetire_ = 0;
    XorShift rng_{0x5F3F9A1B2C4D6E7Full};  // reseeded at every transport play: swing/humanise/probability repeat per run
    uint32_t noteCounter_ = 0;
    bool loopOverrideOff_ = false;
    bool offline_ = false;
    Metronome metro_;
    bool metroOn_ = false;
    double nextMetroTick_ = 0;
    void resetMetronome() noexcept;
    std::atomic<uint32_t> liveEpoch_{0};
    InputTap tap_, polyTap_;
    std::atomic<bool> polyOn_{false};
    std::atomic<int> polyLatency_{0};
    std::vector<float> polyMix_;
    std::atomic<bool> captureReq_{false}, captureActive_{false}, captureAudio_{true};
    std::atomic<uint64_t> captureStartBits_{0}, captureFrames_{0};
    std::atomic<float> inputPeak_{0.0f};
    bool capturing_ = false;
    // live notes
    struct LiveIn { LiveKind kind; uint8_t pitch; float vel; uint8_t dim; };
    SpscFifo<LiveIn, 1024> liveQ_;
    std::atomic_flag liveLock_ = ATOMIC_FLAG_INIT;
    SpscFifo<NoteRecord, 2048> noteRecQ_;
    std::atomic<int> liveTrack_{-1};
    uint32_t liveCounter_ = 0;
    uint32_t liveId_[128] = {};
    bool sustain_ = false;
    bool sustained_[128] = {};
    float expr_[128][3] = {};      // the latest expression value per key (see liveExpression)
    float bendAll_ = 0.0f;
    void sendExpression(int tr, int pitch, int dim) noexcept;   // also echoes the value for recording
    float exprEchoed_[128][3] = {};
    void drainLiveNotes() noexcept;
    void liveRelease(int pitch) noexcept;
    // tracking and monitoring
    dsp::PerformanceTracker tracker_;
    std::atomic<bool> trackerOn_{false};
    bool trackerRunning_ = false;
    int trackTarget_ = -1;
    uint64_t trackerOrigin_ = 0;
    PerformanceFrame perfQueue_[16]{};   // frames waiting for their chunk: delivered in order, one per chunk
    int perfQHead_ = 0, perfQCount_ = 0;
    PerformanceFrame lastPerf_{};     // the newest frame, for performance routes
    int64_t perfAgeFrames_ = 1 << 30;  // frames since it arrived
    SpscFifo<TimedPerf, 2048> perfFifo_;
    std::atomic<uint64_t> inputFrames_{0}, captureStartInput_{0}, perfCount_{0}, perfDropped_{0};
    std::atomic<float> perfF0_{0}, perfLd_{-80.0f}, perfConf_{0}, perfEnv_{0};
    std::atomic<int> trackerLatency_{0};
    std::vector<float> inRingL_, inRingR_, monoScratch_, monChunkL_, monChunkR_;
    uint64_t monRead_ = 0;
    void feedTracker(const float* inL, const float* inR, int n, uint64_t base) noexcept;
    std::atomic<int> latency_{0};
    std::vector<float> oldL_, oldR_;
    std::vector<float> pendL_, pendR_;  // the chunk being handed out
    int pendLen_ = 0, pendPos_ = 0;
};

}  // namespace ddaw::engine
