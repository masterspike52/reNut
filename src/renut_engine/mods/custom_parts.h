#pragma once
// Custom vehicle parts from .nbpatch mods: parts/<anything>.json entries, one
// part each, read at startup (mod_loader.cpp) and served to the game by
// game/custom_parts.cpp.
//
//   {
//     "id": "turbo_engine",                 // [a-z0-9_], unique across mods
//     "base": "propulsion_engines_large",   // shipped part to start from
//     "name": "Turbo Engine",               // shown in the garage
//     "count": 4,                           // how many the player owns
//     "fields": { "0x3FC": { "f32": 120 } },// objparams overrides (big-endian)
//     "objparams": "parts/turbo.bin"        // optional: a whole record instead
//     "script": "parts/teleporter.lua",     // optional: a new kind of part
//     "params": { "distance": 15 },         // the script's settings (its params)
//     "model": "teleporter",                // optional: the part's model
//     "attach": "teleporter"                // optional: its footprint / attach points
//   }
//
// A part with a script is a class of its own, renut_<id>, deriving from its
// base part's class (game/part_classes.h): the script runs its behaviour.
//
// "model" names a part model, aid_model_banjox_vehicleparts_<model> (its
// collision is aid_havok_banjox_vehicleparts_<model>, found by the game from
// the model's name); "attach" an aid_avatarhavokdata_banjox_vehicleblock_
// <attach> asset. Either is a shipped part's (e.g. "gadgets_spring") or one
// the mod brings in a bundle of its own: every bundle file a mod with parts
// adds (Bundle/4f/<id> resident, Bundle/50/<id> stream archive;
// tools/renut_modtool builds them) is loaded with the game's common bundle
// (game/mod_bundles.cpp). One that isn't there keeps the base part's.
//
// "base" is the part's objparams name, with or without
// "aid_objparams_banjox_vehicleblock_". Field values are one of f32, u32, i32,
// u16, u8 (numbers) or str (a string, written with its terminator). The part's
// own asset is aid_objparams_banjox_vehicleblock_renut_<id>.
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
    std::string mod;                 // the mod's name, for logs
};

// The parts in a package's parts/ folder; nullopt (with *error set) when one
// is malformed.
std::optional<std::vector<CustomPart>> ReadCustomParts(ZipReader &zip, std::string *error);

// Startup, before the game runs: adds a loaded mod's parts. A part whose id an
// earlier mod already has is skipped (logged).
void AddCustomParts(std::vector<CustomPart> parts);

// Every loaded part; fixed once the game runs.
const std::vector<CustomPart> &CustomParts();

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

// A part some mod has had, remembered in <mods folder>/.renut_custom_parts.json
// so that vehicles keeping it still load once its mod is gone: the game is
// given its base part instead (no base: the part is left out).
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
