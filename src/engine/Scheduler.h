#pragma once
// Event scheduler: pre-expanded note lists (session clip patterns looping from a launch anchor;
// bounded arrangement events) fired sample-accurately; the render loop splits blocks at the next fire
// time. Swing mirrors Tone's Transport swing math; humanise mirrors engine-tone's makePart jitter,
// both applied at fire time exactly as synthyy's render does. Port of sf-engine scheduler.rs.
//
// Everything here is allocation-free after the patterns are built (builder thread).
#include <cstddef>
#include <optional>
#include <vector>

#include "engine/NoteEvent.h"

namespace ddaw::engine {

enum class TransportMode : uint8_t { Session = 0, Arrangement = 1 };

// A session slot holding an audio clip: index into the track's derived clips, plus loop metadata.
struct AudioSlotRef { int clip = 0; double loopLen = 384; bool looped = false; };

struct ClipPattern {
    double loopLen = 384;
    std::vector<NoteEv> events;  // sorted by tick
};

struct SchedParams {
    double swing = 0;
    double swingTicks = 24;  // subdivision in ticks: "16n" = 24, "8n" = 48 at PPQ 96
    double humanize = 0;
};

// Tone Transport swing: events on odd swingSubdivision positions are pushed late by
// sin(progress*pi) * swing * (subdiv*2/3) ticks; quarter-note downbeats and even subdivisions stay.
double swingOffsetTicks(double absTick, double swing, double swingTicks);
// meta.swingSubdivision ("8n" | "16n", default "16n").
double swingSubdivTicks(const std::string& subdiv);

struct Pending {
    double fireTick = 0;
    uint8_t pitch = 0;
    double durTicks = 0;
    float vel = 0;
    // False for the placeholder that bounds peek() work when a pathological pattern (every note
    // failing its probability roll) never yields a firing note.
    bool audible = true;
};

class TrackSched {
public:
    // ---- build side ----
    std::vector<std::optional<ClipPattern>> session;  // per scene index (nullopt = empty slot)
    std::vector<NoteEv> arr;                          // arrangement events at absolute ticks, sorted
    // Session audio clips (audio tracks): launchable like note clips, played by AudioPlayer.
    std::vector<std::optional<AudioSlotRef>> audioSession;

    // ---- audio side (real-time safe) ----
    // Launch the clip of `scene`, looping from `anchor` ticks (session mode).
    void launch(size_t scene, double anchor) noexcept;
    void stopClip() noexcept { launched_.reset(); pending_.reset(); }
    std::optional<size_t> launchedScene() const noexcept { return launched_; }
    // The launched scene's audio slot, if it holds an audio clip.
    std::optional<AudioSlotRef> audioSlot() const noexcept {
        return launched_ && *launched_ < audioSession.size() ? audioSession[*launched_] : std::nullopt;
    }
    double anchor() const noexcept { return anchor_; }
    // Reset cursors after a position jump (play-from, loop wrap).
    void relocate(TransportMode mode, double now) noexcept;
    // Ensure `pending` holds the next event at/after `now` and return its fire tick, or nullopt.
    // Probability and humanise are rolled here, once per firing.
    std::optional<double> peek(TransportMode mode, double now, const SchedParams& p, XorShift& rng) noexcept;
    // Take the pending event if it is due (fireTick <= dueTick).
    std::optional<Pending> takeDue(double dueTick) noexcept;

private:
    std::optional<size_t> launched_;
    double anchor_ = 0, loopIter_ = 0;
    size_t nextIdx_ = 0, arrIdx_ = 0;
    std::optional<Pending> pending_;
};

}  // namespace ddaw::engine
