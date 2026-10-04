#pragma once
// What an export renders (JUCE-free, so it is unit tested): the mixdown and, optionally, one stem per
// track. A stem is the track as it sits in the mix (through its own effects, fader, pan and the buses it
// sends to) with every other source muted; the master chain and limiter are left out of stems so the
// stems sum back to the unlimited mix.
#include <string>
#include <vector>

#include "engine/OfflineRender.h"
#include "project/Project.h"

namespace ddaw::app {

struct ExportOptions {
    enum class Range { Auto, Arrangement, LoopRegion, Scene };
    Range range = Range::Auto;       // Auto: the arrangement when there is one, otherwise the scene
    std::string sceneId;             // Scene (or Auto without an arrangement): empty = the first
    double sampleRate = 48000.0;
    int bitDepth = 24;               // 16, 24 or 32 (float)
    bool dither = true;              // TPDF, 16-bit only
    bool mixdown = true;
    bool stems = false;
};

struct ExportJob {
    std::string suffix;              // "" for the mixdown, otherwise "-<track name>"
    std::string label;
    project::Fixture fixture;
    engine::RenderOptions render;
};

// Throws nothing: `error` is set (and the list empty) when there is nothing to export.
std::vector<ExportJob> planExport(const project::Project& p, const ExportOptions& o, std::string& error);

// A file-name-safe version of a track name ("Lead / 2" -> "Lead-2").
std::string safeFileName(const std::string& s);

}  // namespace ddaw::app
