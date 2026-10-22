/**
 * @file    online/transport.cpp
 * @brief   The game's peer traffic, carried over EOS.
 *
 * N&B talks to its peers on two sockets of its own (a VDP socket bound to port
 * 1000, and a UDP one), polling them with WSARecvFrom every frame and sending
 * with WSASendTo and sendto, to the IN_ADDR XNetXnAddrToInAddr gave for a
 * peer's XNADDR and a session's key. The SDK's sockets are real host sockets
 * and its XNet does not map addresses, so here:
 * - an IN_ADDR stands for a player and a session, as Xbox Live's secure
 *   connections did: N&B holds two at once to the same player (the lobby's
 *   and the game's), each with traffic of its own. XNetXnAddrToInAddr hands
 *   one out per (XUID from the XNADDR, XNKID) in 10.0.0.0/8, and
 *   XNetInAddrToXnAddr gives the pair back. The online ID the XNADDR carries
 *   is remembered (eos::Remember): a player may first be known from an
 *   XNADDR the host passed on;
 * - the game's sockets stay the SDK's, but bound to a free port of the host's
 *   (several reNut on one machine would all want port 1000); the port the game
 *   asked for is remembered;
 * - what the game sends to such an IN_ADDR goes to that player as an EOS
 *   peer-to-peer message (eos.h, channel 0, to the player's online ID): u8
 *   type (game), u8 protocol, u16 source port, u16 destination port, u8
 *   session XNKID[8], then the payload. Longer than an EOS message may be, it
 *   goes in fragments: u8 type (fragment), u8 message number, u8 index, u8
 *   count, then its part;
 * - a game message that arrives is queued for the game socket bound to its
 *   destination port, from the IN_ADDR of its sender (whom EOS names) and
 *   session (handed out then if it is new: a player joining us), and
 *   WSARecvFrom / recvfrom return it before asking the SDK's socket, which has
 *   nothing.
 * Keepalives (u8 type) go every couple of seconds from the EOS thread to
 * every player the game is connected to, whatever the game is doing: a player
 * heard from before and silent for a while is lost, as Xbox Live's were.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "renut_engine/online/eos.h"
#include "renut_engine/online/friends.h"
#include "renut_engine/online/online.h"
#include "renut_engine/online/online_log.h"
#include "renut_engine/online/xnaddr.h"

REX_EXTERN(__imp__sub_829A8048);  // XNetXnAddrToInAddr
REX_EXTERN(__imp__sub_829A8078);  // XNetInAddrToXnAddr
REX_EXTERN(__imp__sub_829A80A8);  // XNetUnregisterInAddr
REX_EXTERN(__imp__sub_829A80B8);  // XNetConnect
REX_EXTERN(__imp__sub_829A80C8);  // XNetGetConnectStatus
REX_EXTERN(__imp__rex_socket);
REX_EXTERN(__imp__rex_bind);
REX_EXTERN(__imp__rex_closesocket);
REX_EXTERN(__imp__rex_sendto);
REX_EXTERN(__imp__rex_recvfrom);
REX_EXTERN(__imp__sub_829A87B0);  // WSASendTo
REX_EXTERN(__imp__sub_821FB950);  // WSARecvFrom
REX_EXTERN(__imp__rex_XUserMuteListQuery);

REXCVAR_DECLARE(bool, renut_online);

namespace renut::online::transport {

namespace {

using SessionId = friends::SessionId;

constexpr uint8_t kTypeGame = 1;
constexpr uint8_t kTypeKeepAlive = 2;
constexpr uint8_t kTypeFragment = 3;
constexpr size_t kGameHeaderSize = 1 + 1 + 2 + 2 + 8;
constexpr size_t kFragmentHeaderSize = 4;
constexpr size_t kFragmentData = eos::kMaxMessage - kFragmentHeaderSize;
// Messages being put back together: few at a time (a fragment lost leaves one
// behind until it is pushed out).
constexpr size_t kMaxReassembling = 16;
constexpr size_t kMaxPayload = 1400;
constexpr size_t kMaxQueued = 512;
constexpr uint32_t kAfInet = 2;
constexpr uint32_t kSocketError = 0xFFFFFFFF;
// XNET_CONNECT_STATUS.
constexpr uint32_t kConnectPending = 1;
constexpr uint32_t kConnectConnected = 2;
constexpr uint32_t kConnectLost = 3;
// A player heard from before and silent this long is lost, as Xbox Live's
// secure connections were: the game ends its connection then, as it expects.
// Keepalives come from the online thread every couple of seconds whatever the
// game does (a host loading a level sends nothing of its own for a while).
constexpr auto kLostAfter = std::chrono::seconds(20);

struct Datagram {
  uint32_t from_ina = 0;  // host order
  uint16_t from_port = 0;
  std::vector<uint8_t> data;
};

// A game socket: the port it asked for and what arrived for it.
struct GameSocket {
  uint32_t protocol = 0;
  uint16_t port = 0;
  std::deque<Datagram> queue;
};

// A connection: a player in a session, by the IN_ADDR handed out for it.
struct Connection {
  uint64_t xuid = 0;
  SessionId session{};
};

std::mutex g_mutex;
std::unordered_map<uint32_t, GameSocket> g_sockets;           // by the game's handle
std::unordered_map<uint32_t, Connection> g_connections;       // by IN_ADDR (host order)
std::map<std::pair<uint64_t, SessionId>, uint32_t> g_inaddrs; // and back
std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> g_heard_at;
// When the game last asked for a connection to a player (its first IN_ADDR in
// a session): a player not yet heard from is kept alive from then.
std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> g_connected_at;
uint32_t g_next_inaddr = (uint32_t(xnaddr::kInAddrNet) << 24) | 1;
uint32_t g_logged_sends = 0;
uint32_t g_logged_receives = 0;
// Game packets to and from each player since the last traffic report, and the
// connections the game has been told are lost (logged once each).
struct Traffic {
  uint32_t sent = 0;
  uint32_t received = 0;
};
std::map<uint64_t, Traffic> g_traffic;
std::vector<uint32_t> g_reported_lost;
uint32_t g_report_ticks = 0;
constexpr uint32_t kReportTicks = 3;  // every ~6 s while in a session

uint16_t Load16(const uint8_t *base, uint32_t va) {
  return rex::byte_swap(*reinterpret_cast<const uint16_t *>(base + va));
}
uint32_t Load32(const uint8_t *base, uint32_t va) {
  return rex::byte_swap(*reinterpret_cast<const uint32_t *>(base + va));
}
void Store16(uint8_t *base, uint32_t va, uint16_t v) {
  *reinterpret_cast<uint16_t *>(base + va) = rex::byte_swap(v);
}
void Store32(uint8_t *base, uint32_t va, uint32_t v) {
  *reinterpret_cast<uint32_t *>(base + va) = rex::byte_swap(v);
}

std::string InAddrText(uint32_t ina) {
  return fmt::format("{}.{}.{}.{}", ina >> 24, (ina >> 16) & 0xFF, (ina >> 8) & 0xFF, ina & 0xFF);
}

// Requires g_mutex. The IN_ADDR of a player in a session, handed out once.
uint32_t InAddrForLocked(uint64_t xuid, const SessionId &session) {
  const auto key = std::make_pair(xuid, session);
  if (auto it = g_inaddrs.find(key); it != g_inaddrs.end())
    return it->second;
  const uint32_t ina = g_next_inaddr++;
  g_inaddrs[key] = ina;
  g_connections[ina] = {xuid, session};
  g_connected_at[xuid] = std::chrono::steady_clock::now();
  return ina;
}

Connection *ConnectionLocked(uint32_t ina) {
  auto it = g_connections.find(ina);
  return it == g_connections.end() ? nullptr : &it->second;
}

// Requires g_mutex.
void DeliverLocked(uint32_t from_ina, uint16_t from_port, uint16_t to_port, uint32_t protocol,
                   const uint8_t *data, size_t size) {
  for (auto &[handle, socket] : g_sockets) {
    if (socket.port != to_port || socket.protocol != protocol)
      continue;
    if (socket.queue.size() >= kMaxQueued)
      socket.queue.pop_front();
    socket.queue.push_back({from_ina, from_port, std::vector<uint8_t>(data, data + size)});
    return;
  }
}

// Sends a message to a player by its online ID, in fragments when it is too
// long for one EOS message. Not known or not sent: dropped, as a lossy network
// would.
void SendToPlayer(uint64_t xuid, const uint8_t *data, size_t size) {
  const std::string id = eos::IdOf(xuid);
  if (id.empty()) {
    static uint32_t logged = 0;
    if (logged++ < 8)
      RONL_WARN("Online: no online ID known for player {:016X}; packet dropped", xuid);
    return;
  }
  if (size <= eos::kMaxMessage) {
    eos::Send(id, eos::kChannelGame, data, size, false);
    return;
  }
  static std::atomic<uint8_t> next_message{0};
  const uint8_t message = next_message++;
  const size_t count = (size + kFragmentData - 1) / kFragmentData;
  if (count > 255)
    return;
  uint8_t fragment[eos::kMaxMessage];
  for (size_t i = 0; i < count; ++i) {
    const size_t offset = i * kFragmentData;
    const size_t part = std::min(kFragmentData, size - offset);
    fragment[0] = kTypeFragment;
    fragment[1] = message;
    fragment[2] = uint8_t(i);
    fragment[3] = uint8_t(count);
    std::memcpy(fragment + kFragmentHeaderSize, data + offset, part);
    eos::Send(id, eos::kChannelGame, fragment, kFragmentHeaderSize + part, false);
  }
}

// Sends what the game sent on 'handle' to IN_ADDR 'ina' port 'port'; false
// when that is not a connection reNut carries (the SDK's socket sends it).
bool Send(uint32_t handle, uint32_t ina, uint16_t port, const uint8_t *data, size_t size) {
  if (!xnaddr::IsReNutInAddr(ina) || size > kMaxPayload)
    return false;
  std::lock_guard lock(g_mutex);
  auto socket = g_sockets.find(handle);
  Connection *connection = ConnectionLocked(ina);
  if (socket == g_sockets.end() || !connection)
    return false;
  const uint64_t own = PlayerXuid();
  if (connection->xuid == own) {
    // To ourselves: from our own IN_ADDR in that session.
    DeliverLocked(InAddrForLocked(own, connection->session), socket->second.port, port,
                  socket->second.protocol, data, size);
    return true;
  }
  uint8_t packet[kGameHeaderSize + kMaxPayload];
  uint8_t *p = packet;
  auto put = [&p](uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i)
      *p++ = uint8_t(v >> (8 * i));
  };
  put(kTypeGame, 1);
  put(socket->second.protocol, 1);
  put(socket->second.port, 2);
  put(port, 2);
  std::memcpy(p, connection->session.data(), connection->session.size());
  p += connection->session.size();
  std::memcpy(p, data, size);
  SendToPlayer(connection->xuid, packet, kGameHeaderSize + size);
  ++g_traffic[connection->xuid].sent;
  if (g_logged_sends < 8) {
    ++g_logged_sends;
    RONL_INFO("Online: game packet to {:016X} via {} port {} ({} bytes)", connection->xuid,
              InAddrText(ina), port, size);
  }
  return true;
}

// The next datagram queued for 'handle', if it is a game socket with one.
bool Receive(uint32_t handle, Datagram &out) {
  std::lock_guard lock(g_mutex);
  auto socket = g_sockets.find(handle);
  if (socket == g_sockets.end() || socket->second.queue.empty())
    return false;
  out = std::move(socket->second.queue.front());
  socket->second.queue.pop_front();
  return true;
}

void WriteFrom(uint8_t *base, uint32_t from, uint32_t from_length, const Datagram &d) {
  if (!from)
    return;
  std::memset(base + from, 0, 16);
  Store16(base, from, kAfInet);
  Store16(base, from + 2, d.from_port);
  Store32(base, from + 4, d.from_ina);
  if (from_length)
    Store32(base, from_length, 16);
}

// Requires g_mutex. Whether the game is connected to a player.
bool ConnectedLocked(uint64_t xuid) {
  return std::any_of(g_connections.begin(), g_connections.end(),
                     [xuid](const auto &entry) { return entry.second.xuid == xuid; });
}

void OnGamePacket(uint64_t xuid, const uint8_t *data, size_t size) {
  if (size < kGameHeaderSize)
    return;
  const uint8_t *p = data + 1;
  auto get = [&p](int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
      v |= uint64_t(*p++) << (8 * i);
    return v;
  };
  const uint32_t protocol = uint32_t(get(1));
  const uint16_t from_port = uint16_t(get(2));
  const uint16_t to_port = uint16_t(get(2));
  SessionId session;
  std::memcpy(session.data(), p, session.size());
  std::lock_guard lock(g_mutex);
  g_heard_at[xuid] = std::chrono::steady_clock::now();
  const uint32_t ina = InAddrForLocked(xuid, session);
  DeliverLocked(ina, from_port, to_port, protocol, data + kGameHeaderSize,
                size - kGameHeaderSize);
  ++g_traffic[xuid].received;
  if (g_logged_receives < 8) {
    ++g_logged_receives;
    RONL_INFO("Online: game packet from {:016X} via {} to port {} ({} bytes)", xuid,
              InAddrText(ina), to_port, size - kGameHeaderSize);
  }
}

// Messages in fragments being put back together (EOS thread only), by sender
// and message number.
struct Reassembly {
  uint64_t xuid = 0;
  uint8_t message = 0;
  std::vector<std::vector<uint8_t>> parts;
  size_t received = 0;
};
std::deque<Reassembly> g_reassembling;

void OnMessageFrom(uint64_t xuid, const uint8_t *data, size_t size);

void OnFragment(uint64_t xuid, const uint8_t *data, size_t size) {
  if (size < kFragmentHeaderSize)
    return;
  const uint8_t message = data[1], index = data[2], count = data[3];
  if (!count || index >= count)
    return;
  auto it = std::find_if(g_reassembling.begin(), g_reassembling.end(), [&](const Reassembly &r) {
    return r.xuid == xuid && r.message == message;
  });
  if (it == g_reassembling.end()) {
    if (g_reassembling.size() >= kMaxReassembling)
      g_reassembling.pop_front();
    g_reassembling.push_back({xuid, message, std::vector<std::vector<uint8_t>>(count), 0});
    it = g_reassembling.end() - 1;
  }
  if (it->parts.size() != count || !it->parts[index].empty())
    return;
  it->parts[index].assign(data + kFragmentHeaderSize, data + size);
  if (++it->received < count)
    return;
  std::vector<uint8_t> whole;
  for (const std::vector<uint8_t> &part : it->parts)
    whole.insert(whole.end(), part.begin(), part.end());
  g_reassembling.erase(it);
  OnMessageFrom(xuid, whole.data(), whole.size());
}

void OnMessageFrom(uint64_t xuid, const uint8_t *data, size_t size) {
  if (!size)
    return;
  if (data[0] == kTypeGame) {
    OnGamePacket(xuid, data, size);
  } else if (data[0] == kTypeFragment) {
    OnFragment(xuid, data, size);
  } else if (data[0] == kTypeKeepAlive) {
    std::lock_guard lock(g_mutex);
    // Only players the game is connected to.
    if (ConnectedLocked(xuid))
      g_heard_at[xuid] = std::chrono::steady_clock::now();
  }
}

// A message from a player, on the EOS thread.
void OnPeerMessage(const std::string &id, const uint8_t *data, size_t size) {
  const uint64_t xuid = eos::XuidOf(id);
  if (!xuid || xuid == PlayerXuid())
    return;
  OnMessageFrom(xuid, data, size);
}

// Every couple of seconds on the EOS thread: a keepalive to every player the
// game is connected to, until the player is lost (silent for kLostAfter since
// it was last heard from, or since the game connected to it): the game's
// connections are never forgotten, and EOS would go on trying to reach a
// player long gone.
void SendKeepAlives() {
  const uint64_t own = PlayerXuid();
  const auto now = std::chrono::steady_clock::now();
  std::vector<uint64_t> players;
  {
    std::lock_guard lock(g_mutex);
    for (const auto &[ina, connection] : g_connections) {
      const uint64_t xuid = connection.xuid;
      if (xuid == own || std::find(players.begin(), players.end(), xuid) != players.end())
        continue;
      auto last = std::chrono::steady_clock::time_point{};
      if (auto it = g_heard_at.find(xuid); it != g_heard_at.end())
        last = it->second;
      if (auto it = g_connected_at.find(xuid); it != g_connected_at.end())
        last = std::max(last, it->second);
      if (now - last <= kLostAfter)
        players.push_back(xuid);
    }
  }
  const uint8_t packet[] = {kTypeKeepAlive};
  for (uint64_t xuid : players)
    SendToPlayer(xuid, packet, sizeof(packet));
}

// Every few ticks while in a session: per player, the game packets sent and
// received since the last report and how long ago it was last heard from (so
// a log shows who stopped hearing whom when a player drops).
void ReportTraffic() {
  if (!InSession() || ++g_report_ticks % kReportTicks != 0)
    return;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_mutex);
  for (auto &[xuid, traffic] : g_traffic) {
    auto heard = g_heard_at.find(xuid);
    const long long silent =
        heard == g_heard_at.end()
            ? -1
            : std::chrono::duration_cast<std::chrono::seconds>(now - heard->second).count();
    RONL_INFO("Online: traffic with {:016X} ({}): {} sent, {} received; last heard {} s ago", xuid,
              eos::IdOf(xuid), traffic.sent, traffic.received, silent);
    traffic = {};
  }
}

void Tick() {
  SendKeepAlives();
  ReportTraffic();
}

[[maybe_unused]] const bool g_registered = [] {
  eos::SetReceiver(eos::kChannelGame, &OnPeerMessage);
  eos::AddTicker(&Tick);
  return true;
}();

} // namespace

} // namespace renut::online::transport

using namespace renut::online;
using namespace renut::online::transport;

// ---------------------------------------------------------------------------
// XNet addressing
// ---------------------------------------------------------------------------

// INT XNetXnAddrToInAddr(const XNADDR *address, const XNKID *session, IN_ADDR *ina)
REX_HOOK_RAW(sub_829A8048) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0, 0, 0};
  const uint32_t address = ctx.r3.u32;
  const uint32_t session = ctx.r4.u32;
  const uint32_t ina_out = ctx.r5.u32;
  const auto xuid = Enabled() && address ? xnaddr::XuidOf(base + address) : std::nullopt;
  if (!xuid || !ina_out) {
    __imp__sub_829A8048(ctx, base);
    TraceCall("XNetXnAddrToInAddr", args, ctx.r3.u32);
    return;
  }
  SessionId kid{};
  if (session)
    std::memcpy(kid.data(), base + session, kid.size());
  // The player's online ID, which the XNADDR carries: its packets go there.
  eos::Remember(xnaddr::IdOf(base + address));
  uint32_t ina;
  {
    std::lock_guard lock(transport::g_mutex);
    ina = InAddrForLocked(*xuid, kid);
  }
  Store32(base, ina_out, ina);
  ctx.r3.u64 = 0;
  TraceCall("XNetXnAddrToInAddr", args, ctx.r3.u32);
  RONL_INFO("Online: player {:016X} in session {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} "
            "is {}",
            *xuid, kid[0], kid[1], kid[2], kid[3], kid[4], kid[5], kid[6], kid[7],
            InAddrText(ina));
}

// INT XNetInAddrToXnAddr(IN_ADDR ina, XNADDR *address, XNKID *session)
REX_HOOK_RAW(sub_829A8078) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0, 0, 0};
  const uint32_t ina = ctx.r3.u32;
  const uint32_t address = ctx.r4.u32;
  const uint32_t session = ctx.r5.u32;
  if (!Enabled() || !xnaddr::IsReNutInAddr(ina)) {
    __imp__sub_829A8078(ctx, base);
    TraceCall("XNetInAddrToXnAddr", args, ctx.r3.u32);
    return;
  }
  Connection connection;
  {
    std::lock_guard lock(transport::g_mutex);
    if (Connection *c = ConnectionLocked(ina))
      connection = *c;
  }
  if (!connection.xuid) {
    ctx.r3.u64 = uint64_t(-1);
    TraceCall("XNetInAddrToXnAddr", args, ctx.r3.u32);
    return;
  }
  if (address) {
    const xnaddr::XnAddr a = xnaddr::Make(eos::IdOf(connection.xuid));
    std::memcpy(base + address, a.data(), a.size());
  }
  if (session)
    std::memcpy(base + session, connection.session.data(), connection.session.size());
  ctx.r3.u64 = 0;
  TraceCall("XNetInAddrToXnAddr", args, ctx.r3.u32);
}

// INT XNetUnregisterInAddr(IN_ADDR ina)
REX_HOOK_RAW(sub_829A80A8) {
  if (!Enabled() || !xnaddr::IsReNutInAddr(ctx.r3.u32)) {
    __imp__sub_829A80A8(ctx, base);
    return;
  }
  // Kept: the player's packets for that session still arrive by it.
  ctx.r3.u64 = 0;
}

// INT XNetConnect(IN_ADDR ina)
REX_HOOK_RAW(sub_829A80B8) {
  if (!Enabled() || !xnaddr::IsReNutInAddr(ctx.r3.u32)) {
    __imp__sub_829A80B8(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// DWORD XNetGetConnectStatus(IN_ADDR ina)
REX_HOOK_RAW(sub_829A80C8) {
  const uint32_t ina = ctx.r3.u32;
  if (!Enabled() || !xnaddr::IsReNutInAddr(ina)) {
    __imp__sub_829A80C8(ctx, base);
    return;
  }
  uint32_t status = kConnectPending;
  {
    std::lock_guard lock(transport::g_mutex);
    if (Connection *c = ConnectionLocked(ina)) {
      auto heard = transport::g_heard_at.find(c->xuid);
      if (c->xuid == PlayerXuid())
        status = kConnectConnected;
      else if (heard != transport::g_heard_at.end() &&
               std::chrono::steady_clock::now() - heard->second > kLostAfter)
        status = kConnectLost;
      else
        status = kConnectConnected;  // EOS reaches any player by its online ID
      auto &lost = transport::g_reported_lost;
      if (status == kConnectLost && std::find(lost.begin(), lost.end(), ina) == lost.end()) {
        lost.push_back(ina);
        RONL_WARN("Online: the game is told its connection to {:016X} ({}) is lost: not heard "
                  "from for {} s",
                  c->xuid, InAddrText(ina), kLostAfter.count());
      }
    }
  }
  ctx.r3.u64 = status;
}

// DWORD XUserMuteListQuery(DWORD user, XUID talker, BOOL *muted). The XDK's
// asks XAM (XLIVEBASE 0x5800E), which the SDK does not answer, and leaves
// *muted as it was: nobody is muted.
REX_HOOK_RAW(rex_XUserMuteListQuery) {
  const uint32_t muted = ctx.r5.u32;
  if (!Enabled()) {
    __imp__rex_XUserMuteListQuery(ctx, base);
    return;
  }
  if (muted)
    Store32(base, muted, 0);
  ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// The game's sockets
// ---------------------------------------------------------------------------

// SOCKET socket(int af, int type, int protocol)
REX_HOOK_RAW(rex_socket) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0, 0, 0};
  const uint32_t protocol = ctx.r5.u32;
  __imp__rex_socket(ctx, base);
  TraceCall("socket", args, ctx.r3.u32);
  // Whether or not online play is on or EOS signed in yet: the game makes its
  // sockets at start, before the sign-in is done (or online play is turned on).
  if (ctx.r3.u32 == kSocketError)
    return;
  std::lock_guard lock(transport::g_mutex);
  transport::g_sockets[ctx.r3.u32] = GameSocket{protocol, 0, {}};
}

// int bind(SOCKET s, const sockaddr *name, int length)
REX_HOOK_RAW(rex_bind) {
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0, 0, 0};
  const uint32_t handle = ctx.r3.u32;
  const uint32_t name = ctx.r4.u32;
  bool game_socket = false;
  if (name) {
    std::lock_guard lock(transport::g_mutex);
    auto it = transport::g_sockets.find(handle);
    if (it != transport::g_sockets.end()) {
      it->second.port = Load16(base, name + 2);
      game_socket = true;
    }
  }
  // Offline the game's own port is kept.
  if (!game_socket || !REXCVAR_GET(renut_online)) {
    __imp__rex_bind(ctx, base);
    TraceCall("bind", args, ctx.r3.u32);
    return;
  }
  // Any free host port: the game's port is reNut's to route by.
  const uint16_t port = Load16(base, name + 2);
  Store16(base, name + 2, 0);
  __imp__rex_bind(ctx, base);
  Store16(base, name + 2, port);
  TraceCall("bind", args, ctx.r3.u32);
}

// int closesocket(SOCKET s)
REX_HOOK_RAW(rex_closesocket) {
  {
    std::lock_guard lock(transport::g_mutex);
    transport::g_sockets.erase(ctx.r3.u32);
  }
  __imp__rex_closesocket(ctx, base);
}

// int sendto(SOCKET s, const char *buffer, int length, int flags, const sockaddr *to, int to_length)
REX_HOOK_RAW(rex_sendto) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t buffer = ctx.r4.u32;
  const uint32_t length = ctx.r5.u32;
  const uint32_t to = ctx.r7.u32;
  if (Enabled() && to && buffer &&
      Send(handle, Load32(base, to + 4), Load16(base, to + 2), base + buffer, length)) {
    ctx.r3.u64 = length;
    return;
  }
  __imp__rex_sendto(ctx, base);
}

// int recvfrom(SOCKET s, char *buffer, int length, int flags, sockaddr *from, int *from_length)
REX_HOOK_RAW(rex_recvfrom) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t buffer = ctx.r4.u32;
  const uint32_t length = ctx.r5.u32;
  const uint32_t from = ctx.r7.u32;
  const uint32_t from_length = ctx.r8.u32;
  Datagram d;
  if (!Enabled() || !Receive(handle, d)) {
    __imp__rex_recvfrom(ctx, base);
    return;
  }
  const uint32_t size = std::min<uint32_t>(length, uint32_t(d.data.size()));
  std::memcpy(base + buffer, d.data.data(), size);
  WriteFrom(base, from, from_length, d);
  ctx.r3.u64 = size;
}

// int WSASendTo(SOCKET s, WSABUF *buffers, DWORD count, DWORD *sent, DWORD flags,
//               const sockaddr *to, int to_length, WSAOVERLAPPED *overlapped, ...)
REX_HOOK_RAW(sub_829A87B0) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t buffers = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;
  const uint32_t sent = ctx.r6.u32;
  const uint32_t to = ctx.r8.u32;
  const uint32_t overlapped = ctx.r10.u32;
  if (Enabled() && to && buffers && !overlapped) {
    // WSABUF: u32 length, u32 pointer.
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t length = Load32(base, buffers + 8 * i);
      const uint32_t pointer = Load32(base, buffers + 8 * i + 4);
      data.insert(data.end(), base + pointer, base + pointer + length);
    }
    if (Send(handle, Load32(base, to + 4), Load16(base, to + 2), data.data(), data.size())) {
      if (sent)
        Store32(base, sent, uint32_t(data.size()));
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_829A87B0(ctx, base);
}

// int WSARecvFrom(SOCKET s, WSABUF *buffers, DWORD count, DWORD *received, DWORD *flags,
//                 sockaddr *from, int *from_length, WSAOVERLAPPED *overlapped, ...)
REX_HOOK_RAW(sub_821FB950) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t buffers = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;
  const uint32_t received = ctx.r6.u32;
  const uint32_t flags = ctx.r7.u32;
  const uint32_t from = ctx.r8.u32;
  const uint32_t from_length = ctx.r9.u32;
  const uint32_t overlapped = ctx.r10.u32;
  Datagram d;
  if (!Enabled() || overlapped || !buffers || !Receive(handle, d)) {
    __imp__sub_821FB950(ctx, base);
    return;
  }
  size_t offset = 0;
  for (uint32_t i = 0; i < count && offset < d.data.size(); ++i) {
    const uint32_t length = Load32(base, buffers + 8 * i);
    const uint32_t pointer = Load32(base, buffers + 8 * i + 4);
    const size_t part = std::min<size_t>(length, d.data.size() - offset);
    std::memcpy(base + pointer, d.data.data() + offset, part);
    offset += part;
  }
  if (received)
    Store32(base, received, uint32_t(offset));
  if (flags)
    Store32(base, flags, 0);
  WriteFrom(base, from, from_length, d);
  ctx.r3.u64 = 0;
}
