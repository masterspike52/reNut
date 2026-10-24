/**
 * @file    online/identity.cpp
 * @brief   Player 1 signed in to Live, with the privileges online play needs.
 *
 * The SDK's XAM has player 1 signed in locally only, denies every privilege
 * and reports no network link. N&B's profile refresh (sub_821D9068) treats a
 * player as online when XUserGetSigninInfo says SignedInToLive and the link
 * is up (XNetGetEthernetLinkStatus, polled into byte_82E1AA99 by
 * sub_8224C4B8), and then asks for the multiplayer, user-created-content,
 * profile-viewing and communications privileges. With renut_online on and
 * signed in to EOS (eos.h), these are answered here for player 1, through the
 * XDK wrappers the game links. Player 1's online XUID comes from its online
 * ID, and its gamertag is renut_gamertag when set.
 */
#include "renut_engine/app/config_file.h"
#include "renut_engine/online/online.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "renut_engine/app/app_paths.h"
#include "renut_engine/online/online_log.h"
#include "renut_engine/online/eos.h"

REXCVAR_DEFINE_BOOL(renut_online, false, "Nuts&Bolts/Online",
                    "Player 1 is signed in to Xbox Live, so the game's online modes open. Online "
                    "play runs on Epic Online Services (no Epic account needed).");
REXCVAR_DEFINE_STRING(renut_gamertag, "", "Nuts&Bolts/Online",
                      "Player 1's name (gamertag), at most 15 characters; empty: the profile's. "
                      "Also set in the Online window (F5).");

REX_EXTERN(__imp__rex_XUserGetSigninInfo);
REX_EXTERN(__imp__rex_j_XamUserGetName);
REX_EXTERN(__imp__rex_j_XamUserGetSigninState);
REX_EXTERN(__imp__rex_XUserCheckPrivilege);
REX_EXTERN(__imp__rex_XUserGetXUID);
REX_EXTERN(__imp__rex_XNetGetEthernetLinkStatus);

namespace renut::online {

namespace {

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kSigninStateSignedInToLive = 2;
// XUSER_SIGNIN_INFO: xuid, dwInfoFlags, UserSigninState, ...
constexpr uint32_t kSigninInfoFlagsOffset = 8;
constexpr uint32_t kSigninInfoStateOffset = 12;
constexpr uint32_t kSigninInfoNameOffset = 24;
constexpr size_t kNameSize = 16;  // XUSER_NAME_SIZE, the terminator included
constexpr uint32_t kInfoFlagLiveEnabled = 1;      // XUSER_INFO_FLAG_LIVE_ENABLED
constexpr uint32_t kGetSigninInfoOfflineXuid = 2; // XUSER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY
// XNET_ETHERNET_LINK_ACTIVE | _100MBPS | _FULL_DUPLEX
constexpr uint32_t kEthernetLinkUp = 0x01 | 0x02 | 0x08;

void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  *reinterpret_cast<uint32_t *>(base + va) = rex::byte_swap(value);
}

uint32_t Load32(const uint8_t *base, uint32_t va) {
  return rex::byte_swap(*reinterpret_cast<const uint32_t *>(base + va));
}

void Store64(uint8_t *base, uint32_t va, uint64_t value) {
  *reinterpret_cast<uint64_t *>(base + va) = rex::byte_swap(value);
}

// Printable ASCII, at most 15 characters (what a gamertag can hold).
std::string CleanName(std::string_view name) {
  std::string out;
  for (char c : name) {
    if (out.size() + 1 >= kNameSize)
      break;
    if (c >= 0x20 && c < 0x7F)
      out += c;
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

// The name the game is given: renut_gamertag; empty: the profile's stays.
std::string NameOverride() {
  return CleanName(REXCVAR_GET(renut_gamertag));
}

void StoreName(uint8_t *base, uint32_t va, uint32_t capacity, const std::string &name) {
  std::memset(base + va, 0, capacity);
  std::memcpy(base + va, name.data(), std::min<size_t>(name.size(), capacity - 1));
}

// N&B's players: a std::map (checked iterators, its proxy at 0x82E293DC) whose
// head node is at 0x82E293F4 (nodes: left +0, parent +4, right +8, the player
// at +16, is-head byte +21), each player with its controller (user index) at
// +128 and its gamertag, a std::string, at +12 (text +16, inline below 16
// characters; length +32; capacity +36). playerSlotForPad (0x821DD268) looks
// one up.
constexpr uint32_t kPlayersMapHead = 0x82E293F4;
constexpr uint32_t kNodeLeftOffset = 0;
constexpr uint32_t kNodeParentOffset = 4;
constexpr uint32_t kNodeRightOffset = 8;
constexpr uint32_t kNodePlayerOffset = 16;
constexpr uint32_t kNodeIsHeadOffset = 21;
constexpr uint32_t kPlayerUserOffset = 128;
constexpr uint32_t kPlayerNameOffset = 16;
constexpr uint32_t kPlayerNameLengthOffset = 32;
constexpr uint32_t kPlayerNameCapacityOffset = 36;
constexpr uint32_t kPlayerRenameOffset = 224;

uint32_t PlayerOfUser(const uint8_t *base, uint32_t user) {
  const uint32_t head = Load32(base, kPlayersMapHead);
  if (!head)
    return 0;
  uint32_t stack[32];
  uint32_t count = 0;
  stack[count++] = Load32(base, head + kNodeParentOffset);
  for (uint32_t visited = 0; count && visited < 64; ++visited) {
    const uint32_t node = stack[--count];
    if (!node || base[node + kNodeIsHeadOffset])
      continue;
    const uint32_t player = Load32(base, node + kNodePlayerOffset);
    if (player && Load32(base, player + kPlayerUserOffset) == user)
      return player;
    if (count + 2 <= std::size(stack)) {
      stack[count++] = Load32(base, node + kNodeLeftOffset);
      stack[count++] = Load32(base, node + kNodeRightOffset);
    }
  }
  return 0;
}

// N&B's profile refresh (sub_821D9068) takes a player whose gamertag changed
// for another profile signed in: it drops that player and makes a new one,
// which has no controller until START is pressed again. A player marked for
// renaming (+224) takes the new gamertag instead, which is what renaming in
// the Online window is.
void RenameGamePlayer(uint8_t *base, uint32_t user, const std::string &name) {
  const uint32_t player = PlayerOfUser(base, user);
  if (!player || base[player + kPlayerRenameOffset])
    return;
  const uint32_t capacity = Load32(base, player + kPlayerNameCapacityOffset);
  const uint32_t text = capacity < 16 ? player + kPlayerNameOffset
                                      : Load32(base, player + kPlayerNameOffset);
  const uint32_t length = Load32(base, player + kPlayerNameLengthOffset);
  if (!text || length >= 64)
    return;
  const std::string_view stored(reinterpret_cast<const char *>(base + text), length);
  if (stored != name) {
    base[player + kPlayerRenameOffset] = 1;
    RONL_INFO("Online: player {}'s gamertag '{}' becomes '{}', renamed in place (the player and "
              "its controller are kept)",
              user + 1, stored, name);
  }
}

} // namespace

// Signing in to EOS takes a moment: until it is done player 1 stays signed in
// locally only, and the profile refresh takes it to Live once it is.
bool Enabled() {
  return REXCVAR_GET(renut_online) && eos::Start() && eos::SignedIn();
}

std::string PlayerName() {
  const std::string name = NameOverride();
  return name.empty() ? "Player" : name;
}

void SetPlayerName(std::string_view name) {
  rex::cvar::SetFlagByName("renut_gamertag", CleanName(name));
  renut::config::Save(renut::paths::WritableFolder() / "renut.toml");
}

uint64_t PlayerXuid() {
  return eos::OwnXuid();
}

} // namespace renut::online

using namespace renut::online;

// DWORD XUserGetSigninInfo(DWORD user, DWORD flags, XUSER_SIGNIN_INFO *info)
REX_HOOK_RAW(rex_XUserGetSigninInfo) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t info = ctx.r5.u32;
  __imp__rex_XUserGetSigninInfo(ctx, base);
  if (user != 0 || !info || ctx.r3.u32 != kErrorSuccess)
    return;
  if (const std::string name = NameOverride(); !name.empty())
    StoreName(base, info + kSigninInfoNameOffset, kNameSize, name);
  if (Enabled()) {
    Store32(base, info + kSigninInfoFlagsOffset,
            Load32(base, info + kSigninInfoFlagsOffset) | kInfoFlagLiveEnabled);
    Store32(base, info + kSigninInfoStateOffset, kSigninStateSignedInToLive);
    if (!(flags & kGetSigninInfoOfflineXuid))
      Store64(base, info, PlayerXuid());
  }
  const char *returned = reinterpret_cast<const char *>(base + info + kSigninInfoNameOffset);
  RenameGamePlayer(base, user, std::string(returned, strnlen(returned, kNameSize)));
}

// DWORD XamUserGetName(DWORD user, char *buffer, DWORD length)
REX_HOOK_RAW(rex_j_XamUserGetName) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t buffer = ctx.r4.u32;
  const uint32_t length = ctx.r5.u32;
  const std::string name = NameOverride();
  if (user == 0 && buffer && length && !name.empty()) {
    StoreName(base, buffer, length, name);
    ctx.r3.u64 = kErrorSuccess;
    return;
  }
  __imp__rex_j_XamUserGetName(ctx, base);
}

// XUSER_SIGNIN_STATE XamUserGetSigninState(DWORD user)
REX_HOOK_RAW(rex_j_XamUserGetSigninState) {
  if (Enabled() && ctx.r3.u32 == 0) {
    ctx.r3.u64 = kSigninStateSignedInToLive;
    return;
  }
  __imp__rex_j_XamUserGetSigninState(ctx, base);
}

// DWORD XUserCheckPrivilege(DWORD user, XPRIVILEGE_TYPE privilege, BOOL *result)
REX_HOOK_RAW(rex_XUserCheckPrivilege) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t result = ctx.r5.u32;
  if (Enabled() && (user == 0 || user == 0xFF) && result) {
    Store32(base, result, 1);
    ctx.r3.u64 = kErrorSuccess;
    return;
  }
  __imp__rex_XUserCheckPrivilege(ctx, base);
}

// DWORD XUserGetXUID(DWORD user, XUID *xuid)
REX_HOOK_RAW(rex_XUserGetXUID) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t xuid = ctx.r4.u32;
  __imp__rex_XUserGetXUID(ctx, base);
  if (Enabled() && user == 0 && xuid && ctx.r3.u32 == kErrorSuccess)
    Store64(base, xuid, PlayerXuid());
}

// DWORD XNetGetEthernetLinkStatus()
REX_HOOK_RAW(rex_XNetGetEthernetLinkStatus) {
  if (Enabled()) {
    ctx.r3.u64 = kEthernetLinkUp;
    return;
  }
  __imp__rex_XNetGetEthernetLinkStatus(ctx, base);
}
