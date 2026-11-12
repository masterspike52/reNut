// rex.after, rex.every and rex.cancel: functions run later, on the frame thread, as the frames go by.
//
//   local id = rex.every(0.5, function() rex.info("tick") end)
//   rex.after(3, function() rex.cancel(id) end)
//
// Written in Lua on rex.on("frame") and rex.now(), installed before any mod runs, so its frame listener comes first.
// Timers live in the VM, so reloading the scripts drops them with everything else. A timer runs at most once a frame,
// on the first frame its time has come; one that keeps repeating picks up from then rather than running to catch up.

#include "lua_vm.h"

namespace rexfabric::lua {
namespace {

constexpr const char* kTimers = R"LUA(
local rex = ...
local timers = {}
local last = 0

local function add(seconds, fn, repeating)
  if type(seconds) ~= "number" then error("a timer's time is a number of seconds", 3) end
  if type(fn) ~= "function" then error("a timer runs a function", 3) end
  last = last + 1
  timers[last] = {at = rex.now() + seconds, fn = fn, every = repeating and math.max(seconds, 0) or nil}
  return last
end

-- Runs fn once, `seconds` from now. Returns an id for rex.cancel.
function rex.after(seconds, fn) return add(seconds, fn, false) end
-- Runs fn every `seconds` until it's cancelled. Returns an id for rex.cancel.
function rex.every(seconds, fn) return add(seconds, fn, true) end
-- Stops a timer. A timer that already ran, or an id that isn't one, is fine.
function rex.cancel(id) if id ~= nil then timers[id] = nil end end

rex.on("frame", function()
  local now = rex.now()
  local due = {}
  for id, timer in pairs(timers) do
    if timer.at <= now then due[#due + 1] = id end
  end
  -- Oldest first, so timers set together run in the order they were set.
  table.sort(due)
  for _, id in ipairs(due) do
    local timer = timers[id]
    if timer then
      if timer.every then timer.at = now + timer.every else timers[id] = nil end
      local ok, problem = xpcall(timer.fn, debug.traceback)
      if not ok then rex.error("a timer failed: " .. tostring(problem)) end
    end
  end
end)
)LUA";

}  // namespace

void OpenTimers(lua_State* L) {
  if (luaL_loadbuffer(L, kTimers, std::char_traits<char>::length(kTimers), "=rex.timers") != LUA_OK) {
    lua_pop(L, 1);
    return;
  }
  lua_getglobal(L, "rex");
  CallProtected(L, 1, 0, "rex.timers");
}

}  // namespace rexfabric::lua
