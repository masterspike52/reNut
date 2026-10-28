#pragma once
// Xbox LIVE sessions and invites (live_sessions.cpp).

#include <cstdint>
#include <string>
#include <vector>

namespace rexglue_guide::sessions {

// Hex session id of the LIVE session this title hosts, or empty.
std::string HostedSessionId();

// Fetches the invited session and tells the title an invite was accepted
// (XN_LIVE_INVITE_ACCEPTED); it then reads it with XInviteGetAcceptedInfo.
bool AcceptInvite(const std::string& session_id, uint64_t inviter, std::string* error);

// XInviteGetAcceptedInfo (XLiveBase 0x58023) with its marshaled arguments.
bool FillAcceptedInvite(uint32_t args_ptr);

// QoS data a host answers probes for its session with (XNetQosListen), keyed
// by XNKID. Joiners read it with XNetQosLookup; some titles need it to join
// from an invite. Publish runs in the background; Fetch blocks briefly.
void PublishQos(const uint8_t kid[8], std::vector<uint8_t> data);
bool FetchQos(const uint8_t kid[8], std::vector<uint8_t>* data);

// The session a friend (by XUID) hosts in this title that friends may join
// through presence, for the friends list's join state: its XNKID, or false.
// Answers from a list refreshed in the background, so it never waits.
bool FriendJoinableSession(uint64_t xuid, uint8_t kid[8]);
// Refreshes that list in the background when it is stale, so it is ready
// before a title first lists friends.
void RefreshFriendSessions();

// The gamertag of a player hosting a session of this title, from the same
// list; empty when not known.
std::string HostGamertag(uint64_t xuid);

}  // namespace rexglue_guide::sessions
