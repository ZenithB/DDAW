#pragma once
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace ddaw::ui {

// A modal text prompt (rename). `done` is called with the new text only when the user confirms.
inline void promptText(const juce::String& title, const juce::String& initial, std::function<void(juce::String)> done) {
    auto* w = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("text", initial);
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([w, done = std::move(done)](int r) {
        if (r == 1 && done) done(w->getTextEditorContents("text"));
    }), true);
}

}  // namespace ddaw::ui
