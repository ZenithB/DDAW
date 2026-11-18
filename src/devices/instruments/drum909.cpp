// `drum909`: a punchy 909-style kit: a hard sine kick with a click and drive, a snare of tone and bright noise, a tight clap, hats of metal and noise, a tom, a rim shot and a crash.
// Eight pads (pitch mod 8): kick, snare, clap, closed hat, open hat, tom, rim shot, crash. Controls per pad: p{i}_tune, p{i}_decay, p{i}_level (schema/SchemaExt.h). The engine and the voices are in DrumKit.h.
#include "devices/instruments/DrumKit.h"
#include "devices/schema/SchemaExt.h"

namespace ddaw::devices {
namespace {
using namespace drumkit;
using Sm = dsp::SvfMode;
const std::array<Machine::Factory, kPads> kVoices = {{
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Kick>(KickCfg{260.0f, 52.0f, 0.02f, 0.45f, 3000.0f, 3.2f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Snare>(SnareCfg{185.0f, 335.0f, 0.55f, 0.09f, 1500.0f, 0.7f, 0.9f, Sm::Highpass}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Clap>(ClapCfg{1200.0f, 1.6f, 0.012f, 0.010f, 0.8f, 3}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Hat>(HatCfg{0.55f, 0.45f, 7500.0f, 1.35f, 11000.0f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Hat>(HatCfg{0.55f, 0.45f, 7500.0f, 1.35f, 11000.0f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Tom>(TomCfg{230.0f, 115.0f, 0.03f, 0.08f}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<TwoTone>(TwoToneCfg{455.0f, 1667.0f, 1600.0f, 1.2f, false}); },
    +[]() -> std::unique_ptr<PadVoice> { return std::make_unique<Hat>(HatCfg{0.45f, 0.6f, 4500.0f, 1.3f, 7000.0f}); }
}};
}  // namespace

std::unique_ptr<InstrumentDevice> make_drum909() { return std::make_unique<Machine>(schema::kInstDrum909, kVoices); }

}  // namespace ddaw::devices
