# DDAW

C++20 / JUCE 8 desktop audio workstation built around DDSP synthesis (macOS, Apple Silicon, AGPL-3.0).

**Start here:** `docs/PLAN.md` (decisions, milestones, risks), then `docs/ARCH.md` (binding architecture contract; when code and contract disagree, fix the code).

## Context

- synthyy (`~/Documents/synthyy`, Rust/Tauri) is the read-only reference implementation. Its golden fixtures are the port tests. Never write to that repo.
- Current milestone: M0 (repo scaffold, CMake, JUCE 8, test framework, fixture harness, finalise ARCH.md) together with B1 (Magenta DDSP decoder spike: checkpoint to RTNeural, Python vs C++ null test, latency measurement).

## Hard rules

- No allocation, locks, syscalls or exceptions on the audio thread. Neural inference never runs on the audio thread.
- One graph for live playback and offline render.
- One device per file; never edit shared registries or build files from a device task.
- Every device passes its gates (unit tests, fixture or reference comparison, no-allocation check) before merge.
- Commit and push only when asked.
