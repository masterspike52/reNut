// Map edits (docs/content_and_classes.md, Map edits): the worlds and the kinds of edit the game declares
// (rexfabric/content.h), the enabled mods' map edits as they were loaded, and their edits made each time the game
// enters a world. Leaving it takes them with it: the game destroys what was spawned in it along with its scene.
//
// The game says which world it's in (SetWorld). On RexFabric's tick, when that's a new visit or the mods' content was
// loaded again, what was made is undone and the map edits for the world now are made, every mod's in load order and
// each one's edits in order: a Spawn spawns its class's avatar where it says and sets the values it gives on that avatar
// alone; a game's kind is applied as the game declared it. Avatars spawned so are avatars like any other (Lua sees them,
// casts to their class work), and when one goes (a reload of the scripts removes every avatar) the world's edits are
// made again.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rexfabric/content.h>

#include "../mods.h"
#include "assets.h"
#include "types.h"

namespace rexfabric::content {

// A kind of map edit the game declared, its properties as a struct's fields are.
struct MapEditKind {
  std::string path;
  std::string help;
  bool transform = false;
  std::vector<Property> properties;
  std::function<uint64_t(std::string_view world, const Transform& at, const AvatarValues& values)> apply;
  std::function<void(uint64_t handle)> undo;
  std::function<bool(uint64_t handle, const Transform& at)> move;
};

std::shared_ptr<const MapEditKind> FindMapEditKind(std::string_view path);
// The game's kinds' paths, as declared.
std::vector<std::string> MapEditKindPaths();

// Every world: the game's, as declared, then the loaded mods' (docs/content_and_classes.md, Worlds).
std::vector<WorldDecl> Worlds();
bool HasWorld(std::string_view path);
// A loaded mod's world: the world it's built on, a game's or another mod's; empty for a world of the game's or none.
std::string WorldBase(std::string_view path);
// The world the game says it's in now, empty for none.
std::string CurrentWorld();

// Reads the worlds and the map edits of the enabled mods, in load order, after their classes are loaded; the world's
// edits are made again on the next tick.
void LoadMapEdits(const std::vector<ModInfo>& mods);

// An editor's unsaved map edit, shown in the world in place of the loaded one with that path until EndPreview: what
// it changes is changed to fit on the next tick (a spawn moved, its values set), the rest made again. Only a map edit
// that's loaded (its mod enabled) is previewed. Any thread.
void PreviewMapEdit(std::string_view path, const MapEditAsset& asset);
void EndPreview(std::string_view path);

// What went wrong the last time an edit was made in the world (a spawn the class refused, a game kind that found
// nothing to change), empty when it was made. Any thread.
std::string MapEditProblem(std::string_view path, std::string_view edit);

// Makes and undoes the edits as the world and the content say. On the thread RexFabric ticks on, before the avatars
// tick.
void TickMapEdits();

}  // namespace rexfabric::content
