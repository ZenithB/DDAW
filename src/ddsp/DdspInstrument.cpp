#include "ddsp/DdspInstrument.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

#include "devices/Registry.h"
#include "ddsp/Weights.h"
#include "dsp/Math.h"

namespace ddaw::ddsp {

namespace {

std::string gModelsDir;
const char* const kModelNames[4] = {"violin", "flute", "tenor_saxophone", "trumpet"};
constexpr float kSilenceDb = -80.0f;

std::vector<std::string> searchDirs() {
    std::vector<std::string> d;
    if (!gModelsDir.empty()) d.push_back(gModelsDir);
    if (const char* e = std::getenv("DDAW_MODELS")) d.emplace_back(e);
    if (const char* h = std::getenv("HOME")) d.push_back(std::string(h) + "/Library/Application Support/DDAW/models");
#ifdef DDAW_MODELS_DIR
    d.push_back(std::string(DDAW_MODELS_DIR) + "/export");
#endif
    return d;
}

std::string modelPath(int index) {
    for (const auto& dir : searchDirs()) {
        const auto p = std::filesystem::path(dir) / (std::string(kModelNames[std::clamp(index, 0, 3)]) + ".ddspw");
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p.string();
    }
    return {};
}

std::unique_ptr<InstrumentDevice> makeDdsp() { return std::make_unique<DdspInstrument>(); }

}  // namespace

void setModelsDirectory(const std::string& dir) { gModelsDir = dir; }
std::string modelsDirectory() { return gModelsDir; }
const char* modelName(int i) { return kModelNames[std::clamp(i, 0, 3)]; }
bool modelAvailable(int i) { return !modelPath(i).empty(); }
void registerDevices() { registerInstrument("ddsp", &makeDdsp); }

// ------------------------------------------------------------------ construction

DdspInstrument::DdspInstrument() {
    level_.prepare(48000.0f, 15.0f);
    level_.snap(1.0f);
    audioQ_.prepare(1 << 15);
    synth_.prepare(nullptr, 0);
    prepare(48000.0, 128);
}

DdspInstrument::~DdspInstrument() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void DdspInstrument::prepare(double sr, int) {
    // the worker owns the synthesis chain: stop it while the rate changes, and clear what it left behind
    if (thread_.joinable()) { stop_ = true; thread_.join(); }
    Feat dropF;
    while (featQ_.pop(dropF)) {}
    Blk dropB;
    while (blkQ_.pop(dropB)) {}
    audioQ_.clear();
    sr_ = std::max(sr, 16000.0);
    hopHost_ = double(kHopSamples) * sr_ / double(kModelSampleRate);
    hopAccum_ = 0.0;
    resampler_.prepare(double(kModelSampleRate), sr_);
    outBuf_.assign(size_t(kHopSamples * std::ceil(sr_ / double(kModelSampleRate)) + 64), 0.0f);
    // delay between a feature frame and its sound: the synth runs one frame behind, the converter looks ahead,
    // plus a reserve that absorbs the worker's timing jitter
    latency_ = int(std::ceil(hopHost_)) + int(std::ceil(double(resampler_.latencyIn()) * sr_ / double(kModelSampleRate))) + 192;
    level_.prepare(float(sr_), 15.0f);
    state_ = State::Idle;
    silentRun_ = 0;
    curRemain_ = 0;
    silenceLeft_ = 0;
    pushed_ = 0;
    ++gen_;
    progress_.store(~uint64_t(0));
    stop_ = false;
    thread_ = std::thread([this] { inferenceLoop(); });
}

DdspStats DdspInstrument::stats() const {
    DdspStats s;
    s.framesDecoded = decoded_.load();
    s.underruns = underruns_.load();
    s.modelReady = ready_.load();
    s.model = loadedModel_.load();
    return s;
}

// ------------------------------------------------------------------ worker thread

bool DdspInstrument::loadModel(int index) {
    const std::string path = modelPath(index);
    if (path.empty()) return false;
    try {
        auto dec = std::make_unique<Decoder>();
        dec->load(path);
        const auto weights = loadWeights(path);
        const auto it = weights.find("reverb.ir");
        auto rev = std::make_unique<ReverbConv>();
        if (it != weights.end()) rev->prepare(it->second.data.data(), it->second.data.size());
        decoder_ = std::move(dec);
        synth_.swapReverb(rev.get());
        reverb_ = std::move(rev);   // (the previous one is released here: nothing refers to it any more)
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void DdspInstrument::inferenceLoop() {
#ifdef __APPLE__
    {   // the audio thread depends on this thread's deadlines: a time-constraint thread (one frame every 4 ms, a ms or so of work)
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        const auto toAbs = [&](double ms) { return uint32_t(ms * 1e6 * double(tb.denom) / double(tb.numer)); };
        thread_time_constraint_policy_data_t pol;
        pol.period = toAbs(4.0);
        pol.computation = toAbs(1.5);
        pol.constraint = toAbs(3.5);
        pol.preemptible = 1;
        if (thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&pol), THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS)
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
#endif
    Feat f;
    uint32_t seenGen = ~0u, nDone = 0;
    int idleSpins = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        const int want = wantModel_.load(std::memory_order_acquire);
        if (want != loadedModel_.load(std::memory_order_relaxed)) {
            ready_ = false;
            if (loadModel(want)) { loadedModel_ = want; ready_ = true; }
            else loadedModel_ = want;   // tried: the file is missing or unreadable; stay not ready
            continue;
        }
        if (!featQ_.pop(f)) {
            // spin briefly (a frame is due every 4 ms), then sleep: a busy core is only worth it while frames flow
            if (++idleSpins < 40) { std::this_thread::yield(); continue; }
            std::this_thread::sleep_for(std::chrono::microseconds(150));
            continue;
        }
        idleSpins = 0;
        if (!ready_.load(std::memory_order_acquire) || !decoder_) continue;
        if (f.gen != seenGen) {   // a new sound: a clean slate for the network, the synthesis and the converter
            decoder_->reset();
            synth_.reset();
            resampler_.reset();
            seenGen = f.gen;
            nDone = 0;
        }
        decoder_->step(f.ld, f.f0s, raw_);
        decoded_.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < kHopSamples; ++i) {
            rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
            noise64_[i] = float(rng_ >> 8) * (2.0f / 16777216.0f) - 1.0f;   // uniform -1..1, as tf.random.uniform
        }
        synth_.setReverb(reverbOn_.load(std::memory_order_relaxed));
        synth_.setNoiseGain(noiseGain_.load(std::memory_order_relaxed));
        if (synth_.push(raw_, f.f0Hz, noise64_, blk64_)) {
            size_t m = 0;
            resampler_.process(blk64_, kHopSamples, [&](float y, double) { if (m < outBuf_.size()) outBuf_[m++] = y; });
            size_t sent = 0;
            while (sent < m && !stop_.load(std::memory_order_acquire)) {
                const size_t k = audioQ_.push(outBuf_.data() + sent, m - sent);
                sent += k;
                if (k == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            while (m > 0 && !blkQ_.push(Blk{f.gen, uint32_t(m)}) && !stop_.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        progress_.store((uint64_t(f.gen) << 32) | ++nDone, std::memory_order_release);
    }
}

// ------------------------------------------------------------------ parameters and input

void DdspInstrument::setParam(uint16_t i, float v) {
    switch (i) {
        case Model: wantModel_.store(std::clamp(int(std::lround(v)), 0, 3), std::memory_order_release); break;
        case Transpose: transpose_ = std::clamp(int(std::lround(v)), -24, 24); break;
        case Level: level_.setTarget(std::pow(10.0f, std::clamp(v, -40.0f, 12.0f) / 20.0f)); break;
        case Reverb: reverbOn_.store(v >= 0.5f, std::memory_order_relaxed); break;
        case Noise: noiseGain_.store(std::clamp(v, 0.0f, 2.0f), std::memory_order_relaxed); break;
        case Attack: attackMs_ = std::clamp(v, 5.0f, 500.0f); break;
        case Release: releaseMs_ = std::clamp(v, 20.0f, 2000.0f); break;
        case Vibrato: vibratoCents_ = std::clamp(v, 0.0f, 100.0f); break;
        default: break;
    }
}

void DdspInstrument::wake() noexcept {
    // leaving idle: a clean slate (the worker resets itself when it sees the new generation), and a stream that
    // starts `latency_` samples behind
    ++gen_;
    curRemain_ = 0;
    silenceLeft_ = latency_;
    lastOut_ = 0.0f;
    pushed_ = 0;
    state_ = State::Active;
}

void DdspInstrument::pushFeature(float ldDb, float f0Hz) noexcept {
    const bool silent = ldDb < -72.0f && f0Hz <= 0.0f;
    if (state_ == State::Idle) {
        if (silent) return;
        wake();
    }
    // silence for a while (long enough for the reverb to die away) lets the network sleep
    if (silent) { if (++silentRun_ > 425) { state_ = State::Idle; silentRun_ = 0; return; } } else silentRun_ = 0;
    Feat f{scaleLoudnessDb(std::clamp(ldDb, kSilenceDb, 0.0f)), scaleF0Hz(f0Hz), f0Hz, gen_.load(std::memory_order_relaxed)};
    if (featQ_.push(f)) ++pushed_;
}

void DdspInstrument::performance(const PerformanceFrame& f) {
    perfAge_ = 0;
    const float shift = std::pow(2.0f, float(transpose_) / 12.0f);
    pushFeature(f.loudnessDb, f.f0Hz > 0.0f ? f.f0Hz * shift : 0.0f);
}

void DdspInstrument::noteOn(uint8_t pitch, float velocity, uint32_t) {
    noteHz_ = dsp::midiHz(float(std::clamp(int(pitch) + transpose_, 0, 127)));
    ldTarget_ = -55.0f + 37.0f * std::clamp(velocity, 0.0f, 1.0f);   // velocity 1 -> -18 dB, the loudest the models heard
    if (!noteHeld_ || curHz_ <= 0.0f) curHz_ = noteHz_;              // legato glides, a fresh note starts on pitch
    noteHeld_ = true;
}

void DdspInstrument::noteOff(uint32_t) { noteHeld_ = false; }

void DdspInstrument::noteFrame() noexcept {
    // 250 Hz envelope for notes: loudness slews linearly in dB (attack = time from silence to the target),
    // pitch glides over ~20 ms, vibrato fades in with the loudness
    const float hopMs = 4.0f;
    if (noteHeld_) ldCur_ = std::min(ldTarget_, ldCur_ + hopMs * (ldTarget_ - kSilenceDb) / attackMs_);
    else ldCur_ = std::max(kSilenceDb, ldCur_ - hopMs * 60.0f / releaseMs_);
    const bool on = ldCur_ > kSilenceDb + 0.5f;
    if (curHz_ > 0.0f && noteHz_ > 0.0f) curHz_ += (noteHz_ - curHz_) * (1.0f - std::exp(-hopMs / 20.0f));
    vibPhase_ += 2.0f * 3.14159265f * 5.5f * hopMs * 0.001f;
    if (vibPhase_ > 6.2831853f) vibPhase_ -= 6.2831853f;
    const float depth = vibratoCents_ * std::clamp((ldCur_ - kSilenceDb) / 30.0f, 0.0f, 1.0f);
    const float hz = curHz_ * std::pow(2.0f, depth * std::sin(vibPhase_) / 1200.0f);
    pushFeature(on ? ldCur_ : kSilenceDb, on ? hz : 0.0f);
}

void DdspInstrument::reset() {
    noteHeld_ = false;
    ldCur_ = kSilenceDb;
    curHz_ = 0.0f;
    state_ = State::Idle;
    silentRun_ = 0;
    perfAge_ = int64_t(1) << 40;
    curRemain_ = 0;
    silenceLeft_ = 0;
    pushed_ = 0;
    ++gen_;   // whatever the worker still produces for the old sound is dropped as it arrives
    level_.snap(level_.target());
}

// ------------------------------------------------------------------ audio

// The next finished sample, or false when the worker has not delivered one yet.
bool DdspInstrument::nextSample(float& o) noexcept {
    if (silenceLeft_ > 0) { --silenceLeft_; o = 0.0f; return true; }
    while (curRemain_ == 0) {
        Blk b;
        if (!blkQ_.pop(b)) return false;
        if (b.gen == gen_.load(std::memory_order_relaxed)) curRemain_ = b.count;
        else audioQ_.skip(b.count);   // from before the last reset or wake
    }
    audioQ_.pop(&o, 1);
    --curRemain_;
    return true;
}

void DdspInstrument::process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) {
    if (ctx.offline && !ready_.load(std::memory_order_acquire) && wantModel_.load() != loadedModel_.load()) {
        // an offline render starts only once the model is loaded (a missing file is reported by loadedModel_ catching up)
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!ready_.load(std::memory_order_acquire) && wantModel_.load() != loadedModel_.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool tracked = perfAge_ < int64_t(0.1 * sr_);
    perfAge_ = std::min<int64_t>(perfAge_ + n, int64_t(1) << 40);
    if (!tracked) {   // note mode: generate the feature frames ourselves, one per hop
        hopAccum_ += n;
        while (hopAccum_ >= hopHost_) { hopAccum_ -= hopHost_; if (state_ == State::Active || noteHeld_) noteFrame(); }
    }
    if (ctx.offline && state_ == State::Active && pushed_ > 0 && ready_.load(std::memory_order_acquire)) {
        // an offline render has no clock: wait for the worker to catch up with what we sent, for determinism
        const uint64_t target = (uint64_t(gen_.load()) << 32) | pushed_;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (progress_.load(std::memory_order_acquire) != target && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    }
    for (int base = 0; base < n; base += kMaxBlock) {
        const int m = std::min(kMaxBlock, n - base);
        bool starved = false, any = false;
        for (int i = 0; i < m; ++i) {
            float o;
            if (nextSample(o)) { lastOut_ = o; any = true; }
            else if (i == 0 && state_ == State::Idle) { lastOut_ = 0.0f; level_.snap(level_.target()); return; }   // nothing playing
            else { o = lastOut_; lastOut_ *= 0.92f; starved = true; }   // a late block: fade, never click
            mix_[i] = o;
        }
        (void)any;
        for (int i = 0; i < m; ++i) {
            const float o = mix_[i] * level_.next();
            l[base + i] += o;
            r[base + i] += o;
        }
        if (starved && state_ == State::Active && ready_.load(std::memory_order_relaxed)) underruns_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace ddaw::ddsp
