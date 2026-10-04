#pragma once
// Base for every panel: holds the model, gets told when it changes, and ticks at 30 Hz for live state.
#include "app/model/AppModel.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"

namespace ddaw::ui {

class View : public juce::Component {
public:
    explicit View(app::AppModel& m) : model(m) {}
    virtual void refresh(app::ModelEvent) {}  // the model changed
    virtual void tick() {}                    // ~30 Hz, UI thread: poll meters / playhead
protected:
    app::AppModel& model;
};

}  // namespace ddaw::ui
