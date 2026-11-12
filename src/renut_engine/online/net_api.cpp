#include "renut_engine/online/net_api.h"

#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rexfabric/game_api.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "renut_engine/game/guest_call.h"
#include "renut_engine/game/menu_api.h"

// The title's sockets (the Guide's XNet: a peer's address is its secure token, reached directly or relayed).
REX_IMPORT(__imp__NetDll_socket, NetSocket, u32(u32, u32, u32, u32));
REX_IMPORT(__imp__NetDll_closesocket, NetClose, u32(u32, u32));
REX_IMPORT(__imp__NetDll_ioctlsocket, NetIoctl, u32(u32, u32, u32, u32));
REX_IMPORT(__imp__NetDll_bind, NetBind, u32(u32, u32, u32, u32));
REX_IMPORT(__imp__NetDll_sendto, NetSendTo, u32(u32, u32, u32, u32, u32, u32, u32));
REX_IMPORT(__imp__NetDll_recvfrom, NetRecvFrom, u32(u32, u32, u32, u32, u32, u32, u32));
// Going to multiplayer as the main menu's Multiplayer page does (sub_825A12D0): the save's multiplayer data
// loaded (or made), then the front end.
REX_IMPORT(__imp__sub_824C44D8, MultiplayerManager, u32());     // the match manager: + 147 co-op, + 189 Live
REX_IMPORT(__imp__sub_825A15E0, StartMultiplayer, u32(u32, u32));  // (StartMenu, Live)
// Hooked: a world's messages (world, &message), through its game data (+ 5552).
REX_EXTERN(__imp__sub_82513DB8);
// Hooked: a session context set by names (session, user, &context, &value); the lookups it doesn't check.
REX_EXTERN(__imp__sub_826A2DA8);
REX_IMPORT(__imp__sub_826A2E30, FindContext, u32(u32, u32));       // (session, &name): its context, or 0
REX_IMPORT(__imp__sub_826A3010, FindContextValue, u32(u32, u32));  // (context, &name): its value, or 0

namespace renut::online::net_api {
namespace {

using renut::guest::Load32;
using renut::guest::Store16;
using renut::guest::Store32;
using Error = std::runtime_error;

// Guest data.
constexpr uint32_t kGameSession = 0x82D99B58;   // the match's session (net system "Main")
constexpr uint32_t kPartySession = 0x82D99B5C;  // the lobby's party session
constexpr uint32_t kSessionState = 336;         // 5 once it's up
constexpr uint32_t kSessionLocal = 4580;        // this machine's peer; + 36 set on the host
constexpr uint32_t kPeerHost = 36;
constexpr uint32_t kStateUp = 5;
constexpr uint32_t kMultiplayer = 0x82E19934;   // set while MultiPlayerMenu_c lives (lobby and matches)
constexpr uint32_t kNetReady = 0x82D99B54;      // the network has started
// The connections: a std::map's head node (+ 4 the root; a node's + 0 left, + 8 right, + 16 value, + 21 nil),
// each connection's + 52 the sockaddr the game sends to (+ 56 its address).
constexpr uint32_t kConnections = 0x82E29324;
constexpr uint32_t kNodeLeft = 0, kNodeParent = 4, kNodeRight = 8, kNodeValue = 16, kNodeNil = 21;
constexpr uint32_t kConnAddress = 56;
constexpr int kMaxNodes = 256;
constexpr uint32_t kManagerCoop = 147;
constexpr uint32_t kStartMenuVtable = 0x82158240;  // the main menu's scene object (StartMenu)
// Sockets.
constexpr uint32_t kCallerTitle = 1;
constexpr uint32_t kAfInet = 2, kSockDgram = 2, kIpprotoUdp = 17;
constexpr uint32_t kFionbio = 0x8004667E;
constexpr uint32_t kBad = 0xFFFFFFFF;
constexpr uint16_t kPort = 1500;                // the title port; each copy of reNut on a PC shifts it
constexpr int kRetryFrames = 300;
constexpr int kMaxPerTick = 64;
// A packet: "RNUT", version, name length, name, data.
constexpr char kMagic[4] = {'R', 'N', 'U', 'T'};
constexpr uint8_t kVersion = 1;
constexpr uint32_t kHeader = 6;
constexpr uint32_t kMaxPacket = 1024;
constexpr size_t kMaxName = 64;
// Guest scratch: the packet, a sockaddr, its length.
constexpr uint32_t kScratchAddress = kMaxPacket, kScratchLength = kMaxPacket + 16;
constexpr uint32_t kScratchSize = kMaxPacket + 32;
constexpr uint32_t kWorldGameData = 5552;       // a world's game data; 0 once it's torn down

struct Outgoing {
  uint32_t to;  // a peer, or 0 for every one
  std::vector<uint8_t> packet;
};

struct Incoming {
  uint32_t from;
  std::string name;
  std::string data;
};

std::mutex g_mutex;
std::vector<Outgoing> g_outgoing;
// As of the last tick, for the Lua functions on any thread.
std::vector<uint32_t> g_peers;
int g_state = 0;
bool g_host = false;
bool g_multiplayer = false;

std::atomic<uint32_t> g_scratch{0};
uint32_t g_socket = kBad;
int g_retry = 0;
std::optional<rexfabric::Event> g_message;
std::optional<rexfabric::Event> g_tick;

uint32_t SessionState(const uint8_t *base, uint32_t global) {
  const uint32_t session = Load32(base, global);
  return session ? Load32(base, session + kSessionState) : 0;
}

bool SessionHost(const uint8_t *base, uint32_t global) {
  const uint32_t session = Load32(base, global);
  if (!session || Load32(base, session + kSessionState) != kStateUp)
    return false;
  const uint32_t local = Load32(base, session + kSessionLocal);
  return local && base[local + kPeerHost];
}

// Every connection's address (the game's connections, the lobby's and the match's).
std::vector<uint32_t> Peers(const uint8_t *base) {
  std::vector<uint32_t> peers;
  const uint32_t head = Load32(base, kConnections);
  if (!head)
    return peers;
  std::vector<uint32_t> nodes{Load32(base, head + kNodeParent)};
  for (int n = 0; !nodes.empty() && n < kMaxNodes; ++n) {
    const uint32_t node = nodes.back();
    nodes.pop_back();
    if (!node || node == head || base[node + kNodeNil])
      continue;
    nodes.push_back(Load32(base, node + kNodeLeft));
    nodes.push_back(Load32(base, node + kNodeRight));
    const uint32_t connection = Load32(base, node + kNodeValue);
    const uint32_t address = connection ? Load32(base, connection + kConnAddress) : 0;
    if (address && std::find(peers.begin(), peers.end(), address) == peers.end())
      peers.push_back(address);
  }
  return peers;
}

void WriteAddress(uint8_t *base, uint32_t at, uint32_t address) {
  Store16(base, at, uint16_t(kAfInet));
  Store16(base, at + 2, kPort);
  Store32(base, at + 4, address);
  std::memset(base + at + 8, 0, 8);
}

// reNut's own socket, made once the network is up; tried again now and then while it can't be.
bool OpenSocket(renut::guest::Guest &g, uint8_t *base) {
  if (g_socket != kBad)
    return true;
  if (g_retry > 0) {
    --g_retry;
    return false;
  }
  g_retry = kRetryFrames;
  auto &f = g.ctx();
  const uint32_t socket = NetSocket(f, base, kCallerTitle, kAfInet, kSockDgram, kIpprotoUdp);
  if (socket == kBad)
    return false;
  const uint32_t on = g.Alloc(4);
  Store32(base, on, 1);
  NetIoctl(f, base, kCallerTitle, socket, kFionbio, on);
  const uint32_t address = g.Alloc(16);
  WriteAddress(base, address, 0);
  if (NetBind(f, base, kCallerTitle, socket, address, 16) != 0) {
    NetClose(f, base, kCallerTitle, socket);
    return false;
  }
  g_socket = socket;
  return true;
}

void Send(renut::guest::Guest &g, uint8_t *base, const std::vector<uint32_t> &peers, const std::vector<Outgoing> &out) {
  auto &f = g.ctx();
  const uint32_t scratch = g_scratch.load();
  for (const Outgoing &packet : out) {
    std::memcpy(base + scratch, packet.packet.data(), packet.packet.size());
    for (const uint32_t peer : peers) {
      if (packet.to && packet.to != peer)
        continue;
      WriteAddress(base, scratch + kScratchAddress, peer);
      NetSendTo(f, base, kCallerTitle, g_socket, scratch, uint32_t(packet.packet.size()), 0, scratch + kScratchAddress,
                16);
    }
  }
}

std::vector<Incoming> Receive(renut::guest::Guest &g, uint8_t *base) {
  std::vector<Incoming> in;
  auto &f = g.ctx();
  const uint32_t scratch = g_scratch.load();
  for (int i = 0; i < kMaxPerTick; ++i) {
    Store32(base, scratch + kScratchLength, 16);
    const int32_t n = int32_t(NetRecvFrom(f, base, kCallerTitle, g_socket, scratch, kMaxPacket, 0,
                                          scratch + kScratchAddress, scratch + kScratchLength));
    if (n <= 0)
      break;
    const uint8_t *p = base + scratch;
    if (uint32_t(n) < kHeader || std::memcmp(p, kMagic, 4) != 0 || p[4] != kVersion)
      continue;
    const uint32_t name_length = p[5];
    if (kHeader + name_length > uint32_t(n))
      continue;
    in.push_back({Load32(base, scratch + kScratchAddress + 4),
                  std::string(reinterpret_cast<const char *>(p + kHeader), name_length),
                  std::string(reinterpret_cast<const char *>(p + kHeader + name_length), n - kHeader - name_length)});
  }
  return in;
}

// From a main menu row's on_select: as the Multiplayer page's choice (co-op off, as the game's only route has it:
// its co-op parties fail); the save's multiplayer data loads (or is made), then the front end.
void OpenMultiplayer(bool live) {
  const renut::game::menu_api::Press *press = renut::game::menu_api::CurrentPress();
  if (!press || !press->scene || Load32(press->base, press->scene) != kStartMenuVtable)
    throw Error("renut.net.open_multiplayer works from a main menu row's on_select (renut.menu.add)");
  uint8_t *base = press->base;
  renut::guest::Guest g(*press->ctx, base);
  const uint32_t manager = MultiplayerManager(g.ctx(), base);
  if (!manager)
    throw Error("no multiplayer manager yet");
  base[manager + kManagerCoop] = 0;
  StartMultiplayer(g.ctx(), base, press->scene, live ? 1 : 0);
}

std::vector<uint8_t> Packet(const std::string &name, const std::string &data) {
  if (name.empty() || name.size() > kMaxName)
    throw Error("a message's name is 1-64 characters");
  if (kHeader + name.size() + data.size() > kMaxPacket)
    throw Error("a message is at most " + std::to_string(kMaxPacket - kHeader - name.size()) + " bytes with that name");
  std::vector<uint8_t> packet(kMagic, kMagic + 4);
  packet.push_back(kVersion);
  packet.push_back(uint8_t(name.size()));
  packet.insert(packet.end(), name.begin(), name.end());
  packet.insert(packet.end(), data.begin(), data.end());
  return packet;
}

// renut.net: values sent as text (nil, booleans, numbers, strings and tables of them), listeners by name.
constexpr const char kLua[] = R"lua(
local net = renut.net

local function encode(value, out)
  local t = type(value)
  if t == "nil" then out[#out + 1] = "z"
  elseif t == "boolean" then out[#out + 1] = value and "T" or "F"
  elseif t == "number" then out[#out + 1] = "n" .. string.format("%.17g", value) .. ";"
  elseif t == "string" then out[#out + 1] = "s" .. #value .. ":" .. value
  elseif t == "table" then
    local n = 0
    for _ in pairs(value) do n = n + 1 end
    out[#out + 1] = "t" .. n .. ";"
    for k, v in pairs(value) do
      encode(k, out)
      encode(v, out)
    end
  else
    error("renut.net.send: a " .. t .. " can't be sent", 3)
  end
end

local function decode(text, at, depth)
  if depth > 32 then error("too deep") end
  local tag = text:sub(at, at)
  if tag == "z" then return nil, at + 1 end
  if tag == "T" then return true, at + 1 end
  if tag == "F" then return false, at + 1 end
  if tag == "n" then
    local stop = assert(text:find(";", at, true), "bad number")
    return assert(tonumber(text:sub(at + 1, stop - 1)), "bad number"), stop + 1
  end
  if tag == "s" then
    local colon = assert(text:find(":", at, true), "bad string")
    local length = assert(math.tointeger(tonumber(text:sub(at + 1, colon - 1))), "bad string")
    assert(length >= 0 and colon + length <= #text, "bad string")
    return text:sub(colon + 1, colon + length), colon + length + 1
  end
  if tag == "t" then
    local stop = assert(text:find(";", at, true), "bad table")
    local count = assert(math.tointeger(tonumber(text:sub(at + 1, stop - 1))), "bad table")
    local result, pos = {}, stop + 1
    for _ = 1, count do
      local k, v
      k, pos = decode(text, pos, depth + 1)
      v, pos = decode(text, pos, depth + 1)
      if k ~= nil then result[k] = v end
    end
    return result, pos
  end
  error("bad value")
end

function net.send(name, value, peer)
  assert(type(name) == "string" and #name > 0 and #name <= 64, "renut.net.send: the message's name is 1-64 characters")
  assert(peer == nil or math.type(peer) == "integer", "renut.net.send: peer is one of renut.net.peers()")
  local out = {}
  encode(value, out)
  net._send(peer or 0, name, table.concat(out))
end

function net.on(name, fn)
  assert(type(name) == "string", "renut.net.on: the message's name is a string")
  assert(type(fn) == "function", "renut.net.on: fn is a function")
  return rex.on("renut.net_message", function(peer, got, data)
    if got ~= name then return end
    local ok, value = pcall(decode, data, 1, 0)
    if ok then fn(value, peer) end
  end)
end

function net.on_tick(fn)
  assert(type(fn) == "function", "renut.net.on_tick: fn is a function")
  return rex.on("renut.net_tick", fn)
end

function net.peers()
  local list = {}
  for id in net._peers():gmatch("%d+") do list[#list + 1] = math.tointeger(tonumber(id)) end
  return list
end

local STATES = {[0] = "off", "lobby", "match"}
function net.state() return STATES[net._state()] end
function net.is_host() return net._is_host() end
function net.multiplayer() return net._multiplayer() end

function net.open_multiplayer(opts)
  opts = opts or {}
  assert(type(opts) == "table", "renut.net.open_multiplayer: options are a table")
  net._open_multiplayer(opts.live ~= false)
end
)lua";

} // namespace

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("net.", "Online");  // the node editor's add menu
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"net._send", {"peer", "name", "data"}},
           {"net._peers", {}},
           {"net._state", {}},
           {"net._is_host", {}},
           {"net._multiplayer", {}},
           {"net._open_multiplayer", {"live"}},
           {"net_message", {"peer", "name", "data"}},
           {"net_tick", {}}})
    renut.Args(name, args);  // the node editor's pins

  renut.Function("net._send", [](int64_t peer, std::string name, std::string data) {
    std::vector<uint8_t> packet = Packet(name, data);
    std::lock_guard lock(g_mutex);
    g_outgoing.push_back({uint32_t(peer), std::move(packet)});
  });
  renut.Function("net._peers", []() -> std::string {
    std::lock_guard lock(g_mutex);
    std::string list;
    for (const uint32_t peer : g_peers)
      list += std::to_string(peer) + "\n";
    return list;
  });
  renut.Function("net._state", []() -> int64_t {
    std::lock_guard lock(g_mutex);
    return g_state;
  });
  renut.Function("net._is_host", []() -> bool {
    std::lock_guard lock(g_mutex);
    return g_host;
  });
  renut.Function("net._multiplayer", []() -> bool {
    std::lock_guard lock(g_mutex);
    return g_multiplayer;
  });
  renut.Function("net._open_multiplayer", [](bool live) { OpenMultiplayer(live); });
  g_message = renut.Event("net_message");
  g_tick = renut.Event("net_tick");
  renut.Lua(kLua);
}

void Tick(PPCContext &ctx, uint8_t *base) {
  if (!g_scratch.load()) {
    const uint32_t scratch = REX_RUNTIME()->GetGuestHeap().SystemHeapAlloc(kScratchSize);
    if (!scratch)
      return;
    g_scratch.store(scratch);
  }
  const bool multiplayer = base[kMultiplayer] != 0;
  const uint32_t game = SessionState(base, kGameSession);
  const uint32_t party = SessionState(base, kPartySession);
  const int state = game == kStateUp ? 2 : party == kStateUp ? 1 : 0;
  const bool host = state == 2 ? SessionHost(base, kGameSession) : state == 1 && SessionHost(base, kPartySession);
  const std::vector<uint32_t> peers = state ? Peers(base) : std::vector<uint32_t>{};
  std::vector<Outgoing> out;
  {
    std::lock_guard lock(g_mutex);
    g_state = state;
    g_host = host;
    g_multiplayer = multiplayer;
    g_peers = peers;
    out.swap(g_outgoing);
  }

  std::vector<Incoming> in;
  {
    renut::guest::Guest g(ctx, base);
    if (base[kNetReady] && OpenSocket(g, base)) {
      if (!peers.empty())
        Send(g, base, peers, out);
      in = Receive(g, base);
    }
  }
  for (const Incoming &message : in) {
    if (g_message)
      g_message->Fire(int64_t(message.from), message.name, message.data);
  }
  if (g_tick)
    g_tick->Fire();

  // What the listeners sent goes out this frame.
  {
    std::lock_guard lock(g_mutex);
    out.clear();
    out.swap(g_outgoing);
  }
  if (!out.empty() && g_socket != kBad && !peers.empty()) {
    renut::guest::Guest g(ctx, base);
    Send(g, base, peers, out);
  }
}

} // namespace renut::online::net_api

// A message to a world being torn down (its game data gone), such as a player leaving it while it unloads, is
// dropped instead of read through null.
REX_HOOK_RAW(sub_82513DB8) {
  if (!renut::guest::Load32(base, ctx.r3.u32 + renut::online::net_api::kWorldGameData)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82513DB8(ctx, base);
}

// A co-op party sets "Game Mode" to "Co op", a value no session registers (sub_824BB1B8 has Single Player and
// Multiplayer): a context or value that isn't there is skipped instead of read through null.
REX_HOOK_RAW(sub_826A2DA8) {
  bool found;
  {
    // Closed before the original runs, so it gets the game's own r1.
    renut::guest::Guest g(ctx, base);
    const uint32_t context = FindContext(g.ctx(), base, ctx.r3.u32, ctx.r5.u32);
    found = context && FindContextValue(g.ctx(), base, context, ctx.r6.u32);
  }
  if (!found) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_826A2DA8(ctx, base);
}
