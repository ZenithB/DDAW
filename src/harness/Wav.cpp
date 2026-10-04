#include "harness/Wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace ddaw::harness {

namespace {
uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
void wr32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void wr16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void tag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }
}  // namespace

Audio readWav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
        throw std::runtime_error("not a WAV: " + path);

    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t sr = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    for (size_t p = 12; p + 8 <= b.size();) {
        uint32_t len = rd32(&b[p + 4]);
        const uint8_t* body = &b[p + 8];
        size_t avail = std::min<size_t>(len, b.size() - (p + 8));
        if (!std::memcmp(&b[p], "fmt ", 4) && avail >= 16) {
            fmt = rd16(body); ch = rd16(body + 2); sr = rd32(body + 4); bits = rd16(body + 14);
        } else if (!std::memcmp(&b[p], "data", 4)) {
            data = body; dataLen = avail;
        }
        p += 8 + len + (len & 1);
    }
    if (!data || !ch || ch > 2) throw std::runtime_error("unsupported WAV layout: " + path);
    bool pcm16 = fmt == 1 && bits == 16, f32 = fmt == 3 && bits == 32;
    if (!pcm16 && !f32) throw std::runtime_error("unsupported WAV format: " + path);

    size_t bytesPer = bits / 8, n = dataLen / (bytesPer * ch);
    Audio a;
    a.sampleRate = sr;
    a.l.resize(n); a.r.resize(n);
    for (size_t i = 0; i < n; ++i) {
        float s[2];
        for (int c = 0; c < ch; ++c) {
            const uint8_t* q = data + (i * ch + c) * bytesPer;
            if (pcm16) s[c] = float(int16_t(rd16(q))) / 32768.0f;
            else { uint32_t u = rd32(q); std::memcpy(&s[c], &u, 4); }
        }
        a.l[i] = s[0];
        a.r[i] = ch == 2 ? s[1] : s[0];
    }
    return a;
}

void writeWavPcm16(const std::string& path, const Audio& a) {
    size_t n = std::min(a.l.size(), a.r.size());
    std::vector<uint8_t> v;
    uint32_t dataLen = uint32_t(n * 4);
    tag(v, "RIFF"); wr32(v, 36 + dataLen); tag(v, "WAVE");
    tag(v, "fmt "); wr32(v, 16); wr16(v, 1); wr16(v, 2);
    wr32(v, uint32_t(a.sampleRate)); wr32(v, uint32_t(a.sampleRate) * 4); wr16(v, 4); wr16(v, 16);
    tag(v, "data"); wr32(v, dataLen);
    auto q = [](float x) { return uint16_t(int16_t(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f))); };
    for (size_t i = 0; i < n; ++i) { wr16(v, q(a.l[i])); wr16(v, q(a.r[i])); }
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size()));
}

}  // namespace ddaw::harness
