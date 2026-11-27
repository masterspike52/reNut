// rex.world_data() (docs/lua_mods.md, World data): a table of the calling mod's own, kept in the save of the world the
// game is in, as its avatars saved with the world are (content.h, WorldSaveData).
//
//   local data = rex.world_data()
//   data.visits = (data.visits or 0) + 1
//
// A mod's table is the same table for as long as the VM runs: a world loaded fills it again in place, so a script may
// keep it. What's in it is kept here, in C++, while the mod has no table yet (it hasn't asked, isn't loaded, or the
// scripts are reloading), so nothing is lost before it asks.

#include <map>
#include <mutex>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "../log.h"
#include "lua_scripts.h"
#include "lua_vm.h"

namespace rexfabric::lua {
namespace {

// The registry table of every mod's table, by mod folder.
constexpr const char* kTables = "rexfabric.world_data";
constexpr int kMostDepth = 32;

// The data of the mods without a table, by mod folder.
struct Held {
  std::mutex mutex;
  std::map<std::string, nlohmann::json> data;
};

Held& GetHeld() {
  static auto* held = new Held();
  return *held;
}

// What a save leaves out of a mod's table, said once per save.
struct Dropped {
  std::set<std::string> kinds;
};

// The table at `index` as a JSON list when its keys are 1 to n, else an object of its string keys.
nlohmann::json TableToJson(lua_State* L, int index, int depth, Dropped& dropped);

nlohmann::json ToJson(lua_State* L, int index, int depth, Dropped& dropped) {
  switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
    case LUA_TNUMBER: return lua_isinteger(L, index) ? nlohmann::json(lua_tointeger(L, index)) : nlohmann::json(lua_tonumber(L, index));
    case LUA_TSTRING: return lua_tostring(L, index);
    case LUA_TTABLE:
      if (depth < kMostDepth) return TableToJson(L, index, depth + 1, dropped);
      dropped.kinds.insert("tables nested deeper than " + std::to_string(kMostDepth) + " levels");
      return nullptr;
    default:
      dropped.kinds.insert(std::string("values of type ") + luaL_typename(L, index));
      return nullptr;
  }
}

nlohmann::json TableToJson(lua_State* L, int index, int depth, Dropped& dropped) {
  index = lua_absindex(L, index);
  const lua_Unsigned length = lua_rawlen(L, index);
  size_t keys = 0;
  bool listed = true;
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    ++keys;
    if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || static_cast<lua_Unsigned>(lua_tointeger(L, -2)) > length) listed = false;
    lua_pop(L, 1);
  }
  if (listed && keys == length && length > 0) {
    nlohmann::json list = nlohmann::json::array();
    for (lua_Unsigned i = 1; i <= length; ++i) {
      lua_rawgeti(L, index, static_cast<lua_Integer>(i));
      list.push_back(ToJson(L, -1, depth, dropped));
      lua_pop(L, 1);
    }
    return list;
  }
  nlohmann::json object = nlohmann::json::object();
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      nlohmann::json value = ToJson(L, -1, depth, dropped);
      if (!value.is_null()) object[lua_tostring(L, -2)] = std::move(value);
    } else {
      dropped.kinds.insert("keys other than text");
    }
    lua_pop(L, 1);
  }
  return object;
}

void PushJson(lua_State* L, const nlohmann::json& value, int depth) {
  luaL_checkstack(L, 4, "world data nested too deep");
  if (value.is_boolean()) lua_pushboolean(L, value.get<bool>());
  else if (value.is_number_integer()) lua_pushinteger(L, value.get<lua_Integer>());
  else if (value.is_number()) lua_pushnumber(L, value.get<double>());
  else if (value.is_string()) lua_pushstring(L, value.get<std::string>().c_str());
  else if ((value.is_array() || value.is_object()) && depth < kMostDepth) {
    lua_createtable(L, value.is_array() ? static_cast<int>(value.size()) : 0, value.is_object() ? static_cast<int>(value.size()) : 0);
    lua_Integer at = 0;
    for (const auto& [key, item] : value.items()) {
      PushJson(L, item, depth + 1);
      if (value.is_array()) lua_rawseti(L, -2, ++at);
      else lua_setfield(L, -2, key.c_str());
    }
  } else {
    lua_pushnil(L);
  }
}

// Empties the table at `index` and fills it with `data`'s fields: the same table, what it holds the world's.
void Fill(lua_State* L, int index, const nlohmann::json& data) {
  index = lua_absindex(L, index);
  for (;;) {
    lua_pushnil(L);
    if (lua_next(L, index) == 0) break;
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_rawset(L, index);
  }
  if (!data.is_object()) return;
  for (const auto& [key, item] : data.items()) {
    PushJson(L, item, 0);
    lua_setfield(L, index, key.c_str());
  }
}

nlohmann::json Take(const std::string& mod) {
  Held& held = GetHeld();
  std::lock_guard lock(held.mutex);
  const auto found = held.data.find(mod);
  if (found == held.data.end()) return nlohmann::json::object();
  nlohmann::json data = std::move(found->second);
  held.data.erase(found);
  return data;
}

int WorldData(lua_State* L) {
  const std::string mod = CallerMod(L);
  RegistryTable(L, kTables);
  lua_getfield(L, -1, mod.c_str());
  if (lua_istable(L, -1)) {
    lua_remove(L, -2);
    return 1;
  }
  lua_pop(L, 1);
  lua_newtable(L);
  Fill(L, -1, Take(mod));
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, mod.c_str());
  lua_remove(L, -2);
  return 1;
}

// Each mod's table as JSON, by mod folder, an empty one left out.
std::map<std::string, nlohmann::json> TablesToJson(lua_State* L) {
  std::map<std::string, nlohmann::json> tables;
  RegistryTable(L, kTables);
  lua_pushnil(L);
  while (lua_next(L, -2) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
      const std::string mod = lua_tostring(L, -2);
      Dropped dropped;
      nlohmann::json data = TableToJson(L, -1, 0, dropped);
      for (const std::string& kind : dropped.kinds) RF_WARN("[{}] world data: {} are left out of the save", mod, kind);
      if (!data.empty()) tables[mod] = std::move(data);
      else tables[mod] = nullptr;  // emptied: its held data, if any, goes too
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return tables;
}

}  // namespace

nlohmann::json SaveWorldData() {
  nlohmann::json mods = nlohmann::json::object();
  {
    Held& held = GetHeld();
    std::lock_guard lock(held.mutex);
    for (const auto& [mod, data] : held.data) mods[mod] = data;
  }
  VM& vm = Vm();
  std::lock_guard lock(vm.mutex);
  if (!vm.L) return mods;
  for (auto& [mod, data] : TablesToJson(vm.L)) {
    if (data.is_null()) mods.erase(mod);
    else mods[mod] = std::move(data);
  }
  return mods;
}

void RestoreWorldData(const nlohmann::json& mods) {
  {
    Held& held = GetHeld();
    std::lock_guard lock(held.mutex);
    held.data.clear();
    if (mods.is_object()) {
      for (const auto& [mod, data] : mods.items()) held.data[mod] = data;
    }
  }
  VM& vm = Vm();
  std::lock_guard lock(vm.mutex);
  if (!vm.L) return;
  lua_State* L = vm.L;
  // The tables the mods have are filled again in place.
  RegistryTable(L, kTables);
  lua_pushnil(L);
  while (lua_next(L, -2) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) Fill(L, -1, Take(lua_tostring(L, -2)));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

void KeepWorldData(lua_State* L) {
  std::map<std::string, nlohmann::json> tables = TablesToJson(L);
  Held& held = GetHeld();
  std::lock_guard lock(held.mutex);
  for (auto& [mod, data] : tables) {
    if (data.is_null()) held.data.erase(mod);
    else held.data[mod] = std::move(data);
  }
}

void OpenWorldData(lua_State* L) {
  lua_pushcfunction(L, WorldData);
  lua_setfield(L, -2, "world_data");
}

}  // namespace rexfabric::lua
