// The "plugin" devices: an instrument and an effect that wrap a shared Hosted instance (PluginHost.h).
//
// One instance, several graphs. A graph swap builds the new graph while the old one still runs, so for a moment two wrappers
// refer to the same plugin. The wrapper that processes first in a callback and has never processed before claims the
// instance (the new graph renders first and the old graph only for the fade); a wrapper that is no longer the owner outputs
// nothing (an instrument) or leaves its input alone (an effect). While the builder is warming a graph up
// (ProcessContext::warmup) wrappers do not touch the plugin at all, and reset() only asks the owner to reset itself at its
// next block, on the audio thread, so the plugin is never driven from two threads.
//
// MIDI: notes go to channel 1 at the start of the block they arrive in. A note that a graph swap re-sends is recognised by its
// id (the sounding notes live in the Hosted) and not retriggered. Per-note expression is sent as channel-wide pitch bend
// (+-2 semitones), channel pressure and CC 74: right for one voice, not yet per-note (MPE) for several.
#include <algorithm>
#include <cmath>

#include "core/Constants.h"
#include "devices/Registry.h"
#include "plugins/PluginHost.h"

namespace ddaw::plugins {
namespace {

class Wrapper {
public:
    void bindImpl(uint64_t uid, std::string_view) { h_ = PluginHost::instance().find(uid); }

    void prepareImpl(double sr, int maxBlock) {
        if (!h_) return;
        h_->ensurePrepared(sr, maxBlock);
        const int nch = std::max({2, h_->proc->getTotalNumInputChannels(), h_->proc->getTotalNumOutputChannels()});
        scratch_.setSize(nch, std::max(maxBlock, kMaxBlock), false, true, false);
        midi_.ensureSize(16384);
    }

    std::span<const ParamSpec> paramsImpl() const { return h_ ? std::span<const ParamSpec>(h_->specs) : std::span<const ParamSpec>(); }
    void setParamImpl(uint16_t i, float v) { if (h_ && i < h_->params.size()) h_->params[i]->setValue(std::clamp(v, 0.0f, 1.0f)); }
    int latencyImpl() const { return h_ ? h_->latency : 0; }
    void resetImpl() { if (h_ && h_->owner.load(std::memory_order_relaxed) == this) h_->resetRequested.store(true, std::memory_order_release); midi_.clear(); }

    // true when this wrapper may drive the plugin in this block
    bool acquire(const ProcessContext& ctx) {
        if (!h_ || ctx.warmup) return false;
        if (!claimed_) { h_->owner.store(this, std::memory_order_release); claimed_ = true; }
        else if (h_->owner.load(std::memory_order_acquire) != this) return false;
        if (h_->resetRequested.exchange(false, std::memory_order_acq_rel)) h_->proc->reset();
        return true;
    }

    // Run the plugin on scratch_ (n frames, the caller has filled the input channels), then clear the MIDI that went in.
    void run(int n) {
        juce::AudioBuffer<float> view(scratch_.getArrayOfWritePointers(), scratch_.getNumChannels(), n);
        h_->proc->processBlock(view, midi_);
        midi_.clear();
    }

    void noteOnImpl(uint8_t pitch, float velocity, uint32_t id) {
        if (!h_) return;
        for (const auto& n : h_->notes) if (n.used && n.id == id) return;                 // a graph swap re-sending a sounding note
        for (auto& n : h_->notes) if (!n.used) { n = {id, pitch, true}; break; }
        midi_.addEvent(juce::MidiMessage::noteOn(1, int(pitch), juce::uint8(std::clamp(int(std::lround(velocity * 127.0f)), 1, 127))), 0);
    }
    void noteOffImpl(uint32_t id) {
        if (!h_) return;
        for (auto& n : h_->notes)
            if (n.used && n.id == id) { midi_.addEvent(juce::MidiMessage::noteOff(1, int(n.pitch)), 0); n.used = false; return; }
    }
    void expressionImpl(uint32_t id, int dim, float v) {
        if (!h_) return;
        bool mine = false;
        for (const auto& n : h_->notes) if (n.used && n.id == id) mine = true;
        if (!mine) return;
        if (dim == 0) midi_.addEvent(juce::MidiMessage::controllerEvent(1, 74, std::clamp(int(std::lround(v * 127.0f)), 0, 127)), 0);
        else if (dim == 1) midi_.addEvent(juce::MidiMessage::channelPressureChange(1, std::clamp(int(std::lround(v * 127.0f)), 0, 127)), 0);
        else midi_.addEvent(juce::MidiMessage::pitchWheel(1, std::clamp(8192 + int(std::lround(v * (1.0f / 2.0f) * 8191.0f)), 0, 16383)), 0);
    }

    std::shared_ptr<Hosted> h_;
    juce::AudioBuffer<float> scratch_;
    juce::MidiBuffer midi_;
    bool claimed_ = false;
};

class PluginInst final : public InstrumentDevice, private Wrapper {
public:
    std::span<const ParamSpec> params() const override { return paramsImpl(); }
    void bind(uint64_t uid, std::string_view id) override { bindImpl(uid, id); }
    void prepare(double sr, int maxBlock) override { prepareImpl(sr, maxBlock); }
    void setParam(uint16_t i, float v) override { setParamImpl(i, v); }
    void noteOn(uint8_t p, float v, uint32_t id) override { noteOnImpl(p, v, id); }
    void noteOff(uint32_t id) override { noteOffImpl(id); }
    void noteExpression(uint32_t id, int dim, float v) override { expressionImpl(id, dim, v); }
    int latencySamples() const override { return latencyImpl(); }
    void reset() override { resetImpl(); }

    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override {
        if (!acquire(ctx)) return;
        scratch_.clear();
        run(n);
        const int ch = scratch_.getNumChannels();
        const float* a = scratch_.getReadPointer(0);
        const float* b = ch > 1 ? scratch_.getReadPointer(1) : a;
        for (int i = 0; i < n; ++i) { l[i] += a[i]; r[i] += b[i]; }
    }
};

class PluginFx final : public EffectDevice, private Wrapper {
public:
    std::span<const ParamSpec> params() const override { return paramsImpl(); }
    void bind(uint64_t uid, std::string_view id) override { bindImpl(uid, id); }
    void prepare(double sr, int maxBlock) override { prepareImpl(sr, maxBlock); }
    void setParam(uint16_t i, float v) override { setParamImpl(i, v); }
    int latencySamples() const override { return latencyImpl(); }
    void reset() override { resetImpl(); }

    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override {
        if (!acquire(ctx)) return;                         // not the owner: the input passes through untouched
        const int ch = scratch_.getNumChannels();
        const int inCh = h_->proc->getTotalNumInputChannels();
        scratch_.clear();
        if (h_->channels == 1) {                           // mono plugin: mix down, and put its output on both sides
            float* m = scratch_.getWritePointer(0);
            for (int i = 0; i < n; ++i) m[i] = 0.5f * (l[i] + r[i]);
        } else {
            std::copy(l, l + n, scratch_.getWritePointer(0));
            std::copy(r, r + n, scratch_.getWritePointer(1));
        }
        (void)inCh;
        run(n);
        const float* a = scratch_.getReadPointer(0);
        const float* b = ch > 1 && h_->channels > 1 ? scratch_.getReadPointer(1) : a;
        std::copy(a, a + n, l);
        std::copy(b, b + n, r);
    }
};

}  // namespace

void registerDevices() {
    registerInstrument("plugin", [] { return std::unique_ptr<InstrumentDevice>(new PluginInst()); });
    registerEffect("plugin", [] { return std::unique_ptr<EffectDevice>(new PluginFx()); });
}

}  // namespace ddaw::plugins
