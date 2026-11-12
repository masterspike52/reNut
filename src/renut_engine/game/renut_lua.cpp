#include "renut_engine/game/renut_lua.h"

#include <rex/string/text.h>
#include <rexfabric/game_api.h>
#include <rexfabric/rexfabric.h>

#include <filesystem>
#include <string>

namespace renut::game::renut_lua {
namespace {

// Built on the renut functions the other files declare; runs after their Lua.
constexpr const char kLua[] = R"lua(
local vec = renut.vec
local player, actors, objects, vehicles = renut.player, renut.actors, renut.objects, renut.vehicle
local camera, game, hud, world = renut.camera, renut.game, renut.hud, renut.world
renut.debug = renut.debug or {}
renut.challenges = renut.challenges or {}
renut.settings = renut.settings or {}
renut.pad = renut.pad or {}

-- The mod folder of the script `level` calls up ("mods/<mod>/scripts/x.lua"), or "lua".
local function caller_mod(level)
  local source = debug.getinfo(level + 1, "S").source or ""
  return source:match(".*[/\\]([^/\\]+)[/\\]scripts[/\\]") or "lua"
end
local function who_of(actor)
  if actor == nil or type(actor) == "string" then return 0, actor or "" end
  return actor, ""
end
local function xyz(p, what)
  assert(type(p) == "table", what .. " is a position (a vector)")
  return p.x or p[1] or 0, p.y or p[2] or 0, p.z or p[3] or 0
end
-- A handle of these Lua-made listeners: h:off() stops it.
local function handle_of(list, fn)
  local h = {fn = fn}
  function h:off() for i, x in ipairs(list) do if x == self then table.remove(list, i) return end end end
  list[#list + 1] = h
  return h
end

-- Characters ---------------------------------------------------------------------------
function actors.velocity(actor)
  local a, w = who_of(actor)
  return vec(actors._velocity(a, w, 0), actors._velocity(a, w, 1), actors._velocity(a, w, 2))
end
function actors.push(actor, v) local a, w = who_of(actor) actors._push(a, w, xyz(v, "renut.actors.push: the push")) end
function actors.on_ground(actor) local a, w = who_of(actor) return actors._on_ground(a, w) end
function actors.scale(actor) local a, w = who_of(actor) return actors._scale(a, w) end
function actors.exit_vehicle(actor) local a, w = who_of(actor) return actors._exit_vehicle(a, w) end
function actors.yaw(actor) local a, w = who_of(actor) return actors._yaw(a, w) end
function actors.current_body_state(actor) local a, w = who_of(actor) return actors._body_now(a, w) end

-- The players ------------------------------------------------------------------------------
local own_position, own_yaw, own_avatar, own_vehicle = player.position, player.yaw, player.avatar, player.vehicle
local function avatar_of(n)
  if n == nil or n == 1 then return own_avatar() end
  local a = player._avatar_of(n)
  return a ~= 0 and a or nil
end
local function me(n)
  local a = avatar_of(n)
  if not a then error("there's no player " .. tostring(n or 1) .. " in this world", 3) end
  return a
end
function renut.players()
  local list = {}
  for n = 1, 4 do
    local a = avatar_of(n)
    if a then list[#list + 1] = {number = n, avatar = a} end
  end
  return list
end
function player.avatar(n) return avatar_of(n) end
function player.position(n)
  if n == nil or n == 1 then return own_position() end
  local a = avatar_of(n)
  return a and objects.position(a) or nil
end
function player.yaw(n)
  if n == nil or n == 1 then return own_yaw() end
  local a = avatar_of(n)
  return a and actors.yaw(a) or nil
end
function player.vehicle(n)
  if n == nil or n == 1 then return own_vehicle() end
  local a = avatar_of(n)
  return a and actors.vehicle(a) or nil
end
function player.velocity(n) return actors.velocity(me(n)) end
function player.push(v, n) actors.push(me(n), v) end
function player.on_ground(n) return actors.on_ground(me(n)) end
function player.state(n) return actors.current_body_state(me(n)) end
function player.swimming(n)
  local s = player.state(n)
  return (s:find("Swim") or s:find("TreadWater") or s:find("Underwater") or s:find("^Dive")) ~= nil
end
function player.exit_vehicle(n) return actors.exit_vehicle(me(n)) end
local JUMPS = {Jump = true, JumpFromWater = true, JumpFromClimbPole = true, JumpAtVehicle = true,
               JumpAtVehicleAir = true, JumpAtVehicleWater = true}
local LANDINGS = {Land = true, JumpLand = true, JumpLandSoft = true, FallCrashLand = true}
function player.on(event, fn)
  if event == "state" then return rex.on("renut.player_state", fn) end
  if event == "jump" then
    return rex.on("renut.player_state", function(new) if JUMPS[new] then fn(new) end end)
  end
  if event == "land" then
    return rex.on("renut.player_state", function(new) if LANDINGS[new] then fn(new) end end)
  end
  error("renut.player.on: the events are \"state\", \"jump\" and \"land\"", 2)
end

-- Objects ----------------------------------------------------------------------------------
function objects.move(object, at) objects._move(object, xyz(at, "renut.objects.move: where it goes")) end
function objects.turn(object, yaw, pitch, roll) objects._turn(object, yaw or 0, pitch or 0, roll or 0) end
function objects.yaw(object) return objects._yaw(object) end

-- Vehicles ----------------------------------------------------------------------------------
local Vehicle = getmetatable(vehicles.at(1))
function vehicles.list()
  local list = {}
  for address in vehicles._list():gmatch("%d+") do list[#list + 1] = vehicles.at(math.tointeger(tonumber(address))) end
  return list
end
-- What renut.vehicle.spawn takes: the enabled mods' blueprints ("mod: name") and the save's vehicles ("save: name").
function vehicles.blueprints()
  local list = {}
  for name, file in vehicles._blueprints():gmatch("([^\t\n]+)\t([^\n]+)\n") do list[#list + 1] = {name = name, file = file} end
  return list
end
function Vehicle:remove() vehicles._remove(self.address) end
function Vehicle:rotation()
  local a = self.address
  return {x = vehicles._rotation(a, 0), y = vehicles._rotation(a, 1), z = vehicles._rotation(a, 2), w = vehicles._rotation(a, 3)}
end
function Vehicle:set_rotation(q) vehicles._set_rotation(self.address, q.x, q.y, q.z, q.w) end
function Vehicle:yaw()
  local f = self:forward()
  return math.deg(math.atan(f.x, f.z))
end
function Vehicle:set_yaw(degrees)
  local half = math.rad(degrees) / 2
  self:set_rotation({x = 0, y = math.sin(half), z = 0, w = math.cos(half)})
end
function Vehicle:set_name(name) vehicles._set_name(self.address, tostring(name)) end

-- The game ---------------------------------------------------------------------------------
function game.save() return game._save() end
function game.set_paused(paused) game._set_paused(paused ~= false) end
function game.time_scale() return game._time_scale() end
function game.set_time_scale(scale) game._set_time_scale(scale or 1) end
local TIMES = {Game = true, Morning = true, Afternoon = true, Evening = true, Night = true}
function world.time_of_day() return rex.cvar.get("renut_time_of_day", "Game") end
function world.set_time_of_day(time)
  assert(TIMES[time], "renut.world.set_time_of_day: Game, Morning, Afternoon, Evening or Night")
  rex.cvar.set("renut_time_of_day", time)
end

-- The game's sound cues ---------------------------------------------------------------------------
local Cue = {}
Cue.__index = Cue
function renut.sound.cue(cue, opts)
  if type(cue) == "string" then cue = math.tointeger(tonumber(cue)) end
  assert(math.type(cue) == "integer", "renut.sound.cue: the cue is the game's cue id (0x1BFBF80D)")
  opts = opts or {}
  local handle = renut.sound._cue(cue, opts.volume or 1)
  if not handle or handle == 0 then return nil end
  return setmetatable({handle = handle, cue = cue}, Cue)
end
function Cue:stop() renut.sound._cue_stop(self.handle) end
function Cue:playing() return renut.sound._cue_playing(self.handle) end

-- The pad ------------------------------------------------------------------------------------
local pad = renut.pad
local BUTTONS = {dpad_up = 0x0001, dpad_down = 0x0002, dpad_left = 0x0004, dpad_right = 0x0008, start = 0x0010,
                 back = 0x0020, left_stick = 0x0040, right_stick = 0x0080, lb = 0x0100, rb = 0x0200,
                 a = 0x1000, b = 0x2000, x = 0x4000, y = 0x8000}
local function button_bit(button)
  local bit = BUTTONS[type(button) == "string" and button:lower() or button]
  if not bit then error("not a pad button: a, b, x, y, lb, rb, start, back, left_stick, right_stick, dpad_up ...", 3) end
  return bit
end
local function bits(n, which) return math.tointeger(pad._get(n or 1, which)) or 0 end
function pad.down(button, n) return bits(n, 0) & button_bit(button) ~= 0 end
function pad.pressed(button, n) return bits(n, 1) & button_bit(button) ~= 0 end
function pad.released(button, n) return bits(n, 2) & button_bit(button) ~= 0 end
function pad.stick(side, n)
  local i = side == "right" and 5 or 3
  return pad._get(n or 1, i), pad._get(n or 1, i + 1)
end
function pad.trigger(side, n) return pad._get(n or 1, side == "right" and 8 or 7) end

-- The camera ---------------------------------------------------------------------------------
-- Where a world position is on the screen (0-1 across and down) and how far in front it is; nil behind the camera.
function camera.to_screen(p)
  local d = vec(p.x, p.y, p.z) - camera.position()
  local depth = d:dot(camera.forward())
  if depth < 0.05 then return nil end
  local t = math.tan(math.rad(camera.fov()) / 2)
  local sx, sy = d:dot(camera.right()) / (depth * t * renut.mouse._get(2)), d:dot(camera.up()) / (depth * t)
  return (sx + 1) / 2, (1 - sy) / 2, depth
end
function camera.shake(strength, seconds) camera._shake(strength or 1, seconds or 0.5) end
local follow
local own_release = camera.release
function camera.release()
  if follow then follow:off() follow = nil end
  own_release()
end
-- Keeps the camera on a vehicle (offset along its own axes), a character or object, a position, or a function's.
function camera.follow(target, offset, look_offset)
  camera.release()
  offset = offset or vec(0, 4, -10)
  look_offset = look_offset or vec(0, 1, 0)
  local is_vehicle = type(target) == "table" and target.address ~= nil
  follow = game.on("tick", function()
    local p
    if type(target) == "function" then p = target()
    elseif is_vehicle then p = target:has_body() and target:position() or nil
    elseif math.type(target) == "integer" then
      local ok, at = pcall(objects.position, target)
      p = ok and at or nil
    else p = target end
    if not p then return end
    local o = offset
    if is_vehicle then o = target:right() * offset.x + target:up() * offset.y + target:forward() * offset.z end
    local at = vec(p.x, p.y, p.z)
    camera.set(at + o, at + look_offset)
  end)
end
camera.unfollow = camera.release

-- Mod worlds: arriving where the mod says ---------------------------------------------------------
local own_go = renut.worlds.go
local arrival
function renut.worlds.go(id, opts)
  arrival = nil
  if opts and opts.at then
    arrival = {at = vec(xyz(opts.at, "renut.worlds.go: at")), yaw = opts.yaw, ticks = 0, entered = false}
  end
  own_go(id)
end
game.on("world_enter", function() if arrival then arrival.entered = true end end)
-- For the world's first ticks, while the game settles him at its own start.
game.on("tick", function()
  if not arrival or not arrival.entered then return end
  local p = player.position()
  if not p then return end
  local v = player.vehicle()
  if arrival.ticks == 0 or (p - arrival.at):length() > 3 then
    if v then
      if v:has_body() then
        v:set_position(arrival.at)
        if arrival.yaw then v:set_yaw(arrival.yaw) end
      end
    else
      player.teleport(arrival.at)
      if arrival.yaw then actors.turn("banjo", arrival.yaw) end
    end
  end
  arrival.ticks = arrival.ticks + 1
  if arrival.ticks >= 30 then arrival = nil end
end)

-- Drawing on the screen, through RexFabric's overlay (rex.draw) ----------------------------------------
-- 0xRRGGBB, or {r, g, b} from 0 to 1.
local function rgb_of(colour, default)
  if colour == nil then return default or 0xFFFFFF end
  if math.type(colour) == "integer" then return colour & 0xFFFFFF end
  assert(type(colour) == "table", "a colour is 0xRRGGBB or {r, g, b} from 0 to 1")
  local function byte(v) return math.max(0, math.min(255, math.floor((v or 0) * 255 + 0.5))) end
  return (byte(colour.r or colour.x or colour[1]) << 16) | (byte(colour.g or colour.y or colour[2]) << 8) |
         byte(colour.b or colour.z or colour[3])
end
local function key_of(mod, key) return "renut." .. mod .. "." .. tostring(key) end
local function forget(key) rex.draw.remove(key) rex.draw.clear(key .. "#") end

function hud.text(key, text, opts)
  opts = opts or {}
  rex.draw.text(tostring(text), opts.x or 0.5, opts.y or 0.15,
                {key = key_of(caller_mod(2), key), size = opts.size or 0.035, rgb = rgb_of(opts.colour),
                 alpha = opts.alpha, align = opts.align, background = opts.background, seconds = opts.seconds})
end
function hud.bar(key, fraction, opts)
  opts = opts or {}
  local k = key_of(caller_mod(2), key)
  local x, y, w, h = opts.x or 0.35, opts.y or 0.9, opts.width or 0.3, opts.height or 0.025
  local f = math.max(0, math.min(1, fraction or 0))
  rex.draw.rect(x, y, w, h, {key = k .. "#back", rgb = rgb_of(opts.back_colour, 0x000000),
                             alpha = opts.back_alpha or 0.6, rounding = h * 0.3, seconds = opts.seconds})
  rex.draw.rect(x, y, w * f, h, {key = k .. "#fill", rgb = rgb_of(opts.colour, 0x3CC864), alpha = opts.alpha,
                                 rounding = h * 0.3, seconds = opts.seconds})
  if opts.label then
    rex.draw.text(tostring(opts.label), x + w / 2, y + h / 2, {key = k .. "#label", size = h * 0.8, seconds = opts.seconds})
  else
    rex.draw.remove(k .. "#label")
  end
end

-- World markers and debug shapes are put on screen again each frame, from where the camera is then.
local markers = {}  -- key -> {at = vec or object, text, opts}
local shapes = {}   -- in order: {key, kind, points, text, opts, until}
local next_shape = 0
local menu          -- the open renut.hud.menu
local frame_handle
local draw_frame
local function want_frames() if not frame_handle then frame_handle = rex.on("frame", function() draw_frame() end) end end

function hud.marker(key, at, text, opts)
  local k = key_of(caller_mod(2), key)
  assert(math.type(at) == "integer" or type(at) == "table", "renut.hud.marker: at is an object or a position")
  markers[k] = {at = at, text = text and tostring(text) or nil, opts = opts or {}}
  want_frames()
end
function hud.remove(key)
  local k = key_of(caller_mod(2), key)
  markers[k] = nil
  forget(k)
end
function hud.clear()
  local prefix = key_of(caller_mod(2), "")
  for k in pairs(markers) do if k:sub(1, #prefix) == prefix then markers[k] = nil end end
  rex.draw.clear(prefix)
end

local function add_shape(kind, points, text, opts)
  opts = opts or {}
  local key = opts.key and key_of("debug", opts.key) or nil
  if not key then
    next_shape = next_shape + 1
    key = "renut.debug.#" .. next_shape
  end
  for i = #shapes, 1, -1 do if shapes[i].key == key then forget(key) table.remove(shapes, i) end end
  shapes[#shapes + 1] = {key = key, kind = kind, points = points, text = text, opts = opts,
                         until_ = rex.now() + ((opts.seconds or 0) > 0 and opts.seconds or 0.1)}
  want_frames()
end
local function point(p, what) return vec(xyz(p, what)) end
function renut.debug.line(a, b, opts) add_shape("line", {point(a, "a line's start"), point(b, "a line's end")}, nil, opts) end
function renut.debug.sphere(at, radius, opts) add_shape("sphere", {point(at, "a sphere's centre")}, radius or 1, opts) end
function renut.debug.box(at, size, opts) add_shape("box", {point(at, "a box's centre"), point(size, "a box's size")}, nil, opts) end
function renut.debug.text(at, text, opts) add_shape("text", {point(at, "the text's place")}, tostring(text), opts) end
function renut.debug.clear()
  for _, s in ipairs(shapes) do forget(s.key) end
  shapes = {}
end

local CORNERS = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}
local EDGES = {{1, 2}, {2, 3}, {3, 4}, {4, 1}, {5, 6}, {6, 7}, {7, 8}, {8, 5}, {1, 5}, {2, 6}, {3, 7}, {4, 8}}
local FRAME = 0.2  -- what's drawn each frame stays this long, so it goes soon after it's no longer drawn

local function draw_line(key, a, b, opts)
  local ax, ay = camera.to_screen(a)
  local bx, by = camera.to_screen(b)
  if ax and bx then
    rex.draw.line(ax, ay, bx, by, {key = key, rgb = rgb_of(opts.colour, 0x00FF80), alpha = opts.alpha,
                                   thickness = opts.thickness, seconds = FRAME})
  else
    rex.draw.remove(key)
  end
end

local function draw_shape(s)
  local opts, p = s.opts, s.points
  if s.kind == "line" then draw_line(s.key, p[1], p[2], opts)
  elseif s.kind == "sphere" then
    local x, y, depth = camera.to_screen(p[1])
    if not x then rex.draw.remove(s.key) return end
    local radius = s.text / (2 * depth * math.tan(math.rad(camera.fov()) / 2))
    rex.draw.circle(x, y, radius, {key = s.key, rgb = rgb_of(opts.colour, 0x00FF80), alpha = opts.alpha,
                                   filled = opts.filled == true, thickness = opts.thickness, seconds = FRAME})
  elseif s.kind == "box" then
    local c, half = p[1], p[2] / 2
    local corners = {}
    for i, k in ipairs(CORNERS) do corners[i] = c + vec(half.x * k[1], half.y * k[2], half.z * k[3]) end
    for i, e in ipairs(EDGES) do draw_line(s.key .. "#" .. i, corners[e[1]], corners[e[2]], opts) end
  elseif s.kind == "text" then
    local x, y = camera.to_screen(p[1])
    if not x then rex.draw.remove(s.key) return end
    rex.draw.text(s.text, x, y, {key = s.key, size = opts.size or 0.025, rgb = rgb_of(opts.colour), seconds = FRAME,
                                 background = opts.background})
  end
end

local function draw_marker(k, m)
  local at = m.at
  if math.type(at) == "integer" then
    local ok, p = pcall(objects.position, at)
    if not ok then markers[k] = nil forget(k) return end
    at = p
  end
  local x, y = camera.to_screen(at)
  if not x then forget(k) return end
  local o = m.opts
  local size = o.size or 0.012
  rex.draw.circle(x, y, size, {key = k, rgb = rgb_of(o.colour, 0xFFD040), alpha = o.alpha, seconds = FRAME})
  if m.text then
    rex.draw.text(m.text, x, y - size * 2.5, {key = k .. "#text", size = o.text_size or 0.025, rgb = rgb_of(o.colour, 0xFFD040),
                                              alpha = o.alpha, background = true, seconds = FRAME})
  end
end

-- A menu: up and down (the d-pad, the left stick, the arrow keys or the mouse) and A, Return or a click to pick;
-- B or Escape closes it. The game doesn't see the pad while it's open.
local MENU_ROW, MENU_WIDTH = 0.05, 0.4
local function close_menu(choice)
  local m = menu
  if not m then return end
  menu = nil
  rex.input.hold_guest(false)
  rex.draw.clear("renut.menu#")
  if m.fn then m.fn(choice, choice and m.options[choice] or nil) end
end
function hud.menu(def)
  assert(type(def) == "table" and type(def.options) == "table" and #def.options > 0,
         "renut.hud.menu{title = ..., options = {...}, fn = function(index, label) end}")
  close_menu(nil)
  menu = {title = def.title and tostring(def.title) or nil, options = {}, fn = def.fn, at = 1, stick = 0}
  for i, option in ipairs(def.options) do menu.options[i] = tostring(option) end
  rex.input.hold_guest(true)
  want_frames()
end
function hud.close_menu() close_menu(nil) end
function hud.menu_open() return menu ~= nil end

-- A pad button's own edge: a frame drawn twice in one game frame would see pad.pressed twice.
local function pad_edge(m, button)
  m.held = m.held or {}
  local now = pad.down(button)
  local edge = now and m.held[button] == false
  m.held[button] = now
  return edge
end

local function menu_input(m, top)
  local n = #m.options
  local _, stick_y = pad.stick("left")
  local stick = stick_y > 0.5 and 1 or stick_y < -0.5 and -1 or 0
  local up = pad_edge(m, "dpad_up") or rex.input.pressed("Up") or (stick == 1 and m.stick == 0)
  local down = pad_edge(m, "dpad_down") or rex.input.pressed("Down") or (stick == -1 and m.stick == 0)
  m.stick = stick
  if up then m.at = (m.at - 2) % n + 1 end
  if down then m.at = m.at % n + 1 end
  local mx, my = renut.mouse.position()
  local hovered
  if renut.mouse.inside() and math.abs(mx - 0.5) < MENU_WIDTH / 2 then
    local row = math.floor((my - top) / MENU_ROW) + 1
    if row >= 1 and row <= n then hovered = row end
  end
  if hovered and (m.mouse_x ~= mx or m.mouse_y ~= my) then m.at = hovered end
  m.mouse_x, m.mouse_y = mx, my
  if rex.input.pressed("LMB") and hovered then return hovered end
  if pad_edge(m, "a") or rex.input.pressed("Return") then return m.at end
  if pad_edge(m, "b") or rex.input.pressed("Escape") then return false end
end

local function draw_menu(m)
  local n = #m.options
  local height = MENU_ROW * n + (m.title and MENU_ROW * 1.2 or 0)
  local left, top = 0.5 - MENU_WIDTH / 2, 0.5 - height / 2
  local rows = top + (m.title and MENU_ROW * 1.2 or 0)
  local choice = menu_input(m, rows)
  if choice ~= nil then close_menu(choice or nil) return end
  rex.draw.rect(left - 0.01, top - 0.01, MENU_WIDTH + 0.02, height + 0.02,
                {key = "renut.menu#panel", rgb = 0x101418, alpha = 0.85, rounding = 0.01, seconds = FRAME})
  if m.title then
    rex.draw.text(m.title, 0.5, top + MENU_ROW * 0.6, {key = "renut.menu#title", size = 0.035, rgb = 0xFFD040, seconds = FRAME})
  end
  for i, option in ipairs(m.options) do
    local y = rows + MENU_ROW * (i - 1)
    if i == m.at then
      rex.draw.rect(left, y + 0.004, MENU_WIDTH, MENU_ROW - 0.008,
                    {key = "renut.menu#at", rgb = 0x3C78C8, alpha = 0.8, rounding = 0.008, seconds = FRAME})
    end
    rex.draw.text(option, 0.5, y + MENU_ROW / 2, {key = "renut.menu#" .. i, size = 0.03, seconds = FRAME})
  end
end

draw_frame = function()
  local now = rex.now()
  for i = #shapes, 1, -1 do
    if shapes[i].until_ <= now then forget(shapes[i].key) table.remove(shapes, i) end
  end
  local known = pcall(camera.position)
  if known and game.world() then
    for _, s in ipairs(shapes) do draw_shape(s) end
    for k, m in pairs(markers) do draw_marker(k, m) end
  end
  if menu then draw_menu(menu) end
  if not menu and #shapes == 0 and next(markers) == nil and frame_handle then
    frame_handle:off()
    frame_handle = nil
  end
end

-- The world's markers and shapes go with it.
game.on("world_leave", function()
  for k in pairs(markers) do forget(k) end
  markers = {}
  renut.debug.clear()
end)

-- Challenges made in Lua ----------------------------------------------------------------------------
-- A timer (time_limit: lose at 0; show_time: counting up), a counter (goal: win on reaching it), rewards when won.
local C = renut.challenges
local defs, running = {}, nil
local finish_listeners = {}
local Challenge = {}
Challenge.__index = Challenge

function C.add(def)
  assert(type(def) == "table" and type(def.id) == "string", "renut.challenges.add{id = ..., ...}")
  def.mod = caller_mod(2)
  defs[def.id] = def
  return def.id
end
function C.list()
  local list = {}
  for id, def in pairs(defs) do list[#list + 1] = {id = id, name = def.name or id, mod = def.mod} end
  table.sort(list, function(a, b) return a.id < b.id end)
  return list
end
function C.running() return running end
function C.on(event, fn)
  assert(event == "finish", "renut.challenges.on: the event is \"finish\"")
  return handle_of(finish_listeners, fn)
end

local function reward(r)
  for counter, n in pairs(r.counters or {}) do game.award(counter, n) end
  for _, flag in ipairs(r.flags or {}) do game.set_flag(flag) end
  for _, flag in ipairs(r.crates or {}) do renut.crates.give(flag) end
end

local function finish(c, won)
  if running ~= c then return end
  running = nil
  local d = c.def
  pcall(hud.hide)
  if won then
    if d.rewards then reward(d.rewards) end
    hud.message(d.win_text or "Challenge complete!", 3)
  else
    hud.message(d.lose_text or "Challenge failed", 3)
  end
  if d.on_end then d.on_end(c, won) end
  for _, h in ipairs({table.unpack(finish_listeners)}) do h.fn(c.id, won, c.time, c.score) end
end

function Challenge:set(n)
  self.score = n
  local d = self.def
  if d.goal or d.counter then hud.set_counter(n, d.goal or 0) end
  if d.goal and n >= d.goal then finish(self, true) end
end
function Challenge:add(n) self:set(self.score + (n or 1)) end
function Challenge:win() finish(self, true) end
function Challenge:lose() finish(self, false) end
function Challenge:running() return running == self end

function C.start(id)
  local d = defs[id]
  if not d then error("no challenge '" .. tostring(id) .. "' (renut.challenges.add it)", 2) end
  if running then error("a challenge is running already: " .. running.id, 2) end
  local c = setmetatable({id = id, def = d, score = 0, time = 0, started = renut.time()}, Challenge)
  running = c
  local timer, counter = d.time_limit ~= nil or d.show_time == true, d.goal ~= nil or d.counter == true
  if d.time_limit then hud.set_timer(d.time_limit, "down") elseif d.show_time then hud.set_timer(0, "up") end
  if counter then hud.set_counter(0, d.goal or 0) end
  if timer or counter then hud.panel{timer = timer, counter = counter} end
  if d.intro then hud.message(d.intro, 3) end
  if d.on_start then d.on_start(c) end
  return c
end
function C.stop(won) if running then finish(running, won == true) end end

hud.on("timer_done", function()
  if running and running.def.time_limit then finish(running, false) end
end)
game.on("tick", function()
  local c = running
  if not c then return end
  local before = c.time
  c.time = renut.time() - c.started
  if c.def.on_tick then c.def.on_tick(c, c.time - before) end
end)
game.on("world_leave", function() if running then finish(running, false) end end)

-- Mods: which are on, and what each shares ----------------------------------------------------------------
local mods = renut.mods
local exports, awaiting = {}, {}
function mods.list()
  local list = {}
  for name in mods._list():gmatch("[^\n]+") do list[#list + 1] = name end
  return list
end
function mods.enabled(name)
  for _, n in ipairs(mods.list()) do if n == name then return true end end
  return false
end
function mods.this() return caller_mod(2) end
function mods.export(value)
  local mod = caller_mod(2)
  exports[mod] = value
  for _, fn in ipairs(awaiting[mod] or {}) do fn(value) end
  awaiting[mod] = nil
end
function mods.import(name) return exports[name] end
-- fn(value) once `name` exports, at once if it has.
function mods.when(name, fn)
  if exports[name] ~= nil then fn(exports[name]) return end
  awaiting[name] = awaiting[name] or {}
  table.insert(awaiting[name], fn)
end

-- Settings: a mod's own cvars, in reNut's settings (F4) under "Mod: <mod>" -----------------------------------
local function cvar_of(mod, key) return ((mod .. "_" .. tostring(key)):lower():gsub("[^%w_]", "_")) end
function renut.settings.add(def)
  assert(type(def) == "table" and type(def.key) == "string" and def.default ~= nil,
         "renut.settings.add{key = ..., default = ..., description = ...}")
  local mod = caller_mod(2)
  local name, category, text = cvar_of(mod, def.key), "Nuts&Bolts/Mod: " .. mod, def.description or ""
  local d = def.default
  if type(d) == "boolean" then return rex.cvar.boolean(name, d, category, text) end
  if math.type(d) == "integer" then return rex.cvar.int32(name, d, category, text) end
  if type(d) == "number" then return rex.cvar.number(name, d, category, text) end
  if type(d) == "string" then return rex.cvar.string(name, d, category, text) end
  error("renut.settings.add: a default is true/false, a number or text", 2)
end
function renut.settings.get(key) return rex.cvar.get(cvar_of(caller_mod(2), key)) end
function renut.settings.set(key, value) rex.cvar.set(cvar_of(caller_mod(2), key), value) end
function renut.settings.on_change(key, fn)
  rex.cvar.on_change(cvar_of(caller_mod(2), key), function(_, value) fn(value) end)
end
)lua";

} // namespace

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("mods.", "Mods");
  renut.Args("mods._list", {});
  // The enabled mods' folder names, a line each.
  renut.Function("mods._list", [] {
    std::string list;
    for (const std::filesystem::path &dir : rexfabric::EnabledModDirs())
      list += (list.empty() ? "" : "\n") + rex::string::PathToUTF8(dir.filename());
    return list;
  });
  renut.Lua(kLua);
}

} // namespace renut::game::renut_lua
