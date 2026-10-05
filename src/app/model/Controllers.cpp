// Controllers (B5): a gamepad axis / button or a MIDI CC drives a morph map's stick or a macro through the project's
// bindings. Input arrives on the UI thread (the UI polls the devices) already normalised to 0..1. Moving a control
// updates the document without an undo step (Document::applyTransient) and reaches the engine as a live parameter,
// like dragging the knob would; "learn" turns the next control you move into a binding.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app/model/AppModel.h"
#include "app/model/Controllers.h"
#include "project/ProjectJson.h"

namespace ddaw::app {

std::string describeSource(const std::string& s) {
    static const std::pair<const char*, const char*> pad[] = {{"lx", "Left stick X"}, {"ly", "Left stick Y"}, {"rx", "Right stick X"}, {"ry", "Right stick Y"},
        {"lt", "Left trigger"}, {"rt", "Right trigger"}, {"a", "Button A"}, {"b", "Button B"}, {"x", "Button X"}, {"y", "Button Y"},
        {"lb", "Left shoulder"}, {"rb", "Right shoulder"}, {"up", "D-pad up"}, {"down", "D-pad down"}, {"left", "D-pad left"}, {"right", "D-pad right"}};
    if (s.rfind("pad", 0) == 0) {   // "pad:lx" is the first pad, "pad2:lx" the second, ...
        const auto colon = s.find(':');
        if (colon != std::string::npos) {
            const std::string num = s.substr(3, colon - 3), key = s.substr(colon + 1);
            const std::string who = num.empty() ? std::string() : "Pad " + num + " ";
            for (auto& [k, v] : pad) if (key == k) return who + v;
            return who + "gamepad " + key;
        }
    }
    if (s == "midi:bend") return "MIDI pitch bend";
    if (s == "midi:pressure") return "MIDI pressure";
    if (s.rfind("midi:cc", 0) == 0) return "MIDI CC " + s.substr(7);
    return s;
}

namespace {
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t i = s.find(sep, from);
        out.push_back(s.substr(from, i == std::string::npos ? std::string::npos : i - from));
        if (i == std::string::npos) break;
        from = i + 1;
    }
    return out;
}
}  // namespace

bool AppModel::applyTransient(const document::Command& c) {
    try {
        session_->applyTransient(c);
        lastError_.clear();
    } catch (const std::exception& e) {
        lastError_ = e.what();
        return false;
    }
    notify(ModelEvent::Document);
    return true;
}

void AppModel::applyBinding(const project::ControlBinding& b, double v) {
    const auto parts = split(b.target, ':');
    if (parts.size() < 3) return;
    const double u = std::clamp(b.min + (b.invert ? 1.0 - v : v) * (b.max - b.min), 0.0, 1.0);
    const project::Track* track = nullptr;
    for (const auto& t : project().tracks) if (t.id == parts[1]) { track = &t; break; }
    const long idx = std::strtol(parts[2].c_str(), nullptr, 10);
    if (!track || idx < 0) return;
    if (parts[0] == "morph" && parts.size() == 4 && size_t(idx) < track->morph.size()) {
        const auto& m = track->morph[size_t(idx)];
        const double x = parts[3] == "x" ? u : m.x, y = parts[3] == "y" ? u : m.y;
        if (x == m.x && y == m.y) return;   // nothing moved: do not churn the document
        applyTransient({"morph.pos", {{"track", track->uid}, {"index", idx}, {"x", x}, {"y", y}}});
    } else if (parts[0] == "macro" && size_t(idx) < track->macros.size()) {
        if (track->macros[size_t(idx)].value == u) return;
        applyTransient({"macro.value", {{"track", track->uid}, {"index", idx}, {"value", u}}});
    }
}

bool AppModel::controllerInput(const std::string& source, double value) {
    const double v = std::clamp(value, 0.0, 1.0);
    if (!learn_.empty()) {   // learning: the first control that moves by a clear amount becomes the binding
        auto it = learnBase_.find(source);
        if (it == learnBase_.end()) { learnBase_[source] = v; return true; }
        if (std::abs(v - it->second) < 0.2) return true;
        project::ControlBinding b;
        b.source = source;
        b.target = learn_;
        // one source per target: replace the old binding of this target
        for (size_t i = 0; i < project().bindings.size(); ++i)
            if (project().bindings[i].target == learn_) { apply({"binding.remove", {{"index", i}}}); break; }
        apply({"binding.insert", {{"index", project().bindings.size()}, {"binding", project::bindingToJson(b)}}});
        setStatus("Bound " + describeSource(source));
        learn_.clear();
        learnBase_.clear();
        notify(ModelEvent::Document);
        return true;
    }
    bool used = false;
    for (const auto& b : project().bindings) if (b.source == source) { applyBinding(b, v); used = true; }
    return used;
}

void AppModel::startLearn(const std::string& target) {
    learn_ = target;
    learnBase_.clear();
    setStatus("Move a stick, trigger or MIDI control to bind it");
    notify(ModelEvent::Document);
}

void AppModel::cancelLearn() {
    if (learn_.empty()) return;
    learn_.clear();
    learnBase_.clear();
    setStatus("");
    notify(ModelEvent::Document);
}

}  // namespace ddaw::app
