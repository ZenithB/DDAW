#include "document/Session.h"

namespace ddaw::document {

ChangeInfo Session::route(const ChangeInfo& info) {
    if (info.structural) {
        svc_.submit(doc_.snapshot());
        return info;
    }
    bool pushedAll = true;
    for (const auto& pe : info.params) pushedAll &= svc_.pushParam(pe.key, static_cast<float>(pe.value));
    if (info.tempo) pushTempo(*info.tempo);
    // A live push is stamped with the epoch of the graph that is live NOW. If a rebuild is in flight the
    // edit would be dropped as stale when that graph swaps in, and the new graph (built from an older
    // snapshot) would not contain it. Submitting a fresh snapshot makes the newest graph carry it.
    // Likewise when a key could not be resolved (e.g. the device was added in a rebuild that is pending).
    if ((!info.params.empty() && (svc_.busy() || !pushedAll))) svc_.submit(doc_.snapshot());
    return info;
}

void Session::pushTempo(double bpm) {
    Cmd c;
    c.type = CmdType::SetTempo;
    c.setTempo = {bpm};
    engine_.commands().push(c);
}

ChangeInfo Session::apply(const Command& c) { return route(doc_.apply(c)); }
ChangeInfo Session::undo() { return route(doc_.undo()); }
ChangeInfo Session::redo() { return route(doc_.redo()); }

void Session::play(bool arrangement, double fromTicks) {
    Cmd c;
    c.type = CmdType::TransportPlay;
    c.transportPlay = {static_cast<uint8_t>(arrangement ? 1 : 0), fromTicks};
    engine_.commands().push(c);
}

void Session::stop() {
    Cmd c;
    c.type = CmdType::TransportStop;
    engine_.commands().push(c);
}

void Session::launchScene(const std::string& sceneId) {
    const auto r = svc_.resolver();
    uint16_t scene = 0;
    if (!r || !r->scene(sceneId, scene)) return;
    const uint32_t epoch = svc_.publishedEpoch();
    const int tracks = static_cast<int>(doc_.project().tracks.size());
    for (int t = 0; t < tracks; ++t) {
        Cmd c;
        c.type = CmdType::ClipLaunch;
        c.epoch = epoch;
        c.clipLaunch = {static_cast<uint16_t>(t), scene};
        engine_.commands().push(c);
    }
}

void Session::launchClip(size_t trackIndex, const std::string& sceneId) {
    const auto r = svc_.resolver();
    uint16_t scene = 0;
    if (!r || !r->scene(sceneId, scene) || trackIndex >= doc_.project().tracks.size()) return;
    Cmd c;
    c.type = CmdType::ClipLaunch;
    c.epoch = svc_.publishedEpoch();
    c.clipLaunch = {static_cast<uint16_t>(trackIndex), scene};
    engine_.commands().push(c);
}

void Session::stopTrack(size_t trackIndex) {
    if (trackIndex >= doc_.project().tracks.size()) return;
    Cmd c;
    c.type = CmdType::ClipStop;
    c.epoch = svc_.publishedEpoch();
    c.clipStop = {static_cast<uint16_t>(trackIndex)};
    engine_.commands().push(c);
}

void Session::stopClips() {
    const uint32_t epoch = svc_.publishedEpoch();
    const int tracks = static_cast<int>(doc_.project().tracks.size());
    for (int t = 0; t < tracks; ++t) {
        Cmd c;
        c.type = CmdType::ClipStop;
        c.epoch = epoch;
        c.clipStop = {static_cast<uint16_t>(t)};
        engine_.commands().push(c);
    }
}

}  // namespace ddaw::document
