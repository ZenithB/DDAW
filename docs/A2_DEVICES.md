# A2: device ports

Port synthyy's instruments and effects to C++ against the golden fixtures. Read `docs/PLAN.md` (section 6, 9)
and `docs/ARCH.md` (section 4, 12, 13) first. This file is the task brief for every device port.

## State

**Done (2026-10-02):** every device that has a fixture is ported and gated: 31 devices, 31 of 31 fixtures passing baseline
gating. See `docs/PLAN.md` section 10 for the result, the unported fixtures (all A3) and the follow-ups the ports
surfaced. The sections below are the standing brief for any further device port (`sampler`, `ksampler`, `granular`,
`audiobus`, or a new device) and are kept as written.

With `--limiter tone --no-trim` the C++ pipeline reproduces the Rust port's scores to three digits, for example:

| Fixture | C++ spectral | Rust baseline | C++ level (dBFS) | Rust level (dBFS) |
|---|---|---|---|---|
| inst-mono | 0.9785 | 0.9785 | -11.0 | -11.04 |
| fx-comp | 0.9917 | 0.991 | -14.1 | -14.11 |

Shared groundwork (orchestrator-owned, do not edit from a device task): `src/dsp/*`, `src/devices/schema/Schema.generated.h`
(all parameter tables, generated from the Rust registry, including the `rateMode`/`rateSync`/`rateHi` params of
rate-switch effects), the engine, the harness, CMake, and `tests/devices/DeviceTestKit.h`.

**Parallel workflow used:** `scripts/agent_workspace.sh <name>` makes a private build copy (the tree is not under git and
sources are globbed, so one half-written file would break every other build); `scripts/agent_deliver.sh <name> <kind> <type>...`
copies only that device's two files into the main tree and refuses to overwrite unless `DDAW_DELIVER_UPDATE=1`.

## What to port

| Instrument | Rust lines | Fixture | Rust baseline (spectral / level) |
|---|---|---|---|
| `poly` | 345 | `inst-poly` | 0.978 / +0.7 dB |
| `duo` | 306 | `inst-duo` | 0.977 / +1.3 dB |
| `fm` | 276 | `inst-fm` | 0.957 / -0.0 dB |
| `keys` | 285 | `inst-keys` | 0.992 / -0.0 dB |
| `pluck` | 269 | `inst-pluck` | 0.933 / -2.5 dB |
| `drum` | 457 | `inst-drum` | 0.861 / +1.1 dB |
| `sampler` | 211 | none | - |
| `ksampler` | 213 | none | - |
| `granular` | 472 | none | - |
| `audiobus` | 30 | none | - |

| Effect | Rust lines | Fixture | Rust baseline (spectral / level) |
|---|---|---|---|
| `eq` | 137 | `fx-eq` | 0.978 / +1.4 dB |
| `filter` | 173 | `fx-filter` | 0.987 / +1.4 dB |
| `delay` | 147 | `fx-delay` | 0.983 / +1.4 dB |
| `reverb` | 240 | `fx-reverb` | 0.989 / +1.7 dB |
| `plate` | 264 | `fx-plate` | 0.983 / +0.9 dB |
| `eq7` | 241 | `fx-eq7` | 0.985 / +1.4 dB |
| `chorus` | 187 | `fx-chorus` | 0.982 / +1.2 dB |
| `dist` | 98 | `fx-dist` | 0.973 / +0.2 dB |
| `crush` | 138 | `fx-crush` | 0.839 / +1.3 dB |
| `opto` | 423 | `fx-opto` | 0.938 / +0.7 dB |
| `mbcomp` | 390 | `fx-mbcomp` | 0.978 / +0.9 dB |
| `gate` | 480 | none | - |
| `phaser` | 230 | `fx-phaser` | 0.982 / +1.4 dB |
| `pingpong` | 162 | `fx-pingpong` | 0.986 / +2.9 dB |
| `autofilt` | 182 | `fx-autofilt` | 0.982 / +1.1 dB |
| `trem` | 169 | `fx-trem` | 0.980 / +1.9 dB |
| `autopan` | 156 | `fx-autopan` | 0.987 / +6.7 dB |
| `vib` | 185 | `fx-vib` | 0.980 / +1.4 dB |
| `cheby` | 109 | `fx-cheby` | 0.928 / +6.2 dB |
| `widen` | 60 | `fx-widen` | 0.975 / +55.7 dB |
| `shift` | 168 | `fx-shift` | 0.980 / +1.4 dB |
| `duck` | 181 | `fx-duck` | 0.981 / +2.2 dB |
| `autotune` | 341 | `fx-autotune` | 0.981 / +1.5 dB |

Fixtures that need A3 (scheduler and modulation) rather than a device: `feat-automation` (clip envelopes),
`feat-sends` (sends and buses), `feat-swing`, `midi-arp`, `midi-chord`, `midi-velo`. They stay `unported` until A3.
`fx-widen` has a pathological (silent) golden; its level check is skipped automatically.
`sampler`, `ksampler`, `granular` need a sample bank and are built in A4 (see PLAN section 5, A4); `audiobus` is a routing
device decided with them. They have no fixtures.

## Recipe for one device

1. Read the Rust source `crates/sf-dsp/src/{inst,fx}/<type>.rs` (read-only; never write to the synthyy repo). The
   `//!` header documents the browser behaviour being matched. Port operation for operation; keep the constants.
2. Create `src/devices/{instruments,effects}/<type>.cpp`. Everything lives in that one file, in an anonymous
   namespace, plus a factory the registry finds by pattern:
   ```cpp
   namespace ddaw::devices {
   std::unique_ptr<EffectDevice> make_<type>() { return std::make_unique<MyFx>(); }   // or InstrumentDevice
   }
   ```
   The line must start at column 0 exactly as above. CMake globs the file and generates the registry, so no shared
   file is edited. Copy `src/devices/instruments/mono.cpp` or `src/devices/effects/comp.cpp` as the template.
3. `params()` returns the generated table (`schema::kInstMono`, `schema::kFxComp`, ...). Parameter indices are the
   table positions; define a local `enum P : uint16_t` in table order. Keys must stay the schema keys.
4. Interface differences from the Rust traits: `prepare(sr, maxBlock)`, `setParam(index, value)`,
   instruments take `noteOn(pitch, vel, noteId)` / `noteOff(noteId)` (no `trigger(dur)`: the scheduler sends the off
   at the right frame). A monophonic instrument must ignore the off of a superseded note id (see `mono`).
   `process` is additive for instruments and in-place for effects. Tempo-synced devices read `ProcessContext`
   (`positionTicks`, `bpm`, `playing`); sidechain `trigger()` has no equivalent yet (A3), leave a TODO and report it.
5. Latency: if the Rust device has a lookahead or pre-delay, return it from `latencySamples()` (`comp` does).
6. Write `tests/devices/<type>_test.cpp` (picked up by glob). It must call `checkEffectContract(make)` or
   `checkInstrumentContract(make)` from `DeviceTestKit.h` and add at least one analytic check of the device's
   defining behaviour (pitch, corner frequency, delay time, gain reduction, ...). `mono_test.cpp` and
   `comp_test.cpp` show the shape.
7. Run the parity gate for your fixture (below) and record the numbers in your report.

## Rules (CLAUDE.md, restated)

- No allocation, locks, syscalls or exceptions on the audio thread. Allocate in the constructor or `prepare`.
  The kit checks this with the allocation hook; every device must pass it.
- Every audible control-rate parameter is smoothed (default 15 ms) unless stepped; follow the Rust device.
- Nonlinear devices (`dist`, `crush`, `cheby`) oversample 4x with `dsp::Oversampler4`.
- Use the shared utilities in `src/dsp/`. If one is missing or wrong, report it; do not edit it and do not
  reimplement it privately. A device task edits only its own `src/devices/.../<type>.cpp` and
  `tests/devices/<type>_test.cpp`.
- No `-ffast-math`, no `std::mutex`, `new`/`delete`, `std::vector` growth or `std::string` in `process` or `setParam`.
- Never write to `~/Documents/synthyy`. Commit and push only when asked.

## Gates (a device merges only when all pass)

1. Unit tests: `ctest --test-dir build -R <type>` (contract kit plus analytic checks).
2. Parity: gated against the Rust port's own scores, because the goldens are Tone.js composites with no phase
   alignment (synthyy docs/PARITY.md), so an exact null is not achievable.
   ```sh
   ./scripts/sync_synthyy_fixtures.sh                 # once; copies fixtures and writes baseline.txt
   ./build/ddaw_parity tests/fixtures/synthyy --limiter tone --no-trim
   ```
   Your fixture must show `pass` (spectral >= baseline - 0.02, level delta within 1 dB of the baseline's). Aim to
   match the baseline to about 0.005 and 0.3 dB, as `mono` and `comp` do. `ctest -R parity_synthyy` runs the whole set
   and fails on any regression.
3. No-allocation: part of the kit.
4. Report: spectral and level numbers, any deviation from the Rust source and why, and anything blocked.

If your numbers sit well below the baseline, debug with `--dump <dir>` (writes each render as a WAV in the golden's
convention) and `--limiter bypass` to separate device error from limiter behaviour. The first `mono` run was 2.6 dB
low and spectrally off, and the cause was the scheduler and master limiter rather than the device; check those
before suspecting the port.

## Regenerating generated files

`python3 tools/a2/gen_schema.py` rewrites the parameter tables from synthyy's registry. The device registry is
regenerated by CMake on every configure.
