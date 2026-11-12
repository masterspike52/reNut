#include "renut_engine/game/custom_npcs.h"

#include <rex/kernel/guest_memory.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rexfabric/game_api.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <rexfabric/project_hooks.h>
#include "renut_engine/game/part_api.h"
#include "renut_engine/game/stereo.h"
#include "renut_engine/mods/custom_parts.h"
#include "renut_engine/mods/json.h"
#include "renut_engine/mods/mod_loader.h"

REX_EXTERN(__imp__sub_822237D0);
REX_EXTERN(__imp__sub_823F05B0);
REX_EXTERN(__imp__sub_824AC1C0);
REX_EXTERN(__imp__sub_82554828);
REX_EXTERN(__imp__sub_823ED588);
REX_EXTERN(__imp__sub_823EFCA0);
REX_EXTERN(__imp__sub_82227CC0);
REX_EXTERN(__imp__sub_82224670);
REX_EXTERN(__imp__sub_82217A50);
REX_EXTERN(__imp__sub_82265BD0); 
REX_EXTERN(__imp__sub_82223698);
REX_EXTERN(__imp__sub_823A3140);  
REX_EXTERN(__imp__sub_82384340);  

namespace renut::game::custom_npcs {
namespace {

using Error = std::runtime_error;

// System heap memory, zeroed; 0 when there's none.
uint32_t SystemAlloc(uint32_t size) {
  rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY();
  if (!memory)
    return 0;
  return memory->Allocate(size, 0x20, rex::kernel::GuestProtect::kReadWrite, rex::kernel::GuestHeap::kSystem)
      .value_or(0);
}

// Guest memory's host base.
const uint8_t *GuestBase() {
  return REX_GUEST_MEMORY()->GetView().TranslateVirtual<uint8_t *>(0);
}

constexpr uint8_t kTypeScript = 0x19;
constexpr uint8_t kTypeChardata = 0x44;
constexpr uint32_t kObjTagNames = 0x82D99454;
constexpr uint32_t kObjTagCount = 448;
constexpr uint32_t kCounterNames = 0x82D8F128;
constexpr uint32_t kCounterCount = 247;
constexpr uint32_t kFlagNamesPointer = 0x82DF1CF4;
constexpr size_t kMaxName = 64;
constexpr uint32_t kCharacterSlots = 480;
constexpr uint32_t kCharacterSlotSize = 8;
constexpr uint32_t kCharacterSlotTag = 4;
constexpr int64_t kSpeakerCount = 33;
constexpr int64_t kDefaultSpeaker = 32;
constexpr uint32_t kProfileCounters = 308;
constexpr uint32_t kJiggiesBanked = 57;
constexpr uint32_t kPopupJiggy = 1;
constexpr uint32_t kPopupPlainIcon = 9;
constexpr uint32_t kPopupFloats = 0x82D8ECC0;
constexpr uint32_t kPopupFrame = 0x80;
constexpr uint32_t kPopupNewValue = 0x54;
constexpr uint32_t kPopupLast = 0x74;

const char *const kSpareTags[] = {
    "objTag_BanjoX_Actor_Cameron",
    "objTag_BanjoX_Actor_Termite",
    "objTag_BanjoX_Actor_Minion",
    "objTag_BanjoX_Actor_PoliceUpgrade",
};

const char *const kSpareFlags[] = {
    "gameFlag_Normal_General_DLC_Reserved_20", "gameFlag_Normal_General_DLC_Reserved_19",
    "gameFlag_Normal_General_DLC_Reserved_18", "gameFlag_Normal_General_DLC_Reserved_17",
    "gameFlag_Normal_General_DLC_Reserved_16", "gameFlag_Normal_General_DLC_Reserved_15",
    "gameFlag_Normal_General_DLC_Reserved_14", "gameFlag_Normal_General_DLC_Reserved_13",
    "gameFlag_Normal_General_DLC_Reserved_12", "gameFlag_Normal_General_DLC_Reserved_11",
    "gameFlag_Normal_General_DLC_Reserved_10", "gameFlag_Normal_TestNormal1",
};

constexpr int kFirstSpareCounter = 50;
constexpr int kLastSpareCounter = 19;
constexpr uint32_t kLineDataKey = 24;
constexpr size_t kMaxKey = 64;
constexpr uint32_t kLineSpeaker = 32;
constexpr uint32_t kLinePage = 4160;
constexpr uint32_t kDialogSpeakers = 256;
constexpr uint32_t kDialogSpeakerSize = 16;
constexpr uint32_t kHandleVoices = 36;
constexpr uint32_t kMoods = 4;

struct GameVoice {
  const char *name;
  uint32_t wavebank;
};

constexpr GameVoice kGameVoices[] = {
    {"banjo", 0x128959C6},   {"kazooie", 0x12D6978B}, {"mumbo", 0x123D3A8E},         {"humba", 0x126598F9},
    {"grunty", 0x12BBE87D},  {"klungo", 0x1292F526},  {"log", 0x127DF056},           {"bottles", 0x124B99D3},
    {"jinjo", 0x124D3AE8},   {"pikelet", 0x12225223}, {"pigpig", 0x12933CDC},        {"jolly", 0x12D7BE0F},
    {"rhino", 0x1242524F},   {"piddles", 0x12AA3734}, {"blubber", 0x1254D712},       {"ghostpig", 0x12EBC69E},
    {"penguin", 0x1229FB2A}, {"boggy", 0x12FDDE66},   {"mrfit", 0x122EFDF1},         {"kingjingaling", 0x125E8DD6},
    {"thomas", 0x12EDBCC6},
};

enum Kind { kTag, kFlag, kCounter, kKinds };

constexpr const char kKnownFile[] = ".renut_custom_npcs.json";

struct Npc {
  std::string world;
  std::string id;
  std::string name;
  std::string tag;
  uint32_t scene = 0;     
  uint32_t chardata = 0;
  uint32_t speaker = 0;   
  std::string challenge;
  std::string challenge_asset;
  std::string notes_flag, beaten_flag, tt_flag;
  int notes = 0;
  int enemy_health = 0;
  std::string game_voice;
  uint32_t voice_wavebank = 0;
  std::vector<std::filesystem::path> voice;
  std::map<std::string, std::filesystem::path> sounds;
  float voice_volume = 0.9f;
  size_t last_clip = SIZE_MAX;
  std::string chardata_name;
};

struct LineSound {
  std::filesystem::path file;
  float volume = 0;
};

struct Slot {
  std::string name;
  std::string spare;
};

std::mutex g_mutex;
std::vector<Npc> g_npcs;
std::unordered_map<std::string, std::u16string> g_texts;
std::unordered_map<std::string, std::string> g_names[kKinds];
std::vector<Slot> g_known[kKinds];                             
bool g_known_loaded = false;
std::unordered_map<std::string, uint32_t> g_spare_strings;
std::unordered_map<std::string, uint32_t> g_spare_index;  
std::atomic<bool> g_any_names{false};
std::atomic<bool> g_any_texts{false};
std::atomic<uint32_t> g_active_scene{0};
std::atomic<bool> g_any_clips{false};       
std::atomic<bool> g_any_game_voices{false}; 
std::unordered_map<uint32_t, LineSound> g_line_sounds;
std::mt19937 g_random{std::random_device{}()};

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

void Store32(uint8_t *at, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(at, &value, 4);
}

double LoadFloat(const uint8_t *base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

std::string GuestString(const uint8_t *base, uint32_t va, size_t max) {
  std::string s;
  while (s.size() < max && base[va + s.size()])
    s.push_back(char(base[va + s.size()]));
  return s;
}

std::u16string Utf16(const std::string &utf8) {
  std::u16string out;
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0 && i + 3 < utf8.size()) {
      cp = (c & 0x07u) << 18 | (utf8[i + 1] & 0x3Fu) << 12 | (utf8[i + 2] & 0x3Fu) << 6 | (utf8[i + 3] & 0x3Fu);
      n = 4;
    } else if (c >= 0xE0 && i + 2 < utf8.size()) {
      cp = (c & 0x0Fu) << 12 | (utf8[i + 1] & 0x3Fu) << 6 | (utf8[i + 2] & 0x3Fu);
      n = 3;
    } else if (c >= 0xC0 && i + 1 < utf8.size()) {
      cp = (c & 0x1Fu) << 6 | (utf8[i + 1] & 0x3Fu);
      n = 2;
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(char16_t(0xD800 + (cp >> 10)));
      out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(char16_t(cp));
    }
    i += n;
  }
  return out;
}

std::vector<std::string> Spares(Kind kind) {
  std::vector<std::string> spares;
  if (kind == kTag)
    spares.assign(std::begin(kSpareTags), std::end(kSpareTags));
  else if (kind == kFlag)
    spares.assign(std::begin(kSpareFlags), std::end(kSpareFlags));
  else
    for (int n = kFirstSpareCounter; n >= kLastSpareCounter; --n)
      spares.push_back("gameCounter_BanjoX_DLC_Reserved_" + std::to_string(n));
  return spares;
}

const char *KindKey(Kind kind) {
  return kind == kFlag ? "flags" : "counters";
}

std::string JsonEscape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (uint8_t(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(uint8_t(c)));
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

void LoadKnown() {
  if (g_known_loaded)
    return;
  g_known_loaded = true;
  const std::filesystem::path file = renut::mods::ModsFolder() / kKnownFile;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec))
    return;
  std::ifstream in(file, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string error;
  const auto json = renut::mods::ParseJson(text, &error);
  if (!json)
    return;
  for (Kind kind : {kFlag, kCounter}) {
    const std::vector<std::string> spares = Spares(kind);
    for (const renut::mods::JsonValue &entry : json->Array(KindKey(kind))) {
      Slot slot{entry.String("name"), entry.String("slot")};
      const bool taken = std::any_of(g_known[kind].begin(), g_known[kind].end(),
                                     [&](const Slot &k) { return k.name == slot.name || k.spare == slot.spare; });
      if (!slot.name.empty() && !taken && std::find(spares.begin(), spares.end(), slot.spare) != spares.end())
        g_known[kind].push_back(std::move(slot));
    }
  }
}

void SaveKnown() {
  std::string text = "{\n";
  for (Kind kind : {kFlag, kCounter}) {
    text += std::string("  \"") + KindKey(kind) + "\": [\n";
    const std::vector<Slot> &known = g_known[kind];
    for (size_t i = 0; i < known.size(); ++i)
      text += "    { \"name\": \"" + JsonEscape(known[i].name) + "\", \"slot\": \"" + JsonEscape(known[i].spare) + "\" }" + (i + 1 < known.size() ? "," : "") + "\n";
    text += kind == kFlag ? "  ],\n" : "  ]\n";
  }

  text += "}\n";
  const std::filesystem::path folder = renut::mods::ModsFolder();
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::ofstream out(folder / kKnownFile, std::ios::binary | std::ios::trunc);
  out.write(text.data(), std::streamsize(text.size()));
}

std::string Assign(Kind kind, const std::string &name) {
  if (const auto it = g_names[kind].find(name); it != g_names[kind].end())
    return it->second;
  std::string spare;
  if (kind == kTag) {
    for (const std::string &s : Spares(kind)) {
      if (std::none_of(g_names[kind].begin(), g_names[kind].end(), [&](const auto &n) { return n.second == s; })) {
        spare = s;
        break;
      }
    }

  } else {
    LoadKnown();
    std::vector<Slot> &known = g_known[kind];
    const auto it = std::find_if(known.begin(), known.end(), [&](const Slot &k) { return k.name == name; });
    if (it != known.end()) {
      spare = it->spare;
    } else {
      for (const std::string &s : Spares(kind)) {
        if (std::none_of(known.begin(), known.end(), [&](const Slot &k) { return k.spare == s; })) {
          known.push_back({name, s});
          SaveKnown();
          spare = s;
          break;
        }
      }
    }
  }

  if (!spare.empty()) {
    g_names[kind][name] = spare;
    g_any_names.store(true, std::memory_order_release);
  }

  return spare;
}

uint32_t SpareString(uint8_t *base, const std::string &spare) {
  if (g_spare_strings.empty()) {
    size_t size = 0;
    for (Kind kind : {kTag, kFlag, kCounter})
      for (const std::string &s : Spares(kind))
        size += s.size() + 1;
    const uint32_t block = SystemAlloc(uint32_t(size));
    if (!block)
      return 0;
    uint32_t at = block;
    for (Kind kind : {kTag, kFlag, kCounter}) {
      for (const std::string &s : Spares(kind)) {
        std::memcpy(base + at, s.c_str(), s.size() + 1);
        g_spare_strings[s] = at;
        at += uint32_t(s.size() + 1);
      }
    }
  }

  const auto it = g_spare_strings.find(spare);
  return it != g_spare_strings.end() ? it->second : 0;
}

int64_t SpareIndex(const uint8_t *base, uint32_t names, uint32_t count, const std::string &spare) {
  if (const auto it = g_spare_index.find(spare); it != g_spare_index.end())
    return it->second;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t name = Load32(base, names + 4 * i);
    if (name && GuestString(base, name, kMaxName + 1) == spare) {
      g_spare_index[spare] = i;
      return i;
    }
  }
  return -1;
}

int ListKind(const uint8_t *base, uint32_t names) {
  if (names == kObjTagNames)
    return kTag;
  if (names == kCounterNames || names == kCounterNames + 4)
    return kCounter;
  const uint32_t flags = Load32(base, kFlagNamesPointer);
  if (flags && (names == flags || names == flags + 4))
    return kFlag;
  return -1;
}

uint32_t TagValue(const uint8_t *base, const std::string &tag) {
  const auto it = g_names[kTag].find(tag);
  if (it == g_names[kTag].end())
    return 0;
  const int64_t index = SpareIndex(base, kObjTagNames, kObjTagCount, it->second);
  return index < 0 ? 0 : uint32_t(index + 1);
}

bool IsJiggyCounter(const uint8_t *base, uint32_t counter) {
  std::lock_guard lock(g_mutex);
  for (const auto &[name, spare] : g_names[kCounter])
    if (SpareIndex(base, kCounterNames, kCounterCount, spare) == int64_t(counter))
      return true;
  return false;
}

constexpr uint32_t kProfilePointer = 0x82E1B280;
constexpr uint32_t kProfileFlags = 276;
constexpr uint32_t kFlagBits = 8;
constexpr uint32_t kFlagCount = 0x82DF1CF0;

bool FlagSet(const uint8_t *base, const std::string &name) {
  if (name.empty())
    return false;
  const auto it = g_names[kFlag].find(name);
  const uint32_t names = Load32(base, kFlagNamesPointer);
  if (it == g_names[kFlag].end() || !names)
    return false;
  const int64_t index = SpareIndex(base, names + 4, Load32(base, kFlagCount), it->second);
  const uint32_t profile = Load32(base, kProfilePointer);
  const uint32_t holder = profile ? Load32(base, profile + kProfileFlags) : 0;
  const uint32_t bits = holder ? Load32(base, holder + kFlagBits) : 0;
  if (index < 0 || !bits)
    return false;
  const uint32_t flag = uint32_t(index + 1);
  return (base[bits + (flag >> 3)] >> (flag & 7)) & 1;
}

constexpr uint32_t kEntityPosition = 0x50;
constexpr uint32_t kEntityRotation = 0xC0;
constexpr uint32_t kEntityLocal = 0x70;
constexpr uint32_t kEntityLastLocal = 0x60;
constexpr uint32_t kEntityParent = 0x1A0;
constexpr uint32_t kParentMatrix = 0x50;
constexpr uint32_t kEntityMatrixBuilt = 0x170;
constexpr uint32_t kEntityTag = 0x08;
constexpr uint32_t kEntityRigidBody = 0x7C0;
constexpr uint32_t kEntityFlags = 0x7D0;
constexpr uint32_t kPushTransform = 4;
constexpr uint32_t kAvatarVehicle = 0xC3C;
constexpr uint32_t kBodyHits = 0x358;
constexpr uint32_t kBodyMover = 0x6F8;
constexpr uint32_t kMoverMotion = 0x70;
constexpr uint32_t kMoverMotionSize = 0x40;
constexpr uint32_t kCalloutExplode = 77;
constexpr uint32_t kActorRemoval = 0x38;
constexpr double kHitSeconds = 0.75;
constexpr uint32_t kPlaceFrame = 0x100;
constexpr uint32_t kPlaceAt = 0x80;

struct Live {
  uint32_t actor = 0, body = 0;
  int health = 0;
  uint32_t hits_seen = 0;
  std::chrono::steady_clock::time_point last, last_hit;
  bool move = false, turn = false, defeat = false, defeated = false;
  float position[3] = {};
  float yaw = 0;  // degrees
  std::string anim;
};

std::map<std::pair<std::string, std::string>, Live> g_live;  // (world, id) -> it, under g_mutex
std::unordered_map<std::string, uint32_t> g_anim_names;     // a name's guest copy, under g_mutex
std::atomic<uint32_t> g_player_avatar{0};

struct Events {
  rexfabric::Event spawn, tick, hit, defeated;
};

std::optional<Events> g_events;

float FloatAt(const uint8_t *base, uint32_t va) {
  return float(LoadFloat(base, va));
}

void StoreFloat(uint8_t *at, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Store32(at, bits);
}

double YawOf(const uint8_t *base, uint32_t entity) {
  const double x = FloatAt(base, entity + kEntityRotation), y = FloatAt(base, entity + kEntityRotation + 4), z = FloatAt(base, entity + kEntityRotation + 8), w = FloatAt(base, entity + kEntityRotation + 12);
  return std::atan2(2 * (w * y + x * z), 1 - 2 * (x * x + y * y)) * 180.0 / 3.14159265358979;
}

Npc *NpcOfTag(const uint8_t *base, uint32_t tag) {
  const uint32_t scene = g_active_scene.load(std::memory_order_acquire);
  for (Npc &npc : g_npcs)
    if (npc.scene == scene && TagValue(base, npc.tag) == tag)
      return &npc;
  return nullptr;
}

uint32_t AnimName(uint8_t *base, const std::string &name) {
  if (const auto it = g_anim_names.find(name); it != g_anim_names.end())
    return it->second;
  const uint32_t at = SystemAlloc(uint32_t(name.size() + 1));
  if (at)
    std::memcpy(base + at, name.c_str(), name.size() + 1);
  return g_anim_names[name] = at;
}

template <typename F>
void InFrame(PPCContext &ctx, uint8_t *base, F &&call) {
  const uint64_t sp = ctx.r1.u64;
  const uint32_t frame = (uint32_t(sp) - kPlaceFrame) & ~15u;
  Store32(base + frame, uint32_t(sp));
  ctx.r1.u64 = frame;
  call(frame + kPlaceAt);
  ctx.r1.u64 = sp;
}

void NpcTick(PPCContext &ctx, uint8_t *base, uint32_t body, uint32_t actor) {
  if (renut::game::part_api::ModSpawned(actor))
    return;
  std::string world, id;
  bool fresh = false;
  int hits = 0;
  double dt = 0;
  {
    std::lock_guard lock(g_mutex);
    const Npc *npc = NpcOfTag(base, Load32(base, actor + kEntityTag));
    if (!npc)
      return;
    world = npc->world;
    id = npc->id;
    Live &l = g_live[{world, id}];
    const auto now = std::chrono::steady_clock::now();
    if (l.actor != actor) {
      l = Live{};
      l.actor = actor;
      l.body = body;
      l.health = npc->enemy_health;
      l.hits_seen = Load32(base, body + kBodyHits);
      l.last = now;
      fresh = true;
    }

    if (l.defeated)
      return;
    dt = std::chrono::duration<double>(now - l.last).count();
    l.last = now;
    const uint32_t seen = Load32(base, body + kBodyHits);
    if (npc->enemy_health > 0 && seen > l.hits_seen &&
        (l.last_hit.time_since_epoch().count() == 0 || std::chrono::duration<double>(now - l.last_hit).count() >= kHitSeconds)) {
      hits = 1;
      l.last_hit = now;
      l.health -= hits;
      if (l.health <= 0)
        l.defeat = true;
    }

    l.hits_seen = seen;
  }

  if (g_events) {
    const auto saved = ctx;
    if (fresh)
      g_events->spawn.Fire(world, id);
    if (hits)
      g_events->hit.Fire(world, id, int64_t(hits));
    g_events->tick.Fire(world, id, dt);
    ctx = saved;
  }

  Live l;
  {
    std::lock_guard lock(g_mutex);
    Live &live = g_live[{world, id}];
    if (live.actor != actor)
      return;
    l = live;
    live.move = live.turn = false;
    live.anim.clear();
    if (live.defeat)
      live.defeated = true;
  }

  const auto saved = ctx;
  if (l.move || l.turn) {
    const float yaw = float((l.turn ? l.yaw : YawOf(base, actor)) * 3.14159265358979 / 180.0);
    float p[3];
    for (int k = 0; k < 3; ++k)
      p[k] = l.move ? l.position[k] : FloatAt(base, actor + kEntityPosition + 4 * k);
    // Its rotation (pitch, yaw, roll), then its position: local to its parent's matrix if it has one.
    InFrame(ctx, base, [&](uint32_t at) {
      StoreFloat(base + at, 0.0f);
      StoreFloat(base + at + 4, yaw);
      StoreFloat(base + at + 8, 0.0f);
      ctx.r3.u64 = actor;
      ctx.r4.u64 = at;
      ctx.r5.u64 = 1;
      __imp__sub_82224670(ctx, base);
    });

    ctx.r3.u64 = actor;
    __imp__sub_82217A50(ctx, base);
    float local[4] = {p[0], p[1], p[2], 1.0f};
    if (ctx.r3.u32) {
      const uint32_t m = Load32(base, actor + kEntityParent) + kParentMatrix;  // rows: x, y, z, translation
      for (int c = 0; c < 4; ++c)
        local[c] = FloatAt(base, m + 48 + 4 * c) + p[2] * FloatAt(base, m + 32 + 4 * c) + p[1] * FloatAt(base, m + 16 + 4 * c) + p[0] * FloatAt(base, m + 4 * c);
    }
  
    for (int c = 0; c < 4; ++c) {
      StoreFloat(base + actor + kEntityLocal + 4 * c, local[c]);
      StoreFloat(base + actor + kEntityLastLocal + 4 * c, local[c]);
    }

    Store32(base + actor + kEntityMatrixBuilt, 0);
    if (l.move)
      if (const uint32_t mover = Load32(base, l.body + kBodyMover))
        std::memset(base + mover + kMoverMotion, 0, kMoverMotionSize);
    if (Load32(base, actor + kEntityRigidBody))
      Store32(base + actor + kEntityFlags, Load32(base, actor + kEntityFlags) | kPushTransform);
  }

  if (!l.anim.empty()) {
    uint32_t name;
    {
      std::lock_guard lock(g_mutex);
      name = AnimName(base, l.anim);
    }

    if (name) {
      ctx.r3.u64 = actor;
      ctx.r4.u64 = name;
      ctx.r5.u64 = 0;
      __imp__sub_82265BD0(ctx, base);
    }
  }

  if (l.defeat) {
    InFrame(ctx, base, [&](uint32_t at) {
      std::memset(base + at, 0, 16);  // no position: at the actor
      ctx.r3.u64 = actor;
      ctx.r4.u64 = kCalloutExplode;
      ctx.r5.u64 = at;
      __imp__sub_82223698(ctx, base);
    });

    ctx.r3.u64 = actor;
    __imp__sub_823A3140(ctx, base);
    if (!Load32(base, actor + kActorRemoval)) {  // its body kept it: removed anyway
      ctx.r3.u64 = actor;
      ctx.r4.u64 = 0;
      __imp__sub_82384340(ctx, base);
    }

    if (g_events)
      g_events->defeated.Fire(world, id);
  }

  ctx = saved;
}

Live *LiveNpc(const std::string &world, const std::string &id) {
  const auto it = g_live.find({world, id});
  return it != g_live.end() && it->second.actor && !it->second.defeated ? &it->second : nullptr;
}

void NoteLine(uint32_t line, const std::string &key) {
  std::lock_guard lock(g_mutex);
  g_line_sounds.erase(line);
  for (const Npc &npc : g_npcs) {
    for (const auto &[name, file] : npc.sounds) {
      if (key == name || key == "renut_" + npc.world + "_" + npc.id + "_" + name) {
        g_line_sounds[line] = {file, npc.voice_volume};
        return;
      }
    }
  }
}

bool PageClip(uint32_t line, uint32_t speaker, uint32_t page, std::filesystem::path *clip, float *volume) {
  std::lock_guard lock(g_mutex);
  if (const auto it = g_line_sounds.find(line); it != g_line_sounds.end()) {
    if (page == 0) {
      *clip = it->second.file;
      *volume = it->second.volume;
      g_line_sounds.erase(it);
      return true;
    }
    g_line_sounds.erase(it);
  }
  const uint32_t scene = g_active_scene.load(std::memory_order_acquire);
  for (Npc &npc : g_npcs) {
    if (npc.scene != scene || npc.speaker != speaker || npc.voice.empty())
      continue;
    size_t pick = std::uniform_int_distribution<size_t>(0, npc.voice.size() - 1)(g_random);
    if (npc.voice.size() > 1 && pick == npc.last_clip)
      pick = (pick + 1) % npc.voice.size();
    npc.last_clip = pick;
    *clip = npc.voice[pick];
    *volume = npc.voice_volume;
    return true;
  }

  return false;
}


Npc &FindNpc(const std::string &world, const std::string &id) {
  for (Npc &npc : g_npcs)
    if (npc.world == world && npc.id == id)
      return npc;
  throw Error("renut.npcs.add: no NPC '" + id + "' in world '" + world + "'");
}

bool IsSoundFile(const std::filesystem::path &path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return extension == ".ogg" || extension == ".oga" || extension == ".mp3" || extension == ".flac" || extension == ".wav";
}


constexpr const char kLua[] = R"lua(
local npcs = renut.npcs

local function name_of(value, what)
  assert(type(value) == "string" and #value > 0 and #value < 64 and value:match("^[%w_]+$"),
         "renut.npcs.add: " .. what .. " is a name of letters, digits and _, under 64 characters")
  return value
end

-- renut.npcs.add{world, id, name, tag, chardata, speaker, scene, text, challenge, voice, voice_volume, sounds}:
-- an NPC of a mod's world.
function npcs.add(def)
  assert(type(def) == "table", "renut.npcs.add takes a table")
  local world = name_of(def.world, "world (the id of the world she stands in)")
  local id = name_of(def.id, "id")
  -- Sound files: in the calling mod's folder (or full paths).
  local source = debug.getinfo(2, "S").source or ""
  local folder = source:match("^@?(.*)[/\\]scripts[/\\][^/\\]+$")
  local function mod_file(path, what)
    assert(type(path) == "string" and #path > 0, "renut.npcs.add: " .. what .. " is a sound file's path in the mod")
    if path:match("^%a:[/\\]") or path:match("^[/\\]") then
      return path
    end
    assert(folder, "renut.npcs.add: " .. what .. " needs a full path here (not called from a mod's scripts folder)")
    return folder .. "/" .. path
  end
  local tag = name_of(def.tag or ("renut_" .. world .. "_" .. id), "tag")
  local chardata = def.chardata or ("renutnpc_" .. world .. "_" .. id)
  assert(type(chardata) == "string", "renut.npcs.add: chardata is an asset name")
  local scene = def.scene or ("renutworld_" .. world)
  assert(type(scene) == "string", "renut.npcs.add: scene is an asset name")
  local speaker = def.speaker or 32
  assert(type(speaker) == "number" and speaker == math.floor(speaker) and speaker >= 0 and speaker <= 32, "renut.npcs.add: speaker is a dbCharData_Id slot, 0 to 32")
  npcs._add(world, id, tostring(def.name or id), tag, (chardata:gsub("^aid_chardata_", "")), (scene:gsub("^aid_script_", "")), speaker)
  if def.text ~= nil then
    assert(type(def.text) == "table", "renut.npcs.add: text is a table of text keys and their texts")
    for key, text in pairs(def.text) do
      assert(type(key) == "string" and key:match("^[%w_]+$"), "renut.npcs.add: text keys are letters, digits and _")
      npcs._text(key, tostring(text))
    end
  end
  local c = def.challenge
  if c ~= nil then
    assert(type(c) == "table", "renut.npcs.add: challenge is a table")
    if c.flags ~= nil then
      assert(type(c.flags) == "table", "renut.npcs.add: challenge.flags is a list of flag names")
      for _, flag in ipairs(c.flags) do
        npcs._flag(name_of(flag, "a challenge flag"))
      end
    end
    if c.jiggies ~= nil then
      npcs._jiggies(name_of(c.jiggies, "challenge.jiggies"))
    end
    local asset = c.asset or ""
    assert(type(asset) == "string", "renut.npcs.add: challenge.asset is an asset name")
    npcs._challenge(world, id, tostring(c.id or ""), (asset:gsub("^aid_challenge_", "")))
    -- For the world door's stats: flags 2-4 (notes, beaten, T.T.) and the notes it pays.
    local notes = c.notes or 0
    assert(type(notes) == "number" and notes >= 0 and notes == math.floor(notes),
           "renut.npcs.add: challenge.notes is the whole number of notes it pays")
    local f = c.flags or {}
    npcs._progress(world, id, f[2] or "", f[3] or "", f[4] or "", notes)
  end
  -- An enemy: hits wear its health down; at 0 it is beaten (renut.npcs.on("defeated")).
  local enemy = def.enemy
  assert(enemy == nil or type(enemy) == "table", "renut.npcs.add: enemy is a table {health = n}")
  local health = enemy and (enemy.health or 3) or 0
  assert(type(health) == "number" and health >= 0 and health == math.floor(health), "renut.npcs.add: enemy.health is a whole number of hits")
  npcs._enemy(world, id, health)
  -- Its voice: a game character's (by name), or clips of the mod's, one played as each page starts.
  local volume = def.voice_volume or 0.9
  assert(type(volume) == "number" and volume >= 0 and volume <= 1, "renut.npcs.add: voice_volume is from 0 to 1")
  local voice = def.voice
  assert(voice == nil or type(voice) == "string" or type(voice) == "table", "renut.npcs.add: voice is a game character's name or a list of sound files")
  npcs._voice(world, id, type(voice) == "string" and voice or "", volume)
  if type(voice) == "table" then
    for _, file in ipairs(voice) do
      npcs._clip(world, id, "", mod_file(file, "a voice clip"))
    end
  end
  if def.sounds ~= nil then
    assert(type(def.sounds) == "table", "renut.npcs.add: sounds is a table of lines and their sound files")
    for line, file in pairs(def.sounds) do
      assert(type(line) == "string" and line:match("^[%w_]+$"), "renut.npcs.add: sounds' keys are lines (greeting, objectives, win, lose, completed) or dialog keys")
      npcs._clip(world, id, line, mod_file(file, "the " .. line .. " sound"))
    end
  end
end

-- NPCs in play, as objects: npc.world, npc.id, npc.name and the methods below.
local Npc = {}
Npc.__index = Npc
local objects = {}
local function npc_of(world, id)
  local key = world .. "/" .. id
  local o = objects[key]
  if not o then
    local name = id
    for _, n in ipairs(npcs.list()) do
      if n.world == world and n.id == id then name = n.name end
    end
    o = setmetatable({world = world, id = id, name = name}, Npc)
    objects[key] = o
  end
  return o
end

function Npc:in_play() return npcs._get(self.world, self.id, 3, 0) ~= 0 end
function Npc:position()
  return renut.vec(npcs._get(self.world, self.id, 0, 0), npcs._get(self.world, self.id, 0, 1), npcs._get(self.world, self.id, 0, 2))
end
function Npc:set_position(p) npcs._place(self.world, self.id, p.x or p[1], p.y or p[2], p.z or p[3]) end
function Npc:yaw() return npcs._get(self.world, self.id, 1, 0) end
function Npc:set_yaw(degrees) npcs._turn(self.world, self.id, degrees) end
function Npc:play(anim) npcs._play(self.world, self.id, tostring(anim)) end
function Npc:health() return npcs._get(self.world, self.id, 2, 0) end
function Npc:set_health(n) npcs._set_health(self.world, self.id, math.floor(n)) end
function Npc:defeat() npcs._defeat(self.world, self.id) end
-- Its model's shape keys (renut.shapes), by name.
function Npc:shape(name) return renut.shapes.get("renutnpc_" .. self.world .. "_" .. self.id, name) end
function Npc:set_shape(name, weight) renut.shapes.set("renutnpc_" .. self.world .. "_" .. self.id, name, weight) end

-- renut.npcs.on(event, fn): spawn (npc), tick (npc, dt), hit (npc, hits), defeated (npc).
local NPC_EVENTS = {
  spawn = function(fn) return function(w, i) fn(npc_of(w, i)) end end,
  tick = function(fn) return function(w, i, dt) fn(npc_of(w, i), dt) end end,
  hit = function(fn) return function(w, i, hits) fn(npc_of(w, i), hits) end end,
  defeated = function(fn) return function(w, i) fn(npc_of(w, i)) end end,
}
function npcs.on(name, fn)
  local wrap = assert(NPC_EVENTS[name], "renut.npcs.on: unknown event '" .. tostring(name) .. "'")
  assert(type(fn) == "function", "renut.npcs.on: give a function")
  return rex.on("renut.npc_" .. name, wrap(fn))
end

-- The player (Banjo): renut.player.position(), yaw(), driving(); nil / false when not in a level.
local player = renut.player
function player.position()
  if player._get(3, 0) == 0 then return nil end
  return renut.vec(player._get(0, 0), player._get(0, 1), player._get(0, 2))
end
function player.yaw()
  if player._get(3, 0) == 0 then return nil end
  return player._get(1, 0)
end
function player.driving() return player._get(3, 0) ~= 0 and player._get(2, 0) ~= 0 end

function npcs.list()
  local list = {}
  for i = 0, npcs._count() - 1 do
    list[#list + 1] = {world = npcs._info(i, "world"), id = npcs._info(i, "id"), name = npcs._info(i, "name"),
       tag = npcs._info(i, "tag"), speaker = tonumber(npcs._info(i, "speaker")),
        challenge = npcs._info(i, "challenge"), voice = npcs._info(i, "voice")}
  end
  return list
end
)lua";

}

bool HasTexts() {
  return g_any_texts.load(std::memory_order_acquire);
}

uint32_t ActiveScene() {
  return g_active_scene.load(std::memory_order_acquire);
}

std::vector<uint32_t> VoiceWavebanks(uint32_t scene) {
  std::vector<uint32_t> wavebanks;
  if (!scene || !g_any_game_voices.load(std::memory_order_acquire))
    return wavebanks;
  std::lock_guard lock(g_mutex);
  for (const Npc &npc : g_npcs)
    if (npc.scene == scene && npc.voice_wavebank && npc.voice.empty())
      wavebanks.push_back(npc.voice_wavebank);
  return wavebanks;
}

Progress WorldProgress(const uint8_t *base, const std::string &world) {
  Progress p;
  std::lock_guard lock(g_mutex);
  for (const Npc &npc : g_npcs) {
    if (npc.world != world)
      continue;
    const bool beaten = FlagSet(base, npc.beaten_flag), tt = FlagSet(base, npc.tt_flag);
    if (!npc.beaten_flag.empty()) {
      ++p.jiggies_total;
      p.jiggies += beaten;
    }
    if (!npc.tt_flag.empty()) {
      ++p.trophies_total;
      p.trophies += tt;
    }
    p.notes_total += npc.notes;
    if (beaten || tt || FlagSet(base, npc.notes_flag))
      p.notes += npc.notes;
  }
  return p;
}

void SetPlayerAvatar(uint32_t avatar) {
  g_player_avatar.store(avatar, std::memory_order_release);
}

bool FindText(const std::string &key, std::u16string *text) {
  if (!HasTexts())
    return false;
  std::lock_guard lock(g_mutex);
  const auto it = g_texts.find(key);
  if (it == g_texts.end())
    return false;
  *text = it->second;
  return true;
}

std::string NpcTag(const std::string &world, const std::string &id) {
  std::lock_guard lock(g_mutex);
  for (const Npc &npc : g_npcs)
    if (npc.world == world && npc.id == id)
      return npc.tag;
  return {};
}

std::string NpcObjparams(const std::string &world, const std::string &id) {
  std::lock_guard lock(g_mutex);
  for (const Npc &npc : g_npcs)
    if (npc.world == world && npc.id == id)
      return npc.chardata_name;
  return {};
}

std::vector<NpcEntry> Npcs() {
  std::vector<NpcEntry> list;
  std::lock_guard lock(g_mutex);
  for (const Npc &npc : g_npcs)
    list.push_back({npc.world, npc.id, npc.name});
  return list;
}

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("npcs.", "Characters").Category("npc_", "Characters").Category("player.", "Characters");
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"npcs._add", {"world", "id", "name", "tag", "chardata", "scene", "speaker"}},
           {"npcs._text", {"key", "text"}},
           {"npcs._flag", {"name"}},
           {"npcs._jiggies", {"name"}},
           {"npcs._challenge", {"world:npcs.World", "id:npcs.Id", "challenge", "asset"}},
           {"npcs._enemy", {"world:npcs.World", "id:npcs.Id", "health"}},
           {"npcs._get", {"world:npcs.World", "id:npcs.Id", "what", "axis"}},
           {"npcs._place", {"world:npcs.World", "id:npcs.Id", "x", "y", "z"}},
           {"npcs._turn", {"world:npcs.World", "id:npcs.Id", "yaw"}},
           {"npcs._play", {"world:npcs.World", "id:npcs.Id", "anim:npcs.Animation"}},
           {"npcs._set_health", {"world:npcs.World", "id:npcs.Id", "health"}},
           {"npcs._defeat", {"world:npcs.World", "id:npcs.Id"}},
           {"player._get", {"what", "axis"}},
           {"npcs._progress", {"world:npcs.World", "id:npcs.Id", "notes_flag", "beaten_flag", "tt_flag", "notes"}},
           {"npcs._voice", {"world:npcs.World", "id:npcs.Id", "voice:npcs.Voice", "volume"}},
           {"npcs._clip", {"world:npcs.World", "id:npcs.Id", "line", "file"}},
           {"npcs._info", {"index", "key"}},
           {"npc_spawn", {"world", "id"}},
           {"npc_tick", {"world", "id", "dt"}},
           {"npc_hit", {"world", "id", "hits"}},
           {"npc_defeated", {"world", "id"}}})
    renut.Args(name, args);  // the node editor's pins
  const auto added = [](bool worlds) {
    std::vector<std::pair<std::string, std::string>> list;
    std::lock_guard lock(g_mutex);
    for (const Npc &npc : g_npcs) {
      const std::string &value = worlds ? npc.world : npc.id;
      if (std::none_of(list.begin(), list.end(), [&](const auto &entry) { return entry.second == value; }))
        list.emplace_back(worlds ? value : npc.world + ": " + value, value);
    }

    return list;
  };

  renut.Choices("npcs.World", std::function([added] { return added(true); }));
  renut.Choices("npcs.Id", std::function([added] { return added(false); }));
  renut.Choices("npcs.Animation", {{"stand", "stand"}, {"walk", "walk"}, {"run", "run"}, {"idle1", "idle1"}, {"idle2", "idle2"}, {"hit", "hit"}});

  {
    std::vector<std::pair<std::string, std::string>> voices;
    for (const GameVoice &voice : kGameVoices)
      voices.emplace_back(voice.name, voice.name);
    renut.Choices("npcs.Voice", std::move(voices));
  }

  renut.Function("npcs._add", [](std::string world, std::string id, std::string name, std::string tag, std::string chardata, std::string scene, int64_t speaker) {
    if (speaker < 0 || speaker >= kSpeakerCount)
      throw Error("renut.npcs.add: speaker " + std::to_string(speaker) + " is not a dbCharData_Id slot");
    Npc npc{std::move(world), std::move(id), std::move(name), std::move(tag), renut::mods::AssetId(kTypeScript, scene), renut::mods::AssetId(kTypeChardata, chardata), uint32_t(speaker)};
    npc.chardata_name = chardata;
    std::lock_guard lock(g_mutex);
    if (Assign(kTag, npc.tag).empty())
      throw Error("renut.npcs.add: no objTag is left for '" + npc.tag + "'");
    const auto same = std::find_if(g_npcs.begin(), g_npcs.end(), [&](const Npc &n) { return n.world == npc.world && n.id == npc.id; });
    if (same != g_npcs.end()) {
      npc.challenge = same->challenge;
      npc.challenge_asset = same->challenge_asset;
      npc.notes_flag = same->notes_flag;
      npc.beaten_flag = same->beaten_flag;
      npc.tt_flag = same->tt_flag;
      npc.notes = same->notes;
      npc.enemy_health = same->enemy_health;
      *same = std::move(npc);
    } else {
      g_npcs.push_back(std::move(npc));
    }
  });

  renut.Function("npcs._text", [](std::string key, std::string text) {
    std::lock_guard lock(g_mutex);
    g_texts[std::move(key)] = Utf16(text);
    g_any_texts.store(true, std::memory_order_release);
  });

  renut.Function("npcs._flag", [](std::string name) {
    std::lock_guard lock(g_mutex);
    if (Assign(kFlag, name).empty())
      throw Error("renut.npcs.add: no game flag is left for '" + name + "'");
  });

  renut.Function("npcs._jiggies", [](std::string name) {
    std::lock_guard lock(g_mutex);
    if (Assign(kCounter, name).empty())
      throw Error("renut.npcs.add: no game counter is left for '" + name + "'");
  });

  renut.Function("npcs._challenge", [](std::string world, std::string id, std::string challenge, std::string asset) {
    std::lock_guard lock(g_mutex);
    for (Npc &npc : g_npcs) {
      if (npc.world == world && npc.id == id) {
        npc.challenge = std::move(challenge);
        npc.challenge_asset = std::move(asset);
        break;
      }
    }
  });

  renut.Function("npcs._enemy", [](std::string world, std::string id, int64_t health) {
    std::lock_guard lock(g_mutex);
    FindNpc(world, id).enemy_health = int(std::clamp<int64_t>(health, 0, 1000000));
  });

  renut.Function("npcs._get", [](std::string world, std::string id, int64_t what, int64_t axis) -> double {
    const uint8_t *base = GuestBase();
    std::lock_guard lock(g_mutex);
    const Live *l = LiveNpc(world, id);
    if (what == 3)
      return l ? 1.0 : 0.0;
    if (!l)
      throw Error("renut.npcs: " + world + "/" + id + " isn't in play");
    if (what == 0)
      return FloatAt(base, l->actor + kEntityPosition + 4 * uint32_t(std::clamp<int64_t>(axis, 0, 2)));
    if (what == 1)
      return YawOf(base, l->actor);
    return double(l->health);
  });

  renut.Function("npcs._place", [](std::string world, std::string id, double x, double y, double z) {
    std::lock_guard lock(g_mutex);
    Live *l = LiveNpc(world, id);
    if (!l)
      throw Error("renut.npcs: " + world + "/" + id + " isn't in play");
    l->move = true;
    l->position[0] = float(x), l->position[1] = float(y), l->position[2] = float(z);
  });

  renut.Function("npcs._turn", [](std::string world, std::string id, double yaw) {
    std::lock_guard lock(g_mutex);
    Live *l = LiveNpc(world, id);
    if (!l)
      throw Error("renut.npcs: " + world + "/" + id + " isn't in play");
    l->turn = true;
    l->yaw = float(yaw);
  });

  renut.Function("npcs._play", [](std::string world, std::string id, std::string anim) {
    std::lock_guard lock(g_mutex);
    Live *l = LiveNpc(world, id);
    if (!l)
      throw Error("renut.npcs: " + world + "/" + id + " isn't in play");
    l->anim = std::move(anim);
  });

  renut.Function("npcs._set_health", [](std::string world, std::string id, int64_t health) {
    std::lock_guard lock(g_mutex);
    Live *l = LiveNpc(world, id);
    if (!l)
      throw Error("renut.npcs: " + world + "/" + id + " isn't in play");
    l->health = int(health);
    l->defeat = health <= 0;
  });

  renut.Function("npcs._defeat", [](std::string world, std::string id) {
    std::lock_guard lock(g_mutex);
    if (Live *l = LiveNpc(world, id))
      l->defeat = true;
  });

  renut.Function("player._get", [](int64_t what, int64_t axis) -> double {
    const uint8_t *base = GuestBase();
    const uint32_t avatar = g_player_avatar.load(std::memory_order_acquire);
    if (what == 3)
      return avatar ? 1.0 : 0.0;
    if (!avatar)
      throw Error("renut.player: the player isn't in a level");
    if (what == 0)
      return FloatAt(base, avatar + kEntityPosition + 4 * uint32_t(std::clamp<int64_t>(axis, 0, 2)));
    if (what == 1)
      return YawOf(base, avatar);
    return Load32(base, avatar + kAvatarVehicle) ? 1.0 : 0.0;
  });

  g_events = Events{renut.Event("npc_spawn"), renut.Event("npc_tick"), renut.Event("npc_hit"), renut.Event("npc_defeated")};
  renut.Function("npcs._progress", [](std::string world, std::string id, std::string notes_flag, std::string beaten_flag, std::string tt_flag, int64_t notes) {
    std::lock_guard lock(g_mutex);
    for (Npc &npc : g_npcs) {
      if (npc.world == world && npc.id == id) {
        npc.notes_flag = std::move(notes_flag);
        npc.beaten_flag = std::move(beaten_flag);
        npc.tt_flag = std::move(tt_flag);
        npc.notes = int(std::clamp<int64_t>(notes, 0, 1000000));
        break;
      }
    }
  });

  renut.Function("npcs._voice", [](std::string world, std::string id, std::string voice, double volume) {
    std::transform(voice.begin(), voice.end(), voice.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    uint32_t wavebank = 0;
    if (!voice.empty()) {
      std::string names;
      for (const GameVoice &v : kGameVoices) {
        names += names.empty() ? v.name : std::string(", ") + v.name;
        if (voice == v.name)
          wavebank = v.wavebank;
      }

      if (!wavebank)
        throw Error("renut.npcs.add: no game voice '" + voice + "' (" + names + ")");
    }

    std::lock_guard lock(g_mutex);
    Npc &npc = FindNpc(world, id);
    npc.game_voice = std::move(voice);
    npc.voice_wavebank = wavebank;
    npc.voice_volume = float(std::clamp(volume, 0.0, 1.0));
    if (wavebank)
      g_any_game_voices.store(true, std::memory_order_release);
  });

  renut.Function("npcs._clip", [](std::string world, std::string id, std::string line, std::string file) {
    const std::filesystem::path path(std::u8string(file.begin(), file.end()));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
      throw Error("renut.npcs.add: no sound file " + file);
    if (!IsSoundFile(path))
      throw Error("renut.npcs.add: " + file + " is not an .ogg, .mp3, .flac or .wav file");
    std::lock_guard lock(g_mutex);
    Npc &npc = FindNpc(world, id);
    if (line.empty())
      npc.voice.push_back(path);
    else
      npc.sounds[line] = path;
    g_any_clips.store(true, std::memory_order_release);
  });

  renut.Function("npcs._count", [] {
    std::lock_guard lock(g_mutex);
    return int64_t(g_npcs.size());
  });

  renut.Function("npcs._info", [](int64_t index, std::string key) {
    std::lock_guard lock(g_mutex);
    if (index < 0 || size_t(index) >= g_npcs.size())
      throw Error("no NPC " + std::to_string(index));
    const Npc &npc = g_npcs[size_t(index)];
    if (key == "world") return npc.world;
    if (key == "id") return npc.id;
    if (key == "name") return npc.name;
    if (key == "tag") return npc.tag;
    if (key == "speaker") return std::to_string(npc.speaker);
    if (key == "challenge") return npc.challenge;
    if (key == "voice") return npc.voice.empty() ? npc.game_voice : std::string("clips");
    throw Error("unknown NPC info '" + key + "'");
  });

  renut.Lua(kLua);
}

}

REX_HOOK_RAW(sub_822237D0) {
  using namespace renut::game::custom_npcs;
  const uint32_t name = ctx.r5.u32;
  if (name && g_any_names.load(std::memory_order_acquire)) {
    const int kind = ListKind(base, ctx.r4.u32);
    if (kind >= 0 && base[name]) {
      const std::string asked = GuestString(base, name, kMaxName + 1);
      std::lock_guard lock(g_mutex);
      if (const auto it = g_names[kind].find(asked); it != g_names[kind].end())
        if (const uint32_t spare = SpareString(base, it->second))
          ctx.r5.u64 = spare;
    }
  }
  __imp__sub_822237D0(ctx, base);
}

REX_HOOK_RAW(sub_823F05B0) {
  using namespace renut::game::custom_npcs;
  const uint32_t scene = ctx.r3.u32;
  const uint32_t scene_id = scene ? Load32(base, scene) : 0;
  g_active_scene.store(scene_id, std::memory_order_release);
  {
    std::lock_guard lock(g_mutex);
    g_live.clear();
  }
  __imp__sub_823F05B0(ctx, base);
  const uint32_t table = ctx.r3.u32;
  if (!table || !scene_id || !g_any_names.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(g_mutex);
  for (const Npc &npc : g_npcs) {
    if (npc.scene != scene_id)
      continue;
    const uint32_t slot = table + kCharacterSlots + kCharacterSlotSize * npc.speaker;
    Store32(base + slot, npc.chardata);
    Store32(base + slot + kCharacterSlotTag, TagValue(base, npc.tag));
  }
}

REX_HOOK_RAW(sub_823ED588) {
  using namespace renut::game::custom_npcs;
  const uint32_t line = ctx.r3.u32;
  const uint32_t data = ctx.r6.u32;
  if (line && data && g_any_clips.load(std::memory_order_acquire))
    NoteLine(line, GuestString(base, data + kLineDataKey, kMaxKey));
  __imp__sub_823ED588(ctx, base);
}

REX_HOOK_RAW(sub_823EFCA0) {
  using namespace renut::game::custom_npcs;
  const uint32_t dialog = ctx.r3.u32;
  const uint32_t line = ctx.r4.u32;
  std::filesystem::path clip;
  float volume = 0;
  const uint32_t speaker = line ? Load32(base, line + kLineSpeaker) : 0;

  if (!dialog || !line || !g_any_clips.load(std::memory_order_acquire) || speaker >= kSpeakerCount ||
      !PageClip(line, speaker, Load32(base, line + kLinePage), &clip, &volume)) {
    __imp__sub_823EFCA0(ctx, base);
    return;
  }

  const uint32_t handle = Load32(base, dialog + kDialogSpeakers + kDialogSpeakerSize * speaker);
  uint32_t cues[kMoods] = {};
  for (uint32_t m = 0; handle && m < kMoods; ++m) {
    cues[m] = Load32(base, handle + kHandleVoices + 4 * m);
    Store32(base + handle + kHandleVoices + 4 * m, 0);
  }

  __imp__sub_823EFCA0(ctx, base);
  for (uint32_t m = 0; handle && m < kMoods; ++m)
    Store32(base + handle + kHandleVoices + 4 * m, cues[m]);
  renut::game::PlayOneShot(clip, volume, speaker);
}

REX_HOOK_RAW(sub_82227CC0) {
  using namespace renut::game::custom_npcs;
  const uint32_t body = ctx.r3.u32, actor = ctx.r4.u32;
  __imp__sub_82227CC0(ctx, base);
  if (body && actor && g_any_names.load(std::memory_order_acquire)) {
    const auto r3 = ctx.r3;
    NpcTick(ctx, base, body, actor);
    ctx.r3 = r3;
  }
}

// counterAdd (profile, counter, amount, quiet); mods' rex.hook on it run around all of this.
REXFABRIC_HOOK_RAW(sub_824AC1C0) {
  using namespace renut::game::custom_npcs;
  const uint32_t profile = ctx.r3.u32;
  const uint32_t amount = ctx.r5.u32;
  const uint32_t quiet = ctx.r6.u32;

  if (!profile || !g_any_names.load(std::memory_order_acquire) || !IsJiggyCounter(base, ctx.r4.u32)) {
    __imp__sub_824AC1C0(ctx, base);
    return;
  }

  ctx.r6.u64 = 1;
  __imp__sub_824AC1C0(ctx, base);
  const uint32_t counters = Load32(base, profile + kProfileCounters);
  if (!counters)
    return;

  const uint32_t before = Load32(base, counters + 4 * kJiggiesBanked);
  ctx.r3.u64 = profile;
  ctx.r4.u64 = kJiggiesBanked;
  ctx.r5.u64 = amount;
  ctx.r6.u64 = 1;
  __imp__sub_824AC1C0(ctx, base);
  if (quiet)
    return;

  const uint64_t sp = ctx.r1.u64;
  const uint32_t frame = uint32_t(sp) - kPopupFrame;
  Store32(base + frame, uint32_t(sp));
  Store32(base + frame + kPopupNewValue, before + amount);
  Store32(base + frame + kPopupLast, 1);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = profile;
  ctx.r4.u64 = kPopupJiggy;
  ctx.r5.u64 = kPopupPlainIcon;
  ctx.r6.u64 = 1;
  ctx.r7.u64 = ~uint64_t(0);
  ctx.r8.u64 = ~uint64_t(0);
  ctx.r9.u64 = ~uint64_t(0);
  ctx.r10.u64 = before;
  ctx.f1.f64 = LoadFloat(base, kPopupFloats);
  ctx.f2.f64 = LoadFloat(base, kPopupFloats + 4);
  ctx.f3.f64 = LoadFloat(base, kPopupFloats + 8);
  __imp__sub_82554828(ctx, base);
  ctx.r1.u64 = sp;
}
