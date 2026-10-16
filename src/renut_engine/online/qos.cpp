/**
 * @file    online/qos.cpp
 * @brief   QoS probes of friends' sessions, answered from their presence.
 *
 * Before N&B lets a player join a friend's session it probes the host
 * (sub_826CEF20: XNetQosLookup of the session's XNADDR, XNKID and XNKEY) and
 * reads the result (sub_826CEFF8): it needs the probe complete and the target
 * contacted, and the 8 bytes of data the host listens with (its slots and
 * state); without them the friend cannot be joined. The host gives that data
 * with XNetQosListen (sub_826CF718: enable, then set data). Both are reNut's
 * here: a host's data goes to its friends with the session in its presence
 * (friends.cpp), and a probe is answered at once from the friend's presence,
 * its round trip the one presence measured.
 *
 * XNQOS: u32 count, u32 pending, then per target an XNQOSINFO (24 bytes): u8
 * flags, u8 0, u16 probes sent, u16 probes received, u16 data size, u32 data
 * pointer, u16 minimum and median round trip (ms), u32 up and down bits/s.
 * reNut allocates it in guest memory and frees it at XNetQosRelease.
 */
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "renut_engine/online/friends.h"
#include "renut_engine/online/online.h"
#include "renut_engine/online/online_log.h"

REX_EXTERN(__imp__rex_XNetQosListen);
REX_EXTERN(__imp__rex_XNetQosLookup);
REX_EXTERN(__imp__rex_XNetQosRelease);

namespace {

using namespace renut::online;

// XNetQosListen flags.
constexpr uint32_t kListenDisable = 0x02;
constexpr uint32_t kListenSetData = 0x04;
constexpr uint32_t kListenRelease = 0x10;

// XNQOSINFO flags.
constexpr uint8_t kQosComplete = 0x01;
constexpr uint8_t kQosTargetContacted = 0x02;
constexpr uint8_t kQosDataReceived = 0x08;

constexpr uint32_t kQosHeaderSize = 8;
constexpr uint32_t kQosInfoSize = 24;
constexpr uint32_t kQosDataSize = 32;  // room for each target's data after the infos
constexpr uint32_t kBitsPerSecond = 10'000'000;

// XNetQosLookup's 9th to 12th arguments, in the caller's frame.
constexpr uint32_t kStackEvent = 0x64;
constexpr uint32_t kStackResult = 0x6C;

std::mutex g_mutex;
std::unordered_set<uint32_t> g_results;  // XNQOS reNut allocated

uint32_t Load32(const uint8_t *base, uint32_t va) {
  return rex::byte_swap(*reinterpret_cast<const uint32_t *>(base + va));
}
void Store32(uint8_t *base, uint32_t va, uint32_t v) {
  *reinterpret_cast<uint32_t *>(base + va) = rex::byte_swap(v);
}
void Store16(uint8_t *base, uint32_t va, uint16_t v) {
  *reinterpret_cast<uint16_t *>(base + va) = rex::byte_swap(v);
}

friends::SessionId ReadKid(const uint8_t *base, uint32_t va) {
  friends::SessionId id{};
  if (va)
    std::memcpy(id.data(), base + va, id.size());
  return id;
}

} // namespace

// INT XNetQosListen(const XNKID *id, const BYTE *data, UINT size, DWORD bits_per_second,
//                   DWORD flags)
REX_HOOK_RAW(rex_XNetQosListen) {
  if (!Enabled()) {
    __imp__rex_XNetQosListen(ctx, base);
    return;
  }
  const friends::SessionId id = ReadKid(base, ctx.r3.u32);
  const uint32_t data = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  const uint32_t flags = ctx.r7.u32;
  if (flags & (kListenDisable | kListenRelease))
    friends::SetQosData(id, nullptr, 0);
  else if ((flags & kListenSetData) && data && size)
    friends::SetQosData(id, base + data, size);
  ctx.r3.u64 = 0;
}

// INT XNetQosLookup(UINT hosts, const XNADDR *addresses[], const XNKID *ids[],
//                   const XNKEY *keys[], UINT servers, const IN_ADDR server_addresses[],
//                   const DWORD service_ids[], UINT probes, DWORD bits_per_second,
//                   DWORD flags, WSAEVENT event, XNQOS **result)
REX_HOOK_RAW(rex_XNetQosLookup) {
  const uint32_t stack = ctx.r1.u32;
  const uint32_t result_out = Load32(base, stack + kStackResult);
  if (!Enabled() || !result_out) {
    __imp__rex_XNetQosLookup(ctx, base);
    return;
  }
  const uint32_t hosts = ctx.r3.u32;
  const uint32_t ids = ctx.r5.u32;
  const uint32_t servers = ctx.r7.u32;
  const uint16_t probes = uint16_t(std::clamp<uint32_t>(ctx.r10.u32, 1, 0xFFFF));
  const uint32_t event = Load32(base, stack + kStackEvent);
  const uint32_t count = hosts + servers;

  const uint32_t size = kQosHeaderSize + count * (kQosInfoSize + kQosDataSize);
  const uint32_t xnqos = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (!xnqos) {
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  std::memset(base + xnqos, 0, size);
  Store32(base, xnqos, count);
  Store32(base, xnqos + 4, 0);  // none pending: answered at once
  const uint32_t data_area = xnqos + kQosHeaderSize + count * kQosInfoSize;

  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t info = xnqos + kQosHeaderSize + i * kQosInfoSize;
    uint8_t flags = kQosComplete;
    // Title servers (Live's) are gone; hosts are friends' sessions.
    const auto session =
        i < hosts && ids ? friends::FindFriendSession(ReadKid(base, Load32(base, ids + 4 * i)))
                         : std::nullopt;
    if (session) {
      flags |= kQosTargetContacted;
      Store16(base, info + 2, probes);
      Store16(base, info + 4, probes);
      Store16(base, info + 12, session->rtt_ms);
      Store16(base, info + 14, session->rtt_ms);
      Store32(base, info + 16, kBitsPerSecond);
      Store32(base, info + 20, kBitsPerSecond);
      if (session->qos_size) {
        flags |= kQosDataReceived;
        const uint32_t data = data_area + i * kQosDataSize;
        std::memcpy(base + data, session->qos_data.data(), session->qos_size);
        Store16(base, info + 6, session->qos_size);
        Store32(base, info + 8, data);
      }
    }
    base[info] = flags;
    if (i < hosts)
      RONL_INFO("Online: QoS probe of host {}: {}", i,
                !session ? "no friend hosts that session"
                         : session->qos_size ? "answered" : "answered, no data from the host yet");
  }

  {
    std::lock_guard lock(g_mutex);
    g_results.insert(xnqos);
  }
  Store32(base, result_out, xnqos);
  if (event) {
    if (auto e = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event))
      e->Set(0, false);
  }
  ctx.r3.u64 = 0;
}

// INT XNetQosRelease(XNQOS *qos)
REX_HOOK_RAW(rex_XNetQosRelease) {
  const uint32_t xnqos = ctx.r3.u32;
  bool ours;
  {
    std::lock_guard lock(g_mutex);
    ours = g_results.erase(xnqos) != 0;
  }
  if (!ours) {
    __imp__rex_XNetQosRelease(ctx, base);
    return;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(xnqos);
  ctx.r3.u64 = 0;
}
