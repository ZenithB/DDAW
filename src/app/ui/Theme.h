#pragma once
// Colours, fonts and small drawing helpers shared by every view.
#include <juce_gui_basics/juce_gui_basics.h>

namespace ddaw::ui {

namespace col {
inline const juce::Colour bg{0xff17191c}, panel{0xff1f2226}, panel2{0xff272b30}, raised{0xff30353b}, line{0xff3a4047},
    text{0xffe8eaed}, dim{0xff8d949e}, faint{0xff5b626b}, accent{0xffff8a3d}, accentDim{0xff9a5424}, play{0xff4ade80},
    queued{0xffe9c46a}, rec{0xffef5350}, meterLow{0xff3fb76a}, meterMid{0xffe0c040}, meterHigh{0xffe5533d}, grid{0xff2c3138},
    gridBar{0xff3a4048}, black{0xff121417};
}  // namespace col

// A stable colour per track slot.
inline juce::Colour trackColour(size_t index) {
    static const juce::uint32 pal[] = {0xff5aa9e6, 0xffee7b6f, 0xff7bc47f, 0xffc990e8, 0xfff2c14e, 0xff4fd0c4, 0xfff08aa8, 0xff9aa5f0, 0xffd9a066, 0xff8ccf5a};
    return juce::Colour(pal[index % std::size(pal)]);
}

inline juce::Font uiFont(float h = 13.0f, bool bold = false) {
    return juce::Font(juce::FontOptions(h, bold ? juce::Font::bold : juce::Font::plain));
}
inline juce::Font monoFont(float h = 12.0f) { return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), h, juce::Font::plain)); }

inline float textWidth(const juce::Font& f, const juce::String& t) { return juce::GlyphArrangement::getStringWidth(f, t); }

inline void fillRounded(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float radius = 4.0f) {
    g.setColour(c);
    g.fillRoundedRectangle(r, radius);
}

}  // namespace ddaw::ui
