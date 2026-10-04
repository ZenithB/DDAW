#include "engine/AudioClips.h"

#include <algorithm>
#include <numbers>

namespace ddaw::engine {

void DerivedClip::read(double pos, float& outL, float& outR) const noexcept {
    const size_t n = l.size();
    if (n == 0 || pos < 0.0) { outL = outR = 0.0f; return; }
    const size_t i = static_cast<size_t>(pos);
    if (i >= n) { outL = outR = 0.0f; return; }
    const float frac = static_cast<float>(pos - double(i));
    const size_t i2 = i + 1 < n ? i + 1 : (looped ? 0 : i);
    outL = l[i] + (l[i2] - l[i]) * frac;
    outR = r.empty() ? outL : r[i] + (r[i2] - r[i]) * frac;
}

std::optional<DerivedClip> DerivedClip::build(const project::AudioClipData& a, const SampleBuf& buf, double engineSr, double bpm) {
    const size_t rawLen = buf.frames();
    if (rawLen == 0 || buf.sampleRate <= 0.0f || engineSr <= 0.0) return std::nullopt;
    const double sr = buf.sampleRate;
    const double offset = std::max(a.offset.value_or(0.0), 0.0);
    const double dur = a.dur.value_or(0.0);
    const bool looped = a.loop != 0.0;
    const double xfade = a.xfade.value_or(0.0);
    const size_t s0 = std::min(static_cast<size_t>(offset * sr), rawLen - 1);
    const size_t maxN = rawLen - s0;
    const size_t n = dur > 0.0 ? std::clamp(static_cast<size_t>(dur * sr), size_t{1}, maxN) : maxN;
    const size_t xf = (looped && xfade > 0.0) ? std::min(static_cast<size_t>(xfade * sr), n / 2) : 0;
    const size_t outLen = std::max<size_t>(xf > 0 ? n - xf : n, 1);

    auto derive = [&](const std::vector<float>& src) {
        std::vector<float> dst(outLen, 0.0f);
        for (size_t i = 0; i < outLen; ++i) {
            if (xf > 0 && i < xf) {  // crossfade the region tail (out) into its head (in), equal power
                const float t = static_cast<float>(i) / static_cast<float>(xf);
                const float fin = std::sin(t * float(std::numbers::pi / 2)), fout = std::cos(t * float(std::numbers::pi / 2));
                dst[i] = src[s0 + i] * fin + src[s0 + (n - xf) + i] * fout;
            } else {
                dst[i] = s0 + i < src.size() ? src[s0 + i] : 0.0f;
            }
        }
        if (a.rev != 0.0) std::reverse(dst.begin(), dst.end());
        return dst;
    };
    DerivedClip c;
    c.l = derive(buf.l);
    if (!buf.r.empty()) c.r = derive(buf.r);
    const double rate = std::pow(2.0, (a.pitch + a.cents.value_or(0.0) / 100.0) / 12.0);
    c.step = rate * sr / engineSr;
    c.looped = looped;
    c.gain = std::pow(10.0f, static_cast<float>(a.gainDb) / 20.0f);
    c.fadeIn = std::max(a.fadeIn, 0.0) / 96.0 * 60.0 / bpm * engineSr;    // ticks -> seconds -> samples
    c.fadeOut = std::max(a.fadeOut, 0.0) / 96.0 * 60.0 / bpm * engineSr;
    return c;
}

void AudioPlayer::initSessionVoice(const AudioSlot& s, double anchor, double now, double tps) noexcept {
    const DerivedClip& clip = clips[static_cast<size_t>(s.clip)];
    const double lenF = std::max(clip.frames(), 1.0);
    Voice v;
    v.active = true;
    v.clip = s.clip;
    v.anchor = anchor;
    if (s.looped) {
        v.beginTick = anchor;
        if (now > anchor) {
            const double elapsed = (now - anchor) / tps;
            v.pos = std::fmod(elapsed * clip.step, lenF);
            v.played = elapsed;
        }
    } else {
        const double period = std::max(s.loopLen, 1.0);
        const double last = now > anchor ? anchor + std::floor((now - anchor) / period) * period : anchor;
        v.beginTick = last;
        v.nextFire = last + period;
        v.firePeriod = period;
        if (now > last) {
            const double elapsed = (now - last) / tps;
            v.pos = elapsed * clip.step;  // past the end: it waits, armed for the next fire
            v.played = elapsed;
        }
    }
    voices_[0] = v;
}

void AudioPlayer::activateArr(const AudioArrEv& ev, double pos, double played) noexcept {
    size_t vi = 1;
    double oldest = INFINITY;
    int freeIdx = -1;
    for (size_t i = 1; i < voices_.size(); ++i) {
        if (!voices_[i].active) { freeIdx = static_cast<int>(i); break; }
        if (voices_[i].beginTick < oldest) { oldest = voices_[i].beginTick; vi = i; }
    }
    Voice v;
    v.active = true; v.clip = ev.clip; v.pos = pos; v.played = played; v.beginTick = ev.tick; v.stopTick = ev.stopTick;
    voices_[freeIdx >= 0 ? static_cast<size_t>(freeIdx) : vi] = v;
}

void AudioPlayer::render(float* l, float* r, int n, double pos, double tps, TransportMode mode, bool playing,
                         const std::optional<AudioSlot>& launched, double anchor) noexcept {
    if (clips.empty()) return;
    if (!playing || tps <= 0.0) { expected_ = NAN; return; }
    const double end = pos + double(n) * tps;
    const bool resync = !std::isfinite(expected_) || std::abs(pos - expected_) > tps * 0.5 || mode != mode_;
    mode_ = mode;
    expected_ = end;
    if (resync) {
        for (auto& v : voices_) v.active = false;
        if (mode == TransportMode::Arrangement) {
            arrIdx_ = static_cast<size_t>(std::lower_bound(arr.begin(), arr.end(), pos - 1e-9,
                                                           [](const AudioArrEv& e, double v) { return e.tick < v; }) - arr.begin());
            for (size_t ei = 0; ei < arrIdx_; ++ei) {  // clips already begun and still audible restart mid-flight
                const AudioArrEv ev = arr[ei];
                const DerivedClip& clip = clips[static_cast<size_t>(ev.clip)];
                if (ev.stopTick + clip.fadeOut * tps <= pos) continue;
                const double elapsed = (pos - ev.tick) / tps;
                double fpos = elapsed * clip.step;
                if (clip.looped) fpos = std::fmod(fpos, std::max(clip.frames(), 1.0));
                else if (fpos >= clip.frames()) continue;
                activateArr(ev, fpos, elapsed);
            }
        }
    }
    if (mode == TransportMode::Session) {
        if (!launched) voices_[0].active = false;
        else if (!voices_[0].active || voices_[0].clip != launched->clip || voices_[0].anchor != anchor) initSessionVoice(*launched, anchor, pos, tps);
    } else {
        while (arrIdx_ < arr.size() && arr[arrIdx_].tick < end) activateArr(arr[arrIdx_++], 0.0, 0.0);
    }

    for (auto& v : voices_) {
        if (!v.active) continue;
        if (static_cast<size_t>(v.clip) >= clips.size()) { v.active = false; continue; }
        const DerivedClip& clip = clips[static_cast<size_t>(v.clip)];
        const double lenF = clip.frames();
        for (int k = 0; k < n; ++k) {
            const double tick = pos + double(k) * tps;
            if (tick >= v.nextFire) {  // session one-shot: re-fire from the head each clip length
                v.pos = 0.0; v.played = 0.0; v.beginTick = v.nextFire;
                v.nextFire += std::max(v.firePeriod, tps);
            }
            if (tick < v.beginTick) continue;
            if (!clip.looped && v.pos >= lenF) {
                if (std::isfinite(v.nextFire)) continue;  // armed one-shot: silent until the next fire
                v.active = false;
                break;
            }
            float g = clip.gain;
            if (tick >= v.stopTick) {
                if (clip.fadeOut <= 0.0) { v.active = false; break; }
                const double t = (tick - v.stopTick) / tps;  // Tone OneShotSource: the fade-out BEGINS at the stop time
                if (t >= clip.fadeOut) { v.active = false; break; }
                g *= static_cast<float>(1.0 - t / clip.fadeOut);
            }
            if (clip.fadeIn > 0.0 && v.played < clip.fadeIn) g *= static_cast<float>(v.played / clip.fadeIn);
            float sl, sr;
            clip.read(v.pos, sl, sr);
            l[k] += sl * g;
            r[k] += sr * g;
            v.pos += clip.step;
            if (clip.looped && v.pos >= lenF) v.pos -= lenF;
            v.played += 1.0;
        }
    }
}

}  // namespace ddaw::engine
