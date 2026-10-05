#pragma once
// The JUCE implementation of app::PluginProvider: the PluginHost plus the plugins' own editor windows and the scan cache.
#include <map>
#include <memory>

#include "app/model/PluginProvider.h"
#include "plugins/PluginHost.h"

namespace ddaw::plugins {

class EditorWindow;

class JucePluginProvider final : public app::PluginProvider {
public:
    // `cacheFile`: where the scan result is kept (loaded now, written after each scan). Empty: no cache.
    explicit JucePluginProvider(juce::File cacheFile = {}, double sampleRate = 48000.0, int maxBlock = 128);
    ~JucePluginProvider() override;
    void setSampleRate(double sr) { sr_ = sr; }

    std::vector<app::PluginEntry> available() const override;
    void scan(const std::function<void(const std::string&)>& progress) override;
    bool live(uint64_t uid) const override { return PluginHost::instance().find(uid) != nullptr; }
    bool ensure(uint64_t uid, const std::string& pluginId, const std::string& state, std::string& error) override;
    std::span<const ParamSpec> params(uint64_t uid) const override;
    std::string paramName(uint64_t uid, size_t index) const override;
    float value(uint64_t uid, size_t index) const override;
    void setValue(uint64_t uid, size_t index, float v) override;
    std::string state(uint64_t uid) const override;
    void showEditor(uint64_t uid, const std::function<void()>& onClosed) override;
    void forget(uint64_t uid) override;
    void retain(const std::set<uint64_t>& keep) override;
    void closeEditors();
    // Plugins the last scan could not load (they crashed it or refused to open).
    const std::vector<std::string>& failedLastScan() const { return failedLastScan_; }

private:
    void dropEditor(uint64_t uid);
    juce::File cache_;
    std::vector<std::string> failedLastScan_;
    double sr_;
    int block_;
    std::map<uint64_t, std::unique_ptr<EditorWindow>> editors_;
    friend class EditorWindow;
};

}  // namespace ddaw::plugins
