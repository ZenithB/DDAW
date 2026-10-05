#pragma once
#include <set>

#include "app/ui/View.h"

namespace ddaw::ui {

// Instruments, effects, MIDI effects and samples. Double-click adds to the selected track; rows drag
// ("device:<chain>:<type>" / "sample:<id>") onto the device chain, the session grid and the arrangement.
class BrowserPanel : public View {
public:
    explicit BrowserPanel(app::AppModel& m);
    void paint(juce::Graphics&) override;
    void resized() override;
    void refresh(app::ModelEvent) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    std::function<void()> onImportSample;
    // Scan for plugins: the app runs it (it can take seconds) and calls refresh when done. Without a hook the browser scans inline.
    std::function<void()> onScanPlugins;

    struct Row {
        enum Kind { Header, Device, Sample, Import, Plugin, Scan } kind = Header;
        juce::String label;
        app::Chain chain = app::Chain::Effect;
        std::string key;   // device type or sample id
        bool open = true;
    };
    static constexpr int kRowH = 22;

    // Adds the row's item to the current selection (also what a double-click does). Public for tests.
    bool activate(const Row&);
    const std::vector<Row>& rows() const { return rows_; }

private:
    void rebuild();
    int rowAt(int y) const { return (y - 34 + scroll_) / kRowH; }

    juce::TextEditor search_;
    std::vector<Row> rows_;
    std::set<juce::String> closed_;
    int scroll_ = 0;
    int pressed_ = -1;
};

}  // namespace ddaw::ui
