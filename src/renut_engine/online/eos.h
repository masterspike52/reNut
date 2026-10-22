/**
 * @file    online/eos.h
 * @brief   Epic Online Services, which reNut's online play runs on: who player
 *          1 is, and peer-to-peer messages to other players.
 *
 * reNut signs in to EOS with a Device ID (EOS Connect): no Epic account, no
 * launcher. A player is its Product User ID (PUID), 32 hex digits, which is
 * its online ID: friends add each other by it (friends.cpp). EOS reaches any
 * player by PUID, through NAT or its relays.
 *
 * An online XUID is made from a PUID (0x0009 << 48 and a 48-bit hash), so
 * every machine makes the same XUID for a player. The PUID cannot be made back
 * from the XUID: it is learned when a player's ID is seen (a friend, a packet,
 * or an XNADDR, which carries it: xnaddr.h) and remembered here.
 *
 * Every EOS call runs on reNut's EOS thread, which ticks the SDK: sends are
 * queued for it, and received messages and tickers run on it.
 *
 * The EOS product's IDs come from the build (CMake: RENUT_EOS_PRODUCT_ID,
 * RENUT_EOS_SANDBOX_ID, RENUT_EOS_DEPLOYMENT_ID, RENUT_EOS_CLIENT_ID,
 * RENUT_EOS_CLIENT_SECRET, from the Epic Developer Portal).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace renut::online::eos {

// Starts EOS and signs in (once, on first use; signing in takes a moment);
// false when it cannot run at all (no credentials in this build, or the SDK
// failed to start).
bool Start();
// Signed in: OwnId and the messages work.
bool SignedIn();
// What is happening when not signed in (for the Online window): signing in,
// or what went wrong. Empty once signed in.
std::string Status();

// Player 1's online ID (PUID) and XUID; empty / 0 until signed in.
std::string OwnId();
uint64_t OwnXuid();

// Online IDs: 32 hex digits, kept in lower case.
constexpr size_t kIdLength = 32;
using IdBytes = std::array<uint8_t, kIdLength / 2>;
// An online ID from what a player typed or pasted (spaces, dashes and case
// ignored); empty when it is not one.
std::string ParseId(std::string_view text);
bool IdToBytes(std::string_view id, IdBytes &out);
std::string IdFromBytes(const IdBytes &bytes);
// The XUID of an online ID (0 for none).
uint64_t XuidOf(std::string_view id);

// Remembers the online ID of a player, for IdOf.
void Remember(std::string_view id);
// The online ID of a player seen so far, by XUID; empty when not known.
std::string IdOf(uint64_t xuid);

// Peer-to-peer messages. Channel 0: the game's traffic (transport.cpp);
// channel 1: friends (friends.cpp); channel 2: co-op (coop.cpp). At most
// kMaxMessage bytes each. Sent unreliably (as the game's own datagrams) or
// reliably and in order.
constexpr uint8_t kChannelGame = 0;
constexpr uint8_t kChannelFriends = 1;
constexpr uint8_t kChannelCoop = 2;
constexpr size_t kMaxMessage = 1170;  // EOS_P2P_MAX_PACKET_SIZE
// Queued for the EOS thread; false when not signed in or too long.
bool Send(std::string_view id, uint8_t channel, const void *data, size_t size, bool reliable);
// Receives the messages of a channel, on the EOS thread: the sender's online
// ID and the message.
using Receiver = void (*)(const std::string &id, const uint8_t *data, size_t size);
void SetReceiver(uint8_t channel, Receiver receiver);
// Called every couple of seconds on the EOS thread while signed in.
using Ticker = void (*)();
void AddTicker(Ticker ticker);
// Called on the EOS thread when the connection to a player opens (it can be
// reached now: it came online, or answered) and when it closes (it went away,
// or cannot be reached).
using PeerListener = void (*)(const std::string &id, bool connected);
void AddPeerListener(PeerListener listener);

} // namespace renut::online::eos
