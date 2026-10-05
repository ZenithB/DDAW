#include "plugins/PluginProvider.h"

namespace ddaw::plugins {

// The plugin's own editor in a window of its own. Closing the window destroys the editor (the plugin stays loaded).
class EditorWindow final : public juce::DocumentWindow {
public:
    EditorWindow(JucePluginProvider& owner, uint64_t uid, std::shared_ptr<Hosted> h, std::function<void()> onClosed)
        : juce::DocumentWindow(h->info.name, juce::Colours::darkgrey, juce::DocumentWindow::closeButton),
          owner_(owner), uid_(uid), hosted_(std::move(h)), onClosed_(std::move(onClosed)) {
        setUsingNativeTitleBar(true);
        juce::AudioProcessorEditor* ed = hosted_->proc->hasEditor() ? hosted_->proc->createEditorAndMakeActive() : nullptr;
        if (!ed) ed = new juce::GenericAudioProcessorEditor(*hosted_->proc);
        setContentOwned(ed, true);
        setResizable(ed->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        toFront(true);
    }
    ~EditorWindow() override { clearContentComponent(); }
    void closeButtonPressed() override {
        auto cb = std::move(onClosed_);
        const auto uid = uid_;
        auto* owner = &owner_;
        juce::MessageManager::callAsync([owner, uid, cb] { owner->dropEditor(uid); if (cb) cb(); });   // not from inside its own handler
    }

private:
    JucePluginProvider& owner_;
    uint64_t uid_;
    std::shared_ptr<Hosted> hosted_;
    std::function<void()> onClosed_;
};

JucePluginProvider::JucePluginProvider(juce::File cacheFile, double sampleRate, int maxBlock) : cache_(std::move(cacheFile)), sr_(sampleRate), block_(maxBlock) {
    if (cache_ != juce::File()) PluginHost::instance().loadCache(cache_);
}

JucePluginProvider::~JucePluginProvider() { closeEditors(); }

std::vector<app::PluginEntry> JucePluginProvider::available() const {
    std::vector<app::PluginEntry> out;
    for (const auto& p : PluginHost::instance().known()) out.push_back({p.id, p.name, p.vendor, p.format, p.category, p.instrument});
    return out;
}

void JucePluginProvider::scan(const std::function<void(const std::string&)>& progress) {
    if (cache_ != juce::File()) cache_.getParentDirectory().createDirectory();   // the pedal file lives next to the cache
    std::vector<std::string> failed;
    PluginHost::instance().scan([&](const juce::String& s) { if (progress) progress(s.toStdString()); },
                                cache_ == juce::File() ? juce::File() : cache_.getSiblingFile("plugin-scan-pedal.txt"), &failed);
    failedLastScan_ = failed;
    if (cache_ != juce::File()) PluginHost::instance().saveCache(cache_);
}

bool JucePluginProvider::ensure(uint64_t uid, const std::string& pluginId, const std::string& state, std::string& error) {
    if (PluginHost::instance().find(uid)) return false;
    return PluginHost::instance().instantiate(uid, pluginId, state, sr_, block_, error) != nullptr;
}

std::span<const ParamSpec> JucePluginProvider::params(uint64_t uid) const {
    auto h = PluginHost::instance().find(uid);
    return h ? std::span<const ParamSpec>(h->specs) : std::span<const ParamSpec>();   // the Hosted outlives the span while the host holds it
}
std::string JucePluginProvider::paramName(uint64_t uid, size_t i) const { auto h = PluginHost::instance().find(uid); return h && i < h->names.size() ? h->names[i] : std::string(); }
float JucePluginProvider::value(uint64_t uid, size_t i) const { auto h = PluginHost::instance().find(uid); return h ? h->getNormalised(i) : 0.0f; }
void JucePluginProvider::setValue(uint64_t uid, size_t i, float v) { if (auto h = PluginHost::instance().find(uid)) h->setNormalised(i, v); }
std::string JucePluginProvider::state(uint64_t uid) const { auto h = PluginHost::instance().find(uid); return h ? h->captureState() : std::string(); }

void JucePluginProvider::showEditor(uint64_t uid, const std::function<void()>& onClosed) {
    auto h = PluginHost::instance().find(uid);
    if (!h) return;
    if (auto it = editors_.find(uid); it != editors_.end()) { it->second->toFront(true); return; }
    editors_[uid] = std::make_unique<EditorWindow>(*this, uid, h, onClosed);
}

void JucePluginProvider::dropEditor(uint64_t uid) { editors_.erase(uid); }
void JucePluginProvider::closeEditors() { editors_.clear(); }
void JucePluginProvider::forget(uint64_t uid) { dropEditor(uid); PluginHost::instance().forget(uid); }

void JucePluginProvider::retain(const std::set<uint64_t>& keep) {
    std::vector<uint64_t> drop;
    for (auto& [uid, w] : editors_) if (!keep.count(uid)) drop.push_back(uid);
    for (auto uid : drop) dropEditor(uid);
    // instances: the host has no listing, so keep a copy of what to drop from the live map
    PluginHost::instance().retain(keep);
}

}  // namespace ddaw::plugins
