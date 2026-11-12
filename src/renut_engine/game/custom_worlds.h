#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace renut::game::custom_worlds {

// Before rexfabric::Start: declares renut.worlds and renut.doors for RexFabric mods.
// docs/lua-mods/lua-worlds.md describes them.
void Declare();

// The music a mod's world gave (renut.worlds.add's music) for scene script `scene`, and its volume.
bool WorldMusic(uint32_t scene, std::filesystem::path *file, float *volume);

// The scene script's asset id of the mod world `id` (renut.worlds.add), or 0 for none.
uint32_t WorldScene(const std::string &id);

// A player model's asset id (aid_model_renut_player_<model>, NutTool's renut-player-model), or 0 for "".
uint32_t PlayerModelId(const std::string &model);
// The player model the mod world with scene script `scene` makes Banjo with (its player_model), or 0.
uint32_t WorldPlayerModel(uint32_t scene);

} // namespace renut::game::custom_worlds
