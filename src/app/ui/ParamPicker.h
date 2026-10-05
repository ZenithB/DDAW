#pragma once
// A popup tree of the parameters a track's modulation can reach: the instrument, each effect, and the mixer.
// Shared by the performance routes, LFOs, macros and automation lanes.
#include <functional>

#include "app/model/AppModel.h"
#include "app/ui/Theme.h"

namespace ddaw::ui {

inline const project::Track* trackOf(app::AppModel& m, project::Uid uid) { return app::edit::findTrack(m.project(), uid); }

struct PickedParam {
    std::string dest, fxId, pkey;   // the "dest|fxId|pkey" key of the modulation grammar
    juce::String label;             // "Cutoff (Poly Synth)"
    ParamSpec spec{};
    double stored = 0;              // the parameter's stored value (or its default): what a macro starts from
};

// Shows the menu next to `anchor`; `done` is called with the choice. Does nothing when the track is gone.
// `audioRateOnly` lists just the parameters that accept audio-rate modulation (ParamSpec::audioRate).
void pickParam(app::AppModel& model, project::Uid track, juce::Component* anchor, std::function<void(const PickedParam&)> done, bool audioRateOnly = false);

// A readable name for a target of `track`, e.g. "Cutoff" or "Reverb Mix" or "Volume".
juce::String describeTarget(const project::Track& track, const project::ModTarget& t);

// Find the spec a target points at (for ranges and normalisation); false when it no longer exists.
bool targetSpec(const project::Track& track, const project::ModTarget& t, ParamSpec& spec, double& stored);

}  // namespace ddaw::ui
