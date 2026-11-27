// The mods' part of a world's save (include/rexfabric/content.h, WorldSaveData): what's kept beside the saved avatars.

#pragma once

#include <functional>

#include <nlohmann/json.hpp>

namespace rexfabric::content {

// The mods' own data in the world's save, an object of each mod's by its folder: what the Lua VM's rex.world_data
// holds (lua::SaveWorldData) and takes back as a world loads. Without a source the data a world loaded with is kept as
// it was and saved again.
struct ModDataSource {
  std::function<nlohmann::json()> save;
  std::function<void(const nlohmann::json& mods)> restore;
};

// Set once, before the mods start.
void SetModDataSource(ModDataSource source);

}  // namespace rexfabric::content
