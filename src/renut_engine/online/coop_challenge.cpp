#include "renut_engine/online/coop_challenge.h"

#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rexfabric/game_api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "renut_engine/game/guest_call.h"
#include "renut_engine/online/net_api.h"

// The challengeMan's code these run or start (see the hooks at the end of the file).
REX_EXTERN(__imp__sub_821BD590);  // challengeManTick (challengeMan)
REX_EXTERN(__imp__sub_823D5808);  // (challengeManNetObj): the match's net object, from the tick
REX_IMPORT(sub_824FDC38, BeginHooked, void(u32, u32, u32));                  // (challengeMan, &aid, flag), part_api's hook
REX_IMPORT(__imp__sub_824FEDD8, SetChallengeState, void(u32, u32));          // (challengeMan, state)
REX_IMPORT(__imp__sub_82285DC8, PlayerIndex, u32(u32));                      // (profile): its player slot
REX_IMPORT(__imp__sub_82358CD0, Networkable, u32(u32));                      // (&objparams aid): in live_networkenum, u8
// Rewards, as the challenge's settle gives them (sub_82508290) and as the host applies a guest's.
REX_IMPORT(sub_824AC1C0, AddCounterHooked, void(u32, u32, u32, u32));        // (profile, counter, amount, quiet)
REX_IMPORT(__imp__sub_824AC030, CounterIndex, u32(u32));                     // (&counter ref): its id
REX_IMPORT(__imp__sub_824E8FA0, ProfileChanged, void(u32));                  // (profile): progress not saved
// Hooked below; their originals.
REX_EXTERN(__imp__sub_824FE248);
REX_EXTERN(__imp__sub_824FE2C8);
REX_EXTERN(__imp__sub_824FC790);
REX_EXTERN(__imp__sub_824FCA88);
REX_EXTERN(__imp__sub_82462AF8);
REX_IMPORT(__imp__sub_822859F0, RemapPlayer, u32(u32, u32));                 // (profile, player slot)
REX_IMPORT(__imp__sub_82286078, RemovePlayer, u32(u32));                     // (profile), 0: the maps swapped back
REX_IMPORT(__imp__sub_823D7280, BindNetObject, u32(u32));                    // (challengeManNetObj): bound
REX_IMPORT(__imp__sub_824FF188, KeepHighScore, void(u32, u32));              // (challengeMan, profile)
REX_IMPORT(__imp__rex_gameFlagsSetFlag, SetGameFlag, u32(u32, u32, u32, u32));  // (profile, table, flag, unsaved)

namespace renut::online::coop_challenge {
namespace {

using renut::guest::Load32;
using renut::guest::Store32;
using Clock = std::chrono::steady_clock;
using Error = std::runtime_error;

// Guest data.
constexpr uint32_t kMultiplayer = 0x82E19934;   // MultiPlayerMenu_c lives: the challenge code runs as a match's
constexpr uint32_t kRunningWorld = 0x82E19AB4;
constexpr uint32_t kWorldScene = 5552, kSceneChallengeMan = 7008;
constexpr uint32_t kProfiles = 0x82E1B280;      // player 1's profile
constexpr uint32_t kProfileCounters = 308;
// The challengeMan: its net object, running aid, state, result, the params (blocks: + 0 type, + 12 size).
constexpr uint32_t kManNet = 8, kManAid = 12, kManState = 20, kManResult = 636, kManParams = 4;
constexpr uint32_t kManSlots = 96, kSlotScore = 1428, kSlotTime = 1432, kSlotOutcome = 1444;
constexpr uint32_t kStateHold = 8, kStateCountdown = 9, kStatePlaying = 13, kStateSettle = 14;
constexpr uint32_t kBlockHighScore = 604, kBlockLowerBetter = 684;
constexpr uint32_t kNetBound = 104;             // challengeManNetObj: its challengeMan
// An object (CreateObject): networked, its net object; an objparams copy: aid, local only.
constexpr uint32_t kObjectNetworked = 28, kObjectNet = 32, kCopyAid = 136, kCopyLocal = 140;
// The strategy state a CPU racer finishes in (sub_82462AF8): set once it's over the line.
constexpr uint32_t kRacerFinished = 116;
// The start slot's position in sub_824FCFD0's frame (r1 + 0x120), from r5 after 0x824FD29C set it to r1 + 0x88.
constexpr uint32_t kStartPosition = 0x120 - 0x88;
constexpr float kSlotSpacing = 6.0f;

// Messages from C++.
constexpr char kObjectsMessage[] = "renut.coop_ch_objs";   // group, world (u32 each), then net object ids
constexpr char kProgressMessage[] = "renut.coop_progress";  // group, kind, then three u32s
constexpr int kObjectsSendFrames = 30, kGroupExpireFrames = 120;
constexpr size_t kMaxObjects = 120;
constexpr auto kRewardWindow = std::chrono::seconds(120);
enum Kind : uint32_t { kFlag, kCounter, kScore };
constexpr uint32_t kFlagTables = 2;  // normal and global game flags (not volatile)

enum Phase : int { kStarting, kWaiting, kPlaying, kHeldEnd, kReleased };

// This machine's challenge in co-op: every one is a group, its starter's or one it joined.
struct Group {
  uint32_t id = 0;
  bool starter = false;
  uint32_t slot = 0, aid = 0, flag = 0, world = 0, man = 0;
  Phase phase = kStarting;
  bool go = false, left = false, proxies = false, reported = false;
  uint32_t outcome = 0;  // what the release writes over the outcome: 0 keeps it
  Clock::time_point go_time{};
};

struct Join {
  uint32_t group, aid, flag, slot;
};

struct Pending {
  enum Type { kOffer, kReady, kOutcome, kCpuFinish, kOver } type;
  int64_t group = 0, a = 0, b = 0, c = 0;
  double seconds = 0;
  bool flag = false;
};

// A group another machine started: its world and its objects, heard in renut.coop_ch_objs.
struct RemoteGroup {
  uint32_t world = 0;
  int heard = 0;
  std::vector<uint64_t> ids;
};

// What the host gave for a group: each sender's total of a counter, the most of them already applied.
struct Rewards {
  std::map<uint32_t, uint32_t> applied;
  std::map<uint32_t, std::map<uint32_t, uint32_t>> seen;  // by sender (0 the host itself)
};

std::mutex g_mutex;
Group g_group;
std::optional<Join> g_join;           // asked for by Lua, started by the next tick
std::optional<Join> g_starting;       // the join BeginChallenge is starting now
bool g_end_run = false;
bool g_spawn_window = false;
std::set<uint32_t> g_objects;          // the starter's challenge objects (networked ones)
std::string g_objects_sent;
int g_objects_frames = 0;
std::vector<Pending> g_pending;
std::map<uint32_t, RemoteGroup> g_remote;
std::map<uint64_t, uint32_t> g_remote_ids;  // net object id: its group
int g_frame = 0;
uint32_t g_window_group = 0;          // the group whose rewards go to the host, until then
Clock::time_point g_window_until{};
std::map<uint32_t, Rewards> g_rewards;
bool g_applying = false;              // the host giving a guest's reward: not one of its own
std::atomic<int> g_scope{0};
uint32_t g_aid_slot = 0;              // a guest word for BeginChallenge's &aid, which the start screen keeps

std::optional<rexfabric::Event> g_offer, g_ready, g_outcome, g_cpu_finish, g_over;

uint8_t *Base() {
  return REX_RUNTIME()->GetMembase().TranslateVirtual<uint8_t *>(0);
}

bool Heap(uint32_t va) {
  return va >= 0x40000000 && va < 0xC0000000;
}

float LoadFloat(const uint8_t *base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void StoreFloat(uint8_t *base, uint32_t va, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Store32(base, va, bits);
}

// The running world's challengeMan, or 0.
uint32_t RunningMan(const uint8_t *base) {
  const uint32_t world = Load32(base, kRunningWorld);
  const uint32_t scene = Heap(world) ? Load32(base, world + kWorldScene) : 0;
  return Heap(scene) ? scene + kSceneChallengeMan : 0;
}

// The challenge params' main block (type 1), as the challengeMan's code finds it, or 0.
uint32_t ParamsBlock(const uint8_t *base, uint32_t man) {
  uint32_t block = Load32(base, man + kManParams);
  for (int n = 0; Heap(block) && n < 64; ++n) {
    const uint32_t type = Load32(base, block);
    if (!type)
      return 0;
    if (type == 1)
      return block;
    block += Load32(base, block + 12);
  }
  return 0;
}

uint32_t NewGroupId() {
  static std::mt19937 random{std::random_device{}()};
  uint32_t id;
  do
    id = random();
  while (!id);
  return id;
}

void Queue(Pending pending) {
  g_pending.push_back(pending);
}

// The group is gone (its challenge reset, aborted, or the world left): Lua hears it once.
void EndGroupLocked() {
  if (!g_group.id)
    return;
  Pending over{Pending::kOver};
  over.group = g_group.id;
  Queue(over);
  g_group = Group{};
  g_objects.clear();
  g_spawn_window = false;
  g_end_run = false;
}

bool WindowOpen(uint32_t &group) {
  if (!g_window_group || Clock::now() > g_window_until)
    return false;
  group = g_window_group;
  return true;
}

void SendProgress(uint32_t group, Kind kind, uint32_t a, uint32_t b, uint32_t c) {
  std::string data(20, '\0');
  const uint32_t words[] = {group, uint32_t(kind), a, b, c};
  std::memcpy(data.data(), words, sizeof(words));
  net_api::Post(0, kProgressMessage, data);
}

// The host's own reward in its group's window counts as one participant's; a repeat of one given is dropped.
bool HostOwnCounter(uint32_t group, uint32_t counter, uint32_t amount) {
  Rewards &rewards = g_rewards[group];
  uint32_t &mine = rewards.seen[0][counter];
  uint32_t &applied = rewards.applied[counter];
  mine += amount;
  if (mine <= applied)
    return true;
  applied = mine;
  return false;
}

// A guest's reward on the host: each sender's running total, the most of them given once.
void ApplyProgress(PPCContext &ctx, uint8_t *base, uint32_t from, const std::string &data) {
  if (data.size() < 20 || !net_api::Host())
    return;
  uint32_t words[5];
  std::memcpy(words, data.data(), sizeof(words));
  const uint32_t group = words[0], kind = words[1], a = words[2], b = words[3], c = words[4];
  const uint32_t profile = Load32(base, kProfiles);
  if (!group || !Heap(profile))
    return;
  renut::guest::Guest g(ctx, base);
  if (kind == kFlag) {
    if (a < kFlagTables)
      SetGameFlag(g.ctx(), base, profile, a, b, 1);
  } else if (kind == kCounter) {
    uint32_t delta = 0;
    {
      std::lock_guard lock(g_mutex);
      Rewards &rewards = g_rewards[group];
      uint32_t &seen = rewards.seen[from ? from : 1][a];
      uint32_t &applied = rewards.applied[a];
      seen += b;
      if (seen > applied) {
        delta = seen - applied;
        applied = seen;
      }
    }
    if (delta) {
      g_applying = true;
      AddCounterHooked(g.ctx(), base, profile, a, delta, 1);
      g_applying = false;
    }
  } else if (kind == kScore) {
    const uint32_t counters = Load32(base, profile + kProfileCounters);
    if (!Heap(counters) || !a || a > 4096)
      return;
    const uint32_t now = Load32(base, counters + 4 * a);
    const bool better = c ? (b && (!now || b < now)) : b > now;
    if (better) {
      Store32(base, counters + 4 * a, b);
      ProfileChanged(g.ctx(), base, profile);
    }
  }
}

// Another machine's challenge objects: whose they are and in which world.
void TakeObjects(const std::string &data) {
  if (data.size() < 8 || (data.size() - 8) % 8)
    return;
  uint32_t group, world;
  std::memcpy(&group, data.data(), 4);
  std::memcpy(&world, data.data() + 4, 4);
  if (!group)
    return;
  std::lock_guard lock(g_mutex);
  RemoteGroup &remote = g_remote[group];
  for (const uint64_t id : remote.ids)
    g_remote_ids.erase(id);
  remote.ids.clear();
  remote.world = world;
  remote.heard = g_frame;
  for (size_t at = 8; at < data.size(); at += 8) {
    uint64_t id;
    std::memcpy(&id, data.data() + at, 8);
    if (!id)
      continue;
    remote.ids.push_back(id);
    g_remote_ids[id] = group;
  }
  if (remote.ids.empty() && world == 0) {
    g_remote.erase(group);
  }
}

// The starter's networked challenge objects (their net object ids) to everyone: on change and now and then.
void SendObjects(const uint8_t *base) {
  std::string data(8, '\0');
  {
    std::lock_guard lock(g_mutex);
    if (!g_group.id || !g_group.starter || g_group.left)
      return;
    std::memcpy(data.data(), &g_group.id, 4);
    std::memcpy(data.data() + 4, &g_group.world, 4);
    size_t count = 0;
    for (const uint32_t object : g_objects) {
      if (count >= kMaxObjects)
        break;
      if (const uint64_t id = net_api::NetObjectId(base, Load32(base, object + kObjectNet))) {
        data.append(reinterpret_cast<const char *>(&id), 8);
        ++count;
      }
    }
    if (data == g_objects_sent && ++g_objects_frames < kObjectsSendFrames)
      return;
    g_objects_frames = 0;
    g_objects_sent = data;
  }
  net_api::Post(0, kObjectsMessage, data);
}

// The group's end said to the others: no ids, no world.
void SendObjectsGone(uint32_t group) {
  std::string data(8, '\0');
  std::memcpy(data.data(), &group, 4);
  net_api::Post(0, kObjectsMessage, data);
}

void Fire() {
  std::vector<Pending> pending;
  {
    std::lock_guard lock(g_mutex);
    pending.swap(g_pending);
  }
  for (const Pending &p : pending) {
    switch (p.type) {
    case Pending::kOffer:
      if (g_offer)
        g_offer->Fire(p.group, p.a, p.b, p.c);
      break;
    case Pending::kReady:
      if (g_ready)
        g_ready->Fire(p.group);
      break;
    case Pending::kOutcome:
      if (g_outcome)
        g_outcome->Fire(p.group, p.a, p.b, p.seconds, p.c, p.flag);
      break;
    case Pending::kCpuFinish:
      if (g_cpu_finish)
        g_cpu_finish->Fire(p.group, p.seconds);
      break;
    case Pending::kOver:
      if (g_over)
        g_over->Fire(p.group);
      break;
    }
  }
}

// Player 1's slot in the challengeMan's per-player arrays.
uint32_t PlayerSlot(PPCContext &ctx, uint8_t *base) {
  renut::guest::Guest g(ctx, base);
  return PlayerIndex(g.ctx(), base, Load32(base, kProfiles));
}

// The player's outcome, time and score as the settle will see them (state 14 reached).
void QueueOutcome(uint8_t *base, uint32_t man, uint32_t slot, uint32_t group, bool proxies) {
  const uint32_t at = man + kManSlots * slot;
  uint32_t outcome = Load32(base, at + kSlotOutcome);
  if (!outcome)
    outcome = Load32(base, man + kManResult);
  Pending p{Pending::kOutcome};
  p.group = group;
  p.a = Load32(base, man + kManAid);
  p.b = outcome;
  p.c = Load32(base, at + kSlotScore);
  p.seconds = LoadFloat(base, at + kSlotTime);
  p.flag = proxies;
  std::lock_guard lock(g_mutex);
  Queue(p);
}

// renut.net's co-op challenge functions (renut.net.coop_challenge_*), in Lua over the underscore ones.
constexpr const char kLua[] = R"lua(
local net = renut.net

function net.coop_here()
  local world = net._coop_here()
  return world ~= 0 and world or nil
end

local PHASES = {[0] = "starting", "waiting", "playing", "held", "released"}
function net.coop_challenge()
  local state = net._coop_ch_state()
  if state == "" then return nil end
  local f = {}
  for v in state:gmatch("[^;]+") do f[#f + 1] = math.tointeger(tonumber(v)) end
  return {group = f[1], role = f[2] == 1 and "starter" or "joiner", slot = f[3], aid = f[4], flag = f[5],
          world = f[6], phase = PHASES[f[7]]}
end

function net.coop_challenge_join(group, aid, flag, slot)
  assert(math.type(group) == "integer" and math.type(aid) == "integer", "renut.net.coop_challenge_join: group and aid are integers")
  net._coop_ch_join(group, aid, flag or 0, slot or 1)
end
function net.coop_challenge_go() net._coop_ch_go() end
function net.coop_challenge_release(won)
  net._coop_ch_release(won == nil and 0 or (won and 1 or 2))
end
function net.coop_challenge_end_run() net._coop_ch_end_run() end
function net.coop_challenge_leave() net._coop_ch_leave() end

local EVENTS = {offer = true, ready = true, outcome = true, cpu_finish = true, over = true}
function net.on_challenge(event, fn)
  assert(EVENTS[event], "renut.net.on_challenge: unknown event '" .. tostring(event) .. "'")
  assert(type(fn) == "function", "renut.net.on_challenge: fn is a function")
  if event == "outcome" then
    return rex.on("renut.coop_challenge_outcome", function(group, aid, outcome, seconds, score, proxies)
      fn(group, aid, outcome == 1, outcome, seconds, score, proxies)
    end)
  end
  return rex.on("renut.coop_challenge_" .. event, fn)
end
)lua";

}  // namespace

SinglePlayerScope::SinglePlayerScope(uint8_t *base, bool on) : base_(base), on_(on) {
  if (!on_)
    return;
  saved_ = base_[kMultiplayer];
  base_[kMultiplayer] = 0;
  ++g_scope;
}

SinglePlayerScope::~SinglePlayerScope() {
  if (!on_)
    return;
  --g_scope;
  base_[kMultiplayer] = saved_;
}

bool Active(const uint8_t *base) {
  return g_scope.load() > 0 || net_api::CoopActive(base);
}

void Declare() {
  rexfabric::Namespace renut("renut");
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"net._coop_here", {}},
           {"net._coop_ch_state", {}},
           {"net._coop_ch_join", {"group", "aid", "flag", "slot"}},
           {"net._coop_ch_go", {}},
           {"net._coop_ch_release", {"outcome"}},
           {"net._coop_ch_end_run", {}},
           {"net._coop_ch_leave", {}},
           {"coop_challenge_offer", {"group", "aid", "flag", "world"}},
           {"coop_challenge_ready", {"group"}},
           {"coop_challenge_outcome", {"group", "aid", "outcome", "seconds", "score", "proxies"}},
           {"coop_challenge_cpu_finish", {"group", "seconds"}},
           {"coop_challenge_over", {"group"}}})
    renut.Args(name, args);

  renut.Function("net._coop_here", []() -> int64_t { return net_api::CoopWorld(Base()); });
  renut.Function("net._coop_ch_state", []() -> std::string {
    std::lock_guard lock(g_mutex);
    if (!g_group.id)
      return {};
    return std::to_string(g_group.id) + ";" + std::to_string(g_group.starter ? 1 : 2) + ";" +
           std::to_string(g_group.slot) + ";" + std::to_string(g_group.aid) + ";" + std::to_string(g_group.flag) +
           ";" + std::to_string(g_group.world) + ";" + std::to_string(int(g_group.phase));
  });
  renut.Function("net._coop_ch_join", [](int64_t group, int64_t aid, int64_t flag, int64_t slot) {
    if (group <= 0 || group > 0xFFFFFFFFll || aid <= 0 || aid > 0xFFFFFFFFll)
      throw Error("renut.net.coop_challenge_join: not a group and challenge");
    if (!net_api::CoopActive(Base()))
      throw Error("renut.net.coop_challenge_join works in a co-op world");
    std::lock_guard lock(g_mutex);
    if (g_group.id || g_join)
      throw Error("a challenge is running already");
    g_join = Join{uint32_t(group), uint32_t(aid), uint32_t(flag), uint32_t(std::clamp<int64_t>(slot, 0, 7))};
  });
  renut.Function("net._coop_ch_go", [] {
    std::lock_guard lock(g_mutex);
    if (!g_group.id || g_group.go)
      return;
    g_group.go = true;
    g_group.go_time = Clock::now();
  });
  renut.Function("net._coop_ch_release", [](int64_t outcome) {
    std::lock_guard lock(g_mutex);
    if (!g_group.id)
      return;
    g_group.outcome = outcome == 1 || outcome == 2 ? uint32_t(outcome) : 0;
    g_group.phase = kReleased;
  });
  renut.Function("net._coop_ch_end_run", [] {
    std::lock_guard lock(g_mutex);
    if (g_group.id)
      g_end_run = true;
  });
  renut.Function("net._coop_ch_leave", [] {
    std::lock_guard lock(g_mutex);
    g_join.reset();
    if (!g_group.id)
      return;
    g_group.left = true;
    g_group.go = true;
    if (g_group.phase == kHeldEnd)
      g_group.phase = kReleased;
  });
  g_offer = renut.Event("coop_challenge_offer");
  g_ready = renut.Event("coop_challenge_ready");
  g_outcome = renut.Event("coop_challenge_outcome");
  g_cpu_finish = renut.Event("coop_challenge_cpu_finish");
  g_over = renut.Event("coop_challenge_over");
  renut.Lua(kLua);
}

void NetTick(PPCContext &, uint8_t *base) {
  ++g_frame;
  const bool active = net_api::CoopActive(base);
  uint32_t gone = 0;
  {
    std::lock_guard lock(g_mutex);
    if (!active) {
      g_join.reset();
      g_remote.clear();
      g_remote_ids.clear();
      g_rewards.clear();
      g_window_group = 0;
    }
    // The group ends with its challenge: reset, aborted, or its world gone.
    if (g_group.id) {
      const uint32_t man = g_group.man;
      bool over = !active || !man || RunningMan(base) != man;
      if (!over && g_group.aid)
        over = Load32(base, man + kManAid) != g_group.aid;
      if (over) {
        if (g_group.starter)
          gone = g_group.id;
        EndGroupLocked();
      }
    }
    for (auto it = g_remote.begin(); it != g_remote.end();) {
      if (g_frame - it->second.heard < kGroupExpireFrames) {
        ++it;
        continue;
      }
      for (const uint64_t id : it->second.ids)
        g_remote_ids.erase(id);
      it = g_remote.erase(it);
    }
  }
  if (gone)
    SendObjectsGone(gone);
  if (active)
    SendObjects(base);
  Fire();
}

bool TakeMessage(PPCContext &ctx, uint8_t *base, uint32_t from, const std::string &name, const std::string &data) {
  if (name == kObjectsMessage) {
    TakeObjects(data);
    return true;
  }
  if (name == kProgressMessage) {
    ApplyProgress(ctx, base, from, data);
    return true;
  }
  return false;
}

bool RunTick(PPCContext &ctx, uint8_t *base, uint32_t man) {
  if (!net_api::CoopActive(base))
    return false;
  const bool running = man == RunningMan(base);
  // A join Lua asked for starts here, as an NPC's step would, before this frame's tick.
  std::optional<Join> join;
  bool end_run = false;
  {
    std::lock_guard lock(g_mutex);
    if (running && g_join && !Load32(base, man + kManAid)) {
      join = g_join;
      g_join.reset();
    }
    if (g_group.id && g_group.man == man && g_end_run) {
      end_run = true;
      g_end_run = false;
    }
  }
  if (join) {
    if (!g_aid_slot)
      g_aid_slot = REX_RUNTIME()->GetGuestHeap().SystemHeapAlloc(16);
    if (g_aid_slot) {
      Store32(base, g_aid_slot, join->aid);
      {
        std::lock_guard lock(g_mutex);
        g_starting = join;
      }
      renut::guest::Guest g(ctx, base);
      BeginHooked(g.ctx(), base, man, g_aid_slot, join->flag);
    }
    std::lock_guard lock(g_mutex);
    g_starting.reset();
    if (!g_group.id || g_group.id != join->group) {
      Pending over{Pending::kOver};
      over.group = join->group;
      Queue(over);
    }
  }
  // A joiner still playing when the window closes loses, as the game's own fail does.
  if (end_run) {
    const uint32_t state = Load32(base, man + kManState);
    if (state >= kStateCountdown && state <= kStatePlaying) {
      SinglePlayerScope scope(base, true);
      renut::guest::Guest g(ctx, base);
      const uint32_t slot = PlayerIndex(g.ctx(), base, Load32(base, kProfiles));
      Store32(base, man + kSlotOutcome + kManSlots * slot, 2);
      Store32(base, man + kManResult, 2);
      SetChallengeState(g.ctx(), base, man, kStateSettle);
    }
  }
  // The match's net object updates as the game's tick does, outside the single player scope.
  const uint32_t net = Load32(base, man + kManNet);
  if (net) {
    ctx.r3.u64 = net;
    __imp__sub_823D5808(ctx, base);
  }
  // The start waits at state 8 (objects spawned, no countdown) until the group goes.
  {
    std::lock_guard lock(g_mutex);
    if (g_group.id && g_group.man == man && !g_group.left && Load32(base, man + kManState) == kStateHold) {
      if (!g_group.go) {
        if (g_group.phase == kStarting) {
          g_group.phase = kWaiting;
          Pending ready{Pending::kReady};
          ready.group = g_group.id;
          Queue(ready);
        }
        return true;
      }
      g_group.phase = kPlaying;
    }
  }
  const uint32_t still = Load32(base, man + kManNet);
  if (still)
    Store32(base, man + kManNet, 0);
  {
    SinglePlayerScope scope(base, true);
    ctx.r3.u64 = man;
    __imp__sub_821BD590(ctx, base);
  }
  if (still && !Load32(base, man + kManNet))
    Store32(base, man + kManNet, still);
  return true;
}

bool BeginStarting(const uint8_t *base, uint32_t man) {
  if (!Active(base))
    return false;
  std::lock_guard lock(g_mutex);
  if (g_group.id)
    EndGroupLocked();
  Group group;
  if (g_starting) {
    group.id = g_starting->group;
    group.slot = g_starting->slot;
    g_starting.reset();
  } else {
    group.id = NewGroupId();
    group.starter = true;
  }
  group.man = man;
  group.world = net_api::CoopWorld(base);
  g_group = group;
  g_objects.clear();
  g_objects_sent.clear();
  return true;
}

void BeginDone(const uint8_t *base, uint32_t man, uint32_t flag) {
  std::lock_guard lock(g_mutex);
  if (!g_group.id || g_group.man != man)
    return;
  const uint32_t aid = Load32(base, man + kManAid);
  if (!aid) {
    g_group = Group{};
    g_objects.clear();
    return;
  }
  g_group.aid = aid;
  g_group.flag = flag;
  if (g_group.starter) {
    Pending offer{Pending::kOffer};
    offer.group = g_group.id;
    offer.a = aid;
    offer.b = flag;
    offer.c = g_group.world;
    Queue(offer);
  }
}

bool HoldSettle(PPCContext &ctx, uint8_t *base, uint32_t man) {
  bool report = false, release = false, proxies = false;
  uint32_t outcome = 0, group = 0;
  {
    std::lock_guard lock(g_mutex);
    if (!g_group.id || g_group.man != man || g_group.left)
      return false;
    group = g_group.id;
    proxies = g_group.proxies;
    report = !g_group.reported;
    g_group.reported = true;
    if (g_group.phase == kReleased) {
      release = true;
      outcome = g_group.outcome;
      g_group.outcome = 0;
      g_window_group = g_group.id;
      g_window_until = Clock::now() + kRewardWindow;
    } else {
      g_group.phase = kHeldEnd;
    }
  }
  if (!report && !release)
    return true;
  const uint32_t slot = PlayerSlot(ctx, base);
  if (report)
    QueueOutcome(base, man, slot, group, proxies);
  if (!release)
    return true;
  // Released: an outcome Lua changed (a joiner beaten by a CPU) is written before the settle reads it.
  if (outcome) {
    Store32(base, man + kSlotOutcome + kManSlots * slot, outcome);
    Store32(base, man + kManResult, outcome);
  }
  return false;
}

bool SkipObject(PPCContext &ctx, uint8_t *base, uint32_t copy, uint32_t net) {
  {
    std::lock_guard lock(g_mutex);
    if (!g_spawn_window || net || !Heap(copy) || !g_group.id || g_group.starter || g_group.left)
      return false;
  }
  if (!Load32(base, copy + kCopyAid) || Load32(base, copy + kCopyLocal))
    return false;
  bool networked;
  {
    renut::guest::Guest g(ctx, base);
    networked = (Networkable(g.ctx(), base, copy + kCopyAid) & 0xFF) != 0;
  }
  if (!networked)
    return false;
  std::lock_guard lock(g_mutex);
  g_group.proxies = true;
  return true;
}

void ObjectMade(const uint8_t *base, uint32_t object) {
  if (!Heap(object) || !Load32(base, object + kObjectNetworked))
    return;
  std::lock_guard lock(g_mutex);
  if (g_group.id && g_group.starter && !g_group.left)
    g_objects.insert(object);
}

void ObjectRemoved(uint32_t object) {
  std::lock_guard lock(g_mutex);
  g_objects.erase(object);
}

bool CounterAdded(const uint8_t *base, uint32_t profile, uint32_t counter, uint32_t amount) {
  if (g_applying || !amount || profile != Load32(base, kProfiles) || !Active(base))
    return false;
  uint32_t group;
  {
    std::lock_guard lock(g_mutex);
    if (!WindowOpen(group))
      return false;
    if (net_api::Host())
      return HostOwnCounter(group, counter, amount);
  }
  SendProgress(group, kCounter, counter, amount, 0);
  return false;
}

int ProxyVerdict(uint64_t id, uint32_t world) {
  std::lock_guard lock(g_mutex);
  const auto it = g_remote_ids.find(id);
  if (!id || it == g_remote_ids.end())
    return -1;
  const auto remote = g_remote.find(it->second);
  const bool member = g_group.id == it->second && !g_group.left;
  return member && remote != g_remote.end() && world && remote->second.world == world ? 1 : 0;
}

bool GroupInWorld(uint32_t world) {
  if (!world)
    return false;
  std::lock_guard lock(g_mutex);
  for (const auto &[group, remote] : g_remote)
    if (remote.world == world && !remote.ids.empty())
      return true;
  return false;
}

// For the hooks below.
bool InScope() {
  return g_scope.load() > 0;
}

uint32_t JoinerSlot() {
  std::lock_guard lock(g_mutex);
  return g_group.id && !g_group.starter && !g_group.left ? g_group.slot : 0;
}

void SetSpawnWindow(uint32_t man, bool open) {
  std::lock_guard lock(g_mutex);
  g_spawn_window = open && g_group.id && g_group.man == man && !g_group.starter;
}

bool ChallengeRunningHere(const uint8_t *base) {
  std::lock_guard lock(g_mutex);
  return g_group.id && g_group.man && Load32(base, g_group.man + kManAid);
}

void CpuFinished() {
  std::lock_guard lock(g_mutex);
  if (!g_group.id || !g_group.starter || !g_group.go || (g_group.phase != kPlaying && g_group.phase != kHeldEnd))
    return;
  Pending p{Pending::kCpuFinish};
  p.group = g_group.id;
  p.seconds = std::chrono::duration<double>(Clock::now() - g_group.go_time).count();
  Queue(p);
}

// A guest's flag or best time in its group's reward window, for the host.
void ForwardFlag(const uint8_t *base, uint32_t profile, uint32_t table, uint32_t flag) {
  if (g_applying || table >= kFlagTables || profile != Load32(base, kProfiles) || !Active(base) || net_api::Host())
    return;
  uint32_t group;
  {
    std::lock_guard lock(g_mutex);
    if (!WindowOpen(group))
      return;
  }
  SendProgress(group, kFlag, table, flag, 0);
}

void ForwardScore(const uint8_t *base, uint32_t index, uint32_t value, bool lower) {
  if (!Active(base) || net_api::Host())
    return;
  uint32_t group;
  {
    std::lock_guard lock(g_mutex);
    if (!WindowOpen(group))
      return;
  }
  SendProgress(group, kScore, index, value, lower ? 1 : 0);
}

}  // namespace renut::online::coop_challenge

namespace cc = renut::online::coop_challenge;

// (challengeMan, &marker, profile): the start screen's "ready" (sub_82553448 calls it back), outside the tick.
REX_HOOK_RAW(sub_824FE248) {
  cc::SinglePlayerScope scope(base, cc::Active(base));
  __imp__sub_824FE248(ctx, base);
}

// The pause menu's quit or restart (challengeMan, restart).
REX_HOOK_RAW(sub_824FE2C8) {
  cc::SinglePlayerScope scope(base, cc::Active(base));
  __imp__sub_824FE2C8(ctx, base);
}

// (challengeMan, ...): a world's end tears its challenge down.
REX_HOOK_RAW(sub_824FC790) {
  cc::SinglePlayerScope scope(base, cc::Active(base));
  __imp__sub_824FC790(ctx, base);
}

// (challengeMan): state 7 spawns the challenge's placements (sub_82393060, sub_825096F0). On a joiner the
// networked ones aren't made: the starter's are seen instead.
REX_HOOK_RAW(sub_824FCA88) {
  const uint32_t man = ctx.r3.u32;
  cc::SetSpawnWindow(man, true);
  __imp__sub_824FCA88(ctx, base);
  cc::SetSpawnWindow(man, false);
}

// (racer state, vehicle, actor): a CPU racer's race; + 116 is set once as it crosses the line.
REX_HOOK_RAW(sub_82462AF8) {
  const uint32_t state = ctx.r3.u32;
  const bool before = state && renut::guest::Load32(base, state + cc::kRacerFinished);
  __imp__sub_82462AF8(ctx, base);
  if (!before && state && renut::guest::Load32(base, state + cc::kRacerFinished))
    cc::CpuFinished();
}

// (profile, player slot): single player's begin maps player 1 to slot 0. In co-op the match's map stays.
uint32_t RemapPlayer_hook(uint32_t profile, uint32_t slot) {
  if (cc::InScope())
    return 0;
  return RemapPlayer(profile, slot);
}
REX_HOOK(sub_822859F0, RemapPlayer_hook);

// (profile): 0 swaps the profile maps back, as single player's reset does; not in co-op, which never swapped.
uint32_t RemovePlayer_hook(uint32_t profile) {
  if (!profile && cc::InScope())
    return 0;
  return RemovePlayer(profile);
}
REX_HOOK(sub_82286078, RemovePlayer_hook);

// (challengeManNetObj): binds the match's challenge net object to the match world's challengeMan. Not while a
// co-op challenge runs there, which never waits on it.
uint32_t BindNetObject_hook(uint32_t net) {
  const uint8_t *base = cc::Base();
  if (net && !renut::guest::Load32(base, net + cc::kNetBound) && cc::Active(base) && cc::ChallengeRunningHere(base))
    return 0;
  return BindNetObject(net);
}
REX_HOOK(sub_823D7280, BindNetObject_hook);

// (challengeMan, profile): a won challenge's best time or score (profile + 308). A guest's goes to the host.
void KeepHighScore_hook(uint32_t man, uint32_t profile) {
  const uint8_t *base = cc::Base();
  const uint32_t block = cc::Heap(man) ? cc::ParamsBlock(base, man) : 0;
  const uint32_t index = block ? CounterIndex(block + cc::kBlockHighScore) : 0;
  const uint32_t counters = cc::Heap(profile) ? renut::guest::Load32(base, profile + cc::kProfileCounters) : 0;
  const bool known = index && cc::Heap(counters);
  const uint32_t before = known ? renut::guest::Load32(base, counters + 4 * index) : 0;
  KeepHighScore(man, profile);
  if (!known || profile != renut::guest::Load32(base, cc::kProfiles))
    return;
  const uint32_t after = renut::guest::Load32(base, counters + 4 * index);
  if (after != before)
    cc::ForwardScore(base, index, after, renut::guest::Load32(base, block + cc::kBlockLowerBetter) != 0);
}
REX_HOOK(sub_824FF188, KeepHighScore_hook);

// (profile, table, flag, unsaved): sets a game flag. A guest's in its group's reward window goes to the host.
uint32_t SetGameFlag_hook(uint32_t profile, uint32_t table, uint32_t flag, uint32_t unsaved) {
  cc::ForwardFlag(cc::Base(), profile, table, flag);
  return SetGameFlag(profile, table, flag, unsaved);
}
REX_HOOK(rex_gameFlagsSetFlag, SetGameFlag_hook);

// Midasm (sub_824FCFD0, cmpw r11, r15): a start slot (type 21) is the player's when its index is their slot.
// In a co-op challenge the slot the match gave the player keeps, so index 0 is theirs too.
void rex_coopChallengeStartSlot(PPCRegister &r11, PPCRegister &r15) {
  if (cc::InScope() && r11.u32 == 0)
    r11.u64 = r15.u64;
}

// Midasm (sub_824FCFD0, after sub_823936E0 filled the slot's position): a joiner starts beside the starter.
// It takes r5 (r1 + 0x88 after the hooked addi), as a midasm may not take r1.
void rex_coopChallengeStartOffset(PPCRegister &r5) {
  if (!cc::InScope())
    return;
  const uint32_t slot = cc::JoinerSlot();
  if (!slot)
    return;
  uint8_t *base = cc::Base();
  const uint32_t at = r5.u32 + cc::kStartPosition;
  const float side = float((slot + 1) / 2) * cc::kSlotSpacing * (slot % 2 ? 1.0f : -1.0f);
  cc::StoreFloat(base, at, cc::LoadFloat(base, at) + side);
}
