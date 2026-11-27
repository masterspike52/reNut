// Definitions for Lua editors (LuaLS, the Lua extension of VS Code and others): what a mod can call, written into the
// mods folder at every start, so an editor opened on that folder completes and checks rex.*, the game's namespaces and
// the names of the game's functions.
//
//   mods/.api/rex.lua        RexFabric's own API
//   mods/.api/<ns>.lua       each namespace the game declared: functions, types, enums, constants, events
//   mods/.api/functions.lua  rex.FunctionName, every guest function the game named, for rex.hook and rex.call
//   mods/.api/reference.md   the same for reading
//   mods/.luarc.json         points the editor at .api; written only when there is none, so it can be edited
//
// The folder starts with a dot, so the mod list never takes it for a mod.

#pragma once

#include <filesystem>

namespace rexfabric::lua {

void WriteDefinitions(const std::filesystem::path& mods_dir);

}  // namespace rexfabric::lua
