#pragma once
// Reader for the .ddspw tensor container written by tools/b1/export_weights.py.
// Non-real-time (allocates, throws); call from a loader thread.
#include <map>
#include <string>
#include <vector>

namespace ddaw::ddsp {

struct Tensor {
    std::vector<unsigned> dims;
    std::vector<float>    data;  // C order
    size_t size() const { return data.size(); }
};

using TensorMap = std::map<std::string, Tensor>;

// Throws std::runtime_error on a missing/corrupt file.
TensorMap loadWeights(const std::string& path);

}  // namespace ddaw::ddsp
