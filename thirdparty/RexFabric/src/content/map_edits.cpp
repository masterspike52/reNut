#include "map_edits.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <map>
#include <mutex>

#include "../log.h"
#include "assets.h"
#include "avatars.h"

namespace rexfabric::content {
namespace {

using Clock = std::chrono::steady_clock;

// After the avatars an apply made went by themselves, the soonest it's tried again: a world that takes them away as fast
// as they're made isn't asked every tick.
constexpr auto kRetryAfter = std::chrono::seconds(1);

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

bool ValidPath(std::string_view path, std::string* why) {
  size_t start = 0;
  for (size_t slash = 0; slash != std::string_view::npos; start = slash + 1) {
    slash = path.find('/', start);
    if (!ValidName(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start), why)) return false;
  }
  return true;
}

// How many worlds deep a mod's world may be built on others: past this a chain is taken for a loop.
constexpr size_t kDeepestWorld = 8;

// A loaded mod's world: the world it's built on.
struct ModWorld {
  std::string mod;
  std::string path;
  std::string base;
  std::string help;
};

// A loaded mod's map edit.
struct Loaded {
  std::string mod;
  std::string path;
  MapEditAsset asset;
};

// What an apply made, to undo it: a spawned avatar, or a game kind's handle; with the edit as it was made, by the map
// edit it's in, so a preview changes what was made rather than making it again.
struct Made {
  uint32_t avatar = 0;
  std::shared_ptr<const MapEditKind> kind;
  uint64_t handle = 0;
  std::string path;
  MapEditEntry entry;
};

struct Registry {
  std::mutex mutex;
  std::vector<WorldDecl> worlds;  // the game's
  std::vector<ModWorld> mod_worlds;
  std::vector<std::shared_ptr<const MapEditKind>> kinds;
  std::vector<Loaded> loaded;
  bool reload = false;   // the content was loaded again since the last apply
  // The editors' unsaved map edits, by their paths in lower case, shown in place of the loaded ones.
  std::map<std::string, MapEditAsset> previews;
  // What went wrong the last time each edit was made, by its map edit's path and its name, in lower case.
  std::map<std::string, std::string> problems;
  bool previewed = false;  // a preview changed since the last apply
  // As the game says.
  std::string world;
  uint64_t visit = 0;  // goes up with each new visit
  // As applied, on the ticking thread.
  std::string applied_world;
  uint64_t applied_visit = 0;
  std::vector<Made> made;
  Clock::time_point retry_at;
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

// Undoes what an apply made while its world is still there: the avatars removed and the kinds' edits undone. A world
// left takes its own with it: the game destroys the avatars with their scene, and removing them as well would free them
// twice, so they're left to go as any avatar whose world went does (TickAvatars).
void UndoOne(const Made& made) {
  if (made.avatar != 0) {
    RemoveAvatar(made.avatar);
  } else if (made.kind && made.kind->undo) {
    try {
      made.kind->undo(made.handle);
    } catch (const std::exception& error) {
      RF_WARN("content: undoing a {} edit: {}", made.kind->path, error.what());
    }
  }
}

void Undo(std::vector<Made>& made, bool world_there) {
  if (world_there) {
    for (auto it = made.rbegin(); it != made.rend(); ++it) UndoOne(*it);
  }
  made.clear();
}

std::string ProblemKey(std::string_view path, std::string_view edit) { return Lower(path) + "\n" + Lower(edit); }

// What went wrong making an edit: logged, and kept for its editor to show.
void Problem(const Loaded& from, const MapEditEntry& edit, const std::string& why) {
  RF_WARN("content: [{}] {}: edit {}: {}", from.mod, from.path, edit.name, why);
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::string& kept = registry.problems[ProblemKey(from.path, edit.name)];
  kept += (kept.empty() ? "" : "; ") + why;
}

// A spawn: its class's avatar where the edit says, then the values it gives set on it.
void Spawn(const Loaded& from, const MapEditEntry& edit, std::vector<Made>& made) {
  std::string why;
  const uint32_t avatar = SpawnAvatar(edit.spawn, edit.transform, &why);
  if (avatar == 0) {
    Problem(from, edit, why);
    return;
  }
  made.push_back({.avatar = avatar, .path = from.path, .entry = edit});
  for (const auto& [name, value] : edit.values.items()) {
    if (!SetAvatarValue(avatar, name, value, &why)) Problem(from, edit, why);
  }
}

// A game kind's edit, with its values as the kind's properties have them.
void ApplyKind(const Loaded& from, const MapEditEntry& edit, std::string_view world, std::vector<Made>& made) {
  const std::shared_ptr<const MapEditKind> kind = FindMapEditKind(edit.kind);
  if (!kind) {
    Problem(from, edit, "the game has no kind of edit " + edit.kind);
    return;
  }
  AvatarValues values;
  for (const Property& property : kind->properties) {
    const nlohmann::json* set = nullptr;
    for (const auto& [name, value] : edit.values.items()) {
      if (Same(name, property.name)) set = &value;
    }
    values[property.name] = ToPropertyValue(set ? *set : property.value);
  }
  try {
    if (const uint64_t handle = kind->apply(world, edit.transform, values); handle != 0) made.push_back({.kind = kind, .handle = handle, .path = from.path, .entry = edit});
  } catch (const std::exception& error) {
    Problem(from, edit, error.what());
  }
}

void Make(const Loaded& from, const MapEditEntry& edit, std::string_view world, std::vector<Made>& made) {
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    registry.problems.erase(ProblemKey(from.path, edit.name));
  }
  if (edit.kind == kSpawnEdit) Spawn(from, edit, made);
  else ApplyKind(from, edit, world, made);
}

// Brings what was made as `made.entry` to `edit` where it can without making it again: a spawn moved and its values
// set, a game kind's edit moved when only its place changed and the kind can move it. False when it must be made
// again: another kind or class, a value a spawn no longer sets (its class's is wanted back), or what won't take the
// change.
bool Update(Made& made, const MapEditEntry& edit) {
  if (made.entry == edit) return true;
  if (made.kind && made.kind->move && edit.kind == made.entry.kind) {
    MapEditEntry moved = made.entry;
    moved.transform = edit.transform;
    if (!(moved == edit)) return false;
    try {
      if (!made.kind->move(made.handle, edit.transform)) return false;
    } catch (const std::exception& error) {
      RF_WARN("content: moving a {} edit: {}", made.kind->path, error.what());
      return false;
    }
    made.entry = edit;
    return true;
  }
  if (made.avatar == 0 || edit.kind != kSpawnEdit || made.entry.kind != kSpawnEdit || !Same(made.entry.spawn, edit.spawn)) return false;
  for (const auto& [name, value] : made.entry.values.items()) {
    if (!edit.values.contains(name)) return false;
  }
  std::string why;
  if (!(made.entry.transform == edit.transform) && !MoveAvatar(made.avatar, edit.transform, &why)) return false;
  for (const auto& [name, value] : edit.values.items()) {
    const auto old = made.entry.values.find(name);
    if (old != made.entry.values.end() && *old == value) continue;
    if (!SetAvatarValue(made.avatar, name, value, &why)) return false;
  }
  made.entry = edit;
  return true;
}

// The world and the worlds it's built on, nearest first: a mod's world, its base, and so on down to the game's.
std::vector<std::string> WorldChain(const Registry& registry, std::string_view world) {
  std::vector<std::string> chain;
  std::string at(world);
  while (!at.empty() && chain.size() < kDeepestWorld && std::none_of(chain.begin(), chain.end(), [&at](const std::string& seen) { return Same(seen, at); })) {
    chain.push_back(at);
    const auto found = std::find_if(registry.mod_worlds.begin(), registry.mod_worlds.end(), [&at](const ModWorld& known) { return Same(known.path, at); });
    at = found != registry.mod_worlds.end() ? found->base : std::string();
  }
  return chain;
}

// Whether a map edit of `edited` is made in the world whose chain this is: its own, or one it's built on.
bool InChain(const std::vector<std::string>& chain, std::string_view edited) {
  return std::any_of(chain.begin(), chain.end(), [edited](const std::string& world) { return Same(world, edited); });
}

// The world's edits as `loaded` has them now, from what was made: each kept, changed or made again by its map edit and
// name, those no longer there undone, the new ones made.
void Sync(const std::vector<Loaded>& loaded, std::string_view world, const std::vector<std::string>& chain, std::vector<Made>& made) {
  std::vector<Made> kept;
  for (const Loaded& from : loaded) {
    if (!InChain(chain, from.asset.world)) continue;
    for (const MapEditEntry& edit : from.asset.edits) {
      const auto old = std::find_if(made.begin(), made.end(), [&](const Made& m) { return Same(m.path, from.path) && Same(m.entry.name, edit.name); });
      if (old != made.end()) {
        Made moved = std::move(*old);
        made.erase(old);
        if (Update(moved, edit)) {
          kept.push_back(std::move(moved));
          continue;
        }
        UndoOne(moved);
      }
      Make(from, edit, world, kept);
    }
  }
  Undo(made, true);
  made = std::move(kept);
}

// The loaded map edits, each previewed one as its editor has it.
std::vector<Loaded> Effective(const Registry& registry) {
  std::vector<Loaded> effective = registry.loaded;
  for (Loaded& from : effective) {
    if (const auto preview = registry.previews.find(Lower(from.path)); preview != registry.previews.end()) from.asset = preview->second;
  }
  return effective;
}

}  // namespace

bool DeclareWorld(WorldDecl decl) {
  std::string why;
  if (!ValidPath(decl.path, &why)) {
    RF_ERROR("content: the game's world {}: {}", decl.path, why);
    return false;
  }
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (std::any_of(registry.worlds.begin(), registry.worlds.end(), [&decl](const WorldDecl& known) { return Same(known.path, decl.path); })) {
    RF_ERROR("content: the game's world {} is declared twice", decl.path);
    return false;
  }
  registry.worlds.push_back(std::move(decl));
  return true;
}

bool DeclareMapEditKind(MapEditKindDecl decl) {
  std::string why;
  if (!ValidPath(decl.path, &why) || Same(decl.path, kSpawnEdit)) {
    RF_ERROR("content: the game's kind of map edit {}: {}", decl.path, why.empty() ? std::string("Spawn is RexFabric's own") : why);
    return false;
  }
  if (!decl.apply) {
    RF_ERROR("content: the game's kind of map edit {} has no apply", decl.path);
    return false;
  }
  auto kind = std::make_shared<MapEditKind>(MapEditKind{.path = std::move(decl.path), .help = std::move(decl.help), .transform = decl.transform, .apply = std::move(decl.apply), .undo = std::move(decl.undo), .move = std::move(decl.move)});
  for (PropertyDecl& property : decl.properties) kind->properties.push_back({.name = std::move(property.name), .type = std::move(property.type), .value = ToJson(property.value), .help = std::move(property.help)});
  const auto anything = [](std::string_view, std::string*) { return true; };
  if (!CheckDeclared(kind->properties, LoadedTypes(), anything, &why)) {
    RF_ERROR("content: the game's kind of map edit {}: {}", kind->path, why);
    return false;
  }
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (std::any_of(registry.kinds.begin(), registry.kinds.end(), [&kind](const auto& known) { return Same(known->path, kind->path); })) {
    RF_ERROR("content: the game's kind of map edit {} is declared twice", kind->path);
    return false;
  }
  registry.kinds.push_back(std::move(kind));
  return true;
}

void SetWorld(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (registry.world == path) return;
  registry.world = std::string(path);
  ++registry.visit;
}

std::shared_ptr<const MapEditKind> FindMapEditKind(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto found = std::find_if(registry.kinds.begin(), registry.kinds.end(), [path](const auto& known) { return Same(known->path, path); });
  return found != registry.kinds.end() ? *found : nullptr;
}

std::vector<std::string> MapEditKindPaths() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> paths;
  for (const auto& kind : registry.kinds) paths.push_back(kind->path);
  return paths;
}

std::vector<WorldDecl> Worlds() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::vector<WorldDecl> worlds = registry.worlds;
  for (const ModWorld& world : registry.mod_worlds) worlds.push_back({.path = world.path, .help = world.help.empty() ? "A world built on " + world.base : world.help});
  return worlds;
}

bool HasWorld(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  return std::any_of(registry.worlds.begin(), registry.worlds.end(), [path](const WorldDecl& known) { return Same(known.path, path); }) || std::any_of(registry.mod_worlds.begin(), registry.mod_worlds.end(), [path](const ModWorld& known) { return Same(known.path, path); });
}

std::string WorldBase(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto found = std::find_if(registry.mod_worlds.begin(), registry.mod_worlds.end(), [path](const ModWorld& known) { return Same(known.path, path); });
  return found != registry.mod_worlds.end() ? found->base : std::string();
}

std::string GameWorldOf(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const std::vector<std::string> chain = WorldChain(registry, path);
  if (chain.empty()) return {};
  const std::string& last = chain.back();
  const bool game = std::any_of(registry.worlds.begin(), registry.worlds.end(), [&last](const WorldDecl& known) { return Same(known.path, last); });
  return game ? last : std::string();
}

std::string CurrentWorld() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  return registry.world;
}

void LoadMapEdits(const std::vector<ModInfo>& mods) {
  // The worlds first, so a map edit of one is known.
  std::vector<ModWorld> worlds;
  for (const ModInfo& mod : mods) {
    for (const AssetInfo& info : ScanContent(mod).assets) {
      if (info.kind != AssetKind::kWorld) continue;
      std::string error;
      std::optional<WorldAsset> asset = ReadWorldAsset(info.file, &error);
      if (!asset) {
        RF_WARN("content: [{}] {}: {}", mod.folder, info.path, error);
        continue;
      }
      worlds.push_back({.mod = mod.folder, .path = info.path, .base = asset->base, .help = asset->help});
    }
  }
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    registry.mod_worlds = std::move(worlds);
    for (const ModWorld& world : registry.mod_worlds) {
      const std::vector<std::string> chain = WorldChain(registry, world.path);
      const bool reaches_game = !chain.empty() && std::any_of(registry.worlds.begin(), registry.worlds.end(), [&chain](const WorldDecl& known) { return Same(known.path, chain.back()); });
      if (!reaches_game) RF_WARN("content: [{}] {}: it isn't built on one of the game's worlds (its base is \"{}\")", world.mod, world.path, world.base);
    }
  }
  std::vector<Loaded> loaded;
  for (const ModInfo& mod : mods) {
    for (const AssetInfo& info : ScanContent(mod).assets) {
      if (info.kind != AssetKind::kMapEdit) continue;
      std::string error;
      std::optional<MapEditAsset> asset = ReadMapEditAsset(info.file, &error);
      if (!asset) {
        RF_WARN("content: [{}] {}: {}", mod.folder, info.path, error);
        continue;
      }
      if (!HasWorld(asset->world)) RF_WARN("content: [{}] {}: the game has no world \"{}\"", mod.folder, info.path, asset->world);
      loaded.push_back({mod.folder, info.path, std::move(*asset)});
    }
  }
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  RF_INFO("content: {} map edits", loaded.size());
  registry.loaded = std::move(loaded);
  registry.reload = true;
}

void PreviewMapEdit(std::string_view path, const MapEditAsset& asset) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  MapEditAsset& preview = registry.previews[Lower(path)];
  if (preview == asset) return;
  preview = asset;
  registry.previewed = true;
}

std::string MapEditProblem(std::string_view path, std::string_view edit) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto found = registry.problems.find(ProblemKey(path, edit));
  return found != registry.problems.end() ? found->second : std::string();
}

void EndPreview(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (registry.previews.erase(Lower(path)) != 0) registry.previewed = true;
}

void TickMapEdits() {
  Registry& registry = Get();
  std::string world;
  std::vector<std::string> chain;  // the world and those it's built on, whose map edits it takes
  uint64_t visit = 0;
  bool again = false;  // everything made again, not changed to fit
  std::vector<Loaded> loaded;
  std::vector<Made> made;
  {
    std::lock_guard lock(registry.mutex);
    world = registry.world;
    chain = WorldChain(registry, world);
    visit = registry.visit;
    const bool new_visit = visit != registry.applied_visit;
    // What was made went with its world: the game left it and hasn't said so yet (a load that pops the scene within one
    // of its ticks). Nothing's made again until the game names the world it's in next.
    const bool left = std::any_of(registry.made.begin(), registry.made.end(), [](const Made& made) { return made.avatar != 0 && WentWithItsWorld(made.avatar); });
    if (left && !new_visit) {
      registry.made.clear();
      return;
    }
    // What was made went by itself (a reload of the scripts): made again, not too often.
    const bool gone = std::any_of(registry.made.begin(), registry.made.end(), [](const Made& made) { return made.avatar != 0 && !FindAvatar(made.avatar); });
    const bool retry = gone && Clock::now() >= registry.retry_at;
    if (!new_visit && !registry.reload && !retry && !registry.previewed) return;
    // Only previews changed: what was made is changed to fit rather than made again.
    again = new_visit || registry.reload || retry;
    registry.reload = false;
    registry.previewed = false;
    loaded = Effective(registry);
    made = std::move(registry.made);
  }
  // Undone only when it's the same visit (the content loaded again); a world left takes its own with it.
  const bool same_visit = visit == registry.applied_visit && Same(world, registry.applied_world);
  if (!again && same_visit && !world.empty()) {
    Sync(loaded, world, chain, made);
  } else {
    Undo(made, same_visit);
    {
      std::lock_guard lock(registry.mutex);
      registry.problems.clear();
    }
    registry.applied_world = world;
    registry.applied_visit = visit;
    registry.retry_at = Clock::now() + kRetryAfter;
    if (!world.empty()) {
      // The base's edits before the world's own, so a world's edit can change what its base made.
      for (auto at = chain.rbegin(); at != chain.rend(); ++at) {
        for (const Loaded& from : loaded) {
          if (!Same(from.asset.world, *at)) continue;
          for (const MapEditEntry& edit : from.asset.edits) Make(from, edit, world, made);
        }
      }
    }
  }
  std::lock_guard lock(registry.mutex);
  registry.made = std::move(made);
}

}  // namespace rexfabric::content
