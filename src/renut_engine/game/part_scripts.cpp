/**
 * @file    game/part_scripts.cpp
 * @brief   Lua scripts of mods' new kinds of parts (see part_scripts.h).
 *
 * One Lua state, on the game thread. Each script's chunk runs once, with an
 * environment table of its own (its _ENV: the sandbox's functions, then the
 * script's globals and events); each part instance is a table in the
 * registry, keyed by its guest address.
 *
 * Vehicles (NB Studio's Showdown Town co-op research, verified there by
 * moving vehicles in a running game): a whole vehicle is one Havok rigid body,
 * [vehicle + 0x7C0] (0 while a part breaking off rebuilds it); its motion
 * state's position is at body + 0x110, with the 3x3 rotation's columns (the
 * vehicle's right, up and forward axes; parts are authored +Z front) in the
 * 0x30 bytes before it, centres of mass at + 0x10 / + 0x20 from it, the
 * orientation quaternion at + 0x40, linear velocity at + 0x90 and angular
 * velocity at + 0xA0. A teleport moves the position and both centres of mass
 * together.
 */
#include "renut_engine/game/part_scripts.h"

#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

// Lua is built as C++ (CMakeLists.txt): its headers without extern "C".
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/mods/custom_parts.h"

namespace renut::game::part_scripts {
namespace {

using renut::mods::CustomPart;
using renut::mods::JsonValue;

// Guest data: the vehicle's rigid body (see the file comment).
constexpr uint32_t kVehicleBody = 0x7C0;
constexpr uint32_t kBodyMotion = 0x110;
constexpr int32_t kMotionRotation = -0x30;  // columns: right, up, forward
constexpr uint32_t kMotionCentres[] = {0x10, 0x20};
constexpr uint32_t kMotionVelocity = 0x90;
constexpr uint32_t kMotionAngularVelocity = 0xA0;

// Effects (IDA): sub_823DC870(level, const u32 *aid, const vec4 *pos,
// double life (0: the effect's own), int, const vec4 *dir, int) spawns a
// particle effect through the level's effect system (level + 3112; its time
// scale at + 2868). Callers pass an entity's level, entity + 76 (sub_822D44C8;
// the horn reads its vehicle's the same way). Effect names are game asset refs (gameAssetRefGetAid, 0x82254D10):
// a list of tables at kAssetRefs, {?, next, count, entries}, entries
// {char name[64], u32 aid}.
constexpr uint32_t kSpawnEffect = 0x823DC870;
constexpr uint32_t kEntityLevel = 76;      // entity + 76: the level it is in
constexpr uint32_t kLevelEffects = 3112;   // level + 3112: its effect system
// sub_822C1228(const u32 *aid): whether the asset is there, without asking the
// asset manager for it (an asset it can't find stops the game).
constexpr uint32_t kAssetExists = 0x822C1228;
constexpr uint32_t kAssetRefs = 0x82F84B78;
constexpr uint32_t kAssetRefEntrySize = 68;
constexpr uint32_t kAssetRefAid = 64;

// A call may run this many Lua instructions.
constexpr int kInstructionBudget = 5'000'000;
// A script failing this often is stopped.
constexpr int kMaxErrors = 20;

uint8_t *g_base = nullptr;     // guest memory, for the API during a call
PPCContext *g_ctx = nullptr;  // the calling callback's guest context, during a call
uint32_t g_vehicle = 0;       // the calling part's vehicle, during a call (0: none)
uint32_t g_fx_scratch = 0;    // guest: a vec4 (16-byte aligned), then an aid word
lua_State *g_lua = nullptr;

struct Script {
  int env = LUA_NOREF;  // its environment table (registry ref)
  int errors = 0;
  bool stopped = false;
};
std::unordered_map<const CustomPart *, Script> g_scripts;
std::unordered_map<uint32_t, int> g_parts;  // instance -> its table (registry ref)

uint32_t Load32(uint32_t va) {
  uint32_t v;
  std::memcpy(&v, g_base + va, 4);
  return rex::byte_swap(v);
}

float LoadFloat(uint32_t va) {
  const uint32_t bits = Load32(va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void StoreFloat(uint32_t va, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  bits = rex::byte_swap(bits);
  std::memcpy(g_base + va, &bits, 4);
}

// --- vec ----------------------------------------------------------------------

constexpr const char kVecMeta[] = "renut.vec";

struct Vec {
  double x = 0, y = 0, z = 0;
};

void PushVec(lua_State *L, Vec v) {
  lua_createtable(L, 0, 3);
  lua_pushnumber(L, v.x);
  lua_setfield(L, -2, "x");
  lua_pushnumber(L, v.y);
  lua_setfield(L, -2, "y");
  lua_pushnumber(L, v.z);
  lua_setfield(L, -2, "z");
  luaL_setmetatable(L, kVecMeta);
}

Vec CheckVec(lua_State *L, int index) {
  luaL_checktype(L, index, LUA_TTABLE);
  Vec v;
  lua_getfield(L, index, "x");
  v.x = luaL_optnumber(L, -1, 0);
  lua_getfield(L, index, "y");
  v.y = luaL_optnumber(L, -1, 0);
  lua_getfield(L, index, "z");
  v.z = luaL_optnumber(L, -1, 0);
  lua_pop(L, 3);
  return v;
}

int VecNew(lua_State *L) {
  PushVec(L, {luaL_optnumber(L, 1, 0), luaL_optnumber(L, 2, 0), luaL_optnumber(L, 3, 0)});
  return 1;
}

int VecAdd(lua_State *L) {
  const Vec a = CheckVec(L, 1), b = CheckVec(L, 2);
  PushVec(L, {a.x + b.x, a.y + b.y, a.z + b.z});
  return 1;
}

int VecSub(lua_State *L) {
  const Vec a = CheckVec(L, 1), b = CheckVec(L, 2);
  PushVec(L, {a.x - b.x, a.y - b.y, a.z - b.z});
  return 1;
}

int VecMul(lua_State *L) {
  // vec * number or number * vec
  const bool first_is_number = lua_isnumber(L, 1);
  const Vec v = CheckVec(L, first_is_number ? 2 : 1);
  const double s = luaL_checknumber(L, first_is_number ? 1 : 2);
  PushVec(L, {v.x * s, v.y * s, v.z * s});
  return 1;
}

int VecDiv(lua_State *L) {
  const Vec v = CheckVec(L, 1);
  const double s = luaL_checknumber(L, 2);
  PushVec(L, {v.x / s, v.y / s, v.z / s});
  return 1;
}

int VecUnm(lua_State *L) {
  const Vec v = CheckVec(L, 1);
  PushVec(L, {-v.x, -v.y, -v.z});
  return 1;
}

int VecLength(lua_State *L) {
  const Vec v = CheckVec(L, 1);
  lua_pushnumber(L, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
  return 1;
}

int VecNormalized(lua_State *L) {
  const Vec v = CheckVec(L, 1);
  const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  PushVec(L, length > 0 ? Vec{v.x / length, v.y / length, v.z / length} : Vec{});
  return 1;
}

int VecDot(lua_State *L) {
  const Vec a = CheckVec(L, 1), b = CheckVec(L, 2);
  lua_pushnumber(L, a.x * b.x + a.y * b.y + a.z * b.z);
  return 1;
}

int VecCross(lua_State *L) {
  const Vec a = CheckVec(L, 1), b = CheckVec(L, 2);
  PushVec(L, {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x});
  return 1;
}

int VecToString(lua_State *L) {
  const Vec v = CheckVec(L, 1);
  lua_pushfstring(L, "vec(%f, %f, %f)", v.x, v.y, v.z);
  return 1;
}

// --- vehicle ------------------------------------------------------------------

constexpr const char kVehicleMeta[] = "renut.vehicle";

// The vehicle's motion state (guest), or a Lua error when it has no body now.
uint32_t Motion(lua_State *L, int index) {
  luaL_checktype(L, index, LUA_TTABLE);
  lua_getfield(L, index, "_va");
  const uint32_t vehicle = uint32_t(luaL_checkinteger(L, -1));
  lua_pop(L, 1);
  const uint32_t body = vehicle ? Load32(vehicle + kVehicleBody) : 0;
  if (body < 0x40000000 || body >= 0xC0000000)
    luaL_error(L, "the vehicle has no body right now (it is being rebuilt)");
  return body + kBodyMotion;
}

Vec ReadVec(uint32_t va) {
  return {LoadFloat(va), LoadFloat(va + 4), LoadFloat(va + 8)};
}

void WriteVec(uint32_t va, Vec v) {
  StoreFloat(va, float(v.x));
  StoreFloat(va + 4, float(v.y));
  StoreFloat(va + 8, float(v.z));
}

bool Finite(Vec v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

int VehiclePosition(lua_State *L) {
  PushVec(L, ReadVec(Motion(L, 1)));
  return 1;
}

// Moves the vehicle by d: its position and both centres of mass.
void MoveBy(uint32_t motion, Vec d) {
  WriteVec(motion, [&] {
    const Vec p = ReadVec(motion);
    return Vec{p.x + d.x, p.y + d.y, p.z + d.z};
  }());
  for (uint32_t offset : kMotionCentres) {
    const Vec c = ReadVec(motion + offset);
    WriteVec(motion + offset, {c.x + d.x, c.y + d.y, c.z + d.z});
  }
}

int VehicleMove(lua_State *L) {
  const uint32_t motion = Motion(L, 1);
  const Vec d = CheckVec(L, 2);
  if (!Finite(d))
    return luaL_error(L, "move: not a finite vector");
  MoveBy(motion, d);
  return 0;
}

int VehicleSetPosition(lua_State *L) {
  const uint32_t motion = Motion(L, 1);
  const Vec p = CheckVec(L, 2), now = ReadVec(motion);
  if (!Finite(p))
    return luaL_error(L, "set_position: not a finite vector");
  MoveBy(motion, {p.x - now.x, p.y - now.y, p.z - now.z});
  return 0;
}

int VehicleVelocity(lua_State *L) {
  PushVec(L, ReadVec(Motion(L, 1) + kMotionVelocity));
  return 1;
}

int VehicleSetVelocity(lua_State *L) {
  const uint32_t motion = Motion(L, 1);
  const Vec v = CheckVec(L, 2);
  if (!Finite(v))
    return luaL_error(L, "set_velocity: not a finite vector");
  WriteVec(motion + kMotionVelocity, v);
  return 0;
}

int VehicleAngularVelocity(lua_State *L) {
  PushVec(L, ReadVec(Motion(L, 1) + kMotionAngularVelocity));
  return 1;
}

int VehicleSetAngularVelocity(lua_State *L) {
  const uint32_t motion = Motion(L, 1);
  const Vec v = CheckVec(L, 2);
  if (!Finite(v))
    return luaL_error(L, "set_angular_velocity: not a finite vector");
  WriteVec(motion + kMotionAngularVelocity, v);
  return 0;
}

int VehicleAxis(lua_State *L, int column) {
  PushVec(L, ReadVec(Motion(L, 1) + kMotionRotation + 0x10 * column));
  return 1;
}

int VehicleRight(lua_State *L) { return VehicleAxis(L, 0); }
int VehicleUp(lua_State *L) { return VehicleAxis(L, 1); }
int VehicleForward(lua_State *L) { return VehicleAxis(L, 2); }

void PushVehicle(lua_State *L, uint32_t vehicle) {
  lua_createtable(L, 0, 1);
  lua_pushinteger(L, vehicle);
  lua_setfield(L, -2, "_va");
  luaL_setmetatable(L, kVehicleMeta);
}

// --- fx -----------------------------------------------------------------------

void StoreWord(uint32_t va, uint32_t v) {
  v = rex::byte_swap(v);
  std::memcpy(g_base + va, &v, 4);
}

// The asset ref named 'name''s aid, or 0 (the lookup gameAssetRefGetAid does).
uint32_t AssetRefAid(const char *name) {
  for (uint32_t table = Load32(kAssetRefs); table; table = Load32(table + 4)) {
    const uint32_t count = Load32(table + 8), entries = Load32(table + 12);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t entry = entries + i * kAssetRefEntrySize;
      if (std::strncmp(reinterpret_cast<const char *>(g_base + entry), name, kAssetRefAid) == 0)
        return Load32(entry + kAssetRefAid);
    }
  }
  return 0;
}

// fx.play(effect, position): the game's particle effect there. 'effect' is an
// asset ref name, or an asset id ("0x4B32520C", or a number), as
// renut_log_effects lists them. An effect the current level hasn't loaded is
// skipped (logged once): true when it played.
int FxPlay(lua_State *L) {
  if (!g_ctx || !g_vehicle)
    return luaL_error(L, "fx.play: the part is on no vehicle (effects need one to belong to)");
  uint32_t aid = 0;
  if (lua_type(L, 1) == LUA_TNUMBER) {
    aid = uint32_t(lua_tointeger(L, 1));
  } else {
    const char *name = luaL_checkstring(L, 1);
    char *end = nullptr;
    if ((name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) &&
        (aid = uint32_t(std::strtoul(name + 2, &end, 16)), end && !*end)) {
      // an asset id
    } else if (!(aid = AssetRefAid(name))) {
      return luaL_error(L, "fx.play: the game has no effect named '%s'", name);
    }
  }
  const Vec p = CheckVec(L, 2);
  if (!Finite(p))
    return luaL_error(L, "fx.play: not a finite position");
  if (!g_fx_scratch && !(g_fx_scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(32, 16)))
    return luaL_error(L, "fx.play: no guest memory");
  StoreWord(g_fx_scratch + 16, aid);
  PPCContext &ctx = *g_ctx;
  auto *dispatcher = REX_KERNEL_STATE()->function_dispatcher();
  ctx.r3.u64 = g_fx_scratch + 16;
  if (::PPCFunc *exists = dispatcher->GetFunction(kAssetExists))
    exists(ctx, g_base);
  if (!(ctx.r3.u32 & 0xFF)) {
    static std::unordered_map<uint32_t, bool> logged;
    if (!logged[aid]) {
      logged[aid] = true;
      RNUT_WARN("Part scripts: effect {:08X} isn't loaded here; fx.play skips it", aid);
    }
    lua_pushboolean(L, 0);
    return 1;
  }
  // The effect belongs to the vehicle's level.
  const uint32_t level = Load32(g_vehicle + kEntityLevel);
  if (level < 0x40000000 || level >= 0xC0000000 || !Load32(level + kLevelEffects)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RNUT_WARN("Part scripts: the vehicle's level ({:08X}) has no effect system; fx.play skips its effects",
                level);
    }
    lua_pushboolean(L, 0);
    return 1;
  }
  WriteVec(g_fx_scratch, p);
  StoreFloat(g_fx_scratch + 12, 1.0f);
  ctx.r3.u64 = level;
  ctx.r4.u64 = g_fx_scratch + 16;
  ctx.r5.u64 = g_fx_scratch;
  ctx.f1.f64 = 0.0;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = 0;
  if (::PPCFunc *spawn = dispatcher->GetFunction(kSpawnEffect))
    spawn(ctx, g_base);
  lua_pushboolean(L, 1);
  return 1;
}

// --- part -----------------------------------------------------------------------

constexpr const char kPartMeta[] = "renut.part";

int PartVehicle(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_getfield(L, 1, "_vehicle");
  const uint32_t vehicle = uint32_t(lua_tointeger(L, -1));
  lua_pop(L, 1);
  if (!vehicle)
    lua_pushnil(L);
  else
    PushVehicle(L, vehicle);
  return 1;
}

// --- the sandbox ------------------------------------------------------------

int Log(lua_State *L) {
  std::string text;
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    if (i > 1)
      text += ' ';
    text += luaL_tolstring(L, i, nullptr);
    lua_pop(L, 1);
  }
  lua_Debug where{};
  std::string source = "script";
  if (lua_getstack(L, 1, &where) && lua_getinfo(L, "Sl", &where))
    source = std::string(where.short_src) + ":" + std::to_string(where.currentline);
  RNUT_INFO("[{}] {}", source, text);
  return 0;
}

void Budget(lua_State *L, lua_Debug *) {
  luaL_error(L, "the script ran too long (over %d instructions in one call)", kInstructionBudget);
}

void Register(lua_State *L, const char *meta, const luaL_Reg *methods, const luaL_Reg *operators) {
  luaL_newmetatable(L, meta);
  if (operators)
    luaL_setfuncs(L, operators, 0);
  lua_newtable(L);
  luaL_setfuncs(L, methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
}

lua_State *Lua() {
  if (g_lua)
    return g_lua;
  lua_State *L = luaL_newstate();
  if (!L)
    return nullptr;
  // Only the libraries the sandbox gives out are opened.
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_settop(L, 0);

  static const luaL_Reg vec_methods[] = {{"length", VecLength},
                                         {"normalized", VecNormalized},
                                         {"dot", VecDot},
                                         {"cross", VecCross},
                                         {nullptr, nullptr}};
  static const luaL_Reg vec_operators[] = {{"__add", VecAdd}, {"__sub", VecSub}, {"__mul", VecMul},
                                           {"__div", VecDiv}, {"__unm", VecUnm}, {"__tostring", VecToString},
                                           {nullptr, nullptr}};
  Register(L, kVecMeta, vec_methods, vec_operators);
  static const luaL_Reg vehicle_methods[] = {{"position", VehiclePosition},
                                             {"set_position", VehicleSetPosition},
                                             {"move", VehicleMove},
                                             {"velocity", VehicleVelocity},
                                             {"set_velocity", VehicleSetVelocity},
                                             {"angular_velocity", VehicleAngularVelocity},
                                             {"set_angular_velocity", VehicleSetAngularVelocity},
                                             {"forward", VehicleForward},
                                             {"up", VehicleUp},
                                             {"right", VehicleRight},
                                             {nullptr, nullptr}};
  Register(L, kVehicleMeta, vehicle_methods, nullptr);
  static const luaL_Reg part_methods[] = {{"vehicle", PartVehicle}, {nullptr, nullptr}};
  Register(L, kPartMeta, part_methods, nullptr);
  g_lua = L;
  return L;
}

void PushJson(lua_State *L, const JsonValue &v) {
  switch (v.type) {
  case JsonValue::Type::Bool: lua_pushboolean(L, v.boolean); break;
  case JsonValue::Type::Number: lua_pushnumber(L, v.number); break;
  case JsonValue::Type::String: lua_pushlstring(L, v.string.data(), v.string.size()); break;
  case JsonValue::Type::Array:
    lua_createtable(L, int(v.array.size()), 0);
    for (size_t i = 0; i < v.array.size(); ++i) {
      PushJson(L, v.array[i]);
      lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    break;
  case JsonValue::Type::Object:
    lua_createtable(L, 0, int(v.object.size()));
    for (const auto &member : v.object) {
      PushJson(L, member.value);
      lua_setfield(L, -2, member.name.c_str());
    }
    break;
  default: lua_pushnil(L); break;
  }
}

// The safe base functions, copied from the real globals into an environment.
void FillSandbox(lua_State *L, int env, const CustomPart &part) {
  static const char *const kBase[] = {"assert", "error",  "ipairs", "next",     "pairs",  "pcall",
                                      "rawequal", "rawget", "rawlen", "rawset", "select", "setmetatable",
                                      "getmetatable", "tonumber", "tostring", "type", "xpcall"};
  lua_pushglobaltable(L);
  for (const char *name : kBase) {
    lua_getfield(L, -1, name);
    lua_setfield(L, env, name);
  }
  for (const char *lib : {"math", "string", "table", "utf8"}) {
    lua_getfield(L, -1, lib);
    lua_setfield(L, env, lib);
  }
  lua_pop(L, 1);
  lua_pushcfunction(L, Log);
  lua_setfield(L, env, "log");
  lua_pushcfunction(L, Log);
  lua_setfield(L, env, "print");
  lua_pushcfunction(L, VecNew);
  lua_setfield(L, env, "vec");
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, FxPlay);
  lua_setfield(L, -2, "play");
  lua_setfield(L, env, "fx");
  PushJson(L, part.params.type == JsonValue::Type::Object ? part.params : JsonValue{JsonValue::Type::Object});
  lua_setfield(L, env, "params");
  lua_pushstring(L, part.id.c_str());
  lua_setfield(L, env, "part_id");
}

std::string Name(const CustomPart &part) {
  return part.mod + "/" + part.script;
}

// The part's script, loaded and run on first use; nullptr when it can't run.
Script *ScriptOf(const CustomPart &part) {
  auto [it, made] = g_scripts.try_emplace(&part);
  Script &script = it->second;
  if (!made)
    return script.stopped ? nullptr : &script;
  lua_State *L = Lua();
  if (!L) {
    script.stopped = true;
    return nullptr;
  }
  const std::string name = "@" + Name(part);
  if (luaL_loadbufferx(L, part.script_source.data(), part.script_source.size(), name.c_str(), "t") != LUA_OK) {
    RNUT_ERROR("Part script {}: {}", Name(part), lua_tostring(L, -1));
    lua_pop(L, 1);
    script.stopped = true;
    return nullptr;
  }
  lua_newtable(L);  // its environment
  FillSandbox(L, lua_gettop(L), part);
  lua_pushvalue(L, -1);
  script.env = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_setupvalue(L, -2, 1);  // the chunk's _ENV
  lua_sethook(L, Budget, LUA_MASKCOUNT, kInstructionBudget);
  const int status = lua_pcall(L, 0, 0, 0);
  lua_sethook(L, nullptr, 0, 0);
  if (status != LUA_OK) {
    RNUT_ERROR("Part script {}: {}", Name(part), lua_tostring(L, -1));
    lua_pop(L, 1);
    script.stopped = true;
    return nullptr;
  }
  RNUT_INFO("Part script {} loaded", Name(part));
  return &script;
}

// The part instance's table (made on first use).
void PushPart(lua_State *L, const CustomPart &part, uint32_t instance) {
  auto it = g_parts.find(instance);
  if (it != g_parts.end()) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
    return;
  }
  lua_newtable(L);
  luaL_setmetatable(L, kPartMeta);
  lua_pushinteger(L, instance);
  lua_setfield(L, -2, "_instance");
  PushJson(L, part.params.type == JsonValue::Type::Object ? part.params : JsonValue{JsonValue::Type::Object});
  lua_setfield(L, -2, "params");
  lua_pushvalue(L, -1);
  g_parts[instance] = luaL_ref(L, LUA_REGISTRYINDEX);
}

// Calls the script's event 'name' (if it has one) with the part (its vehicle
// noted when given) and 'nargs' more arguments pushed by 'push_args'.
template <typename PushArgs>
void Call(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle,
          const char *name, int nargs, PushArgs push_args) {
  g_base = base;
  g_ctx = &ctx;
  g_vehicle = vehicle;
  struct ClearContext {
    ~ClearContext() {
      g_ctx = nullptr;
      g_vehicle = 0;
    }
  } clear_context;
  Script *script = ScriptOf(part);
  if (!script)
    return;
  lua_State *L = g_lua;
  PushPart(L, part, instance);
  if (vehicle) {
    lua_pushinteger(L, vehicle);
    lua_setfield(L, -2, "_vehicle");
  }
  lua_pop(L, 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, script->env);
  lua_getfield(L, -1, name);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    return;
  }
  PushPart(L, part, instance);
  push_args(L);
  lua_sethook(L, Budget, LUA_MASKCOUNT, kInstructionBudget);
  const int status = lua_pcall(L, 1 + nargs, 0, 0);
  lua_sethook(L, nullptr, 0, 0);
  if (status != LUA_OK) {
    RNUT_WARN("Part script {}: {}: {}", Name(part), name, lua_tostring(L, -1));
    lua_pop(L, 1);
    if (++script->errors >= kMaxErrors) {
      script->stopped = true;
      RNUT_ERROR("Part script {} failed {} times; it is stopped", Name(part), kMaxErrors);
    }
  }
  lua_pop(L, 1);  // the environment
}

auto NoArgs = [](lua_State *) {};

} // namespace

void Spawn(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance) {
  Call(ctx, base, part, instance, 0, "on_spawn", 0, NoArgs);
}

void Destroy(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance) {
  Call(ctx, base, part, instance, 0, "on_destroy", 0, NoArgs);
  auto it = g_parts.find(instance);
  if (it != g_parts.end()) {
    if (g_lua)
      luaL_unref(g_lua, LUA_REGISTRYINDEX, it->second);
    g_parts.erase(it);
  }
}

void Attach(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle) {
  Call(ctx, base, part, instance, vehicle, "on_attach", 0, NoArgs);
}

void Tick(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle,
          double dt) {
  Call(ctx, base, part, instance, vehicle, "on_tick", 1, [dt](lua_State *L) { lua_pushnumber(L, dt); });
}

void Button(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle,
            bool down) {
  Call(ctx, base, part, instance, vehicle, down ? "on_button_down" : "on_button_up", 0, NoArgs);
}

} // namespace renut::game::part_scripts
