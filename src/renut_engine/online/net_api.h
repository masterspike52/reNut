#pragma once

#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <cstdint>

namespace renut::online::net_api {

// Before rexfabric::Start: declares renut.net, messages between the players of a multiplayer session.
void Declare();

// Game thread, once a frame (the frame hook): sends and takes renut.net's messages, fires its events.
void Tick(PPCContext &ctx, uint8_t *base);

} // namespace renut::online::net_api
