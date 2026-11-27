// reNut vehicle parts for RexFabric Lua mods: a renut.parts.add{...} script (reNut's
// docs/tutorials/parts-and-vehicles.md) in a mod folder, made from a shipped part and its edits.
#pragma once

#include "gamescript.hpp"

namespace nut::renutpart {

struct Part {
    std::string id;          // 1-40 characters of a-z, 0-9 and _
    std::string base;        // the shipped part it starts from: wheels_monster
    std::string name;        // the garage name; empty keeps the base's (or the edited record's name)
    int count = 1;           // how many the player owns, 1-99
    Bytes original, edited;  // the base's record and an edited copy: what differs becomes fields
    bool behaviour = false;  // a new kind of part: every callback, ready to fill in
    std::string model;       // its own model (renutmodel.hpp builds it); empty keeps the base's
    std::string attach;      // its own footprint (renutmodel.hpp); empty keeps the base's
};

// "aid_objparams_banjox_vehicleblock_wheels_monster" or "wheels_monster" -> "wheels_monster".
std::string baseName(const std::string& part);
bool validId(const std::string& id);

// The part's script: its renut.parts.add call.
std::string lua(const Part& p, const gamescript::NameLookup& names = {});

// Writes <mod>/scripts/<id>.lua, and <mod>/mod.toml when the folder has none; the script's path.
// An existing script is only replaced with `overwrite`.
fs::path write(const fs::path& mod, const Part& p, const std::string& modName, const std::string& author, bool overwrite,
               const gamescript::NameLookup& names = {});

} // namespace nut::renutpart
