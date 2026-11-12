#pragma once

namespace renut::game::renut_lua {

// Before rexfabric::Start, after the other renut declarations (its Lua builds on theirs): the renut API's Lua-made
// parts, as players, the pad, the HUD's drawing (through rex.draw), challenges, mods and settings.
void Declare();

} // namespace renut::game::renut_lua
