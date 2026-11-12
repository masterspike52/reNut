#pragma once

namespace renut::game::sound_api {

// Before rexfabric::Start: declares renut.sound for Lua mods, sound files of their own played in the world
// (stereo.h's PlayModSound).
void Declare();

} // namespace renut::game::sound_api
