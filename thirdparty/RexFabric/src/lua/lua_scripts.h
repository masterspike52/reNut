// Lua mods: each enabled mod's scripts folder, run in one shared VM.
//
//   mods/MyMod/scripts/main.lua
//   mods/MyMod/scripts/lib/util.lua   -- require("lib.util")
//
// Every .lua directly in scripts runs at load, by file name. Mods load in load order, so a later mod's hooks
// run after an earlier one's.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

// The console (lua_console.cpp): a line queued to run in the VM on the next frame, and what each line gave back, its
// results joined or its error. Any thread.
struct ConsoleReply {
  uint64_t id = 0;
  std::string text;
  bool error = false;
};
uint64_t QueueConsole(std::string code);
std::vector<ConsoleReply> TakeConsoleReplies();
// Runs the queued lines. Each frame, on the frame thread, before the frame event.
void RunConsole();

// The mods' world data (lua_world_data.cpp, rex.world_data) for the world's save: an object of each mod's data by its
// folder, the mods without a table now included as they were loaded. Restoring replaces it all, filling the tables the
// mods have in place; an empty object empties them. Both on the thread the world is saved and loaded on.
nlohmann::json SaveWorldData();
void RestoreWorldData(const nlohmann::json& mods);

}  // namespace rexfabric::lua
