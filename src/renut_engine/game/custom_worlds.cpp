#include "renut_engine/game/custom_worlds.h"

#include <rex/kernel/guest_memory.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rexfabric/game_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "renut_engine/game/custom_npcs.h"
#include "renut_engine/game/part_classes.h"
#include "renut_engine/mods/custom_parts.h"

REX_EXTERN(__imp__sub_82364768);  // adds a marker asset's records to a scene's marker list
REX_EXTERN(__imp__sub_82332D38);  // (block, size): the block grown to size, moved if need be
REX_EXTERN(__imp__sub_822D7248);  // a havok asset's mesh entry without a MOPP (type 0)
REX_EXTERN(__imp__sub_822D6780);  // a havok asset unloads
REX_EXTERN(__imp__sub_8291B4B0);  // (thread memory, size, 39): a Havok block
REX_EXTERN(__imp__sub_82933A08);  // hkpMoppCompilerInput(this)
REX_EXTERN(__imp__sub_82933ED0);  // hkpMoppUtility::buildCode(shape container, &input, 0): the code
REX_EXTERN(__imp__sub_82934250);  // hkpMoppBvTreeShape(this, child shape, code)
REX_EXTERN(__imp__sub_8240E860);  // a portal's state (0 off, 1 locked, 2 usable)
REX_EXTERN(__imp__sub_82265BD0);  // (entity, const char *name, const char *suffix or 0): plays its animation of that name
REX_EXTERN(__imp__sub_822084C8);  // (entity, 0, f1 time): sets its animation's time (1: its end)
REX_EXTERN(__imp__sub_822E8258);  // (model instance + 140, setting, value)
REX_EXTERN(__imp__sub_825CB1A0);  // (portal info panel, &{scene, ...}): fills a world door's stats
REX_EXTERN(__imp__sub_82A25780);  // (text element, const wchar_t *): sets its text
REX_EXTERN(__imp__sub_82385678);  // (water manager, background): makes the scene's water bodies as it starts
REX_EXTERN(__imp__sub_82386720);  // (water manager, setup index, plane quad or 0, regions or 0): a water body

namespace renut::game::custom_worlds {
namespace {

using Error = std::runtime_error;

// The rows of a world door's stats panel, each "got/total" (renut.worlds.add's stats).
enum StatRow { kJiggies, kTrophies, kJinjos, kNotes, kStatRows };
constexpr const char *kStatKeys[kStatRows] = {"jiggies", "trophies", "jinjos", "notes"};

struct Stat {
  int got = -1;    // -1: counted from the world's challenges
  int total = -1;  // -1: counted from the world's challenges
};

// A water body: a flat surface at `height` over x0-x1, z0-z1 (renut.worlds.add's water).
struct WaterBody {
  float x0 = 0, z0 = 0, x1 = 0, z1 = 0, height = 0;
};
constexpr size_t kMaxWater = 8;  // the water manager's bodies (sub_82386720 doesn't check)

// A world: a scene script (aid_script_<scene>, in one of its mod's resident bundles) that loads
// the world's own bundle, markers and background, as the game's worlds do.
struct World {
  std::string id;
  std::string name;
  uint32_t scene = 0;
  std::filesystem::path music;  // a sound file it loops while played in, or empty
  float music_volume = 0.8f;
  Stat stats[kStatRows];
  std::vector<WaterBody> water;  // body i takes the scene's i-th water setup (op_7D)
  uint32_t player_model = 0;     // the model Banjo is made with here (player_model), or 0 for his own
};

// A door in Showdown Town into a world.
struct Door {
  std::string id;
  std::string world;
  float position[3] = {};
  float yaw = 0;      // radians
  uint32_t look = 0;  // its portal objparams (its model, so its hologram), or 0 for the world doors'
};

constexpr uint8_t kTypeModel = 0x04;
constexpr uint8_t kTypeScript = 0x19;
constexpr uint8_t kTypeObjparams = 0x1F;

// Showdown Town's marker (aid_marker_banjox_showdowntown_main), which places its world doors.
constexpr uint32_t kShowdownTownMarker = 0x0DC3D9BF;
// A scene's marker list (sub_82364768): record count, then the end of the records (bytes from
// the list's start), then the records.
constexpr uint32_t kListCount = 0;
constexpr uint32_t kListEnd = 4;
constexpr uint32_t kListRecords = 8;
// A marker record: size, kind, id, link and group ids, position, rotation (radians, y the yaw).
constexpr uint32_t kRecordSize = 0;
constexpr uint32_t kRecordKind = 4;
constexpr uint32_t kRecordId = 6;
constexpr uint32_t kRecordLink = 8;
constexpr uint32_t kRecordGroup = 0xA;
constexpr uint32_t kRecordPosition = 0x14;
constexpr uint32_t kRecordRotation = 0x20;
// A portal (kind 28, made by sub_824F4B08): its target scene, unlock script, game flag (no flag:
// always open) and jiggy cost.
constexpr uint16_t kKindPortal = 28;
constexpr uint32_t kPortalSize = 208;
constexpr uint32_t kPortalObjparams = 0x3C;
constexpr uint32_t kPortalTarget = 0x40;
constexpr uint32_t kPortalUnlockScript = 0x44;
constexpr uint32_t kPortalFlag = 0x48;
constexpr uint32_t kPortalFlagSize = 64;
constexpr uint32_t kPortalCost = 0x88;
// The door the mods' doors are copies of: Nutty Acres' first act's (aid_objparams_banjox_portal_worlddoor).
constexpr uint16_t kTemplateDoorId = 20;
// The mods' doors' marker ids (the game's two markers use 0-19999).
constexpr uint16_t kFirstDoorId = 30000;

std::mutex g_mutex;
std::vector<World> g_worlds;
std::vector<Door> g_doors;

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

uint16_t Load16(const uint8_t *base, uint32_t va) {
  uint16_t v;
  std::memcpy(&v, base + va, 2);
  return rex::byte_swap(v);
}

void Store32(uint8_t *at, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(at, &value, 4);
}

void Store16(uint8_t *at, uint16_t value) {
  value = rex::byte_swap(value);
  std::memcpy(at, &value, 2);
}

void StoreFloat(uint8_t *at, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Store32(at, bits);
}

// The template door's record from a marker list, or nothing.
std::vector<uint8_t> TemplateDoor(const uint8_t *base, uint32_t list) {
  const uint32_t count = Load32(base, list + kListCount);
  uint32_t record = list + kListRecords;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t size = Load32(base, record + kRecordSize);
    if (size < 8)
      break;
    if (Load16(base, record + kRecordKind) == kKindPortal && size == kPortalSize &&
        Load16(base, record + kRecordId) == kTemplateDoorId)
      return {base + record, base + record + size};
    record += size;
  }
  return {};
}

// The doors to add, as the records to append.
std::vector<std::vector<uint8_t>> DoorRecords(const std::vector<uint8_t> &model) {
  std::lock_guard lock(g_mutex);
  std::vector<std::vector<uint8_t>> records;
  for (const Door &door : g_doors) {
    const auto world = std::find_if(g_worlds.begin(), g_worlds.end(), [&](const World &w) { return w.id == door.world; });
    if (world == g_worlds.end() || records.size() >= 1000)
      continue;
    std::vector<uint8_t> record = model;
    uint8_t *r = record.data();
    Store16(r + kRecordId, uint16_t(kFirstDoorId + records.size()));
    Store16(r + kRecordLink, 0);
    Store16(r + kRecordGroup, 0);
    for (int axis = 0; axis < 3; ++axis) {
      StoreFloat(r + kRecordPosition + 4 * axis, door.position[axis]);
      StoreFloat(r + kRecordRotation + 4 * axis, axis == 1 ? door.yaw : 0.0f);
    }
    if (door.look)
      Store32(r + kPortalObjparams, door.look);
    Store32(r + kPortalTarget, world->scene);
    Store32(r + kPortalUnlockScript, 0);
    std::memset(r + kPortalFlag, 0, kPortalFlagSize);
    Store32(r + kPortalCost, 0);
    records.push_back(std::move(record));
  }
  return records;
}

// A portal entity (sub_8240D198): its model instance, its flag (an index + 1; 0 for none), its state
// and its target scene. Its model's setting 1 is the door closed (1) or open (0).
constexpr uint32_t kPortalModel = 2240;
constexpr uint32_t kPortalState = 2804;
constexpr uint32_t kPortalOpening = 2820;
constexpr uint32_t kPortalFlagIndex = 2828;
constexpr uint32_t kPortalScene = 2856;
constexpr uint32_t kModelSettings = 140;
constexpr uint32_t kPortalUsable = 2;
// The animations a portal plays as it opens: from off ("openingandactivating") or locked ("opening").
constexpr uint32_t kAnimOpeningAndActivating = 0x821234A8;
constexpr uint32_t kAnimOpening = 0x821234C0;

// Whether `scene` is one of the mods' worlds.
bool IsWorldScene(uint32_t scene) {
  std::lock_guard lock(g_mutex);
  return std::any_of(g_worlds.begin(), g_worlds.end(), [&](const World &w) { return w.scene == scene; });
}

// The mod world whose scene is `scene`, copied out; false for none.
bool WorldOfScene(uint32_t scene, World *out) {
  std::lock_guard lock(g_mutex);
  const auto it = std::find_if(g_worlds.begin(), g_worlds.end(), [&](const World &w) { return w.scene == scene; });
  if (it == g_worlds.end())
    return false;
  *out = *it;
  return true;
}

// --- A world door's stats panel (PortalInfo_c, sub_825CB1A0) ----

// Its elements: the title, then the texts of the jiggies, T.T. trophies, Jinjo tokens, notes and time
// rows. The game fills them only for its own worlds' scenes.
constexpr uint32_t kInfoTitle = 196;
constexpr uint32_t kInfoRowTexts[kStatRows] = {220, 224, 228, 232};
constexpr uint32_t kInfoTimeText = 236;
constexpr uint32_t kTextFrame = 0x200;  // a guest stack frame holding the text
constexpr uint32_t kTextAt = 0x80;
constexpr size_t kTextMax = (kTextFrame - kTextAt) / 2 - 1;

std::u16string Utf16(const std::string &utf8) {
  std::u16string out;
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    uint32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
    for (size_t k = 1; k < n && i + k < utf8.size(); ++k)
      cp = cp << 6 | (uint8_t(utf8[i + k]) & 0x3F);
    out.push_back(cp < 0x10000 ? char16_t(cp) : u'?');
    i += n;
  }
  return out;
}

// Sets a text element's text, from a frame of our own below the caller's.
void SetText(PPCContext &ctx, uint8_t *base, uint32_t element, std::u16string text) {
  if (!element)
    return;
  text.resize(std::min(text.size(), kTextMax));
  const uint64_t sp = ctx.r1.u64;
  const uint32_t frame = uint32_t(sp) - kTextFrame;
  Store32(base + frame, uint32_t(sp));
  for (size_t i = 0; i <= text.size(); ++i)
    Store16(base + frame + kTextAt + 2 * uint32_t(i), i < text.size() ? uint16_t(text[i]) : 0);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = element;
  ctx.r4.u64 = frame + kTextAt;
  __imp__sub_82A25780(ctx, base);
  ctx.r1.u64 = sp;
}

// --- A world's collision: its mesh's MOPP, built as it loads ----

// A havok asset (sub_822D6708): its shape at +8 and the shape's kind at +12 (3: a mesh). NutTool
// writes a world's as a bare mesh (entry type 0, which no game file uses), without its MOPP.
constexpr uint32_t kAssetShape = 8;
constexpr uint32_t kAssetKind = 12;
constexpr uint32_t kKindMesh = 3;
constexpr uint32_t kExtendedMeshShapeVtable = 0x820033B8;
constexpr uint32_t kMeshShapeContainer = 16;  // the mesh's hkpShapeContainer
// hkpMoppCompilerInput, with chunk subdivision (+30) off: every MOPP reader takes unchunked code.
constexpr uint32_t kMoppInputSize = 48;
constexpr uint32_t kMoppInputChunks = 30;
constexpr uint32_t kBvTreeSize = 64;  // hkpMoppBvTreeShape
// A Havok object: memory size (0: not Havok's to free) at +4, references at +6, and its deleting
// destructor first in its vtable. The thread's Havok memory is at (r13)[0] + 8.
constexpr uint32_t kObjectMemorySize = 4;
constexpr uint32_t kObjectReferences = 6;
constexpr uint32_t kThreadHavokMemory = 8;
constexpr uint32_t kHavokBlockKind = 39;

std::mutex g_trees_mutex;
std::map<uint32_t, uint32_t> g_trees;  // a world's havok asset -> the bv tree built for it

// Calls the guest function at 'address' (r3, r4, r5 set by the caller); its r3.
uint32_t CallGuest(PPCContext &ctx, uint8_t *base, uint32_t address) {
  ::PPCFunc *function = renut::game::GuestFunction(address);
  if (!function)
    return 0;
  function(ctx, base);
  return ctx.r3.u32;
}

// One reference less; the object goes with its last.
void RemoveReference(PPCContext &ctx, uint8_t *base, uint32_t object) {
  if (!object || !Load16(base, object + kObjectMemorySize))
    return;
  const uint16_t references = uint16_t(Load16(base, object + kObjectReferences) - 1);
  Store16(base + object + kObjectReferences, references);
  if (references)
    return;
  ctx.r3.u64 = object;
  ctx.r4.u64 = 1;
  CallGuest(ctx, base, Load32(base, Load32(base, object)));
}

// The mesh's MOPP and the bv tree over it (as sub_82391F28 makes one), or 0.
uint32_t BuildTree(PPCContext &ctx, uint8_t *base, uint32_t mesh) {
  // 16-byte aligned, as the game's own (on its stack) is: it holds vectors.
  rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY();
  if (!memory)
    return 0;
  const uint32_t block = memory
                             ->Allocate(kMoppInputSize + 16, 0x20, rex::kernel::GuestProtect::kReadWrite,
                                        rex::kernel::GuestHeap::kSystem)
                             .value_or(0);
  if (!block)
    return 0;
  const uint32_t input = (block + 15) & ~15u;
  ctx.r3.u64 = input;
  __imp__sub_82933A08(ctx, base);
  base[input + kMoppInputChunks] = 0;
  ctx.r3.u64 = mesh + kMeshShapeContainer;
  ctx.r4.u64 = input;
  ctx.r5.u64 = 0;
  __imp__sub_82933ED0(ctx, base);
  const uint32_t code = ctx.r3.u32;
  (void)memory->Free(block);
  if (!code)
    return 0;
  ctx.r3.u64 = Load32(base, Load32(base, ctx.r13.u32) + kThreadHavokMemory);
  ctx.r4.u64 = kBvTreeSize;
  ctx.r5.u64 = kHavokBlockKind;
  __imp__sub_8291B4B0(ctx, base);
  const uint32_t tree_block = ctx.r3.u32;
  if (!tree_block) {
    RemoveReference(ctx, base, code);
    return 0;
  }
  Store16(base + tree_block + kObjectMemorySize, uint16_t(kBvTreeSize));
  ctx.r3.u64 = tree_block;
  ctx.r4.u64 = mesh;
  ctx.r5.u64 = code;
  __imp__sub_82934250(ctx, base);
  const uint32_t tree = ctx.r3.u32;
  RemoveReference(ctx, base, code);  // the tree keeps it
  return tree;
}

// --- The Lua side: renut.worlds.add, renut.doors.add ----

constexpr const char kLua[] = R"lua(
local worlds, doors = renut.worlds, renut.doors

-- renut.worlds.add{id, name, scene, music, music_volume}: a world of a mod, entered through its doors.
function worlds.add(def)
  assert(type(def) == "table", "renut.worlds.add takes a table")
  assert(type(def.id) == "string" and def.id:match("^[%w_]+$"),
         "renut.worlds.add: the world needs an id of letters, digits and _")
  local scene = def.scene or ("renutworld_" .. def.id)
  assert(type(scene) == "string" and #scene > 0, "renut.worlds.add: scene is an asset name")
  scene = scene:gsub("^aid_script_", "")
  -- Music: a file in the calling mod's folder (or a full path).
  local music = def.music or ""
  assert(type(music) == "string", "renut.worlds.add: music is a sound file's path in the mod")
  if music ~= "" and not music:match("^%a:[/\\]") and not music:match("^[/\\]") then
    local source = debug.getinfo(2, "S").source or ""
    local folder = source:match("^@?(.*)[/\\]scripts[/\\][^/\\]+$")
    assert(folder, "renut.worlds.add: music needs a full path here (not called from a mod's scripts folder)")
    music = folder .. "/" .. music
  end
  local volume = def.music_volume or 0.8
  assert(type(volume) == "number" and volume >= 0 and volume <= 1, "renut.worlds.add: music_volume is from 0 to 1")
  worlds._add(def.id, tostring(def.name or def.id), scene, music, volume)
  if def.stats ~= nil then
    worlds.stats(def.id, def.stats)
  end
  -- The player model Banjo is made with in this world (NutTool's renut-player-model, in a mod's bundles).
  if def.player_model ~= nil then
    assert(type(def.player_model) == "string", "renut.worlds.add: player_model is a player model's id")
    worlds._player_model(def.id, def.player_model)
  end
  -- Water: bodies {min = {x, z}, max = {x, z}, height = y}, the i-th using the scene's i-th water setup.
  if def.water ~= nil then
    assert(type(def.water) == "table" and #def.water <= 8, "renut.worlds.add: water is a list of up to 8 bodies")
    local shape = "renut.worlds.add: a water body is {min = {x, z}, max = {x, z}, height = y}"
    for _, b in ipairs(def.water) do
      assert(type(b) == "table" and type(b.min) == "table" and type(b.max) == "table" and type(b.height) == "number", shape)
      local x0, z0 = b.min.x or b.min[1], b.min.z or b.min[2]
      local x1, z1 = b.max.x or b.max[1], b.max.z or b.max[2]
      assert(type(x0) == "number" and type(z0) == "number" and type(x1) == "number" and type(z1) == "number", shape)
      assert(x1 > x0 and z1 > z0, "renut.worlds.add: a water body's max is past its min")
      worlds._water(def.id, x0, z0, x1, z1, b.height)
    end
  end
end

-- renut.worlds.stats(id, {jiggies, trophies, jinjos, notes}): its door's stats panel. Each is a total
-- (a number) or {got = n, total = n}; what's left out is counted from the world's challenges.
local function whole(v, what)
  assert(v == nil or (type(v) == "number" and v >= 0 and v == math.floor(v)),
         "renut.worlds.stats: " .. what .. " is a whole number, 0 or more")
  return v or -1
end
function worlds.stats(id, stats)
  assert(type(id) == "string", "renut.worlds.stats: the world's id first")
  assert(type(stats) == "table", "renut.worlds.stats: stats is a table")
  for key, v in pairs(stats) do
    assert(key == "jiggies" or key == "trophies" or key == "jinjos" or key == "notes",
           "renut.worlds.stats: its rows are jiggies, trophies, jinjos and notes")
    if type(v) == "table" then
      worlds._stat(id, key, whole(v.got, key .. ".got"), whole(v.total, key .. ".total"))
    else
      worlds._stat(id, key, -1, whole(v, key))
    end
  end
end

function worlds.list()
  local list = {}
  for i = 0, worlds._count() - 1 do
    list[#list + 1] = {id = worlds._info(i, "id"), name = worlds._info(i, "name")}
  end
  return list
end

-- The kinds of door, by their portal objparams ("": the world door). An arch is the mod's copy (its look).
local door_styles = {wall = "", palace = "banjox_portal_hornportal", hidden = "banjox_portal_mumbosmotors", arch = false}

-- renut.doors.add{world, position, yaw, style, look, id}: a door in Showdown Town into a world.
function doors.add(def)
  assert(type(def) == "table", "renut.doors.add takes a table")
  assert(type(def.world) == "string", "renut.doors.add: the door needs the id of the world it leads to")
  local p = def.position
  assert(type(p) == "table", "renut.doors.add: the door needs a position {x, y, z}")
  local x, y, z = p.x or p[1], p.y or p[2], p.z or p[3]
  assert(type(x) == "number" and type(y) == "number" and type(z) == "number",
         "renut.doors.add: position is {x, y, z}")
  local yaw = def.yaw or 0
  assert(type(yaw) == "number", "renut.doors.add: yaw is in degrees")
  local style = def.style or "wall"
  assert(door_styles[style] ~= nil, "renut.doors.add: style is wall, arch, palace or hidden")
  local look = def.look or door_styles[style]
  assert(look, "renut.doors.add: an arch door needs its look (the copy NutTool's world maker puts in the mod)")
  assert(type(look) == "string", "renut.doors.add: look is a portal objparams asset name")
  doors._add(tostring(def.id or def.world), def.world, x, y, z, yaw, (look:gsub("^aid_objparams_", "")))
end
)lua";

} // namespace

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("worlds.", "Worlds").Category("doors.", "Worlds");  // the node editor's add menu
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"worlds._add", {"id", "name", "scene", "music", "volume"}},
           {"worlds._stat", {"id:worlds.Id", "key", "got", "total"}},
           {"worlds._water", {"id:worlds.Id", "x0", "z0", "x1", "z1", "height"}},
           {"worlds._player_model", {"id:worlds.Id", "model"}},
           {"worlds._info", {"index", "key"}},
           {"doors._add", {"id", "world:worlds.Id", "x", "y", "z", "yaw", "look"}}})
    renut.Args(name, args);  // the node editor's pins
  // The node editor's list of the mods' worlds.
  renut.Choices("worlds.Id", std::function([] {
    std::vector<std::pair<std::string, std::string>> list;
    std::lock_guard lock(g_mutex);
    for (const World &world : g_worlds)
      list.emplace_back(world.name.empty() ? world.id : world.name + " (" + world.id + ")", world.id);
    return list;
  }));
  renut.Function("worlds._add", [](std::string id, std::string name, std::string scene, std::string music, double volume) {
    World world{std::move(id), std::move(name), renut::mods::AssetId(kTypeScript, scene)};
    if (!music.empty()) {
      world.music = std::filesystem::path(std::u8string(music.begin(), music.end()));
      std::error_code ec;
      if (!std::filesystem::is_regular_file(world.music, ec))
        throw Error("renut.worlds.add: no music file " + music);
    }
    world.music_volume = float(volume);
    std::lock_guard lock(g_mutex);
    const auto same = std::find_if(g_worlds.begin(), g_worlds.end(), [&](const World &w) { return w.id == world.id; });
    if (same != g_worlds.end())
      *same = std::move(world);
    else
      g_worlds.push_back(std::move(world));
  });
  renut.Function("worlds._stat", [](std::string id, std::string key, int64_t got, int64_t total) {
    const auto row = std::find_if(std::begin(kStatKeys), std::end(kStatKeys), [&](const char *k) { return key == k; });
    if (row == std::end(kStatKeys))
      throw Error("renut.worlds.stats: no row '" + key + "'");
    std::lock_guard lock(g_mutex);
    const auto world = std::find_if(g_worlds.begin(), g_worlds.end(), [&](const World &w) { return w.id == id; });
    if (world == g_worlds.end())
      throw Error("renut.worlds.stats: no world '" + id + "' (add it first)");
    Stat &stat = world->stats[row - std::begin(kStatKeys)];
    stat.got = int(std::clamp<int64_t>(got, -1, 99999));
    stat.total = int(std::clamp<int64_t>(total, -1, 99999));
  });
  renut.Function("worlds._water", [](std::string id, double x0, double z0, double x1, double z1, double height) {
    std::lock_guard lock(g_mutex);
    const auto world = std::find_if(g_worlds.begin(), g_worlds.end(), [&](const World &w) { return w.id == id; });
    if (world == g_worlds.end())
      throw Error("renut.worlds.add: no world '" + id + "'");
    if (world->water.size() >= kMaxWater)
      throw Error("renut.worlds.add: at most 8 water bodies");
    world->water.push_back({float(x0), float(z0), float(x1), float(z1), float(height)});
  });
  renut.Function("worlds._player_model", [](std::string id, std::string model) {
    std::lock_guard lock(g_mutex);
    const auto world = std::find_if(g_worlds.begin(), g_worlds.end(), [&](const World &w) { return w.id == id; });
    if (world == g_worlds.end())
      throw Error("renut.worlds.add: no world '" + id + "'");
    world->player_model = PlayerModelId(model);
  });
  renut.Function("worlds._count", [] {
    std::lock_guard lock(g_mutex);
    return int64_t(g_worlds.size());
  });
  renut.Function("worlds._info", [](int64_t index, std::string key) {
    std::lock_guard lock(g_mutex);
    if (index < 0 || size_t(index) >= g_worlds.size())
      throw Error("no world " + std::to_string(index));
    const World &world = g_worlds[size_t(index)];
    if (key == "id") return world.id;
    if (key == "name") return world.name;
    throw Error("unknown world info '" + key + "'");
  });
  renut.Function("doors._add", [](std::string id, std::string world, double x, double y, double z, double yaw,
                                  std::string look) {
    Door door{std::move(id), std::move(world), {float(x), float(y), float(z)}, float(yaw * 3.14159265358979 / 180.0),
              look.empty() ? 0u : renut::mods::AssetId(kTypeObjparams, look)};
    std::lock_guard lock(g_mutex);
    const auto same = std::find_if(g_doors.begin(), g_doors.end(), [&](const Door &d) { return d.id == door.id; });
    if (same != g_doors.end())
      *same = std::move(door);
    else
      g_doors.push_back(std::move(door));
  });
  renut.Lua(kLua);
}

bool WorldMusic(uint32_t scene, std::filesystem::path *file, float *volume) {
  if (!scene)
    return false;
  std::lock_guard lock(g_mutex);
  for (const World &world : g_worlds) {
    if (world.scene != scene || world.music.empty())
      continue;
    *file = world.music;
    *volume = world.music_volume;
    return true;
  }
  return false;
}

uint32_t PlayerModelId(const std::string &model) {
  return model.empty() ? 0 : renut::mods::AssetId(kTypeModel, "renut_player_" + model);
}

uint32_t WorldPlayerModel(uint32_t scene) {
  if (!scene)
    return 0;
  std::lock_guard lock(g_mutex);
  const auto it = std::find_if(g_worlds.begin(), g_worlds.end(), [&](const World &w) { return w.scene == scene; });
  return it == g_worlds.end() ? 0 : it->player_model;
}

uint32_t WorldScene(const std::string &id) {
  std::lock_guard lock(g_mutex);
  const auto it = std::find_if(g_worlds.begin(), g_worlds.end(), [&](const World &w) { return w.id == id; });
  return it == g_worlds.end() ? 0 : it->scene;
}

} // namespace renut::game::custom_worlds

// sub_82364768(const u32 *marker, list, s16 id offset): adds a marker asset's records to a scene's
// list and returns the list (moved as it grows). Showdown Town's gets the mods' doors after its own.
REX_HOOK_RAW(sub_82364768) {
  using namespace renut::game::custom_worlds;
  const uint32_t marker = ctx.r3.u32 ? Load32(base, ctx.r3.u32) : 0;
  __imp__sub_82364768(ctx, base);
  uint32_t list = ctx.r3.u32;
  if (marker != kShowdownTownMarker || !list)
    return;
  const std::vector<uint8_t> model = TemplateDoor(base, list);
  if (model.empty())
    return;
  const auto r4 = ctx.r4;
  for (const std::vector<uint8_t> &record : DoorRecords(model)) {
    const uint32_t end = Load32(base, list + kListEnd);
    ctx.r3.u64 = list;
    ctx.r4.u64 = end + uint32_t(record.size());
    __imp__sub_82332D38(ctx, base);
    if (!ctx.r3.u32)
      break;
    list = ctx.r3.u32;
    std::memcpy(base + list + end, record.data(), record.size());
    Store32(base + list + kListCount, Load32(base, list + kListCount) + 1);
    Store32(base + list + kListEnd, end + uint32_t(record.size()));
  }
  ctx.r3.u64 = list;
  ctx.r4 = r4;
}

// sub_8240E860(portal, state): a usable portal without a flag (the mods' doors) skips what opens one
// with a flag; it is done here as the game does it, the animation set to its end as an open door's is
// on arrival (sub_8240D198), so a world's door stands open as the game's own do.
REX_HOOK_RAW(sub_8240E860) {
  using namespace renut::game::custom_worlds;
  const uint32_t portal = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  const uint32_t before = portal ? Load32(base, portal + kPortalState) : 0;  // as the game checks it
  __imp__sub_8240E860(ctx, base);
  if (state != kPortalUsable || !portal || Load32(base, portal + kPortalFlagIndex) ||
      !IsWorldScene(Load32(base, portal + kPortalScene)))
    return;
  const auto r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5;
  const auto f1 = ctx.f1;
  if (before <= 1) {
    ctx.r3.u64 = portal;
    ctx.r4.u64 = before == 0 ? kAnimOpeningAndActivating : kAnimOpening;
    ctx.r5.u64 = 0;
    __imp__sub_82265BD0(ctx, base);
  }
  Store32(base + portal + kPortalOpening, 1);
  if (const uint32_t model = Load32(base, portal + kPortalModel)) {
    ctx.r3.u64 = model + kModelSettings;
    ctx.r4.u64 = 1;
    ctx.r5.u64 = 0;
    __imp__sub_822E8258(ctx, base);
  }
  ctx.r3.u64 = portal;
  ctx.r4.u64 = 0;
  ctx.f1.f64 = 1.0;
  __imp__sub_822084C8(ctx, base);
  ctx.r3 = r3;
  ctx.r4 = r4;
  ctx.r5 = r5;
  ctx.f1 = f1;
}

// int sub_825CB1A0(portal info panel, &{scene, ...}): a world door's stats panel opens. For a mod
// world's door, its rows get the world's stats (renut.worlds.stats) and its title the world's name.
REX_HOOK_RAW(sub_825CB1A0) {
  using namespace renut::game::custom_worlds;
  const uint32_t info = ctx.r3.u32;
  const uint32_t scene = ctx.r4.u32 ? Load32(base, ctx.r4.u32) : 0;
  __imp__sub_825CB1A0(ctx, base);
  World world;
  if (!info || !scene || !WorldOfScene(scene, &world))
    return;
  const auto r3 = ctx.r3, r4 = ctx.r4;
  const renut::game::custom_npcs::Progress p = renut::game::custom_npcs::WorldProgress(base, world.id);
  const int counted[kStatRows][2] = {
      {p.jiggies, p.jiggies_total}, {p.trophies, p.trophies_total}, {0, 0}, {p.notes, p.notes_total}};
  for (int row = 0; row < kStatRows; ++row) {
    const Stat &s = world.stats[row];
    const int got = s.got >= 0 ? s.got : counted[row][0];
    const int total = s.total >= 0 ? s.total : counted[row][1];
    const std::string text = std::to_string(got) + "/" + std::to_string(total);
    SetText(ctx, base, Load32(base, info + kInfoRowTexts[row]), std::u16string(text.begin(), text.end()));
  }
  SetText(ctx, base, Load32(base, info + kInfoTitle), Utf16(world.name));
  // Its time in the level isn't kept for mod worlds.
  SetText(ctx, base, Load32(base, info + kInfoTimeText), u"--:--");
  ctx.r3 = r3;
  ctx.r4 = r4;
}

// sub_82385678(water manager, background): the scene's water bodies are made as it starts, from its
// background model's planes. A mod world's come first, so the water simulation is built over the first.
constexpr uint32_t kWaterFrame = 0x100;
constexpr uint32_t kWaterQuad = 0x80;  // 16-aligned in the frame: four corners {x, y, z, 1}

REX_HOOK_RAW(sub_82385678) {
  using namespace renut::game::custom_worlds;
  World world;
  const uint32_t manager = ctx.r3.u32;
  if (manager && WorldOfScene(renut::game::custom_npcs::ActiveScene(), &world) && !world.water.empty()) {
    const auto r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5, r6 = ctx.r6;
    const uint64_t sp = ctx.r1.u64;
    const uint32_t frame = (uint32_t(sp) - kWaterFrame) & ~15u;
    Store32(base + frame, uint32_t(sp));
    for (size_t i = 0; i < world.water.size() && i < kMaxWater; ++i) {
      const WaterBody &b = world.water[i];
      // p0, then p0 along x, p0 along z and the far corner, as a background's plane is.
      const float corners[4][2] = {{b.x0, b.z0}, {b.x1, b.z0}, {b.x0, b.z1}, {b.x1, b.z1}};
      for (int c = 0; c < 4; ++c) {
        uint8_t *at = base + frame + kWaterQuad + 16 * c;
        StoreFloat(at, corners[c][0]);
        StoreFloat(at + 4, b.height);
        StoreFloat(at + 8, corners[c][1]);
        StoreFloat(at + 12, 1.0f);
      }
      ctx.r1.u64 = frame;
      ctx.r3.u64 = manager;
      ctx.r4.u64 = i;
      ctx.r5.u64 = frame + kWaterQuad;
      ctx.r6.u64 = 0;
      __imp__sub_82386720(ctx, base);
    }
    ctx.r1.u64 = sp;
    ctx.r3 = r3;
    ctx.r4 = r4;
    ctx.r5 = r5;
    ctx.r6 = r6;
  }
  __imp__sub_82385678(ctx, base);
}

// sub_822D7248(asset): a havok asset's bare mesh (entry type 0). A world's has no MOPP: its bv tree
// is built here, so the asset is a mesh under a MOPP as the game's own are (sub_822D73E0).
REX_HOOK_RAW(sub_822D7248) {
  using namespace renut::game::custom_worlds;
  const uint32_t asset = ctx.r3.u32;
  __imp__sub_822D7248(ctx, base);
  if (!asset || Load32(base, asset + kAssetKind) != kKindMesh)
    return;
  const uint32_t mesh = Load32(base, asset + kAssetShape);
  if (!mesh || Load32(base, mesh) != kExtendedMeshShapeVtable)
    return;
  const auto r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5;
  if (const uint32_t tree = BuildTree(ctx, base, mesh)) {
    Store32(base + asset + kAssetShape, tree);
    std::lock_guard lock(g_trees_mutex);
    g_trees[asset] = tree;
  }
  ctx.r3 = r3;
  ctx.r4 = r4;
  ctx.r5 = r5;
}

// sub_822D6780(asset): a havok asset unloads. A tree built for it goes first, while the mesh it
// holds (in the asset's packfile data) is still there.
REX_HOOK_RAW(sub_822D6780) {
  using namespace renut::game::custom_worlds;
  const uint32_t asset = ctx.r3.u32;
  uint32_t tree = 0;
  {
    std::lock_guard lock(g_trees_mutex);
    if (const auto it = g_trees.find(asset); it != g_trees.end()) {
      tree = it->second;
      g_trees.erase(it);
    }
  }
  if (tree) {
    const auto r3 = ctx.r3;
    Store32(base + asset + kAssetShape, 0);
    RemoveReference(ctx, base, tree);
    ctx.r3 = r3;
  }
  __imp__sub_822D6780(ctx, base);
}
