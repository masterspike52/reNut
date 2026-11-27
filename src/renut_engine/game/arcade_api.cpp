#include "renut_engine/game/arcade_api.h"

#include <rex/kernel/guest_memory.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rexfabric/game_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "renut_engine/game/guest_call.h"

REX_EXTERN(__imp__sub_8244B238);  // (arcade, dt): its tick
REX_EXTERN(__imp__sub_8244B520);  // (arcade): tears it down
REX_EXTERN(__imp__sub_82449338);  // (Klungo, state): his state, with its sound and pose

namespace renut::game::arcade_api {
namespace {

using Error = std::runtime_error;
using renut::guest::Load32;
using renut::guest::Store32;

// Klungo's arcade in Showdown Town (built by sub_82449E98, one at a time): 0 while it's closed.
constexpr uint32_t kArcade = 0x82E19AE8;
// Its five levels' text, read as it opens, and its sprites' rects {x0, y0, x1, y1} in its sprite sheet's pixels.
constexpr uint32_t kMaps = 0x82D8F55C;
constexpr uint32_t kSprites = 0x82DA3170;
constexpr uint32_t kLevelCount = 5;
constexpr uint32_t kBossLevel = 4;
constexpr uint32_t kSpriteCount = 21;
constexpr size_t kMapRows = 8;        // the screen is 240 high, a tile 32
constexpr size_t kMapColumns = 10;    // the last 10 are the screen the camera stops at
constexpr float kScreenWidth = 320.0f;

// The arcade's fields.
constexpr uint32_t kState = 88;
constexpr uint32_t kCrashChance = 96;  // one in this a frame, while Klungo can crash
constexpr uint32_t kUnlocked = 124;    // the last level the level menu lets him play, 0 to 4
constexpr uint32_t kWon = 128;         // the last level won, -1 for none
constexpr uint32_t kLevel = 132;
constexpr uint32_t kCamera = 160;  // the screen's left edge in the level
constexpr uint32_t kScroll = 164;  // what the camera moves a frame
constexpr uint32_t kLevels = 204;  // the five level objects
constexpr uint32_t kCrashSeconds = 256;
constexpr uint32_t kPlayer = 260;
constexpr uint32_t kBoss = 264;
constexpr uint32_t kPaused = 273;
constexpr uint32_t kShotMin = 276;  // the boss's seconds between shots, at least and at most
constexpr uint32_t kShotMax = 280;
constexpr uint32_t kShotSpeed = 284;

// A level: its width in pixels, its tiles (a byte each, row by row) and the sprite its enemies are under.
constexpr uint32_t kLevelWidth = 4;
constexpr uint32_t kColumns = 12;
constexpr uint32_t kRows = 16;
constexpr uint32_t kGrid = 28;
constexpr uint32_t kRoot = 32;
constexpr uint32_t kChildren = 248;  // a sprite's children: begin, end

// A sprite (Klungo, the enemies and the boss are), then Klungo's and the boss's own fields.
constexpr uint32_t kDirty = 80;
constexpr uint32_t kX = 200;
constexpr uint32_t kY = 204;
constexpr uint32_t kHalfWidth = 208;
constexpr uint32_t kCanCrash = 276;
constexpr uint32_t kSpeed = 280;
constexpr uint32_t kPlayerState = 296;
constexpr uint32_t kVx = 308;
constexpr uint32_t kVy = 312;
constexpr uint32_t kGravity = 316;
constexpr uint32_t kJump = 324;
constexpr uint32_t kOnGround = 332;
constexpr uint32_t kBossBeaten = 276;

constexpr uint32_t kPlaying = 6;
constexpr uint32_t kEntering = 0, kRunning = 1, kDying = 2, kBossFight = 4;

constexpr const char *kStates[] = {"starting", "closing", "title", "main_menu", "level_menu", "credits", "playing"};
constexpr const char *kPlayerStates[] = {"entering", "running",  "dying", "finishing", "boss",
                                         "credits",  "finished", "dead",  "crashed"};
constexpr char kTiles[] = {' ', 'X', 'T', 'F'};
constexpr const char *kTuningKeys[] = {"speed",         "gravity",       "jump",          "crash_chance",
                                       "crash_seconds", "boss_shot_min", "boss_shot_max", "boss_shot_speed",
                                       "unlocked"};

uint8_t *Base() { return REX_GUEST_MEMORY()->GetView().TranslateVirtual<uint8_t *>(0); }

bool Heap(uint32_t va) { return va >= 0x40000000 && va < 0xC0000000; }

float LoadFloat(const uint8_t *base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void StoreFloat(uint8_t *base, uint32_t va, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Store32(base, va, bits);
}

template <size_t N>
std::string NameOf(const char *const (&names)[N], uint32_t i) {
  return i < N ? names[i] : "unknown";
}

float Finite(double value, const char *what) {
  if (!std::isfinite(value))
    throw Error(std::string(what) + " is a number");
  return float(value);
}

uint32_t Arcade(const uint8_t *base) {
  const uint32_t arcade = Load32(base, kArcade);
  if (!Heap(arcade))
    throw Error("Klungo's arcade isn't open");
  return arcade;
}

uint32_t Player(const uint8_t *base, uint32_t arcade) {
  const uint32_t player = Load32(base, arcade + kPlayer);
  if (!Heap(player))
    throw Error("the arcade has no Klungo yet");
  return player;
}

uint32_t LevelOf(const uint8_t *base, uint32_t arcade) {
  const uint32_t level = Load32(base, arcade + kLevel);
  const uint32_t object = level < kLevelCount ? Load32(base, arcade + kLevels + 4 * level) : 0;
  if (!Heap(object))
    throw Error("the arcade has no level up");
  return object;
}

uint32_t LevelIndex(int64_t n) {
  if (n < 1 || n > int64_t(kLevelCount))
    throw Error("the arcade's levels are 1 to 5");
  return uint32_t(n - 1);
}

uint32_t SpriteAt(int64_t i) {
  if (i < 0 || i >= int64_t(kSpriteCount))
    throw Error("the arcade's sprites are 0 to 20");
  return kSprites + 16 * uint32_t(i);
}

// The byte of the level's tile at (column, row), both from 1, row 1 the top.
uint32_t TileAt(const uint8_t *base, uint32_t level, int64_t column, int64_t row) {
  const int64_t columns = int32_t(Load32(base, level + kColumns));
  const int64_t rows = std::min<int64_t>(int32_t(Load32(base, level + kRows)), kMapRows);
  if (column < 1 || column > columns || row < 1 || row > rows)
    throw Error("no tile there: the level is " + std::to_string(columns) + " by " + std::to_string(rows));
  return Load32(base, level + kGrid) + uint32_t((row - 1) * columns + (column - 1));
}

std::vector<uint32_t> Enemies(const uint8_t *base, uint32_t level) {
  const uint32_t root = Load32(base, level + kRoot);
  if (!Heap(root))
    return {};
  const uint32_t begin = Load32(base, root + kChildren), end = Load32(base, root + kChildren + 4);
  if (!Heap(begin) || end < begin || end - begin > 4 * 0x10000)
    return {};
  std::vector<uint32_t> enemies;
  for (uint32_t va = begin; va < end; va += 4)
    enemies.push_back(Load32(base, va));
  return enemies;
}

uint32_t Enemy(const uint8_t *base, int64_t i) {
  const std::vector<uint32_t> enemies = Enemies(base, LevelOf(base, Arcade(base)));
  if (i < 1 || i > int64_t(enemies.size()) || !Heap(enemies[size_t(i - 1)]))
    throw Error("the level has " + std::to_string(enemies.size()) + " enemies");
  return enemies[size_t(i - 1)];
}

// A map the way the game's are: 8 lines, the ground at the bottom, each line ending in a newline.
std::string Normalize(const std::string &text) {
  std::vector<std::string> lines(1);
  for (const char c : text) {
    if (c == '\n')
      lines.emplace_back();
    else if (c != '\r')
      lines.back() += c;
  }
  if (lines.size() > 1 && lines.back().empty())
    lines.pop_back();
  if (lines.size() > kMapRows)
    throw Error("a map is 8 lines at most (the screen's height)");
  size_t columns = 0;
  for (const std::string &line : lines) {
    columns = std::max(columns, line.size());
    for (const char c : line)
      if (c != ' ' && c != 'X' && c != 'T' && c != 'F' && (c < '1' || c > '9'))
        throw Error(std::string("a map has no tile '") + c + "': space, X, T, F, or 1 to 9 enemies");
  }
  if (columns < kMapColumns)
    throw Error("a map is 10 columns at least (the screen's width)");
  std::string map(kMapRows - lines.size(), '\n');
  for (const std::string &line : lines)
    map += line + '\n';
  return map;
}

bool TuningKey(const std::string &key) {
  return std::find_if(std::begin(kTuningKeys), std::end(kTuningKeys),
                      [&](const char *k) { return key == k; }) != std::end(kTuningKeys);
}

void CheckTuning(const std::string &key, double value) {
  if (!TuningKey(key))
    throw Error("no arcade tuning '" + key +
                "': speed, gravity, jump, crash_chance, crash_seconds, boss_shot_min, boss_shot_max, "
                "boss_shot_speed or unlocked");
  Finite(value, key.c_str());
  const bool whole = value == std::floor(value);
  if (key == "unlocked" && (!whole || value < 1 || value > kLevelCount))
    throw Error("unlocked is a level, 1 to 5");
  if (key == "crash_chance" && (!whole || value < 0 || value > std::numeric_limits<int32_t>::max()))
    throw Error("crash_chance is one in that many frames (a whole number), 0 for never");
  if ((key == "speed" || key == "crash_seconds" || key == "boss_shot_min" || key == "boss_shot_max") && value < 0)
    throw Error(key + " can't be below 0");
}

// What the mods changed, under g_mutex: the tuning put in as the arcade opens, and the game's own maps and sprites.
std::mutex g_mutex;
std::map<std::string, double> g_tuning;
std::array<uint32_t, kLevelCount> g_game_maps{};  // 0 while the game's own is in
std::array<uint32_t, kLevelCount> g_map_copies{};
std::array<std::array<uint32_t, 4>, kSpriteCount> g_game_sprites{};
std::array<bool, kSpriteCount> g_sprite_changed{};

std::atomic<bool> g_retune{false};
std::atomic<bool> g_kill{false};
std::atomic<bool> g_win{false};
std::atomic<bool> g_full_speed{false};

struct Events {
  rexfabric::Event open, close, tick, state, player;
};
std::optional<Events> g_events;

// The game thread's: the arcade its events are about, and its states last tick.
uint32_t g_seen = 0;
uint32_t g_state = 0;
uint32_t g_player_state = 0;

void Retune(uint8_t *base, uint32_t arcade) {
  const uint32_t player = Load32(base, arcade + kPlayer);
  if (!Heap(player))
    return;
  std::map<std::string, double> tuning;
  {
    std::lock_guard lock(g_mutex);
    tuning = g_tuning;
  }
  for (const auto &[key, value] : tuning) {
    const float f = float(value);
    if (key == "speed") {
      StoreFloat(base, arcade + kScroll, f);
      StoreFloat(base, player + kSpeed, f);
    } else if (key == "gravity") {
      StoreFloat(base, player + kGravity, f);
    } else if (key == "jump") {
      StoreFloat(base, player + kJump, -f);
    } else if (key == "crash_chance") {
      if (value < 1)
        base[player + kCanCrash] = 0;
      else
        Store32(base, arcade + kCrashChance, uint32_t(value));
    } else if (key == "crash_seconds") {
      StoreFloat(base, arcade + kCrashSeconds, f);
    } else if (key == "boss_shot_min") {
      StoreFloat(base, arcade + kShotMin, f);
    } else if (key == "boss_shot_max") {
      StoreFloat(base, arcade + kShotMax, f);
    } else if (key == "boss_shot_speed") {
      StoreFloat(base, arcade + kShotSpeed, f);
    } else if (key == "unlocked") {
      Store32(base, arcade + kUnlocked, uint32_t(value) - 1);
    }
  }
  // sub_82448B98 picks the next shot as rand() % the ticks between the two, so they can't round the same.
  const float min = LoadFloat(base, arcade + kShotMin);
  if (int(LoadFloat(base, arcade + kShotMax) * 30.0f) <= int(min * 30.0f))
    StoreFloat(base, arcade + kShotMax, min + 0.1f);
}

double LiveTuning(const uint8_t *base, uint32_t arcade, const std::string &key) {
  const uint32_t player = Player(base, arcade);
  if (key == "speed")
    return LoadFloat(base, player + kSpeed);
  if (key == "gravity")
    return LoadFloat(base, player + kGravity);
  if (key == "jump")
    return -LoadFloat(base, player + kJump);
  if (key == "crash_chance")
    return base[player + kCanCrash] ? double(int32_t(Load32(base, arcade + kCrashChance))) : 0.0;
  if (key == "crash_seconds")
    return LoadFloat(base, arcade + kCrashSeconds);
  if (key == "boss_shot_min")
    return LoadFloat(base, arcade + kShotMin);
  if (key == "boss_shot_max")
    return LoadFloat(base, arcade + kShotMax);
  if (key == "boss_shot_speed")
    return LoadFloat(base, arcade + kShotSpeed);
  return double(int32_t(Load32(base, arcade + kUnlocked)) + 1);
}

// renut.arcade.win: the boss beaten in its level (the game's own fall and run follow), else Klungo at the level's end.
void Finish(uint8_t *base, uint32_t arcade, uint32_t player, uint32_t state) {
  const uint32_t level = Load32(base, arcade + kLevel);
  if (level == kBossLevel) {
    const uint32_t boss = Load32(base, arcade + kBoss);
    if (state == kBossFight && Heap(boss) && !Load32(base, boss + kBossBeaten))
      Store32(base, boss + kBossBeaten, 1);
    return;
  }
  const uint32_t object = level < kLevelCount ? Load32(base, arcade + kLevels + 4 * level) : 0;
  if (state != kRunning || !Heap(object))
    return;
  const float end = float(int32_t(Load32(base, object + kLevelWidth))) - kScreenWidth;
  StoreFloat(base, arcade + kCamera, end);
  StoreFloat(base, player + kX, end + LoadFloat(base, player + kHalfWidth) + 1.0f);
  base[player + kDirty] = 1;
}

void BeforeTick(uint8_t *base, uint32_t arcade) {
  if (arcade == g_seen) {
    if (g_retune.exchange(false))
      Retune(base, arcade);
    return;
  }
  g_seen = arcade;
  g_state = Load32(base, arcade + kState);
  const uint32_t player = Load32(base, arcade + kPlayer);
  g_player_state = Heap(player) ? Load32(base, player + kPlayerState) : 0;
  g_retune = false;
  g_kill = false;
  g_win = false;
  Retune(base, arcade);
  if (g_events)
    g_events->open.Fire();
}

void AfterTick(PPCContext &ctx, uint8_t *base, uint32_t arcade, double dt) {
  const uint32_t player = Load32(base, arcade + kPlayer);
  if (!Heap(player))
    return;
  const bool kill = g_kill.exchange(false), win = g_win.exchange(false);
  const bool playing = Load32(base, arcade + kState) == kPlaying && !base[arcade + kPaused];
  const uint32_t now = Load32(base, player + kPlayerState);
  if (playing && kill && (now == kEntering || now == kRunning || now == kBossFight)) {
    const auto saved = ctx;
    ctx.r3.u64 = player;
    ctx.r4.u64 = kDying;
    __imp__sub_82449338(ctx, base);
    ctx = saved;
  } else if (playing && win) {
    Finish(base, arcade, player, now);
  }

  const uint32_t state = Load32(base, arcade + kState);
  const uint32_t player_state = Load32(base, player + kPlayerState);
  const uint32_t old_state = std::exchange(g_state, state);
  const uint32_t old_player = std::exchange(g_player_state, player_state);
  if (!g_events)
    return;
  if (state != old_state)
    g_events->state.Fire(NameOf(kStates, state), NameOf(kStates, old_state));
  if (player_state != old_player)
    g_events->player.Fire(NameOf(kPlayerStates, player_state), NameOf(kPlayerStates, old_player));
  g_events->tick.Fire(dt);
}

void Closing(uint32_t arcade) {
  if (arcade != g_seen)
    return;
  g_seen = 0;
  g_full_speed = false;
  if (g_events)
    g_events->close.Fire();
}

// renut.arcade: the getters, setters and listeners, on the functions Declare makes.
constexpr const char kLua[] = R"lua(
local arcade = renut.arcade

local function whole(n, what)
  local i = math.tointeger(n)
  if not i then error("renut.arcade: " .. what .. " is a whole number", 3) end
  return i
end
local SPRITES = {tile_x = 0, tile_t = 1, finish = 2, klungo_run1 = 3, klungo_run2 = 4, klungo_run3 = 5,
                 klungo_run4 = 6, klungo_credits = 7, boss1 = 8, boss2 = 9, boss_beaten = 10, boss_shot1 = 13,
                 boss_shot2 = 14, enemy1 = 15, enemy2 = 16, background = 17, crash_screen = 18,
                 crash_overlay = 19, bezel = 20}
local function sprite_of(sprite)
  if type(sprite) == "string" then
    local i = SPRITES[sprite]
    if not i then error("renut.arcade: no sprite '" .. sprite .. "' (tile_x, klungo_run1, enemy1, background ...)", 3) end
    return i
  end
  return whole(sprite, "a sprite's number")
end
local function when_open(fn) return function(...) if arcade._open() then return fn(...) end return nil end end

function arcade.open() return arcade._open() end
arcade.state = when_open(function() return arcade._state("game") end)
arcade.level = when_open(function() return math.tointeger(arcade._get("level")) end)
arcade.won = when_open(function() return math.tointeger(arcade._get("won")) end)
arcade.paused = when_open(function() return arcade._get("paused") ~= 0 end)
arcade.camera = when_open(function() return arcade._get("camera") end)
arcade.player = when_open(function()
  local get = arcade._get
  return {x = get("x"), y = get("y"), vx = get("vx"), vy = get("vy"), state = arcade._state("player"),
          on_ground = get("on_ground") ~= 0}
end)
arcade.size = when_open(function()
  local get = arcade._get
  return math.tointeger(get("columns")), math.tointeger(get("rows")), math.tointeger(get("width"))
end)
function arcade.set_player(t)
  assert(type(t) == "table", "renut.arcade.set_player{x = ..., y = ..., vx = ..., vy = ...}")
  for _, key in ipairs({"x", "y", "vx", "vy"}) do
    if t[key] ~= nil then arcade._set(key, t[key]) end
  end
end
function arcade.set_camera(x) arcade._set("camera", x) end

function arcade.tile(column, row) return arcade._tile(whole(column, "the column"), whole(row, "the row")) end
function arcade.set_tile(column, row, tile)
  arcade._set_tile(whole(column, "the column"), whole(row, "the row"), tostring(tile or " "))
end
function arcade.enemies()
  local list = {}
  if not arcade._open() then return list end
  for i = 1, math.tointeger(arcade._get("enemies")) do
    list[i] = {index = i, x = arcade._enemy(i, 0), y = arcade._enemy(i, 1)}
  end
  return list
end
function arcade.move_enemy(i, x, y) arcade._move_enemy(whole(i, "the enemy"), x, y) end

function arcade.map(level) return arcade._map(whole(level, "the level")) end
function arcade.set_map(level, text)
  assert(type(text) == "string", "renut.arcade.set_map(level, text): the map is its text, a line a row")
  arcade._set_map(whole(level, "the level"), text)
end
function arcade.reset_map(level) arcade._reset_map(whole(level, "the level")) end

function arcade.sprite(sprite)
  local i = sprite_of(sprite)
  return {x0 = arcade._sprite(i, 0), y0 = arcade._sprite(i, 1), x1 = arcade._sprite(i, 2), y1 = arcade._sprite(i, 3)}
end
function arcade.set_sprite(sprite, rect)
  assert(type(rect) == "table", "renut.arcade.set_sprite(sprite, {x0 = ..., y0 = ..., x1 = ..., y1 = ...})")
  arcade._set_sprite(sprite_of(sprite), whole(rect.x0 or rect[1], "x0"), whole(rect.y0 or rect[2], "y0"),
                     whole(rect.x1 or rect[3], "x1"), whole(rect.y1 or rect[4], "y1"))
end
function arcade.reset_sprite(sprite) arcade._reset_sprite(sprite_of(sprite)) end

function arcade.tune(t)
  assert(type(t) == "table", "renut.arcade.tune{speed = ..., gravity = ..., jump = ..., ...}")
  for key, value in pairs(t) do arcade._tune(tostring(key), value) end
end
function arcade.untune(key) arcade._untune(key and tostring(key) or "") end
local WHOLE = {crash_chance = true, unlocked = true}
function arcade.tuning(key)
  local value = arcade._tuning(tostring(key))
  if value ~= value then return nil end
  return WHOLE[key] and math.tointeger(value) or value
end

function arcade.kill() arcade._kill() end
function arcade.win() arcade._win() end
function arcade.full_speed(on) arcade._full_speed(on and true or false) end

local RAW = {open = "renut.arcade_open", close = "renut.arcade_close", tick = "renut.arcade_tick",
             state = "renut.arcade_state", player = "renut.arcade_player"}
local KLUNGO = {won = "finishing", died = "dying", crashed = "crashed"}
function arcade.on(event, fn)
  assert(type(fn) == "function", "renut.arcade.on(event, fn)")
  if RAW[event] then return rex.on(RAW[event], fn) end
  if event == "start" then
    return rex.on("renut.arcade_state", function(new) if new == "playing" then fn(arcade.level()) end end)
  end
  local wanted = KLUNGO[event]
  if wanted then
    return rex.on("renut.arcade_player", function(new) if new == wanted then fn(arcade.level()) end end)
  end
  error("renut.arcade.on: the events are open, close, tick, state, player, start, won, died and crashed", 2)
end
)lua";

} // namespace

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("arcade", "Klungo's Arcade");  // the node editor's add menu
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"arcade._open", {}},
           {"arcade._state", {"which"}},
           {"arcade._get", {"key"}},
           {"arcade._set", {"key", "value"}},
           {"arcade._tile", {"column", "row"}},
           {"arcade._set_tile", {"column", "row", "tile"}},
           {"arcade._enemy", {"enemy", "axis"}},
           {"arcade._move_enemy", {"enemy", "x", "y"}},
           {"arcade._map", {"level"}},
           {"arcade._set_map", {"level", "text"}},
           {"arcade._reset_map", {"level"}},
           {"arcade._sprite", {"sprite", "corner"}},
           {"arcade._set_sprite", {"sprite", "x0", "y0", "x1", "y1"}},
           {"arcade._reset_sprite", {"sprite"}},
           {"arcade._tune", {"key", "value"}},
           {"arcade._untune", {"key"}},
           {"arcade._tuning", {"key"}},
           {"arcade._kill", {}},
           {"arcade._win", {}},
           {"arcade._full_speed", {"on"}},
           {"arcade_tick", {"dt"}},
           {"arcade_state", {"new", "old"}},
           {"arcade_player", {"new", "old"}}})
    renut.Args(name, args);  // the node editor's pins

  renut.Function("arcade._open", [] { return Heap(Load32(Base(), kArcade)); });
  // "game": the arcade's screen; "player": Klungo's state.
  renut.Function("arcade._state", [](std::string which) -> std::string {
    const uint8_t *base = Base();
    const uint32_t arcade = Arcade(base);
    if (which == "game")
      return NameOf(kStates, Load32(base, arcade + kState));
    if (which == "player")
      return NameOf(kPlayerStates, Load32(base, Player(base, arcade) + kPlayerState));
    throw Error("renut.arcade._state: \"game\" or \"player\"");
  });
  renut.Function("arcade._get", [](std::string key) -> double {
    const uint8_t *base = Base();
    const uint32_t arcade = Arcade(base);
    if (key == "level")
      return double(Load32(base, arcade + kLevel) + 1);
    if (key == "won")
      return double(int32_t(Load32(base, arcade + kWon)) + 1);
    if (key == "camera")
      return LoadFloat(base, arcade + kCamera);
    if (key == "paused")
      return base[arcade + kPaused];
    if (key == "width" || key == "columns" || key == "rows" || key == "enemies") {
      const uint32_t level = LevelOf(base, arcade);
      if (key == "width")
        return double(int32_t(Load32(base, level + kLevelWidth)));
      if (key == "columns")
        return double(int32_t(Load32(base, level + kColumns)));
      if (key == "rows")
        return double(std::min<int32_t>(int32_t(Load32(base, level + kRows)), int32_t(kMapRows)));
      return double(Enemies(base, level).size());
    }
    const uint32_t player = Player(base, arcade);
    if (key == "x")
      return LoadFloat(base, player + kX);
    if (key == "y")
      return LoadFloat(base, player + kY);
    if (key == "vx")
      return LoadFloat(base, player + kVx);
    if (key == "vy")
      return LoadFloat(base, player + kVy);
    if (key == "on_ground")
      return base[player + kOnGround];
    throw Error("renut.arcade._get: no '" + key + "'");
  });
  renut.Function("arcade._set", [](std::string key, double value) {
    uint8_t *base = Base();
    const uint32_t arcade = Arcade(base);
    const float f = Finite(value, key.c_str());
    if (key == "camera") {
      StoreFloat(base, arcade + kCamera, f);
      return;
    }
    const uint32_t player = Player(base, arcade);
    const uint32_t field = key == "x" ? kX : key == "y" ? kY : key == "vx" ? kVx : key == "vy" ? kVy : 0;
    if (!field)
      throw Error("renut.arcade._set: no '" + key + "'");
    StoreFloat(base, player + field, f);
    base[player + kDirty] = 1;
  });

  renut.Function("arcade._tile", [](int64_t column, int64_t row) -> std::string {
    const uint8_t *base = Base();
    const int8_t tile = int8_t(base[TileAt(base, LevelOf(base, Arcade(base)), column, row)]);
    return std::string(1, tile >= 0 && tile < 4 ? kTiles[tile] : ' ');
  });
  renut.Function("arcade._set_tile", [](int64_t column, int64_t row, std::string tile) {
    const char *found = tile.size() == 1 ? std::find(std::begin(kTiles), std::end(kTiles), tile[0]) : std::end(kTiles);
    if (found == std::end(kTiles))
      throw Error("a tile is \" \", \"X\", \"T\" or \"F\"");
    uint8_t *base = Base();
    base[TileAt(base, LevelOf(base, Arcade(base)), column, row)] = uint8_t(found - kTiles);
  });
  renut.Function("arcade._enemy", [](int64_t i, int64_t axis) -> double {
    const uint8_t *base = Base();
    return LoadFloat(base, Enemy(base, i) + (axis ? kY : kX));
  });
  renut.Function("arcade._move_enemy", [](int64_t i, double x, double y) {
    uint8_t *base = Base();
    const uint32_t enemy = Enemy(base, i);
    StoreFloat(base, enemy + kX, Finite(x, "x"));
    StoreFloat(base, enemy + kY, Finite(y, "y"));
    base[enemy + kDirty] = 1;
  });

  renut.Function("arcade._map", [](int64_t n) -> std::string {
    const uint8_t *base = Base();
    const uint32_t text = Load32(base, kMaps + 4 * LevelIndex(n));
    std::string map;
    for (uint32_t va = text; text && base[va] && map.size() < 0x100000; ++va)
      map += char(base[va]);
    return map;
  });
  renut.Function("arcade._set_map", [](int64_t n, std::string text) {
    const uint32_t i = LevelIndex(n);
    const std::string map = Normalize(text);
    rex::kernel::ISystemHeap &heap = REX_RUNTIME()->GetGuestHeap();
    const uint32_t copy = heap.SystemHeapAlloc(uint32_t(map.size() + 1));
    if (!copy)
      throw Error("no guest memory for the map");
    uint8_t *base = Base();
    std::memcpy(base + copy, map.c_str(), map.size() + 1);
    std::lock_guard lock(g_mutex);
    if (!g_game_maps[i])
      g_game_maps[i] = Load32(base, kMaps + 4 * i);
    Store32(base, kMaps + 4 * i, copy);
    if (g_map_copies[i])
      heap.SystemHeapFree(g_map_copies[i]);
    g_map_copies[i] = copy;
  });
  renut.Function("arcade._reset_map", [](int64_t n) {
    const uint32_t i = LevelIndex(n);
    std::lock_guard lock(g_mutex);
    if (!g_game_maps[i])
      return;
    Store32(Base(), kMaps + 4 * i, std::exchange(g_game_maps[i], 0));
    REX_RUNTIME()->GetGuestHeap().SystemHeapFree(std::exchange(g_map_copies[i], 0));
  });

  renut.Function("arcade._sprite", [](int64_t i, int64_t corner) -> int64_t {
    if (corner < 0 || corner > 3)
      throw Error("renut.arcade._sprite: corner 0 to 3 (x0, y0, x1, y1)");
    return int32_t(Load32(Base(), SpriteAt(i) + 4 * uint32_t(corner)));
  });
  renut.Function("arcade._set_sprite", [](int64_t i, int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    const uint32_t at = SpriteAt(i);
    if (x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 || x1 > 4096 || y1 > 4096)
      throw Error("a sprite's rect is x0 < x1 and y0 < y1, in the sprite sheet's pixels");
    uint8_t *base = Base();
    std::lock_guard lock(g_mutex);
    if (!g_sprite_changed[size_t(i)]) {
      for (uint32_t k = 0; k < 4; ++k)
        g_game_sprites[size_t(i)][k] = Load32(base, at + 4 * k);
      g_sprite_changed[size_t(i)] = true;
    }
    const int64_t rect[4] = {x0, y0, x1, y1};
    for (uint32_t k = 0; k < 4; ++k)
      Store32(base, at + 4 * k, uint32_t(rect[k]));
  });
  renut.Function("arcade._reset_sprite", [](int64_t i) {
    const uint32_t at = SpriteAt(i);
    std::lock_guard lock(g_mutex);
    if (!std::exchange(g_sprite_changed[size_t(i)], false))
      return;
    for (uint32_t k = 0; k < 4; ++k)
      Store32(Base(), at + 4 * k, g_game_sprites[size_t(i)][k]);
  });

  renut.Function("arcade._tune", [](std::string key, double value) {
    CheckTuning(key, value);
    std::lock_guard lock(g_mutex);
    g_tuning[key] = value;
    g_retune = true;
  });
  // "" for all of them: the game's own values come back as the arcade next opens.
  renut.Function("arcade._untune", [](std::string key) {
    if (!key.empty() && !TuningKey(key))
      CheckTuning(key, 0);
    std::lock_guard lock(g_mutex);
    if (key.empty())
      g_tuning.clear();
    else
      g_tuning.erase(key);
  });
  // The open arcade's value, else the mods' (NaN for none).
  renut.Function("arcade._tuning", [](std::string key) -> double {
    if (!TuningKey(key))
      CheckTuning(key, 0);
    const uint8_t *base = Base();
    if (const uint32_t arcade = Load32(base, kArcade); Heap(arcade) && Heap(Load32(base, arcade + kPlayer)))
      return LiveTuning(base, arcade, key);
    std::lock_guard lock(g_mutex);
    const auto it = g_tuning.find(key);
    return it != g_tuning.end() ? it->second : std::numeric_limits<double>::quiet_NaN();
  });

  // Both wait for the arcade's next tick, and only do something while a level is played.
  renut.Function("arcade._kill", [] {
    if (Heap(Load32(Base(), kArcade)))
      g_kill = true;
  });
  renut.Function("arcade._win", [] {
    if (Heap(Load32(Base(), kArcade)))
      g_win = true;
  });
  // Only while the arcade is open; it goes off again as the arcade closes.
  renut.Function("arcade._full_speed", [](bool on) { g_full_speed = on && Heap(Load32(Base(), kArcade)); });

  g_events = Events{renut.Event("arcade_open"), renut.Event("arcade_close"), renut.Event("arcade_tick"),
                    renut.Event("arcade_state"), renut.Event("arcade_player")};
  renut.Lua(kLua);
}

bool FullSpeed() { return g_full_speed.load(std::memory_order_relaxed); }

} // namespace renut::game::arcade_api

// The arcade's tick: tuning in as it opens, then renut.arcade's kill and win, then its events. Raw: the original
// runs with the game's own context.
REX_HOOK_RAW(sub_8244B238) {
  namespace api = renut::game::arcade_api;
  const uint32_t arcade = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  api::BeforeTick(base, arcade);
  __imp__sub_8244B238(ctx, base);
  api::AfterTick(ctx, base, arcade, dt);
}

// The arcade closing: renut.arcade_close while it's still whole.
REX_HOOK_RAW(sub_8244B520) {
  renut::game::arcade_api::Closing(ctx.r3.u32);
  __imp__sub_8244B520(ctx, base);
}
