// Listeners for named events: the game's own, and RexFabric's "frame". Like hook chains, a cell's listener
// list is immutable once published, so firing never takes a lock and never sees a list change underfoot.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rexfabric/api.h>
#include <rexfabric/game_api.h>

namespace rexfabric::events {

using Listener = std::function<void(std::span<const Value> args)>;

// Zero when `fn` is empty.
uint64_t Listen(std::string_view name, const RexFabricMod* owner, Listener fn);
bool Unlisten(uint64_t id);
void UnlistenAll(const RexFabricMod* owner);

// Every event name the game declared or a mod listened for, sorted.
std::vector<std::string> Names();

}  // namespace rexfabric::events
