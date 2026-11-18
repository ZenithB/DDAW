#pragma once
// A new session starts with two send buses (Send A and Send B); most model and view tests want a blank project instead.
#include "app/model/AppModel.h"
#include "app/model/Edit.h"

inline void startEmpty(ddaw::app::AppModel& m) {
    std::vector<ddaw::project::Uid> ids;
    for (auto& t : m.project().tracks) ids.push_back(t.uid);
    for (auto id : ids) m.apply(ddaw::app::edit::removeTrack(id));
    m.document().clearHistory();
}
