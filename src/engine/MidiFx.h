#pragma once
// MIDI effects: a clip's notes run through a track's Scale / Chord / Arp / Velocity / Random chain
// before scheduling. Pure note-list transforms; port of sf-engine midifx.rs (itself an exact port of
// synthyy's src/audio/midifx.ts). Runs on the builder thread, never the audio thread.
#include <string>
#include <vector>

#include "engine/NoteEvent.h"
#include "project/Project.h"

namespace ddaw::engine {

struct MidiFxContext {
    std::string trackId;          // seeds the per-track RNG (FNV-1a), so rolls decorrelate between tracks
    bool isDrum = false;          // scale/chord/arp skip drum tracks; velo/rand do not
    double root = 9;              // project key root (meta.root)
    std::string scale = "minor";  // project scale id (meta.scale); unknown ids fall back to major
    double loopLen = 384;         // the clip's loop length in ticks
};

// Expand `notes` through `chain` (disabled devices are skipped, unknown types pass through) and
// flatten to events sorted by tick. An empty chain is the identity. `exprOf` (optional, parallel to `notes`) gives each
// note's expression-curve index; it travels with the note through the effects (chord tones share it; arpeggio notes have none).
std::vector<NoteEv> expandMidi(const std::vector<project::DeviceSpec>& chain,
                               const std::vector<project::Note>& notes, const MidiFxContext& ctx, const std::vector<int>* exprOf = nullptr);

}  // namespace ddaw::engine
