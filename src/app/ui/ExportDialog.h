#pragma once
// The export dialog: format, range, stems; runs the render on a worker thread with a progress bar and a
// cancel button, and reports back through `onDone`.
#include <atomic>
#include <thread>

#include "app/ui/Export.h"
#include "app/ui/View.h"

namespace ddaw::ui {

class ExportDialog : public juce::Component, private juce::Timer {
public:
    // The project is copied at construction: the user may keep editing while the export runs.
    ExportDialog(app::AppModel& m, std::function<void(const juce::String&)> onDone);
    ~ExportDialog() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    // The options chosen so far (public for tests).
    app::ExportOptions options() const { return opt_; }
    void start(const juce::File& base);
    // Handles a finished run (the timer calls it ~15 times a second; tests call it directly).
    void pump();   // what the Export button does after the file is chosen

private:
    void timerCallback() override { pump(); }
    void chooseFile();
    void refresh();

    app::AppModel& model_;
    std::shared_ptr<const project::Project> project_;
    std::shared_ptr<project::SampleBank> bank_;
    app::ExportOptions opt_;
    std::function<void(const juce::String&)> onDone_;
    Chip bits_[3] = {Chip("16-bit"), Chip("24-bit"), Chip("32-bit float")};
    Chip rate_[3] = {Chip("44.1 kHz"), Chip("48 kHz"), Chip("96 kHz")};
    Chip range_[3] = {Chip("Arrangement"), Chip("Loop region"), Chip("Scene")};
    Chip stems_{"Stems"}, mix_{"Mixdown"}, go_{"Export...", col::accent}, cancel_{"Cancel"};
    std::unique_ptr<juce::FileChooser> chooser_;
    std::thread worker_;
    std::atomic<bool> running_{false}, cancelFlag_{false};
    std::atomic<double> fraction_{0.0};
    juce::String label_ = {}, result_;
    juce::CriticalSection labelLock_;
    ExportResult finished_;
    std::atomic<bool> finishedFlag_{false};
};

}  // namespace ddaw::ui
