#include "renut_engine/game/mod_data.h"

#include <rexfabric/game_api.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "renut_engine/app/app_paths.h"
#include "renut_engine/online/online.h"

namespace renut::game::mod_data {
namespace {

using Error = std::runtime_error;
using json = nlohmann::json;

// A key or a value much bigger than this is a mistake, not data to keep.
constexpr size_t kMaxKey = 256;
constexpr size_t kMaxValue = 1 << 20;

std::mutex g_mutex;
std::map<std::string, json> g_mods;  // a mod's folder name -> its keys, as loaded

// A mod's folder name, as a file name: letters, digits, spaces, _, - and . (no path separators).
std::string ModName(const std::string &mod) {
  if (mod.empty() || mod.size() > 128 || mod == "." || mod == "..")
    throw Error("not a mod's name: '" + mod + "'");
  for (const char c : mod) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '_' && c != '-' && c != '.')
      throw Error("not a mod's name: '" + mod + "'");
  }
  return mod;
}

std::filesystem::path DataFile(const std::string &mod) {
  return renut::paths::WritableFolder() / "mod_data" / (mod + ".json");
}

// The mod's keys, read from its file the first time. Requires g_mutex.
json &Keys(const std::string &mod) {
  const auto found = g_mods.find(mod);
  if (found != g_mods.end())
    return found->second;
  json keys = json::object();
  std::ifstream in(DataFile(mod), std::ios::binary);
  if (in) {
    std::stringstream text;
    text << in.rdbuf();
    // A damaged file starts over rather than failing every call.
    json read = json::parse(text.str(), nullptr, false);
    if (read.is_object())
      keys = std::move(read);
  }
  return g_mods[mod] = std::move(keys);
}

// Writes the mod's keys, through a temporary file so a crash mid-write leaves the old one. Requires g_mutex.
void Write(const std::string &mod) {
  const std::filesystem::path file = DataFile(mod);
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path temporary = file.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out)
      throw Error("can't write " + file.string());
    out << g_mods[mod].dump(1, '\t');
  }
  std::filesystem::rename(temporary, file, ec);
  if (ec)
    throw Error("can't write " + file.string() + ": " + ec.message());
}

std::string Key(const std::string &key) {
  if (key.empty() || key.size() > kMaxKey)
    throw Error("a key is 1-" + std::to_string(kMaxKey) + " characters");
  return key;
}

// renut.data: values as Lua source (numbers, text, true/false and tables of them), read back with load.
constexpr const char kLua[] = R"lua(
local data = renut.data

-- The folder name of the mod whose script called (level 3: the caller of a renut.data function).
local function caller_mod()
  local source = debug.getinfo(3, "S").source or ""
  local mod = source:match("[/\\]([^/\\]+)[/\\]scripts[/\\][^/\\]+$")
  assert(mod, "renut.data works from a mod's scripts")
  return mod
end

local function encode(value, depth)
  local kind = type(value)
  if kind == "number" then
    if math.type(value) == "integer" then return string.format("%d", value) end
    if value ~= value then return "0/0" end
    if value == math.huge then return "1/0" end
    if value == -math.huge then return "-1/0" end
    return string.format("%.17g", value)
  elseif kind == "string" then
    return string.format("%q", value)
  elseif kind == "boolean" then
    return tostring(value)
  elseif kind == "table" then
    assert(depth < 32, "renut.data: tables nested too deep")
    local entries = {}
    for k, v in pairs(value) do
      entries[#entries + 1] = "[" .. encode(k, depth + 1) .. "]=" .. encode(v, depth + 1)
    end
    return "{" .. table.concat(entries, ",") .. "}"
  end
  error("renut.data keeps numbers, text, true/false and tables of them, not a " .. kind, depth + 3)
end

function data.get(key, default)
  local mod = caller_mod()
  if not data._has(mod, key) then return default end
  local chunk = load("return " .. data._get(mod, key), "=renut.data", "t", {})
  local ok, value = pcall(chunk)
  if ok then return value end
  return default
end

function data.set(key, value)
  local mod = caller_mod()
  if value == nil then data._erase(mod, key) else data._set(mod, key, encode(value, 0)) end
end

function data.keys()
  local mod = caller_mod()
  local keys = {}
  for key in data._keys(mod):gmatch("[^\n]+") do
    if key:sub(1, 5) ~= "save/" then keys[#keys + 1] = key end
  end
  return keys
end

-- The same, but kept for the save played (player 1's profile), under "save/<profile>/<key>".
local function save_key(key)
  assert(type(key) == "string", "renut.data: a key is text")
  return "save/" .. data._save_id() .. "/" .. key
end
function data.save_get(key, default)
  local mod, at = caller_mod(), save_key(key)
  if not data._has(mod, at) then return default end
  local ok, value = pcall(load("return " .. data._get(mod, at), "=renut.data", "t", {}))
  if ok then return value end
  return default
end
function data.save_set(key, value)
  local mod, at = caller_mod(), save_key(key)
  if value == nil then data._erase(mod, at) else data._set(mod, at, encode(value, 0)) end
end
function data.save_keys()
  local mod, prefix = caller_mod(), save_key("")
  local keys = {}
  for key in data._keys(mod):gmatch("[^\n]+") do
    if key:sub(1, #prefix) == prefix then keys[#keys + 1] = key:sub(#prefix + 1) end
  end
  return keys
end
)lua";

} // namespace

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("data.", "Data");  // the node editor's add menu
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"data._has", {"mod", "key"}},
           {"data._get", {"mod", "key"}},
           {"data._set", {"mod", "key", "value"}},
           {"data._erase", {"mod", "key"}},
           {"data._keys", {"mod"}},
           {"data._save_id", {}}})
    renut.Args(name, args);  // the node editor's pins

  // The save played: player 1's online id, else their gamertag (a save is a profile's).
  renut.Function("data._save_id", [] {
    if (const uint64_t xuid = renut::online::PlayerXuid()) {
      char id[24];
      std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(xuid));
      return std::string(id);
    }
    std::string name = renut::online::PlayerName();
    for (char &c : name)
      if (c == '/' || c == '\n')
        c = '_';
    if (name.empty())
      throw Error("no player profile is signed in to keep a save's data for");
    return name;
  });

  renut.Function("data._has", [](std::string mod, std::string key) {
    std::lock_guard lock(g_mutex);
    const json &keys = Keys(ModName(mod));
    return keys.contains(Key(key));
  });
  renut.Function("data._get", [](std::string mod, std::string key) {
    std::lock_guard lock(g_mutex);
    const json &keys = Keys(ModName(mod));
    const auto found = keys.find(Key(key));
    return found != keys.end() && found->is_string() ? found->get<std::string>() : std::string();
  });
  renut.Function("data._set", [](std::string mod, std::string key, std::string value) {
    if (value.size() > kMaxValue)
      throw Error("a value is at most " + std::to_string(kMaxValue) + " bytes");
    std::lock_guard lock(g_mutex);
    const std::string name = ModName(mod);
    json &keys = Keys(name);
    const std::string at = Key(key);
    if (keys.contains(at) && keys[at] == value)
      return;
    keys[at] = std::move(value);
    Write(name);
  });
  renut.Function("data._erase", [](std::string mod, std::string key) {
    std::lock_guard lock(g_mutex);
    const std::string name = ModName(mod);
    if (Keys(name).erase(Key(key)))
      Write(name);
  });
  renut.Function("data._keys", [](std::string mod) {
    std::lock_guard lock(g_mutex);
    std::string list;
    for (const auto &[key, value] : Keys(ModName(mod)).items())
      list += (list.empty() ? "" : "\n") + key;
    return list;
  });
  renut.Lua(kLua);
}

} // namespace renut::game::mod_data
