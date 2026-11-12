#include "lua_vm.h"

#include <fstream>
#include <sstream>

#include <rexfabric/midasm.h>

#include "../events.h"
#include "../hooks.h"
#include "../log.h"
#include "../mod.h"
#include "lua_scripts.h"

namespace rexfabric::lua {
namespace {

RexFabricMod lua_owner{"lua"};
std::vector<ScriptMod> loaded;

int Traceback(lua_State* L) {
  if (const char* message = lua_tostring(L, 1)) {
    luaL_traceback(L, L, message, 1);
  } else if (!lua_isnoneornil(L, 1) && !luaL_callmeta(L, 1, "__tostring")) {
    lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
  }
  return 1;
}

// `require` finds files inside the mod, and never native modules.
void SetPackagePath(lua_State* L, const std::filesystem::path& dir) {
  lua_getglobal(L, "package");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  const std::string root = dir.generic_string();
  const std::string path = root + "/?.lua;" + root + "/?/init.lua";
  lua_pushlstring(L, path.data(), path.size());
  lua_setfield(L, -2, "path");
  lua_pushliteral(L, "");
  lua_setfield(L, -2, "cpath");
  lua_pop(L, 1);
}

int Print(lua_State* L) {
  std::string text;
  for (int i = 1; i <= lua_gettop(L); ++i) {
    if (i > 1) text += ' ';
    text += luaL_tolstring(L, i, nullptr);
    lua_pop(L, 1);
  }
  RF_INFO("[{}] {}", CallerMod(L), text);
  return 0;
}

lua_State* Create() {
  lua_State* L = luaL_newstate();
  if (!L) return nullptr;
  luaL_openlibs(L);
  lua_newtable(L);
  OpenRex(L);
  OpenHost(L);
  lua_setglobal(L, "rex");
  OpenTimers(L);
  OpenGame(L);
  lua_pushcfunction(L, Print);
  lua_setglobal(L, "print");
  return L;
}

bool RunFile(VM& vm, const std::filesystem::path& path, const std::string& mod) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    RF_ERROR("[{}] can't read {}", mod, path.string());
    return false;
  }
  std::ostringstream source;
  source << file.rdbuf();
  const std::string text = source.str();
  const std::string chunk = "@" + path.generic_string();
  if (luaL_loadbuffer(vm.L, text.data(), text.size(), chunk.c_str()) != LUA_OK) {
    RF_ERROR("[{}] {}", mod, lua_tostring(vm.L, -1));
    lua_pop(vm.L, 1);
    return false;
  }
  vm.loading_mod = mod;
  SetPackagePath(vm.L, path.parent_path());
  const bool ok = CallProtected(vm.L, 0, 0, path.filename().string().c_str());
  vm.loading_mod.clear();
  return ok;
}

void Teardown(VM& vm) {
  if (!vm.L) return;
  Hooks::Get().RemoveAll(vm.owner);
  ReleaseAllMidasm(vm.owner);
  events::UnlistenAll(vm.owner);
  ResetRex();
  ResetHost();
  lua_close(vm.L);
  vm.L = nullptr;
  vm.ctx_ref = LUA_NOREF;
  vm.ctx = nullptr;
  vm.base = nullptr;
  loaded.clear();
}

}  // namespace

VM& Vm() {
  static VM vm;
  return vm;
}

bool CallProtected(lua_State* L, int nargs, int nresults, const char* what) {
  const int handler = lua_gettop(L) - nargs;
  lua_pushcfunction(L, Traceback);
  lua_insert(L, handler);
  const int status = lua_pcall(L, nargs, nresults, handler);
  lua_remove(L, handler);
  if (status == LUA_OK) return true;
  RF_ERROR("[{}] {}: {}", CallerMod(L), what, lua_tostring(L, -1) ? lua_tostring(L, -1) : "error");
  lua_pop(L, 1);
  return false;
}

void RegistryTable(lua_State* L, const char* key) {
  lua_getfield(L, LUA_REGISTRYINDEX, key);
  if (lua_istable(L, -1)) return;
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, key);
}

std::string CallerMod(lua_State* L) {
  const VM& vm = Vm();
  if (!vm.loading_mod.empty()) return vm.loading_mod;
  // The innermost Lua function on the stack that lives in a mod's scripts folder.
  lua_Debug ar;
  for (int level = 0; lua_getstack(L, level, &ar); ++level) {
    lua_getinfo(L, "S", &ar);
    const std::string_view source = ar.source ? ar.source : "";
    const size_t scripts = source.rfind("/scripts/");
    if (source.empty() || source.front() != '@' || scripts == std::string_view::npos) continue;
    const std::string_view mod_dir = source.substr(0, scripts);
    return std::string(mod_dir.substr(mod_dir.rfind('/') + 1));
  }
  return "lua";
}

void Start(const std::vector<ModInfo>& mods) {
  VM& vm = Vm();
  std::lock_guard lock(vm.mutex);
  if (vm.L) return;
  std::vector<std::pair<const ModInfo*, std::vector<std::filesystem::path>>> scripted;
  for (const ModInfo& mod : mods) {
    std::vector<std::filesystem::path> files = LuaScripts(mod.dir);
    if (!files.empty()) scripted.emplace_back(&mod, std::move(files));
  }
  if (scripted.empty()) return;

  vm.owner = &lua_owner;
  vm.L = Create();
  if (!vm.L) {
    RF_ERROR("can't create the Lua VM");
    return;
  }
  // First on the frame event, so cvar watchers run before the mods' own frame listeners.
  events::Listen("frame", vm.owner, [](std::span<const Value>) {
    VM& vm = Vm();
    std::lock_guard lock(vm.mutex);
    if (vm.L) DrainCvarChanges(vm.L);
  });

  for (const auto& [mod, files] : scripted) {
    ScriptMod& record = loaded.emplace_back(ScriptMod{mod->folder});
    for (const std::filesystem::path& file : files) {
      if (RunFile(vm, file, mod->folder)) record.files.push_back(file.filename().string());
      else if (record.error.empty()) record.error = file.filename().string() + " failed";
    }
    RF_INFO("[{}] {} of {} scripts ran", mod->folder, record.files.size(), files.size());
  }
  RF_INFO("Lua {}.{}: {} script mods", LUA_VERSION_MAJOR, LUA_VERSION_MINOR, loaded.size());
}

void Reload(const std::vector<ModInfo>& mods) {
  VM& vm = Vm();
  std::lock_guard lock(vm.mutex);
  Teardown(vm);
  Start(mods);
  RF_INFO("scripts reloaded");
}

void Stop() {
  VM& vm = Vm();
  std::lock_guard lock(vm.mutex);
  Teardown(vm);
}

bool Running() {
  VM& vm = Vm();
  std::lock_guard lock(vm.mutex);
  return vm.L != nullptr;
}

const std::vector<ScriptMod>& Loaded() { return loaded; }

}  // namespace rexfabric::lua
