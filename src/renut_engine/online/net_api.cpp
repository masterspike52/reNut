#include "renut_engine/online/net_api.h"

#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rexfabric/game_api.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "renut_engine/game/guest_call.h"
#include "renut_engine/game/menu_api.h"
#include "renut_engine/mods/custom_parts.h"
#include "renut_engine/online/coop_challenge.h"

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
// Showdown Town as LoadShowdownTown (sub_82548080) loads it: a time of day's scene from timeofdaysettings.
REX_IMPORT(__imp__rex_assetManOpenParams, OpenAsset, u32(u32, u32));  // (&aid, 0): its data
REX_IMPORT(__imp__sub_82204DF0, ReleaseAsset, void(u32));            // (data)
REX_IMPORT(sub_823EA9F0, PickTownScene, u32(u32));                    // (entries): one, renut_time_of_day's
// Hooked: the scene a match loads (StartLoad sub_824B15C8) and unloads (EndGame sub_824B1E48).
REX_EXTERN(__imp__sub_824B2158);
// Hooked: the match's Initialise (game object vtable [19]): in co-op the host adds the record's AI teammates.
REX_EXTERN(__imp__sub_824B1468);
// Hooked: whether a start marker (type 21) is a player's in a match (world, profile, marker, &Player): the one
// whose index (+ 52) is the start slot the match gave the player (Player + 116), for each machine's own players.
REX_EXTERN(__imp__sub_82549318);
REX_IMPORT(__imp__sub_82285DC8, PlayerIndex, u32(u32));       // (profile): its player index
REX_IMPORT(__imp__sub_82357FB8, PlayerObject, u32(u32));      // (player index): its Player net object
// Hooked: in a match, this machine's session member a player start is for (start, &member): the one whose slot
// (+ 112) is the start's index (+ 52). Story worlds spawn Banjo through it (sub_824F40E8, sub_824F4940).
REX_EXTERN(__imp__sub_82549250);
// Hooked: a scripted sequence's camera step (step, sequence, info, camera), through its player's HUD.
REX_EXTERN(__imp__sub_823EBC80);
REX_IMPORT(__imp__sub_82212D18, PlayerProfile, u32(u32, u32));                    // (player, 0)
REX_IMPORT(__imp__sub_82247D30, HudByHandle, u32(u32, u32));                       // (UI + 72's, handle)
REX_IMPORT(__imp__sub_82260128, HudOfProfile, u32(u32, u32, u32));                 // (UI + 80's, profile, sequence)
// Hooked: a session context set by names (session, user, &context, &value); the lookups it doesn't check.
REX_EXTERN(__imp__sub_826A2DA8);
REX_IMPORT(__imp__sub_826A2E30, FindContext, u32(u32, u32));       // (session, &name): its context, or 0
REX_IMPORT(__imp__sub_826A3010, FindContextValue, u32(u32, u32));  // (context, &name): its value, or 0
// The game's save, as gameSaveSave writes it and its load applies it (sub_824E8570).
REX_IMPORT(__imp__sub_824E8F30, SaveSize, u32());
REX_IMPORT(__imp__sub_824E9148, WriteSave, u32(u32, u32));        // (profile, buffer)
REX_IMPORT(__imp__sub_824E9410, ReadSave, u32(u32, u32, u32));    // (profile, data, size): whether it took
REX_IMPORT(__imp__sub_82286E48, ApplyLoadedSave, u32(u32));       // (player 1's profile), after a load
// Hooked: a scene change (&aid, arg), as a world's door or act menu makes it: every loaded world ends (the
// multiplayer lobby's too), an autosave, then the scene is pushed with no parent world.
REX_EXTERN(__imp__sub_82289810);
REX_IMPORT(__imp__sub_82289640, EndWorld, u32(u32, u32));  // (world, 0): it and its child worlds end
REX_IMPORT(__imp__rex_appScenePush, PushScene, u32(u32, u32, u32, u32, u32, u32));  // (parent, &aid, kind, 1, arg, screen)
REX_IMPORT(rex_gameSaveSave, Autosave, u32(u32));          // (profile), part_api's hook and its game_save
// Hooked: another machine's Banjo and vehicle made here from their net objects (retried while they return 0).
REX_EXTERN(__imp__sub_823D1AD8);
REX_EXTERN(__imp__sub_825F0368);
// Hooked: a world's messages (world, &message), through its game data (+ 5552).
REX_EXTERN(__imp__sub_82513DB8);
REX_IMPORT(__imp__rex_meInternalAlloc, GuestAlloc, u32(u32, u32));  // (size, 0)
REX_IMPORT(__imp__rex_meInternalFree, GuestFree, void(u32));

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
constexpr uint8_t kTypeScript = 0x19;           // a scene's asset type
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
// Guest scratch: the packet, a sockaddr, its length, the co-op scene's aid.
constexpr uint32_t kScratchAddress = kMaxPacket, kScratchLength = kMaxPacket + 16, kScratchScene = kMaxPacket + 20;
constexpr uint32_t kScratchSize = kMaxPacket + 32;
// The loaded worlds (4 slots: + 0 the parent world, + 4 the world, its + 0 its scene's aid, + 8 set once it
// ends, + 16 its kind: 0 the lobby's, 1 a match's, which StartLoad pushes under the lobby's world).
constexpr uint32_t kWorlds = 0x82E18154, kWorldSize = 32, kWorldCount = 4;
constexpr uint32_t kSlotParent = 0, kSlotWorld = 4, kSlotEnding = 8, kSlotKind = 16, kMatchWorld = 1;
constexpr uint32_t kWorldGameData = 5552;       // a world's game data; 0 once it's torn down
// Where each player is: their Banjo's and vehicle's net object ids (+ 12, 8 bytes, the same on every machine)
// with their world's scene aid, sent now and then; another machine's Banjo or vehicle is made only in that world.
constexpr char kWhereMessage[] = "renut.coop_where";  // aid (u32), then the ids
constexpr uint32_t kWorldAvatars = 2628;        // world: each player's avatar, 8 bytes apart, by profile + 128
constexpr uint32_t kProfileIndex = 128, kAvatarVehicle = 3132, kObjectNet = 32;
constexpr uint32_t kNetId = 12, kNetIdSize = 8, kNetIdValid = 19;
constexpr int kWhereSendFrames = 30;
constexpr int kUnclaimedFrames = 90;  // how long a vehicle waits for a player's "where" before it's the host's own
constexpr uint32_t kSignedIn = 0x82E1B2F6;      // a scene change autosaves (sub_82289810): player 1 signed in,
constexpr uint32_t kProfileUnsaved = 324;       // with progress not saved
// Scenes out of a world (frontend, retail_mainmenu_scene, start_intro, garage, multiplayer_frontend's aids).
constexpr uint32_t kNotWorlds[] = {0x82E10200, 0x82E10204, 0x82E1020C, 0x82E10210, 0x82E10218};

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

std::atomic<uint32_t> g_coop_scene{0};  // the scene a match loads instead of its own; 0 for its own
std::atomic<uint32_t> g_scratch{0};
uint32_t g_socket = kBad;
int g_retry = 0;
bool g_was_multiplayer = false;
std::optional<rexfabric::Event> g_message;
std::optional<rexfabric::Event> g_tick;

// The co-op save: the host's own progress, which the others play while a co-op world's match is on, their own
// set aside in memory until they leave multiplayer. All on the game thread.
constexpr uint32_t kProfiles = 0x82E1B280;  // player 1's profile
constexpr char kSaveMessage[] = "renut.coop_save";  // version, size, offset (u32 each), then the bytes
constexpr uint32_t kSaveHeader = 12;
constexpr uint32_t kSaveChunk = 900;
constexpr uint32_t kMaxSave = 4u << 20;
constexpr int kSaveSendFrames = 120;
std::vector<uint8_t> g_coop_save;  // the host's progress, as sent or as a guest got it
uint32_t g_coop_version = 0;
std::vector<uint8_t> g_own_save;
bool g_coop_active = false;
int g_save_frames = 0;
uint32_t g_incoming_version = 0;  // a guest's copy, chunk by chunk
std::vector<uint8_t> g_incoming;
std::vector<bool> g_incoming_have;
size_t g_incoming_count = 0;
// Where the others are, by their net object ids; what this machine last sent. All on the game thread.
std::map<uint64_t, uint32_t> g_where;
std::map<uint32_t, std::vector<uint64_t>> g_where_ids;  // by sender
std::string g_where_sent;
int g_where_frames = 0;
std::map<uint64_t, int> g_unclaimed;  // a vehicle no player has claimed: the frame it was first tried
int g_frame = 0;

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

uint32_t SaveVersion(const std::vector<uint8_t> &bytes) {
  uint32_t hash = 2166136261u;
  for (const uint8_t b : bytes)
    hash = (hash ^ b) * 16777619u;
  return hash ? hash : 1;
}

// The profile's progress as the game saves it (gameSaveSave's buffer), or nothing.
std::vector<uint8_t> SerializeSave(renut::guest::Guest &g, uint8_t *base, uint32_t profile) {
  auto &f = g.ctx();
  const uint32_t size = SaveSize(f, base);
  const uint32_t buffer = size && size <= kMaxSave ? GuestAlloc(f, base, size, 0) : 0;
  if (!buffer)
    return {};
  std::memset(base + buffer, 0, size);
  std::vector<uint8_t> bytes;
  if (WriteSave(f, base, profile, buffer) & 0xFF)
    bytes.assign(base + buffer, base + buffer + size);
  GuestFree(f, base, buffer);
  return bytes;
}

// A save's progress into the profile, as the game's load does; false when it didn't take.
bool ApplySave(renut::guest::Guest &g, uint8_t *base, uint32_t profile, const std::vector<uint8_t> &bytes) {
  if (bytes.empty())
    return false;
  auto &f = g.ctx();
  const uint32_t buffer = GuestAlloc(f, base, uint32_t(bytes.size()), 0);
  if (!buffer)
    return false;
  std::memcpy(base + buffer, bytes.data(), bytes.size());
  const bool took = (ReadSave(f, base, profile, buffer, uint32_t(bytes.size())) & 0xFF) != 0;
  GuestFree(f, base, buffer);
  if (took && profile == Load32(base, kProfiles))
    ApplyLoadedSave(f, base, profile);
  return took;
}

// The host's progress as it is now, to send.
void TakeHostSave(renut::guest::Guest &g, uint8_t *base) {
  const uint32_t profile = Load32(base, kProfiles);
  std::vector<uint8_t> bytes = profile ? SerializeSave(g, base, profile) : std::vector<uint8_t>{};
  if (bytes.empty())
    return;
  g_coop_save = std::move(bytes);
  g_coop_version = SaveVersion(g_coop_save);
}

// The co-op save to every player, chunk by chunk.
void SendSave() {
  const uint32_t total = uint32_t(g_coop_save.size());
  std::lock_guard lock(g_mutex);
  for (uint32_t offset = 0; offset < total; offset += kSaveChunk) {
    const uint32_t length = std::min(kSaveChunk, total - offset);
    std::string data(kSaveHeader, '\0');
    std::memcpy(data.data(), &g_coop_version, 4);
    std::memcpy(data.data() + 4, &total, 4);
    std::memcpy(data.data() + 8, &offset, 4);
    data.append(reinterpret_cast<const char *>(g_coop_save.data() + offset), length);
    g_outgoing.push_back({0, Packet(kSaveMessage, data)});
  }
}

// A chunk of the host's co-op save; the whole of it is the save a guest's matches take.
void TakeSaveChunk(const std::string &data) {
  if (data.size() < kSaveHeader)
    return;
  uint32_t version, total, offset;
  std::memcpy(&version, data.data(), 4);
  std::memcpy(&total, data.data() + 4, 4);
  std::memcpy(&offset, data.data() + 8, 4);
  if (!total || total > kMaxSave || offset >= total || offset % kSaveChunk)
    return;
  if (version != g_incoming_version || g_incoming.size() != total) {
    g_incoming_version = version;
    g_incoming.assign(total, 0);
    g_incoming_have.assign((total + kSaveChunk - 1) / kSaveChunk, false);
    g_incoming_count = 0;
  }
  const size_t chunk = offset / kSaveChunk;
  if (g_incoming_have[chunk])
    return;
  const size_t length = std::min<size_t>(data.size() - kSaveHeader, total - offset);
  std::memcpy(g_incoming.data() + offset, data.data() + kSaveHeader, length);
  g_incoming_have[chunk] = true;
  if (++g_incoming_count == g_incoming_have.size() && version != g_coop_version) {
    g_coop_save = g_incoming;
    g_coop_version = version;
  }
}

// A co-op world's match loads: a guest's own save set aside, the host's progress in. The host plays its own.
void BeginCoopSave(PPCContext &ctx, uint8_t *base) {
  const uint32_t profile = Load32(base, kProfiles);
  if (g_coop_active || !profile || g_coop_save.empty())
    return;
  {
    std::lock_guard lock(g_mutex);
    if (g_host)
      return;
  }
  renut::guest::Guest g(ctx, base);
  g_own_save = SerializeSave(g, base, profile);
  if (g_own_save.empty())
    return;
  ApplySave(g, base, profile, g_coop_save);
  g_coop_active = true;
}

// Out of multiplayer: the player's own save back.
void EndCoopSave(PPCContext &ctx, uint8_t *base) {
  if (g_coop_active) {
    const uint32_t profile = Load32(base, kProfiles);
    renut::guest::Guest g(ctx, base);
    if (profile)
      ApplySave(g, base, profile, g_own_save);
  }
  g_coop_active = false;
  g_own_save.clear();
  g_coop_save.clear();
  g_coop_version = 0;
  g_incoming_version = 0;
  g_incoming.clear();
  g_incoming_have.clear();
  g_incoming_count = 0;
}

// Playing co-op: a co-op world set, in multiplayer.
bool CoopOn(const uint8_t *base) {
  return g_coop_scene.load() && base[kMultiplayer];
}

// The slot of the match's world (the co-op world, under the lobby's), or 0.
uint32_t MatchWorldSlot(const uint8_t *base) {
  for (uint32_t i = 0; i < kWorldCount; ++i) {
    const uint32_t slot = kWorlds + i * kWorldSize;
    if (Load32(base, slot + kSlotWorld) && !Load32(base, slot + kSlotEnding) &&
        Load32(base, slot + kSlotKind) == kMatchWorld)
      return slot;
  }
  return 0;
}

// The co-op world this machine is in (its scene's aid), or 0.
uint32_t CoopWorldAid(const uint8_t *base) {
  const uint32_t slot = MatchWorldSlot(base);
  const uint32_t world = slot ? Load32(base, slot + kSlotWorld) : 0;
  return world ? Load32(base, world) : 0;
}

// A net object's id (+ 12: three u16s, a byte, valid), or 0 when it has none.
uint64_t NetId(const uint8_t *base, uint32_t net) {
  if (!net || !base[net + kNetIdValid])
    return 0;
  uint64_t id;
  std::memcpy(&id, base + net + kNetId, kNetIdSize);
  return id;
}

// "Where": this machine's co-op world, its Banjo's and vehicle's net object ids; to everyone when it changes and
// now and then (a player who joins late hears it too).
void SendWhere(const uint8_t *base) {
  const uint32_t aid = CoopWorldAid(base);
  const uint32_t slot = MatchWorldSlot(base);
  const uint32_t world = slot ? Load32(base, slot + kSlotWorld) : 0;
  const uint32_t profile = Load32(base, kProfiles);
  std::string data(4, '\0');
  std::memcpy(data.data(), &aid, 4);
  if (world && profile) {
    const uint32_t avatar = Load32(base, world + kWorldAvatars + 8 * Load32(base, profile + kProfileIndex));
    for (const uint32_t object : {avatar, avatar ? Load32(base, avatar + kAvatarVehicle) : 0u})
      if (const uint64_t id = object ? NetId(base, Load32(base, object + kObjectNet)) : 0)
        data.append(reinterpret_cast<const char *>(&id), kNetIdSize);
  }
  if (data == g_where_sent && ++g_where_frames < kWhereSendFrames)
    return;
  g_where_frames = 0;
  g_where_sent = data;
  std::lock_guard lock(g_mutex);
  g_outgoing.push_back({0, Packet(kWhereMessage, data)});
}

// Another player's "where": their ids now map to their world.
void TakeWhere(uint32_t from, const std::string &data) {
  if (data.size() < 4 || (data.size() - 4) % kNetIdSize)
    return;
  uint32_t aid;
  std::memcpy(&aid, data.data(), 4);
  for (const uint64_t id : g_where_ids[from])
    g_where.erase(id);
  std::vector<uint64_t> &ids = g_where_ids[from];
  ids.clear();
  for (size_t at = 4; at < data.size(); at += kNetIdSize) {
    uint64_t id;
    std::memcpy(&id, data.data() + at, kNetIdSize);
    ids.push_back(id);
    g_where[id] = aid;
  }
}

// Whether another machine's Banjo or vehicle (its net object) may be made here: in co-op, once its owner has said
// they're in this machine's world. A challenge's object only for its group; other vehicles after a short wait.
bool ProxyHere(const uint8_t *base, uint32_t net, bool player) {
  if (!CoopOn(base))
    return true;
  const uint32_t aid = CoopWorldAid(base);
  const uint64_t id = NetId(base, net);
  if (const auto where = g_where.find(id); where != g_where.end())
    return aid && where->second == aid;
  if (const int verdict = coop_challenge::ProxyVerdict(id, aid); verdict >= 0)
    return verdict != 0;
  if (player || !id || coop_challenge::GroupInWorld(aid))
    return false;
  const auto first = g_unclaimed.try_emplace(id, g_frame).first;
  return g_frame - first->second >= kUnclaimedFrames;
}

// A world or Showdown Town, not the menus or the garage.
bool WorldScene(const uint8_t *base, uint32_t aid) {
  for (const uint32_t scene : kNotWorlds)
    if (Load32(base, scene) == aid)
      return false;
  return aid != 0;
}

// A scene change in a co-op world as the game makes one, but only the match's world ends: the lobby's stays and
// is the new one's parent, as StartLoad pushes it, so the session stays up. A guest's borrowed save isn't saved.
void CoopChangeScene(PPCContext &ctx, uint8_t *base, uint32_t slot, uint32_t aid_at, uint32_t arg) {
  const uint32_t lobby = Load32(base, slot + kSlotParent);
  const uint32_t world = Load32(base, slot + kSlotWorld);
  const uint32_t aid = Load32(base, aid_at);
  const uint32_t profile = Load32(base, kProfiles);
  renut::guest::Guest g(ctx, base);
  auto &f = g.ctx();
  if (!g_coop_active && base[kSignedIn] && profile && base[profile + kProfileUnsaved])
    Autosave(f, base, profile);
  EndWorld(f, base, world, 0);
  PushScene(f, base, lobby, aid_at, kMatchWorld, 1, arg, 0);
  g_coop_scene.store(aid);
}

// The game's asset refs by name ("hubworld", "multiplayer_frontend"), as sub_82223868 finds them: a list
// (+ 4 next, + 8 count, + 12 entries) of 68-byte entries, the name then the aid at + 64. 0 when there's none.
uint32_t AssetRefAid(const std::string &name) {
  constexpr uint32_t kRefs = 0x82DF1CD8, kRefNext = 4, kRefCount = 8, kRefEntries = 12;
  constexpr uint32_t kEntrySize = 68, kEntryAid = 64, kMaxLists = 4096;
  const uint8_t *base = REX_RUNTIME()->GetMembase().TranslateVirtual<uint8_t *>(0);
  if (name.empty() || name.size() >= kEntryAid)
    return 0;
  uint32_t list = Load32(base, kRefs);
  for (uint32_t n = 0; list && n < kMaxLists; ++n, list = Load32(base, list + kRefNext)) {
    const uint32_t count = Load32(base, list + kRefCount);
    const uint32_t entries = Load32(base, list + kRefEntries);
    for (uint32_t i = 0; entries && i < count; ++i) {
      const uint32_t entry = entries + kEntrySize * i;
      if (std::memcmp(base + entry, name.c_str(), name.size() + 1) == 0)
        return Load32(base, entry + kEntryAid);
    }
  }
  return 0;
}

// Showdown Town's scene as the game loads it: a time of day's (timeofdaysettings, 80-byte entries, + 0 the
// scene), which loads its sky and background before the town. Needs a press's context (a row's on_select).
uint32_t TownScene() {
  constexpr uint32_t kTimeOfDaySettings = 0x82E10214;  // its aid, set by sub_82547BA0
  const renut::game::menu_api::Press *press = renut::game::menu_api::CurrentPress();
  if (!press)
    throw Error("renut.net.coop_world(\"showdowntown\") works from a row's on_select (renut.menu.add)");
  uint8_t *base = press->base;
  if (!Load32(base, kTimeOfDaySettings))
    throw Error("the game hasn't found its time of day settings yet");
  renut::guest::Guest g(*press->ctx, base);
  const uint32_t data = OpenAsset(g.ctx(), base, kTimeOfDaySettings, 0);
  if (!data)
    throw Error("the game's time of day settings didn't open");
  const uint32_t entry = PickTownScene(g.ctx(), base, data);
  const uint32_t scene = entry ? Load32(base, entry) : 0;
  ReleaseAsset(g.ctx(), base, data);
  if (!scene)
    throw Error("no time of day of Showdown Town to load");
  return scene;
}

// A scene: "showdowntown" (as the game loads it), the game's asset ref name, a script name ("aid_script_..."),
// or its aid ("0x19ABCDEF").
uint32_t SceneAid(std::string scene) {
  if (scene.size() > 2 && scene[0] == '0' && (scene[1] == 'x' || scene[1] == 'X'))
    return uint32_t(std::strtoul(scene.c_str() + 2, nullptr, 16));
  if (scene == "showdowntown")
    return TownScene();
  if (const uint32_t aid = AssetRefAid(scene))
    return aid;
  constexpr std::string_view kPrefix = "aid_script_";
  if (scene.starts_with(kPrefix))
    scene.erase(0, kPrefix.size());
  return renut::mods::AssetId(kTypeScript, scene);
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

function net.coop_world(scene)
  if scene == nil then net._coop_scene("") return end
  if math.type(scene) == "integer" then net._coop_scene(string.format("0x%08X", scene)) return end
  assert(type(scene) == "string" and #scene > 0, "renut.net.coop_world: a scene's name or asset id, or nil")
  net._coop_scene(scene)
end

function net.coop_scene()
  local scene = net._coop_scene_get()
  return scene ~= 0 and scene or nil
end

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
           {"net._coop_scene", {"scene"}},
           {"net._coop_scene_get", {}},
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
  renut.Function("net._coop_scene", [](std::string scene) {
    g_coop_scene.store(scene.empty() ? 0 : SceneAid(std::move(scene)));
  });
  renut.Function("net._coop_scene_get", []() -> int64_t { return g_coop_scene.load(); });
  renut.Function("net._open_multiplayer", [](bool live) { OpenMultiplayer(live); });
  g_message = renut.Event("net_message");
  g_tick = renut.Event("net_tick");
  renut.Lua(kLua);
  coop_challenge::Declare();
}

bool CoopActive(const uint8_t *base) {
  return CoopOn(base);
}

uint32_t CoopWorld(const uint8_t *base) {
  return CoopWorldAid(base);
}

bool Host() {
  std::lock_guard lock(g_mutex);
  return g_host;
}

void Post(uint32_t to, const std::string &name, const std::string &data) {
  std::vector<uint8_t> packet = Packet(name, data);
  std::lock_guard lock(g_mutex);
  g_outgoing.push_back({to, std::move(packet)});
}

uint64_t NetObjectId(const uint8_t *base, uint32_t net) {
  return NetId(base, net);
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
  // Out of multiplayer: the player's own save back, and the next match loads its own scene again.
  if (g_was_multiplayer && !multiplayer) {
    g_coop_scene.store(0);
    EndCoopSave(ctx, base);
    g_where.clear();
    g_where_ids.clear();
    g_where_sent.clear();
    g_unclaimed.clear();
  }
  ++g_frame;
  if (CoopOn(base) && !peers.empty())
    SendWhere(base);
  g_was_multiplayer = multiplayer;
  // The host's progress to everyone, over and over (a player who joins late gets it too).
  if (multiplayer && host && g_coop_scene.load() && !peers.empty() && ++g_save_frames >= kSaveSendFrames) {
    g_save_frames = 0;
    {
      renut::guest::Guest g(ctx, base);
      TakeHostSave(g, base);
    }
    if (!g_coop_save.empty())
      SendSave();
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
    if (message.name == kSaveMessage) {
      if (!host)
        TakeSaveChunk(message.data);
      continue;
    }
    if (message.name == kWhereMessage) {
      TakeWhere(message.from, message.data);
      continue;
    }
    if (coop_challenge::TakeMessage(ctx, base, message.from, message.name, message.data))
      continue;
    if (g_message)
      g_message->Fire(int64_t(message.from), message.name, message.data);
  }
  coop_challenge::NetTick(ctx, base);
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

bool TakeGameSave(PPCContext &, uint8_t *, uint32_t) {
  // Only a guest playing the host's progress is active; its own save isn't written over with it.
  return g_coop_active;
}

} // namespace renut::online::net_api

// The match's scene aid (&): the co-op world renut.net.coop_world set, else the match record's own. A co-op
// world's first load also puts the co-op save in place of the player's own.
REX_HOOK_RAW(sub_824B2158) {
  const uint32_t scene = renut::online::net_api::g_coop_scene.load();
  const uint32_t scratch = renut::online::net_api::g_scratch.load();
  if (scene && scratch) {
    renut::online::net_api::BeginCoopSave(ctx, base);
    const uint32_t slot = scratch + renut::online::net_api::kScratchScene;
    renut::guest::Store32(base, slot, scene);
    ctx.r3.u64 = slot;
    return;
  }
  __imp__sub_824B2158(ctx, base);
}

// A match spawns each machine's own players at the start marker of the slot it gave them (sub_824FCFD0). A story
// world has one start, index 0: in a co-op world it's every player's, as in single player, so all of them spawn.
REX_HOOK_RAW(sub_82549318) {
  using renut::guest::Load16;
  using renut::guest::Load32;
  using renut::guest::Store32;
  constexpr uint32_t kMarkerGroup = 10, kMarkerIndex = 52;
  const uint32_t profile = ctx.r4.u32, marker = ctx.r5.u32, player_out = ctx.r6.u32;
  __imp__sub_82549318(ctx, base);
  if ((ctx.r3.u32 & 0xFF) || !renut::online::net_api::g_coop_scene.load() || !profile || !marker ||
      Load16(base, marker + kMarkerGroup) || Load32(base, marker + kMarkerIndex) != 0)
    return;
  renut::guest::Guest g(ctx, base);
  const uint32_t player = PlayerObject(g.ctx(), base, PlayerIndex(g.ctx(), base, profile));
  if (player_out)
    Store32(base, player_out, player);
  ctx.r3.u64 = 1;
}

// A story world's player start only spawns the member whose match slot is its index, so one machine got a Banjo.
// In a co-op world it takes this machine's member whatever slot the match gave them.
REX_HOOK_RAW(sub_82549250) {
  constexpr uint32_t kStartIndex = 52, kSlots = 16;
  const uint32_t start = ctx.r3.u32, member_out = ctx.r4.u32;
  __imp__sub_82549250(ctx, base);
  if ((ctx.r3.u32 & 0xFF) || !renut::online::net_api::g_coop_scene.load() || !start)
    return;
  const uint32_t index = renut::guest::Load32(base, start + kStartIndex);
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    if (slot == index)
      continue;
    renut::guest::Store32(base, start + kStartIndex, slot);
    ctx.r3.u64 = start;
    ctx.r4.u64 = member_out;
    __imp__sub_82549250(ctx, base);
    if (ctx.r3.u32 & 0xFF)
      break;
  }
  renut::guest::Store32(base, start + kStartIndex, index);
}

// A scene change ends every loaded world, the lobby's under a match too, which left the session. In a co-op
// world only this player's world ends and the new one goes under the lobby's; the others stay where they are.
REX_HOOK_RAW(sub_82289810) {
  namespace net = renut::online::net_api;
  const uint32_t aid_at = ctx.r3.u32, arg = ctx.r4.u32;
  const uint32_t aid = aid_at ? renut::guest::Load32(base, aid_at) : 0;
  const uint32_t slot = net::CoopOn(base) ? net::MatchWorldSlot(base) : 0;
  if (!slot || !net::WorldScene(base, aid)) {
    __imp__sub_82289810(ctx, base);
    return;
  }
  net::CoopChangeScene(ctx, base, slot, aid_at, arg);
  ctx.r3.u64 = 1;
}

// (net object): another machine's Banjo made here, in this machine's match world. In co-op only while its owner
// is in the same world; until then 0, which the game retries (its "ready" check sub_8236ADB0).
REX_HOOK_RAW(sub_823D1AD8) {
  if (!renut::online::net_api::ProxyHere(base, ctx.r3.u32, true)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823D1AD8(ctx, base);
}

// (net object): another machine's vehicle made here, the same way.
REX_HOOK_RAW(sub_825F0368) {
  if (!renut::online::net_api::ProxyHere(base, ctx.r3.u32, false)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_825F0368(ctx, base);
}

// A message to a world being torn down (its game data gone), such as a player leaving it while it unloads, is
// dropped instead of read through null.
REX_HOOK_RAW(sub_82513DB8) {
  if (!renut::guest::Load32(base, ctx.r3.u32 + renut::online::net_api::kWorldGameData)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82513DB8(ctx, base);
}

// A sequence's camera step for a player with no HUD on this machine (another machine's player): in a co-op
// world it's skipped, instead of writing through null; the caller then ends the step for want of a HUD.
REX_HOOK_RAW(sub_823EBC80) {
  using renut::guest::Load32;
  constexpr uint32_t kStepPlayer = 100, kStepByHandle = 997, kStepHandle = 992;
  constexpr uint32_t kUi = 0x82E19A60, kUiHandles = 72, kUiHuds = 80, kHudCamera = 232;
  const uint32_t step = ctx.r3.u32;
  const uint32_t ui = Load32(base, kUi);
  if (renut::online::net_api::g_coop_scene.load() && step && ui) {
    renut::guest::Guest g(ctx, base);
    auto &f = g.ctx();
    uint32_t hud;
    if (base[step + kStepByHandle]) {
      hud = HudByHandle(f, base, Load32(base, ui + kUiHandles), Load32(base, step + kStepHandle));
    } else {
      const uint32_t profile = PlayerProfile(f, base, Load32(base, step + kStepPlayer), 0);
      hud = HudOfProfile(f, base, Load32(base, ui + kUiHuds), profile, ctx.r4.u32);
    }
    if (!hud || !Load32(base, hud + kHudCamera)) {
      ctx.r3.u64 = 0;  // its caller (sub_823EE4E8) ignores this, then finds no HUD and ends the step
      return;
    }
  }
  __imp__sub_823EBC80(ctx, base);
}

// No AI teammates in a co-op world: only the players. Their profiles have no + 284, which a world's load
// (sub_824F1CE0, through every profile) reads through.
REX_HOOK_RAW(sub_824B1468) {
  if (renut::online::net_api::g_coop_scene.load()) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_824B1468(ctx, base);
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
