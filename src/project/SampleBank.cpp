#include "project/SampleBank.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace ddaw::project {

namespace {
uint32_t rd32(const unsigned char* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint16_t rd16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

SamplePtr decodeWavSample(const std::vector<unsigned char>& b) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
        throw std::runtime_error("not a WAV file");
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t sr = 0;
    const unsigned char* data = nullptr;
    size_t dataLen = 0;
    for (size_t p = 12; p + 8 <= b.size();) {
        const uint32_t len = rd32(&b[p + 4]);
        const unsigned char* body = &b[p + 8];
        const size_t avail = std::min<size_t>(len, b.size() - (p + 8));
        if (!std::memcmp(&b[p], "fmt ", 4) && avail >= 16) {
            fmt = rd16(body); ch = rd16(body + 2); sr = rd32(body + 4); bits = rd16(body + 14);
            if (fmt == 0xFFFE && avail >= 26) fmt = rd16(body + 24);  // WAVE_FORMAT_EXTENSIBLE: the sub-format tag
        } else if (!std::memcmp(&b[p], "data", 4)) {
            data = body; dataLen = avail;
        }
        p += 8 + len + (len & 1);
    }
    if (!data || ch < 1 || sr == 0) throw std::runtime_error("WAV has no usable fmt/data chunks");
    const bool pcm = fmt == 1 && (bits == 16 || bits == 24 || bits == 32), flt = fmt == 3 && bits == 32;
    if (!pcm && !flt) throw std::runtime_error("unsupported WAV sample format");

    const size_t bytes = bits / 8, frames = dataLen / (bytes * ch);
    auto buf = std::make_shared<SampleBuf>();
    buf->sampleRate = static_cast<float>(sr);
    buf->l.resize(frames);
    if (ch >= 2) buf->r.resize(frames);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < std::min<int>(ch, 2); ++c) {
            const unsigned char* q = data + (i * ch + size_t(c)) * bytes;
            float v;
            if (flt) { uint32_t u = rd32(q); std::memcpy(&v, &u, 4); }
            else if (bits == 16) v = float(int16_t(rd16(q))) / 32768.0f;
            else if (bits == 24) v = float((int32_t(q[0] | (q[1] << 8) | (q[2] << 16)) << 8) >> 8) / 8388608.0f;
            else v = float(int32_t(rd32(q))) / 2147483648.0f;
            (c == 0 ? buf->l : buf->r)[i] = v;
        }
    return buf;
}

void writeWavFloat32(const std::string& path, const SampleBuf& buf) {
    const size_t frames = buf.l.size();
    const uint16_t ch = buf.r.empty() ? 1 : 2;
    const uint32_t dataLen = static_cast<uint32_t>(frames * ch * 4), sr = static_cast<uint32_t>(buf.sampleRate);
    std::vector<unsigned char> o;
    auto w32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<unsigned char>(x >> (8 * i))); };
    auto w16 = [&](uint16_t x) { o.push_back(static_cast<unsigned char>(x)); o.push_back(static_cast<unsigned char>(x >> 8)); };
    auto tag = [&](const char* t) { o.insert(o.end(), t, t + 4); };
    tag("RIFF"); w32(36 + dataLen); tag("WAVE");
    tag("fmt "); w32(16); w16(3); w16(ch); w32(sr); w32(sr * ch * 4); w16(static_cast<uint16_t>(ch * 4)); w16(32);
    tag("data"); w32(dataLen);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c) {
            const float v = c == 0 ? buf.l[i] : buf.r[i];
            uint32_t u; std::memcpy(&u, &v, 4); w32(u);
        }
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
}

SamplePtr loadWavSample(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), {});
    return decodeWavSample(bytes);
}

}  // namespace ddaw::project
