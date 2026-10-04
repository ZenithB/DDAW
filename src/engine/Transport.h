#pragma once
// Transport: ticks at PPQ 96 (ARCH 2), owned by the audio thread. Absolute frame counting makes
// event placement sample-accurate and repeatable between live and offline runs:
//   frame(tick) = anchorFrame + llround((tick - anchorTick) * framesPerTick)
// A tempo change re-anchors at the current position so earlier events keep their frames.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/Constants.h"
#include "engine/Scheduler.h"

namespace ddaw::engine {

class Transport {
public:
    void prepare(double sampleRate) noexcept { sr_ = sampleRate; nowFrame_ = 0; stop(); updateFpt(); }

    void setTempo(double bpm) noexcept {
        if (bpm <= 0.0) return;
        if (playing_) { anchorTick_ = positionTicks(); anchorFrame_ = nowFrame_; }
        bpm_ = bpm;
        updateFpt();
    }
    void play(double fromTicks, TransportMode mode = TransportMode::Session) noexcept {
        mode_ = mode;
        playing_ = true;
        anchorTick_ = fromTicks;   // may be negative: a count-in before the song starts
        anchorFrame_ = nowFrame_;
    }
    void stop() noexcept {
        if (playing_) anchorTick_ = positionTicks();  // hold the playhead where playback stopped
        playing_ = false;
    }

    bool   playing() const noexcept { return playing_; }
    TransportMode mode() const noexcept { return mode_; }
    // Arrangement loop region. Wrapping is applied by wrapIfNeeded() after each advance.
    bool   loopEnabled = false;
    double loopStart = 0.0, loopEnd = 4.0 * 384.0;
    double bpm() const noexcept { return bpm_; }
    double framesPerTick() const noexcept { return fpt_; }
    int64_t nowFrame() const noexcept { return nowFrame_; }
    // Position at the first frame not yet processed. Held at the last position while stopped.
    double positionTicks() const noexcept {
        return playing_ ? anchorTick_ + double(nowFrame_ - anchorFrame_) / fpt_ : anchorTick_;
    }
    // Frames from now until `tick` (<= 0 means due now or overdue).
    int64_t framesUntilTick(double tick) const noexcept {
        return anchorFrame_ + std::llround((tick - anchorTick_) * fpt_) - nowFrame_;
    }
    // Timeline position of absolute frame `f`, valid for f >= anchorFrame() (the frame the current anchor was set).
    int64_t anchorFrame() const noexcept { return anchorFrame_; }
    double tickAtFrame(int64_t f) const noexcept { return anchorTick_ + double(f - anchorFrame_) / fpt_; }
    void advance(int frames) noexcept { nowFrame_ += frames; }
    // If the playhead reached the loop end (arrangement mode), jump back into the loop. Returns true
    // when it wrapped, so the schedulers can relocate their cursors.
    bool wrapIfNeeded() noexcept {
        if (!playing_ || mode_ != TransportMode::Arrangement || !loopEnabled || loopEnd <= loopStart) return false;
        const double pos = positionTicks();
        if (pos < loopEnd - 1e-9) return false;
        const double len = loopEnd - loopStart;
        anchorTick_ = loopStart + std::fmod(pos - loopStart, len);
        anchorFrame_ = nowFrame_;
        return true;
    }

private:
    void updateFpt() noexcept { fpt_ = sr_ * 60.0 / bpm_ / kPpq; }
    double  sr_ = 48000.0, bpm_ = 120.0, fpt_ = 0.0, anchorTick_ = 0.0;
    int64_t nowFrame_ = 0, anchorFrame_ = 0;
    bool    playing_ = false;
    TransportMode mode_ = TransportMode::Session;
};

}  // namespace ddaw::engine
