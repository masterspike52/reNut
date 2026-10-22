/**
 * @file    online/coop.cpp
 * @brief   Co-op in Showdown Town on N&B's own multiplayer.
 *
 * N&B (IDA): an online match starts by pushing its game entry's scene script
 * (sub_824D3B48: the script from sub_824D46D8, e.g.
 * aid_script_banjox_live_testtrack_airbrawl, then appScenePush_822BBFE8, which
 * copies the script's asset id into the pending scene dword_82FAB038). While
 * the match runs the game is in multiplayer (byte_82FAC650) on the in-game
 * session ("Slave", dword_82E5EDE0, set up by sub_824D6A90): the networked
 * entities get network objects at level start (sub_8221CCB0), each player's
 * Banjo, vehicle, blocks, weapons and shots are replicated (NetObj types
 * registered from 0x82FAEF98), and another player's Banjo is built from its
 * network object with the normal spawner (sub_823F8C68 → sub_8239D9F0).
 *
 * In a co-op session (coop.h) a match's scene is Showdown Town instead of its
 * arena: the host's, the town script LoadShowdownTown_82564808 would load for
 * its progress (the entries of dword_82FA30A4, picked by sub_824104C0), which
 * joiners get from its presence so everyone loads the same town.
 * renut_coop_town does the same for any online match (testing).
 *
 * Joining: the game takes an accepted invite in sub_822848E0 (each frame:
 * XNotifyGetNext for XN_LIVE_INVITE_ACCEPTED, then XInviteGetAcceptedInfo):
 * a pending invite is cancelled (sub_826CD0C8), then +33 = 1, +36 the user,
 * +40 the host's XSESSION_INFO (60 bytes: XNKID, XNADDR, XNKEY), +100 whether
 * it came from a game invite; the game then joins that session. A co-op join
 * is put there the same way, with the friend's hosted session.
 *
 * Town scripts: op 141 of sceneScript2GroupsExecute (sub_823CDD18; r3 the op,
 * its record + 4 id, + 8 marker, + 12 keep driver; r4 the scene) gives player
 * 1 its vehicle again, at the marker or, with none, where its current vehicle
 * is (the scene's vehicle per user, scene + 2628 + 8 × user; + 0x50 position,
 * + 0xC0 rotation). A joiner of a co-op match has no vehicle yet when the town
 * script runs it, which read through a null pointer (+ 0xC0): the op is left
 * out then, and the player starts on foot.
 *
 * Spawning: a player marker (markerPlayerExecute, sub_82516C70) in
 * multiplayer only spawns a Banjo for a local "Player" network object whose
 * player index (+ 96) is the marker's (+ 52), found by sub_82565910(marker,
 * &netobj); with none it spawns nothing. The town is made for one player, so
 * its marker matches one side only (the host was left without a Banjo, the
 * camera in the sky). When it matches nobody local in a co-op match, it is
 * asked again with the local player's index (sub_822B8BC0 for player 1,
 * dword_82FAD9F0, then that index's network object, sub_8238B150), once per
 * match load.
 *
 * The town is fixed once: the host's when it starts hosting, a joiner's from
 * the host's presence when it joins (the pick can be random when the progress
 * does not decide it, and presence is briefly without the session while it
 * changes). It decides the time of day too: a scene's (sub_82533FE8, scene +
 * 17964) is the "timeofdaysettings" entry whose script it runs.
 *
 * Progress: the town is the host's. Player 1's progress is what the game saves
 * (gameSaveSave_8250BB40: sub_8250C500(player, &data) makes it, 32 bytes of
 * header, + 8/12/16 three flag sets' sizes, + 20 the counters' count, + 24/28
 * two bit sets' sizes, then each; sub_8250C670(player, data, size) loads it,
 * as the save's load sub_8250BD08 does). A joiner asks the host for its
 * progress (EOS channel 2) when it joins and after each match start; the host
 * answers from its game thread. At match start the joiner keeps its own and
 * loads the host's into player 1, but its own options (counters 242-246:
 * brightness, contrast and volumes, sub_8250BD08's defaults), and does not
 * save while it holds the host's; out of co-op its own comes back.
 *
 * Challenges: each player plays their own copy (coop::OfflineChallenge: the
 * challenge manager runs as single player), alone, at the same time as
 * another player's, or with others: a challenge a player starts is offered to
 * the players in its scene, who get a prompt (overlay) and join with BACK
 * within kJoinWindow, which starts the same challenge on their game
 * (challengeManBegin on their scene's manager). The players' "ready"
 * (sub_82520598) is held until all of them are (the starter tells them to
 * go), so their challenges start together; each has its own opponents,
 * objectives and result.
 */
#include "renut_engine/online/coop.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "imgui.h"
#include "renut_engine/game/controllers.h"
#include "renut_engine/game/guest_call.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/online/eos.h"
#include "renut_engine/online/friends.h"
#include "renut_engine/online/online.h"
#include "renut_engine/online/online_log.h"

REXCVAR_DEFINE_BOOL(renut_coop_town, false, "Nuts&Bolts/Online",
                    "Co-op test: every online match (not only co-op ones) takes place in Showdown "
                    "Town instead of its arena, on the game's own multiplayer.");

REX_EXTERN(__imp__sub_824D3B48);  // online match start: push the game entry's scene script
REX_EXTERN(__imp__sub_822848E0);  // each frame: accepted invites, ...
REX_EXTERN(__imp__sub_823CDD18);  // sceneScript2GroupsExecute: a scene script op
REX_EXTERN(__imp__sub_82565910);  // player marker: the local Player network object for it
REX_EXTERN(__imp__gameSaveSave);  // saves a player's progress
REX_EXTERN(__imp__sub_822ABC28);  // writes the achievements a player's progress has
REX_EXTERN(__imp__appScenePush);  // queues a scene
REX_EXTERN(__imp__supportGameLoadNewScene);  // a scene from the one playing (world doors, back to town)
REX_EXTERN(__imp__sub_822BBF08);             // closes an app scene
REX_EXTERN(__imp__sub_8239E220);             // builds a remote entity's object here, retried
REX_IMPORT(__imp__sub_826CD0C8, CancelPendingInvite, void(u32));
REX_IMPORT(__imp__sub_8250C500, MakeSave, u32(u32, u32));         // (player, u8 **data)
REX_IMPORT(__imp__sub_8250C670, LoadSave, u32(u32, u32, u32));    // (player, data, size)
REX_IMPORT(__imp__meInternalAlloc, MeAlloc, u32(u32, u32));
REX_IMPORT(__imp__meInternalFree, MeFree, void(u32));
REX_IMPORT(__imp__sub_8251FF78, ChallengeManBegin, void(u32, u32, u32));  // (manager, const aid *challenge, flag)
REX_IMPORT(__imp__sub_822B8BC0, PlayerIndex, u32(u32));       // player → its index
REX_IMPORT(__imp__sub_8238B150, PlayerNetObject, u32(u32));   // index → its "Player" network object
REX_IMPORT(__imp__assetManOpenParams, AssetManOpenParams, u32(u32, u32));  // (const u32 *aid, may fail)
REX_IMPORT(__imp__sub_824104C0, PickTownScene, u32(u32));                 // town entries → the one for this progress
REX_IMPORT(__imp__sub_82235E68, AssetRelease, void(u32));

namespace {

using namespace renut::online;
using renut::guest::Guest;
using renut::guest::Load32;
using renut::guest::Store32;

// Guest data.
constexpr uint32_t kPendingScene = 0x82FAB038;  // the scene script appScenePush_822BBFE8 queued
constexpr uint32_t kTownScenes = 0x82FA30A4;    // aid of Showdown Town's scene entries (80 bytes each, script aid first)
constexpr uint32_t kTownEntries = 4;
constexpr uint32_t kTownEntrySize = 80;
constexpr uint32_t kMultiplayer = 0x82FAC650;   // byte: the game is in an online match
constexpr uint32_t kLevelLoaded = 0x82E5EDEC;   // byte: a level is loaded (network objects can be built)
// sub_822848E0's object: an accepted invite.
constexpr uint32_t kInvitePending = 33;
constexpr uint32_t kInviteUser = 36;
constexpr uint32_t kInviteHostInfo = 40;
constexpr uint32_t kInviteFromGameInvite = 100;
// Scene script op 141: player 1's vehicle again.
constexpr uint32_t kOpRespawnVehicle = 141;
constexpr uint32_t kOpId = 4;
constexpr uint32_t kOpMarker = 8;
constexpr uint32_t kPlayers = 0x82FAD9F0;   // player slots; [0] player 1
constexpr uint32_t kPlayerGone = 89;        // byte
constexpr uint32_t kPlayerUser = 108;
constexpr uint32_t kSceneVehicles = 2628;  // scene: per user, 8 bytes, its vehicle first
// Player markers and "Player" network objects.
constexpr uint32_t kMarkerPlayerIndex = 52;
constexpr uint32_t kNetObjPlayerIndex = 96;
// Saved progress (sub_8250C500).
constexpr uint32_t kSaveHeader = 32;
constexpr uint32_t kSaveCounters = 20;  // header: the counters' count
constexpr uint32_t kOptionFirst = 242;  // the counters of the options
constexpr uint32_t kOptionCount = 5;
constexpr size_t kMaxSave = 1 << 20;

// Co-op messages (EOS channel 2), reliable: u8 type, u8 version, then for the
// progress, u32 total size, u32 offset (big-endian) and that part of it.
enum CoopMessage : uint8_t {
  kAskProgress = 1,
  kProgress = 2,
  kScene = 3,  // u8 id[16] the player's online ID, u8 its player index, u32 its scene script
  // Challenges, between the players directly (each one's sender is its player):
  kChallengeOffer = 4,  // u32 scene, u32 challenge, u32 its start flag: it started one there
  kChallengeJoin = 5,   // u32 challenge: to its starter, the sender joins it
  kChallengeReady = 6,  // u32 challenge: to its starter, the sender is ready to start it
  kChallengeGo = 7,     // u32 challenge: from its starter, everyone starts now
};
constexpr uint8_t kCoopVersion = 1;
constexpr size_t kProgressHeader = 10;
constexpr size_t kSceneSize = 2 + 16 + 1 + 4;
// A network object: + 36 its state, whose + 164 is its owner (the same for
// every object of a player's game; sub_8269F150 asks it whether it is this
// game's). An entity's network object: + 104 its entity once built here.
constexpr uint32_t kNetObjState = 36;
constexpr uint32_t kNetObjStateOwner = 164;
constexpr uint32_t kNetObjEntity = 104;
constexpr uint32_t kMaxPlayers = 8;
// The online play frontend's app scene script ("multiplayer_frontend"):
// closing that app destroys its XUI scene, whose destroy (sub_825886D8)
// leaves both sessions and ends multiplayer.
constexpr uint32_t kFrontendScript = 0x82FA30A8;
// The app slot id the online match's scene is pushed with (sub_824D3B48).
constexpr uint32_t kMatchSlotId = 1;
// Challenges: the current app slot (dword_82FAB0FC; + 4 its scene), a scene's
// + 5552, whose + 6176 is its challenge manager (banjoxScriptBlockingExecute
// op 96 starts challenges on it); the manager's + 20 state (0: none), + 1348
// its single-player "ready" (sub_82520598).
constexpr uint32_t kCurrentAppSlot = 0x82FAB0FC;
constexpr uint32_t kSlotScene = 4;
constexpr uint32_t kSceneData = 5552;
constexpr uint32_t kSceneChallengeMan = 6176;
constexpr uint32_t kChallengeState = 20;
constexpr uint32_t kChallengeReadyFlag = 1348;
constexpr uint16_t kButtonBack = 0x0020;  // XINPUT_GAMEPAD_BACK
// How long a challenge can be joined after it starts, and how long its
// players wait for each other to be ready.
constexpr auto kJoinWindow = std::chrono::seconds(10);
constexpr auto kReadyWait = std::chrono::seconds(30);

std::mutex g_mutex;
coop::Role g_role = coop::Role::kNone;
std::string g_host_id;
// The town scene of this co-op session (0: pick it at match start).
uint32_t g_town = 0;
// A join for the game to take as an accepted invite.
std::optional<friends::Session> g_pending_join;
// Host: who asked for its progress. Joiner: the host's progress, coming in
// and complete (not loaded yet).
std::vector<std::string> g_progress_asked;
std::vector<uint8_t> g_progress_in;
std::vector<uint8_t> g_host_progress;
// Host: its joiners (who asked for its progress). Both: where each player is,
// by online ID: its player index (the "Player" network objects' + 96, which
// changes from the lobby to the match) and its scene.
std::vector<std::string> g_joiners;
struct Where {
  uint32_t index = 0;
  uint32_t scene = 0;
};
std::map<std::string, Where> g_scenes;
// Challenges: one another player started in this game's scene, which player 1
// can join (until 'until'); the challenge player 1 is in with others (its
// starter, its players, those ready, whether its starter said go).
using Clock = std::chrono::steady_clock;
struct ChallengeOffer {
  std::string from;
  uint32_t scene = 0;
  uint32_t challenge = 0;
  uint32_t flag = 0;
  Clock::time_point until;
};
std::optional<ChallengeOffer> g_offer;
struct ChallengeGroup {
  std::string starter;
  uint32_t challenge = 0;  // 0: none
  std::vector<std::string> players;
  std::vector<std::string> ready;
  bool go = false;
  Clock::time_point joinable_until;
};
ChallengeGroup g_group;
// Game thread: the local Banjo spawned in this match load; the joiner's own
// progress while player 1 has the host's (empty: it has its own); the scene
// this game is in.
bool g_local_spawned = false;
std::vector<uint8_t> g_own_progress;
uint32_t g_my_scene = 0;
// Game thread: the scene whose level is loaded here (0: none yet); a scene
// change is going on (from its start until player 1 has spawned in the new
// level).
uint32_t g_level_scene = 0;
bool g_changing_scene = false;

void AskProgress(const std::string &host) {
  const uint8_t m[] = {kAskProgress, kCoopVersion};
  eos::Send(host, eos::kChannelCoop, m, sizeof(m), true);
}

uint32_t LoadBe32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void StoreBe32(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

std::vector<uint8_t> SceneMessage(const std::string &id, const Where &where) {
  std::vector<uint8_t> m(kSceneSize);
  m[0] = kScene;
  m[1] = kCoopVersion;
  eos::IdBytes bytes{};
  eos::IdToBytes(id, bytes);
  std::memcpy(m.data() + 2, bytes.data(), bytes.size());
  m[18] = uint8_t(where.index);
  StoreBe32(m.data() + 19, where.scene);
  return m;
}

// A co-op message, on the EOS thread.
void OnCoopMessage(const std::string &id, const uint8_t *data, size_t size) {
  if (size < 2 || data[1] != kCoopVersion)
    return;
  std::lock_guard lock(g_mutex);
  switch (data[0]) {
  case kAskProgress:
    if (g_role == coop::Role::kHost &&
        std::find(g_progress_asked.begin(), g_progress_asked.end(), id) == g_progress_asked.end())
      g_progress_asked.push_back(id);
    if (g_role == coop::Role::kHost && std::find(g_joiners.begin(), g_joiners.end(), id) == g_joiners.end()) {
      g_joiners.push_back(id);
      // Where everyone is, for the new joiner.
      for (const auto &[player, where] : g_scenes) {
        const std::vector<uint8_t> m = SceneMessage(player, where);
        eos::Send(id, eos::kChannelCoop, m.data(), m.size(), true);
      }
    }
    break;
  case kScene: {
    // A joiner's, for the host (which tells the other joiners); the host's
    // (its own or a joiner's), for a joiner.
    const bool from_host = g_role == coop::Role::kJoin && id == g_host_id;
    const bool from_joiner = g_role == coop::Role::kHost &&
                             std::find(g_joiners.begin(), g_joiners.end(), id) != g_joiners.end();
    if (size < kSceneSize || (!from_host && !from_joiner))
      return;
    eos::IdBytes bytes{};
    std::memcpy(bytes.data(), data + 2, bytes.size());
    const std::string player = eos::IdFromBytes(bytes);
    if (player == eos::OwnId())
      return;
    const Where where{data[18], LoadBe32(data + 19)};
    Where &known = g_scenes[player];
    if (known.index != where.index || known.scene != where.scene)
      RONL_INFO("co-op: {} (player {}) is in scene {:08X}", player, where.index, where.scene);
    known = where;
    if (from_joiner) {
      for (const std::string &other : g_joiners) {
        if (other != id)
          eos::Send(other, eos::kChannelCoop, data, kSceneSize, true);
      }
    }
    break;
  }
  case kChallengeOffer:
    // From a co-op player (one whose scene is known); whether player 1 can
    // join it (same scene, no challenge of its own) is the game thread's.
    if (size >= 14 && g_scenes.count(id)) {
      g_offer = ChallengeOffer{id, LoadBe32(data + 2), LoadBe32(data + 6), LoadBe32(data + 10),
                               Clock::now() + kJoinWindow};
      RONL_INFO("co-op: {} started challenge {:08X} in scene {:08X}", id, g_offer->challenge, g_offer->scene);
    }
    break;
  case kChallengeJoin:
    if (size >= 6 && g_group.starter == eos::OwnId() && LoadBe32(data + 2) == g_group.challenge &&
        std::find(g_group.players.begin(), g_group.players.end(), id) == g_group.players.end()) {
      g_group.players.push_back(id);
      RONL_INFO("co-op: {} joins this player's challenge", id);
    }
    break;
  case kChallengeReady:
    if (size >= 6 && g_group.starter == eos::OwnId() && LoadBe32(data + 2) == g_group.challenge &&
        std::find(g_group.ready.begin(), g_group.ready.end(), id) == g_group.ready.end())
      g_group.ready.push_back(id);
    break;
  case kChallengeGo:
    if (size >= 6 && id == g_group.starter && LoadBe32(data + 2) == g_group.challenge)
      g_group.go = true;
    break;
  case kProgress: {
    if (g_role != coop::Role::kJoin || id != g_host_id || size < kProgressHeader)
      return;
    const uint32_t total = LoadBe32(data + 2);
    const uint32_t offset = LoadBe32(data + 6);
    const size_t part = size - kProgressHeader;
    if (total > kMaxSave || offset + part > total)
      return;
    if (offset == 0)
      g_progress_in.assign(total, 0);
    if (g_progress_in.size() != total)
      return;
    std::memcpy(g_progress_in.data() + offset, data + kProgressHeader, part);
    if (offset + part == total) {
      g_host_progress.swap(g_progress_in);
      g_progress_in.clear();
      RONL_INFO("co-op: the host's progress came ({} bytes)", total);
    }
    break;
  }
  default:
    break;
  }
}

[[maybe_unused]] const bool g_registered = [] {
  eos::SetReceiver(eos::kChannelCoop, &OnCoopMessage);
  return true;
}();

// Player 1's progress as the game saves it (empty if it cannot be made).
std::vector<uint8_t> ReadProgress(PPCContext &ctx, uint8_t *base, uint32_t player) {
  Guest g(ctx, base);
  const uint32_t out = g.Alloc(4);
  const bool made = MakeSave(g.frame(), base, player, out) & 0xFF;
  const uint32_t data = Load32(base, out);
  std::vector<uint8_t> save;
  if (made && data) {
    size_t size = kSaveHeader + size_t(4) * Load32(base, data + kSaveCounters);
    for (uint32_t field : {8u, 12u, 16u, 24u, 28u})
      size += Load32(base, data + field);
    if (size <= kMaxSave)
      save.assign(base + data, base + data + size);
  }
  if (data)
    MeFree(g.frame(), base, data);
  return save;
}

// Loads 'save' into player 1, as the game loads its save.
bool WriteProgress(PPCContext &ctx, uint8_t *base, uint32_t player, const std::vector<uint8_t> &save) {
  if (save.size() < kSaveHeader)
    return false;
  Guest g(ctx, base);
  const uint32_t data = MeAlloc(g.frame(), base, uint32_t(save.size()), 0);
  if (!data)
    return false;
  std::memcpy(base + data, save.data(), save.size());
  const bool loaded = LoadSave(g.frame(), base, player, data, uint32_t(save.size())) & 0xFF;
  MeFree(g.frame(), base, data);
  return loaded;
}

// The options counters of 'from' in 'to' (both the same layout).
void KeepOptions(std::vector<uint8_t> &to, const std::vector<uint8_t> &from) {
  if (to.size() < kSaveHeader || from.size() != to.size() ||
      std::memcmp(to.data(), from.data(), kSaveHeader) != 0)
    return;
  const uint32_t count = LoadBe32(to.data() + kSaveCounters);
  if (count < kOptionFirst + kOptionCount)
    return;
  const size_t counters = kSaveHeader + LoadBe32(to.data() + 8) + LoadBe32(to.data() + 12) +
                          LoadBe32(to.data() + 16);
  const size_t at = counters + 4 * kOptionFirst;
  if (at + 4 * kOptionCount > to.size())
    return;
  std::memcpy(to.data() + at, from.data() + at, 4 * kOptionCount);
}

// Host, game thread: its progress to those who asked.
void ServeProgress(PPCContext &ctx, uint8_t *base) {
  std::vector<std::string> asked;
  {
    std::lock_guard lock(g_mutex);
    asked.swap(g_progress_asked);
    if (g_role != coop::Role::kHost)
      return;
  }
  const uint32_t player = Load32(base, kPlayers);
  if (asked.empty() || !player)
    return;
  const std::vector<uint8_t> save = ReadProgress(ctx, base, player);
  if (save.empty()) {
    RONL_WARN("co-op: could not make player 1's progress for the joiners");
    return;
  }
  constexpr size_t kPart = eos::kMaxMessage - kProgressHeader;
  for (const std::string &id : asked) {
    for (size_t offset = 0; offset < save.size(); offset += kPart) {
      const size_t part = std::min(kPart, save.size() - offset);
      std::vector<uint8_t> m(kProgressHeader + part);
      m[0] = kProgress;
      m[1] = kCoopVersion;
      StoreBe32(m.data() + 2, uint32_t(save.size()));
      StoreBe32(m.data() + 6, uint32_t(offset));
      std::memcpy(m.data() + kProgressHeader, save.data() + offset, part);
      eos::Send(id, eos::kChannelCoop, m.data(), m.size(), true);
    }
    RONL_INFO("co-op: sent player 1's progress ({} bytes) to {}", save.size(), id);
  }
}

// Joiner, game thread, at match start: the host's progress in player 1.
void TakeHostProgress(PPCContext &ctx, uint8_t *base) {
  if (coop::CurrentRole() != coop::Role::kJoin)
    return;
  std::vector<uint8_t> save;
  std::string host;
  {
    std::lock_guard lock(g_mutex);
    save.swap(g_host_progress);
    host = g_host_id;
  }
  AskProgress(host);  // for the next match
  if (save.empty()) {
    if (g_own_progress.empty())
      RONL_WARN("co-op: the host's progress has not come yet: this match has your own");
    return;
  }
  const uint32_t player = Load32(base, kPlayers);
  if (!player)
    return;
  if (g_own_progress.empty()) {
    g_own_progress = ReadProgress(ctx, base, player);
    if (g_own_progress.empty()) {
      RONL_WARN("co-op: could not keep your own progress: this match has it, not the host's");
      return;
    }
  }
  KeepOptions(save, g_own_progress);
  if (WriteProgress(ctx, base, player, save))
    RONL_INFO("co-op: this match has the host's progress (yours is kept and not saved over)");
  else
    RONL_WARN("co-op: the host's progress did not load (another version of the game?)");
}

// Joiner, game thread, out of co-op: its own progress back. If it does not
// load, player 1 keeps the host's and the game still does not save.
bool g_restore_failed = false;

void RestoreOwnProgress(PPCContext &ctx, uint8_t *base) {
  if (g_own_progress.empty() || g_restore_failed || coop::CurrentRole() == coop::Role::kJoin)
    return;
  const uint32_t player = Load32(base, kPlayers);
  if (!player)
    return;
  if (!WriteProgress(ctx, base, player, g_own_progress)) {
    g_restore_failed = true;
    RONL_ERROR("co-op: your own progress did not load back; the game will not save until it is "
               "restarted (your save is as it was before co-op)");
    return;
  }
  g_own_progress.clear();
  RONL_INFO("co-op: your own progress is back");
}

// Game thread: a co-op scene load is going on, which keeps online play's
// frontend app open.
bool g_keeping_frontend = false;

// ---------------------------------------------------------------------------
// Challenges with others: started together, each player in their own copy.
// ---------------------------------------------------------------------------

// The prompt the overlay shows (any thread reads it; empty: none).
std::mutex g_prompt_mutex;
std::string g_prompt;

void SetPrompt(std::string text) {
  std::lock_guard lock(g_prompt_mutex);
  g_prompt = std::move(text);
}

class ChallengePrompt : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    std::string text;
    {
      std::lock_guard lock(g_prompt_mutex);
      text = g_prompt;
    }
    if (text.empty())
      return;
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.8f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##coop_challenge", nullptr, flags)) {
      ImGui::PushFont(nullptr, 26.0f);
      ImGui::TextUnformatted(text.c_str());
      ImGui::PopFont();
    }
    ImGui::End();
  }
};

void ShowPromptDialog() {
  static ChallengePrompt prompt;
  static bool added = false;
  if (!added) {
    added = true;
    renut::gpu::overlay::AddDialog(&prompt);
  }
}

// A co-op player's name for the prompt.
std::string PlayerName(const std::string &id) {
  for (const friends::Friend &f : friends::Friends())
    if (f.id == id && !f.gamertag.empty())
      return f.gamertag;
  return "Another player";
}

void SendChallengeMessage(const std::string &to, CoopMessage type, uint32_t challenge) {
  uint8_t m[6] = {type, kCoopVersion};
  StoreBe32(m + 2, challenge);
  eos::Send(to, eos::kChannelCoop, m, sizeof(m), true);
}

// The current scene's challenge manager (0: none).
uint32_t CurrentChallengeMan(const uint8_t *base) {
  const uint32_t slot = Load32(base, kCurrentAppSlot);
  const uint32_t scene = slot ? Load32(base, slot + kSlotScene) : 0;
  const uint32_t data = scene ? Load32(base, scene + kSceneData) : 0;
  return data ? data + kSceneChallengeMan : 0;
}

bool InChallenge(const uint8_t *base) {
  const uint32_t man = CurrentChallengeMan(base);
  return man && Load32(base, man + kChallengeState) != 0;
}

// Game thread: player 1's challenge "ready" held until its group starts
// together (the manager it is held on; since when).
uint32_t g_ready_man = 0;
Clock::time_point g_ready_since;
uint16_t g_last_buttons = 0;

// Game thread: player 1 started challenge 'challenge' itself: the others in
// its scene can join it for a while.
void OfferChallenge(const uint8_t *base, uint32_t challenge, uint32_t flag) {
  const std::string own = eos::OwnId();
  std::vector<std::string> others;
  bool anyone_here = false;
  {
    std::lock_guard lock(g_mutex);
    for (const auto &[id, where] : g_scenes) {
      if (id == own)
        continue;
      others.push_back(id);
      anyone_here |= where.scene == g_level_scene;
    }
    g_group = {own, challenge, {own}, {}, false, anyone_here ? Clock::now() + kJoinWindow : Clock::now()};
  }
  uint8_t m[14] = {kChallengeOffer, kCoopVersion};
  StoreBe32(m + 2, g_level_scene);
  StoreBe32(m + 6, challenge);
  StoreBe32(m + 10, flag);
  for (const std::string &id : others)
    eos::Send(id, eos::kChannelCoop, m, sizeof(m), true);
}

// Game thread: player 1 joins 'offer': the same challenge starts here, in its
// own copy, and its starter is told.
void JoinChallenge(PPCContext &ctx, uint8_t *base, const ChallengeOffer &offer) {
  const uint32_t man = CurrentChallengeMan(base);
  if (!man)
    return;
  {
    std::lock_guard lock(g_mutex);
    g_group = {offer.from, offer.challenge, {offer.from, eos::OwnId()}, {}, false, Clock::now()};
    g_offer.reset();
  }
  SendChallengeMessage(offer.from, kChallengeJoin, offer.challenge);
  RONL_INFO("co-op: joining {}'s challenge {:08X}", offer.from, offer.challenge);
  coop::OfflineChallenge offline(base);
  Guest g(ctx, base);
  const uint32_t aid = g.Alloc(16);
  Store32(base, aid, offer.challenge);
  ChallengeManBegin(g.frame(), base, man, aid, offer.flag);
}

// Game thread, after sub_82520598 made player 1 ready on 'man': held while its
// group waits for each other (or for others to join).
void HoldReady(uint8_t *base, uint32_t man) {
  if (coop::CurrentRole() == coop::Role::kNone)
    return;
  std::string starter;
  uint32_t challenge;
  bool wait;
  {
    std::lock_guard lock(g_mutex);
    starter = g_group.starter;
    challenge = g_group.challenge;
    const bool own = starter == eos::OwnId();
    wait = challenge && (!own || g_group.players.size() > 1 || Clock::now() < g_group.joinable_until);
  }
  if (!wait)
    return;
  Store32(base, man + kChallengeReadyFlag, 0);
  g_ready_man = man;
  g_ready_since = Clock::now();
  if (starter != eos::OwnId())
    SendChallengeMessage(starter, kChallengeReady, challenge);
}

// Game thread, each frame: the join prompt and its button, and a held ready
// let go once the group starts.
void ChallengeFrame(PPCContext &ctx, uint8_t *base) {
  const uint16_t buttons = renut::controllers::Player1Buttons();
  const bool back_pressed = (buttons & kButtonBack) && !(g_last_buttons & kButtonBack);
  g_last_buttons = buttons;
  const auto now = Clock::now();

  if (g_ready_man) {
    bool go = false;
    std::vector<std::string> tell;
    std::string waiting_text;
    uint32_t challenge = 0;
    {
      std::lock_guard lock(g_mutex);
      challenge = g_group.challenge;
      const bool timed_out = now - g_ready_since > kReadyWait;
      if (g_group.starter == eos::OwnId()) {
        bool all_ready = now >= g_group.joinable_until;
        for (const std::string &id : g_group.players)
          all_ready &= id == g_group.starter ||
                       std::find(g_group.ready.begin(), g_group.ready.end(), id) != g_group.ready.end();
        go = all_ready || timed_out;
        if (go)
          for (const std::string &id : g_group.players)
            if (id != g_group.starter)
              tell.push_back(id);
      } else {
        go = g_group.go || timed_out;
      }
    }
    if (go) {
      for (const std::string &id : tell)
        SendChallengeMessage(id, kChallengeGo, challenge);
      Store32(base, g_ready_man + kChallengeReadyFlag, 1);
      g_ready_man = 0;
      SetPrompt({});
      RONL_INFO("co-op: challenge {:08X} starts ({} told)", challenge, tell.size());
    } else {
      const long left = long(std::chrono::duration_cast<std::chrono::seconds>(kReadyWait - (now - g_ready_since)).count());
      SetPrompt("Waiting for the other players... (" + std::to_string(std::max(left, 0L)) + ")");
      ShowPromptDialog();
    }
    return;
  }

  std::optional<ChallengeOffer> offer;
  {
    std::lock_guard lock(g_mutex);
    if (g_offer && now >= g_offer->until)
      g_offer.reset();
    offer = g_offer;
  }
  if (!offer || offer->scene != g_level_scene || g_changing_scene || InChallenge(base)) {
    SetPrompt({});
    return;
  }
  if (back_pressed) {
    SetPrompt({});
    JoinChallenge(ctx, base, *offer);
    return;
  }
  const long left = long(std::chrono::duration_cast<std::chrono::seconds>(offer->until - now).count());
  SetPrompt(PlayerName(offer->from) + " started a challenge: press BACK to join (" +
            std::to_string(std::max(left, 0L) + 1) + ")");
  ShowPromptDialog();
}

// Game thread: OfflineChallenge's depth and the multiplayer flag it hides.
int g_offline_depth = 0;
uint8_t g_hidden_multiplayer = 0;

bool InCoopMatch(const uint8_t *base) {
  return coop::CurrentRole() != coop::Role::kNone && InSession() && coop::InMultiplayer(base);
}

constexpr uint32_t kNoPlayer = 0xFFFFFFFF;

// Player 1's player index (kNoPlayer: none).
uint32_t LocalPlayerIndex(PPCContext &ctx, uint8_t *base) {
  const uint32_t player = Load32(base, kPlayers);
  if (!player)
    return kNoPlayer;
  rex::CallFrame frame(ctx);
  return PlayerIndex(frame, base, player);
}

// Game thread: what this game last told the others.
Where g_announced;

void BeginSceneChange() {
  g_changing_scene = true;
  g_local_spawned = false;
}

// Game thread: this game's scene and player index for the other co-op players
// (the host's to its joiners, a joiner's to the host, which tells the
// others), when either has changed since it last told them.
void AnnounceScene(PPCContext &ctx, uint8_t *base) {
  const uint32_t index = LocalPlayerIndex(ctx, base);
  if (!g_my_scene || index == kNoPlayer || index >= kMaxPlayers)
    return;
  if (g_announced.index == index && g_announced.scene == g_my_scene)
    return;
  g_announced = {index, g_my_scene};
  const std::string own = eos::OwnId();
  std::vector<std::string> to;
  {
    std::lock_guard lock(g_mutex);
    g_scenes[own] = g_announced;
    if (g_role == coop::Role::kHost)
      to = g_joiners;
    else if (g_role == coop::Role::kJoin && !g_host_id.empty())
      to.push_back(g_host_id);
  }
  const std::vector<uint8_t> m = SceneMessage(own, g_announced);
  for (const std::string &id : to)
    eos::Send(id, eos::kChannelCoop, m.data(), m.size(), true);
  RONL_INFO("co-op: in scene {:08X} (player {}), told {} player(s)", g_my_scene, index, to.size());
}

// A network object's owner (see kNetObjStateOwner).
uint32_t OwnerOf(const uint8_t *base, uint32_t netobj) {
  const uint32_t state = netobj ? Load32(base, netobj + kNetObjState) : 0;
  return state ? Load32(base, state + kNetObjStateOwner) : 0;
}

// Whether remote network object 'netobj' belongs to a player in another scene
// than the one loaded here ('player': its player index, kNoPlayer if not
// found). A player whose scene is not known yet counts as being elsewhere
// while anyone is: an object from another level has assets this level does
// not. While this game changes scene (the old level still loaded, or none)
// nothing remote is built.
bool FromOtherScene(PPCContext &ctx, uint8_t *base, uint32_t netobj, uint32_t &player) {
  player = kNoPlayer;
  const uint32_t owner = OwnerOf(base, netobj);
  if (owner) {
    rex::CallFrame frame(ctx);
    for (uint32_t i = 0; i < kMaxPlayers; ++i) {
      const uint32_t player_netobj = PlayerNetObject(frame, base, i);
      if (player_netobj && OwnerOf(base, player_netobj) == owner) {
        player = i;
        break;
      }
    }
  }
  if (player != kNoPlayer && player == LocalPlayerIndex(ctx, base))
    return false;  // this game's own
  if (g_changing_scene)
    return true;
  if (!g_level_scene)
    return false;
  const std::string own = eos::OwnId();
  std::lock_guard lock(g_mutex);
  bool anyone_elsewhere = false;
  for (const auto &[id, where] : g_scenes) {
    if (id == own)
      continue;
    if (player != kNoPlayer && where.index == player)
      return where.scene != g_level_scene;
    anyone_elsewhere |= where.scene != g_level_scene;
  }
  return anyone_elsewhere;
}

}  // namespace

namespace renut::online::coop {

Role CurrentRole() {
  std::lock_guard lock(g_mutex);
  return g_role;
}

std::string HostId() {
  std::lock_guard lock(g_mutex);
  return g_host_id;
}

void Host(uint32_t town) {
  {
    std::lock_guard lock(g_mutex);
    g_role = Role::kHost;
    g_host_id.clear();
    g_town = town;
    g_pending_join.reset();
    g_progress_in.clear();
    g_host_progress.clear();
    g_joiners.clear();
    g_scenes.clear();
  }
  friends::SetCoopHosting(true, town);
  RONL_INFO("co-op: hosting (town scene {:08X})", town);
}

bool Join(const std::string &friend_id) {
  for (const friends::Friend &f : friends::Friends()) {
    if (f.id != friend_id || !f.online || !f.session.Valid() || !f.session.coop)
      continue;
    friends::SetCoopHosting(false, 0);
    std::lock_guard lock(g_mutex);
    g_role = Role::kJoin;
    g_host_id = friend_id;
    g_town = f.session.coop_town;
    g_pending_join = f.session;
    g_progress_in.clear();
    g_host_progress.clear();
    g_scenes.clear();
    RONL_INFO("co-op: joining {} ({}), town scene {:08X}", f.gamertag, friend_id, g_town);
    AskProgress(friend_id);
    return true;
  }
  RONL_WARN("co-op: {} hosts no co-op session to join", friend_id);
  return false;
}

void Leave() {
  {
    std::lock_guard lock(g_mutex);
    if (g_role == Role::kNone)
      return;
    g_role = Role::kNone;
    g_host_id.clear();
    g_town = 0;
    g_pending_join.reset();
    g_progress_asked.clear();
    g_progress_in.clear();
    g_host_progress.clear();
    g_joiners.clear();
    g_scenes.clear();
  }
  friends::SetCoopHosting(false, 0);
  RONL_INFO("co-op: left");
}

bool Active() {
  return CurrentRole() != Role::kNone || REXCVAR_GET(renut_coop_town);
}

OfflineChallenge::OfflineChallenge(uint8_t *base) : base_(base) {
  if (g_offline_depth++ == 0) {
    g_hidden_multiplayer = 0;
    if (CurrentRole() != Role::kNone && base_[kMultiplayer]) {
      g_hidden_multiplayer = base_[kMultiplayer];
      base_[kMultiplayer] = 0;
    }
  }
}

OfflineChallenge::~OfflineChallenge() {
  if (--g_offline_depth == 0 && g_hidden_multiplayer) {
    base_[kMultiplayer] = g_hidden_multiplayer;
    g_hidden_multiplayer = 0;
  }
}

bool InMultiplayer(const uint8_t *base) {
  return base[kMultiplayer] || (g_offline_depth > 0 && g_hidden_multiplayer);
}

// The town script LoadShowdownTown_82564808 would load now (0 if none).
uint32_t TownScene(PPCContext &ctx, uint8_t *base) {
  if (!Load32(base, kTownScenes))
    return 0;
  rex::CallFrame frame(ctx);
  const uint32_t entries = AssetManOpenParams(frame, base, kTownScenes, 0);
  if (!entries)
    return 0;
  const uint32_t entry = PickTownScene(frame, base, entries);
  const uint32_t script = entry ? Load32(base, entry) : 0;
  AssetRelease(frame, base, entries);
  return script;
}

}  // namespace renut::online::coop

namespace {

// The session's town (0: none fixed).
uint32_t SessionTown() {
  std::lock_guard lock(g_mutex);
  return g_town;
}

}  // namespace

REX_HOOK_RAW(sub_824D3B48) {
  __imp__sub_824D3B48(ctx, base);
  if (!coop::Active())
    return;
  g_local_spawned = false;
  TakeHostProgress(ctx, base);
  const uint32_t arena = Load32(base, kPendingScene);
  uint32_t town = SessionTown();
  if (!town)
    town = coop::TownScene(ctx, base);
  if (!town) {
    RONL_WARN("co-op: no Showdown Town scene to put the match in; it stays in its arena ({:08X})", arena);
    return;
  }
  Store32(base, kPendingScene, town);
  g_my_scene = town;
  g_announced = {};
  BeginSceneChange();
  AnnounceScene(ctx, base);
  RONL_INFO("co-op: the match goes to Showdown Town (scene {:08X}) instead of its arena ({:08X}), multiplayer {}",
            town, arena, int(base[kMultiplayer]));
}

// entry *sub_824104C0(entries): the Showdown Town scene entry to load (4 of 80
// bytes, the script first), by player 1's progress or at random; used by
// LoadShowdownTown_82564808 (back to town from a world). In a co-op session
// every return to town is the session's town, so everyone is in the same one
// (a different one is another scene, and its players are not built there).
REX_EXTERN(__imp__sub_824104C0);
REX_HOOK_RAW(sub_824104C0) {
  const uint32_t entries = ctx.r3.u32;
  const uint32_t town = coop::CurrentRole() != coop::Role::kNone ? SessionTown() : 0;
  if (town && entries) {
    for (uint32_t i = 0; i < kTownEntries; ++i) {
      if (Load32(base, entries + kTownEntrySize * i) == town) {
        ctx.r3.u64 = entries + kTownEntrySize * i;
        RONL_INFO("co-op: back to the session's Showdown Town (scene {:08X})", town);
        return;
      }
    }
  }
  __imp__sub_824104C0(ctx, base);
}

// int sub_823CDD18(op **, scene): a scene script op.
REX_HOOK_RAW(sub_823CDD18) {
  const uint32_t op = ctx.r3.u32 ? Load32(base, ctx.r3.u32) : 0;
  if (op && Load32(base, op + kOpId) == kOpRespawnVehicle && !Load32(base, op + kOpMarker)) {
    const uint32_t scene = ctx.r4.u32;
    const uint32_t player = Load32(base, kPlayers);
    const uint32_t user = player ? Load32(base, player + kPlayerUser) : 0;
    const bool has_vehicle = player && !base[player + kPlayerGone] && user < 4 &&
                             Load32(base, scene + kSceneVehicles + 8 * user);
    if (!has_vehicle) {
      RONL_INFO("co-op: the town script would put player 1's vehicle where it is, but it has none: left out");
      ctx.r3.u64 = 3;  // done
      return;
    }
  }
  __imp__sub_823CDD18(ctx, base);
}

// bool sub_82565910(marker, NetObj **out): the local Player network object a
// player marker spawns a Banjo for.
REX_HOOK_RAW(sub_82565910) {
  const uint32_t marker = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82565910(ctx, base);
  if (ctx.r3.u8) {
    g_local_spawned = true;
    return;
  }
  if (!coop::Active() || g_local_spawned || !marker)
    return;
  const uint32_t player = Load32(base, kPlayers);
  if (!player)
    return;
  uint32_t index;
  uint32_t netobj;
  {
    rex::CallFrame frame(ctx);
    index = PlayerIndex(frame, base, player);
    netobj = PlayerNetObject(frame, base, index);
  }
  const uint32_t wanted = Load32(base, marker + kMarkerPlayerIndex);
  if (!netobj) {
    RONL_WARN("co-op: the town's player marker is for player index {}, and player 1 (index {}) has no "
              "network object yet: no Banjo for it",
              wanted, index);
    return;
  }
  const uint32_t local = Load32(base, netobj + kNetObjPlayerIndex);
  Store32(base, marker + kMarkerPlayerIndex, local);
  ctx.r3.u64 = marker;
  ctx.r4.u64 = out;
  __imp__sub_82565910(ctx, base);
  Store32(base, marker + kMarkerPlayerIndex, wanted);
  if (ctx.r3.u8) {
    g_local_spawned = true;
    RONL_INFO("co-op: the town's player marker is for player index {}: player 1 (index {}) spawns there",
              wanted, local);
  } else {
    RONL_WARN("co-op: the town's player marker (index {}) matches no local player, not even index {}",
              wanted, local);
  }
}

// int appScenePush(app, const aid *script, slot id, ...): a new scene, while in
// online play logged with where it came from. Its app slot gets 'slot id'
// (0x82FAB0DC → slot + 16), and a remote player's objects are built in the
// scene whose slot id is the one they send (+ 348 of theirs, their scene's):
// the online match is pushed with 1 (sub_824D3B48), apart from online play's
// frontend, 0. Scene changes push 0 (sub_822BC0D8) and close every other
// scene; a co-op one keeps the frontend open, so the new scene is pushed with
// 1 as the match is (with 0 the others' Banjos went into the frontend's
// scene, unseen).
REX_HOOK_RAW(appScenePush) {
  if (InSession()) {
    const uint32_t script = ctx.r4.u32 ? Load32(base, ctx.r4.u32) : 0;
    if (g_keeping_frontend)
      ctx.r5.u64 = kMatchSlotId;
    RONL_INFO("co-op: scene {:08X} pushed in online play, slot id {} (from {})", script, ctx.r5.u32,
              renut::guest::GuestBacktrace(ctx, base));
  }
  __imp__appScenePush(ctx, base);
}

// int supportGameLoadNewScene(const aid *script, scene, from_door, door
// transform): a scene from the one playing (world doors, sub_8224CBE8; back
// to town, LoadShowdownTown_82564808). It closes the app scenes
// (sub_822BC0D8), online play's frontend among them, which ended the session
// for everyone: in a co-op match that one stays. Each player goes on their
// own, and tells the others its new scene as it leaves (before its new
// level's objects reach them). Online play's own way out (to
// "multiplayer_frontend") is left as it is.
REX_HOOK_RAW(supportGameLoadNewScene) {
  const uint32_t script = ctx.r3.u32 ? Load32(base, ctx.r3.u32) : 0;
  if (!script || !InCoopMatch(base) || script == Load32(base, kFrontendScript)) {
    __imp__supportGameLoadNewScene(ctx, base);
    return;
  }
  g_local_spawned = false;
  g_my_scene = script;
  BeginSceneChange();
  AnnounceScene(ctx, base);
  g_keeping_frontend = true;
  __imp__supportGameLoadNewScene(ctx, base);
  g_keeping_frontend = false;
}

// void sub_821FFEB8(): each frame while a level is loaded, the game's network
// update (sets byte_82E5EDEC, makes the level's network objects). A scene
// change is over once player 1's Banjo has spawned in the new level (its
// player marker, sub_82565910; g_local_spawned is cleared as the change
// starts): the others' objects in that scene can be built here from then. The
// scene told again if player 1's index has changed (it does from the lobby to
// the match).
REX_EXTERN(__imp__sub_821FFEB8);
REX_HOOK_RAW(sub_821FFEB8) {
  __imp__sub_821FFEB8(ctx, base);
  if (!InCoopMatch(base))
    return;
  if (base[kLevelLoaded] && g_local_spawned && (g_changing_scene || !g_level_scene)) {
    g_changing_scene = false;
    g_level_scene = g_my_scene;
    RONL_INFO("co-op: scene {:08X} is loaded here", g_level_scene);
  }
  AnnounceScene(ctx, base);
}

// int sub_8239E220(entity NetObj): builds a remote entity's object here (its
// type's build, vtable + 0x44: a Banjo sub_823F8C68, a vehicle sub_826074C8,
// ...), retried while it has none (+ 104) and a level is loaded. N&B's
// netcode has every player in the same level; in co-op each goes where they
// like, and an object from another level (a Banjo, a vehicle and its parts)
// names assets this level does not have, which crashed. A player's objects
// are built here only while they are in this game's scene; their game takes
// them away with its level when they leave.
REX_HOOK_RAW(sub_8239E220) {
  const uint32_t netobj = ctx.r3.u32;
  if (netobj && !Load32(base, netobj + kNetObjEntity) && base[kLevelLoaded] &&
      coop::CurrentRole() != coop::Role::kNone) {
    static std::map<uint32_t, bool> held;  // logged once each
    uint32_t player;
    if (FromOtherScene(ctx, base, netobj, player)) {
      if (!held[netobj]) {
        held[netobj] = true;
        RONL_INFO("co-op: player {}'s object {:08X} is in another scene: not built here", int(player), netobj);
      }
      ctx.r3.u64 = 0;
      return;
    }
    if (held.erase(netobj))
      RONL_INFO("co-op: player {}'s object {:08X} is in this scene now: built here", int(player), netobj);
  }
  __imp__sub_8239E220(ctx, base);
}

// BOOL sub_822BBF08(app, ...): closes an app scene (slots 0x82FAB05C, 32 bytes
// each; an app's + 0 is its scene script). Online play's frontend stays open
// through a co-op scene load.
REX_HOOK_RAW(sub_822BBF08) {
  const uint32_t app = ctx.r3.u32;
  if (g_keeping_frontend && app) {
    const uint32_t script = Load32(base, app);
    if (script && script == Load32(base, kFrontendScript)) {
      RONL_INFO("co-op: online play's frontend stays open through the scene change");
      ctx.r3.u64 = 0;
      return;
    }
    RONL_INFO("co-op: app scene {:08X} closes for the scene change", script);
  }
  __imp__sub_822BBF08(ctx, base);
}

// Challenges in a co-op session (coop::OfflineChallenge): challengeManBegin
// (sub_8251FF78; online it takes every session player from the online game,
// sub_824D3CC8), challengeManSetState (sub_82521018), challengeManReset
// (sub_82521B70) run as single player; challengeManTick and Teardown do in
// frameHooks.cpp, which hooks them. challengeCreateNetObject (sub_82522940:
// the challenge manager's network object, which runs one challenge for the
// whole session) makes none: each player's challenge is their own.
REX_EXTERN(__imp__sub_8251FF78);
// A challenge player 1 starts itself (its manager idle: not a retry, which
// starts it again through challengeManReset) is offered to the others.
REX_HOOK_RAW(sub_8251FF78) {
  coop::OfflineChallenge offline(base);
  const uint32_t man = ctx.r3.u32;
  const uint32_t challenge = ctx.r4.u32 ? Load32(base, ctx.r4.u32) : 0;
  const uint32_t flag = ctx.r5.u32;
  const bool fresh = man && Load32(base, man + kChallengeState) == 0;
  if (coop::CurrentRole() != coop::Role::kNone) {
    RONL_INFO("co-op: a challenge starts here (challenge {:08X}), this player's own copy", challenge);
    if (fresh && challenge)
      OfferChallenge(base, challenge, flag);
  }
  __imp__sub_8251FF78(ctx, base);
}

// Back to no challenge (state 0): player 1's challenge group is over.
REX_EXTERN(__imp__sub_82521018);
REX_HOOK_RAW(sub_82521018) {
  coop::OfflineChallenge offline(base);
  const uint32_t state = ctx.r4.u32;
  __imp__sub_82521018(ctx, base);
  if (state == 0 && coop::CurrentRole() != coop::Role::kNone) {
    std::lock_guard lock(g_mutex);
    g_group = {};
    g_ready_man = 0;
  }
}

REX_EXTERN(__imp__sub_82521B70);
REX_HOOK_RAW(sub_82521B70) {
  coop::OfflineChallenge offline(base);
  __imp__sub_82521B70(ctx, base);
}

REX_EXTERN(__imp__sub_82522940);
REX_HOOK_RAW(sub_82522940) {
  if (coop::CurrentRole() != coop::Role::kNone)
    return;
  __imp__sub_82522940(ctx, base);
}

// The challenge's functions the rest of the game calls (its intro screen, the
// players' goals and results, script requests) run as single player too: one
// taking its online way left the offline challenge waiting. sub_82520598
// (player ready: online it sets a per-pad byte, + 1296, offline the
// challenge's ready, + 1348, which challengeStateWaitForPlayersReady waits
// for; called by the intro screen, sub_825C6978 / sub_825C8748: the game
// stopped taking input), challengeSetPlayerResult, challengeReqStop,
// challengeOutcomeEndsChallenge, challengeGroupAdvance / Evaluate,
// challengeStatusCombine, challengeRecountdownDone, challengeInitRequirements,
// and the restart requests.
#define RENUT_OFFLINE_CHALLENGE(fn)          \
  REX_EXTERN(__imp__##fn);                   \
  REX_HOOK_RAW(fn) {                         \
    coop::OfflineChallenge offline(base);    \
    __imp__##fn(ctx, base);                  \
  }
RENUT_OFFLINE_CHALLENGE(sub_82529E60)
// Callers of challengeManSetState from outside the manager, which check the
// multiplayer flag themselves: challengeCountdownDone (sub_82524A68, from the
// countdown screen sub_825C1618: online it goes to state 18, the online
// challenge's, whose per-player data a single-player challenge has none of:
// a write through null), meOpenLoadingEnd (sub_825259E8), sub_825A92C0
// (pause menu), sub_82520618, and the manager's setup sub_8251EBD8.
RENUT_OFFLINE_CHALLENGE(sub_82524A68)
RENUT_OFFLINE_CHALLENGE(sub_825259E8)
RENUT_OFFLINE_CHALLENGE(sub_825A92C0)
RENUT_OFFLINE_CHALLENGE(sub_82520618)
RENUT_OFFLINE_CHALLENGE(sub_8251EBD8)
RENUT_OFFLINE_CHALLENGE(sub_823FF418)
RENUT_OFFLINE_CHALLENGE(sub_8252A638)
RENUT_OFFLINE_CHALLENGE(sub_825237C8)
RENUT_OFFLINE_CHALLENGE(sub_82526B50)
RENUT_OFFLINE_CHALLENGE(sub_82523E48)
RENUT_OFFLINE_CHALLENGE(sub_82524BC0)
RENUT_OFFLINE_CHALLENGE(sub_8251EF58)
RENUT_OFFLINE_CHALLENGE(challengeReqStart_MultiPlayerResetPlayers)
RENUT_OFFLINE_CHALLENGE(challengeReqStart_ResetToGameRef)
#undef RENUT_OFFLINE_CHALLENGE

// void sub_82520598(manager, ..., player): player 1 is ready to start (see
// above); with others in its challenge the start waits for them (HoldReady).
REX_EXTERN(__imp__sub_82520598);
REX_HOOK_RAW(sub_82520598) {
  const uint32_t man = ctx.r3.u32;
  coop::OfflineChallenge offline(base);
  __imp__sub_82520598(ctx, base);
  if (man && Load32(base, man + kChallengeReadyFlag))
    HoldReady(base, man);
}

// void sub_822B8780(player, index): gives a local player its player index
// (the map sub_822B8BC0 reads). A single-player challenge start numbers the
// local players from 0; in co-op player 1 keeps its index in the session (a
// joiner's is not 0: it would take the host's).
REX_EXTERN(__imp__sub_822B8780);
REX_HOOK_RAW(sub_822B8780) {
  if (g_offline_depth > 0 && g_hidden_multiplayer)
    return;
  __imp__sub_822B8780(ctx, base);
}


// sub_822ABC28(player): writes the achievement of each "achievement unlocked"
// flag of the player's progress (+ 248 flags, 1-50; sub_822ABD28 →
// XUserWriteAchievements), as loading progress does at its end. Not while
// player 1 has the host's: those are the host's achievements.
REX_HOOK_RAW(sub_822ABC28) {
  if (!g_own_progress.empty() && ctx.r3.u32 == Load32(base, kPlayers))
    return;
  __imp__sub_822ABC28(ctx, base);
}

// int gameSaveSave(player): not while player 1 has the host's progress.
REX_HOOK_RAW(gameSaveSave) {
  if (!g_own_progress.empty() && ctx.r3.u32 == Load32(base, kPlayers)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RONL_INFO("co-op: not saving: player 1 has the host's progress");
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__gameSaveSave(ctx, base);
}

// Each frame: the game's accepted invites, then a co-op join as one; the
// host's progress to joiners who asked, and a joiner's own back after co-op.
REX_HOOK_RAW(sub_822848E0) {
  const uint32_t invites = ctx.r3.u32;
  __imp__sub_822848E0(ctx, base);
  ServeProgress(ctx, base);
  RestoreOwnProgress(ctx, base);
  if (InCoopMatch(base))
    ChallengeFrame(ctx, base);
  else
    SetPrompt({});
  std::optional<friends::Session> join;
  {
    std::lock_guard lock(g_mutex);
    join.swap(g_pending_join);
  }
  if (!join || !invites)
    return;
  if (base[invites + kInvitePending]) {
    rex::CallFrame frame(ctx);
    CancelPendingInvite(frame, base, invites);
  }
  base[invites + kInvitePending] = 1;
  Store32(base, invites + kInviteUser, 0);
  std::memcpy(base + invites + kInviteHostInfo, join->id.data(), join->id.size());
  std::memcpy(base + invites + kInviteHostInfo + 8, join->host_address.data(), join->host_address.size());
  std::memcpy(base + invites + kInviteHostInfo + 44, join->key.data(), join->key.size());
  base[invites + kInviteFromGameInvite] = 0;
  RONL_INFO("co-op: the game takes the join as an accepted invite");
}
