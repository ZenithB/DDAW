#pragma once
// A document wired to a live engine: every edit is routed by what it needs. A parameter edit becomes a
// SetParam command on the live graph (no rebuild, no click); a structural edit submits a fresh snapshot
// to the builder thread. UI thread only (it owns the single command lane).
#include "document/Document.h"
#include "engine/GraphService.h"

namespace ddaw::document {

class Session {
public:
    Session(Document& doc, engine::GraphService& service, engine::Engine& engine) : doc_(doc), svc_(service), engine_(engine) {}

    // Apply an edit and route its effect to the engine. Returns the change report. Throws
    // std::invalid_argument for a bad command (nothing changes).
    ChangeInfo apply(const Command& c);
    ChangeInfo undo();
    ChangeInfo redo();
    // A group of edits is one undo step; its merged effect reaches the engine at endGroup.
    void beginGroup(const std::string& label) { doc_.beginGroup(label); }
    ChangeInfo endGroup() { return route(doc_.endGroup()); }
    // While a group is open, edits apply to the document at once but reach the engine at endGroup.
    ChangeInfo applyDeferred(const Command& c) { return doc_.apply(c); }

    // Rebuild from the current document regardless of the last change (e.g. after loading a project).
    void rebuild() { svc_.submit(doc_.snapshot()); }

    // Transport and clip control: engine commands, not document edits.
    void play(bool arrangement = false, double fromTicks = 0.0);
    void stop();
    void launchScene(const std::string& sceneId);
    void stopClips();
    // One clip slot / one track's playing clip (by document track index and scene id).
    void launchClip(size_t trackIndex, const std::string& sceneId);
    void stopTrack(size_t trackIndex);

    Document& document() { return doc_; }

private:
    ChangeInfo route(const ChangeInfo& info);
    void pushTempo(double bpm);

    Document& doc_;
    engine::GraphService& svc_;
    engine::Engine& engine_;
};

}  // namespace ddaw::document
