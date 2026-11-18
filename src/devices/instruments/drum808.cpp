// `drum808`: an analogue-style 808 kit: a long sine kick, a two-tone snare, a four-burst clap with a tail, metallic hats and cymbal, a tom and a cowbell.
// Eight pads (pitch mod 8): kick, snare, clap, closed hat, open hat, low tom, cowbell, cymbal. Controls per pad: p{i}_tune, p{i}_decay, p{i}_level (schema/SchemaExt.h). The engine and the voices are in DrumKit.h.
#include "devices/instruments/DrumKit.h"
#include "devices/schema/SchemaExt.h"

namespace ddaw::devices {
namespace {
using namespace drumkit;
using Sm = dsp::SvfMode;
const std::array<Machine::Factory, kPads> kVoices = {{
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Kick>(KickCfg{170.0f, 48.0f, 0.03f, 0.12f, 1800.0f, 1.3f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Snare>(SnareCfg{175.0f, 330.0f, 0.7f, 0.1f, 2400.0f, 0.8f, 0.7f, Sm::Bandpass}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Clap>(ClapCfg{1100.0f, 1.1f, 0.011f, 0.012f, 1.0f, 4}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Hat>(HatCfg{0.9f, 0.1f, 7000.0f, 1.0f, 9500.0f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Hat>(HatCfg{0.9f, 0.1f, 7000.0f, 1.0f, 9500.0f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Tom>(TomCfg{165.0f, 95.0f, 0.07f, 0.03f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<TwoTone>(TwoToneCfg{540.0f, 800.0f, 1400.0f, 1.6f, true}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Hat>(HatCfg{0.7f, 0.35f, 5500.0f, 1.1f, 8000.0f}); }
}};
}  // namespace

std::unique_ptr<InstrumentDevice> make_drum808() { return std::make_unique<Machine>(schema::kInstDrum808, kVoices); }

}  // namespace ddaw::devices
