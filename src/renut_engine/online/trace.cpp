/**
 * @file    online/trace.cpp
 * @brief   Logs N&B's Live calls (renut_online_trace), to map its online flows.
 *
 * Each hook calls the original and logs its first six arguments and its
 * result. A call made every frame is logged its first few times, then every
 * kLogEvery-th time. Wrappers without a name in the generated code go by
 * address (IDA names in the comments).
 */
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "renut_engine/online/online.h"
#include "renut_engine/online/online_log.h"

REXCVAR_DEFINE_BOOL(renut_online_trace, false, "Nuts&Bolts/Online",
                    "Log the game's Xbox Live calls (sessions, XNet, QoS, friends, invites)");

namespace {

constexpr uint32_t kLogFirst = 16;
constexpr uint32_t kLogEvery = 512;

std::mutex g_mutex;
std::unordered_map<std::string_view, uint32_t> g_calls;

} // namespace

void renut::online::TraceCall(std::string_view name, const uint32_t (&args)[6], uint32_t result) {
  if (!REXCVAR_GET(renut_online_trace))
    return;
  uint32_t call;
  {
    std::lock_guard lock(g_mutex);
    call = ++g_calls[name];
  }
  if (call > kLogFirst && call % kLogEvery)
    return;
  RONL_INFO("{}({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}) = {:08X} [call {}]", name, args[0],
            args[1], args[2], args[3], args[4], args[5], result, call);
}

using renut::online::TraceCall;

#define RENUT_TRACE_LIVE(fn, name)                                                                 \
  REX_EXTERN(__imp__##fn);                                                                         \
  REX_HOOK_RAW(fn) {                                                                               \
    const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,                                  \
                              ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};                                 \
    __imp__##fn(ctx, base);                                                                        \
    TraceCall(name, args, ctx.r3.u32);                                                             \
  }

// Sessions (XSessionCreate, XSessionDelete, XSessionSearchByID,
// XSessionLeaveLocal, XSessionLeaveRemote, XSessionMigrateHost: sessions.cpp).
RENUT_TRACE_LIVE(rex_XSessionModify, "XSessionModify")
RENUT_TRACE_LIVE(rex_XSessionJoinLocal, "XSessionJoinLocal")
RENUT_TRACE_LIVE(rex_XSessionJoinRemote, "XSessionJoinRemote")
RENUT_TRACE_LIVE(rex_XSessionArbitrationRegister, "XSessionArbitrationRegister")
RENUT_TRACE_LIVE(rex_XSessionStart, "XSessionStart")
RENUT_TRACE_LIVE(rex_XSessionEnd, "XSessionEnd")
RENUT_TRACE_LIVE(rex_XSessionSearch, "XSessionSearch")
RENUT_TRACE_LIVE(rex_XSessionSearchEx, "XSessionSearchEx")
RENUT_TRACE_LIVE(rex_XSessionWriteStats, "XSessionWriteStats")

// Secure addressing (XnAddrToInAddr, InAddrToXnAddr, Connect, GetConnectStatus,
// UnregisterInAddr: transport.cpp) and QoS.
RENUT_TRACE_LIVE(rex_XNetServerToInAddr, "XNetServerToInAddr")
// (XNetQosListen, XNetQosLookup, XNetQosRelease: qos.cpp.)
RENUT_TRACE_LIVE(rex_XNetQosServiceLookup, "XNetQosServiceLookup")

// Invites, notifications, enumeration (friends: friends.cpp).
RENUT_TRACE_LIVE(rex_XEnumerate, "XEnumerate")
RENUT_TRACE_LIVE(rex_XInviteGetAcceptedInfo, "XInviteGetAcceptedInfo")
RENUT_TRACE_LIVE(rex_XInviteSend, "XInviteSend")
RENUT_TRACE_LIVE(rex_XNotifyCreateListener, "XNotifyCreateListener")
RENUT_TRACE_LIVE(rex_XOnlineStartupEx, "XOnlineStartupEx")

// Sockets (socket, bind, closesocket, sendto, recvfrom, WSASendTo,
// WSARecvFrom: transport.cpp).
RENUT_TRACE_LIVE(rex_connect, "connect")
RENUT_TRACE_LIVE(rex_ioctlsocket, "ioctlsocket")
RENUT_TRACE_LIVE(rex_setsockopt, "setsockopt")
RENUT_TRACE_LIVE(rex_getsockname, "getsockname")
RENUT_TRACE_LIVE(rex_select, "select")
RENUT_TRACE_LIVE(rex_WSAEventSelect, "WSAEventSelect")

// Stats.
RENUT_TRACE_LIVE(rex_XUserReadStats, "XUserReadStats")
RENUT_TRACE_LIVE(rex_XUserCreateStatsEnumeratorByRank, "XUserCreateStatsEnumeratorByRank")
