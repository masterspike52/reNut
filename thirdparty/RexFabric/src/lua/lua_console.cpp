// The Lua console (docs/lua_mods.md, The console): lines typed in its window, run in the mods' VM on the next frame,
// on the frame thread where the game's API works, their results or errors sent back.
//
// A line is tried as an expression first ("return " before it), as Lua's own interpreter does, so `tip.garden.hour`
// shows the hour; one that doesn't read as an expression runs as statements. It runs as the mod "console", which may
// name every mod's content (ContentMod).

#include "lua_scripts.h"

#include <deque>
#include <mutex>

#include "lua_vm.h"

namespace rexfabric::lua {
namespace {

// Replies kept for the window until it takes them.
constexpr size_t kMostReplies = 256;

struct Queued {
  uint64_t id;
  std::string code;
};

struct Console {
  std::mutex mutex;
  std::deque<Queued> queued;
  std::vector<ConsoleReply> replies;
  uint64_t next = 1;
};

Console& Get() {
  static auto* console = new Console();
  return *console;
}

int Traceback(lua_State* L) {
  luaL_traceback(L, L, lua_tostring(L, 1), 1);
  return 1;
}

// Runs one line in the VM; its results joined, or its error.
ConsoleReply Run(VM& vm, const Queued& line) {
  lua_State* L = vm.L;
  const int top = lua_gettop(L);
  lua_pushcfunction(L, Traceback);
  const std::string expression = "return " + line.code;
  if (luaL_loadbuffer(L, expression.data(), expression.size(), "=console") != LUA_OK) {
    lua_pop(L, 1);
    if (luaL_loadbuffer(L, line.code.data(), line.code.size(), "=console") != LUA_OK) {
      ConsoleReply reply{.id = line.id, .text = lua_tostring(L, -1), .error = true};
      lua_settop(L, top);
      return reply;
    }
  }
  const std::string loading = std::exchange(vm.loading_mod, kConsoleMod);
  const int status = lua_pcall(L, 0, LUA_MULTRET, top + 1);
  vm.loading_mod = loading;
  if (status != LUA_OK) {
    ConsoleReply reply{.id = line.id, .text = lua_tostring(L, -1) ? lua_tostring(L, -1) : "an error that isn't text", .error = true};
    lua_settop(L, top);
    return reply;
  }
  std::string text;
  for (int i = top + 2; i <= lua_gettop(L); ++i) {
    if (!text.empty()) text += ", ";
    text += luaL_tolstring(L, i, nullptr);
    lua_pop(L, 1);
  }
  lua_settop(L, top);
  return {.id = line.id, .text = text.empty() ? std::string("(nothing)") : text};
}

}  // namespace

uint64_t QueueConsole(std::string code) {
  Console& console = Get();
  std::lock_guard lock(console.mutex);
  const uint64_t id = console.next++;
  console.queued.push_back({id, std::move(code)});
  return id;
}

std::vector<ConsoleReply> TakeConsoleReplies() {
  Console& console = Get();
  std::lock_guard lock(console.mutex);
  return std::exchange(console.replies, {});
}

void RunConsole() {
  Console& console = Get();
  std::deque<Queued> lines;
  {
    std::lock_guard lock(console.mutex);
    lines.swap(console.queued);
  }
  if (lines.empty()) return;
  std::vector<ConsoleReply> replies;
  {
    VM& vm = Vm();
    std::lock_guard lock(vm.mutex);
    for (const Queued& line : lines) {
      if (!vm.L) replies.push_back({.id = line.id, .text = "There's no Lua VM: no enabled mod has scripts", .error = true});
      else replies.push_back(Run(vm, line));
    }
  }
  std::lock_guard lock(console.mutex);
  for (ConsoleReply& reply : replies) console.replies.push_back(std::move(reply));
  if (console.replies.size() > kMostReplies) console.replies.erase(console.replies.begin(), console.replies.end() - kMostReplies);
}

}  // namespace rexfabric::lua
