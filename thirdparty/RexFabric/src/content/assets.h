// Mods' content as files, for the content browser and its editors (docs/content_and_classes.md). The registries
// (classes.h, types.h) hold what loaded; this sees every mod's content folder as it is on disk, a mod that's off
// included, and changes it: new folders, classes, enums, structs and tables, renames and moves that take references
// along, duplicates and deletes.
//
// Every path is a full one, as the browser shows it: "MyMod" is the mod's content folder, "MyMod/Garden" a folder in
// it, "MyMod/Garden/Rock" the asset mods/MyMod/content/Garden/Rock.json. Files beside an asset named after it
// ("Rock.graph.json") are its own and go wherever it goes.

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "../mods.h"
#include "classes.h"
#include "components.h"
#include "localisation.h"
#include "values.h"

namespace rexfabric::content {

// The folder of a mod the browser opens as the mod, and the extension of every asset file.
inline constexpr std::string_view kContentFolder = "content";
inline constexpr std::string_view kAssetExtension = ".json";

// What an asset file holds, by its "asset". Files of kinds RexFabric doesn't know are kOther.
enum class AssetKind { kClass, kEnum, kStruct, kTable, kLanguage, kTexture, kMapEdit, kMaterial, kMesh, kWorld, kPacket, kOther };

struct AssetInfo {
  std::string path;  // "MyMod/Garden/Rock"
  std::string name;  // "Rock"
  std::filesystem::path file;
  AssetKind kind = AssetKind::kOther;
  std::string parent;  // a class's parent, as its file names it
};

// One mod's content: every folder in it and every asset, each sorted by path.
struct ModContent {
  std::string mod;
  std::vector<std::string> folders;
  std::vector<AssetInfo> assets;
};

// Assets as their files hold them, to edit and write back. Keys of a file they don't know are kept as they were. A
// default is kept as the file has it, fitted to its type only where it loads (types.h's CheckDeclared).
struct ClassAsset {
  std::string parent = "Object";
  std::string help;
  std::vector<Property> properties;  // the ones it adds; declared_by is left empty
  nlohmann::json defaults = nlohmann::json::object();
  // An avatar class's: the backing it names (empty to keep its parent's), and the components it adds to its parent's.
  std::string backing;
  std::vector<Component> components;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const ClassAsset& other) const;
};

struct EnumAsset {
  std::string help;
  std::vector<EnumValue> values;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const EnumAsset& other) const;
};

struct StructAsset {
  std::string help;
  std::vector<Property> fields;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const StructAsset& other) const;
};

// A data table: rows of one struct, each under a name, in order.
struct TableAsset {
  std::string row;  // the struct of its rows
  std::string help;
  std::vector<std::pair<std::string, nlohmann::json>> rows;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const TableAsset& other) const;
};

// The mod's texts in place of some of the game's own, in one of its languages (docs/content_and_classes.md, The
// game's texts): each by the game's id for it, with the game's text it was written over, so one whose game text
// changed since is out of date.
struct LanguageAsset {
  std::string language;  // the code of the language it's for
  std::string help;
  StringTable texts;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const LanguageAsset& other) const = default;
};

// A picture a mod imported (docs/content_and_classes.md, Textures), an asset of the class Texture. The picture is the
// image file beside it under its name ("Logo.png" for Logo.json), so it goes wherever the asset goes.
struct TextureAsset {
  std::string help;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const TextureAsset& other) const = default;
};

// A 3D model a mod imported (docs/content_and_classes.md, Meshes), an asset of the class Mesh. The model is the file
// beside it under its name ("Town.glb" for Town.json), as a texture's picture is; the game reads and draws it.
struct MeshAsset {
  std::string help;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const MeshAsset& other) const = default;
};

// A mod's world (docs/content_and_classes.md, Worlds), an asset of the class World: one of the game's worlds, its
// base, as it is, with the map edits made for this world besides the base's own. The game enters it in place of its
// base when something asks it to (a signpost that goes there).
struct WorldAsset {
  std::string base;
  std::string help;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const WorldAsset& other) const = default;
};

// A packet (docs/content_and_classes.md, Packets), an asset of the class Packet: a kind of ground a player buys and
// pours, a variant of one of the game's own kinds (its base) that looks and sells as it says. Its name and help are
// texts, translated as every mod text is.
struct PacketAsset {
  std::string base;  // a value of the game's packet base enum, by name
  std::string name;
  std::string help;
  std::string icon;  // a Texture asset
  nlohmann::json look = nlohmann::json::object();  // a value of the base's look struct
  PacketUnlock unlock = PacketUnlock::kDefault;
  int64_t level = 0;
  int64_t price = 0;
  double order = 0.0;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const PacketAsset& other) const = default;
};

// What a packet file's "unlock" says.
inline constexpr std::string_view kPacketUnlockDefault = "Default";
inline constexpr std::string_view kPacketUnlockShop = "Shop";

// One change a map edit makes to its world (docs/content_and_classes.md, Map edits): RexFabric's own Spawn, an avatar
// of a class placed there, or one of the kinds the game declares (DeclareMapEditKind).
inline constexpr std::string_view kSpawnEdit = "Spawn";
struct MapEditEntry {
  std::string name;  // unique in its map edit
  std::string kind = std::string(kSpawnEdit);
  Transform transform;  // where it is in the world
  std::string spawn;    // Spawn: the avatar class
  // Spawn: values of the class's properties set on this avatar alone; a game's kind: its properties.
  nlohmann::json values = nlohmann::json::object();

  bool operator==(const MapEditEntry&) const = default;
};

// A map edit (docs/content_and_classes.md, Map edits), an asset of the class MapEdit: the world it changes and its
// edits, made in order each time the game enters that world.
struct MapEditAsset {
  std::string world;
  std::string help;
  std::vector<MapEditEntry> edits;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const MapEditAsset& other) const = default;
};

// A material (docs/content_and_classes.md, Materials), an asset of the class Material: one of the game's material
// types, the values it gives the type's properties (a texture by its slot, a colour), and what the editor previews it
// on.
struct MaterialAsset {
  std::string type;
  std::string help;
  nlohmann::json values = nlohmann::json::object();
  nlohmann::json preview;  // the value of the type's preview choice (the model it's shown on), null for the default
  // A graph material's graph (content/material_graph.h), null for a material of a type without one.
  nlohmann::json graph;
  nlohmann::json rest = nlohmann::json::object();

  bool operator==(const MaterialAsset& other) const = default;
};

// The picture formats a texture can be imported from, by extension, lower case.
inline constexpr std::string_view kImageExtensions[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"};
// The model formats a mesh can be imported from: binary glTF, its textures inside it.
inline constexpr std::string_view kMeshExtensions[] = {".glb"};

// Something naming an asset: the asset it's in, and where ("parent", "default for price").
struct Reference {
  std::string from;
  std::string where;
};

std::filesystem::path ContentDir(const ModInfo& mod);
// The folder or asset file a path names, in `mod`'s content folder; the folder itself for the mod's path.
std::filesystem::path FolderOnDisk(const ModInfo& mod, std::string_view folder);
std::filesystem::path AssetFile(const ModInfo& mod, std::string_view path);

// The mod a path starts with ("MyMod" of "MyMod/Garden/Rock"), and the folder an asset or folder is in ("MyMod/Garden").
std::string_view ModOf(std::string_view path);
std::string_view FolderOf(std::string_view path);
std::string_view NameOf(std::string_view path);

// Reads a mod's content folder. Names that aren't valid are left out, as the loaders skip them.
ModContent ScanContent(const ModInfo& mod);

std::optional<ClassAsset> ReadClassAsset(const std::filesystem::path& file, std::string* error);
bool WriteClassAsset(const std::filesystem::path& file, const ClassAsset& asset, std::string* error);
std::optional<EnumAsset> ReadEnumAsset(const std::filesystem::path& file, std::string* error);
bool WriteEnumAsset(const std::filesystem::path& file, const EnumAsset& asset, std::string* error);
std::optional<StructAsset> ReadStructAsset(const std::filesystem::path& file, std::string* error);
bool WriteStructAsset(const std::filesystem::path& file, const StructAsset& asset, std::string* error);
std::optional<TableAsset> ReadTableAsset(const std::filesystem::path& file, std::string* error);
bool WriteTableAsset(const std::filesystem::path& file, const TableAsset& asset, std::string* error);

std::optional<TextureAsset> ReadTextureAsset(const std::filesystem::path& file, std::string* error);
bool WriteTextureAsset(const std::filesystem::path& file, const TextureAsset& asset, std::string* error);
// A texture asset's picture: the image file beside its file under its name, or empty for none.
std::filesystem::path TextureImageFile(const std::filesystem::path& asset_file);
// Makes `asset_file`'s picture a copy of `source`, dropping a picture it had in another format.
bool SetTextureImage(const std::filesystem::path& asset_file, const std::filesystem::path& source, std::string* error);

std::optional<MeshAsset> ReadMeshAsset(const std::filesystem::path& file, std::string* error);
bool WriteMeshAsset(const std::filesystem::path& file, const MeshAsset& asset, std::string* error);
// A mesh asset's model: the model file beside its file under its name, or empty for none.
std::filesystem::path MeshModelFile(const std::filesystem::path& asset_file);
// Makes `asset_file`'s model a copy of `source`.
bool SetMeshModel(const std::filesystem::path& asset_file, const std::filesystem::path& source, std::string* error);

std::optional<WorldAsset> ReadWorldAsset(const std::filesystem::path& file, std::string* error);
bool WriteWorldAsset(const std::filesystem::path& file, const WorldAsset& asset, std::string* error);

std::optional<PacketAsset> ReadPacketAsset(const std::filesystem::path& file, std::string* error);
bool WritePacketAsset(const std::filesystem::path& file, const PacketAsset& asset, std::string* error);

std::optional<MapEditAsset> ReadMapEditAsset(const std::filesystem::path& file, std::string* error);
bool WriteMapEditAsset(const std::filesystem::path& file, const MapEditAsset& asset, std::string* error);
std::optional<MaterialAsset> ReadMaterialAsset(const std::filesystem::path& file, std::string* error);
bool WriteMaterialAsset(const std::filesystem::path& file, const MaterialAsset& asset, std::string* error);

std::optional<LanguageAsset> ReadLanguageAsset(const std::filesystem::path& file, std::string* error);
bool WriteLanguageAsset(const std::filesystem::path& file, const LanguageAsset& asset, std::string* error);

// The enums and structs as the files have them now: a mod's read from its file, so one of a mod that's off or saved a
// moment ago is there, and the game's from the registry. For the editors' FitValue.
TypeLookup FileTypes(const ModList& mods);

// The properties a class with this parent inherits, its ancestors' first, each with the value nearest the class: a
// mod's class read from its file, so a class of a mod that's off or not saved since reads as it is now, and the
// rest from the registry. `problem` says where the chain broke, if it did.
std::vector<Property> InheritedProperties(const ModList& mods, std::string_view parent, std::string* problem = nullptr);
// What a class with this parent inherits as an avatar class, read the same way: the game class above it, the backing
// (its nearest mod ancestor's that names one, or the first the game offers for that game class; empty for none), and
// every component its ancestors have, theirs first.
struct InheritedAvatar {
  std::string game_class;
  std::string backing;
  std::vector<Component> components;
};
InheritedAvatar InheritAvatar(const ModList& mods, std::string_view parent);
// The class and its ancestors, nearest first, read the same way. Stops where the chain breaks.
std::vector<std::string> ClassChain(const ModList& mods, std::string_view path);

// Why `name` can't be a new folder or asset in `folder`, or nothing: a name that isn't valid, or one an asset or
// folder there has already, ignoring case. `keep` is a path allowed to have it, for a rename that changes only case.
std::string NameProblem(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view keep = {});

bool CreateFolder(const ModInfo& mod, std::string_view folder, std::string_view name, std::string* error);
bool CreateClass(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view parent, std::string* error);
bool CreateEnum(const ModInfo& mod, std::string_view folder, std::string_view name, std::string* error);
bool CreateStruct(const ModInfo& mod, std::string_view folder, std::string_view name, std::string* error);
bool CreateTable(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view row, std::string* error);
bool CreateLanguage(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view language, std::string* error);
bool CreateMapEdit(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view world, std::string* error);
bool CreateMaterial(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view type, std::string* error);
// A new texture of the picture at `source`, which is copied in beside it.
bool CreateTexture(const ModInfo& mod, std::string_view folder, std::string_view name, const std::filesystem::path& source, std::string* error);
// A new mesh of the model at `source`, which is copied in beside it.
bool CreateMesh(const ModInfo& mod, std::string_view folder, std::string_view name, const std::filesystem::path& source, std::string* error);
// A new world on the game's world `base`.
bool CreateWorld(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view base, std::string* error);
// A new packet of the game's packet base `base`, unlocked by default.
bool CreatePacket(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view base, std::string* error);
// Copies an asset beside itself, under a free name ("Rock_Copy", "Rock_Copy2"); `made` is the copy's path.
bool Duplicate(const ModInfo& mod, std::string_view path, std::string* made, std::string* error);
// Renames an asset or folder, or moves it to another folder of the same mod, and makes every asset of every mod
// naming it name its new path.
bool Rename(const ModList& mods, std::string_view path, std::string_view name, std::string* error);
bool Move(const ModList& mods, std::string_view path, std::string_view folder, std::string* error);
// Deletes an asset, or a folder and everything in it. What named them is left as it is, for the editors to report.
bool Delete(const ModInfo& mod, std::string_view path, std::string* error);

// Every asset of every mod naming `path`, or for a folder, anything in it, leaving out what's inside it itself.
std::vector<Reference> ReferencesTo(const ModList& mods, std::string_view path);

// After an enum's values or a struct's fields were renamed (old name to new) and the enum or struct saved: every value
// of it in every class, struct and table asset of every mod takes the new names. Gives what it couldn't write, if anything.
std::string RenameEnumValues(const ModList& mods, std::string_view enum_path, const std::map<std::string, std::string>& renames);
std::string RenameStructFields(const ModList& mods, std::string_view struct_path, const std::map<std::string, std::string>& renames);

}  // namespace rexfabric::content
