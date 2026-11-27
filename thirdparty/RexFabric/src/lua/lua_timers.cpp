// rex.after, rex.every and rex.cancel: functions run later, on the frame thread, as the frames go by. And waiting:
// rex.run runs a function as a coroutine, which rex.wait and rex.wait_for pause until time has passed or an event
// fires, as Unreal's latent nodes do.
//
//   local id = rex.every(0.5, function() rex.info("tick") end)
//   rex.after(3, function() rex.cancel(id) end)
//   rex.run(function()
//     rex.wait(2)
//     local garden = rex.wait_for("tip.garden_loaded")
//   end)
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

-- Waiting -------------------------------------------------------------------------------------------------

local function resume(co, ...)
  local ok, problem = coroutine.resume(co, ...)
  if not ok then rex.error("a waiting function failed: " .. debug.traceback(co, tostring(problem))) end
end

-- The coroutine a wait pauses; an error outside rex.run, where there's nothing to pause.
local function current(what)
  local co, main = coroutine.running()
  if co == nil or main then error(what .. " waits only inside a function rex.run runs", 3) end
  return co
end

-- Runs fn(...) now as a coroutine, so it may wait; it goes on by itself as its waits end.
function rex.run(fn, ...)
  if type(fn) ~= "function" then error("rex.run runs a function", 2) end
  resume(coroutine.create(fn), ...)
end

-- Waits `seconds`, then goes on.
function rex.wait(seconds)
  local co = current("rex.wait")
  if type(seconds) ~= "number" then error("rex.wait waits a number of seconds", 2) end
  add(seconds, function() resume(co) end, false)
  return coroutine.yield()
end

-- Each event's waiting coroutines, with what they wait for; one listener per event wakes them.
local waiting = {}

-- Waits until the event fires (with arguments `filter` says yes to, when it's given), then gives its arguments; or
-- gives nothing after `timeout` seconds, when it's given.
function rex.wait_for(event, filter, timeout)
  local co = current("rex.wait_for")
  if type(event) ~= "string" then error("rex.wait_for waits for an event, by its name", 2) end
  if filter ~= nil and type(filter) ~= "function" then error("rex.wait_for's filter is a function", 2) end
  local list = waiting[event]
  if not list then
    list = {}
    waiting[event] = list
    rex.on(event, function(...)
      local woken = {}
      for i = #list, 1, -1 do
        local entry = list[i]
        local ok, wanted = true, true
        if entry.filter then ok, wanted = pcall(entry.filter, ...) end
        if not ok then rex.error("a wait_for filter failed: " .. tostring(wanted)) end
        if not ok or wanted then
          table.remove(list, i)
          woken[#woken + 1] = entry
        end
      end
      -- The first to wait goes on first.
      for i = #woken, 1, -1 do
        if woken[i].timer then rex.cancel(woken[i].timer) end
        resume(woken[i].co, ...)
      end
    end)
  end
  local entry = {co = co, filter = filter}
  if timeout ~= nil then
    entry.timer = add(timeout, function()
      for i, held in ipairs(list) do
        if held == entry then
          table.remove(list, i)
          break
        end
      end
      resume(co)
    end, false)
  end
  list[#list + 1] = entry
  return coroutine.yield()
end

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
