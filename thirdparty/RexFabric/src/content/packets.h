// Packets (docs/content_and_classes.md, Packets): what the game declared of them (rexfabric/content.h, DeclarePackets),
// its own packets, and the enabled mods' packet assets as they were loaded, for the game to list with Packets.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rexfabric/content.h>

#include "../mods.h"

namespace rexfabric::content {

// Whether the game declared packets: without it there's no New Packet.
bool HasPackets();
// The game's enum a packet's base is one of, by its Lua name; empty before DeclarePackets.
std::string PacketBaseEnum();
// The struct a packet of `base` takes its look from, empty for a base the game gave no look.
std::string PacketLookStruct(std::string_view base);
// The paths of the game's own packets, in their order.
std::vector<std::string> GamePacketPaths();

// Reads the packets of the enabled mods. With the classes, after the types.
void LoadPackets(const std::vector<ModInfo>& mods);

}  // namespace rexfabric::content
