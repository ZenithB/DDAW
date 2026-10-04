#include "app/AudioHost.h"

#include <algorithm>
#include <chrono>

namespace ddaw::app {

namespace {
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

AudioHost::AudioHost(GraphFactory factory) : factory_(std::move(factory)) {}

AudioHost::~AudioHost() { stop(); }

juce::String AudioHost::start(int bufferSize, double sampleRate) {
    auto err = dm_.initialise(0, 2, nullptr, true);
    if (err.isNotEmpty()) return err;
    if (bufferSize > 0 || sampleRate > 0.0) {
        auto setup = dm_.getAudioDeviceSetup();
        if (bufferSize > 0) setup.bufferSize = bufferSize;
        if (sampleRate > 0.0) setup.sampleRate = sampleRate;
        err = dm_.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) return err;
    }
    dm_.addAudioCallback(this);
    return dm_.getCurrentAudioDevice() ? juce::String() : juce::String("no audio device opened");
}

juce::String AudioHost::setInputEnabled(bool on) {
    auto setup = dm_.getAudioDeviceSetup();
    if (on == inputEnabled()) return {};
    // Explicit channels: "default" would use the count given to initialise(), which was 0.
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    if (on) setup.inputChannels.setRange(0, 2, true);
    return dm_.setAudioDeviceSetup(setup, true);   // reopens the device: audioDeviceAboutToStart follows
}

void AudioHost::stop() {
    dm_.removeAudioCallback(this);
    dm_.closeAudioDevice();
}

void AudioHost::audioDeviceAboutToStart(juce::AudioIODevice* d) {
    device_ = d;
    numIn_.store(d->getActiveInputChannels().countNumberOfSetBits());
    inLat_.store(d->getInputLatencyInSamples());
    outLat_.store(d->getOutputLatencyInSamples());
    const double sr = d->getCurrentSampleRate();
    bufferSize_.store(d->getCurrentBufferSizeSamples());
    sampleRate_.store(sr);
    scratch_.assign(size_t(std::max(d->getCurrentBufferSizeSamples(), 8192)) * 2, 0.0f);
    engine_.prepare(sr);
    if (factory_) engine_.setInitialGraph(factory_(sr));
    callbacks_ = 0; overBudget_ = 0; late_ = 0; sumUs_ = 0; maxUs_ = 0; maxFrac_ = 0; lastStartNs_ = 0;
    if (onDeviceStarted)
        juce::MessageManager::callAsync([this, sr] { if (onDeviceStarted) onDeviceStarted(sr); });
}

void AudioHost::audioDeviceStopped() { device_ = nullptr; }

void AudioHost::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                                 int numSamples, const juce::AudioIODeviceCallbackContext&) {
    juce::ScopedNoDenormals noDenormals;
    const int64_t t0 = nowNs();

    // Render into the first two output channels (a mono device gets the left channel).
    float* l = numOut > 0 ? out[0] : scratch_.data();
    float* r = numOut > 1 ? out[1] : scratch_.data() + scratch_.size() / 2;
    if (numSamples > int(scratch_.size() / 2)) numSamples = int(scratch_.size() / 2);  // never grow here
    // Input: a stereo pair, or one channel as mono (a mono device feeds both sides).
    const float* inL = nullptr;
    const float* inR = nullptr;
    if (numIn > 0 && in) {
        switch (InputMode(inputMode_.load(std::memory_order_relaxed))) {
            case InputMode::Stereo: inL = in[0]; inR = numIn > 1 ? in[1] : in[0]; break;
            case InputMode::Left: inL = inR = in[0]; break;
            case InputMode::Right: inL = inR = numIn > 1 ? in[1] : in[0]; break;
        }
    }
    engine_.processIO(inL, inR, l, r, numSamples);
    for (int c = 2; c < numOut; ++c) std::fill_n(out[c], numSamples, 0.0f);

    const int64_t t1 = nowNs();
    const double us = double(t1 - t0) / 1000.0;
    const double bufUs = 1e6 * double(numSamples) / sampleRate_.load(std::memory_order_relaxed);
    const int64_t prev = lastStartNs_.exchange(t0);
    if (prev != 0 && double(t0 - prev) / 1000.0 > 1.5 * bufUs) late_.fetch_add(1, std::memory_order_relaxed);
    if (us > bufUs) overBudget_.fetch_add(1, std::memory_order_relaxed);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    sumUs_.store(sumUs_.load(std::memory_order_relaxed) + us, std::memory_order_relaxed);
    if (us > maxUs_.load(std::memory_order_relaxed)) maxUs_.store(us, std::memory_order_relaxed);
    const double frac = us / bufUs;
    if (frac > maxFrac_.load(std::memory_order_relaxed)) maxFrac_.store(frac, std::memory_order_relaxed);
}

HostStats AudioHost::stats() const {
    HostStats s;
    s.callbacks = callbacks_.load();
    s.meanProcessUs = s.callbacks ? sumUs_.load() / double(s.callbacks) : 0.0;
    s.maxProcessUs = maxUs_.load();
    s.maxBudgetFraction = maxFrac_.load();
    s.overBudget = overBudget_.load();
    s.lateCallbacks = late_.load();
    s.deviceXRuns = device_ ? device_->getXRunCount() : -1;
    s.bufferSize = bufferSize_.load();
    s.sampleRate = sampleRate_.load();
    return s;
}

}  // namespace ddaw::app
