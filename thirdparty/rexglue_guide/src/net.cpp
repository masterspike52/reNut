// Sockets and XNet for titles, in place of the rexglue runtime's.
//
// Sockets are native sockets in a table of our own. XNet addressing works the
// way a console's does as far as the title can tell: XNetXnAddrToInAddr hands
// out an opaque IN_ADDR per peer, and datagrams from that peer arrive from the
// same IN_ADDR. That needs the sender's XNADDR, so every datagram a title
// sends to a peer or broadcasts carries a small header with it.
//
// Several copies of a game on one PC each take a port offset slot, and
// broadcasts go to every slot, so System Link works between them too.

#include "net.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>
#include <rex/system/xevent.h>

#include "guest.h"
#include "live.h"
#include "live_sessions.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
using native_t = SOCKET;
constexpr native_t kBadNative = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using native_t = int;
constexpr native_t kBadNative = -1;
#endif

using namespace rexglue_guide::guest;

namespace rexglue_guide::net {
namespace {

// ------------------------------------------------------------- platform --

// Winsock error codes, which is what titles expect.
constexpr uint32_t kWsaEWouldBlock = 10035;
constexpr uint32_t kWsaEInProgress = 10036;
constexpr uint32_t kWsaENotSock = 10038;
constexpr uint32_t kWsaEMsgSize = 10040;
constexpr uint32_t kWsaEInval = 10022;
constexpr uint32_t kWsaEAddrInUse = 10048;
constexpr uint32_t kWsaENotConn = 10057;
constexpr uint32_t kWsaIoPending = 997;
constexpr uint32_t kWsaIoIncomplete = 996;

thread_local uint32_t t_last_error = 0;

void EnsureWinsock() {
#if defined(_WIN32)
  static const bool started = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)started;
#endif
}

uint32_t NativeError() {
#if defined(_WIN32)
  return uint32_t(WSAGetLastError());
#else
  switch (errno) {
    case EWOULDBLOCK: return kWsaEWouldBlock;
    case EINPROGRESS: return kWsaEInProgress;
    case EMSGSIZE: return kWsaEMsgSize;
    case EADDRINUSE: return kWsaEAddrInUse;
    case ENOTCONN: return kWsaENotConn;
    case ECONNREFUSED: return 10061;
    case ECONNRESET: return 10054;
    case ETIMEDOUT: return 10060;
    case EISCONN: return 10056;
    case EALREADY: return 10037;
    default: return 10004;
  }
#endif
}

void CloseNative(native_t s) {
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
}

void SetNonBlocking(native_t s, bool on) {
#if defined(_WIN32)
  u_long v = on ? 1 : 0;
  ioctlsocket(s, FIONBIO, &v);
#else
  const int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
}

bool Readable(native_t s, int timeout_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  return select(int(s) + 1, &set, nullptr, nullptr, &tv) > 0;
}

// ------------------------------------------------------------- identity --

// The address other machines on the LAN reach us at: the interface that
// routes to the internet (connecting a UDP socket sends nothing).
uint32_t LanAddress() {
  static uint32_t cached = [] {
    EnsureWinsock();
    uint32_t result = 0;
    native_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadNative) return result;
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
      sockaddr_in local{};
      socklen_t len = sizeof(local);
      if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) result = local.sin_addr.s_addr;
    }
    CloseNative(s);
    return result;
  }();
  return cached;
}

struct Interfaces {
  std::vector<uint32_t> addresses;   // network order
  std::vector<uint32_t> broadcasts;  // directed broadcast per subnet
};

const Interfaces& LocalInterfaces() {
  static std::mutex mutex;
  static Interfaces cached;
  static auto at = std::chrono::steady_clock::time_point{};
  std::lock_guard lock(mutex);
  if (!cached.addresses.empty() && std::chrono::steady_clock::now() - at < std::chrono::seconds(10)) return cached;
  at = std::chrono::steady_clock::now();
  cached = {};
  auto add = [](uint32_t ip, uint32_t prefix) {
    cached.addresses.push_back(ip);
    if (prefix > 0 && prefix < 31) cached.broadcasts.push_back(htonl(ntohl(ip) | ~(0xFFFFFFFFu << (32 - prefix))));
  };
#if defined(_WIN32)
  ULONG size = 15000;
  std::vector<uint8_t> buffer(size);
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  if (ret == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    ret = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  if (ret == NO_ERROR) {
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
      for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
        auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
        if (sa && sa->sin_family == AF_INET && sa->sin_addr.s_addr) add(sa->sin_addr.s_addr, ua->OnLinkPrefixLength);
      }
    }
  }
#else
  ifaddrs* addrs = nullptr;
  if (getifaddrs(&addrs) == 0) {
    for (ifaddrs* it = addrs; it; it = it->ifa_next) {
      if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
      if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
      uint32_t prefix = 0;
      if (it->ifa_netmask) {
        prefix = uint32_t(__builtin_popcount(ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_netmask)->sin_addr.s_addr)));
      }
      add(reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr, prefix);
    }
    freeifaddrs(addrs);
  }
#endif
  std::sort(cached.broadcasts.begin(), cached.broadcasts.end());
  cached.broadcasts.erase(std::unique(cached.broadcasts.begin(), cached.broadcasts.end()), cached.broadcasts.end());
  return cached;
}

bool IsLocalAddress(uint32_t ip) {
  if ((ntohl(ip) >> 24) == 127 || ip == LanAddress()) return true;
  const auto& local = LocalInterfaces().addresses;
  return std::find(local.begin(), local.end(), ip) != local.end();
}

// Title ports are the System Link pair 1000/1001; the slot stride leaves room
// for titles that use a few ports around them.
constexpr uint16_t kBasePort = 1001;
constexpr uint16_t kSlotStride = 10;
constexpr uint16_t kSlotCount = 8;

std::mutex g_offset_mutex;
bool g_offset_resolved = false;
uint16_t g_offset = 0;
std::unordered_map<uint16_t, native_t> g_reservations;

native_t ReservePort(uint16_t port) {
  native_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBadNative) return s;
#if defined(_WIN32)
  const BOOL exclusive = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseNative(s);
    return kBadNative;
  }
  return s;
}

void ReleaseReservation(uint16_t port) {
  std::lock_guard lock(g_offset_mutex);
  auto it = g_reservations.find(port);
  if (it == g_reservations.end()) return;
  CloseNative(it->second);
  g_reservations.erase(it);
}

uint64_t MachineHash() {
  static const uint64_t hash = [] {
    char host[256] = {};
#if defined(_WIN32)
    DWORD len = sizeof(host);
    GetComputerNameA(host, &len);
#else
    gethostname(host, sizeof(host));
#endif
    return std::hash<std::string>{}(host) * 0x9E3779B97F4A7C15ULL;
  }();
  return hash;
}

// --------------------------------------------------------------- tokens --

// IN_ADDRs handed to titles for XNet peers: 0xAB0000nn, one per instance.
// A peer as the title addresses it: an instance and a session key (XNKID), as
// a console's secure association was. A title may hold several at once to one
// instance (Nuts & Bolts: its lobby's session and its match's), each with
// traffic of its own, so each pair gets its own IN_ADDR.
using Kid = std::array<uint8_t, 8>;

struct Peer {
  XnAddr addr;
  Kid kid{};
  uint32_t observed_ip = 0;  // where its datagrams actually come from
};

std::mutex g_peers_mutex;
std::unordered_map<uint32_t, Peer> g_peers;
uint32_t g_next_token = 0xAB000001;

Kid ReadKid(const uint8_t* p) {
  Kid kid{};
  if (p) std::memcpy(kid.data(), p, kid.size());
  return kid;
}

uint32_t TokenFor(const XnAddr& addr, const Kid& kid) {
  std::lock_guard lock(g_peers_mutex);
  uint32_t observed_ip = 0;
  for (auto& [token, peer] : g_peers) {
    if (!peer.addr.SameMachine(addr)) continue;
    if (peer.kid == kid) {
      peer.addr = addr;
      return token;
    }
    if (peer.observed_ip) observed_ip = peer.observed_ip;
  }
  const uint32_t token = g_next_token++;
  g_peers[token] = {addr, kid, observed_ip};
  return token;
}

// The IN_ADDR a datagram is delivered from. One sent without a session key (a
// broadcast, or a peer on the older header) comes from the instance's IN_ADDR
// as before: its keyless one, else the one the title got first. One with a key
// comes from the IN_ADDR for that pair, handed out now if new (a peer joining
// a session of ours).
uint32_t TokenForReceived(const XnAddr& addr, const Kid& kid) {
  if (kid == Kid{}) {
    std::lock_guard lock(g_peers_mutex);
    uint32_t found = 0;
    for (auto& [token, peer] : g_peers) {
      if (!peer.addr.SameMachine(addr)) continue;
      if (peer.kid == kid) {
        found = token;
        break;
      }
      if (!found || token < found) found = token;
    }
    if (found) {
      g_peers[found].addr = addr;
      return found;
    }
  }
  return TokenFor(addr, kid);
}

bool LookupToken(uint32_t token, Peer* out) {
  std::lock_guard lock(g_peers_mutex);
  auto it = g_peers.find(token);
  if (it == g_peers.end()) return false;
  *out = it->second;
  return true;
}

// Where an instance's datagrams come from holds for all its sessions.
void NoteObserved(uint32_t token, uint32_t ip) {
  std::lock_guard lock(g_peers_mutex);
  auto it = g_peers.find(token);
  if (it == g_peers.end()) return;
  const XnAddr addr = it->second.addr;
  for (auto& [t, peer] : g_peers) {
    if (peer.addr.SameMachine(addr)) peer.observed_ip = ip;
  }
}

// TCP to the title server: XNetServerToInAddr hands out this IN_ADDR.
constexpr uint32_t kServerToken = 0xAC000001;

// ---------------------------------------------------------------- header --

// magic, sequence, the sender's XNADDR, the session key it sent for (zero for
// broadcasts): the receiver delivers it from its IN_ADDR for that pair. The
// older header (RXSL, 44 bytes, no key) is still read, as keyless.
constexpr uint8_t kMagic[4] = {'R', 'X', 'S', '2'};
constexpr size_t kHeaderSize = 52;
constexpr uint8_t kMagicV1[4] = {'R', 'X', 'S', 'L'};
constexpr size_t kHeaderSizeV1 = 44;
std::atomic<uint32_t> g_sequence{std::random_device{}()};

void WriteHeader(uint8_t* out, uint32_t sequence, const Kid& kid) {
  std::memcpy(out, kMagic, 4);
  out[4] = uint8_t(sequence >> 24);
  out[5] = uint8_t(sequence >> 16);
  out[6] = uint8_t(sequence >> 8);
  out[7] = uint8_t(sequence);
  LocalXnAddr().Write(out + 8);
  std::memcpy(out + 44, kid.data(), kid.size());
}

// The header's size, or 0 when the datagram has none.
size_t ParseHeader(const uint8_t* buf, size_t len, XnAddr* sender, Kid* kid, uint32_t* sequence) {
  size_t size;
  if (len >= kHeaderSize && std::memcmp(buf, kMagic, 4) == 0) {
    size = kHeaderSize;
    *kid = ReadKid(buf + 44);
  } else if (len >= kHeaderSizeV1 && std::memcmp(buf, kMagicV1, 4) == 0) {
    size = kHeaderSizeV1;
    *kid = Kid{};
  } else {
    return 0;
  }
  *sequence = (uint32_t(buf[4]) << 24) | (uint32_t(buf[5]) << 16) | (uint32_t(buf[6]) << 8) | buf[7];
  sender->Read(buf + 8);
  return size;
}

// Fanned-out broadcasts reach a peer several times; deliver each once.
bool IsDuplicate(uint32_t token, uint32_t sequence) {
  static std::mutex mutex;
  static std::unordered_map<uint32_t, std::deque<uint32_t>> recent;
  std::lock_guard lock(mutex);
  auto& seen = recent[token];
  if (std::find(seen.begin(), seen.end(), sequence) != seen.end()) return true;
  seen.push_back(sequence);
  if (seen.size() > 64) seen.pop_front();
  return false;
}

// --------------------------------------------------------------- sockets --

struct Overlapped {
  uint32_t guest = 0;  // XOVERLAPPED*
  bool send = false;
  std::vector<std::pair<uint32_t, uint32_t>> buffers;  // guest ptr, length
  std::vector<uint8_t> data;                           // pending send bytes
  bool done = false;
  uint32_t bytes = 0;
  uint32_t error = 0;
};

struct Sock {
  native_t fd = kBadNative;
  int type = 0;  // 1 stream, 2 datagram
  bool nonblocking = false;
  uint16_t title_port = 0;
  uint16_t host_port = 0;
  bool to_title_server = false;
  std::mutex io_mutex;
  std::deque<std::shared_ptr<Overlapped>> pending;  // overlapped TCP I/O
};

std::mutex g_socks_mutex;
std::unordered_map<uint32_t, std::shared_ptr<Sock>> g_socks;
uint32_t g_next_handle = 0x0EA00004;

std::shared_ptr<Sock> FindSock(uint32_t handle) {
  std::lock_guard lock(g_socks_mutex);
  auto it = g_socks.find(handle);
  return it == g_socks.end() ? nullptr : it->second;
}

// Guest sockaddr_in: family(be16) port(be16) addr(4, network order).
struct GuestAddr {
  uint16_t port = 0;
  uint32_t addr = 0;  // as the title wrote it, read big-endian (so tokens read 0xAB......)
};

GuestAddr ReadAddr(uint32_t ptr) {
  GuestAddr a;
  if (const uint8_t* p = At(ptr)) {
    a.port = Load16(p + 2);
    a.addr = Load32(p + 4);
  }
  return a;
}

void WriteAddr(uint32_t ptr, uint32_t addr, uint16_t port) {
  uint8_t* p = At(ptr);
  if (!p) return;
  Store16(p, 2);
  Store16(p + 2, port);
  Store32(p + 4, addr);
  std::memset(p + 8, 0, 8);
}

sockaddr_in Native(uint32_t ip_net, uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = ip_net;
  a.sin_port = htons(port);
  return a;
}

int SendNative(native_t fd, const void* data, size_t size, const sockaddr_in& to) {
  return int(sendto(fd, static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)));
}

// ---------------------------------------------------------------- relay --

// Players on different networks often can't reach each other directly, so
// datagrams for a peer behind another public address also go through the
// RexLiveless relay until the peer is heard from directly. Every title UDP
// socket registers with the relay every few seconds, which also keeps its NAT
// mapping open. Copies arriving both ways are dropped as duplicates.
//   RXRG xuid key(16) port   registration (port: the title's, before offset)
//   RXRL xuid port from packet   a datagram for that account's socket for
//                                title port `port`, sent from title port `from`

constexpr size_t kRelayHeaderSize = 16;

struct Relay {
  bool ok = false;
  sockaddr_in addr{};
  uint8_t key[16] = {};
  uint64_t xuid = 0;
  uint32_t public_ip = 0;
};

Relay CurrentRelay() {
  static std::mutex mutex;
  static Relay cached;
  static std::string host;
  static auto at = std::chrono::steady_clock::time_point{};
  std::lock_guard lock(mutex);
  if (std::chrono::steady_clock::now() - at < std::chrono::seconds(1)) return cached;
  at = std::chrono::steady_clock::now();
  const auto live = live::Snapshot();
  Relay r;
  if (live.signed_in && live.relay_port && live.relay_key.size() == 32 && !live.server_host.empty()) {
    if (host != live.server_host || !cached.ok) {
      addrinfo hints{}, *found = nullptr;
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;
      if (getaddrinfo(live.server_host.c_str(), nullptr, &hints, &found) == 0 && found) {
        r.addr = *reinterpret_cast<sockaddr_in*>(found->ai_addr);
        r.ok = true;
        freeaddrinfo(found);
      }
    } else {
      r.addr = cached.addr;
      r.ok = true;
    }
    host = live.server_host;
    r.addr.sin_port = htons(live.relay_port);
    for (int i = 0; i < 16; ++i) r.key[i] = uint8_t(std::stoi(live.relay_key.substr(size_t(i) * 2, 2), nullptr, 16));
    r.xuid = live.me.xuid;
    inet_pton(AF_INET, live.public_ip.c_str(), &r.public_ip);
  }
  cached = r;
  return cached;
}

// REXLIVELESS_FORCE_RELAY=1 sends peer traffic only through the relay (for
// testing it between copies on one PC).
bool ForceRelay() {
  static const bool force = [] {
    const char* env = std::getenv("REXLIVELESS_FORCE_RELAY");
    return env && env[0] == '1';
  }();
  return force;
}

bool UseRelay(const Peer& peer, const Relay& relay) {
  if (!relay.ok || !peer.addr.Xuid()) return false;
  if (ForceRelay()) return true;
  if (peer.observed_ip) return false;  // heard from it directly
  // Another network: a public address that isn't ours, and not this PC.
  return peer.addr.ina_online && relay.public_ip && peer.addr.ina_online != relay.public_ip &&
         !IsLocalAddress(peer.addr.ina);
}

void PutXuid(uint8_t* out, uint64_t xuid) {
  for (int i = 0; i < 8; ++i) out[i] = uint8_t(xuid >> (56 - 8 * i));
}

void RegisterWithRelay(native_t fd, uint16_t title_port, const Relay& relay) {
  uint8_t msg[30];
  std::memcpy(msg, "RXRG", 4);
  PutXuid(msg + 4, relay.xuid);
  std::memcpy(msg + 12, relay.key, 16);
  msg[28] = uint8_t(title_port >> 8);
  msg[29] = uint8_t(title_port);
  SendNative(fd, msg, sizeof(msg), relay.addr);
}

// Keeps every bound title UDP socket registered (and its NAT mapping open).
void RelayThread() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const Relay relay = CurrentRelay();
    if (!relay.ok) continue;
    std::vector<std::shared_ptr<Sock>> socks;
    {
      std::lock_guard lock(g_socks_mutex);
      for (auto& [handle, s] : g_socks) {
        if (s->type != 1 && s->title_port) socks.push_back(s);
      }
    }
    for (auto& s : socks) RegisterWithRelay(s->fd, s->title_port, relay);
  }
}

void StartRelayRegistration(const Sock& s) {
  static std::once_flag started;
  std::call_once(started, [] { std::thread(RelayThread).detach(); });
  const Relay relay = CurrentRelay();
  if (relay.ok) RegisterWithRelay(s.fd, s.title_port, relay);
}

void SendViaRelay(native_t fd, const Relay& relay, uint64_t xuid, uint16_t title_port, uint16_t from_port,
                  const uint8_t* packet, size_t size) {
  std::vector<uint8_t> msg(kRelayHeaderSize + size);
  std::memcpy(msg.data(), "RXRL", 4);
  PutXuid(msg.data() + 4, xuid);
  msg[12] = uint8_t(title_port >> 8);
  msg[13] = uint8_t(title_port);
  msg[14] = uint8_t(from_port >> 8);
  msg[15] = uint8_t(from_port);
  std::memcpy(msg.data() + kRelayHeaderSize, packet, size);
  SendNative(fd, msg.data(), msg.size(), relay.addr);
}

// The title server takes a connection only if it opens with "RXAU", the
// player's XUID and their relay key (the server's HMAC of that XUID), so nobody
// can collect someone else's crates. Connects within 5 s and sends that before
// any of the title's bytes.
bool ConnectTitleServer(Sock& s, const sockaddr_in& to) {
  const auto live = live::Snapshot();
  if (!live.signed_in || live.relay_key.size() != 32) {
    t_last_error = 10051;  // WSAENETUNREACH
    return false;
  }
  uint8_t hello[28];
  std::memcpy(hello, "RXAU", 4);
  PutXuid(hello + 4, live.me.xuid);
  for (int i = 0; i < 16; ++i) hello[12 + i] = uint8_t(std::stoi(live.relay_key.substr(size_t(i) * 2, 2), nullptr, 16));

  SetNonBlocking(s.fd, true);
  bool connected = connect(s.fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0;
  if (!connected) {
    const uint32_t e = NativeError();
    if (e == kWsaEWouldBlock || e == kWsaEInProgress) {
      fd_set write_set, error_set;
      FD_ZERO(&write_set);
      FD_ZERO(&error_set);
      FD_SET(s.fd, &write_set);
      FD_SET(s.fd, &error_set);
      timeval tv{5, 0};
      int value = 0;
      socklen_t size = sizeof(value);
      connected = select(int(s.fd) + 1, nullptr, &write_set, &error_set, &tv) > 0 && FD_ISSET(s.fd, &write_set) &&
                  getsockopt(s.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &size) == 0 && value == 0;
    }
  }
  connected = connected && send(s.fd, reinterpret_cast<const char*>(hello), int(sizeof(hello)), 0) == int(sizeof(hello));
  SetNonBlocking(s.fd, s.nonblocking);
  if (!connected) t_last_error = 10061;  // WSAECONNREFUSED
  return connected;
}

// A datagram for a peer, a broadcast, or a plain address.
int SendDatagram(Sock& s, const uint8_t* data, uint32_t size, const GuestAddr& to) {
  const bool broadcast = to.addr == 0xFFFFFFFFu || (to.addr & 0xFFu) == 0xFFu;
  Peer peer;
  const bool is_peer = !broadcast && LookupToken(to.addr, &peer);
  if (!broadcast && !is_peer) {
    const int sent = SendNative(s.fd, data, size, Native(htonl(to.addr), to.port));
    if (sent < 0) t_last_error = NativeError();
    return sent;
  }

  std::vector<uint8_t> packet(kHeaderSize + size);
  WriteHeader(packet.data(), g_sequence.fetch_add(1), is_peer ? peer.kid : Kid{});
  std::memcpy(packet.data() + kHeaderSize, data, size);

  if (is_peer) {
    const uint16_t offset = peer.addr.port_online >= kBasePort ? uint16_t(peer.addr.port_online - kBasePort) : 0;
    uint32_t ip = peer.observed_ip ? peer.observed_ip : (peer.addr.ina ? peer.addr.ina : peer.addr.ina_online);
    if (IsLocalAddress(ip) || IsLocalAddress(peer.addr.ina)) ip = htonl(INADDR_LOOPBACK);
    // Same public address means same NAT: use the LAN address instead.
    const auto live = live::Snapshot();
    if (!IsLocalAddress(ip) && peer.addr.ina && peer.addr.ina_online && !peer.observed_ip &&
        peer.addr.ina_online != peer.addr.ina) {
      uint32_t mine = 0;
      inet_pton(AF_INET, live.public_ip.c_str(), &mine);
      ip = peer.addr.ina_online == mine ? peer.addr.ina : peer.addr.ina_online;
    }
    const Relay relay = CurrentRelay();
    const bool relayed = UseRelay(peer, relay);
    if (!(relayed && ForceRelay()) &&
        SendNative(s.fd, packet.data(), packet.size(), Native(ip, uint16_t(to.port + offset))) < 0 && !relayed) {
      t_last_error = NativeError();
      return -1;
    }
    if (relayed) SendViaRelay(s.fd, relay, peer.addr.Xuid(), to.port, s.title_port, packet.data(), packet.size());
    return int(size);
  }

  // Broadcast: each subnet (the limited broadcast leaves through one adapter
  // only), plus loopback, on every slot's copy of the port.
  const int on = 1;
  setsockopt(s.fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
  std::vector<uint32_t> targets = LocalInterfaces().broadcasts;
  targets.push_back(htonl(INADDR_BROADCAST));
  targets.push_back(htonl(INADDR_LOOPBACK));
  for (uint16_t slot = 0; slot < kSlotCount; ++slot) {
    for (uint32_t ip : targets) SendNative(s.fd, packet.data(), packet.size(), Native(ip, uint16_t(to.port + slot * kSlotStride)));
  }
  return int(size);
}

// One datagram for the title: our header stripped and the sender reported by
// its token. Returns -1 with t_last_error set, like recvfrom.
int ReceiveDatagram(Sock& s, uint8_t* buf, uint32_t len, uint32_t from_ptr, uint32_t fromlen_ptr) {
  thread_local std::vector<uint8_t> scratch(65536);
  const XnAddr local = LocalXnAddr();
  for (;;) {
    sockaddr_in src{};
    socklen_t src_len = sizeof(src);
    const int n = int(recvfrom(s.fd, reinterpret_cast<char*>(scratch.data()), int(scratch.size()), 0,
                               reinterpret_cast<sockaddr*>(&src), &src_len));
    if (n < 0) {
      t_last_error = NativeError();
#if defined(_WIN32)
      // A port unreachable reply to an earlier send; not the title's business.
      if (t_last_error == 10054) continue;
#endif
      return -1;
    }
    XnAddr sender;
    Kid kid{};
    uint32_t sequence = 0;
    uint32_t from_addr = ntohl(src.sin_addr.s_addr);
    uint16_t from_port = ntohs(src.sin_port);
    const uint8_t* payload = scratch.data();
    uint32_t payload_size = uint32_t(n);
    bool relayed = false;
    if (n > int(kRelayHeaderSize) && std::memcmp(payload, "RXRL", 4) == 0) {
      relayed = true;
      from_port = uint16_t((payload[14] << 8) | payload[15]);  // the sender's title port
      payload += kRelayHeaderSize;
      payload_size -= uint32_t(kRelayHeaderSize);
    }
    const size_t header_size = ParseHeader(payload, payload_size, &sender, &kid, &sequence);
    const bool from_peer = header_size != 0;
    bool deliver = from_peer || !relayed;
    if (deliver && from_peer) {
      if (sender.SameMachine(local)) {
        deliver = false;  // our own broadcast
      } else {
        const uint32_t token = TokenForReceived(sender, kid);
        if (IsDuplicate(token, sequence)) {
          deliver = false;
        } else {
          if (!relayed && !IsLocalAddress(src.sin_addr.s_addr)) NoteObserved(token, src.sin_addr.s_addr);
          const uint16_t offset = sender.port_online >= kBasePort ? uint16_t(sender.port_online - kBasePort) : 0;
          from_addr = token;
          if (!relayed) from_port = from_port >= offset ? uint16_t(from_port - offset) : from_port;
          payload += header_size;
          payload_size -= uint32_t(header_size);
        }
      }
    }
    if (deliver) {
      const uint32_t copy = std::min(payload_size, len);
      std::memcpy(buf, payload, copy);
      WriteAddr(from_ptr, from_addr, from_port);
      if (uint8_t* fl = At(fromlen_ptr)) Store32(fl, 16);
      return int(copy);
    }
    // Dropped one. Titles only read after select() said there's data and
    // treat an error then as fatal, so hand over an empty datagram when
    // nothing else is waiting.
    if (!Readable(s.fd, 0)) {
      WriteAddr(from_ptr, 0, 0);
      if (uint8_t* fl = At(fromlen_ptr)) Store32(fl, 16);
      return 0;
    }
  }
}

// ----------------------------------------------------- overlapped (TCP) --

// Completes overlapped requests: XOVERLAPPED is InternalLow (status),
// InternalHigh (bytes), InternalContext, hEvent, ...
void FinishOverlapped(const Overlapped& o) {
  uint8_t* ov = At(o.guest);
  if (!ov) return;
  Store32(ov + 0, o.error ? o.error : 0);
  Store32(ov + 4, o.bytes);
  const uint32_t event = Load32(ov + 0xC);
  if (event) {
    if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event)) ev->Set(0, false);
  }
}

// Tries to make progress on a socket's oldest overlapped request.
bool PumpOverlapped(Sock& s) {
  std::lock_guard lock(s.io_mutex);
  if (s.pending.empty()) return false;
  Overlapped& o = *s.pending.front();
  if (o.send) {
    const int n = int(send(s.fd, reinterpret_cast<const char*>(o.data.data()), int(o.data.size()), 0));
    if (n < 0) {
      const uint32_t e = NativeError();
      if (e == kWsaEWouldBlock) return false;
      o.error = e;
    } else {
      o.bytes = uint32_t(n);
    }
  } else {
    uint32_t total = 0;
    for (auto& b : o.buffers) total += b.second;
    std::vector<uint8_t> tmp(total);
    const int n = int(recv(s.fd, reinterpret_cast<char*>(tmp.data()), int(total), 0));
    if (n < 0) {
      const uint32_t e = NativeError();
      if (e == kWsaEWouldBlock) return false;
      o.error = e;
    } else {
      uint32_t at = 0;
      for (auto& b : o.buffers) {
        const uint32_t c = std::min(b.second, uint32_t(n) - at);
        if (c) std::memcpy(At(b.first), tmp.data() + at, c);
        at += c;
      }
      o.bytes = uint32_t(n);
    }
  }
  o.done = true;
  FinishOverlapped(o);
  s.pending.pop_front();
  return true;
}

// Background completion of overlapped TCP I/O.
void OverlappedThread() {
  for (;;) {
    std::vector<std::shared_ptr<Sock>> busy;
    {
      std::lock_guard lock(g_socks_mutex);
      for (auto& [h, s] : g_socks) {
        std::lock_guard io(s->io_mutex);
        if (!s->pending.empty()) busy.push_back(s);
      }
    }
    if (busy.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    native_t top = 0;
    for (auto& s : busy) {
      std::lock_guard io(s->io_mutex);
      if (s->pending.empty()) continue;
      FD_SET(s->fd, s->pending.front()->send ? &wr : &rd);
      top = std::max(top, s->fd);
    }
    timeval tv{0, 20000};
    select(int(top) + 1, &rd, &wr, nullptr, &tv);
    for (auto& s : busy) {
      while (PumpOverlapped(*s)) {
      }
    }
  }
}

void EnsureOverlappedThread() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(OverlappedThread).detach(); });
}

// ------------------------------------------------------------- select --

struct FdSet {
  std::vector<uint32_t> handles;
  void Load(uint32_t ptr) {
    handles.clear();
    const uint8_t* p = At(ptr);
    if (!p) return;
    const uint32_t count = std::min<uint32_t>(Load32(p), 64);
    for (uint32_t i = 0; i < count; ++i) handles.push_back(Load32(p + 4 + i * 4));
  }
  void Store(uint32_t ptr, const std::vector<uint32_t>& ready) const {
    uint8_t* p = At(ptr);
    if (!p) return;
    Store32(p, uint32_t(ready.size()));
    for (size_t i = 0; i < ready.size(); ++i) Store32(p + 4 + i * 4, ready[i]);
  }
};

}  // namespace

// --------------------------------------------------------------- public --

void XnAddr::Read(const uint8_t* g) {
  std::memcpy(&ina, g, 4);
  std::memcpy(&ina_online, g + 4, 4);
  port_online = uint16_t((g[8] << 8) | g[9]);
  std::memcpy(enet, g + 10, 6);
  std::memcpy(online, g + 16, 20);
}

void XnAddr::Write(uint8_t* g) const {
  std::memcpy(g, &ina, 4);
  std::memcpy(g + 4, &ina_online, 4);
  g[8] = uint8_t(port_online >> 8);
  g[9] = uint8_t(port_online);
  std::memcpy(g + 10, enet, 6);
  std::memcpy(g + 16, online, 20);
}

bool XnAddr::SameMachine(const XnAddr& o) const {
  return port_online == o.port_online && std::memcmp(enet, o.enet, 6) == 0;
}

uint64_t XnAddr::Xuid() const {
  if (std::memcmp(online, "RXLV", 4) != 0) return 0;
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) x = (x << 8) | online[4 + i];
  return x;
}

uint16_t PortOffset() {
  std::lock_guard lock(g_offset_mutex);
  if (g_offset_resolved) return g_offset;
  g_offset_resolved = true;
  EnsureWinsock();
  for (uint16_t slot = 0; slot < kSlotCount; ++slot) {
    const uint16_t offset = uint16_t(slot * kSlotStride);
    const native_t a = ReservePort(uint16_t(kBasePort - 1 + offset));
    const native_t b = a == kBadNative ? kBadNative : ReservePort(uint16_t(kBasePort + offset));
    if (a != kBadNative && b != kBadNative) {
      g_reservations[uint16_t(kBasePort - 1 + offset)] = a;
      g_reservations[uint16_t(kBasePort + offset)] = b;
      g_offset = offset;
      REXKRNL_INFO("Rexglue Guide: System Link port offset {} (slot {})", offset, slot);
      return g_offset;
    }
    if (a != kBadNative) CloseNative(a);
  }
  REXKRNL_WARN("Rexglue Guide: every System Link port slot is taken; using offset 0");
  return g_offset;
}

XnAddr LocalXnAddr() {
  XnAddr a;
  const uint32_t lan = LanAddress();
  a.ina = lan ? lan : htonl(INADDR_LOOPBACK);
  const auto live = live::Snapshot();
  if (!live.public_ip.empty()) inet_pton(AF_INET, live.public_ip.c_str(), &a.ina_online);
  a.port_online = uint16_t(kBasePort + PortOffset());
  // Locally administered MAC, unique per machine and port slot.
  const uint64_t mac = MachineHash() ^ (uint64_t(PortOffset()) * 0xD1B54A32D192ED03ULL);
  for (int i = 0; i < 6; ++i) a.enet[i] = uint8_t(mac >> (i * 8));
  a.enet[0] = uint8_t((a.enet[0] & 0xFC) | 0x02);
  // The RexLiveless account, so peers can tell who an address belongs to.
  if (live.signed_in) {
    std::memcpy(a.online, "RXLV", 4);
    for (int i = 0; i < 8; ++i) a.online[4 + i] = uint8_t(live.me.xuid >> (56 - 8 * i));
  }
  return a;
}

}  // namespace rexglue_guide::net

// ===================================================================== imports
// NetDll_* functions take the XNCALLER type first; the real arguments follow.

using namespace rexglue_guide::net;

namespace {

std::shared_ptr<Sock> SockArg(PPCContext& ctx, int n) {
  auto s = FindSock(Arg(ctx, n));
  if (!s) t_last_error = kWsaENotSock;
  return s;
}

constexpr uint32_t kSocketError = 0xFFFFFFFFu;

}  // namespace

// socket(af, type, protocol)
extern "C" REX_FUNC(__imp__NetDll_socket) {
  EnsureWinsock();
  const uint32_t type = Arg(ctx, 2);
  const uint32_t proto = Arg(ctx, 3);
  auto s = std::make_shared<Sock>();
  s->type = int(type);
  if (type == 1) {
    s->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  } else {
    s->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);  // VDP (254) is UDP underneath
  }
  (void)proto;
  if (s->fd == kBadNative) {
    t_last_error = NativeError();
    return Return(ctx, kSocketError);
  }
  std::lock_guard lock(g_socks_mutex);
  const uint32_t handle = g_next_handle;
  g_next_handle += 4;
  g_socks[handle] = s;
  Return(ctx, handle);
}

// closesocket(s)
extern "C" REX_FUNC(__imp__NetDll_closesocket) {
  std::shared_ptr<Sock> s;
  {
    std::lock_guard lock(g_socks_mutex);
    auto it = g_socks.find(Arg(ctx, 1));
    if (it == g_socks.end()) {
      t_last_error = kWsaENotSock;
      return Return(ctx, kSocketError);
    }
    s = it->second;
    g_socks.erase(it);
  }
  CloseNative(s->fd);
  Return(ctx, 0);
}

// shutdown(s, how)
extern "C" REX_FUNC(__imp__NetDll_shutdown) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  shutdown(s->fd, int(Arg(ctx, 2)));
  Return(ctx, 0);
}

// setsockopt(s, level, optname, optval, optlen)
extern "C" REX_FUNC(__imp__NetDll_setsockopt) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  const uint32_t level = Arg(ctx, 2), name = Arg(ctx, 3);
  const uint8_t* val = At(Arg(ctx, 4));
  const int v = val ? int(Load32(val)) : 0;
  if (level == 0xFFFF) {
    switch (name) {
      case 0x0020:  // SO_BROADCAST
        setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&v), sizeof(v));
        break;
      case 0x1001:  // SO_SNDBUF
        setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&v), sizeof(v));
        break;
      case 0x1002:  // SO_RCVBUF
        setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&v), sizeof(v));
        break;
      default:
        // SO_REUSEADDR (4) is ignored on purpose: consoles never share ports,
        // and copies on one PC must not split each other's traffic. XNet's
        // own options (0x5801 and up) don't apply.
        break;
    }
  } else if (level == 6 && name == 1) {  // TCP_NODELAY
    setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v), sizeof(v));
  }
  Return(ctx, 0);
}

// getsockopt(s, level, optname, optval, optlen*)
extern "C" REX_FUNC(__imp__NetDll_getsockopt) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  const uint32_t level = Arg(ctx, 2), name = Arg(ctx, 3);
  uint8_t* val = At(Arg(ctx, 4));
  uint8_t* len = At(Arg(ctx, 5));
  int value = 0;
  if (level == 0xFFFF && name == 0x1007) {  // SO_ERROR
    socklen_t size = sizeof(value);
    getsockopt(s->fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &size);
  } else if (level == 0xFFFF && (name == 0x1001 || name == 0x1002)) {
    socklen_t size = sizeof(value);
    getsockopt(s->fd, SOL_SOCKET, name == 0x1001 ? SO_SNDBUF : SO_RCVBUF, reinterpret_cast<char*>(&value), &size);
  }
  if (val) Store32(val, uint32_t(value));
  if (len) Store32(len, 4);
  Return(ctx, 0);
}

// ioctlsocket(s, cmd, argp)
extern "C" REX_FUNC(__imp__NetDll_ioctlsocket) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  const uint32_t cmd = Arg(ctx, 2);
  uint8_t* arg = At(Arg(ctx, 3));
  if (cmd == 0x8004667E && arg) {  // FIONBIO
    s->nonblocking = Load32(arg) != 0;
    SetNonBlocking(s->fd, s->nonblocking);
  } else if (cmd == 0x4004667F && arg) {  // FIONREAD
#if defined(_WIN32)
    u_long n = 0;
    ioctlsocket(s->fd, FIONREAD, &n);
#else
    int n = 0;
    ioctl(s->fd, FIONREAD, &n);
#endif
    Store32(arg, uint32_t(n));
  }
  Return(ctx, 0);
}

// bind(s, name, namelen)
extern "C" REX_FUNC(__imp__NetDll_bind) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  const GuestAddr name = ReadAddr(Arg(ctx, 2));
  s->title_port = name.port;
  // Always ANY: a socket bound to one address doesn't get broadcasts on
  // Windows. Non-zero ports move by this copy's slot offset.
  uint16_t port = name.port;
  if (port) {
    port = uint16_t(port + PortOffset());
    if (s->type == 2) ReleaseReservation(port);
  }
  const sockaddr_in addr = Native(htonl(INADDR_ANY), port);
  if (bind(s->fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    t_last_error = NativeError();
    REXKRNL_ERROR("Rexglue Guide: bind to port {} failed ({})", port, t_last_error);
    return Return(ctx, kSocketError);
  }
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  getsockname(s->fd, reinterpret_cast<sockaddr*>(&bound), &len);
  s->host_port = ntohs(bound.sin_port);
  if (s->nonblocking) SetNonBlocking(s->fd, true);
  if (s->type != 1 && s->title_port) StartRelayRegistration(*s);
  Return(ctx, 0);
}

// connect(s, name, namelen)
extern "C" REX_FUNC(__imp__NetDll_connect) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  const GuestAddr name = ReadAddr(Arg(ctx, 2));
  sockaddr_in to = Native(htonl(name.addr), name.port);
  if (name.addr == kServerToken) {
    // The title server: the RexLiveless server's XTS port, whatever port the
    // title asked for (it learns them from our own topology answer).
    const auto live = rexglue_guide::live::Snapshot();
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (!live.xts_port || live.server_host.empty() ||
        getaddrinfo(live.server_host.c_str(), nullptr, &hints, &found) != 0 || !found) {
      t_last_error = 10051;  // WSAENETUNREACH
      return Return(ctx, kSocketError);
    }
    to = *reinterpret_cast<sockaddr_in*>(found->ai_addr);
    to.sin_port = htons(live.xts_port);
    freeaddrinfo(found);
    s->to_title_server = true;
    return Return(ctx, ConnectTitleServer(*s, to) ? 0 : kSocketError);
  }
  if (connect(s->fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0) {
    t_last_error = NativeError();
    if (t_last_error == 10035) t_last_error = kWsaEWouldBlock;  // in progress, nonblocking
    return Return(ctx, kSocketError);
  }
  Return(ctx, 0);
}

// select(nfds, readfds, writefds, exceptfds, timeout)
extern "C" REX_FUNC(__imp__NetDll_select) {
  const uint32_t ptrs[3] = {Arg(ctx, 2), Arg(ctx, 3), Arg(ctx, 4)};
  FdSet sets[3];
  fd_set native[3];
  native_t top = 0;
  for (int i = 0; i < 3; ++i) {
    FD_ZERO(&native[i]);
    if (!ptrs[i]) continue;
    sets[i].Load(ptrs[i]);
    for (uint32_t h : sets[i].handles) {
      if (auto s = FindSock(h)) {
        FD_SET(s->fd, &native[i]);
        top = std::max(top, s->fd);
      }
    }
  }
  timeval tv{}, *timeout = nullptr;
  if (const uint8_t* t = At(Arg(ctx, 5))) {
    tv.tv_sec = long(Load32(t));
    tv.tv_usec = long(Load32(t + 4));
    timeout = &tv;
  }
  const int n = select(int(top) + 1, ptrs[0] ? &native[0] : nullptr, ptrs[1] ? &native[1] : nullptr,
                       ptrs[2] ? &native[2] : nullptr, timeout);
  if (n < 0) {
    t_last_error = NativeError();
    return Return(ctx, kSocketError);
  }
  uint32_t ready_total = 0;
  for (int i = 0; i < 3; ++i) {
    if (!ptrs[i]) continue;
    std::vector<uint32_t> ready;
    for (uint32_t h : sets[i].handles) {
      auto s = FindSock(h);
      if (s && FD_ISSET(s->fd, &native[i])) ready.push_back(h);
    }
    sets[i].Store(ptrs[i], ready);
    ready_total += uint32_t(ready.size());
  }
  Return(ctx, ready_total);
}

// recvfrom(s, buf, len, flags, from, fromlen*)
extern "C" REX_FUNC(__imp__NetDll_recvfrom) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  uint8_t* buf = At(Arg(ctx, 2));
  const uint32_t len = Arg(ctx, 3);
  if (s->type == 1) {
    const int n = int(recv(s->fd, reinterpret_cast<char*>(buf), int(len), 0));
    if (n < 0) t_last_error = NativeError();
    return Return(ctx, uint32_t(n));
  }
  Return(ctx, uint32_t(ReceiveDatagram(*s, buf, len, Arg(ctx, 5), Arg(ctx, 6))));
}

// recv(s, buf, len, flags)
extern "C" REX_FUNC(__imp__NetDll_recv) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  uint8_t* buf = At(Arg(ctx, 2));
  const uint32_t len = Arg(ctx, 3);
  if (s->type == 2) return Return(ctx, uint32_t(ReceiveDatagram(*s, buf, len, 0, 0)));
  const int n = int(recv(s->fd, reinterpret_cast<char*>(buf), int(len), 0));
  if (n < 0) t_last_error = NativeError();
  Return(ctx, uint32_t(n));
}

// sendto(s, buf, len, flags, to, tolen)
extern "C" REX_FUNC(__imp__NetDll_sendto) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  const uint8_t* buf = At(Arg(ctx, 2));
  const uint32_t len = Arg(ctx, 3);
  if (s->type == 1 || !Arg(ctx, 5)) {
    const int n = int(send(s->fd, reinterpret_cast<const char*>(buf), int(len), 0));
    if (n < 0) t_last_error = NativeError();
    return Return(ctx, uint32_t(n));
  }
  Return(ctx, uint32_t(SendDatagram(*s, buf, len, ReadAddr(Arg(ctx, 5)))));
}

// send(s, buf, len, flags)
extern "C" REX_FUNC(__imp__NetDll_send) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  const int n = int(send(s->fd, reinterpret_cast<const char*>(At(Arg(ctx, 2))), int(Arg(ctx, 3)), 0));
  if (n < 0) t_last_error = NativeError();
  Return(ctx, uint32_t(n));
}

// WSARecv(s, WSABUF* bufs, count, DWORD* received, DWORD* flags, overlapped, routine)
extern "C" REX_FUNC(__imp__NetDll_WSARecv) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  auto o = std::make_shared<Overlapped>();
  const uint8_t* bufs = At(Arg(ctx, 2));
  for (uint32_t i = 0; bufs && i < Arg(ctx, 3); ++i) o->buffers.push_back({Load32(bufs + i * 8 + 4), Load32(bufs + i * 8)});
  o->guest = Arg(ctx, 6);
  if (uint8_t* flags = At(Arg(ctx, 5))) Store32(flags, 0);
  if (!o->guest) {
    // Blocking or nonblocking without overlapped: read now.
    uint32_t total = 0;
    for (auto& b : o->buffers) total += b.second;
    std::vector<uint8_t> tmp(total);
    const int n = int(recv(s->fd, reinterpret_cast<char*>(tmp.data()), int(total), 0));
    if (n < 0) {
      t_last_error = NativeError();
      return Return(ctx, kSocketError);
    }
    uint32_t at = 0;
    for (auto& b : o->buffers) {
      const uint32_t c = std::min(b.second, uint32_t(n) - at);
      if (c) std::memcpy(At(b.first), tmp.data() + at, c);
      at += c;
    }
    if (uint8_t* got = At(Arg(ctx, 4))) Store32(got, uint32_t(n));
    return Return(ctx, 0);
  }
  if (uint8_t* ov = At(o->guest)) Store32(ov, kWsaIoPending);
  {
    std::lock_guard lock(s->io_mutex);
    s->pending.push_back(o);
  }
  SetNonBlocking(s->fd, true);
  EnsureOverlappedThread();
  t_last_error = kWsaIoPending;
  Return(ctx, kSocketError);
}

// WSASend(s, WSABUF* bufs, count, DWORD* sent, flags, overlapped, routine)
extern "C" REX_FUNC(__imp__NetDll_WSASend) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  auto o = std::make_shared<Overlapped>();
  o->send = true;
  const uint8_t* bufs = At(Arg(ctx, 2));
  for (uint32_t i = 0; bufs && i < Arg(ctx, 3); ++i) {
    const uint32_t len = Load32(bufs + i * 8), ptr = Load32(bufs + i * 8 + 4);
    const uint8_t* data = At(ptr);
    if (data) o->data.insert(o->data.end(), data, data + len);
  }
  o->guest = Arg(ctx, 6);
  if (!o->guest) {
    const int n = int(send(s->fd, reinterpret_cast<const char*>(o->data.data()), int(o->data.size()), 0));
    if (n < 0) {
      t_last_error = NativeError();
      return Return(ctx, kSocketError);
    }
    if (uint8_t* sent = At(Arg(ctx, 4))) Store32(sent, uint32_t(n));
    return Return(ctx, 0);
  }
  if (uint8_t* ov = At(o->guest)) Store32(ov, kWsaIoPending);
  {
    std::lock_guard lock(s->io_mutex);
    s->pending.push_back(o);
  }
  SetNonBlocking(s->fd, true);
  EnsureOverlappedThread();
  t_last_error = kWsaIoPending;
  Return(ctx, kSocketError);
}

// WSASendTo(s, WSABUF* bufs, count, DWORD* sent, flags, to, tolen, overlapped, routine)
// Nuts & Bolts sends its peer traffic with this. A datagram goes at once, so
// an overlapped send completes at once too.
extern "C" REX_FUNC(__imp__NetDll_WSASendTo) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  std::vector<uint8_t> data;
  const uint8_t* bufs = At(Arg(ctx, 2));
  for (uint32_t i = 0; bufs && i < Arg(ctx, 3); ++i) {
    const uint32_t len = Load32(bufs + i * 8), ptr = Load32(bufs + i * 8 + 4);
    const uint8_t* p = At(ptr);
    if (p) data.insert(data.end(), p, p + len);
  }
  const uint32_t to = Arg(ctx, 6);
  int n;
  if (s->type == 1 || !to) {
    n = int(send(s->fd, reinterpret_cast<const char*>(data.data()), int(data.size()), 0));
    if (n < 0) t_last_error = NativeError();
  } else {
    n = SendDatagram(*s, data.data(), uint32_t(data.size()), ReadAddr(to));
  }
  if (n < 0) return Return(ctx, kSocketError);
  if (uint8_t* sent = At(Arg(ctx, 4))) Store32(sent, uint32_t(n));
  if (const uint32_t ov = Arg(ctx, 8)) {
    Overlapped o;
    o.guest = ov;
    o.bytes = uint32_t(n);
    FinishOverlapped(o);
  }
  Return(ctx, 0);
}

// WSARecvFrom(s, WSABUF* bufs, count, DWORD* received, DWORD* flags, from, int* fromlen,
//             overlapped, routine)
// Nuts & Bolts polls its peer sockets with this every frame. A datagram is
// read now: an overlapped call that finds one completes at once, and one that
// finds none fails as a nonblocking read does.
extern "C" REX_FUNC(__imp__NetDll_WSARecvFrom) {
  auto s = SockArg(ctx, 1);
  if (!s) return Return(ctx, kSocketError);
  std::vector<std::pair<uint32_t, uint32_t>> buffers;  // guest ptr, length
  uint32_t total = 0;
  const uint8_t* bufs = At(Arg(ctx, 2));
  for (uint32_t i = 0; bufs && i < Arg(ctx, 3); ++i) {
    buffers.push_back({Load32(bufs + i * 8 + 4), Load32(bufs + i * 8)});
    total += buffers.back().second;
  }
  std::vector<uint8_t> tmp(total);
  int n;
  if (s->type == 1) {
    n = int(recv(s->fd, reinterpret_cast<char*>(tmp.data()), int(total), 0));
    if (n < 0) t_last_error = NativeError();
  } else {
    n = ReceiveDatagram(*s, tmp.data(), total, Arg(ctx, 6), Arg(ctx, 7));
  }
  if (n < 0) return Return(ctx, kSocketError);
  uint32_t at = 0;
  for (auto& b : buffers) {
    const uint32_t c = std::min(b.second, uint32_t(n) - at);
    if (c) std::memcpy(At(b.first), tmp.data() + at, c);
    at += c;
  }
  if (uint8_t* got = At(Arg(ctx, 4))) Store32(got, uint32_t(n));
  if (uint8_t* flags = At(Arg(ctx, 5))) Store32(flags, 0);
  if (const uint32_t ov = Arg(ctx, 8)) {
    Overlapped o;
    o.guest = ov;
    o.bytes = uint32_t(n);
    FinishOverlapped(o);
  }
  Return(ctx, 0);
}

// WSAGetOverlappedResult(s, overlapped, DWORD* transferred, BOOL wait, DWORD* flags)
extern "C" REX_FUNC(__imp__NetDll_WSAGetOverlappedResult) {
  const uint8_t* ov = At(Arg(ctx, 2));
  if (!ov) {
    t_last_error = kWsaEInval;
    return Return(ctx, 0);
  }
  const bool wait = Arg(ctx, 4) != 0;
  auto s = FindSock(Arg(ctx, 1));
  for (int spins = 0; Load32(ov) == kWsaIoPending; ++spins) {
    if (!wait || !s) {
      t_last_error = kWsaIoIncomplete;
      return Return(ctx, 0);
    }
    PumpOverlapped(*s);
    if (Load32(ov) == kWsaIoPending) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (uint8_t* bytes = At(Arg(ctx, 3))) Store32(bytes, Load32(ov + 4));
  if (uint8_t* flags = At(Arg(ctx, 5))) Store32(flags, 0);
  const uint32_t status = Load32(ov);
  if (status != 0) {
    t_last_error = status;
    return Return(ctx, 0);
  }
  Return(ctx, 1);
}

// WSAGetLastError()
extern "C" REX_FUNC(__imp__NetDll_WSAGetLastError) {
  Return(ctx, t_last_error);
}

// ------------------------------------------------------------------- XNet --

// XNetGetTitleXnAddr(XNADDR*) -> XNET_GET_XNADDR_* flags
extern "C" REX_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
  const XnAddr local = LocalXnAddr();
  if (uint8_t* out = At(Arg(ctx, 1))) local.Write(out);
  uint32_t flags = 0x02 | 0x04;  // ETHERNET | STATIC
  if (local.ina_online && rexglue_guide::live::Snapshot().signed_in) flags |= 0x80;  // ONLINE
  Return(ctx, flags);
}

// XNetGetEthernetLinkStatus() -> ACTIVE | 100MBPS | FULL_DUPLEX
extern "C" REX_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) {
  Return(ctx, 0x01 | 0x02 | 0x08);
}

// XNetXnAddrToInAddr(XNADDR*, XNKID*, IN_ADDR*)
extern "C" REX_FUNC(__imp__NetDll_XNetXnAddrToInAddr) {
  const uint8_t* in = At(Arg(ctx, 1));
  uint8_t* out = At(Arg(ctx, 3));
  if (!in || !out) return Return(ctx, kWsaEInval);
  XnAddr addr;
  addr.Read(in);
  Store32(out, TokenFor(addr, ReadKid(At(Arg(ctx, 2)))));
  Return(ctx, 0);
}

// XNetInAddrToXnAddr(IN_ADDR (by value), XNADDR*, XNKID*)
extern "C" REX_FUNC(__imp__NetDll_XNetInAddrToXnAddr) {
  Peer peer;
  uint8_t* out = At(Arg(ctx, 2));
  if (!out || !LookupToken(Arg(ctx, 1), &peer)) return Return(ctx, kWsaEInval);
  peer.addr.Write(out);
  if (uint8_t* kid = At(Arg(ctx, 3))) std::memcpy(kid, peer.kid.data(), peer.kid.size());
  Return(ctx, 0);
}

// XNetConnect(IN_ADDR). A peer's IN_ADDR needs no key exchange here: its
// datagrams go straight to it.
extern "C" REX_FUNC(__imp__NetDll_XNetConnect) {
  Peer peer;
  if (!LookupToken(Arg(ctx, 1), &peer)) return CallOriginal("__imp__NetDll_XNetConnect", ctx, base);
  Return(ctx, 0);
}

// XNetGetConnectStatus(IN_ADDR) -> XNET_CONNECT_STATUS_*. Titles wait for
// CONNECTED before they talk to a peer (Nuts & Bolts does); the runtime's is a
// stub that never says so.
extern "C" REX_FUNC(__imp__NetDll_XNetGetConnectStatus) {
  constexpr uint32_t kConnected = 2;
  Peer peer;
  if (!LookupToken(Arg(ctx, 1), &peer)) return CallOriginal("__imp__NetDll_XNetGetConnectStatus", ctx, base);
  Return(ctx, kConnected);
}

// XNetUnregisterInAddr(IN_ADDR)
extern "C" REX_FUNC(__imp__NetDll_XNetUnregisterInAddr) {
  Return(ctx, 0);
}

// XNetServerToInAddr(IN_ADDR server, DWORD service_id, IN_ADDR*)
extern "C" REX_FUNC(__imp__NetDll_XNetServerToInAddr) {
  REXKRNL_INFO("Rexglue Guide: XNetServerToInAddr({:08X}, service {:08X})", Arg(ctx, 1), Arg(ctx, 2));
  if (uint8_t* out = At(Arg(ctx, 3))) Store32(out, kServerToken);
  Return(ctx, 0);
}

namespace {

// XNQOS: count, pending, then 32-byte XNQOSINFO entries (TiP's poller steps
// 0x20): flags, reserved, probes xmit/recv, data len, data ptr, rtt min/med,
// up/down bits per second, 8 reserved.
std::mutex g_qos_mutex;
std::unordered_set<uint32_t> g_live_qos;

}  // namespace

// XNetQosLookup(n consoles, XNADDR**, XNKID**, XNKEY**, n gateways, IN_ADDR*,
//               DWORD* service ids, probes, bps, flags, event, XNQOS**)
extern "C" REX_FUNC(__imp__NetDll_XNetQosLookup) {
  const uint32_t consoles = Arg(ctx, 1);
  const uint32_t gateways = Arg(ctx, 5);
  const uint32_t event = Arg(ctx, 11);
  uint8_t* out = At(Arg(ctx, 12));
  const uint32_t count = std::max<uint32_t>(1, consoles + gateways);
  // Each console's listen data (what its host published), stored after the
  // entries in the same block so XNetQosRelease frees it too.
  std::vector<std::vector<uint8_t>> data(count);
  const uint8_t* kids = At(Arg(ctx, 3));
  for (uint32_t i = 0; i < consoles && kids; ++i) {
    if (const uint8_t* kid = At(Load32(kids + 4 * i))) rexglue_guide::sessions::FetchQos(kid, &data[i]);
  }
  uint32_t size = 8 + 32 * count;
  for (const auto& d : data) size += uint32_t((d.size() + 7) & ~size_t(7));
  const uint32_t qos = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  uint8_t* q = At(qos);
  std::memset(q, 0, size);
  Store32(q, count);
  uint32_t data_at = qos + 8 + 32 * count;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* info = q + 8 + 32 * i;
    info[0] = 0x01 | 0x02;  // COMPLETE | TARGET_CONTACTED
    Store16(info + 2, 4);
    Store16(info + 4, 4);
    if (!data[i].empty()) {
      info[0] |= 0x08;  // DATA_RECEIVED
      Store16(info + 6, uint16_t(data[i].size()));
      Store32(info + 8, data_at);
      std::memcpy(At(data_at), data[i].data(), data[i].size());
      data_at += uint32_t((data[i].size() + 7) & ~size_t(7));
    }
    Store16(info + 12, 30);  // rtt min
    Store16(info + 14, 50);  // rtt median
    Store32(info + 16, 8 * 1024 * 1024);
    Store32(info + 20, 8 * 1024 * 1024);
  }
  {
    std::lock_guard lock(g_qos_mutex);
    g_live_qos.insert(qos);
  }
  if (out) Store32(out, qos);
  if (event) {
    if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event)) ev->Set(0, false);
  }
  Return(ctx, 0);
}

// XNetQosRelease(XNQOS*). Titles may release one twice; only free live ones.
extern "C" REX_FUNC(__imp__NetDll_XNetQosRelease) {
  const uint32_t qos = Arg(ctx, 1);
  {
    std::lock_guard lock(g_qos_mutex);
    if (!g_live_qos.erase(qos)) return Return(ctx, 0);
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos);
  Return(ctx, 0);
}

// XNetQosListen(XNKID*, data, size, bps, flags). Probes never reach us, so
// the data a host sets goes to RexLiveless with its session, and joiners'
// XNetQosLookup reads it back from there.
extern "C" REX_FUNC(__imp__NetDll_XNetQosListen) {
  constexpr uint32_t kSetData = 0x04;
  const uint8_t* kid = At(Arg(ctx, 1));
  const uint8_t* data = At(Arg(ctx, 2));
  const uint32_t size = std::min<uint32_t>(Arg(ctx, 3), 1024);
  if (kid && (Arg(ctx, 5) & kSetData)) {
    rexglue_guide::sessions::PublishQos(kid, data ? std::vector<uint8_t>(data, data + size) : std::vector<uint8_t>());
  }
  Return(ctx, 0);
}
