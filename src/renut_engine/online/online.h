#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace renut::online {

// Player 1 is signed in to Live.
bool Enabled();

// Player 1's gamertag. At most 15 characters (XUSER_NAME_SIZE).
std::string PlayerName();
void SetPlayerName(std::string_view name);

// Player 1's online XUID (0x0009...), 0 when not signed in.
uint64_t PlayerXuid();

// Whether the game has an Xbox Live session (XSessionCreate until XSessionDelete);
// cheats are off then unless the host allows them (hooks.cpp).
bool InSession();

// In a session: whether player 1 hosts it, and else the host's online ID (empty
// when not known).
bool HostingSession();
std::string SessionHostId();

// In a session: whether its host allows cheats.
bool SessionAllowsCheats();
// Player 1 may allow cheats: no session, or player 1 hosts it.
bool CanAllowSessionCheats();
// The session's host changed or the session ended: what an earlier host
// answered no longer counts.
void ForgetSessionCheats();

} // namespace renut::online
