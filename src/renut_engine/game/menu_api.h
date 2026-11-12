#pragma once

#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <cstdint>

namespace renut::game::menu_api {

// Before rexfabric::Start: declares renut.menu, rows Lua mods add to the game's XUI list menus (the main
// menu's single player list, the pause menu's, any other).
void Declare();

// The A press on a row of a mod's: its context, and the scene object it went to.
struct Press {
  PPCContext *ctx;
  uint8_t *base;
  uint32_t scene;
};

// While a row's on_select runs (renut.menu_select), the press it came from; nullptr otherwise.
const Press *CurrentPress();

} // namespace renut::game::menu_api
