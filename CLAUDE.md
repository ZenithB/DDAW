# DDAW

C++20 / JUCE 8 desktop audio workstation built around DDSP synthesis (macOS, Apple Silicon, AGPL-3.0).

**Start here:** `docs/PLAN.md` (decisions, milestones, risks), then `docs/ARCH.md` (binding architecture contract; when code and contract disagree, fix the code).

## Context

- synthyy (`~/Documents/synthyy`, Rust/Tauri) is the read-only reference implementation. Its golden fixtures are the port tests. Never write to that repo.
- Status (2026-10-06): milestones M0, A1-A6, B1-B6 and the sidechain are done; L1 (plugin hosting) is started (ARCH 23). The honest list of what is not done or not verified is at the end of docs/PLAN.md. Next candidates: out-of-process plugin hosting, L2 custom training, hardware checks.

## Hard rules

- No allocation, locks, syscalls or exceptions on the audio thread. Neural inference never runs on the audio thread.
- One graph for live playback and offline render.
- One device per file; never edit shared registries or build files from a device task.
- Every device passes its gates (unit tests, fixture or reference comparison, no-allocation check) before merge.
- Commit and push only when asked.
