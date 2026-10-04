#pragma once
// The recording options popup: count-in, input monitoring, latency offset and input channels.
#include "app/ui/View.h"

namespace ddaw::ui {

class RecordOptions : public juce::Component {
public:
    explicit RecordOptions(app::AppModel& m);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void refreshChips();
    app::AppModel& model_;
    Chip countIn_[3] = {Chip("0"), Chip("1"), Chip("2")};
    Chip monitor_{"MONITOR", col::accent};
    Chip mode_[3] = {Chip("L+R"), Chip("LEFT"), Chip("RIGHT")};
    NumberBox offset_{-500, 500, 0, " ms"};
    juce::String info_;
};

}  // namespace ddaw::ui
