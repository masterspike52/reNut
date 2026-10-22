/**
 * @file    online/sessions.cpp
 * @brief   The sessions N&B hosts, advertised to friends; a friend's, found by ID.
 *
 * The SDK's XSessionCreate makes a handle and leaves XSESSION_INFO as it was.
 * A host's session info is what others join it by, so reNut fills it in:
 * XSESSION_INFO is a session ID (XNKID), the host's XNADDR and a key exchange
 * key (XNKEY), 60 bytes. The session the game hosts with presence (the online
 * menu's, XSessionCreate flags 0x23) is advertised to friends (friends.cpp),
 * and a friend's game joining it searches for it by ID (sub_826EA3D8), which is
 * answered from that friend's presence.
 *
 * XNADDR is the address N&B's peers know each other by; reNut makes it from the
 * online ID (xnaddr.h).
 */
#include <algorithm>
#include <atomic>
#include <cstring>
#include <random>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "renut_engine/game/guest_call.h"
#include "renut_engine/online/eos.h"
#include "renut_engine/online/friends.h"
#include "renut_engine/online/online.h"
#include "renut_engine/online/online_log.h"
#include "renut_engine/online/xnaddr.h"

REX_EXTERN(__imp__sub_82BEEE28);  // XSessionCreate
REX_EXTERN(__imp__rex_XSessionDelete);
REX_EXTERN(__imp__rex_XSessionSearchByID);
REX_EXTERN(__imp__rex_XNetGetTitleXnAddr);
REX_EXTERN(__imp__rex_XSessionLeaveLocal);
REX_EXTERN(__imp__rex_XSessionLeaveRemote);
REX_EXTERN(__imp__rex_XSessionMigrateHost);

using rex::X_RESULT;

namespace {

using namespace renut::online;

// XSESSION_CREATE flags.
constexpr uint32_t kCreateHost = 0x00000001;
constexpr uint32_t kCreateUsesPresence = 0x00000002;
constexpr uint32_t kCreateJoinViaPresenceDisabled = 0x00000200;

// XSESSION_INFO: sessionID, hostAddress, keyExchangeKey.
constexpr uint32_t kInfoAddressOffset = 8;
constexpr uint32_t kInfoKeyOffset = 44;
// XSESSION_SEARCHRESULT_HEADER (count, results) and one XSESSION_SEARCHRESULT
// (XSESSION_INFO, open and filled public and private slots, properties and
// contexts: counts and pointers).
constexpr uint32_t kSearchHeaderSize = 8;
constexpr uint32_t kSearchResultSize = 92;

// XNKID: the top bits of the first byte are the kind of session.
constexpr uint8_t kXnkidOnlinePeer = 0x80;

// XNetGetTitleXnAddr: ETHERNET | STATIC | GATEWAY | DNS | ONLINE.
constexpr uint32_t kXnAddrStatus = 0x02 | 0x04 | 0x20 | 0x40 | 0x80;

uint32_t Load32(const uint8_t *base, uint32_t va) {
  return rex::byte_swap(*reinterpret_cast<const uint32_t *>(base + va));
}

void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  *reinterpret_cast<uint32_t *>(base + va) = rex::byte_swap(value);
}

// Sessions the game created and has not deleted. The SDK gives every session
// the same handle, so they are counted rather than told apart.
std::atomic<int> g_sessions{0};

std::mt19937_64 &Random() {
  static std::mt19937_64 random(std::random_device{}());
  return random;
}

// Player 1's XNADDR (xnaddr.h).
friends::XnAddr OwnXnAddr() {
  return xnaddr::Make(eos::OwnId());
}

// A session call that only XAM's bookkeeping does (nothing reNut tracks):
// succeeded, at once or through the overlapped.
void Succeed(PPCContext &ctx, uint32_t overlapped) {
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    ctx.r3.u64 = X_ERROR_IO_PENDING;
  } else {
    ctx.r3.u64 = X_ERROR_SUCCESS;
  }
}

} // namespace

bool renut::online::InSession() {
  return g_sessions.load() > 0;
}

// DWORD XSessionLeaveLocal(HANDLE session, DWORD count, const DWORD *users,
//                          XOVERLAPPED *overlapped)
// DWORD XSessionLeaveRemote(HANDLE session, DWORD count, const XUID *xuids,
//                           XOVERLAPPED *overlapped)
// The SDK fails both (XGI 0xB0013); N&B leaves sessions as it moves from its
// match setup to the match, and from the match back to the lobby.
REX_HOOK_RAW(rex_XSessionLeaveLocal) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0};
  if (!Enabled()) {
    __imp__rex_XSessionLeaveLocal(ctx, base);
    return;
  }
  Succeed(ctx, ctx.r6.u32);
  TraceCall("XSessionLeaveLocal", args, ctx.r3.u32);
}

REX_HOOK_RAW(rex_XSessionLeaveRemote) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0};
  if (!Enabled()) {
    __imp__rex_XSessionLeaveRemote(ctx, base);
    return;
  }
  // Which players the game takes out (a player dropped shows here).
  const uint32_t count = ctx.r4.u32;
  const uint32_t xuids = ctx.r5.u32;
  for (uint32_t i = 0; xuids && i < count && i < 8; ++i) {
    const uint64_t xuid = uint64_t(Load32(base, xuids + 8 * i)) << 32 | Load32(base, xuids + 8 * i + 4);
    RONL_INFO("Online: the game takes player {:016X} ({}) out of a session (from {})", xuid,
              eos::IdOf(xuid), renut::guest::GuestBacktrace(ctx, base));
  }
  Succeed(ctx, ctx.r6.u32);
  TraceCall("XSessionLeaveRemote", args, ctx.r3.u32);
}

// DWORD XSessionMigrateHost(HANDLE session, DWORD user, XSESSION_INFO *info,
//                           XOVERLAPPED *overlapped)
// The host left: a remaining player takes its sessions over (user: a player
// of ours), and the session info names it as the host; the others (user
// XUSER_INDEX_NONE) are given the new host's info.
REX_HOOK_RAW(rex_XSessionMigrateHost) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0};
  if (!Enabled()) {
    __imp__rex_XSessionMigrateHost(ctx, base);
    return;
  }
  constexpr uint32_t kUserIndexNone = 0xFE;
  const uint32_t user = ctx.r4.u32;
  const uint32_t info = ctx.r5.u32;
  if (info && user != kUserIndexNone) {
    const friends::XnAddr own = OwnXnAddr();
    std::memcpy(base + info + kInfoAddressOffset, own.data(), own.size());
    RONL_INFO("Online: the host left; this player hosts the session now");
  }
  Succeed(ctx, ctx.r6.u32);
  TraceCall("XSessionMigrateHost", args, ctx.r3.u32);
}

// DWORD XSessionCreate(DWORD flags, DWORD user, DWORD public_slots,
//                      DWORD private_slots, ULONGLONG *nonce, XSESSION_INFO *info,
//                      XOVERLAPPED *overlapped, HANDLE *session)
REX_HOOK_RAW(sub_82BEEE28) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                            ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const uint32_t flags = ctx.r3.u32;
  const uint32_t public_slots = ctx.r5.u32;
  const uint32_t nonce = ctx.r7.u32;
  const uint32_t info = ctx.r8.u32;
  __imp__sub_82BEEE28(ctx, base);
  TraceCall("XSessionCreate", args, ctx.r3.u32);
  if (ctx.r3.u32 == X_ERROR_SUCCESS || ctx.r3.u32 == X_ERROR_IO_PENDING) {
    if (g_sessions.fetch_add(1) == 0)
      RONL_INFO("Online: in a multiplayer session: cheats are off until it ends");
  }
  if (!Enabled() || !(flags & kCreateHost) || !info)
    return;

  friends::Session session;
  for (uint8_t &b : session.id)
    b = uint8_t(Random()());
  session.id[0] = uint8_t(kXnkidOnlinePeer | (session.id[0] & 0x0F));
  for (uint8_t &b : session.key)
    b = uint8_t(Random()());
  session.host_address = OwnXnAddr();
  session.public_slots = uint8_t(std::min<uint32_t>(public_slots, 0xFF));
  session.filled_slots = 1;
  session.joinable = !(flags & kCreateJoinViaPresenceDisabled);

  std::memcpy(base + info, session.id.data(), session.id.size());
  std::memcpy(base + info + kInfoAddressOffset, session.host_address.data(),
              session.host_address.size());
  std::memcpy(base + info + kInfoKeyOffset, session.key.data(), session.key.size());
  if (nonce)
    *reinterpret_cast<uint64_t *>(base + nonce) = rex::byte_swap(uint64_t(Random()()));

  // The online menu's session is the one friends join through presence.
  if (flags & kCreateUsesPresence) {
    friends::SetHostedSession(session);
    RONL_INFO("Online: hosting session {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} "
              "({} public slots{})",
              session.id[0], session.id[1], session.id[2], session.id[3], session.id[4],
              session.id[5], session.id[6], session.id[7], public_slots,
              session.joinable ? ", joinable" : "");
  }
}

// DWORD XSessionDelete(HANDLE session, XOVERLAPPED *overlapped)
REX_HOOK_RAW(rex_XSessionDelete) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                            ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  if (Enabled())
    RONL_INFO("Online: the game deletes a session (from {})", renut::guest::GuestBacktrace(ctx, base));
  __imp__rex_XSessionDelete(ctx, base);
  TraceCall("XSessionDelete", args, ctx.r3.u32);
  int sessions = g_sessions.load();
  while (sessions > 0 && !g_sessions.compare_exchange_weak(sessions, sessions - 1)) {
  }
  if (sessions == 1)
    RONL_INFO("Online: the last multiplayer session ended: cheats are allowed again");
  // The SDK gives every session the same handle: any deletion ends hosting
  // until the game hosts again.
  if (Enabled())
    friends::SetHostedSession({});
}

// DWORD XSessionSearchByID(XNKID id, DWORD user, DWORD *buffer_size,
//                          XSESSION_SEARCHRESULT_HEADER *results, XOVERLAPPED *overlapped)
REX_HOOK_RAW(rex_XSessionSearchByID) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                            ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  if (!Enabled()) {
    __imp__rex_XSessionSearchByID(ctx, base);
    return;
  }
  const uint64_t id_value = ctx.r3.u64;
  const uint32_t buffer_size = ctx.r5.u32;
  const uint32_t results = ctx.r6.u32;
  const uint32_t overlapped = ctx.r7.u32;
  constexpr uint32_t kNeeded = kSearchHeaderSize + kSearchResultSize;
  // What the XDK's XSessionSearchByID asks for (0x536), whatever the result.
  constexpr uint32_t kRequired = 1334;

  if (!buffer_size) {
    ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
    TraceCall("XSessionSearchByID", args, ctx.r3.u32);
    return;
  }
  if (!results || Load32(base, buffer_size) < kRequired) {
    Store32(base, buffer_size, kRequired);
    ctx.r3.u64 = X_ERROR_INSUFFICIENT_BUFFER;
    TraceCall("XSessionSearchByID", args, ctx.r3.u32);
    return;
  }

  friends::SessionId id;
  for (int i = 0; i < 8; ++i)
    id[i] = uint8_t(id_value >> (56 - 8 * i));
  const auto session = friends::FindFriendSession(id);

  std::memset(base + results, 0, kNeeded);
  if (session) {
    const uint32_t result = results + kSearchHeaderSize;
    Store32(base, results, 1);
    Store32(base, results + 4, result);
    std::memcpy(base + result, session->id.data(), session->id.size());
    std::memcpy(base + result + kInfoAddressOffset, session->host_address.data(),
                session->host_address.size());
    std::memcpy(base + result + kInfoKeyOffset, session->key.data(), session->key.size());
    const uint32_t filled = std::max<uint32_t>(session->filled_slots, 1);
    const uint32_t open = session->public_slots > filled ? session->public_slots - filled : 0;
    Store32(base, result + 60, open);    // dwOpenPublicSlots
    Store32(base, result + 68, filled);  // dwFilledPublicSlots
  }
  RONL_INFO("Online: session {:016X} {}", id_value, session ? "found at a friend" : "not found");

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    ctx.r3.u64 = X_ERROR_IO_PENDING;
  } else {
    ctx.r3.u64 = X_ERROR_SUCCESS;
  }
  TraceCall("XSessionSearchByID", args, ctx.r3.u32);
}

// DWORD XNetGetTitleXnAddr(XNADDR *address)
REX_HOOK_RAW(rex_XNetGetTitleXnAddr) {
  const uint32_t address = ctx.r3.u32;
  if (!Enabled() || !address) {
    __imp__rex_XNetGetTitleXnAddr(ctx, base);
    return;
  }
  const friends::XnAddr own = OwnXnAddr();
  std::memcpy(base + address, own.data(), own.size());
  ctx.r3.u64 = kXnAddrStatus;
}
