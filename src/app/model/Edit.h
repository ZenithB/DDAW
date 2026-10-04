#pragma once
// Command builders for the edits the UI makes. Pure functions of (project, intent) -> document Command,
// so every gesture is testable without a window and every edit goes through undo/redo.
#include <optional>
#include <string>

#include "document/Document.h"

namespace ddaw::app::edit {

using document::Command;
using project::Project;
using project::Uid;

constexpr double kPpq = 96.0;
constexpr double kBarTicks = 384.0;  // 4/4

// A clip addressed the way the document commands expect: session (track uid + scene id) or arrangement.
struct ClipRef {
    Uid track = 0;
    std::string scene;     // session clip
    std::string arrKey;    // arrangement clip (wins when set)
    bool valid() const { return !arrKey.empty() || (track != 0 && !scene.empty()); }
    bool operator==(const ClipRef&) const = default;
    nlohmann::json toJson() const;
};
const project::Clip* findClip(const Project& p, const ClipRef& r);
const project::Track* findTrack(const Project& p, Uid uid);
const project::Track* findTrackById(const Project& p, const std::string& id);

std::string uniqueTrackId(const Project& p);
std::string uniqueSceneId(const Project& p);
std::string uniqueArrKey(const Project& p);
std::string uniqueDeviceId(const Project& p, const std::string& type);

// ---- tracks and scenes ----
Command addTrack(const Project& p, project::TrackKind kind, std::string name = "");
Command removeTrack(Uid uid);
Command addScene(const Project& p);
Command removeScene(const std::string& sceneId);

// ---- devices ----
// chain: "fx" | "midifx" | "master". index < 0 appends.
Command addDevice(const Project& p, Uid track, const std::string& chain, const std::string& type, int index = -1);
Command removeDevice(Uid device);
Command setInstrument(const Project& p, Uid track, const std::string& type);

// ---- clips and notes ----
Command newSessionClip(Uid track, const std::string& scene, double lenTicks = kBarTicks);
Command clearSessionClip(Uid track, const std::string& scene);
Command newArrClip(const Project& p, Uid track, double startTick, double lenTicks = kBarTicks);
Command moveArrClip(const Project& p, const std::string& key, double startTick, std::optional<Uid> toTrack = std::nullopt);
Command resizeArrClip(const Project& p, const std::string& key, double lenTicks);
Command removeArrClip(const std::string& key);
Command setClipLength(const ClipRef& r, const Project& p, double lenTicks);

Command addNote(const ClipRef& r, int pitch, double start, double dur, double vel = 0.8);
Command editNote(const ClipRef& r, const project::Note& n);
Command removeNote(const ClipRef& r, Uid noteUid);

// ---- helpers ----
double snapTicks(double ticks, double grid);  // nearest multiple of `grid` (grid <= 0: unchanged)
double snapFloor(double ticks, double grid);

}  // namespace ddaw::app::edit
