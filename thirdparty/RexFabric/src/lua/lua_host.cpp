// rex.input, rex.switch and rex.midasm: the game's keyboard and mouse, the switches its compiled-in patches
// read, and the midasm sites it compiled in.

#include <set>
#include <string>
#include <unordered_map>

#include <rexfabric/host_input.h>
#include <rexfabric/midasm.h>
#include <rexfabric/switches.h>

#include "lua_vm.h"

namespace rexfabric::lua {
namespace {

// rex.input ----------------------------------------------------------------------------------------------

std::unordered_map<std::string, uint16_t>& KeyCodes() {
  static std::unordered_map<std::string, uint16_t> codes;
  return codes;
}

// Each key's state when rex.input.pressed last looked, which makes pressed an edge.
std::unordered_map<uint16_t, bool>& Edges() {
  static std::unordered_map<uint16_t, bool> edges;
  return edges;
}

// A key name, or a code the mod already knows. 0 for a name this game doesn't know.
uint16_t CheckKey(lua_State* L, int arg) {
  if (lua_isnumber(L, arg)) return static_cast<uint16_t>(lua_tointeger(L, arg));
  const std::string name = luaL_checkstring(L, arg);
  const auto it = KeyCodes().find(name);
  if (it != KeyCodes().end()) return it->second;
  const HostInput& host = GetHostInput();
  const uint16_t code = host.resolve_key ? host.resolve_key(name) : 0;
  KeyCodes().emplace(name, code);
  return code;
}

bool KeyDown(uint16_t code) {
  const HostInput& host = GetHostInput();
  return code && host.key_down && host.key_down(code);
}

int InputAvailable(lua_State* L) {
  lua_pushboolean(L, HasHostInput());
  return 1;
}

int InputDown(lua_State* L) {
  lua_pushboolean(L, KeyDown(CheckKey(L, 1)));
  return 1;
}

// True once when a key goes down: the first call after the press, then false until it's released. One
// caller per key, since the edge is used up.
int InputPressed(lua_State* L) {
  const uint16_t code = CheckKey(L, 1);
  const bool down = KeyDown(code);
  bool& was_down = Edges()[code];
  lua_pushboolean(L, down && !was_down);
  was_down = down;
  return 1;
}

int InputMouseButton(lua_State* L) {
  const HostInput& host = GetHostInput();
  lua_pushboolean(L, host.mouse_button_down && host.mouse_button_down(static_cast<int>(luaL_checkinteger(L, 1))));
  return 1;
}

int InputMouseDelta(lua_State* L) {
  const HostInput& host = GetHostInput();
  const auto [dx, dy] = host.consume_mouse_delta ? host.consume_mouse_delta() : std::pair<int32_t, int32_t>{};
  lua_pushinteger(L, dx);
  lua_pushinteger(L, dy);
  return 2;
}

int InputWheel(lua_State* L) {
  const HostInput& host = GetHostInput();
  lua_pushinteger(L, host.consume_wheel ? host.consume_wheel() : 0);
  return 1;
}

int InputAccepts(lua_State* L) {
  const HostInput& host = GetHostInput();
  lua_pushboolean(L, host.accepts_input && host.accepts_input());
  return 1;
}

int InputHoldGuest(lua_State* L) {
  luaL_checkany(L, 1);
  const HostInput& host = GetHostInput();
  if (host.set_guest_input_held) host.set_guest_input_held(lua_toboolean(L, 1) != 0);
  return 0;
}

// Drops piled up mouse motion. Key edges stay: a mod flushing on its own hotkey would otherwise see that
// key, still held, as a fresh press next frame.
int InputFlush(lua_State*) {
  const HostInput& host = GetHostInput();
  if (host.flush) host.flush();
  return 0;
}

constexpr luaL_Reg kInputFuncs[] = {
    {"available", InputAvailable}, {"down", InputDown}, {"pressed", InputPressed}, {"mouse_button", InputMouseButton}, {"mouse_delta", InputMouseDelta},
    {"wheel", InputWheel}, {"accepts", InputAccepts}, {"hold_guest", InputHoldGuest}, {"flush", InputFlush},
    {nullptr, nullptr},
};

// rex.switch ---------------------------------------------------------------------------------------------

// The switches Lua set, put back to 0 when the scripts stop.
std::set<std::string>& SetByLua() {
  static std::set<std::string> names;
  return names;
}

int SwitchGet(lua_State* L) {
  lua_pushinteger(L, GetSwitch(luaL_checkstring(L, 1)));
  return 1;
}

int SwitchSet(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  luaL_checkany(L, 2);
  SetSwitch(name, lua_isboolean(L, 2) ? lua_toboolean(L, 2) : static_cast<int32_t>(luaL_checkinteger(L, 2)));
  SetByLua().insert(name);
  return 0;
}

int SwitchList(lua_State* L) {
  const auto switches = Switches();
  lua_createtable(L, 0, static_cast<int>(switches.size()));
  for (const auto& [name, value] : switches) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name.c_str());
  }
  return 1;
}

constexpr luaL_Reg kSwitchFuncs[] = {{"get", SwitchGet}, {"set", SwitchSet}, {"list", SwitchList}, {nullptr, nullptr}};

// rex.midasm ---------------------------------------------------------------------------------------------
// A claimed site calls into Lua with the registers its stub lent. The frame userdata is made once and
// pointed at each call in turn; only one call is live at a time, under the VM lock.

constexpr const char* kFrameMeta = "rex.midasm.frame";
constexpr const char* kMidasmTable = "rexfabric.midasms";  // key -> function

struct Frame {
  uint32_t address = 0;
  const MidasmReg* regs = nullptr;
  size_t count = 0;
  bool live = false;
};

struct MidasmState {
  Frame frame;
  int frame_ref = LUA_NOREF;
  std::unordered_map<uint64_t, std::pair<uint64_t, std::string>> claims;  // claim id -> key, owner
  uint64_t next_key = 1;
};

MidasmState& Midasms() {
  static MidasmState state;
  return state;
}

const Frame& CheckFrame(lua_State* L) {
  luaL_checkudata(L, 1, kFrameMeta);
  const Frame& frame = Midasms().frame;
  if (!frame.live) luaL_error(L, "midasm frame used outside the handler that produced it (frames are only valid for the duration of the callback)");
  return frame;
}

PPCRegister* CheckRegister(lua_State* L, const Frame& frame, std::string_view name) {
  for (size_t i = 0; i < frame.count; ++i) {
    if (name == frame.regs[i].name) return frame.regs[i].reg;
  }
  // A register the stub didn't lend is a mod bug; nil would only turn up as arithmetic on nil later.
  luaL_error(L, "midasm site 0x%08X did not expose register '%s'", frame.address, std::string(name).c_str());
  return nullptr;
}

bool IsFloat(std::string_view name) { return !name.empty() && name[0] == 'f'; }

int FrameIndex(lua_State* L) {
  const Frame& frame = CheckFrame(L);
  const std::string_view key = luaL_checkstring(L, 2);
  if (key == "address") {
    lua_pushinteger(L, frame.address);
    return 1;
  }
  const PPCRegister* reg = CheckRegister(L, frame, key);
  if (IsFloat(key)) lua_pushnumber(L, reg->f64);
  else lua_pushinteger(L, reg->u32);
  return 1;
}

int FrameNewIndex(lua_State* L) {
  const Frame& frame = CheckFrame(L);
  const std::string_view key = luaL_checkstring(L, 2);
  PPCRegister* reg = CheckRegister(L, frame, key);
  if (IsFloat(key)) reg->f64 = luaL_checknumber(L, 3);
  else reg->u64 = static_cast<uint32_t>(luaL_checkinteger(L, 3));
  return 0;
}

bool Trampoline(void* user, uint32_t address, const MidasmReg* regs, size_t count) {
  VM& vm = Vm();
  std::lock_guard lock(vm.mutex);
  if (!vm.L) return false;
  lua_State* L = vm.L;
  RegistryTable(L, kMidasmTable);
  lua_rawgeti(L, -1, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(user)));
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    return false;
  }
  MidasmState& state = Midasms();
  const Frame previous = state.frame;
  state.frame = {address, regs, count, true};
  lua_rawgeti(L, LUA_REGISTRYINDEX, state.frame_ref);
  // Only a truthy return asks for the patch, and a handler that raised asks for nothing, so a broken mod
  // can't send the guest down a branch it never meant to take.
  bool patch = false;
  if (CallProtected(L, 1, 1, "midasm handler")) {
    patch = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  state.frame = previous;
  return patch;
}

// rex.midasm(address, fn): the address is the instruction the game's midasm config names. A site with no
// stub behind it never calls, which looks the same as one the guest hasn't reached yet.
int MidasmAttach(lua_State* L) {
  const lua_Integer address = luaL_checkinteger(L, 1);
  if (address < 0 || address > 0xFFFFFFFFll) return luaL_error(L, "midasm address %I out of range", address);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  MidasmState& state = Midasms();
  const uint64_t key = state.next_key++;
  RegistryTable(L, kMidasmTable);
  lua_pushvalue(L, 2);
  lua_rawseti(L, -2, static_cast<lua_Integer>(key));
  lua_pop(L, 1);
  const uint64_t id = ClaimMidasm(Vm().owner, static_cast<uint32_t>(address), Trampoline, reinterpret_cast<void*>(static_cast<uintptr_t>(key)));
  state.claims[id] = {key, CallerMod(L)};
  lua_pushinteger(L, static_cast<lua_Integer>(id));
  return 1;
}

int MidasmDetach(lua_State* L) {
  const auto id = static_cast<uint64_t>(luaL_checkinteger(L, 1));
  MidasmState& state = Midasms();
  const auto it = state.claims.find(id);
  const bool released = it != state.claims.end() && ReleaseMidasm(id);
  if (released) {
    RegistryTable(L, kMidasmTable);
    lua_pushnil(L);
    lua_rawseti(L, -2, static_cast<lua_Integer>(it->second.first));
    lua_pop(L, 1);
    state.claims.erase(it);
  }
  lua_pushboolean(L, released);
  return 1;
}

int MidasmList(lua_State* L) {
  MidasmState& state = Midasms();
  lua_newtable(L);
  lua_Integer n = 0;
  for (const MidasmClaim& claim : MidasmClaims()) {
    const auto it = state.claims.find(claim.id);
    if (it == state.claims.end()) continue;
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, static_cast<lua_Integer>(claim.id));
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, claim.address);
    lua_setfield(L, -2, "address");
    lua_pushstring(L, it->second.second.c_str());
    lua_setfield(L, -2, "owner");
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

}  // namespace

void OpenHost(lua_State* L) {
  luaL_newlib(L, kInputFuncs);
  lua_setfield(L, -2, "input");
  luaL_newlib(L, kSwitchFuncs);
  lua_setfield(L, -2, "switch");
  lua_pushcfunction(L, MidasmAttach);
  lua_setfield(L, -2, "midasm");
  lua_pushcfunction(L, MidasmDetach);
  lua_setfield(L, -2, "unmidasm");
  lua_pushcfunction(L, MidasmList);
  lua_setfield(L, -2, "midasms");

  luaL_newmetatable(L, kFrameMeta);
  lua_pushcfunction(L, FrameIndex);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, FrameNewIndex);
  lua_setfield(L, -2, "__newindex");
  lua_pushstring(L, kFrameMeta);
  lua_setfield(L, -2, "__metatable");
  lua_pop(L, 1);
  lua_newuserdatauv(L, 1, 0);
  luaL_setmetatable(L, kFrameMeta);
  Midasms().frame_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

void ResetHost() {
  KeyCodes().clear();
  Edges().clear();
  Midasms() = {};
  for (const std::string& name : SetByLua()) SetSwitch(name, 0);
  SetByLua().clear();
}

}  // namespace rexfabric::lua
