#pragma once

#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <cstdint>
#include <functional>
#include <string>

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

// A row of reNut's own, which the scripts reloading never clears: `list` as renut.menu.add's, `image` a texture asset
// id or 0. `on_select` runs on its A press, with CurrentPress set.
void AddBuiltinRow(const std::string &list, const std::string &text, uint32_t image, std::function<void()> on_select);

// A press on row `index` of the list object `list` from a scene's own A handler: when the row is a mod's (or reNut's),
// its press runs and this returns true, so the scene skips its own.
bool PressModRow(PPCContext &ctx, uint8_t *base, uint32_t list, int32_t index, uint32_t user, uint32_t scene);

// Whether row `index` of the list object `list` is a mod's row; its text then goes in `text` (UTF-16).
bool ModRow(const uint8_t *base, uint32_t list, int32_t index, std::u16string *text);

} // namespace renut::game::menu_api
