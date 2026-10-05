// Apple GameController backend of GamepadInput. Controllers appear through the application's run loop, so a process
// without one (the headless test and snapshot tools) simply sees none.
#include "app/platform/Gamepad.h"

#include <algorithm>

#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

namespace ddaw::app {

// Haptics engines are kept alive per pad (creating one is slow, and a pulse must outlive the call that starts it).
struct GamepadInput::Impl {
    NSMutableDictionary* engines = [[NSMutableDictionary alloc] init];
    ~Impl() { [engines release]; }
};

GamepadInput::GamepadInput() : impl_(std::make_unique<Impl>()) {
    if (@available(macOS 11.3, *)) GCController.shouldMonitorBackgroundEvents = YES;   // keep reading while another app is in front
}
GamepadInput::~GamepadInput() = default;

std::vector<GamepadState> GamepadInput::poll() {
    std::vector<GamepadState> pads;
    @autoreleasepool {
        for (GCController* c in [GCController controllers]) {
            if (pads.size() >= kMaxPads) break;
            GCExtendedGamepad* g = c.extendedGamepad;
            if (!g) continue;
            GamepadState s;
            s.connected = true;
            s.name = c.vendorName ? std::string([c.vendorName UTF8String]) : std::string("Game controller");
            s.lx = g.leftThumbstick.xAxis.value;  s.ly = g.leftThumbstick.yAxis.value;
            s.rx = g.rightThumbstick.xAxis.value; s.ry = g.rightThumbstick.yAxis.value;
            s.lt = g.leftTrigger.value;           s.rt = g.rightTrigger.value;
            s.a = g.buttonA.isPressed; s.b = g.buttonB.isPressed; s.x = g.buttonX.isPressed; s.y = g.buttonY.isPressed;
            s.lb = g.leftShoulder.isPressed;      s.rb = g.rightShoulder.isPressed;
            s.up = g.dpad.up.isPressed; s.down = g.dpad.down.isPressed; s.left = g.dpad.left.isPressed; s.right = g.dpad.right.isPressed;
            pads.push_back(std::move(s));
        }
    }
    return pads;
}

bool GamepadInput::rumble(size_t index, float intensity, float seconds) {
    @autoreleasepool {
        size_t n = 0;
        for (GCController* c in [GCController controllers]) {
            if (!c.extendedGamepad) continue;
            if (n++ != index) continue;
            if (@available(macOS 11.0, *)) {
                GCDeviceHaptics* haptics = c.haptics;
                if (!haptics) return false;
                NSNumber* key = [NSNumber numberWithUnsignedLong:index];
                CHHapticEngine* engine = [impl_->engines objectForKey:key];
                NSError* err = nil;
                if (!engine) {
                    engine = [haptics createEngineWithLocality:GCHapticsLocalityDefault];
                    if (!engine) return false;
                    [engine startAndReturnError:&err];
                    if (err) return false;
                    [impl_->engines setObject:engine forKey:key];   // retained by the dictionary
                }
                CHHapticEventParameter* amount = [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:std::clamp(intensity, 0.0f, 1.0f)] autorelease];
                CHHapticEvent* event = [[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[amount] relativeTime:0 duration:std::clamp(seconds, 0.02f, 2.0f)] autorelease];
                CHHapticPattern* pattern = [[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&err] autorelease];
                if (!pattern || err) return false;
                id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&err];
                if (!player || err) return false;
                return [player startAtTime:0 error:&err] && !err;
            }
            return false;
        }
    }
    return false;
}

}  // namespace ddaw::app
