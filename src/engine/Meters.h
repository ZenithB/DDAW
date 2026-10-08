#pragma once
// Meters: a fixed bank of atomics the audio thread writes and the UI polls (ARCH 1). One bank per
// Engine, sized at compile time so a graph swap never reallocates or invalidates a reader.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ddaw::engine {

constexpr int kMaxMeterTracks = 128;
constexpr float kMeterReleaseSeconds = 0.3f;  // peak-hold fall time, as in synthyy
constexpr int kMaxFxMeters = 8;    // effects per strip whose gain reduction is metered (the strip's first eight)
constexpr int kMaxBands = 3;       // meters per effect (the multiband compressor's bands)

struct MeterSnapshot {
    float trackPeak[kMaxMeterTracks]{}, trackRms[kMaxMeterTracks]{};
    int   trackScene[kMaxMeterTracks];         // launched session scene per track, -1 none
    double trackAnchor[kMaxMeterTracks]{};     // its launch anchor in ticks (> playhead: still queued)
    float masterPeak = 0, masterRms = 0, limiterGrDb = 0;
    double playheadTicks = 0;
    bool playing = false;
};

class MeterBank {
public:
    MeterBank() { for (auto& a : trackScene_) a.store(-1, std::memory_order_relaxed); }
    // Per-block decay for a decaying peak hold. Computed once per block by the engine.
    static float decay(int frames, double sampleRate) noexcept {
        return std::exp(-float(frames) / (float(sampleRate) * kMeterReleaseSeconds));
    }
    static void peakRms(const float* l, const float* r, int n, float& peak, float& rms) noexcept {
        float p = 0.0f; double sq = 0.0;
        for (int i = 0; i < n; ++i) {
            p = std::max({p, std::abs(l[i]), std::abs(r[i])});
            sq += 0.5 * (double(l[i]) * l[i] + double(r[i]) * r[i]);
        }
        peak = p;
        rms = n ? static_cast<float>(std::sqrt(sq / n)) : 0.0f;
    }

    // Audio thread. Peak decays; RMS is the block value.
    void setTrack(int i, float peak, float rms, float decayPerBlock) noexcept {
        if (i < 0 || i >= kMaxMeterTracks) return;
        store(trackPeak_[size_t(i)], std::max(peak, load(trackPeak_[size_t(i)]) * decayPerBlock));
        store(trackRms_[size_t(i)], rms);
    }
    void setMaster(float peak, float rms, float grDb, float decayPerBlock) noexcept {
        store(masterPeak_, std::max(peak, load(masterPeak_) * decayPerBlock));
        store(masterRms_, rms);
        store(grDb_, grDb);
    }
    // Gain reduction of a track's effect (track -1: the master chain), in dB <= 0. Audio thread writes, anyone reads.
    void setFxGr(int track, int fx, int band, float db) noexcept {
        if (track < -1 || track >= kMaxMeterTracks || fx < 0 || fx >= kMaxFxMeters || band < 0 || band >= kMaxBands) return;
        store(fxGr_[idx(track, fx, band)], db);
    }
    float fxGr(int track, int fx, int band) const noexcept {
        if (track < -1 || track >= kMaxMeterTracks || fx < 0 || fx >= kMaxFxMeters || band < 0 || band >= kMaxBands) return 0.0f;
        return load(fxGr_[idx(track, fx, band)]);
    }
    void clearFx() noexcept { for (auto& a : fxGr_) store(a, 0.0f); }   // after a graph swap: the old effects are gone
    void setTrackScene(int i, int scene, double anchorTicks) noexcept {
        if (i < 0 || i >= kMaxMeterTracks) return;
        trackScene_[size_t(i)].store(scene, std::memory_order_relaxed);
        std::uint64_t b; std::memcpy(&b, &anchorTicks, 8);
        trackAnchor_[size_t(i)].store(b, std::memory_order_relaxed);
    }
    void setTransport(double ticks, bool playing) noexcept {
        std::uint64_t b; std::memcpy(&b, &ticks, 8);
        playhead_.store(b, std::memory_order_relaxed);
        playing_.store(playing, std::memory_order_relaxed);
    }
    void clearTracks(int from) noexcept {  // tracks that no longer exist after a graph swap
        for (int i = std::max(from, 0); i < kMaxMeterTracks; ++i) { store(trackPeak_[size_t(i)], 0); store(trackRms_[size_t(i)], 0); }
    }

    // Any thread.
    MeterSnapshot snapshot() const noexcept {
        MeterSnapshot s;
        for (int i = 0; i < kMaxMeterTracks; ++i) {
            s.trackPeak[i] = load(trackPeak_[size_t(i)]); s.trackRms[i] = load(trackRms_[size_t(i)]);
            s.trackScene[i] = trackScene_[size_t(i)].load(std::memory_order_relaxed);
            std::uint64_t b = trackAnchor_[size_t(i)].load(std::memory_order_relaxed); std::memcpy(&s.trackAnchor[i], &b, 8);
        }
        s.masterPeak = load(masterPeak_); s.masterRms = load(masterRms_); s.limiterGrDb = load(grDb_);
        std::uint64_t b = playhead_.load(std::memory_order_relaxed); std::memcpy(&s.playheadTicks, &b, 8);
        s.playing = playing_.load(std::memory_order_relaxed);
        return s;
    }

private:
    static void store(std::atomic<std::uint32_t>& a, float v) noexcept { std::uint32_t b; std::memcpy(&b, &v, 4); a.store(b, std::memory_order_relaxed); }
    static float load(const std::atomic<std::uint32_t>& a) noexcept { std::uint32_t b = a.load(std::memory_order_relaxed); float v; std::memcpy(&v, &b, 4); return v; }
    static size_t idx(int track, int fx, int band) noexcept { return (size_t(track + 1) * kMaxFxMeters + size_t(fx)) * kMaxBands + size_t(band); }
    std::array<std::atomic<std::uint32_t>, size_t(kMaxMeterTracks + 1) * kMaxFxMeters * kMaxBands> fxGr_{};
    std::array<std::atomic<std::uint32_t>, kMaxMeterTracks> trackPeak_{}, trackRms_{};
    std::array<std::atomic<int>, kMaxMeterTracks> trackScene_{};
    std::array<std::atomic<std::uint64_t>, kMaxMeterTracks> trackAnchor_{};
    std::atomic<std::uint32_t> masterPeak_{0}, masterRms_{0}, grDb_{0};
    std::atomic<std::uint64_t> playhead_{0};
    std::atomic<bool> playing_{false};
};

}  // namespace ddaw::engine
