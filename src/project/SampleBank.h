#pragma once
// The sample store: decoded PCM keyed by sample id, owned by the control side and read at graph-build
// time. Buffers are injected into instruments (`setSample`) and baked into audio-clip voices; a graph
// rebuild re-injects. A missing id plays silent, never an error.
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Sample.h"

namespace ddaw::project {

class SampleBank {
public:
    void put(const std::string& id, SamplePtr buf) { map_[id] = std::move(buf); }
    SamplePtr get(const std::string& id) const {
        auto it = map_.find(id);
        return it == map_.end() ? nullptr : it->second;
    }
    bool contains(const std::string& id) const { return map_.count(id) != 0; }
    size_t size() const { return map_.size(); }
    std::vector<std::string> ids() const {
        std::vector<std::string> v;
        for (auto& [k, b] : map_) v.push_back(k);
        std::sort(v.begin(), v.end());
        return v;
    }
    void clear() { map_.clear(); }

private:
    std::unordered_map<std::string, SamplePtr> map_;
};

// Decode a WAV file (PCM 16/24/32-bit or 32-bit float, mono or stereo; extra channels are dropped).
// Throws std::runtime_error on an unreadable or unsupported file. Control path only.
SamplePtr loadWavSample(const std::string& path);
// Write a sample as a 32-bit float WAV (lossless). Throws on I/O failure.
void writeWavFloat32(const std::string& path, const SampleBuf& buf);
SamplePtr decodeWavSample(const std::vector<unsigned char>& bytes);

}  // namespace ddaw::project
