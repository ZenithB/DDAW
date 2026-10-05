# DDAW: Project Plan

A desktop audio workstation built around DDSP (differentiable digital signal processing) synthesis: audio-rate modulation, latent-space control from a single joystick, and live performance tracking that drives synthesis through continuous control curves instead of MIDI notes.

Status: planning agreed 2026-10-02. Read this file first, then `docs/ARCH.md` (the binding architecture contract).

---

## 1. Goals and non-negotiables

**Goals**

- A full DAW: multitrack timeline, session grid, piano roll, mixer, built-in effects, recording, export.
- DDSP synthesis as first-class instruments and effects, not bolt-ons.
- Audio-rate (A-rate) modulation on modulation-capable devices, without aliasing.
- One controller movement (joystick, XY pad) navigating many synthesis parameters smoothly and non-linearly.
- Voice or acoustic instrument in, continuous pitch and loudness curves out, driving synthesis with the player's breath, vibrato and jitter intact.
- Interactive use within 50 ms end-to-end latency. Heavier experimentation runs in offline render modes.

**Non-negotiables**

1. **Real-time safety.** No allocation, locks, syscalls or exceptions on the audio thread. Neural inference never runs on the audio thread.
2. **Render-path parity.** One graph serves live playback and offline export. No second engine.
3. **Verification before features.** Every ported device is checked against synthyy's golden fixtures; every DDSP device is checked against a Python reference render.
4. **synthyy is untouched.** It remains the Rust/web reference implementation. DDAW reads synthyy projects; it never writes to the synthyy repo.

---

## 2. Decisions

| Area | Decision | Why |
|---|---|---|
| Stack | C++20, JUCE 8, CMake | Largest audio ecosystem and reference code (including DDSP-VST); native controller access; native plugin hosting later |
| UI | Native JUCE components | Controllers read directly with no webview polling or bridge hop; one language end to end |
| Platform | macOS, Apple Silicon only | Development and target machine; enables NEON and Accelerate |
| Licence | AGPL-3.0 (open source, free release) | Compatible with JUCE's open-source licence; GPL components allowed |
| Repo | New repo `DDAW` at `~/Documents/DDAW` | synthyy stays a clean reference |
| Project files | Native versioned JSON format; import synthyy `ProjectJSON` | Freedom to add DDSP and A-rate features; synthyy projects and golden fixtures still load |
| Collaboration | Deferred, designed for | Command-based document model with stable IDs, so a CRDT layer can be added later |
| Neural models | Pretrained Magenta DDSP solo-instrument checkpoints now; custom training later | No training budget in v1 |
| Inference | RTNeural (BSD-3), compile-time model definitions | Built for real-time audio; supports the GRU and dense layers the DDSP decoder uses; avoids ONNX conversion risk |
| Plugin hosting | Later milestone (host VST3/AU in the app) | Not needed to prove the DDSP ideas; JUCE supports it natively |
| Build method | Multi-agent orchestration, as with the synthyy Rust port | Contract-first, one owner per file, tests as gates |
| Order | Two parallel tracks joined by the device contract | Engine port and DDSP work proceed independently |

---

## 3. Architecture overview

```
JUCE application (single process)
|
|-- UI thread (native JUCE components)
|     session grid, arrangement, piano roll, mixer, device panels,
|     morph map editor, performance-tracking monitor
|     reads controllers directly: MIDI/MPE (JUCE), gamepad (GameController / SDL3)
|
|-- Document model (command-based, stable IDs, undo; future CRDT seam)
|     -> builds engine graphs off-thread; synthyy importer; native save format
|
|-- Command FIFO (SPSC, fixed-size, numeric addressing)  ->  AUDIO THREAD
|                                                          transport -> scheduler
|                                                          -> track graph:
|                                                             [instrument -> fx chain]
|                                                             control-rate modulation
|                                                             A-rate modulation ports
|                                                          -> buses -> master -> out
|
|-- Inference thread (DDSP decoders, RTNeural)
|     audio thread posts feature frames (f0, loudness) -> FIFO
|     inference returns synth controls (harmonics, noise) -> FIFO
|
|-- Analysis (in audio thread, cheap): YIN pitch, matched loudness, envelope followers
|
|-- Services (non-RT): file I/O, decode, offline render, recording writer
```

Details and the device interface are in `docs/ARCH.md`.

---

## 4. Synthesis and control design

### 4.1 Device families and their sources

The papers that informed each device family. These are design references, not implementation commitments.

| DDAW device family | Technique | Key references |
|---|---|---|
| DDSP Instrument (neural) | Harmonic-plus-noise synth driven by a GRU decoder | Engel et al. 2020a; Carney et al. 2021 (efficient real-time kernels) |
| Harmonic+Noise synth (hand-controlled) | Additive harmonics + filtered noise | Engel 2020a; Fabbro 2020; Nercessian 2021 |
| Source-filter voice | Excitation (pulse, cyclic noise) through learned or LPC filter | Wang 2019a/b, 2020 (NSF, cyclic noise); Valin and Skoglund 2019 (LPCNet); Juvela 2019; Subramani 2022 |
| FM operator synth | Audio-rate FM with differentiable-style parameters | Caspe 2022; Ye 2023 |
| Subtractive synth | Oscillators + filter + ADSR | Masuda and Saito 2021, 2023 (differentiable ADSR) |
| Wavetable synth | Learned or designed wavetables | Shan 2022 |
| Waveshaping synth | Waveshaper with learned transfer curve | Hayes 2021 |
| Modal / physical | Resonant modes; inharmonic piano; waveguides | Diaz 2023; Renault 2022; Südholt 2023 |
| Percussion | Hybrid noise + transient synthesis | Shier 2023 |
| Performance rendering (later) | Expressive control curves from score or MIDI | Castellon 2020; Jonason 2020; Wu 2022c |

### 4.2 Audio-rate modulation

- The engine is block-based (MAX_BLOCK = 128 frames). Control-rate modulation (automation, LFOs, macros, joystick) is evaluated per block and goes through a one-pole smoother.
- **A-rate is scoped to modulation-capable devices.** Those devices declare A-rate parameters, which receive a per-sample modulation buffer and **bypass the smoother**. A smoother on an A-rate input would low-pass the modulation away.
- Sources for A-rate ports: audio-rate LFOs and oscillators, other tracks' audio (sidechain), envelope followers.
- Global per-sample modulation of every parameter is out of scope: CPU cost scales with every parameter in the graph.

### 4.3 Anti-aliasing, per module type

| Module | Method |
|---|---|
| Harmonic/additive (DDSP core) | Zero any partial above Nyquist (exact, cheap) |
| A-rate FM/AM operators | 4x oversampling with polyphase half-band filters |
| Classic oscillators | PolyBLEP or mip-mapped wavetables |
| Waveshapers | Oversampling, or antiderivative anti-aliasing where the curve allows |

### 4.4 Latent-space joystick

- **v1: designed morph map.** The user places anchor presets on a 2D field (more dimensions via a second stick or modifier). The joystick position interpolates all mapped parameters between anchors using smooth weights (inverse-distance or radial basis functions) with per-parameter response curves, so one movement changes dozens of parameters non-linearly. Works on every device and needs no ML.
- **DDSP Instrument.** The pretrained Magenta solo-instrument models have **no latent (z) encoder** (pitch and loudness only, `Autoencoder.encoder = None`). The morph map instead targets the model choice (crossfading between instrument models), pitch and loudness offsets, harmonic distribution shaping, noise level, and downstream effects.
- **Later: learned map.** A small model trained on preset parameter sets gives a denser, smoother space. Custom DDSP training with a z encoder (later milestone) would add true timbre latents.
- Control rate (around 60 to 250 Hz with smoothing) is sufficient for the joystick. It does not need A-rate.

### 4.5 Performance tracking (bypassing MIDI)

- **Input:** live input monitoring from a mic or instrument at the host rate.
- **Pitch:** a YIN-style tracker (own implementation), producing f0 and confidence per frame. Window of about two periods of the lowest expected pitch (about 25 ms for 80 Hz).
- **Loudness for neural models:** computed to **match Magenta's training features exactly**: A-weighted power in dB, 16 kHz, 250 frames per second, 512-point FFT, 80 dB range. A mismatch here gives the model wrong inputs.
- **Envelope followers:** attack/release followers (peak and RMS) used as modulation sources, not as model inputs.
- **Output:** continuous control curves (f0, loudness, confidence, envelope) that drive DDSP instruments directly and can also be routed to any parameter or recorded as automation lanes. No note quantisation.
- **Offline mode:** higher-quality pitch tracking (CREPE-class) and heavier processing for experimentation renders.

### 4.6 Neural inference

- Magenta solo-instrument decoder: 512-unit GRU plus fully connected layers, outputting amplitude, 60-harmonic distribution and 65 noise magnitudes per frame.
- Models run at 16 kHz feature rate and 250 frames per second (4 ms hop). The synthesiser runs at the host sample rate and interpolates controls between frames.
- Inference runs on a dedicated thread. The audio thread posts feature frames and reads results from FIFOs. The added latency (one frame of synth lookahead plus the handoff) is reported for compensation. The inference thread should spin rather than block (ARCH section 6).
- Weights are exported from the TensorFlow checkpoints to a tensor container (`.ddspw`) and loaded into RTNeural `DenseT` and `GRULayerT` layers with fixed compile-time sizes. LayerNorm and LeakyReLU are ours (RTNeural has neither). See ARCH section 6.

### 4.7 Latency budget (interactive target: 50 ms or less)

Measured in B1 on Apple Silicon, 48 kHz, 128-frame buffers. "Structural" means it follows from the design, not from a measurement.

| Stage | Value | Source |
|---|---|---|
| Audio input + output buffers (128 frames each way) | about 5.3 ms, plus device latency | structural; device latency not measured |
| Pitch analysis + loudness look-ahead + resampler (a pitch step to its frame) | 21 ms (300 Hz floor), 25 ms (150 Hz), 29 ms (70 Hz) | **measured in B2** (tests/dsp/tracker_test.cpp) |
| Feature frame hop quantisation | up to 4 ms (2 ms average) | structural |
| Inference thread: post to result, spinning wait | 0.8 ms mean, 1.9 ms p99 | measured |
| Inference thread: post to result, blocking wait | 1.9 ms mean, 4.4 ms p99, 1.5 to 2.1% of frames over one hop | measured |
| Audio-thread pickup at the next block boundary | up to 2.7 ms | structural |
| Synth lookahead (frame f+1 needed to render f) | 4 ms | structural, found in B1 |
| **Total, worst case, spinning wait** | **about 18 ms + YIN window** | 28 ms (violin) to 43 ms (80 Hz floor) |

Within the 50 ms target in both cases. The YIN window dominates; raising the pitch floor lowers latency.

---

## 5. Milestones

### M0: Contract and scaffold (blocks both tracks)

- Repo, CMake, JUCE 8, test framework, CI on macOS.
- `docs/ARCH.md` finalised: device interface, parameter registry, A-rate ports, threading, FIFOs, graph swap, document command model.
- Golden-fixture harness: load synthyy fixture projects through the importer, render, compare against synthyy's golden WAVs (RMS null and spectral similarity).
- **Exit:** a stub device passes the harness end to end.

### Track A: Engine and DAW port (synthyy as spec)

- **A1 Audio core:** device manager, graph, transport (PPQ 96 ticks), master chain and limiter, meters. Exit: test tone and empty project at a 64-frame buffer, no dropouts.
- **A2 Device ports:** synthyy's 11 instruments and 23 effects, one file each, against the fixtures. Exit: all fixtures render; similarity reported per device.
- **A3 Sequencing and modulation:** scheduler (session and arrangement, swing, humanise), MIDI effects, automation, LFOs, macros, sends and buses.
- **A4 Projects:** synthyy importer and native save format with versioning and migration.
  - **Sample-based instruments are built here:** `sampler`, `ksampler` and `granular` are implemented in A4, together with the sample bank they depend on (storage in the project format, import/decode, per-device sample slots, and drum pad sample overrides, which A2 left unported). They have no fixtures and are deliberately not part of A2 or A3. `audiobus` (routing) is decided alongside.
- **A5 Native UI:** session grid, arrangement view, piano roll, mixer, device panels, browser.
- **A6 Recording and export:** input recording with latency compensation, offline mixdown and stems, 30-minute soak test.

### Track B: DDSP and control

- **B1 Decoder spike (highest risk, first):** export one Magenta checkpoint to RTNeural; render the same f0 and loudness input in Python and C++; null-test the outputs; measure per-frame inference time and end-to-end latency. Exit: matching render and a latency figure. If it fails, fall back to ONNX Runtime and report.
- **B2 Tracking path:** input monitoring, YIN tracker, matched loudness extractor, envelope followers, control-curve recording.
- **B3 DDSP Instrument device:** harmonic-plus-noise synth with Nyquist masking, model selector (violin, flute, tenor sax, trumpet), inference thread integration, latency reporting.
- **B4 A-rate modulation:** A-rate parameter ports, audio-rate LFOs and oscillators as sources, FM/AM operator synth with 4x oversampling.
- **B5 Morph map and controllers:** morph map engine and editor, gamepad input, MPE routing.
- **B6 Synth families:** hand-controlled harmonic+noise, subtractive with ADSR, wavetable, waveshaping, modal, percussion.

### Join points

- B3 to B6 devices plug into A1 to A3 as soon as each exists, through the M0 contract.
- B2 needs A1 input monitoring; until then it develops against an offline test harness.
- B5 UI work lands inside A5.

### Later milestones

- **L1 Plugin hosting:** VST3/AU instruments and effects inside the app.
- **L2 Custom training:** train DDSP models (including a z encoder) on Colab; import through the B1 export path.
- **L3 Learned morph maps.**
- **L4 Collaboration:** CRDT layer over the command model.
- **L5 Experimental offline engines:** source-filter and LPC voices, heavier neural models, CREPE-quality tracking.

---

## 6. Verification gates

- **Every device:** unit tests (non-silence, no NaN/inf, parameter sweeps stable, one analytic check such as filter stop-band or oscillator pitch).
- **Ported devices:** golden-fixture similarity against synthyy, reported per device.
- **DDSP devices:** null test against the Python reference render for identical inputs.
- **Real-time safety:** no-allocation checks on the audio thread in tests (RealtimeSanitizer or a custom allocator hook), plus a 30-minute soak at a 64-frame buffer.
- **Latency:** measured end to end for the tracking path and reported in this file.

---

## 7. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Checkpoint export to RTNeural fails or mismatches | Blocks neural devices | **Retired in B1**: matches TF; ONNX fallback not needed |
| Inference thread misses frame deadlines under load | Audible dropouts in the DDSP device | Spinning wait (0 to 0.07% late frames); hold-last-frame policy; fp16/int8 weights if more headroom is needed |
| Checkpoint licence unconfirmed | Cannot ship the pretrained models | Keep out of the repo; confirm terms before any release; custom training (L2) is the fallback |
| Pretrained timbre quality below expectations (reported for DDSP-VST) | Weaker headline feature | Hand-controlled synth families stand alone; custom training in L2 |
| Tracking latency over 50 ms for low voices | Feels laggy | Configurable pitch floor; measure in B1 and B2 |
| Native UI is the largest workload | Delays a usable app | Port engine first; UI follows A1 to A3; agents parallelise panels |
| Parallel tracks collide | Rework | Contract-first M0; one owner per file |
| A-rate CPU cost | Dropouts | Scoped A-rate ports only; oversample only where needed |

---

## 8. To verify before or during M0

| Item | Outcome (2026-10-02) |
|---|---|
| JUCE 8 licence terms for AGPL distribution | Confirmed: JUCE 8 is dual-licensed AGPLv3 / commercial; AGPLv3 distribution is allowed with all AGPLv3 obligations. JUCE 9 exists; pinned to 8.0.15. |
| Magenta checkpoint terms | **Open.** DDSP code is Apache-2.0; no licence statement for the pretrained checkpoints found in the repo or notebooks. Do not redistribute until confirmed. |
| Gamepad input on macOS | Decided: Apple GameController framework (B5). |
| RTNeural 512-unit GRU, allocation-free, timing | Done in B1: allocation-free (tested), 0.78 ms per frame with the NEON backend. Needs the XSIMD backend and no `-ffast-math`. |
| RealtimeSanitizer in Apple clang | Not available (Apple clang 16 rejects `-fsanitize=realtime`). Custom allocator hook used instead (ARCH section 15). |

---

## 9. How the build is run

- Contract first: M0 is done by one agent before any parallel work.
- Each agent owns specific files (one device per file) and never edits shared registries or build files; missing dependencies are reported, not added.
- Each agent's work must pass its gates (unit tests, fixture or reference comparison, no-allocation checks) before it is merged.
- An orchestrator integrates, runs the full suite, and updates the status section of this file after each milestone.

---

## 10. Status

**M0 (scaffold and contract), 2026-10-02:** done except where noted.

- Repo, CMake, JUCE 8.0.15 app target, Catch2 tests, macOS CI workflow (`.github/workflows/ci.yml`, not yet run on GitHub).
- `docs/ARCH.md` finalised (command FIFO with graph epochs, graph and latency model, events, DSP utility API, fixture thresholds, real-time enforcement).
- Contract code: `Device.h`, `Cmd.h`, `SpscFifo.h`; stub devices `StubGain` and `StubTone`; device registry; synthyy fixture importer; offline renderer (scene scope); parity runner and `ddaw_parity` CLI.
- **Exit criterion met:** the stub fixture passes the parity runner at the tight tier against an independent Python golden (RMS null -86 dB). All 37 synthyy fixtures import, render without crashing, and are reported `unported` with reasons until their devices are ported (A2).

**B1 (decoder spike), 2026-10-02:** done. Exit criterion met: matching render and a latency figure.

- Exported all four Magenta solo checkpoints (violin, flute, tenor sax, trumpet) and ran them in C++ on RTNeural layers. Same architecture for all four, so one compile-time model definition.
- Decoder output matches TensorFlow within 2.5e-4 max over 1000 recurrent frames. The C++ harmonic synth matches a float64 spec at -125 to -131 dB; decoder + synth against TF's own harmonic output reaches -52.8 to -59.0 dB, limited by TF's float32 arithmetic (ARCH section 13).
- Per-frame cost 0.78 ms mean (19% of one core) with the NEON backend; the STL backend is 8x slower and unusable. Thread handoff and the full latency budget are in section 4.7.
- **Not covered:** noise synthesis and reverb (random / post-chain; deferred to B3), the YIN analysis window (B2), and device I/O latency.
- **Open decisions for B3:** spin versus hybrid wait for the inference thread; whether to keep the model's learned reverb; fp16/int8 weights if headroom is needed.
- Two things found that the plan did not anticipate: the model has no LayerNorm support in RTNeural (custom layer), and `-ffast-math` corrupts RTNeural's xsimd GRU.
- Reproduce with `scripts/b1.sh`. Checkpoint terms remain unconfirmed (section 8).

**A1 (audio core), 2026-10-02:** done. Exit criterion met: a test tone and an empty project at a 64-frame buffer with no dropouts.

- `Engine` (command drain with epoch checks, transport at PPQ 96, sample-accurate block splitting, graph swap with crossfade and retirement, test tone, master limiter, meters), `Graph` (tracks, mixer, delay compensation) and `GraphBuilder` (builder-thread construction plus the string-to-address resolver). Offline render now runs through the same Engine and Graph (one graph for live and offline).
- Verified: output is bit-identical for callback sizes of 1, 7, 64, 100, 333, 1024 and whole-clip; no allocation across commands, events, tone, limiter and a graph swap; the limiter never exceeds its ceiling on random, loud and spike input.
- Real device (CoreAudio, 64 frames at 44.1 kHz, 4118 callbacks): worst process time 34 us, 2.3% of the buffer; no over-budget or late callbacks, no driver xruns. Offline soak at 64 frames with 8 tracks: p99 50 us (4%), max 150 us (11%) of the 1333 us budget.
- `JUCE AudioHost` (device manager, denormal flush, per-callback timing) and `ddaw_audiotest` (headless device check, exit status reflects dropouts); the app shell has device selection, a tone switch and live stats.
- **Not yet done:** the 30-minute soak (A6), buses/sends/sidechains and the real scheduler (A3), input monitoring (A6/B2), compat limiter for synthyy loud-fixture parity (A2).
- Found: `Transport::stop` lost the playhead (fixed, tested); the limiter differs from synthyy's `Tone.Limiter` by design (ARCH 10).

**A2 (device ports), 2026-10-02:** done for every device that has a fixture. 31 of 31 fixtures with a ported device pass baseline gating; 269 of 269 tests pass in Debug and Release; no forbidden constructs in device code.

- **Ported (31 devices):** instruments `mono`, `poly`, `duo`, `fm`, `keys`, `pluck`, `drum`; effects `comp`, `dist`, `crush`, `cheby`, `widen`, `eq`, `filter`, `eq7`, `autofilt`, `delay`, `pingpong`, `reverb`, `plate`, `chorus`, `phaser`, `vib`, `trem`, `autopan`, `opto`, `duck`, `mbcomp`, `gate`, `shift`, `autotune`. Each matches the Rust port's recorded scores to about 0.001 spectral and 0.05 dB (worst: `trem` 0.0016 below, `autotune` 0.26 dB off, both inside the gate).
- **Method:** the shared groundwork (utilities, generated schema, harness, registry generation, test kit, parity gate) was built first, then nine parallel agents each ported 2 to 5 devices in private workspaces and delivered only their own two files per device. Four of the nine hit nothing that needed a shared change; the follow-ups below came from the others.
- **Unported fixtures (6), all A3:** `feat-automation` (clip envelopes), `feat-sends` (sends and buses), `feat-swing`, `midi-arp`, `midi-chord`, `midi-velo`.
- **Not ported (no fixture):** `sampler`, `ksampler`, `granular` (need a sample bank): **scheduled for A4**, built there with the sample bank and the project format. `audiobus` (routing) is decided with them.

Follow-ups surfaced by the ports (none blocks A3):

1. ~~**Sidechain/trigger input** for `duck`~~ Done in A3: `EffectDevice::trigger()` and `setSidechain()`, driven by the engine when a note fires on the source track.
2. **Key and scale context** for `autotune`: ProcessContext has no `keyRoot`/`scaleMask`, so it always snaps to A minor. The Rust baseline was also produced with those defaults (a C# major experiment scored lower), so parity is unaffected, but the device is incomplete.
3. **Per-band gain-reduction meter** for `mbcomp`: `gainReductionDb()` returns the deepest band; the three band values are kept but not exposed.
4. **Drum sample slots** (`set_sample`) are not ported; every pad synthesizes. Done in A4 with the sample bank.
5. **Float-sine LFO variant:** `trem` sits 0.0016 below baseline because the shared `dsp::Lfo` evaluates sin in double (as Rust's `lfo.rs` does) while Rust's `trem` uses an inline float sin. Add a float variant only if the gap matters.
6. **Latency reporting:** `dist`, `crush`, `cheby` do not report the oversampler's roughly 33-sample delay (matching Rust); `gate`'s lookahead is a creative delay and reports 0. Decide before delay compensation matters for these.
7. **`plate` smoother snap:** the agent suspects Rust glides the smoothers in from defaults while C++ snaps on `reset()` (0.001 spectral, 0.0001 level; not chased). The same behaviour explains why `widen` renders silent where Rust leaves a -48 dBFS ramp residue; the C++ behaviour matches the browser, which sets parameters at construction.
8. **`rateSwitch` params:** `rateMode`, `rateSync`, `rateHi` were missing from the generated schema (found by four agents independently). The generator now appends them to the six affected tables and the six devices (`autofilt`, `chorus`, `phaser`, `vib`, `trem`, `autopan`) implement Sync, Low and High, with tests.

**A3 (scheduler, modulation, sends), 2026-10-03:** done except `dest "midi"` modulation. **All 37 synthyy fixtures now pass baseline gating; 335 of 335 tests pass in Debug and Release; none unported.**

- **Scheduler** (`TrackSched`): session patterns looping from a launch anchor, arrangement events, probability, swing, humanise, launch quantisation, relocation, arrangement loop wrap, sample-accurate gate-offs; MIDI effects (`scale`, `chord`, `arp`, `velo`, `rand`) ported by a parallel agent (28 tests); the metronome.
- **Bus network:** buses with topological order and delayed back-edges, A/B send buses, per-bus sends, legacy returns, the feedback bus, master effects, device `out` gains, sidechain ducks.
- **Modulation:** clip and arrangement envelopes, track and master automation, free and synced LFOs, LFO-rate routing, macros.
- **Newly passing fixtures:** `feat-swing` (0.9293 vs Rust 0.9294), `feat-sends` (0.9748 vs 0.9748), `midi-arp`, `midi-chord`, `midi-velo`, `feat-automation` (0.9762 vs 0.9765).
- **Verified:** block-size independence is still bit-exact; no allocation across the whole new engine surface under command bursts, relaunch, stop/play in both modes and a graph swap (a kitchen-sink project with 7 tracks and 4 buses); the kitchen sink uses 2.3% of a 64-frame buffer at p99 (5.7% max, Release); the real-device check still reports no dropouts.
- **Not in A3:** `dest "midi"` modulation (a control-thread pattern regeneration), per-device meters, clip follow actions. (Audio clips and tracks and the sample-based instruments were delivered in A4.)

**Reassessment, 2026-10-03 (between A3 and A4).** Findings and fixes:

- **Fixed: output depended on the callback size** whenever anything moved (automation, smoothing): up to 3e-3 per sample at 32 frames, so a 64-frame live callback and a 128-frame offline render differed. My earlier "bit-identical for every callback size" claim (A1, A3) had only been tested with static parameters. The engine now renders on a fixed absolute 128-frame grid (ARCH 2); a nine-size regression test covers it with everything moving.
- **Fixed: FTZ/DAZ was set only in the JUCE host**, so offline renders and tests broke ARCH 1's contract. `Engine::process` now sets it.
- **Verified: no undefined behaviour or bounds violations.** All 323 core tests (every device, every fixture) pass under UBSan plus libc++ hardened mode. AddressSanitizer and ThreadSanitizer cannot run on this machine (they abort or crash on a trivial program: toolchain and OS mismatch), so they are not part of the gate until a working toolchain is available.
- **Open, needs a decision:** no `LICENSE` file exists although the plan says AGPL-3.0; synthyy is declared `UNLICENSED` (all rights reserved), so the owner must confirm that porting its code into an AGPL project is intended; checkpoint terms are still unconfirmed. The tree is not under version control. CI exists but has never run, and cannot enforce the 37-fixture parity gate or the B1 checks because the goldens (34 MB) and checkpoints are git-ignored.
- **Done in A4:** the command-based document model with stable IDs and undo (ARCH 8) and the builder-thread service that turns edits into graphs.
- **Found and fixed during A4:** a graph swap dropped every sounding note (30 rapid swaps produced silence); `Graph::inheritNotes` now carries notes and gate-offs across.

**A4 (projects, samples, audio clips), 2026-10-03:** done. 417 of 417 tests pass in Debug, including the 37-fixture parity gate.

- **Document model, session and builder-thread service** (ARCH 8): undo/redo with exact inverses, stable uids, live parameter pushes, epoch-matched resolver publication, coalesced rebuilds, race handled by resubmitting a snapshot.
- **Native format:** versioned JSON plus a package directory with float32 WAV samples, a migration chain, and the synthyy importer.
- **Sample bank and sample-based instruments:** `sampler`, `ksampler`, `granular` and drum pad sample overrides (parallel agents, as in A2).
- **Audio clips and tracks:** session loop and one-shot re-fire, arrangement start/stop with fade-out, pitch, reverse, crop, loop crossfade, gain, fades; callback-size independent (test).
- **Checkpoint terms (2026-10-05):** re-checked upstream: the DDSP repository is Apache-2.0 and its README says nothing about the pretrained checkpoints or their training data, so the terms stay unconfirmed. Enforced instead of decided: the weights are never committed (`scripts/check_no_weights.sh`, run in CI), `NOTICE` credits Magenta (Apache-2.0, 2019 Google LLC) and the other dependencies, and the app and tests work without the weights (the DDSP tests skip). A binary release that bundles weights needs written confirmation first; otherwise ship without them.
- **Decided (2026-10-05):** synthyy will be released under the AGPL, the same licence as DDAW (AGPL-3.0), so porting its code and fixtures into DDAW needs no relicensing. `LICENSE` (the unmodified GNU AGPL-3.0 text from gnu.org) is in the tree root.
- **Still open (needs the user):** git, CI gating of parity (Git LFS), checkpoint licence, ASan/TSan. Engine gaps: `autotune` key/scale, per-band `mbcomp` meters, `dest "midi"` modulation, PDC for sends.

**A5 (native UI), 2026-10-03:** the application is usable end to end: open or create a project, build a song in the session grid and the arrangement, edit notes, mix, add and tune devices, save, export audio. 435 tests pass in Debug and Release, including 9 UI cases (interaction and export) and the live-engine model test.

- **Delivered:** session grid, arrangement view, piano roll with velocity lane, mixer, device panels generated from the parameter schema (with sample pickers and drum pad samples), browser, transport bar, menus and shortcuts, save/open (.ddaw package or synthyy JSON), offline export to 24-bit WAV (same graph as live playback), unsaved-changes guard, audio settings. The app starts with a demo song.
- **Found and fixed on the way:** an empty project failed to build, so File > New kept playing the old song; chips ignored clicks away from their parent's origin; a drum test sliced 68 samples past the end of its buffer (random failures when run together); `clip.set` accepted a scene that does not exist.
- **Verified how:** headless snapshots of every view (`ddaw_uishot`), synthetic-event tests of every gesture, a live-engine run of the model, and a launch of the real application (stays up, audio device opens). I did not play audio through speakers or click through the app by hand.
- **Not done (listed in ARCH 17):** modulation UI (LFOs, macros, envelopes, automation lanes), return strips in the mixer, computer-keyboard/MIDI note input, bespoke device panels, recording (A6). Output-device latency compensation and sample-rate change while the app runs (the builder uses the rate it started with) also remain.

**A6 (recording and export), 2026-10-03:** done for audio. 449 tests pass in Debug and Release (421 in the hardened build, parity excluded). Design in ARCH 18.

- **Audio input recording:** arm audio tracks, REC (optional 1-2 bar count-in), Stop. The take streams to disk, then becomes a clip on every armed track, trimmed by the measured latency (device input + output + engine delay + a user offset), as one undo step. Verified with a simulated loopback device at three round-trip latencies, with and without a count-in: the compensated recording lands on the timeline to within the click's attack ramp; the audio thread allocates nothing.
- **Mixdown and stems:** export dialog with range (arrangement, loop region, scene), 44.1/48/96 kHz, 16-bit (TPDF dither)/24-bit/32-bit float, mixdown and/or one stem per source track; progress and cancel. Stems sum back to the unlimited mix to -80 dB.
- **30-minute soak at a 64-frame buffer (`scripts/soak.sh`):** PASS in both modes, Release build, 30 minutes of audio each, with a random musician (launches, tempo, parameters, tracks, effects, notes, instruments, clip moves, undo/redo, 9 whole-project save/reloads). Accelerated: 969 graph swaps, 0 failed builds, 0 audio-thread allocations, no NaN/inf, peak 0.891, worst callback 319 us of 1333 us, memory flat. Paced against the clock on a real-time thread: 3,996 graph swaps, 0 failed builds, 0 allocations, 0 late wakeups, worst callback 978 us (73% of the budget, a graph-swap block; steady-state worst 650 us), memory +12 MB. The 73% worst case is the number to watch (a swap block renders the old and the new graph); `ddaw_swapbench` isolates it (about 190 us on the demo song, more on the soak's 12-track random projects).
- **Found and fixed on the way:** a device restart (opening the input, changing device or rate) silently replaced the live graph with an empty one - the host now tells the model, which rebuilds at the new rate; a project with no tracks could not be built as a live graph (fixed in A5); opening the input needs explicit channels, not "default"; New/Open now finish a take in progress.
- **Not done:** MIDI and computer-keyboard note recording (needs the live note-input path with the note's tick echoed from the audio thread; moved to B2 with the rest of live input), recording into a session slot, punch in/out, multi-channel interfaces beyond one stereo pair. **Not verified:** recording from a real microphone/interface (no input device was exercised; only the loopback simulation and a launch of the app), and a soak on the actual CoreAudio device (the paced soak uses a real-time thread but no driver).

**B2 (tracking path), 2026-10-03:** done. 474 tests pass in Debug and Release (444 in the hardened build, parity excluded). Design in ARCH 19.

- **Analysis:** a streaming resampler to 16 kHz, Magenta's loudness reproduced to within 0.02 dB of `ddsp.spectral_ops.compute_loudness` on a reference signal, YIN pitch (tones 82 Hz-1.2 kHz within 8 cents at 44.1 and 48 kHz, no octave errors on a harmonic tone with a weak fundamental, noise and silence unvoiced, vibrato followed), attack/release and RMS envelope followers, all combined in `PerformanceTracker` (250 frames per second, each stamped with its moment on the input timeline).
- **Measured latency:** a pitch step reaches a frame 29 ms (70 Hz floor), 25 ms (150 Hz) or 21 ms (300 Hz) later; input to audible `follow` synth through the engine 21 ms at 64- and 128-frame callbacks, device latency excluded; PLAN 4.7 updated. Tracker cost: mean 25 us, worst 73 us per 64-frame callback (about 5% of the buffer).
- **In the engine and project:** input -> tracker -> the target track's instrument (new `follow` instrument: sings the tracked pitch, plays notes when nothing is tracked); input monitoring through any audio track's effects (delayed exactly one 128-frame chunk, sample-exact); performance routes (`perf` on a track: pitch, loudness, envelope or confidence drives parameters, snapping back when tracking stops); recording of those curves into automation lanes (latency-compensated, simplified, merged over the recorded span, one undo step); live tracker settings by command. UI: an Input tab (tracker on/off, instrument, pitch floor, live pitch and loudness trace, route list with record toggles), an IN chip on audio-track mixer strips.
- **Soak:** the 30-minute accelerated soak now includes the tracker, the voice follower, performance routes and monitoring under the random edits: PASS (449,903 tracker frames, 2,110 graph swaps, 0 audio-thread allocations, worst callback 368 us of 1333 us, memory flat).
- **Not done:** CREPE-class offline pitch tracking; polyphonic input; MIDI and computer-keyboard note input (needs notes echoed from the audio thread with their tick); recording of the raw curves for the DDSP instrument (B3 reads the tracker directly). **Not verified:** tracking on a real microphone or interface (all tests use synthetic signals and the simulated device). A 10-minute paced real-time soak with B2 in it also passed (1,387 graph swaps, 0 allocations, 0 late wakeups); its worst callback was 1048 us of 1333 us (79%, a graph-swap block), so the swap-block margin noted in A6 is still the number to watch.

**B3, live note input, modulation UI, polyphonic input, 2026-10-04:** done. 505 tests pass in Debug and Release (461 in the hardened build, parity excluded); design in ARCH 6 (B3) and 19.

- **B3 DDSP instrument:** the four Magenta models as a playable device (`ddsp`): noise stage and the learned 48,000-tap reverb ported and verified against DDSP, inference on the device's own thread, played by the tracker (timbre transfer) or by notes, deterministic offline render, latency reported for compensation. Measured: input tone -> audible violin 26 ms (device latency excluded); live 64-frame callbacks 0-3 underrun blocks in 3 s; no audio-thread allocation. The 30-minute accelerated soak now includes a ddsp track: see below.
- **Live note input:** MIDI keyboards (hot-plug) and a computer-keyboard piano feed a polyphonic live path with sustain pedal, retrigger handling and tick-stamped echo; arm a synth or drum track and REC records what you play into a clip, latency compensated.
- **Polyphonic input, both senses:** (1) chords of simultaneous notes from MIDI or the keyboard play polyphonically and record; (2) polyphonic audio input: a multi-pitch estimator turns a chord at the audio input into notes on the live target (triads to five voices, octave pairs, noise-safe), about 81 ms after the chord starts.
- **Modulation UI:** LFOs, macros and an automation-lane editor (track lanes and clip envelopes) in a Modulation tab; macro and LFO knobs are live (no graph rebuild); macros now work without an automation lane.
- **Found and fixed on the way:** TensorFlow's own float32 reverb is 7.8e-4 off the exact convolution (gate set accordingly, with the proof); a macro with a value but no automation lane did nothing; every tracker frame now reaches the instrument (one per chunk, in order) instead of only the newest.
- **Not done:** CREPE-class offline pitch tracking; MPE and pitch bend from controllers (B5); polyphonic tracking that handles a missing fundamental or very close low notes with the short window; the DDSP instrument has no per-model pitch-range protection (out-of-range input is passed to the network as is).

### Graph-swap margin (after B3)

- **Finding.** On the 11-track stress project with the DDSP violin playing, `ddaw_swapbench` (paced, real-time thread) showed swap blocks at 38-51% of the 1333 us budget, but the steady state was as bad: p99.9 1124 us, worst 1425 us. The spikes were the DDSP instrument synthesising a 64-sample hop (harmonic bank, noise filter, 750-partition reverb convolution) on the audio thread every 4 ms, not the swap itself.
- **Change.** Synthesis and the 16 kHz to host-rate conversion moved to the device's worker thread (`src/ddsp/DdspInstrument`, `core/SpscFloatRing.h`); the audio thread only sends feature frames and pops finished samples. Generations tag each run of samples so a reset needs no cross-thread clearing; offline renders wait on a progress counter, so exports stay deterministic. The tracker's YIN difference function is now single precision (mean 15 us per 64-frame callback). Latency reserve went from 128 to 192 samples for the worker's jitter. `Graph::warmUp` was measured again and gives nothing now, so it is not wired into `GraphService`.
- **Result (bench).** Steady worst 1425 -> 330 us (p99.9 291); swap blocks worst 676 -> 338 us, **25% of the budget** (was 51%). Chunk high-water 1385 -> 262 us.
- **Result (soak).** All gates pass: 505/505 tests in Debug and Release, 463/463 hardened (UBSan + libc++ hardening); 30-minute accelerated soak with the DDSP instrument: PASS (1637 swaps, 0 allocations, memory flat; one wall-time outlier of 1790 us that used 269 us of CPU, i.e. preemption); 5-minute paced real-time soak: PASS (700 swaps, 0 late wakeups, 0 over budget).
- **Second pass (swap block).** The retiring graph is now rendered only for a 64-frame crossfade (`kSwapFade`, `Engine.h`) instead of the whole 128-frame chunk: its share of the swap block fell from 379 to 204 us on the soak's projects (75 us on the bench project). Paced 3-minute soak: worst callback 888 us (67%, was 1004 us / 75%), CPU time of the slowest swap block 887 us against 762 us for the slowest ordinary block, so the swap block is now only about 125 us above the heaviest steady chunk. `ddaw_devbench` (new) measures every built-in device alone at its defaults: all are cheap (worst p99 about 140 us for an FM chord, most under 50 us), so what sets the soak's worst chunk is the project's size (it grows to dozens of tracks), not one device. `ddaw_soak` now prints the engine's swap timers.
- **Not met on this machine today.** The `ddsp instrument: live pacing` test (at most 3 underruns in 3 s) fails with 5-20 underruns while Stremio, Synthyy.app and `audioanalyticsd` are busy. Measured cause: the decoder step alone runs at a median of 1.8 ms (p99 4.4 ms) instead of the documented 0.78 ms mean (p99 1.8 ms), because the model is memory-bound (19 MB of weights per frame) and the other processes take the bandwidth; the same test passed (0-3 underruns) earlier on a quieter machine. Re-run it when the machine is quiet. The worker is now a time-constraint thread (mach policy, QoS fallback); that does not help against bandwidth contention. The test retries up to three times and requires the best run to be clean.
- **Still open.** Further swap-block savings would need a shorter fade (32 frames saves about 100 us more, at a harder transition) or work off the audio thread, and the DDSP decoder's memory-bound cost is the real limit of the live DDSP path on a loaded machine.

### Repository, CI and sanitizers (2026-10-05)

- **Git.** The tree is a git repository on `main` (baseline commit; no remote yet, nothing pushed). `.gitignore` excludes build trees, Python environments and `models/`. The synthyy fixtures are committed (75 files, none over 1 MB, 34 MB): plain git is enough, Git LFS is not needed (`git lfs migrate` later if they grow). A fresh clone was configured, built and tested: 475 tests pass (the DDSP tests skip when the weights are absent), including the parity run.
- **CI gate (`.github/workflows/ci.yml`).** Blocking: `scripts/check_no_weights.sh`, then every test except the wall-clock ones (`ctest -LE timing`), including `parity_synthyy`, now `--strict` (all 37 fixtures ported: a regression or an unported fixture fails). The four timing tests (`[timing]`: live pacing, 8-track budget, kitchen-sink budget, tracker cost) run in a non-blocking step. Catch tags are ctest labels (`ADD_TAGS_AS_LABELS`), so `-L`/`-LE timing|threads` select them.
- **Nightly (`nightly.yml`).** The 30-minute soak, `ddaw_swapbench` and `ddaw_devbench` (artifacts); the hardened (UBSan + libc++) build; and a Linux sanitizer job: ThreadSanitizer on the `threads` tests (FIFOs, the sample ring, live notes, graph service, polyphonic input, the live-engine model), and AddressSanitizer + UBSan on the whole suite. A canary step fails the job if TSan does not flag a deliberate race.
- **Not verified: the Linux jobs.** TSan and ASan cannot run on this Mac (Apple clang 16 and Homebrew LLVM 20 both crash or hang on macOS 26), and there is no Linux here, so `nightly.yml` has not run. What was checked instead: the same code builds and passes under GCC 15 with libstdc++ (462 tests, 12 `threads` tests), which found and fixed three missing includes that only libstdc++ reports (`CompKernel.h`, `Scheduler.h`, `devbench.cpp`); the workflow YAML parses. Expect to fix small things on its first run. The DDSP worker handoff is covered through `SpscFloatRing` (new two-thread test), not by running the instrument under TSan.
- **Licences.** `LICENSE` (AGPL-3.0), `NOTICE` (Magenta DDSP, JUCE under its AGPL option, RTNeural, nlohmann/json, Catch2). Checkpoint terms stay unconfirmed; weights are never committed (see the entry above).
- **Tests now.** 503 in Debug and Release (the 2 new `spscring` tests included), 462 in the hardened build (timing tests excluded). Timing tests: pass 3 of 3 runs once the machine was quiet. Accelerated 30-minute soak with the DDSP instrument after the crossfade change: PASS (2028 swaps, 0 allocations, worst CPU 320 us per block; 37 wall-time outliers with no CPU time, i.e. preemption on a loaded machine).

### B4: audio-rate modulation (2026-10-05)

- **Done.** Audio-rate routes (oscillator and track sources, optional envelope follower) into the A-rate ports of a device, with build-time ordering and loop rejection; the `fmop` four-operator FM/AM instrument with 4x oversampling and three A-rate ports; the Audio-rate tab; project JSON, document commands (live depth and rate), soak coverage. Design and verification in ARCH 20.
- **Verified.** 525 tests in Debug and Release (482 in the hardened build), including FM sidebands against Bessel functions, alias rejection (with a negative control), A-rate ports, bit-identical output for callback sizes 1 to 1024, no allocation on the audio thread. 30-minute accelerated soak with the new device and random route edits: PASS (1812 swaps, 0 allocations).
- **Not done.** A-rate ports on the existing 11 instruments and 23 effects (each needs a per-sample path), a bus as a modulation source, per-voice routes, master-effect targets, band-limited audio-rate oscillator shapes. B5 (morph map and controllers) is next.

### B5: morph map and controllers (2026-10-05)

- **Done.** The morph map engine (idw and rbf blends, response curves, live stick) and its editor; controller bindings with learn, driven by the GameController gamepad backend and by MIDI CCs, bend and pressure; MPE routing from MIDI channels to per-note expression, implemented in `fmop`, `poly`, `mono` and `ddsp`. Design and verification in ARCH 21.
- **Verified.** 560+ tests (see ARCH 21 for what each covers); the 30-minute accelerated soak with morph edits, controller input and live expression: PASS (1842 swaps, 0 allocations).
- **Not verified on hardware.** A real gamepad and a real MPE controller: the gamepad backend is exercised with none attached and everything above it with stubbed readings; MPE with synthetic MIDI messages.
- **Open items taken on afterwards (2026-10-05).** Recording and playing back expression in clips (curves on notes, JSON, offline and live playback, takes record them); MPE zone setup and pitch-bend-sensitivity over MIDI (RPNs); the MPE switch, ranges and zones saved with the input settings; bend and pressure in `duo`, `fm`, `keys`, `follow`, `sampler`, `ksampler` and `granular`; up to four gamepads; a rumble pulse when Learn binds a control. Details in ARCH 21.
- **Expression-curve editing (2026-10-06).** Piano-roll lanes for slide, pressure and bend (add, drag, delete, draw, clear; one undo step per gesture), duplicate keeps curves, and curves of notes already sounding now survive a graph swap (re-found by note uid), so an edit while playing takes effect at once.
- **Still not done.** Expression in `pluck` and `drum`; any use of rumble beyond the Learn pulse; one gesture across several notes' curves; copy and paste of curve segments. Hardware checks (a real gamepad, a real MPE controller) remain.

### B6 Synth families (2026-10-06)

Done: `harmnoise` (hand-controlled harmonic + noise), `subtractive` (two PolyBLEP oscillators, sub, noise, ladder filter with its own ADSR, key and velocity tracking), `wavetable` (four built-in banks, mip-mapped, unison and spread), `waveshaper` (six transfer functions, 4x oversampled), `modal` (seven models of up to 24 resonant modes) and `perc` (hybrid body + noise + metal + click). Each passes the generic gates plus analytic tests of every control (closed forms where there are any: partial levels, Bessel spectra of the fold, exact Chebyshev harmonics, mode frequencies and T60), takes MPE expression, and `subtractive`, `wavetable` and `waveshaper` have A-rate ports. The soak now switches tracks among all of them: 30 minutes accelerated, 1738 graph swaps, 0 allocations, 0 NaN, 0 callbacks over budget in CPU time (2 wall-time outliers from scheduler preemption). Verified: Debug and Release 655 tests, hardened (UBSan) 602, GCC 15 with libstdc++ 602 (no app). Details and limits in ARCH 22.
