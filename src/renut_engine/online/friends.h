/**
 * @file    online/friends.h
 * @brief   N&B's friends list: reNut's own, kept by online ID, over EOS.
 *
 * Xbox Live's friends are gone, and N&B players only join each other through
 * the friends list. reNut keeps one of its own: players add each other by
 * online ID (eos.h, copied and pasted), with a friend request the other
 * accepts. Friends are kept in online_friends.txt. Every couple of seconds a
 * player tells each friend it is online, with its gamertag and the session it
 * hosts (ID, key, slots and QoS data); a friend heard from lately is online,
 * and joinable while it hosts. All of it goes peer to peer over EOS (channel
 * 1). The host's XNADDR is made from its online ID (xnaddr.h).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace renut::online::friends {

// N&B's title ID, which the friends menu keeps entries of.
constexpr uint32_t kTitleId = 0x4D5307ED;

// XONLINE_FRIENDSTATE flags.
constexpr uint32_t kStateOnline = 0x00000001;
constexpr uint32_t kStatePlaying = 0x00000002;
constexpr uint32_t kStateJoinable = 0x00000010;

using SessionId = std::array<uint8_t, 8>;   // XNKID
using XnAddr = std::array<uint8_t, 36>;     // XNADDR, as the game sees it
using SessionKey = std::array<uint8_t, 16>; // XNKEY

// A session a player hosts: what XSESSION_INFO holds, its slots, and the data
// its host gives QoS probes (XNetQosListen; N&B's: 8 bytes of slots and state).
struct Session {
  SessionId id{};
  XnAddr host_address{};
  SessionKey key{};
  uint8_t public_slots = 0;
  uint8_t filled_slots = 0;
  bool joinable = false;
  std::array<uint8_t, 32> qos_data{};
  uint8_t qos_size = 0;
  // A friend's: the round trip to it (an estimate; EOS does not measure it).
  uint16_t rtt_ms = 0;
  // A co-op session (coop.h), and the Showdown Town scene script its matches use.
  bool coop = false;
  uint32_t coop_town = 0;

  bool Valid() const;
};

struct Friend {
  std::string id;        // online ID
  uint64_t xuid = 0;
  std::string gamertag;
  bool online = false;
  uint32_t state = 0;    // XONLINE_FRIENDSTATE
  uint32_t title_id = 0;
  Session session;       // the session it hosts
};

// A friend request received, and one sent (its gamertag once known).
struct Request {
  std::string id;
  std::string gamertag;
};

// EOS is signed in (renut_online on): the friends list works.
bool EnsureStarted();

std::vector<Friend> Friends();
std::vector<Request> Requests();
std::vector<Request> SentRequests();

// By online ID (eos::ParseId).
void SendRequest(const std::string &id);
void CancelRequest(const std::string &id);
void Accept(const std::string &id);
void Decline(const std::string &id);
void Remove(const std::string &id);

// The session player 1 hosts (its id zero: none), told to friends.
void SetHostedSession(const Session &session);
Session HostedSession();
// What player 1's game answers QoS probes for a session with (XNetQosListen);
// told with the hosted session. 'size' 0: none.
void SetQosData(const SessionId &id, const uint8_t *data, size_t size);
// Player 1's hosted session is co-op (told with it), its matches in this town
// scene script.
void SetCoopHosting(bool coop, uint32_t town);
// A friend's hosted session with this ID.
std::optional<Session> FindFriendSession(const SessionId &id);

} // namespace renut::online::friends
