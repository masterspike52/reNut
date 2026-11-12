#pragma once

namespace renut::game::mod_data {

// Before rexfabric::Start: declares renut.data, what each Lua mod keeps between sessions (best times, unlocks,
// settings), in reNut's writable folder rather than the mod's own.
void Declare();

} // namespace renut::game::mod_data
