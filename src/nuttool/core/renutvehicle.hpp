// Vehicles for reNut mods: a blueprint (the game's saved vehicle) made from a vehicle file, for a
// mod's vehicles folder, which reNut adds to the player's saved vehicles once.
#pragma once

#include "caff.hpp"

namespace nut::renutvehicle {

// `vehicleJson`: {"name": "MECH", "parts": [{"part": "body_light_cube", "cell": [x, y, z],
// "rotation": [x, y, z] (degrees), "colour": "rrggbb", "button": "A" | "B" | "X"}, ...]}. A reNut
// part (renut.parts.add) names its shipped base too: {"part": "mymod_leg", "base": "gadgets_horn"}.
// The parts' records (block type, mass) come from `bundle`, the game's parts bundle.
Bytes blueprint(const caff::Caff& bundle, const std::string& vehicleJson);

} // namespace nut::renutvehicle
