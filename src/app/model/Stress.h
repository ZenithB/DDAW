#pragma once
// A deliberately heavy project for soaks and engine benchmarks: the demo song, plus a sampler and an audio
// track with looping clips, a voice-follower, a monitored microphone track with a reverb, performance routes
// and (when the neural instrument is registered) a DDSP violin.
#include "app/model/AppModel.h"

namespace ddaw::app {

void buildStressProject(AppModel& m, double sampleRate);

}  // namespace ddaw::app
