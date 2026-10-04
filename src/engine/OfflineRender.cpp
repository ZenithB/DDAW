#include "engine/OfflineRender.h"

#include <algorithm>
#include <cmath>

#include "engine/Engine.h"
#include "engine/GraphBuilder.h"

namespace ddaw::engine {

// One graph, one engine: this is the same code path the audio callback runs (PLAN 1, render-path
// parity). The only offline-specific steps are driving process() in a loop and trimming the
// reported latency so audio lands at timeline position zero (ARCH 10).
RenderResult renderFixture(const project::Fixture& fx, double sr, const RenderOptions& opt, const project::SampleBank* bank) {
    constexpr uint32_t kEpoch = 1;
    BuildResult built = buildGraph(fx, sr, kEpoch, bank);

    Engine eng;
    eng.prepare(sr, opt.master);
    eng.setOffline(true);
    eng.setLoopOverrideOff(true);  // offline: play through, never wrap
    eng.setInitialGraph(std::move(built.graph));

    const double bpm = fx.project.meta.bpm > 0 ? fx.project.meta.bpm : 120.0;
    Cmd tempo;
    tempo.type = CmdType::SetTempo;
    tempo.setTempo = {bpm};
    eng.commands().push(tempo);
    if (built.mode == TransportMode::Session) {
        // Launch the scene on every track while stopped (anchor 0), then play: the batch lands together.
        const int tracks = static_cast<int>(fx.project.tracks.size());
        for (int t = 0; t < tracks; ++t) {
            Cmd launch;
            launch.type = CmdType::ClipLaunch;
            launch.epoch = kEpoch;
            launch.clipLaunch = {static_cast<uint16_t>(t), static_cast<uint16_t>(built.sceneIndex)};
            eng.commands().push(launch);
        }
    }
    Cmd play;
    play.type = CmdType::TransportPlay;
    play.transportPlay = {static_cast<uint8_t>(built.mode == TransportMode::Arrangement ? 1 : 0), built.fromTicks};
    eng.commands().push(play);

    const double fpt = sr * 60.0 / bpm / kPpq;
    const int64_t body = static_cast<int64_t>(std::ceil(built.lengthTicks * fpt + kTailSeconds * sr));
    const int64_t lat = opt.trimLatency ? eng.latencySamples() : 0;

    std::vector<float> l(size_t(body + lat), 0.0f), r(size_t(body + lat), 0.0f);
    RenderResult out;
    int64_t done = body + lat;
    for (int64_t pos = 0; pos < body + lat; pos += kMaxBlock) {
        const int n = static_cast<int>(std::min<int64_t>(kMaxBlock, body + lat - pos));
        eng.process(l.data() + pos, r.data() + pos, n);
        if (opt.progress && (pos / kMaxBlock) % 64 == 0 && !opt.progress(double(pos) / double(body + lat))) {
            out.cancelled = true;
            done = pos + n;
            break;
        }
    }
    l.resize(size_t(done));
    r.resize(size_t(done));

    out.sampleRate = sr;
    const int64_t from = std::min<int64_t>(lat, done);
    out.l.assign(l.begin() + from, l.end());
    out.r.assign(r.begin() + from, r.end());
    out.unsupported = std::move(built.unsupported);
    return out;
}

}  // namespace ddaw::engine
