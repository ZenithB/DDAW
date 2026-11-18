// `drumlinn`: a LinnDrum-style digital kit: short, dry, mid-heavy hits run through an 8-bit, held-sample-rate stage for that crunch; kick, snare, clap, hats, tom, cowbell and tambourine.
// Eight pads (pitch mod 8): kick, snare, clap, hat, open hat, tom, cowbell, tambourine. Controls per pad: p{i}_tune, p{i}_decay, p{i}_level (schema/SchemaExt.h). The engine and the voices are in DrumKit.h.
#include "devices/instruments/DrumKit.h"
#include "devices/schema/SchemaExt.h"

namespace ddaw::devices {
namespace {
using namespace drumkit;
using Sm = dsp::SvfMode;
const std::array<Machine::Factory, kPads> kVoices = {{
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Kick>(KickCfg{175.0f, 62.0f, 0.022f, 0.25f, 2200.0f, 1.7f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Snare>(SnareCfg{200.0f, 330.0f, 0.45f, 0.06f, 3200.0f, 0.8f, 0.8f, Sm::Bandpass}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Clap>(ClapCfg{1500.0f, 1.0f, 0.016f, 0.014f, 0.6f, 2}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Hat>(HatCfg{0.25f, 0.8f, 8000.0f, 1.1f, 10000.0f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Hat>(HatCfg{0.25f, 0.8f, 8000.0f, 1.1f, 10000.0f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Tom>(TomCfg{190.0f, 125.0f, 0.04f, 0.05f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<TwoTone>(TwoToneCfg{560.0f, 845.0f, 1500.0f, 1.4f, true}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Clap>(ClapCfg{7000.0f, 1.0f, 0.006f, 0.008f, 1.0f, 3}); }
}};
}  // namespace

std::unique_ptr<InstrumentDevice> make_drumlinn() { return std::make_unique<Machine>(schema::kInstDrumLinn, kVoices, Digital{8, 24000.0f}); }

}  // namespace ddaw::devices
