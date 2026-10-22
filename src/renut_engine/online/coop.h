/**
 * @file    online/coop.h
 * @brief   Co-op in Showdown Town on N&B's own multiplayer (coop.cpp).
 *
 * A player hosts co-op from the Single Player menu (game/coop_menu.cpp): the
 * game's online play starts as it does from Multiplayer, Online, and friends
 * are told the session is co-op (friends.cpp presence). A friend joins it from
 * their Single Player menu: the game accepts it as an invite to that session.
 * Every match of a co-op session takes place in Showdown Town.
 */
#pragma once

#include <rex/ppc/context.h>

#include <cstdint>
#include <string>

namespace renut::online::coop {

enum class Role { kNone, kHost, kJoin };

Role CurrentRole();
// The friend joined (kJoin).
std::string HostId();

// Hosting co-op; 'town' is the Showdown Town scene script its matches use.
void Host(uint32_t town);
// Joining 'friend_id''s co-op session: false when it hosts none.
bool Join(const std::string &friend_id);
void Leave();

// A match now takes place in Showdown Town (co-op, or renut_coop_town).
bool Active();

// The Showdown Town scene script the game would load now for player 1's
// progress (LoadShowdownTown_82564808's pick; 0 if none). Game thread.
uint32_t TownScene(PPCContext &ctx, uint8_t *base);

// In a co-op session challenges are each player's own: while one of the
// challenge manager's functions runs (challengeManBegin / Tick / SetState /
// Teardown / Reset), the game is not in multiplayer for it (byte_82FAC650
// reads 0), so it takes its single-player ways (player 1 only, no waiting for
// the others, nothing told over the network). Game thread; nests.
class OfflineChallenge {
 public:
  explicit OfflineChallenge(uint8_t *base);
  ~OfflineChallenge();
  OfflineChallenge(const OfflineChallenge &) = delete;
  OfflineChallenge &operator=(const OfflineChallenge &) = delete;

 private:
  uint8_t *base_;
};

// Whether the game is in an online match, OfflineChallenge or not.
bool InMultiplayer(const uint8_t *base);

}  // namespace renut::online::coop
