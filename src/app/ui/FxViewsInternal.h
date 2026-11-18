#pragma once
// The per-family constructors behind FxView::make (each returns nullptr for a type it does not draw).
#include "app/ui/FxView.h"

namespace ddaw::ui {
std::unique_ptr<FxView> makeEqView(const std::string& type, app::AppModel&, project::Uid, const app::DeviceInfo&, int availH);
std::unique_ptr<FxView> makeDynView(const std::string& type, app::AppModel&, project::Uid, const app::DeviceInfo&, int availH);
std::unique_ptr<FxView> makeTimeView(const std::string& type, app::AppModel&, project::Uid, const app::DeviceInfo&, int availH);
std::unique_ptr<FxView> makeModView(const std::string& type, app::AppModel&, project::Uid, const app::DeviceInfo&, int availH);
std::unique_ptr<FxView> makeColourView(const std::string& type, app::AppModel&, project::Uid, const app::DeviceInfo&, int availH);
}  // namespace ddaw::ui
