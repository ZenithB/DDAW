#include "ddsp/Weights.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ddaw::ddsp {

namespace {
template <class T>
T rd(std::ifstream& f) {
    T v;
    if (!f.read(reinterpret_cast<char*>(&v), sizeof v)) throw std::runtime_error("truncated .ddspw");
    return v;
}
}  // namespace

TensorMap loadWeights(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    char magic[6];
    if (!f.read(magic, 6) || std::memcmp(magic, "DDSPW1", 6)) throw std::runtime_error("not a .ddspw file: " + path);
    TensorMap out;
    const auto n = rd<uint32_t>(f);
    for (uint32_t i = 0; i < n; ++i) {
        std::string name(rd<uint32_t>(f), '\0');
        if (!f.read(name.data(), static_cast<std::streamsize>(name.size()))) throw std::runtime_error("truncated .ddspw");
        Tensor t;
        t.dims.resize(rd<uint32_t>(f));
        size_t count = 1;
        for (auto& d : t.dims) { d = rd<uint32_t>(f); count *= d; }
        t.data.resize(count);
        if (!f.read(reinterpret_cast<char*>(t.data.data()), static_cast<std::streamsize>(count * 4)))
            throw std::runtime_error("truncated .ddspw");
        out.emplace(std::move(name), std::move(t));
    }
    return out;
}

}  // namespace ddaw::ddsp
