#pragma once
// Custom vehicle parts from .nbpatch mods' parts/*.json entries (id, base, name, count,
// fields, objparams, script, params, model, attach), served by game/custom_parts.cpp.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "renut_engine/mods/json.h"

namespace renut::mods {

class ZipReader;

struct CustomPartField {
    uint32_t offset = 0;
    std::vector<uint8_t> bytes;  // big-endian, as written into the record
};

struct CustomPart {
    std::string id;
    std::string base;  // objparams name without "aid_objparams_"
    std::string name;
    uint32_t count = 1;
    std::vector<CustomPartField> fields;
    std::vector<uint8_t> objparams;  // a whole record, or empty
    std::string script;              // the script's path in the mod, or empty
    std::string script_source;       // its text
    JsonValue params;                // "params" (an object), or null
    std::string model;               // "banjox_vehicleparts_<model>", or empty: the base's
    std::string attach;              // "banjox_vehicleblock_<attach>", or empty: the base's
    std::string mod;                 // the mod's name
    std::string garage_type;         // a garage type it shares with the parts of that name, or empty
    bool own_record = false;         // a part of its own (renut.parts.add without a base): its record and garage items made from scratch
    bool lua = false;                // behaviour from a RexFabric Lua mod (renut.parts.add)
    bool base_action = false;        // its button still does the base part's action too
};

// A part with behaviour of its own (a script or Lua callbacks): a class of its own in the game.
inline bool HasBehaviour(const CustomPart &part) {
    return !part.script.empty() || part.lua;
}

// The part file rules, for parts made elsewhere (renut.parts.add): a valid id, and the
// base, model and attach names as the game's assets call them.
bool ValidPartId(const std::string &id);
std::string PartBaseName(std::string base);
std::string PartModelName(std::string model);
std::string PartAttachName(std::string attach);

// The parts in a package's parts/ folder; nullopt (with *error set) when one
// is malformed.
std::optional<std::vector<CustomPart>> ReadCustomParts(ZipReader &zip, std::string *error);

// Startup, before the game runs: adds a loaded mod's parts. A part whose id an
// earlier mod already has is skipped.
void AddCustomParts(std::vector<CustomPart> parts);

// Every loaded part; fixed once the game runs.
const std::vector<CustomPart> &CustomParts();
const CustomPart *FindCustomPart(const std::string &id);

// After the mods (.nbpatch and RexFabric) have added theirs, before the game runs:
// AddCustomParts adds nothing from then on.
void FixCustomParts();
bool CustomPartsFixed();

// A bundle a mod with parts adds: id 0x4Fxxxxxx (Bundle/4f/xxxxxx, resident)
// or 0x50xxxxxx (Bundle/50/xxxxxx, stream archive).
struct ModBundle {
    uint32_t id = 0;
    std::string mod;
};

// Startup, before the game runs.
void AddModBundle(ModBundle bundle);

// Every mod bundle, in load order; fixed once the game runs.
const std::vector<ModBundle> &ModBundles();

// A part some mod has had, remembered in <mods folder>/.renut_custom_parts.json so
// vehicles using it still load (as its base part) once its mod is gone.
struct KnownCustomPart {
    uint32_t aid = 0;
    uint32_t base_aid = 0;  // 0: made from a whole record, no base
    std::string id;
    std::string mod;
};

// Startup, after the mods are loaded (or not: renut_mods off): reads the
// remembered parts and adds this run's.
void RememberCustomParts(const std::filesystem::path &mods_folder);

// A remembered part, or nullptr; fixed once the game runs.
const KnownCustomPart *FindKnownCustomPart(uint32_t aid);
const std::vector<KnownCustomPart> &KnownCustomParts();
bool AnyKnownCustomParts();

// N&B's asset id: type << 24 | (CRC-32 without the final inversion) & 0xFFFFFF
// of the name without its "aid_<type>_" prefix.
uint32_t AssetId(uint8_t type, const std::string &name);

} // namespace renut::mods
