#pragma once
// One channel's mixing section (ARCH 31): the header of its column in the session view. A track or a bus has a fader with a meter, pan,
// mute and solo (and record arm / input monitoring on tracks that can), a send level to every bus it may send to, and, on a bus, the
// output chooser; the master strip is just the master fader. A compact mode keeps the name and the buttons for small windows.
#include <memory>
#include <vector>

#include "app/model/AppModel.h"
#include "app/ui/Widgets.h"

namespace ddaw::ui {

class MixStrip : public juce::Component {
public:
    enum class Role { Track, Bus, Master };
    static constexpr int kW = 122, kMasterW = 112, kOpenH = 200, kClosedH = 48, kSendRowH = 15;

    MixStrip(app::AppModel& m, project::Uid uid, Role role, size_t colourIndex);
    void sync();                       // read the document
    void setPeak(float p) { fader_.setPeak(p); }
    void setCompact(bool c);
    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    std::function<void(project::Uid)> onContext;       // right-click on the name: the track menu
    project::Uid uid() const { return uid_; }
    Role role() const { return role_; }

    // for tests
    Fader& fader() { return fader_; }
    BarSlider& pan() { return pan_; }
    BarSlider* send(const std::string& busId);         // the send row to this bus, if it is shown
    int sendRows() const { return int(rows_.size()); }
    int hiddenSends() const { return hidden_; }
    Chip& mute() { return mute_; }
    Chip& solo() { return solo_; }
    Chip& arm() { return arm_; }
    Chip& monitor() { return mon_; }
    Chip& stop() { return stop_; }
    Chip& out() { return out_; }
    Chip& more() { return more_; }
    juce::String name() const { return name_; }
    juce::String outLabel() const { return out_.text(); }

private:
    void rebuildSends(const project::Track& t);
    void outMenu();
    app::AppModel& model_;
    project::Uid uid_;
    Role role_;
    size_t colour_;
    bool compact_ = false, selected_ = false, audio_ = false, armable_ = false, clipTrack_ = false;
    juce::String name_;
    Fader fader_;
    BarSlider pan_;
    Chip mute_{"M", col::queued}, solo_{"S", col::accent}, arm_{"R", col::rec}, mon_{"IN", col::meterMid}, stop_{"", col::dim}, out_{"Out: Master"}, more_{"+"};
    struct Row { std::string busId; std::unique_ptr<BarSlider> slider; };
    std::vector<Row> rows_;
    std::vector<std::string> targetIds_;               // every bus this strip may send to, shown or not
    int hidden_ = 0;
};

}  // namespace ddaw::ui
