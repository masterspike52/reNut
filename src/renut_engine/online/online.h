/**
 * @file    online/online.h
 * @brief   reNut's stand-in for Xbox Live: who player 1 is online.
 *
 * Xbox Live is gone and the SDK reports no one signed in to it, so with
 * renut_online on reNut answers N&B's Live calls itself (the XDK wrappers the
 * game links, hooked in the online folder), on Epic Online Services (eos.h).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace renut::online {

// renut_online, and signed in to EOS: player 1 is signed in to Live.
bool Enabled();

// Player 1's gamertag: renut_gamertag, else "Player". At most 15 characters
// (XUSER_NAME_SIZE).
std::string PlayerName();
// Sets renut_gamertag and saves renut.toml.
void SetPlayerName(std::string_view name);

// Player 1's online XUID (0x0009...), made from its online ID (0 until signed
// in to EOS).
uint64_t PlayerXuid();

// Whether the game is in online multiplayer: it has an Xbox Live session (its
// online menu's lobby, a match's setup or a match), from XSessionCreate until
// XSessionDelete. Cheats are off then (hooks.cpp).
bool InSession();

// renut_online_trace: logs a Live call, its first six arguments and result.
void TraceCall(std::string_view name, const uint32_t (&args)[6], uint32_t result);

} // namespace renut::online
