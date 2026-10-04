#include "app/ui/Export.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace ddaw::ui {

namespace {

// TPDF dither at 1 LSB of a 16-bit target: the sum of two uniform variables, so quantisation error does
// not correlate with the signal.
struct Dither {
    uint32_t s = 0x9E3779B9u;
    float uniform() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return float(s >> 8) * (1.0f / 16777216.0f); }
    float tpdf() { return (uniform() - uniform()) * (1.0f / 32768.0f); }
};

void writeWav(const juce::File& f, const std::vector<float>& l, const std::vector<float>& r, double sr, int bits, bool dither) {
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(f.createOutputStream());
    if (!stream) throw std::runtime_error("cannot write " + f.getFullPathName().toStdString());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(sr).withNumChannels(2).withBitsPerSample(bits));
    if (!writer) throw std::runtime_error("cannot create a WAV writer");
    const float* chans[2] = {l.data(), r.data()};
    std::vector<float> dl, dr;
    if (bits == 16 && dither) {   // dither on copies, in blocks of the whole file (a few MB at most)
        Dither d;
        dl = l; dr = r;
        for (auto& v : dl) v = std::clamp(v + d.tpdf(), -1.0f, 1.0f);
        for (auto& v : dr) v = std::clamp(v + d.tpdf(), -1.0f, 1.0f);
        chans[0] = dl.data();
        chans[1] = dr.data();
    }
    if (!writer->writeFromFloatArrays(chans, 2, int(l.size()))) throw std::runtime_error("writing the WAV failed");
}

}  // namespace

ExportResult exportProject(const project::Project& p, const project::SampleBank& bank, const app::ExportOptions& o, const juce::File& base,
                           std::atomic<bool>* cancel, std::function<void(double, const juce::String&)> progress) {
    ExportResult r;
    try {
        std::string err;
        auto jobs = app::planExport(p, o, err);
        if (jobs.empty()) { r.message = err.empty() ? "Nothing to export" : "Cannot export: " + juce::String(err); return r; }
        const int bits = o.bitDepth == 16 || o.bitDepth == 32 ? o.bitDepth : 24;
        const auto dir = base.getParentDirectory();
        const auto stem = base.getFileNameWithoutExtension();
        for (size_t j = 0; j < jobs.size(); ++j) {
            auto& job = jobs[j];
            const double lo = double(j) / double(jobs.size()), span = 1.0 / double(jobs.size());
            job.render.progress = [&, lo, span](double f) {
                if (progress) progress(lo + f * span, juce::String(job.label));
                return !(cancel && cancel->load());
            };
            const auto res = engine::renderFixture(job.fixture, o.sampleRate, job.render, &bank);
            if (res.cancelled) { r.cancelled = true; r.message = "Export cancelled"; for (auto& f : r.files) f.deleteFile(); r.files.clear(); return r; }
            const auto file = job.suffix.empty() ? base.withFileExtension("wav") : dir.getChildFile(stem + juce::String(job.suffix) + ".wav");
            writeWav(file, res.l, res.r, o.sampleRate, bits, o.dither);
            r.files.push_back(file);
            if (j == 0) r.seconds = double(res.l.size()) / o.sampleRate;
            r.unsupported += res.unsupported.size();
        }
        r.ok = true;
        if (progress) progress(1.0, "Done");
        r.message = r.files.size() == 1 ? "Exported " + r.files[0].getFileName() + juce::String::formatted(" (%.1f s)", r.seconds)
                                        : "Exported " + juce::String(int(r.files.size())) + " files to " + dir.getFileName() + juce::String::formatted(" (%.1f s each)", r.seconds);
        if (r.unsupported) r.message << "  [" << juce::String(int(r.unsupported)) << " unsupported feature(s)]";
    } catch (const std::exception& e) {
        r.ok = false;
        r.message = juce::String("Export failed: ") + e.what();
    }
    return r;
}

ExportResult exportAudio(const project::Project& p, const project::SampleBank& bank, const std::string& sceneId, const juce::File& out, double sr) {
    app::ExportOptions o;
    o.sampleRate = sr;
    o.sceneId = sceneId;
    return exportProject(p, bank, o, out);
}

}  // namespace ddaw::ui
