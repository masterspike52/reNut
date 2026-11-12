#include "renut_engine/game/sound_api.h"

#include <rexfabric/game_api.h>

#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "renut_engine/game/stereo.h"
#include "renut_engine/mods/mod_loader.h"

namespace renut::game::sound_api {
namespace {

using Error = std::runtime_error;

// A sound file: a full path, or one relative to the mods folder ("my_mod/sounds/boing.wav").
std::filesystem::path SoundFile(const std::string &file) {
  std::filesystem::path path(std::u8string(file.begin(), file.end()));
  if (path.is_relative())
    path = renut::mods::ModsFolder() / path;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    throw Error("no sound file " + file);
  return path;
}

float Finite(double value, const char *what) {
  if (!std::isfinite(value))
    throw Error(std::string("not a finite ") + what);
  return float(value);
}

// renut.sound.play(file, opts): the calling mod's file, an object to change or stop it with.
constexpr const char kLua[] = R"lua(
local sound = renut.sound
local Sound = {}
Sound.__index = Sound

-- A path in the calling mod's folder (level 3: the script that called renut.sound.play), or a full one.
local function mod_file(path, level)
  assert(type(path) == "string" and #path > 0, "renut.sound.play: the sound is a file's path in the mod")
  if path:match("^%a:[/\\]") or path:match("^[/\\]") then return path end
  local source = debug.getinfo(level, "S").source or ""
  local folder = source:match("^@?(.*)[/\\]scripts[/\\][^/\\]+$")
  assert(folder, "renut.sound.play: '" .. path .. "' needs a full path here (not called from a mod's scripts folder)")
  return folder .. "/" .. path
end

local function where(at)
  if at == nil then return false, 0, 0, 0 end
  assert(type(at) == "table", "renut.sound: at is a position (a vector)")
  return true, at.x or 0, at.y or 0, at.z or 0
end

function sound.play(file, opts)
  opts = opts or {}
  assert(type(opts) == "table", "renut.sound.play: options are a table")
  local positional, x, y, z = where(opts.at)
  local handle = sound._play(mod_file(file, 3), opts.volume or 1, opts.pitch or 1, opts.loop == true, positional,
                             x, y, z, opts.range or 70)
  if handle == 0 then return nil end
  return setmetatable({handle = handle, volume = opts.volume or 1, at = opts.at}, Sound)
end

-- The music of every world from now on (the level's own paused), or back to the world's own without a file.
function sound.music(file, volume)
  if file == nil then sound._music("", 0) return end
  sound._music(mod_file(file, 3), volume or 1)
end

function Sound:stop() sound._stop(self.handle) end
function Sound:playing() return sound._playing(self.handle) end
function Sound:set_volume(volume)
  self.volume = volume
  local positional, x, y, z = where(self.at)
  sound._set(self.handle, volume, positional, x, y, z)
end
function Sound:move(at)
  self.at = at
  local positional, x, y, z = where(at)
  sound._set(self.handle, self.volume, positional, x, y, z)
end
function Sound:set_pitch(pitch)
  assert(type(pitch) == "number", "sound:set_pitch: the pitch is a number (2 is an octave up)")
  sound._pitch(self.handle, pitch)
end
)lua";

} // namespace

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("sound.", "Sound");  // the node editor's add menu
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"sound._play", {"file", "volume", "pitch", "loop", "positional", "x", "y", "z", "range"}},
           {"sound._set", {"sound", "volume", "positional", "x", "y", "z"}},
           {"sound._stop", {"sound"}},
           {"sound._pitch", {"sound", "pitch"}},
           {"sound._playing", {"sound"}},
           {"sound._music", {"file", "volume"}}})
    renut.Args(name, args);  // the node editor's pins

  renut.Function("sound._play", [](std::string file, double volume, double pitch, bool loop, bool positional, double x,
                                   double y, double z, double range) -> int64_t {
    const std::filesystem::path path = SoundFile(file);
    if (positional)
      Finite(x + y + z, "position");
    return renut::game::PlayModSound(path, Finite(volume, "volume"), Finite(pitch, "pitch"), loop, positional, float(x),
                                     float(y), float(z), Finite(range, "range"));
  });
  renut.Function("sound._set", [](int64_t handle, double volume, bool positional, double x, double y, double z) {
    if (positional)
      Finite(x + y + z, "position");
    renut::game::SetModSound(uint32_t(handle), Finite(volume, "volume"), positional, float(x), float(y), float(z));
  });
  renut.Function("sound._stop", [](int64_t handle) { renut::game::StopModSound(uint32_t(handle)); });
  renut.Function("sound._pitch", [](int64_t handle, double pitch) {
    renut::game::SetModSoundPitch(uint32_t(handle), Finite(pitch, "pitch"));
  });
  renut.Function("sound._playing", [](int64_t handle) { return renut::game::ModSoundPlaying(uint32_t(handle)); });
  renut.Function("sound._music", [](std::string file, double volume) {
    renut::game::SetMusicOverride(file.empty() ? std::filesystem::path() : SoundFile(file), Finite(volume, "volume"));
  });
  renut.Lua(kLua);
}

} // namespace renut::game::sound_api
