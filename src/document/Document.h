#pragma once
// The document model (ARCH 8): one project, edited only through serialisable commands, with stable
// object ids and undo/redo. The UI never touches engine state; it issues commands, and the change
// report tells the engine session what to do: push a live parameter value (no rebuild) or rebuild
// the graph from a snapshot.
//
// A command is data ({kind, args}): it can be logged, replayed, sent over a wire, and, with its
// inverse, undone. This is the seam for a future CRDT layer. Applying a command either succeeds
// completely or throws std::invalid_argument and leaves the document untouched.
//
// Commands (args are JSON; objects are addressed by uid, clips by (trackUid, sceneId)):
//   meta.set      {field, value}                       title bpm swing swingSubdivision humanize root scale
//                                                      launchQ masterGain loopOn loopStart loopEnd tsTop tsBottom
//   track.insert  {index, track, clips?, arr?}         the inverse of track.remove
//   track.remove  {uid}
//   track.set     {uid, field, value}                  name kind gain pan mute solo sendA sendB output send
//   track.move    {uid, index}
//   device.insert {chain: "fx"|"midifx"|"master", track?, index, device}
//   device.remove {uid}
//   device.set    {uid, field, value}                  on out id srcTrack srcPitch
//   device.param  {uid, key, value|null}               null removes the stored value
//   inst.set      {track, device}                      replace the instrument
//   scene.insert  {index, id, clips?} / scene.remove {id}
//   clip.set      {track, scene, clip|null}
//   arrclip.set   {key, arr|null}                      arr = {trackId, start, clip}
//   note.add      {clip, note, index?} / note.remove {clip, uid} / note.edit {clip, uid, note}
//                                                      clip = {track, scene} or {arr: key}
//   env.set       {scope, key, points|null}            scope = {track, scene} | {track} | {master: true} | {arr: key}
//   lfo.insert    {track, index, lfo} / lfo.remove {track, index} / lfo.edit {track, index, lfo}
//   macro.value   {track, index, value}                the macro knob, live (no rebuild)
//   lfo.field     {track, index, field, value}         depth | hz | phase of an LFO, live (no rebuild)
//   perf.insert   {track, index, perf} / perf.remove {track, index} / perf.edit {track, index, perf}   performance routes (B2)
//   binding.insert {index, binding} / binding.remove {index} / binding.edit {index, binding}   controller bindings (B5)
//   morph.insert  {track, index, morph} / morph.remove {track, index} / morph.edit {track, index, morph}   morph maps (B5)
//   morph.pos     {track, index, x, y}                 the stick of a morph map, live (no rebuild)
//   arate.insert  {track, index, arate} / arate.remove {track, index} / arate.edit {track, index, arate}   audio-rate routes (B4)
//   arate.field   {track, index, field, value}         depth | hz of an audio-rate route, live (no rebuild)
//   macro.insert  {track, index, macro} / macro.remove {track, index} / macro.edit {track, index, macro}
//   return.insert {index, ret} / return.remove {index} / return.edit {index, ret}
//   compound      {label, commands: [...]}
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "project/Project.h"

namespace ddaw::document {

struct Command {
    std::string kind;
    nlohmann::json args = nlohmann::json::object();
};

// A parameter value the live engine can take without a rebuild. `key` is a resolver key
// ("<trackId>|inst|<key>", "<trackId>|<fxId>|<key>", "master|<fxId>|<key>", "<trackId>|mix|gain", ...).
struct ParamEdit {
    std::string key;
    double value = 0;
};

struct ChangeInfo {
    bool structural = false;                 // the graph must be rebuilt from a new snapshot
    std::vector<ParamEdit> params;           // live-pushable edits (only when not structural)
    std::optional<double> tempo;             // meta.bpm changed
    bool changed() const { return structural || !params.empty() || tempo.has_value(); }
};

class Document {
public:
    Document();
    // Takes ownership of a project; any object without a uid gets one (nextUid is the counter to resume from).
    explicit Document(project::Project p, project::Uid nextUid = 1);

    const project::Project& project() const { return p_; }
    project::Uid nextUid() const { return nextUid_; }
    uint64_t revision() const { return revision_; }  // bumps on every change, including undo and redo
    std::shared_ptr<const project::Project> snapshot() const { return std::make_shared<const project::Project>(p_); }

    // Apply a command. Returns what changed; `applied` is the normalised command (uids filled in) that
    // was actually executed, which is what a log or a collaborator should record.
    ChangeInfo apply(const Command& c, Command* applied = nullptr);
    // Execute without recording history: for values that stream in from a controller (a stick moving 60 times a second
    // must not fill the undo stack). The document changes and is marked modified; the change report is returned.
    ChangeInfo applyTransient(const Command& c);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    ChangeInfo undo();
    ChangeInfo redo();
    void clearHistory() { undo_.clear(); redo_.clear(); }

    // Transactions: commands applied between begin and end form one undo step.
    void beginGroup(const std::string& label);
    ChangeInfo endGroup();
    const std::string& undoLabel() const;

    // ---- lookups (by stable id) ----
    const project::Track* findTrack(project::Uid uid) const;
    const project::DeviceSpec* findDevice(project::Uid uid) const;

private:
    struct Step { Command forward, inverse; std::string label; };
    struct Exec { Command forward, inverse; ChangeInfo info; };
    Exec execute(const Command& c);
    void bump() { ++revision_; }

    project::Project p_;
    project::Uid nextUid_ = 1;
    uint64_t revision_ = 0;
    std::deque<Step> undo_, redo_;
    bool grouping_ = false;
    std::string groupLabel_;
    std::vector<Exec> group_;
};

// Convenience constructors for the commands the UI issues most.
namespace cmd {
Command setParam(project::Uid deviceUid, const std::string& key, double value);
Command setTrack(project::Uid trackUid, const std::string& field, nlohmann::json value);
Command setMeta(const std::string& field, nlohmann::json value);
}  // namespace cmd

}  // namespace ddaw::document
