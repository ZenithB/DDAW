# DDAW: Architecture Contract

Binding contract for every agent and contributor. Read `docs/PLAN.md` first. When this document and the code disagree, fix the code, or change this document deliberately in a reviewed commit.

Status: final for M0 (2026-10-02). Changes after this point go through a reviewed commit that also updates the code. Contract headers: `src/core/Device.h`, `src/core/Cmd.h`, `src/core/SpscFifo.h`; they must match this file.

---

## 1. Threads

| Thread | Owns | May not |
|---|---|---|
| Audio (real-time) | Graph processing, transport, scheduler, analysis (YIN, loudness, followers) | Allocate, lock, block, log, throw, call system APIs |
| Inference | DDSP decoders (RTNeural) | Touch the graph directly |
| UI (message thread) | JUCE components, controller input, document edits | Touch audio-thread state except via FIFOs and atomics |
| Builder | Turning document snapshots into new graphs | Run on the audio thread |
| Services | File I/O, decoding, offline render, recording writer | Block the UI |

- FTZ/DAZ: `Engine::process` sets it for the duration of the call (`core/Denormals.h`), so live callbacks, offline renders and tests run under the same floating-point mode; devices may assume it. (The JUCE host also sets it; harmless.)
- Communication between threads uses single-producer single-consumer FIFOs with fixed-size messages, and atomics for meters and playhead.

## 2. Block model

- Audio is processed in planar stereo blocks of at most `kMaxBlock = 128` frames.
- **Fixed internal grid.** The engine renders in chunks that end on multiples of `kMaxBlock` of the absolute frame counter, or earlier at a scheduled event (note, gate-off, loop end, metronome beat), never at a callback boundary. Samples are handed out of an internal buffer, so for any callback size (32, 64, 100, 1024, or the 128 of an offline render) the output is bit-identical, including while automation, LFOs, sends and smoothed gains move. Commands, graph swaps and modulation take effect at chunk starts, so a control change lands within one chunk (at most `kMaxBlock` frames, 2.7 ms at 48 kHz); scheduled events stay sample-accurate. The cost is that a callback can render up to one chunk ahead, so worst-case callback time is about twice the average (the kitchen-sink test project: p99 4.6% of a 64-frame buffer). Verified by `tests/engine/engine_rt_test.cpp` ("output is bit-identical for every callback size, with everything moving").
- The sample rate is fixed at `prepare()`; a rate change rebuilds the graph.
- Time base: ticks at PPQ 96, matching synthyy, so imported projects map one to one.

## 3. Parameters

```cpp
enum class Curve { Linear, Exponential, Stepped };

struct ParamSpec {
    uint16_t    index;       // numeric address used on the audio thread
    const char* key;         // stable string key (save format, automation)
    float       min, max, def;
    Curve       curve;
    float       smoothingMs; // control-rate smoothing; 0 for stepped params
    bool        audioRate;   // true: accepts a per-sample modulation buffer
};
```

- Each device exposes a static `std::span<const ParamSpec>` through a `params()` accessor on its interface (`ParamSpec.index` must equal its position in the span).
- Strings are resolved to numeric indices off the audio thread. No string crosses into the audio thread.
- Control-rate values (automation, LFOs, macros, morph map) arrive via `setParam()` at block boundaries and pass through the device's smoother.
- A-rate parameters additionally receive a modulation buffer (section 5) that **bypasses** the smoother.

## 4. Device interfaces

```cpp
struct ProcessContext {
    double  sampleRate;
    double  positionTicks;   // at the first frame of this block
    double  bpm;
    bool    playing;
    bool    looping;         // arrangement loop active
    double  loopStartTicks;  // valid when looping
    double  loopEndTicks;    // valid when looping
    uint8_t tsTop, tsBottom; // time signature
};

struct ModInputs {
    // Indexed by A-rate ordinal: the position of the param among the device's
    // ParamSpecs with audioRate == true, in ParamSpec order (not ParamSpec.index).
    // nullptr when unmodulated this block. Buffers hold bipolar modulation in
    // parameter units, length == numFrames. Empty span for devices with no
    // A-rate params.
    std::span<const float* const> audioRate;
};

// Continuous performance control (tracked voice/instrument), no notes.
struct PerformanceFrame {
    float f0Hz;          // 0 when unvoiced
    float confidence;    // 0..1
    float loudnessDb;    // matched to Magenta features (A-weighted, 80 dB range)
    float envelope;      // follower output, 0..1
};

class EffectDevice {
public:
    virtual ~EffectDevice() = default;
    virtual std::span<const ParamSpec> params() const = 0;
    virtual void prepare(double sampleRate, int maxBlock) = 0; // may allocate
    virtual void setParam(uint16_t index, float value) = 0;    // block boundary only
    virtual void process(float* l, float* r, int numFrames,
                         const ProcessContext&, const ModInputs&) = 0;
    virtual void reset() = 0;
    virtual int  latencySamples() const { return 0; }
    virtual float gainReductionDb() const { return 0.0f; }
};

class InstrumentDevice {
public:
    virtual ~InstrumentDevice() = default;
    virtual std::span<const ParamSpec> params() const = 0;
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    virtual void setParam(uint16_t index, float value) = 0;
    virtual void noteOn(uint8_t pitch, float velocity, uint32_t noteId) = 0;
    virtual void noteOff(uint32_t noteId) = 0;
    virtual void noteExpression(uint32_t noteId, int dimension, float value) {} // MPE
    virtual void performance(const PerformanceFrame&) {}  // note-less control
    // ADDITIVE: mix into l/r.
    virtual void process(float* l, float* r, int numFrames,
                         const ProcessContext&, const ModInputs&) = 0;
    virtual void reset() = 0;
    virtual int  latencySamples() const { return 0; }
};
```

Rules for every device:

- No allocation, locks, syscalls or exceptions in `setParam`, `noteOn/Off`, `performance` or `process`. Allocate in the constructor or `prepare`.
- Every audible control-rate parameter is smoothed (default about 15 ms) unless stepped.
- Polyphonic instruments use a fixed voice pool with oldest-voice stealing.
- Nonlinear devices and A-rate FM/AM operators oversample (4x default) using the shared oversampler.
- Harmonic synthesis zeroes partials above Nyquist.
- Use the shared DSP utilities (smoother, oversampler, PolyBLEP oscillator, SVF, ADSR, delay line, LFO, one-pole, YIN, loudness extractor, envelope follower) instead of re-deriving them.
- One device per file: `src/devices/instruments/<type>.cpp`, `src/devices/effects/<type>.cpp`. Agents never edit shared registries or build files.

## 5. Modulation

**Control rate** (per block): automation lanes, LFOs, macros, morph map, controller input. Combined as base value (automation or manual) plus bipolar offsets, clamped to range, delivered via `setParam`.

**Audio rate** (per sample, scoped):

- Only parameters with `audioRate = true` accept A-rate modulation.
- Sources: audio-rate LFOs and oscillators, another track's audio (sidechain), envelope followers.
- The graph renders sources before targets each block and passes buffers through `ModInputs`. Routing loops are rejected at graph build time.
- A-rate buffers are pre-allocated per route at graph build.

**Implementation (A3).** `engine/Modulation.{h,cpp}` is the block-rate modulation stage, run by the Engine once per rendered chunk before the graph renders. Mappings are `"dest|fxId|pkey"` keys (dest: `inst`, `mix`, `send`, an effect id, `lfo`, `macro`, `midi`), resolved by the builder to numeric routes; the per-block work is arithmetic plus `Graph::setParam`, so it allocates nothing. Sources: the launched clip's envelopes (session, looped over the clip length), track lanes and arrangement clip envelopes (arrangement mode; clip envelopes under the playhead override lanes), master automation, per-track LFOs (synced to a tempo division or free-running in Hz; `lfo` routes modulate another LFO's rate, applied the next block) and macros (one 0..1 value fanned out linearly). Combination: automation gives the base along the spec's curve (log for exponential specs), each LFO adds `raw * depth * (max - min) / 2`, the sum clamps to the spec range, and a mapping that deactivates snaps the parameter back to its stored base. The base of a route is the stored parameter value, else the spec default (synthyy requires the value to be stored and silently drops the route otherwise). Not ported: `dest "midi"` (modulating a MIDI-fx parameter, which regenerates patterns on a control thread); the builder reports it as unsupported.

**Performance routing (implemented in B2, section 19):** the tracking analyser publishes `PerformanceFrame`s per 4 ms hop. Instruments that implement `performance()` receive them directly; any field can also be routed as a control-rate modulation source or recorded to automation.

- `performance()` is called only at block boundaries, never mid-block. A 128-frame block is shorter than one 4 ms hop at any supported rate (2.7 ms at 48 kHz), so at most one hop completes per block; the graph delivers it once, before `process()`. Several blocks may pass with no new frame, in which case `performance()` is not called. Devices that need every hop (the DDSP instrument) run their own analysis (section 6) and do not rely on this path.
- Frames are not interpolated by the graph. A device that wants sub-block smoothness interpolates internally.

## 6. Neural inference (DDSP devices)

Verified in B1 (2026-10-02) against the Magenta checkpoints for violin, flute, tenor sax and trumpet.

- **Model.** All four share one architecture: two input stacks (loudness and f0, each 3 x Dense 512 + LayerNorm + LeakyReLU), a 512-unit GRU on their concatenation, an output stack (3 x Dense 512 + LayerNorm + LeakyReLU) on `[ld, f0, gru]`, and `Dense(512 -> 126)`. The 126 raw outputs per frame are amplitude (1), harmonic distribution (60), noise magnitudes (65). Roughly 4.85 M float32 weights (19 MB) per instrument, plus a 48000-sample learned reverb impulse response that the synth chain applies after the harmonic and noise sum.
- **Rates.** The model runs at 16 kHz with 250 frames per second (hop 64 samples). The harmonic synth runs at the model rate; a rate converter to the host rate is part of the DDSP device (B3).
- **Features.** The DDSP instrument computes features (f0, loudness) at 250 frames per second on the audio thread, using the shared YIN and matched loudness extractor (16 kHz analysis path). Inputs to the decoder are `ld/80 + 1` and `hz_to_midi(f0)/127` (0 Hz maps to 0).
- **Threads.** Each feature frame is pushed to the inference thread's input FIFO. The inference thread runs the decoder and pushes the 126 raw controls to the output FIFO. The same thread applies `exp_sigmoid` scaling and Nyquist normalisation and synthesises at the model rate (moved off the audio thread after the swap-margin work; see B3 implementation below); the audio thread only consumes finished host-rate samples.
- **Synth lookahead.** The synth's frequency interpolation and Hann amplitude cross-fade between frames `f` and `f+1`, so interval `f` cannot be rendered until frame `f+1` exists. The synth therefore runs exactly one frame (4 ms) behind the decoder. This is structural, not a tuning choice.
- **Late frames.** If a control frame is late, the device holds the last frame and counts the miss (exposed for diagnostics). It never waits.
- **Inference thread scheduling (measured, M-series, 4 ms cadence).** A blocking wait (`atomic::wait`) costs about 1.1 ms of wake latency per frame and 1.5 to 2.1% of frames exceed one 4 ms hop. A spinning wait gives 0.8 ms mean, about 1.9 ms p99, and 0 to 0.07% late frames, at the price of one busy core while a DDSP instrument is active. Thread QoS class made no measurable difference. B3 chooses between spin and a hybrid (spin briefly before blocking); the per-frame deadline-miss counter is the acceptance metric.
- **Cost.** About 0.78 ms mean per frame (19% of one core), p99 1.8 ms, max 2.3 ms with RTNeural's XSIMD (NEON) backend. The STL backend takes 6.05 ms (151% of real time) and is not usable. The model is memory-bound (19 MB of weights read per frame), so fp16 or int8 weights are the first lever if more headroom is needed.
- **Latency.** Inference latency is fixed and reported via `latencySamples()`, together with the one-frame synth lookahead.
- **Implementation notes.**
  - RTNeural provides `DenseT` and `GRULayerT` only. It has no LayerNorm and no LeakyReLU, so both are ours (`NormAct`, epsilon 1e-3, alpha 0.2). The decoder is a graph, not a sequence, so RTNeural's JSON model loader and `ModelT` are not used.
  - Weights are exported from the TF checkpoint to the `.ddspw` tensor container (`tools/b1/export_weights.py`); the dense kernels are transposed to RTNeural's `[out][in]` layout, GRU tensors are used as exported by TF (`reset_after` biases).
  - Do not compile this code with `-ffast-math`: it breaks RTNeural's xsimd `tanh` and `exp` and corrupts the GRU output.
  - The ONNX Runtime fallback was not needed.

**Implementation (B3, `src/ddsp/`).** `Synth16k` is the whole model-rate chain, streaming and allocation-free: the harmonic bank (B1), `NoiseFilter` (the model's `FilteredNoise` with `window_size = 0`: each frame's 65 magnitudes, scaled by `exp_sigmoid(x - 5)`, become a 128-tap Hann-windowed zero-phase FIR applied to that frame's 64 white-noise samples, overlap-added, advanced by the filter's 62-sample delay compensation) and `ReverbConv` (the model's learned 48,000-tap impulse response with the first tap masked and the dry signal kept, as uniform-partition FFT convolution: 750 partitions of 64 samples, about 0.5% of a core). Verified against DDSP itself with the white noise fixed (`tools/b3/ref_noise_reverb.py`): the noise stage matches to 5e-4 relative (TensorFlow's float32 FFT on a signal 40 dB below the harmonics), the reverb to the float32 limit of TensorFlow's own convolution (7.8e-4 relative; checked in numpy that TensorFlow, not this port, is the inexact side), and the whole chain to the B1 end-to-end gate (-50 dB; the harmonic bank alone differs from TF's float32 by -57 dB).

The `ddsp` instrument (`DdspInstrument`) wraps it: parameters `model` (violin, flute, tenor sax, trumpet), `transpose`, `level`, `reverb`, `noise`, and for notes `attack`, `release`, `vibrato`. It is played by `performance()` frames (the tracker: timbre transfer) or by notes (a 250 Hz loudness envelope and a 20 ms glide are generated on the audio thread). The audio thread pushes feature frames (scaled loudness, scaled f0, stamped with a generation) into a wait-free FIFO; **the device's own thread** (QoS user-interactive) runs the RTNeural decoder, the whole `Synth16k` chain (harmonic bank, noise filter, reverb convolution) and the `StreamResampler` conversion to the host rate, and hands finished samples back through a wait-free float ring plus a small FIFO that tags each run of samples with its generation. The audio thread only pops samples (plays them through the level smoother); it plays `latencySamples()` zeros first (one frame of synth lookahead + the converter + a 192-sample reserve for the worker's scheduling jitter; about 10 ms at 48 kHz, reported to the graph for delay compensation). Synthesis used to run on the audio thread, one 64-sample hop every 4 ms; it dominated the steady-state worst case (1.1-1.4 ms of a 1.33 ms buffer, measured by `ddaw_swapbench`), so it was moved: the worst chunk fell to about 0.33 ms. A model change is loaded by the worker (weights, decoder and reverb, swapped in between frames and released there). A new sound after silence (or a `reset()`) bumps the generation: the worker resets the network, synthesis and converter when it sees it, and runs of samples from an older generation are discarded by the audio thread (no clearing across threads). `prepare()` stops and restarts the worker, so the rate change never races it. After 1.7 s of silence the network is put to sleep. A late frame never blocks: the ring runs dry and the output fades. In an **offline render** (`ProcessContext::offline`, set by `renderFixture`) the device waits for the worker (a progress counter of generation and frames done) after every block, so exports are deterministic and land exactly where the clip says (tested: identical samples run to run; a note at beat 1 starts at 0.5 s). Registration is explicit (`ddsp::registerDevices()`; `devices/Registry` takes extra factories) because the neural runtime lives outside `ddaw_core`; model files are looked up in `$DDAW_MODELS`, the per-user models folder, then the source tree. Measured: pitch of the output within 1% of the note; timbre differs between models; a tone at the input is audible as a violin 26 ms later (Release, device latency excluded); live, 3 s of 64-frame callbacks gave 0-3 underrun blocks and zero audio-thread allocations.

## 7. Graph lifecycle

- The builder thread constructs a complete new graph from a document snapshot, assigns it the next `epoch` (monotonic `uint32`), calls `prepare()` on new devices, bakes the document's current parameter values into it, and hands a pointer to the audio thread via a FIFO.
- The audio thread swaps at a block boundary and returns the old graph through a FIFO for deletion on the builder thread.
- **Epochs.** Every graph-addressed `Cmd` (everything except transport, tempo and metronome) carries the epoch of the graph whose indices it was resolved against. The audio thread applies a command only when `cmd.epoch == liveGraph.epoch`; otherwise it drops it and bumps a diagnostic counter. This closes the race between in-flight commands and a swap without locks or per-command lookups. Parameter drops are harmless because the new graph already contains the document's current values (baked at build). Dropped `NoteOff`s are harmless because the carried-over gate-offs are re-scheduled in the new graph (below).
- **Resolver.** The UI/controller layer resolves string keys to numeric addresses with the `ParamResolver` of the most recently published epoch and stamps that epoch on the command. A resolver is immutable once published; the builder publishes a new one with each graph.
- **Swap behaviour.** At the swap block the audio thread renders the new graph for the whole chunk and the old graph only for its first `kSwapFade` (64) frames, crossfading linearly over those frames (about 1.3-1.5 ms), so a swap is click-free. Rendering the old graph for the whole chunk was pure overhead on the most expensive block: on the soak's projects it cost 379 us against 204 us for the 64-frame fade. Effect tails do not carry over. Sounding notes and launched session clips do: `Graph::inheritNotes` re-triggers the old graph's active notes (same note id) in the new graph and copies their pending gate-offs, and the engine hands launch state across with the schedule. Notes the input layer holds are therefore not dropped by a rebuild.
- Parameter changes and transport commands use the command FIFO and never trigger a rebuild. Structural edits (add/remove/reorder devices or tracks, sample-rate change, A-rate routing change, `latencySamples()` change) rebuild.
- **Implementation (A1).** `Engine::postGraph` hands a graph over through an 8-slot SPSC lane and returns false when full (the caller keeps ownership). The audio thread swaps at the start of the next callback, crossfades over that callback's first chunk (at most `kMaxBlock` frames), then pushes the old graph onto the retired lane for `Engine::takeRetired()`. If the retired lane is full the old graph waits in a small audio-side list; only if that also overflows (the builder has stopped collecting) is a graph leaked, because deleting it on the audio thread is forbidden. `Engine::liveEpoch()` is what producers stamp on commands. A swap seeks the new graph's schedule to the current transport position.
- **Rebuild budget.** Builds run off the audio thread and may take as long as they need; edits arriving during a build are coalesced and a single follow-up build is queued.

## 8. Document model

- All edits are commands applied to a single document with stable object IDs. Undo and redo operate on commands.
- The UI never mutates engine state directly; it issues commands, and the builder reflects them into the graph.
- This command layer is the seam for future collaboration (CRDT), so commands must be serialisable and order-independent where practical.
- Save format: versioned native JSON with migrations. Importer: synthyy `ProjectJSON`, mapping device type strings and parameter keys to DDAW devices.
- **Implementation (A4).** `document::Document` (`src/document/Document.h`) owns the `Project` and applies `Command{kind,args}` values; every command yields its exact inverse, so undo/redo is a pair of stacks, with grouping for compound edits. Notes, devices and tracks carry a stable `Uid`. Each apply reports a `ChangeInfo` (structural, live parameter, or tempo only) which decides what the session does next.
- **Session and service.** `document::Session` joins a document to a `engine::GraphService`. Structural changes submit a snapshot to the service's builder thread (coalesced: edits during a build queue one follow-up build). Live parameter and tempo changes are pushed straight to the engine as epoch-stamped commands via the current `ParamResolver`; if a push races a rebuild the session resubmits a snapshot, so no edit is lost. The service publishes a resolver only after the engine confirms it swapped to that epoch, so a resolver never addresses a graph the audio thread does not yet run.
- **Native format.** A JSON header (`format`, `version`) plus the project, a chain of one-step migrations from version N to N+1, and a package directory (project JSON plus the samples as float32 WAV, lossless). Unknown newer versions are refused, never guessed at.
- **Sample bank.** `project::SampleBank` maps a sample id to an immutable `SamplePtr`. It lives on the control side; the builder injects buffers into instruments (`InstrumentDevice::setSample`, including drum pad slots 0-7) and bakes them into audio-clip voices. A missing id plays silent.
- **Audio clips and tracks.** Built in `engine/AudioClips.*` (port of sf-engine's `DerivedClip` and `AudioVoice`). A clip is derived off the audio thread: crop to offset/duration, equal-power loop crossfade, reverse, rate `2^((pitch + cents/100)/12)` times the sample-rate ratio, gain, fade times converted from ticks. Each audio track owns an `AudioPlayer` with 6 voices: voice 0 is the session voice (looped clips stay locked to the launch anchor, one-shots re-fire every clip length), voices 1..5 play arrangement clips (start at their tick, stop at clip end with the fade-out, oldest voice stolen, resync on any playhead jump). Plain data, no allocation on the audio thread.

## 9. Command FIFO

One SPSC ring (UI/builder to audio). Messages are trivially copyable, fixed size, and carry numeric addresses only. Mirrors synthyy's `Cmd` lane so imported projects and fixtures map directly.

```cpp
constexpr uint16_t kTargetMaster = 0xFFFF;  // master strip instead of a track
constexpr uint8_t  kSlotInst     = 0;       // instrument
constexpr uint8_t  kSlotFx0      = 1;       // fx device i is kSlotFx0 + i
constexpr uint8_t  kSlotMixer    = 0xFF;    // gain, pan, sends
constexpr uint16_t kParamOut     = 0xFFFF;  // device output gain (dB), not a ParamSpec

struct ParamAddr { uint16_t target; uint8_t slot; uint16_t param; };

enum class CmdType : uint8_t {
    SetParam, NoteOn, NoteOff, NoteExpression, Performance,
    ClipLaunch, ClipStop, TransportPlay, TransportStop, SetTempo,
    MetronomeOn, MetronomeOff, TestTone
};

// Mixer-slot parameters (slot kSlotMixer): kMixGain (dB; with target kTargetMaster it is the
// master gain) and kMixPan (-1..1).

struct Cmd {                       // <= 32 bytes, static_assert'd
    CmdType  type;
    uint32_t epoch;                // graph epoch for graph-addressed commands (section 7)
    union {                        // named payload structs: SetParamP, NoteOnP, NoteOffP, NoteExprP,
        SetParamP setParam;        // PerformanceP, ClipLaunchP, ClipStopP, TransportPlayP (mode 0
        NoteOnP   noteOn;          // session / 1 arrangement), SetTempoP, TestToneP (freq, level dB, on)
        /* ... see src/core/Cmd.h */
    };
};
```

`TransportPlay`, `TransportStop`, `SetTempo`, `MetronomeOn/Off` and `TestTone` are engine-level, not graph-addressed, and carry no epoch. `TestTone` is a diagnostic sine on the master bus, ahead of the limiter, owned by the Engine so it survives graph swaps; it exists for device bring-up and the A1 audio test.

- Capacity 4096 messages (`CommandFifo` in `src/core/Cmd.h`, built on `SpscFifo`). `push` returns false when full; producers drop `SetParam` (last-value-wins) and retry everything else next tick. The audio thread never waits.
- Addresses are resolved off the audio thread by a `ParamResolver` built with each graph (section 7). Key grammar (as synthyy's `CommandResolver`): `<trackId>|inst|<key>`, `<trackId>|<fxId>|<key>` (`fxId` is the device's `id`, else its type; key `out` is the device output gain), `<trackId>|mix|<gain|pan|sendA|sendB>`, `<trackId>|send|<busId>` (and `<trackId>|send|<trackId>` for the feedback bus's self-send), `master|gain`, `master|<fxId>|<key>`. Track and scene ids resolve to the indices commands carry (`ParamResolver::track` / `scene`). Addresses are graph-relative indices, valid only for the epoch they were resolved against; the epoch stamp on each `Cmd` makes stale ones droppable.
- The audio thread drains the FIFO at the start of each block, then splits the block at scheduled event boundaries (section 2).
- Meters, playhead and per-device diagnostics (late-frame counts) flow the other way through atomics, not the FIFO.

## 10. Graph, buses and latency

```
track[i]:  instrument -> fx[0..n) -> mixer strip (gain, pan, mute/solo, sends)
                                          |-> sendA bus -> bus fx -> master
                                          |-> sendB bus -> bus fx -> master
                                          |-> route bus[k] (optional, post-fader)
                                          '-> master
master:    master fx -> limiter -> meters -> out
```

- Topology: tracks, then buses in dependency order, then master. A-rate modulation sources are scheduled before their targets (section 5). The graph builder topologically sorts the whole thing; cycles (audio or modulation) are rejected with an error, never run.
- **Sidechain** is a read-only tap of another track's post-fader or pre-fader signal, delivered to the target as an extra input buffer pair or, for A-rate routes, as an envelope-followed or raw modulation buffer. Sidechain taps add a dependency edge, so they participate in the sort.
- **Buffers** are preallocated at build time: one planar stereo pair per track, per bus and per A-rate route, each `kMaxBlock` frames. Processing allocates nothing.
- **Latency compensation.** Each device reports `latencySamples()`. A track's latency is the sum over its chain. At build time the graph computes the maximum latency across all tracks and buses that feed the master, and inserts a fixed delay line (preallocated) on every shorter path so they align. Compensation is per path, including sends and sidechain taps. The compensated total is exposed as `Graph::totalLatencySamples()`.
- **Implementation (A3): the bus network.** `Graph` mirrors sf-engine `render_chunk`: tracks render in document order, then buses in topological order (Kahn over output and send edges; leftovers in document order). A track: instrument, instrument `out` gain, effect chain each with its `out` gain, pan (Web Audio law), fader * mute smoother, delay compensation, then routing: master (non-bus tracks always), another bus (`output`), per-bus sends (up to 16), the A and B sends (to the bus marked `send: "A"` / `"B"`, else to the first two `returns` effects), and the feedback bus's own delayed output. A bus-to-bus edge that would close a cycle goes through a 0.09 s delay (a back edge) so the loop regenerates instead of being dropped. Master: sum, metronome, master gain, master effect chain; the Engine then applies the limiter. Mute and solo are baked into a smoother at build. Sidechain ducks: a note fired on the source track (or a pad of it) calls `EffectDevice::trigger()` on the duck, which the builder switched into sidechain mode with `setSidechain(true)`.
- **Delay compensation (A3).** A strip's path latency is its own chain plus the bus chain it routes into; every shorter non-bus path is delayed to the longest, and master effects add to the graph latency. Sends and bus-return paths are not compensated.
- **Implementation (A1).** `Graph` holds tracks and the master gain; the `Engine` holds what must stay continuous across a swap: test tone, master limiter, meters, transport. A track's delay-compensation line is sized at `finalize()` from the longest track chain. `Engine::latencySamples()` is the live graph's latency plus the limiter lookahead (about 1.5 ms); offline render trims that many leading samples. Buses, sends and sidechains arrive with A3.
- **Master limiter.** A true lookahead brickwall limiter (`src/dsp/Limiter.h`), ceiling -1 dBFS, 1.5 ms lookahead, 50 ms release, stereo-linked: no output sample exceeds the ceiling, and below the ceiling it is bit-exact apart from its delay. synthyy's master is `Tone.Limiter(-1)`, which is a compressor kernel with a 6 ms pre-delay and a soft knee, not a brickwall. Fixture parity for loud material therefore needs a compat mode that uses the ported compressor (A2); quiet fixtures, including the stub fixture, are unaffected.
- **Meters.** `MeterBank` is a fixed bank of 128 per-track peak/RMS atomics plus master peak/RMS, limiter reduction and the playhead (`src/engine/Meters.h`). Peak hold falls with a 0.3 s time constant, as in synthyy. The bank belongs to the Engine and is never reallocated.
- Latency changes (for example swapping the DDSP model) require a graph rebuild; devices must not change `latencySamples()` after `prepare()`.
- Live monitoring paths (input tracking to DDSP instrument) are not delay-compensated against other tracks; they report their latency to the UI instead (section 6).
- Offline render uses the same graph and the same compensation, then trims the leading `totalLatencySamples()` so audio lands at timeline position zero.

## 11. Events and scheduling

**Implementation (A3).** The static note list of A1 is replaced by `engine/Scheduler.{h,cpp}` (port of sf-engine `scheduler.rs`). Each track has a `TrackSched`: session patterns per scene (clip notes expanded through the track's MIDI-fx chain by `engine/MidiFx`, on the builder thread) looping from a launch anchor, plus bounded arrangement events at absolute ticks. A lazy `peek` / `takeDue` iterator rolls note probability, swing (Tone's Transport swing math) and humanise at fire time from an `XorShift` reseeded at every transport play, so runs repeat exactly. The Engine fires everything due at the current frame, then splits the chunk at the nearest of: the next event, the next gate-off, the loop end, the next metronome beat. A fired note gets a noteId and a gate-off queued at `frame + floor(max(durTicks * framesPerTick, 20 ms))` (`Graph::scheduleGate`). `ClipLaunch` anchors at the next launch boundary (`meta.launchQ` bars) while playing, and at 0 while stopped so a scene's batch of launches lands together. The arrangement loop region wraps at its end (offline renders ignore it). A graph swap carries launched clips over by track index (synthyy drops them until the host relaunches). The metronome is an engine-level click on every beat of the meter, accented on bar starts. The text below is the original design contract and still describes the event types.

The scheduler lives on the audio thread and turns clips, arrangement lanes and live input into sample-accurate device calls.

```cpp
struct NoteEvent {                 // sorted by (frameOffset, seq)
    uint32_t frameOffset;          // within the current block
    uint16_t track;
    enum class Kind : uint8_t { On, Off, Expression, Performance } kind;
    uint8_t  pitch;
    float    velocity;             // On; 0..1
    uint32_t noteId;               // unique per sounding note; Off/Expression refer to it
    int8_t   dimension;            // Expression: MPE dim (0 slide/Y, 1 pressure, 2 pitch-bend/X)
    float    value;
};
```

- Pitch is MIDI note number 0..127. Pitch bend and MPE expression arrive as `Expression`, not as extra notes. Note IDs are assigned by the scheduler (clip playback) or the input layer (live), from a monotonic counter that is never reused within a session.
- The event buffer is a fixed-capacity array (default 1024 per block) allocated in `prepare()`. On overflow the lowest-priority events (Expression, then Performance) are dropped and counted; On/Off are never dropped.
- **MIDI effects** (arp, chord, scale, velocity, humanise) run between the scheduler and the instrument as `NoteEvent -> NoteEvent` transforms with fixed scratch buffers. They may add delayed events beyond the block; those are held in a fixed ring and re-emitted in later blocks.
- **Swing and humanise** are applied by the scheduler at event-generation time. Humanise uses a seeded xorshift whose seed is part of the document, so offline renders are repeatable.
- **Transport** counts ticks at PPQ 96. Tick position is a double; frame offsets are `round((tickEvent - tickBlockStart) * framesPerTick)`. The block split on the nearest scheduled event keeps tempo changes sample-accurate.
- `performance` events from the command FIFO carry `PerformanceFrame`s from the tracking analyser to a track; the graph forwards the latest one per block (section 5).

## 12. Shared DSP utilities

All live in `src/dsp/`, header-only where sensible, namespace `ddaw::dsp`. All are allocation-free after `prepare()`/construction and safe on the audio thread. Devices use these; they do not reimplement them. Signatures below are the contract; internals are free.

```cpp
struct Smoother {                  // one-pole, control-rate params
    void prepare(double sr, float timeMs);
    void setTarget(float v);       // jump with no ramp: snap()
    void snap(float v);
    float next();                  // per sample
    void  fill(float* dst, int n); // per block
};

struct Oversampler {               // polyphase half-band, 2x or 4x (cascade)
    void prepare(int factor, int maxBlock);   // factor 1, 2 or 4
    int  latencySamples() const;
    // Upsample in, run user callback at the high rate, downsample out.
    template <class F> void process(float* x, int n, F&& atHighRate);
};

struct PolyBlepOsc {
    void  prepare(double sr);
    void  setFreq(float hz);
    void  setShape(Shape s);       // Saw, Square, Tri
    void  setPulseWidth(float pw);
    float next();
};

struct Svf {                       // TPT state-variable filter
    void  prepare(double sr);
    void  set(float cutoffHz, float q);
    float lp(float), hp(float), bp(float);    // or process() -> struct {lp,bp,hp,notch}
};

struct Adsr {
    void  prepare(double sr);
    void  set(float aMs, float dMs, float s, float rMs);
    void  gateOn(); void gateOff();
    float next(); bool active() const;
};

struct DelayLine {                 // fractional read, power-of-two capacity
    void  prepare(int maxSamples);
    void  write(float x);
    float read(float delaySamples) const;     // linear or Lagrange, per build option
};

struct Lfo {
    void  prepare(double sr);
    void  setRateHz(float hz); void setRateTicks(double ticksPerCycle, double bpm);
    void  setShape(LfoShape s); void sync(double phase);
    float next();                   // -1..1
};

struct OnePole { void prepare(double sr); void setCutoff(float hz); float lp(float); float hp(float); };

struct Yin {                       // pitch estimator on a block (analysis rate 16 kHz)
    void  prepare(double sr, float minHz, float maxHz, float threshold = 0.15f);
    int   windowSamples() const;                      // 2 * longest period; the estimate belongs to the middle
    int   latencySamples() const;                     // analysis window / 2
    YinResult estimate(const float* x);               // {f0Hz (0 = none), confidence}
};

struct LoudnessFrame {             // exactly Magenta's compute_loudness, one frame
    void  prepare();               // 512-point Hann FFT, A-weighted power over 257 bins, 80 dB floor
    float compute(const float* x512);                 // x512[256] is the centre sample
};

struct StreamResampler {           // windowed-sinc, any ratio; output aligned in TIME with the input
    void prepare(double inRate, double outRate, int halfWidth = 16);
    template <class F> void process(const float* x, int n, F&& emit);   // emit(y, inputTime)
    int  latencyIn() const;
};

struct EnvelopeFollower {          // attack/release peak, or a symmetric RMS mean
    void  prepare(double sr, Mode = Mode::Peak); void setAttackMs(float); void setReleaseMs(float);
    float next(float x); void fill(const float* in, float* out, int n);
};

struct PerformanceTracker {        // the composition the engine and the DDSP instrument use (section 19)
    void prepare(double hostRate, const TrackerConfig& = {});
    void setConfig(const TrackerConfig&);             // audio-thread safe: no allocation
    int  push(const float* mono, int n, TimedFrame* out, int maxOut);   // frames at 250 per second
    int  latencySamples() const;
};
```

- **Implemented so far:** `Smoother`, `DelayLine` (integer and linear-interpolated read), a `Limiter` (master-bus only), and the B2 tracking utilities (`Fft`, `StreamResampler`, `LoudnessFrame`, `Yin`, `EnvelopeFollower`, `PerformanceTracker`), all in `src/dsp/`. The rest of the list is implemented with the devices that first need it.
- Every utility has a unit test with an analytic check (filter stop-band, oscillator pitch, ADSR timing, YIN against synthetic tones, loudness against a Python reference).
- Utilities may be added to this list only by the orchestrator or M0 owner (PLAN section 9); a device task that needs a missing utility reports it.

## 13. Fixture harness thresholds

Mirrors synthyy's parity harness so scores are comparable.

- **Inputs:** `fixtures/projects/*.json` (`{ name, scope, project }`) and `fixtures/golden/*.wav` (44.1 kHz stereo) are copied into `tests/fixtures/synthyy/` (git-ignored) by `scripts/sync_synthyy_fixtures.sh`, never referenced across repos at test time, and never written back to synthyy. DDAW's own fixtures, in the same layout with goldens from an independent Python reference, live in `tests/fixtures/ddaw/`.
- **Render:** DDAW renders the fixture offline at 44.1 kHz through the same graph used for live playback. synthyy goldens start transport at +0.02 s inside the buffer; the harness shifts the golden by that amount before comparing.
- **Metrics per fixture:**
  1. RMS null: `rms(a - b) / rms(b)` in dB, over the overlapping length.
  2. Spectral cosine similarity: mean over 2048-sample Hann frames, hop 1024, of cosine similarity between log-magnitude spectra of the mono mixdown.
  3. Rendered level in dBFS (to detect silence).
- **Hard assertions (always on):** no panic, no NaN or inf, no clipping beyond the limiter ceiling. When the golden is louder than -50 dBFS the render must not be all zero, unless the device is a stub (rendered RMS < 1e-7, status `silent-stub`; `STRICT=1` makes this fatal).
- **Tolerance tiers**, declared with the device's port in `<fixturesDir>/tiers.txt` (`<name> tight|spectral|baseline`, default `baseline` when the fixture has a baseline entry, else advisory), not here:

| Tier | Applies to | Pass criteria |
|---|---|---|
| Tight | DDAW's own fixtures with an independent reference (the stub fixture) | RMS null <= -60 dB |
| Baseline | Every ported synthyy device | Spectral >= the Rust port's score - 0.02, and rendered-minus-golden level within 1 dB of the Rust port's (skipped when the golden is silent) |
| Spectral | Reserved for devices with a stated tolerance of their own | Cosine similarity >= 0.98 and level within 1 dB of the golden |
| Advisory | A fixture with no baseline | Report only; hard assertions still apply |

  The goldens are Tone.js composites with no phase alignment, so exact nulls are not achievable (synthyy `docs/PARITY.md`); the Rust port's recorded scores (`target/parity-report.json`, copied to `tests/fixtures/synthyy/baseline.txt` by the sync script) are the realistic bar. Parity runs use `--limiter tone --no-trim`: the browser rendered through `Tone.Limiter(-1)` (a soft-knee compressor kernel with a 6 ms pre-delay, `Engine` mode `ToneCompat`), and the goldens contain the browser's own uncompensated pre-delays, so the latency trim is off. The transport keeps running, and session clips keep looping, through the render tail.
- **Report:** `build/parity-report.json` and a printed table, one row per fixture: name, tier, rms-null dB, cosine similarity, level dBFS, status (`pass`, `fail`, `advisory`, `silent-stub`).
- **Regenerating goldens** stays a synthyy-side activity (browser engine is synthyy's reference). DDAW only copies them in.
- **DDSP reference harness** (separate from the above; `ddaw_b1 verify`, `tools/b1/harmonic_f64.py`, ctest `b1_*`). The Python side runs the real Magenta model (TF 2.11, ddsp 3.7) on a deterministic synthetic (f0, loudness) performance and dumps decoder outputs and the harmonic synth signal. Measured gates (all four instruments pass):
  1. **Decoder vs TF**, max abs error over 1000 recurrent frames on the raw 126 outputs: <= 5e-4 (measured 2.5e-4 worst, RMS 1e-5; an independent float32 numpy decoder differs from TF by up to 1.1e-4 itself).
  2. **Harmonic synth vs the float64 spec** (isolated, same controls in): RMS null <= -100 dB (measured -125 to -131 dB).
  3. **Decoder + synth vs TF's harmonic signal**: RMS null <= -50 dB (measured -52.8 to -59.0 dB). The gate is -50 dB, not -60 dB, because TF's own float32 `angular_cumsum` is the floor: the float64 spec differs from TF by -56.8 dB on violin, and emulating TF's float32 arithmetic in numpy brings that to -80.7 dB.
  - The reference uses `use_angular_cumsum=True` for the harmonic synth. The checkpoints' gin default (`tf.cumsum` in float32) drifts over 64000 samples and is not a usable reference.
  - Noise and reverb are **not** covered by the audio null test: the noise path is random and the reverb is applied after it. The 65 noise magnitudes are verified at the decoder-output level (gate 1). A deterministic noise-and-reverb null test belongs to B3.

## 14. Verification hooks

- Golden-fixture harness: import synthyy fixture projects, render offline, compare against synthyy golden WAVs, write a per-fixture report.
- DDSP reference harness: identical feature inputs rendered in Python (Magenta DDSP) and in DDAW, null-tested.
- No-allocation enforcement on the audio thread in tests.
- Soak test: 30 minutes at a 64-frame buffer under synthetic CPU load.

## 15. Real-time safety enforcement

- Apple clang 16 has no RealtimeSanitizer (`-fsanitize=realtime` is rejected), so enforcement is a **custom allocator hook**: tests replace global `operator new` and count calls made on a thread while an `AllocGuard` is live (`tests/AllocGuard.h`). Every device and every FIFO has a test that wraps its audio-path calls in a guard and requires zero allocations.
- The hook catches heap allocation only. Locks, syscalls and exceptions are covered by code review and by the rule that audio-thread code includes no headers that declare them (`<mutex>`, `<iostream>`, `<fstream>`, `<filesystem>`). Revisit RealtimeSanitizer when a toolchain that has it is available.

## 16. Decisions recorded during M0

- **Gamepad input (B5):** Apple's GameController framework, not SDL3. The target is macOS only, and it needs no third-party dependency. Revisit if a second platform is added.
- **Dependency pins:** JUCE 8.0.15 (JUCE 9 exists; staying on 8 per PLAN), Catch2 3.16.0, nlohmann/json 3.12.0. RTNeural is added in B1.
- **Licence:** JUCE 8 is dual-licensed AGPLv3 / commercial; AGPLv3 distribution is permitted, with all AGPLv3 obligations (source offer for network and distributed use), which matches DDAW's AGPL-3.0 release.
- **Checkpoint terms:** the DDSP code is Apache-2.0. No licence statement for the pretrained solo-instrument checkpoints was found in the repository or its notebooks; treat their terms as unconfirmed and do not redistribute them in a release until confirmed. Checkpoints stay out of the repo (fetched by script).
- **RTNeural:** pinned to commit `95c3c0f` (no release tags), XSIMD backend (NEON), selectable with `-DRTNEURAL_BACKEND=XSIMD|STL`. The default Eigen backend does not compile for this model (stack-allocation limit).
- **B1 spike result:** the decoder runs correctly and in time. See section 6 for numbers and the scheduling caveat.
- **Reverb:** the checkpoints include a trainable 48000-sample reverb impulse response. The DDSP device (B3) needs a convolution stage for it, or an explicit decision to drop it and expose DDAW's own reverb instead.
- **Output device latency is not ours to hide.** On the development machine the default output is Bluetooth headphones: CoreAudio honours a 64-frame buffer but the device reports 9025 frames (205 ms) of output latency. DDAW's own latency budget (PLAN 4.7) excludes it; the audio test prints the device figure so it is never mistaken for engine latency. Live performance needs a wired or built-in output.
- **A3 deviations from sf-engine, all deliberate:** modulation routes take the spec default when a parameter is not stored (synthyy drops the route); launched clips survive a graph swap; PDC covers each track's own chain and the bus chain it feeds, not sends; `dest "midi"` modulation, and per-device fx/instrument meters are not ported. Audio clips and tracks were ported in A4.

## 17. Native UI (A5)

- **Layers.** `src/app/model/` is JUCE-free and unit tested: `Catalog` (every device type with its category, label and parameter schema, read from the generated schema tables), `Edit` (pure command builders: add track/scene/device/clip/note, move and resize arrangement clips), `Timeline` (tick/pixel maths, snap grids, bar.beat labels) and `AppModel` (the document wired to the live engine through `Session`, the selection, transport state, file operations, the sample bank, listeners). `src/app/ui/` holds the JUCE components; each view derives from `ui::View` (`refresh(ModelEvent)` when the model changes, `tick()` at 30 Hz for meters and the playhead). `AppController` (menus, shortcuts, file choosers, export, unsaved-changes guard) and `Main.cpp` live with the application target only.
- **One rule: a view never touches the engine.** Every edit is `AppModel::apply(Command)`, so it is undoable and reaches the engine the way ARCH 7 and 8 describe. Live parameter edits (knobs, faders, tempo) are wrapped in a gesture (`beginGesture`/`endGesture`) so a drag is one undo step. Clip and note drags are previewed in the view and committed on mouse-up as one grouped edit, so a drag never causes a rebuild storm.
- **Engine to UI.** Only atomics: `MeterBank` carries per-track peak, the master meter, the playhead and, per track, the launched scene and its launch anchor (so the session grid shows playing and queued clips). Nothing on the audio thread knows the UI exists.
- **Views.** `SessionView` (clip grid, scene launch, track headers, drag-and-drop of samples onto audio slots), `ArrangementView` (ruler with cursor and loop region, lanes, clip move/resize, waveform for audio clips, zoom about the cursor), `ClipEditor` (piano roll with grid snap, marquee selection, nudge, duplicate and a velocity lane; drum tracks show the eight pads as rows; audio clips show the waveform with gain, pitch, loop and reverse), `MixerView` (strips with fader, meter, pan, sends, mute/solo, routing menu, master), `DeviceChainView` (a panel per MIDI effect, instrument and effect, with a knob for every schema parameter, power/move/remove, sample pickers, ducker source, drum pad samples, master chain), `BrowserPanel` (search, devices, samples; double-click or drag to add), `TransportBar`.
- **Testing without a window.** `ddaw_uishot` renders the window or any tab to a PNG on a standalone engine (`--demo`, `--project`, `--main`, `--detail`, `--select-track`, `--select-clip`). `tests/app/ui_test.cpp` drives the real views with synthetic mouse and key events and asserts on the document; `tests/app/appmodel_test.cpp` runs the model against a live engine (launch, edit while playing, mute, File > New).
- **Pitfalls found while building it.** `juce::Component::contains()` re-tests the point in the parent's space, so a chip that is not at its parent's origin ignored clicks (use local bounds). An empty project must build a live graph, otherwise File > New leaves the old song playing (`buildGraph` accepts zero tracks for the `live` scope).
- **Not in A5 (planned later):** the modulation UI (LFOs, macros, clip and arrangement envelopes, automation lanes) and send-return strips in the mixer; computer-keyboard and MIDI note input (B2); bespoke device panels (EQ curve, compressor meters); recording (A6).

## 18. Recording and export (A6)

**Input path.** `AudioHost` opens the device input only when a track is armed (the first arm asks macOS for microphone access; the app bundle carries `NSMicrophoneUsageDescription`). The callback passes the chosen input (stereo pair, left or right as mono) to `Engine::processIO`, which renders the output exactly like `process`, then meters the input, runs the tracker and the per-track monitoring path (section 19), and feeds the capture ring while a take is running. The ring (`InputTap`, 30 s, wait-free) is drained by `engine::Recorder`'s writer thread into a float32 WAV, so a long take never accumulates in memory; a full ring drops frames and counts them (the take is reported as unreliable).

**Where a take starts.** A take begins at the first callback in which the transport is playing. The engine reads the start position from the transport's own anchor, in frames: the stream frame of the first captured sample is `nowFrame - pending - n`, playback began at `anchorFrame` (possibly inside this callback, in which case the leading frames are skipped), and `Transport::tickAtFrame` gives its timeline tick, which may be negative (a count-in: the transport can start before tick 0). Exact for a constant tempo; a tempo change during a take shifts the end of the clip by the tempo's effect.

**Latency compensation.** The performer hears the output after the engine's latency plus the device output latency, plays in time with it, and the input arrives after the device input latency. A recorded sample is therefore late by `inputLatency + outputLatency + Engine::latencySamples()` (graph delay compensation plus the limiter), plus a user offset for interfaces that misreport (Bluetooth does). `RecordingController` trims that many frames from the head of the clip (`AudioClipData::offset`), plus any part of the count-in before tick 0, and places the clip at `max(0, startTick)`. Verified with a loopback device simulation: with round trips of 2400, 4410 and 9000 frames the compensated recording puts the click on the timeline to within the click's own attack ramp, with and without a count-in; and the whole take is one undoable edit.

**Workflow.** Arm audio tracks (session header, arrangement header or mixer strip), press REC: the take records from the arrangement cursor (after 0 to 2 bars of count-in with the metronome), every armed track receives a clip referencing the same sample, and Stop (or Play) ends the take. Options (count-in, input channels, monitoring, latency offset) persist per user in `settings.json`. A device restart (opening the input, changing the device) re-prepares the Engine and resets it to an empty graph; `AudioHost::onDeviceStarted` makes the model rebuild the project at the new rate (`GraphService::setSampleRate`), so playback resumes without a manual reload.

**Not in A6.** Recording MIDI/computer-keyboard notes (it needs the live note-input path, with note events echoed from the audio thread with their tick, and belongs with B2); recording into a session slot; punch in/out; per-take comping; multi-channel interfaces beyond one stereo pair.

**Export.** `app::planExport` turns the project and options into render jobs: the mixdown, and optionally one stem per source track. A stem keeps the track's own chain, fader and pan and the buses it feeds, mutes every other source (buses stay audible, since solo would mute them) and omits the master chain and limiter, so the stems sum back to the unlimited mix (tested to -80 dB). Ranges are the arrangement, the loop region or a scene; rates 44.1/48/96 kHz; 16-bit (TPDF dither), 24-bit or 32-bit float. The renderer is the same `Engine` and `Graph` as live playback (render-path parity), now with a progress/cancel callback; the dialog runs it on a worker thread and removes partial files on cancel. Offline output is trimmed by the engine latency so audio lands at timeline position 0.

**Soak.** `ddaw_soak` (scripts/soak.sh) runs the real engine at a 64-frame block for N minutes of audio while a randomised musician edits the project (launches, play/stop, tempo, parameters, tracks, effects, notes, instruments, clip moves, undo/redo, metronome, save and reload of the whole project). It fails on any audio-thread allocation, NaN/inf, output above full scale, failed build, memory growth beyond 64 MB after the first minute, leftover retired graphs, or a callback that needs more CPU time than its buffer lasts. Debug builds are flagged and not timed. `ddaw_swapbench` measures what a graph swap costs the audio thread.

## 19. Tracking path (B2)

**What it is.** The live input is analysed into a stream of `PerformanceFrame`s - fundamental frequency, loudness, envelope, confidence - at 250 frames per second (a 4 ms hop), and those frames drive instruments and parameters. No notes are involved.

**Analysis (`PerformanceTracker`, `src/dsp/`).** The mono input is resampled to Magenta's 16 kHz (`StreamResampler`: windowed sinc, 16 zero crossings, better than 70 dB of alias rejection, time-aligned with the input). Frame `k` describes the moment `k * 64` samples after the stream start. Loudness is Magenta's own computation (`LoudnessFrame`): 512-point periodic-Hann FFT, A-weighted power averaged over the 257 bins, `10 log10`, 80 dB floor; verified against `ddsp.spectral_ops.compute_loudness` on a reference signal (chirp, vibrato tone, noise, silence, quiet tone, high tone; tests/fixtures/ddaw/b2, regenerated by `tools/b2/ref_features.py` in the B1 environment): worst difference 0.02 dB. Pitch is YIN (cumulative-mean-normalised difference, absolute threshold 0.15, first dip followed to its minimum, parabolic refinement) over a window of twice the longest period; confidence is `1 - d'`; a frame is unvoiced when confidence is below 0.5 or loudness below -70 dB. The envelope is a 5 ms / 80 ms peak follower at the analysis rate. Tones from 82 Hz to 1.2 kHz are tracked within 8 cents at 44.1 and 48 kHz, a harmonic-rich 147 Hz tone with a weak fundamental is not heard an octave off, white noise is unvoiced, and a +/-50 cent 5.5 Hz vibrato is followed to within 12 cents.

**Latency (measured, 48 kHz, tests/dsp/tracker_test.cpp).** A pitch step is reported 29 ms (70 Hz floor), 25 ms (150 Hz) or 21 ms (300 Hz) after it happens; that includes the analysis window, the 16 ms the loudness frame looks ahead, and the resampler. Through the engine, an input that starts is audible on a `follow` synth about 21 ms later at 64- and 128-frame callbacks (device latency not included). A higher pitch floor lowers the latency; the floor is a tracker setting (`TrackerConfig`, changed live by a command, no allocation). Cost: mean 25 us, worst 73 us per 64-frame callback, about 5% of the buffer.

**In the engine.** `Engine::processIO` stores the input on an input timeline (a ring) and, when `setTrackerEnabled(true)`, runs the tracker on it BEFORE rendering. Frames go three places: (1) the newest frame is delivered once, at the next chunk boundary, to the instrument of the tracking target track (`Tracking` command; `InstrumentDevice::performance`), as in section 5; (2) every frame is queued with its position on the input timeline (`popPerformance`) for the recorder; (3) the newest is kept for display and for performance routes. Frames are never interpolated by the graph. The `follow` instrument (`devices/instruments/follow.cpp`) is the first consumer: it glides to the tracked pitch, follows the envelope, closes on unvoiced or quiet input, and plays notes when no frames arrive. Tracking and monitoring are re-applied by `AppModel::tick` after each published graph, because the engine addresses tracks by index and the model by uid.

**Input monitoring.** A track with monitoring on (`MonitorInput` command; a chip on audio-track strips) mixes the live input in ahead of its effect chain, so the effects (reverb on a voice) work on the live signal. Because the engine renders on a fixed 128-frame grid, the monitored input is delayed by exactly one chunk (128 frames, 2.7 ms at 48 kHz) so a chunk never needs input that has not arrived; the output equals the input shifted by 128 frames to the sample (tested at callback sizes 64, 100 and 512). This replaces the A6 direct-to-master monitor. Monitoring is not delay-compensated against other tracks (section 10).

**Performance routes.** `Track.perf` (project JSON key `perf`, document commands `perf.insert/remove/edit`) maps a source (`f0`, `loudness`, `confidence`, `envelope`) over a range to 0..1 and applies it to parameters like an automation lane does (`dest|fxId|pkey` targets, the same resolution as LFO targets). `f0` is mapped on a log scale (default 80..800 Hz) so equal intervals move the control equally; an unvoiced frame holds the last value. The route is evaluated by the modulation stage every chunk while the tracker is live (a frame less than 100 ms old) and the parameter snaps back to its stored value when it stops. The format change is additive, so no version bump.

**Recording curves.** With "record curves" on, REC also writes every route marked `rec` into the track's automation lane for its targets (arrangement mode lanes, absolute ticks): the engine notes where the take starts on the input timeline (`requestCapture(true, false)`: no audio is stored), the model collects the frames, and at stop `buildLane` places each at `startTick + (inputFrame - startInput - compensation) * ticksPerFrame` (the same latency compensation as audio takes), drops unvoiced pitch frames, simplifies with Ramer-Douglas-Peucker (0.01 of the range, at most 4,000 points), and merges into the existing lane replacing only the recorded span. One undo step. Tested end to end: a sung 200->500 Hz glide produced a lane whose kept points are within 1.5% of the range of the true pitch at their timeline positions.

**Not in B2.** CREPE-class offline pitch tracking (PLAN 4.5 "offline mode"); polyphonic input; MIDI and computer-keyboard note input (still open: it needs notes echoed from the audio thread with their tick, like the performance frames); the DDSP instrument itself (B3) consumes the same tracker.

**Live note input (MIDI keyboard, computer keyboard, pads).** `Engine::liveNote(kind, pitch, velocity)` is callable from any thread (a short producer lock; the audio thread only pops a wait-free queue). At the next chunk boundary the engine plays the note on the **live target track** (a `LiveTrack` command; the model picks the armed synth/drum track, else the selected one, and re-sends it after every rebuild), allocates a note id, keeps one sounding note per key (striking a key again ends the old one), implements the sustain pedal (CC64: released notes are held until it lifts), and echoes every note-on/off with its timeline tick in a queue for recording. Polyphony is the instrument's own: a chord is simply several notes (tested: a triad's three fundamentals sound together and each releases alone; a storm of keys from two threads allocates nothing). `LiveInput` (UI) opens every MIDI input (hot-plug checked every 2.5 s) and implements the computer keyboard as a piano (A W S E D F T G Y H U J K O L P ; ; Z/X octave, C/V velocity; off by default; disabled while a modifier is held or a text field has focus).

**Recording notes.** Arming a synth or drum track (the red dot, as for audio) makes it the live target and records what is played: REC captures the take's start tick exactly as for audio (`requestCapture(true, false)`, no input device needed), and at stop each armed track gets a clip: note-ons paired with their note-offs, ticks shifted earlier by the device's output latency plus the engine's delay (a key has no input latency), notes still held end with the take, the clip rounded up to whole beats. Verified with a chord and a held note played against a paced device: the chord's notes start within one chunk of each other, durations within 10 ticks, velocities exact, one undo step.

**Polyphonic audio input (`PolyInput`).** A service thread reads a mono copy of the input (a ring the audio thread feeds only while it is on), resamples to 16 kHz, and every 32 ms analyses the last 128 ms with `dsp::PolyPitch`: Hann-windowed spectrum (zero-padded x4), parabolic peak picking, then iterative multi-pitch estimation in the manner of Klapuri - every peak proposed as a 1st-4th partial, each hypothesis scored by the harmonics it explains (weighted 1/sqrt(h)), the best taken, the partials it explains weakened to 12%, repeat up to 6 voices. Guards against the classic failures: the fundamental must be audible (a subharmonic is not a note), at least three of the first six partials must match unless the frame is a clean tone, a voice's partials must carry 5-6% of the frame's energy, and a frame whose strongest peak is not ten times the median bin is noise. `PolyNoteTracker` adds hysteresis (a note starts when seen in 2 frames, ends after 3 missing; candidates more than 45 cents from a semitone are ignored; velocity from the partial's level). Notes go to the live-note path, so they play the target instrument and are recorded like keys (with the input device latency and the analysis delay added to the compensation). Tested on synthetic harmonic chords: triads, a seventh chord, a five-voice chord, an octave pair, a quiet upper note, vibrato, noise and silence (no phantom notes), and a low chord that needs the 256 ms window; end to end through the engine a C major chord at the input starts its three notes about 81 ms later (the analysis window plus confirmation; reported conservatively as 100 ms) and silence releases them. Limits: a chord whose notes are closer than a window can resolve (low register, 128 ms) needs `window = 4096`; a missing fundamental (some low piano notes) is not found; a repeated note without a gap is not retriggered; percussion and heavy distortion are not harmonic.

**Modulation UI.** The Modulation tab shows the selected track's LFOs (on, shape of seven, free in Hz or synced to a division, depth, targets), macros (a knob fanning out to many parameters; the first target starts the knob where the parameter already is, so adding it changes nothing) and an automation editor for the track's timeline lanes or the open clip's envelopes (click to add a point, drag, double-click to delete, snapped to a grid unless Shift is held; the value under the pointer is shown in the parameter's own units). Targets are chosen from a popup of the track's instrument, effects and mixer. **Live edits do not rebuild the graph** (a rebuild drops effect tails): a macro's value and an LFO's depth, rate and phase are live parameters (`macro.value`, `lfo.field` commands; resolver keys `<track>|macro|<i>` and `<track>|lfo<i>|depth|hz|phase`; `Graph::setParam` slots `kSlotMacro` / `kSlotLfo`), while shape, sync, division, target lists and lane points are structural edits. A macro with targets is always on (its knob is a modulation source applied every block; automation of the macro overrides it).
