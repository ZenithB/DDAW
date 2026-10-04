#include "engine/Recorder.h"

#include <chrono>
#include <cstdint>
#include <cstring>

namespace ddaw::engine {

namespace {

void put32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void put16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

// A canonical 44-byte IEEE-float WAV header; sizes are patched when the take ends.
void writeHeader(FILE* f, int channels, double sr, uint32_t dataBytes) {
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 3);   // IEEE float
    put16(f, uint16_t(channels));
    put32(f, uint32_t(sr));
    put32(f, uint32_t(sr * channels * 4));
    put16(f, uint16_t(channels * 4));
    put16(f, 32);
    std::fwrite("data", 1, 4, f);
    put32(f, dataBytes);
}

}  // namespace

Recorder::~Recorder() {
    if (running_.load()) stop(500);
}

bool Recorder::start(const std::string& path, int channels, double sampleRate, std::string& error) {
    if (running_.load()) { error = "already recording"; return false; }
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) { error = "cannot write " + path; return false; }
    path_ = path;
    channels_ = channels == 1 ? 1 : 2;
    sr_ = sampleRate;
    written_ = 0;
    writeHeader(file_, channels_, sr_, 0);
    constexpr size_t kChunk = 4096;
    bufL_.assign(kChunk, 0.0f);
    bufR_.assign(kChunk, 0.0f);
    inter_.assign(kChunk * 2, 0.0f);
    // the ring must be empty before the engine starts feeding it
    while (engine_.inputTap().pop(bufL_.data(), bufR_.data(), kChunk) > 0) {}
    finishing_ = false;
    running_ = true;
    writer_ = std::thread([this] { writerLoop(); });
    engine_.requestCapture(true);
    return true;
}

void Recorder::drainOnce(bool& any) {
    const size_t n = engine_.inputTap().pop(bufL_.data(), bufR_.data(), bufL_.size());
    any = n > 0;
    if (!n) return;
    if (channels_ == 1) std::fwrite(bufL_.data(), sizeof(float), n, file_);
    else {
        for (size_t i = 0; i < n; ++i) { inter_[2 * i] = bufL_[i]; inter_[2 * i + 1] = bufR_[i]; }
        std::fwrite(inter_.data(), sizeof(float), 2 * n, file_);
    }
    written_ += n;
}

void Recorder::writerLoop() {
    bool any = false;
    while (!finishing_.load(std::memory_order_acquire)) {
        drainOnce(any);
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    do { drainOnce(any); } while (any);   // whatever the engine pushed before it acknowledged the stop
}

Recorder::Take Recorder::stop(int timeoutMs) {
    Take t;
    t.path = path_;
    t.sampleRate = sr_;
    t.channels = channels_;
    if (!running_.load()) { t.error = "not recording"; return t; }
    engine_.requestCapture(false);
    // the audio thread acknowledges at its next callback
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (engine_.captureInfo().active && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto info = engine_.captureInfo();
    if (info.active) t.error = "the audio thread did not stop the capture";
    finishing_ = true;
    if (writer_.joinable()) writer_.join();
    running_ = false;
    // patch the sizes
    std::fseek(file_, 0, SEEK_SET);
    writeHeader(file_, channels_, sr_, uint32_t(written_ * uint64_t(channels_) * 4));
    std::fclose(file_);
    file_ = nullptr;
    t.startTick = info.startTick;
    t.frames = written_;
    t.dropped = info.dropped;
    t.ok = t.error.empty() && written_ > 0;
    if (t.error.empty() && written_ == 0) t.error = "nothing was recorded (the transport never started, or no input reached the engine)";
    return t;
}

}  // namespace ddaw::engine
