#include "app/ui/ControllerInput.h"

#include <cmath>

namespace ddaw::ui {

namespace {
constexpr float kDeadzone = 0.08f;
double stickUnit(float raw) { return std::abs(raw) < kDeadzone ? 0.5 : 0.5 + 0.5 * double(std::clamp(raw, -1.0f, 1.0f)); }
}  // namespace

ControllerInput::ControllerInput(app::AppModel& m, LiveInput& live) : model_(m), live_(live) { startTimerHz(60); }
ControllerInput::~ControllerInput() { stopTimer(); }

void ControllerInput::feed(const std::string& source, double v) { last_[source] = v; deliver(source, v); }

void ControllerInput::deliver(const std::string& source, double v) {
    const bool learning = !model_.learnTarget().empty();
    model_.controllerInput(source, v);
    if (learning && model_.learnTarget().empty() && source.rfind("pad", 0) == 0) {   // the control just got bound: a short pulse says so
        const auto colon = source.find(':');
        const std::string num = colon == std::string::npos ? std::string() : source.substr(3, colon - 3);
        const size_t pad = num.empty() ? 0 : size_t(std::max(1, std::atoi(num.c_str())) - 1);
        if (!override_ && !overrideAll_) gamepad_.rumble(pad);
        ++pulses_;
    }
}

void ControllerInput::sendIfChanged(const std::string& source, double v) {
    const auto it = last_.find(source);
    if (it != last_.end() && std::abs(it->second - v) < 0.002) return;
    last_[source] = v;
    deliver(source, v);
}

void ControllerInput::pollOnce() {
    if (overrideAll_) pads_ = *overrideAll_;
    else if (override_) pads_ = {*override_};
    else pads_ = gamepad_.poll();
    for (size_t i = 0; i < pads_.size(); ++i) {
        const auto& p = pads_[i];
        const std::string pre = i == 0 ? "pad:" : "pad" + std::to_string(i + 1) + ":";   // "pad:" is the first pad: bindings made with one pad keep working
        sendIfChanged(pre + "lx", stickUnit(p.lx));
        sendIfChanged(pre + "ly", stickUnit(p.ly));
        sendIfChanged(pre + "rx", stickUnit(p.rx));
        sendIfChanged(pre + "ry", stickUnit(p.ry));
        sendIfChanged(pre + "lt", double(p.lt));
        sendIfChanged(pre + "rt", double(p.rt));
        const std::pair<const char*, bool> buttons[] = {{"a", p.a}, {"b", p.b}, {"x", p.x}, {"y", p.y}, {"lb", p.lb}, {"rb", p.rb},
                                                        {"up", p.up}, {"down", p.down}, {"left", p.left}, {"right", p.right}};
        for (const auto& [name, down] : buttons) sendIfChanged(pre + name, down ? 1.0 : 0.0);
    }
    live_.pollControllers([this](const std::string& source, double v) { sendIfChanged(source, v); });
}

}  // namespace ddaw::ui
