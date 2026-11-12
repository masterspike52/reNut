// The Lua VM shared by the Lua sources.
//
// One VM runs every Lua mod. Hooks fire on several guest threads, so every way into the VM takes `mutex`
// first. A hook's Lua runs under the lock; the guest function it wraps runs outside it, so a lock is never
// held across guest code the mod didn't ask to call.

#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

// Lua is compiled as C++, so its errors unwind C++ frames as exceptions; its headers are included without
// lua.hpp's extern "C".
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <rex/ppc/context.h>
#include <rexfabric/api.h>
#include <rexfabric/game_api.h>

namespace rexfabric::lua {

struct VM {
  lua_State* L = nullptr;
  std::recursive_mutex mutex;

  // The owner of every hook, claim and listener Lua makes, so a reload drops them all at once.
  RexFabricMod* owner = nullptr;
  // The mod whose file is loading; outside a load, the mod is found from the calling function's source.
  std::string loading_mod;

  // The context userdata is made once and pointed at each hooked call in turn. `ctx` is null outside a
  // hook, so a mod that keeps the object gets an error instead of a dead guest stack frame.
  int ctx_ref = LUA_NOREF;
  PPCContext* ctx = nullptr;
  uint8_t* base = nullptr;
  uint32_t ctx_address = 0;
};

VM& Vm();

// The parts of the API, each installed into the `rex` table on top of the stack.
void OpenRex(lua_State* L);
void OpenHost(lua_State* L);
// The game's namespaces, as globals.
void OpenGame(lua_State* L);
// rex.after, rex.every and rex.cancel (lua_timers.cpp), once the `rex` global is set.
void OpenTimers(lua_State* L);

// Drops what the parts keep beside the VM: hook sites, claims, listeners, caches. The VM is still open.
void ResetRex();
void ResetHost();

// A game value as Lua sees it: objects become proxies, none becomes nil.
void PushValue(lua_State* L, const Value& value);

// Runs the cvar watchers whose cvar changed since the last call. Frame thread, under the lock.
void DrainCvarChanges(lua_State* L);

// lua_pcall with a traceback. Logs and pops the error; true on success.
bool CallProtected(lua_State* L, int nargs, int nresults, const char* what);
// The table at registry[key], made if it isn't there, pushed.
void RegistryTable(lua_State* L, const char* key);
// The mod a Lua function belongs to: the one loading, or the one whose file the caller lives in.
std::string CallerMod(lua_State* L);

}  // namespace rexfabric::lua
