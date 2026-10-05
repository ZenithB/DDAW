#include "engine/Scheduler.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include "core/Constants.h"

namespace ddaw::engine {

double swingOffsetTicks(double absTick, double swing, double swingTicks) {
    if (swing <= 0.0 || swingTicks <= 0.0) return 0.0;
    const double t = std::round(absTick);
    const double period = swingTicks * 2.0;
    if (std::abs(std::fmod(t, double(kPpq))) < 1e-9 || std::abs(std::fmod(t, period)) < 1e-9) return 0.0;
    const double progress = std::fmod(t, period) / period;
    return std::sin(progress * std::numbers::pi) * swing * (period / 3.0);
}

double swingSubdivTicks(const std::string& subdiv) { return subdiv == "8n" ? kPpq / 2.0 : kPpq / 4.0; }

void TrackSched::launch(size_t scene, double anchor) noexcept {
    const bool hasNotes = scene < session.size() && session[scene].has_value();
    const bool hasAudio = scene < audioSession.size() && audioSession[scene].has_value();
    if (hasNotes || hasAudio) {
        launched_ = scene;
        anchor_ = anchor;
        loopIter_ = 0.0;
        nextIdx_ = 0;
        pending_.reset();
    }
}

void TrackSched::relocate(TransportMode mode, double now) noexcept {
    pending_.reset();
    if (mode == TransportMode::Session) {
        if (launched_ && session[*launched_]) {
            const auto& p = *session[*launched_];
            const double len = std::max(p.loopLen, 1.0);
            const double rel = now - anchor_;
            const double li = std::max(std::floor(rel / len), 0.0);
            const double pos = rel - li * len;
            loopIter_ = li;
            nextIdx_ = static_cast<size_t>(std::lower_bound(p.events.begin(), p.events.end(), pos - 1e-9,
                                                            [](const NoteEv& e, double v) { return e.tick < v; }) - p.events.begin());
        }
    } else {
        arrIdx_ = static_cast<size_t>(std::lower_bound(arr.begin(), arr.end(), now - 1e-9,
                                                       [](const NoteEv& e, double v) { return e.tick < v; }) - arr.begin());
    }
}

std::optional<double> TrackSched::peek(TransportMode mode, double now, const SchedParams& p, XorShift& rng) noexcept {
    if (pending_) return pending_->fireTick;
    // Bound the work per call: a pattern whose notes all fail their probability roll must not spin
    // the audio thread.
    for (int guard = 0; guard < 4096; ++guard) {
        NoteEv ev;
        double baseTick;
        if (mode == TransportMode::Session) {
            if (!launched_) return std::nullopt;
            const auto& pat = session[*launched_];
            if (!pat || pat->events.empty()) return std::nullopt;
            const double len = std::max(pat->loopLen, 1.0);
            if (nextIdx_ >= pat->events.size()) { nextIdx_ = 0; loopIter_ += 1.0; }
            ev = pat->events[nextIdx_++];
            baseTick = anchor_ + loopIter_ * len + ev.tick;
        } else {
            if (arrIdx_ >= arr.size()) return std::nullopt;
            ev = arr[arrIdx_++];
            baseTick = ev.tick;
        }
        if (ev.pr < 1.0f && rng.nextF64() > double(ev.pr)) continue;
        double fire = baseTick + swingOffsetTicks(baseTick, p.swing, p.swingTicks);
        float vel = ev.vel;
        if (p.humanize > 0.0) {
            // +-10% of a beat of timing, +-10% of velocity, scaled by the knob
            fire += (rng.nextF64() * 2.0 - 1.0) * 0.1 * kPpq * p.humanize;
            vel = std::clamp(vel + float((rng.nextF64() * 2.0 - 1.0) * 0.1 * p.humanize), 0.02f, 1.0f);
        }
        if (fire < now) fire = now;  // a negative jitter never schedules into the past
        pending_ = Pending{fire, ev.pitch, ev.durTicks, vel, true, ev.expr};
        return fire;
    }
    // Pathological pattern: park a silent marker one loop ahead.
    const double hold = now + 4.0 * 384.0;
    pending_ = Pending{hold, 0, 0.0, 0.0f, false};
    return hold;
}

std::optional<Pending> TrackSched::takeDue(double dueTick) noexcept {
    if (pending_ && pending_->fireTick <= dueTick) {
        auto p = pending_;
        pending_.reset();
        return p;
    }
    return std::nullopt;
}

}  // namespace ddaw::engine
