// Lua mods: each enabled mod's scripts folder, run in one shared VM.
//
//   mods/MyMod/scripts/main.lua
//   mods/MyMod/scripts/lib/util.lua   -- require("lib.util")
//
// Every .lua directly in scripts runs at load, by file name. Mods load in load order, so a later mod's hooks
// run after an earlier one's.

#pragma once

#include <string>
#include <vector>

#include "../mods.h"

namespace rexfabric::lua {

struct ScriptMod {
  std::string name;  // the mod's folder
  std::vector<std::string> files;
  std::string error;  // the first failure, if one failed
};

void Start(const std::vector<ModInfo>& mods);
// Drops every hook, claim, switch and listener, and runs the mods again in a new VM.
void Reload(const std::vector<ModInfo>& mods);
void Stop();
bool Running();
const std::vector<ScriptMod>& Loaded();

}  // namespace rexfabric::lua
