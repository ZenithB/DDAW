// ddaw_audiotest [seconds=4] [bufferFrames=64] [sampleRate=0] [toneHz=440] [toneDb=-30]
// A1 exit check on a real device: phase 1 plays a quiet test tone, phase 2 an empty project, both
// through the same Engine the app uses. Reports the device configuration, callback timing and any
// dropouts. Exit status is non-zero on an over-budget callback, a late callback or a driver xrun.
#include <cstdio>
#include <cstdlib>

#include <juce_events/juce_events.h>

#include "app/AudioHost.h"

using namespace ddaw;

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 4.0;
    const int buffer = argc > 2 ? std::atoi(argv[2]) : 64;
    const double sr = argc > 3 ? std::atof(argv[3]) : 0.0;
    const float hz = argc > 4 ? float(std::atof(argv[4])) : 440.0f;
    const float db = argc > 5 ? float(std::atof(argv[5])) : -30.0f;

    juce::ScopedJuceInitialiser_GUI juceInit;
    app::AudioHost host([](double rate) { return std::make_unique<engine::Graph>(1, rate); });  // empty project

    if (auto err = host.start(buffer, sr); err.isNotEmpty()) { std::fprintf(stderr, "cannot open audio: %s\n", err.toRawUTF8()); return 2; }
    auto* dev = host.deviceManager().getCurrentAudioDevice();
    std::printf("device: %s | %s | %.0f Hz | requested %d frames, got %d | output latency %d frames\n",
                dev->getTypeName().toRawUTF8(), dev->getName().toRawUTF8(), dev->getCurrentSampleRate(), buffer,
                dev->getCurrentBufferSizeSamples(), dev->getOutputLatencyInSamples());

    Cmd tone; tone.type = CmdType::TestTone; tone.testTone = {hz, db, 1};
    host.engine().commands().push(tone);
    std::printf("phase 1: %.0f Hz tone at %.0f dBFS for %.1f s (empty project)\n", hz, db, seconds);
    juce::Thread::sleep(int(seconds * 1000));
    auto s1 = host.stats();

    tone.testTone.on = 0;
    host.engine().commands().push(tone);
    std::printf("phase 2: empty project, silence, for %.1f s\n", seconds / 2);
    juce::Thread::sleep(int(seconds * 500));
    auto s = host.stats();
    host.stop();

    std::printf("callbacks %llu (phase 1: %llu) | process mean %.1f us, max %.1f us | budget %.0f us/callback\n",
                (unsigned long long)s.callbacks, (unsigned long long)s1.callbacks, s.meanProcessUs, s.maxProcessUs,
                1e6 * s.bufferSize / s.sampleRate);
    std::printf("worst callback used %.1f%% of its buffer | over-budget %llu | late %llu | driver xruns %d\n",
                100.0 * s.maxBudgetFraction, (unsigned long long)s.overBudget, (unsigned long long)s.lateCallbacks, s.deviceXRuns);
    const bool ok = s.callbacks > 0 && s.overBudget == 0 && s.lateCallbacks == 0 && s.deviceXRuns <= 0;
    std::printf("%s\n", ok ? "PASS: no dropouts" : "FAIL");
    return ok ? 0 : 1;
}
