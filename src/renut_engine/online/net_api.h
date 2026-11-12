#pragma once

#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <cstdint>
#include <string>

namespace renut::online::net_api {

// Before rexfabric::Start: declares renut.net, messages between the players of a multiplayer session and
// the co-op world the game's match loads.
void Declare();

// Game thread, once a frame (the frame hook): sends and takes renut.net's messages, fires its events.
void Tick(PPCContext &ctx, uint8_t *base);

// Game thread, gameSaveSave(profile): true when a co-op session takes the save (the host's co-op save is
// written; a guest's nothing), so the player's own save isn't.
bool TakeGameSave(PPCContext &ctx, uint8_t *base, uint32_t profile);

// Co-op for coop_challenge: a co-op world is played; its scene's aid (0 for none); this machine hosts.
bool CoopActive(const uint8_t *base);
uint32_t CoopWorld(const uint8_t *base);
bool Host();

// A message from C++ (to a peer, or 0 for everyone), sent at the end of the frame.
void Post(uint32_t to, const std::string &name, const std::string &data);

// A net object's id (+ 12, 8 bytes, the same on every machine), or 0 when it has none.
uint64_t NetObjectId(const uint8_t *base, uint32_t net);

} // namespace renut::online::net_api
