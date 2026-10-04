#pragma once
// Offline export (mixdown and stems) to WAV through the same graph the live engine plays.
#include <atomic>
#include <functional>
#include <vector>

#include <juce_core/juce_core.h>

#include "app/model/ExportPlan.h"
#include "project/Project.h"
#include "project/SampleBank.h"

namespace ddaw::ui {

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    juce::String message;                  // "Exported name.wav (12.3 s)" or the failure reason
    double seconds = 0;                    // length of the (first) file
    size_t unsupported = 0;                // features the renderer could not honour
    std::vector<juce::File> files;         // what was written
};

// Renders every job of the plan and writes one file each: the mixdown to `base`, stems next to it as
// "<base name>-<track>.wav". Blocking: run on a worker thread. `progress(fraction, label)` is called from
// that thread; setting `*cancel` stops at the next callback and removes the partial file.
ExportResult exportProject(const project::Project& p, const project::SampleBank& bank, const app::ExportOptions& options, const juce::File& base,
                           std::atomic<bool>* cancel = nullptr, std::function<void(double, const juce::String&)> progress = {});

// The simple form: the mixdown at a given rate, 24-bit.
ExportResult exportAudio(const project::Project& p, const project::SampleBank& bank, const std::string& sceneId, const juce::File& out,
                         double sampleRate = 48000.0);

}  // namespace ddaw::ui
