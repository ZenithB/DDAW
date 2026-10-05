#pragma once
// A game controller, read by polling (B5). macOS: Apple's GameController framework (PLAN 8: no third-party library).
// poll() is cheap and may be called from the UI timer; with no controller connected it returns `connected == false`.
// Sticks are -1..1 (up and right positive), triggers 0..1, buttons pressed/released.
#include <memory>
#include <string>
#include <vector>

namespace ddaw::app {

struct GamepadState {
    bool connected = false;
    std::string name;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    bool a = false, b = false, x = false, y = false, lb = false, rb = false, up = false, down = false, left = false, right = false;
};

class GamepadInput {
public:
    GamepadInput();
    ~GamepadInput();
    // Every connected extended gamepad, in the order the system lists them (at most kMaxPads).
    std::vector<GamepadState> poll();
    // A short vibration pulse on pad `index` (0 = the first). False when there is no such pad or it has no haptics.
    bool rumble(size_t index, float intensity = 0.6f, float seconds = 0.15f);
    static constexpr size_t kMaxPads = 4;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ddaw::app
