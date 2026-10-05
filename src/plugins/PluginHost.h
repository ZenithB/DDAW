#pragma once
// Plugin hosting (L1). A hosted VST3 / AudioUnit plugin is one live instance, owned by the PluginHost on the message thread
// and shared with the graph: every graph build that contains the plugin's device wraps the same instance, so rebuilding a
// graph (any edit) never reloads a plugin or loses its state. Only one wrapper drives the instance at a time; the newest
// graph to process claims it, and the one it replaces goes quiet (see PluginDevice.cpp).
//
// What the project stores for a plugin device: DeviceSpec::plugin (an id the host can find the plugin by), ::pluginName and
// ::pluginState (the plugin's own state blob, base64). A plugin's parameters are exposed to automation, LFOs, macros and
// morph maps as normalised 0..1 controls whose keys come from the plugin's parameter ids; the base value of a modulated
// parameter is the plugin's value when it was instantiated.
//
// Real-time note: the plugin's processBlock runs on the audio thread and the engine cannot enforce its behaviour there. The
// wrapper itself allocates nothing; a plugin that allocates or blocks is that plugin's fault (and why a crash-prone plugin
// is scanned out of process, ARCH 23).
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "core/Device.h"

namespace ddaw::plugins {

struct PluginInfo {
    std::string id;          // what DeviceSpec::plugin holds
    std::string name, vendor, format, category;
    bool instrument = false;
    int numInputs = 0, numOutputs = 0;
};

// One live plugin instance.
class Hosted {
public:
    std::unique_ptr<juce::AudioPluginInstance> proc;
    PluginInfo info;
    uint64_t uid = 0;
    // Parameters as the engine sees them: normalised 0..1, default = the value at instantiation. `keys` owns the strings
    // that `specs[i].key` points to; `names` are the plugin's own labels, for the UI.
    std::vector<ParamSpec> specs;
    std::vector<std::string> keys, names;
    std::vector<juce::AudioProcessorParameter*> params;
    int channels = 2;                       // channels of the main buses as configured (1 or 2)
    int latency = 0;                        // samples, as reported when prepared

    // --- audio-thread side (see PluginDevice.cpp) ---
    std::atomic<const void*> owner{nullptr};
    std::atomic<bool> resetRequested{false};
    struct Note { uint32_t id = 0; uint8_t pitch = 0; bool used = false; };
    std::array<Note, 64> notes;             // sounding notes: a graph swap re-sends them, the plugin must not retrigger

    // Prepare the plugin for this rate and block size, once: a second graph for the same instance must not call
    // prepareToPlay while the first is processing. Builder thread.
    void ensurePrepared(double sampleRate, int maxBlock);
    // The plugin's state as base64 (empty when it has none).
    std::string captureState() const;
    // Message thread: set a parameter by index from the UI (notifies the plugin's own editor).
    void setNormalised(size_t index, float v);
    float getNormalised(size_t index) const;

private:
    std::mutex prepMutex_;
    double preparedSr_ = 0.0;
    int preparedBlock_ = 0;
};

class PluginHost {
public:
    static PluginHost& instance();
    // Destroy the host and every plugin it holds. Message thread, before the JUCE message manager goes (the app calls it on
    // quit). Plugins must not be destroyed by static destruction, which runs on no particular thread.
    static void shutdown();
    PluginHost();
    ~PluginHost();

    // Find every plugin in the default locations of the formats this build hosts. Message thread; may take seconds (loads each
    // plugin to read its description). `progress` is called with the plugin being examined.
    // `deadMansPedal`: a file the scanner writes the plugin it is about to load into. If a plugin crashes the app during the
    // scan, the next scan finds its name there and skips it (and lists it in `failed`), so one bad plugin costs one restart.
    std::vector<PluginInfo> scan(const std::function<void(const juce::String&)>& progress = {}, const juce::File& deadMansPedal = {}, std::vector<std::string>* failed = nullptr);
    std::vector<PluginInfo> known() const;
    // The scan result is kept between runs (a scan loads every plugin and takes seconds).
    void loadCache(const juce::File& file);
    void saveCache(const juce::File& file) const;

    // Create the instance for device `uid` (or return the one already live), restore `stateBase64` into it. Message thread.
    // On failure returns null and fills `error`.
    std::shared_ptr<Hosted> instantiate(uint64_t uid, const std::string& pluginId, const std::string& stateBase64, double sampleRate, int maxBlock, std::string& error);
    // The live instance of a device, or null. Any thread.
    std::shared_ptr<Hosted> find(uint64_t uid) const;
    void forget(uint64_t uid);
    void forgetAll();
    void retain(const std::set<uint64_t>& keep);   // forget every instance whose device is not in `keep`

    // "<format>#<file or identifier>#<name>" and back.
    static std::string makeId(const juce::PluginDescription&);
    static bool splitId(const std::string& id, std::string& format, std::string& file, std::string& name);

    juce::AudioPluginFormatManager& formats() { return formats_; }

private:
    static PluginInfo infoOf(const juce::PluginDescription&);
    mutable std::mutex mutex_;
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList known_;
    std::map<uint64_t, std::shared_ptr<Hosted>> live_;
};

// Register the "plugin" instrument and effect with the device registry (the factories need the host, so they live here, not in
// ddaw_core). Call once at startup, before any graph is built.
void registerDevices();

}  // namespace ddaw::plugins
