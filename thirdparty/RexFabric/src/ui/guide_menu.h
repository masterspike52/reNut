// The mods menu in RexLiveless's Guide: a blade of its own, and the same menu as a window for the menu key.
// Built only when the project attaches RexFabric WITH_GUIDE.

#pragma once

#include <string_view>

#include "menu_model.h"

namespace rexfabric::ui::guide {

void AddMenus(MenuModel& model);
void Toggle();
void Show(bool show);
bool Shown();

}  // namespace rexfabric::ui::guide
