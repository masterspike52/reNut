// The mods' part of a world's save (include/rexfabric/content.h, WorldSaveData; docs/content_and_classes.md, Saved with
// the world): the saved avatars as records the game keeps in its own save, spawned again when the world loads, and each
// mod's own data (rex.world_data), by its folder, and the game's own parts (DeclareWorldSavePart), by their names.
//
//   {"format": 1, "avatars": [{"class": "MyMod/Statue", "at": {"position": [40, 0, -89]}, "values": {"colour": "Red"}}],
//    "mods": {"MyMod": {"visits": 3}}, "game": {"packets": {...}}}

#include "world_saves.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <rexfabric/content.h>

#include "../log.h"
#include "avatars.h"
#include "classes.h"
#include "components.h"

namespace rexfabric::content {
namespace {

constexpr int kFormat = 1;

// The records the last restore couldn't spawn, written back into every save of the world until the next restore; the
// mods' data as the world loaded with it, while there's no source to hand it to; the source; the game's parts, and
// the parts the last restore had that no declaration names, kept for the next save.
struct Kept {
  std::mutex mutex;
  nlohmann::json records = nlohmann::json::array();
  nlohmann::json mods = nlohmann::json::object();
  ModDataSource source;
  std::vector<WorldSavePartDecl> parts;
  nlohmann::json unknown_parts = nlohmann::json::object();
};

Kept& GetKept() {
  static auto* kept = new Kept();
  return *kept;
}

nlohmann::json RecordOf(const SavedAvatar& avatar) {
  nlohmann::json record = {{"class", avatar.path}, {"at", WriteTransform(avatar.at)}};
  if (!avatar.values.empty()) record["values"] = avatar.values;
  return record;
}

// Spawns one record's avatar, saved, with the values it kept; false with `why` when its class can't be spawned now.
bool Spawn(const nlohmann::json& record, std::string* why) {
  const std::string path = record.value("class", std::string());
  if (!FindClass(path) || BackingOf(path).empty()) {
    *why = "there's no avatar class " + path + " loaded";
    return false;
  }
  Transform at;
  if (record.contains("at") && !ReadTransform(record["at"], at, why)) return false;
  const uint32_t id = SpawnAvatar(path, at, why);
  if (id == 0) return false;
  SetAvatarSaved(id, true);
  if (const auto values = record.find("values"); values != record.end() && values->is_object()) {
    for (const auto& [name, value] : values->items()) {
      std::string refused;
      if (!SetAvatarValue(id, name, value, &refused)) RF_WARN("content: saved avatar of {}: its {} is dropped: {}", path, name, refused);
    }
  }
  return true;
}

// The game's parts as a save holds them: each part's JSON by its name, the ones no declaration names kept as they came.
nlohmann::json SaveParts(const std::vector<WorldSavePartDecl>& parts, nlohmann::json kept) {
  for (const WorldSavePartDecl& part : parts) {
    const std::string text = part.save();
    if (text.empty()) continue;
    nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) RF_WARN("content: the game's world save part {} isn't JSON; it isn't saved", part.name);
    else kept[part.name] = std::move(json);
  }
  return kept;
}

// Hands each declared part what the save had of it, empty for nothing; gives the rest, to keep.
nlohmann::json RestoreParts(const std::vector<WorldSavePartDecl>& parts, const nlohmann::json& saved) {
  nlohmann::json unknown = saved.is_object() ? saved : nlohmann::json::object();
  for (const WorldSavePartDecl& part : parts) {
    const auto found = unknown.find(part.name);
    const std::string text = found != unknown.end() ? found->dump() : std::string();
    if (found != unknown.end()) unknown.erase(found);
    part.restore(text);
  }
  return unknown;
}

}  // namespace

bool DeclareWorldSavePart(WorldSavePartDecl decl) {
  if (decl.name.empty() || !decl.save || !decl.restore) {
    RF_ERROR("content: a world save part needs a name, a save and a restore");
    return false;
  }
  Kept& kept = GetKept();
  std::lock_guard lock(kept.mutex);
  if (std::any_of(kept.parts.begin(), kept.parts.end(), [&decl](const WorldSavePartDecl& part) { return part.name == decl.name; })) {
    RF_ERROR("content: the world save part {} is declared twice", decl.name);
    return false;
  }
  kept.parts.push_back(std::move(decl));
  return true;
}

void SetModDataSource(ModDataSource source) {
  Kept& kept = GetKept();
  std::lock_guard lock(kept.mutex);
  kept.source = std::move(source);
}

std::string WorldSaveData() {
  nlohmann::json records = nlohmann::json::array();
  for (const SavedAvatar& avatar : SavedAvatars()) records.push_back(RecordOf(avatar));
  nlohmann::json mods;
  ModDataSource source;
  std::vector<WorldSavePartDecl> parts;
  nlohmann::json unknown_parts;
  {
    Kept& kept = GetKept();
    std::lock_guard lock(kept.mutex);
    for (const nlohmann::json& record : kept.records) records.push_back(record);
    mods = kept.mods;
    source = kept.source;
    parts = kept.parts;
    unknown_parts = kept.unknown_parts;
  }
  // Outside the lock: the source reads the Lua VM under its own, and the parts read the game.
  if (source.save) mods = source.save();
  nlohmann::json game = SaveParts(parts, std::move(unknown_parts));
  // Nothing to keep: the game's save stays as the game alone would write it.
  const bool has_mods = mods.is_object() && !mods.empty();
  if (records.empty() && !has_mods && game.empty()) return {};
  nlohmann::json save = {{"format", kFormat}, {"avatars", std::move(records)}};
  if (has_mods) save["mods"] = std::move(mods);
  if (!game.empty()) save["game"] = std::move(game);
  return save.dump();
}

size_t RestoreWorldSave(std::string_view data) {
  nlohmann::json kept = nlohmann::json::array();
  nlohmann::json mods = nlohmann::json::object();
  nlohmann::json game = nlohmann::json::object();
  size_t spawned = 0;
  if (!data.empty()) {
    const nlohmann::json save = nlohmann::json::parse(data, nullptr, false);
    if (save.is_object() && save.contains("mods") && save["mods"].is_object()) mods = save["mods"];
    if (save.is_object() && save.contains("game") && save["game"].is_object()) game = save["game"];
    const auto avatars = save.is_object() ? save.find("avatars") : save.end();
    if (save.is_discarded() || avatars == save.end() || !avatars->is_array()) {
      RF_WARN("content: the mods' part of the world's save can't be read, so nothing of it is spawned or kept");
    } else {
      for (const nlohmann::json& record : *avatars) {
        if (!record.is_object() || !record.value("class", nlohmann::json()).is_string()) {
          RF_WARN("content: a record in the world's save isn't an avatar's; it's dropped");
          continue;
        }
        std::string why;
        if (Spawn(record, &why)) {
          ++spawned;
          continue;
        }
        RF_INFO("content: saved avatar of {} kept for the next save, not spawned: {}", record.value("class", std::string()), why);
        kept.push_back(record);
      }
      RF_INFO("content: {} saved avatars back in the world, {} kept for mods that aren't loaded", spawned, kept.size());
    }
  }
  ModDataSource source;
  std::vector<WorldSavePartDecl> parts;
  {
    Kept& held = GetKept();
    std::lock_guard lock(held.mutex);
    held.records = std::move(kept);
    held.mods = mods;
    source = held.source;
    parts = held.parts;
  }
  if (!mods.empty()) RF_INFO("content: world data back for {} mods", mods.size());
  if (source.restore) source.restore(mods);
  nlohmann::json unknown = RestoreParts(parts, game);
  {
    Kept& held = GetKept();
    std::lock_guard lock(held.mutex);
    held.unknown_parts = std::move(unknown);
  }
  return spawned;
}

}  // namespace rexfabric::content
