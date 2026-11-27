// Character packs for reNut: what each game character needs beyond its own bundles (its model, textures and the
// rest that only world bundles hold), copied into a small stream bundle reNut loads to spawn it in any world.
#pragma once

#include "common.hpp"

#include <functional>

namespace nut::renutchars {

using Say = std::function<void(const std::string&)>;

// Writes <mods>/.characters/Bundle/50/<id of renutchar_<character>> for every character (objparams with a model)
// in the game's actor bundles under `assets`, or those named in `only`. Returns how many were written.
int build(const fs::path& assets, const fs::path& mods, const std::vector<std::string>& only, const Say& say);

} // namespace nut::renutchars
