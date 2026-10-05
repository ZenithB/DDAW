#include "plugins/PluginHost.h"

#include <algorithm>
#include <cctype>

namespace ddaw::plugins {

namespace {
constexpr size_t kMaxParams = 2048;

std::string sanitise(const juce::String& s) {
    std::string out;
    for (auto c : s.toStdString()) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-') ? c : '_';
    return out;
}
}  // namespace

// ---- Hosted --------------------------------------------------------------------------------------------------------------

void Hosted::ensurePrepared(double sampleRate, int maxBlock) {
    std::lock_guard<std::mutex> lk(prepMutex_);
    if (preparedSr_ == sampleRate && preparedBlock_ == maxBlock) return;
    proc->setRateAndBufferSizeDetails(sampleRate, maxBlock);   // the bus layout was chosen when it was created
    proc->prepareToPlay(sampleRate, maxBlock);
    preparedSr_ = sampleRate;
    preparedBlock_ = maxBlock;
    latency = proc->getLatencySamples();
}

std::string Hosted::captureState() const {
    juce::MemoryBlock mb;
    proc->getStateInformation(mb);
    return mb.getSize() == 0 ? std::string() : juce::Base64::toBase64(mb.getData(), mb.getSize()).toStdString();
}

void Hosted::setNormalised(size_t i, float v) { if (i < params.size()) params[i]->setValueNotifyingHost(std::clamp(v, 0.0f, 1.0f)); }
float Hosted::getNormalised(size_t i) const { return i < params.size() ? params[i]->getValue() : 0.0f; }

// ---- PluginHost ----------------------------------------------------------------------------------------------------------

namespace {
std::unique_ptr<PluginHost>& hostSlot() { static std::unique_ptr<PluginHost> h; return h; }
}  // namespace

PluginHost& PluginHost::instance() {
    auto& h = hostSlot();
    if (!h) h = std::make_unique<PluginHost>();
    return *h;
}

void PluginHost::shutdown() { hostSlot().reset(); }

PluginHost::PluginHost() {
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    formats_.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
#endif
#if JUCE_PLUGINHOST_VST3
    formats_.addFormat(std::make_unique<juce::VST3PluginFormat>());
#endif
}

PluginHost::~PluginHost() { forgetAll(); }

std::string PluginHost::makeId(const juce::PluginDescription& d) {
    return d.pluginFormatName.toStdString() + "#" + d.fileOrIdentifier.toStdString() + "#" + d.name.toStdString();
}

bool PluginHost::splitId(const std::string& id, std::string& format, std::string& file, std::string& name) {
    const auto a = id.find('#'), b = id.rfind('#');
    if (a == std::string::npos || b == a) return false;
    format = id.substr(0, a);
    file = id.substr(a + 1, b - a - 1);
    name = id.substr(b + 1);
    return !format.empty() && !file.empty();
}

PluginInfo PluginHost::infoOf(const juce::PluginDescription& d) {
    PluginInfo i;
    i.id = makeId(d);
    i.name = d.name.toStdString();
    i.vendor = d.manufacturerName.toStdString();
    i.format = d.pluginFormatName.toStdString();
    i.category = d.category.toStdString();
    i.instrument = d.isInstrument;
    i.numInputs = d.numInputChannels;
    i.numOutputs = d.numOutputChannels;
    return i;
}

std::vector<PluginInfo> PluginHost::scan(const std::function<void(const juce::String&)>& progress, const juce::File& deadMansPedal, std::vector<std::string>* failed) {
    std::lock_guard<std::mutex> lk(mutex_);
    known_.clear();
    for (int f = 0; f < formats_.getNumFormats(); ++f) {
        auto* format = formats_.getFormat(f);
        juce::PluginDirectoryScanner scanner(known_, *format, format->getDefaultLocationsToSearch(), true, deadMansPedal);
        juce::String name;
        while (scanner.scanNextFile(true, name)) if (progress) progress(name);
        if (failed) for (const auto& f : scanner.getFailedFiles()) failed->push_back(f.toStdString());
    }
    std::vector<PluginInfo> out;
    for (const auto& d : known_.getTypes()) out.push_back(infoOf(d));
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name; });
    return out;
}

std::vector<PluginInfo> PluginHost::known() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<PluginInfo> out;
    for (const auto& d : known_.getTypes()) out.push_back(infoOf(d));
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name; });
    return out;
}

void PluginHost::loadCache(const juce::File& file) {
    if (auto xml = juce::XmlDocument::parse(file)) { std::lock_guard<std::mutex> lk(mutex_); known_.recreateFromXml(*xml); }
}

void PluginHost::saveCache(const juce::File& file) const {
    std::unique_ptr<juce::XmlElement> xml;
    { std::lock_guard<std::mutex> lk(mutex_); xml = known_.createXml(); }
    if (xml) { file.getParentDirectory().createDirectory(); xml->writeTo(file); }
}

std::shared_ptr<Hosted> PluginHost::find(uint64_t uid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = live_.find(uid);
    return it == live_.end() ? nullptr : it->second;
}

void PluginHost::forget(uint64_t uid) {
    std::shared_ptr<Hosted> drop;
    { std::lock_guard<std::mutex> lk(mutex_); auto it = live_.find(uid); if (it != live_.end()) { drop = std::move(it->second); live_.erase(it); } }
}

void PluginHost::retain(const std::set<uint64_t>& keep) {
    std::vector<std::shared_ptr<Hosted>> drop;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = live_.begin(); it != live_.end();) {
            if (keep.count(it->first)) { ++it; continue; }
            drop.push_back(std::move(it->second));
            it = live_.erase(it);
        }
    }
}

void PluginHost::forgetAll() {
    std::map<uint64_t, std::shared_ptr<Hosted>> drop;
    { std::lock_guard<std::mutex> lk(mutex_); drop.swap(live_); }
}

std::shared_ptr<Hosted> PluginHost::instantiate(uint64_t uid, const std::string& pluginId, const std::string& stateBase64, double sr, int maxBlock, std::string& error) {
    if (auto live = find(uid)) return live;
    std::string formatName, file, name;
    if (!splitId(pluginId, formatName, file, name)) { error = "not a plugin id: " + pluginId; return nullptr; }
    juce::AudioPluginFormat* format = nullptr;
    for (int f = 0; f < formats_.getNumFormats(); ++f) if (formats_.getFormat(f)->getName().toStdString() == formatName) format = formats_.getFormat(f);
    if (!format) { error = "this build cannot host " + formatName + " plugins"; return nullptr; }
    juce::OwnedArray<juce::PluginDescription> types;
    format->findAllTypesForFile(types, juce::String(file));
    const juce::PluginDescription* desc = nullptr;
    for (auto* t : types) if (t->name.toStdString() == name) desc = t;
    if (!desc && types.size() > 0) desc = types[0];
    if (!desc) { error = "plugin not found: " + name + " (" + file + ")"; return nullptr; }

    juce::String err;
    auto proc = formats_.createPluginInstance(*desc, sr, maxBlock, err);
    if (!proc) { error = "could not load " + name + ": " + err.toStdString(); return nullptr; }

    auto h = std::make_shared<Hosted>();
    h->uid = uid;
    h->info = infoOf(*desc);
    // stereo in and out where the plugin allows it, else mono, else whatever it prefers
    {
        auto layout = proc->getBusesLayout();
        auto stereo = layout;
        for (auto& b : stereo.inputBuses) if (!b.isDisabled()) b = juce::AudioChannelSet::stereo();
        for (auto& b : stereo.outputBuses) if (!b.isDisabled()) b = juce::AudioChannelSet::stereo();
        if (!proc->checkBusesLayoutSupported(stereo) || !proc->setBusesLayout(stereo)) {
            auto mono = layout;
            for (auto& b : mono.inputBuses) if (!b.isDisabled()) b = juce::AudioChannelSet::mono();
            for (auto& b : mono.outputBuses) if (!b.isDisabled()) b = juce::AudioChannelSet::mono();
            if (proc->checkBusesLayoutSupported(mono)) proc->setBusesLayout(mono);
        }
        h->channels = std::max(1, std::min(2, std::max(proc->getMainBusNumOutputChannels(), proc->getMainBusNumInputChannels())));
    }
    if (!stateBase64.empty()) {
        juce::MemoryBlock mb;
        if (juce::MemoryOutputStream out(mb, false); juce::Base64::convertFromBase64(out, juce::String(stateBase64))) {
            out.flush();
            proc->setStateInformation(mb.getData(), int(mb.getSize()));
        }
    }
    h->proc = std::move(proc);
    for (auto* p : h->proc->getParameters()) {
        if (h->params.size() >= kMaxParams) break;
        const size_t i = h->params.size();
        juce::String id = p->getName(64);
        if (auto* hp = dynamic_cast<juce::HostedAudioProcessorParameter*>(p)) id = hp->getParameterID();
        std::string key = "p_" + sanitise(id);
        if (std::find(h->keys.begin(), h->keys.end(), key) != h->keys.end()) key += "_" + std::to_string(i);
        h->keys.push_back(key);
        h->names.push_back(p->getName(64).toStdString());
        h->params.push_back(p);
    }
    h->specs.reserve(h->params.size());
    for (size_t i = 0; i < h->params.size(); ++i)
        h->specs.push_back({uint16_t(i), h->keys[i].c_str(), 0.0f, 1.0f, h->params[i]->getValue(), Curve::Linear, 0.0f, false});
    h->ensurePrepared(sr, maxBlock);
    std::lock_guard<std::mutex> lk(mutex_);
    live_[uid] = h;
    return h;
}

}  // namespace ddaw::plugins
