#include "assets.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <set>

#include "../ui/image_decode.h"
#include "localisation.h"
#include "map_edits.h"
#include "material_graph.h"
#include "materials.h"
#include "packets.h"
#include "types.h"

namespace rexfabric::content {
namespace {

constexpr std::string_view kObject = "Object";
// What "asset" says in each kind's file.
constexpr std::string_view kClassKind = "class";
constexpr std::string_view kEnumKind = "enum";
constexpr std::string_view kStructKind = "struct";
constexpr std::string_view kTableKind = "table";
constexpr std::string_view kLanguageKind = "language";
constexpr std::string_view kTextureKind = "texture";
constexpr std::string_view kMapEditKind = "map_edit";
constexpr std::string_view kMaterialKind = "material";
constexpr std::string_view kMeshKind = "mesh";
constexpr std::string_view kWorldKind = "world";
constexpr std::string_view kPacketKind = "packet";
// The keys each kind's file has that its asset holds itself; the rest go in the asset's `rest`.
constexpr std::string_view kClassKeys[] = {"asset", "parent", "help", "properties", "defaults", "backing", "components"};
constexpr std::string_view kEnumKeys[] = {"asset", "help", "values"};
constexpr std::string_view kStructKeys[] = {"asset", "help", "fields"};
constexpr std::string_view kTableKeys[] = {"asset", "row", "help", "rows"};
constexpr std::string_view kLanguageKeys[] = {"asset", "language", "help", "texts"};
constexpr std::string_view kTextureKeys[] = {"asset", "help"};
constexpr std::string_view kMeshKeys[] = {"asset", "help"};
constexpr std::string_view kWorldKeys[] = {"asset", "base", "help"};
constexpr std::string_view kPacketKeys[] = {"asset", "base", "name", "help", "icon", "look", "unlock", "level", "price", "order"};
constexpr std::string_view kMapEditKeys[] = {"asset", "world", "help", "edits"};
constexpr std::string_view kMaterialKeys[] = {"asset", "type", "help", "values", "preview", "graph"};
// A duplicate's name: the original's with this after it, then a number when that's taken.
constexpr std::string_view kCopySuffix = "_Copy";

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

// Whether `path` is `base` or inside it, ignoring case.
bool Within(std::string_view path, std::string_view base) {
  const std::string lower = Lower(path);
  const std::string prefix = Lower(base);
  return lower == prefix || (lower.size() > prefix.size() && lower.starts_with(prefix) && lower[prefix.size()] == '/');
}

// An object's value under a key, ignoring case; null for none.
const nlohmann::json* FindKey(const nlohmann::json& object, std::string_view key) {
  for (const auto& [name, value] : object.items()) {
    if (Same(name, key)) return &value;
  }
  return nullptr;
}

std::string Join(std::string_view folder, std::string_view name) { return std::string(folder) + "/" + std::string(name); }

// The path inside the mod's content folder: "Garden/Rock" of "MyMod/Garden/Rock", empty for the mod itself.
std::string_view InsideMod(std::string_view path) {
  const size_t slash = path.find('/');
  return slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
}

nlohmann::json ReadJson(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {};
  return nlohmann::json::parse(in, nullptr, false);
}

bool WriteJson(const std::filesystem::path& file, const nlohmann::ordered_json& json, std::string* error) {
  std::error_code code;
  std::filesystem::create_directories(file.parent_path(), code);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << json.dump(2) << "\n";
  if (!out && error) *error = "couldn't write " + file.string();
  return static_cast<bool>(out);
}

// A file of the kind, as an object: null with `error` for another kind or no object at all.
std::optional<nlohmann::json> ReadKind(const std::filesystem::path& file, std::string_view kind, std::string* error) {
  nlohmann::json json = ReadJson(file);
  if (!json.is_object()) {
    if (error) *error = file.filename().string() + " isn't a JSON object";
    return std::nullopt;
  }
  if (json.value("asset", std::string()) != kind) {
    if (error) *error = file.filename().string() + " isn't " + (kind == kEnumKind ? "an " : "a ") + std::string(kind);
    return std::nullopt;
  }
  return json;
}

// The keys of a file its asset doesn't hold, kept to write back.
template <size_t N>
nlohmann::json Rest(const nlohmann::json& json, const std::string_view (&known)[N]) {
  nlohmann::json rest = nlohmann::json::object();
  for (const auto& [key, value] : json.items()) {
    if (std::find(std::begin(known), std::end(known), key) == std::end(known)) rest[key] = value;
  }
  return rest;
}

// A property's or a field's declaration in a file, and back.
std::optional<Property> ReadDeclared(const nlohmann::json& entry, std::string* error) {
  std::optional<ValueType> type = ReadType(entry, error);
  if (!type) return std::nullopt;
  return Property{.name = entry.value("name", std::string()), .type = std::move(*type), .value = entry.value("default", nlohmann::json()), .help = entry.value("help", std::string())};
}

nlohmann::ordered_json WriteDeclared(const Property& declared) {
  nlohmann::ordered_json entry{{"name", declared.name}};
  WriteType(declared.type, entry);
  // None is the empty value, which is what a declaration without a default has.
  if (!declared.value.is_null()) entry["default"] = declared.value;
  if (!declared.help.empty()) entry["help"] = declared.help;
  return entry;
}

bool SameDeclared(const std::vector<Property>& a, const std::vector<Property>& b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const Property& x, const Property& y) { return x.name == y.name && x.type == y.type && x.value == y.value && x.help == y.help; });
}

bool IsImageFile(const std::filesystem::path& file) {
  const std::string extension = Lower(file.extension().string());
  return std::find(std::begin(kImageExtensions), std::end(kImageExtensions), extension) != std::end(kImageExtensions);
}

bool IsMeshFile(const std::filesystem::path& file) {
  const std::string extension = Lower(file.extension().string());
  return std::find(std::begin(kMeshExtensions), std::end(kMeshExtensions), extension) != std::end(kMeshExtensions);
}

// The files beside an asset that are its own: named after it and then a dot ("Rock.graph.json" for Rock.json).
std::vector<std::filesystem::path> Companions(const std::filesystem::path& file) {
  std::vector<std::filesystem::path> found;
  std::error_code error;
  const std::string prefix = Lower(file.stem().string()) + ".";
  for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), error)) {
    const std::string name = Lower(entry.path().filename().string());
    if (entry.is_regular_file(error) && name.starts_with(prefix) && !Same(entry.path().filename().string(), file.filename().string())) found.push_back(entry.path());
  }
  return found;
}

// A companion's file name once its asset is named `name`: "Rock.graph.json" to "Stone.graph.json".
std::string CompanionName(const std::filesystem::path& companion, const std::filesystem::path& asset, std::string_view name) {
  return std::string(name) + companion.filename().string().substr(asset.stem().string().size());
}

const ModInfo* ModFor(const ModList& mods, std::string_view path, std::string* error) {
  const ModInfo* mod = mods.Find(ModOf(path));
  if (!mod && error) *error = "There's no mod named " + std::string(ModOf(path));
  return mod;
}

// A class as InheritedProperties walks it: from its file for a mod's class, from the registry for the rest.
struct ClassView {
  std::string path;
  std::string parent;
  std::vector<Property> properties;
  nlohmann::json defaults = nlohmann::json::object();
  bool game = false;  // RexFabric's or the game's, from the registry
  std::string backing;
  std::vector<Component> components;
};

std::optional<ClassView> LookUp(const ModList& mods, std::string_view path) {
  if (std::optional<Class> known = FindClass(path); known && known->mod.empty()) return ClassView{.path = known->path, .parent = known->parent, .properties = known->properties, .defaults = known->defaults, .game = true};
  const ModInfo* mod = mods.Find(ModOf(path));
  if (!mod || InsideMod(path).empty()) return std::nullopt;
  std::optional<ClassAsset> asset = ReadClassAsset(AssetFile(*mod, path), nullptr);
  if (!asset) return std::nullopt;
  ClassView view{.path = std::string(path), .parent = asset->parent, .properties = std::move(asset->properties), .defaults = std::move(asset->defaults), .backing = std::move(asset->backing), .components = std::move(asset->components)};
  for (Property& property : view.properties) property.declared_by = view.path;
  return view;
}

// Each value an asset holds, with its type and where it is: a class's own properties' defaults and the inherited ones
// it sets (typed by what it inherits, so a value only typed by a property that went away is left alone), a struct's
// fields' defaults.
using ValueVisit = std::function<void(const ValueType& type, nlohmann::json& value, const std::string& where)>;

// The values a class's components set, each typed by its component type's property.
void ForEachComponentValue(std::vector<Component>& components, const ValueVisit& visit) {
  for (Component& component : components) {
    if (const std::shared_ptr<const ComponentType> type = FindComponentType(component.type)) {
      for (auto& [name, value] : component.properties.items()) {
        const auto found = std::find_if(type->properties.begin(), type->properties.end(), [&name](const Property& property) { return Same(property.name, name); });
        if (found != type->properties.end()) visit(found->type, value, "component " + component.name + ", " + found->name);
      }
    }
    ForEachComponentValue(component.children, visit);
  }
}

void ForEachValue(const ModList& mods, ClassAsset& asset, const ValueVisit& visit) {
  for (Property& property : asset.properties) visit(property.type, property.value, "property " + property.name);
  const std::vector<Property> inherited = InheritedProperties(mods, asset.parent);
  for (auto& [name, value] : asset.defaults.items()) {
    const auto found = std::find_if(inherited.begin(), inherited.end(), [&name](const Property& property) { return Same(property.name, name); });
    if (found != inherited.end()) visit(found->type, value, "default for " + name);
  }
  ForEachComponentValue(asset.components, visit);
}

void ForEachValue(StructAsset& asset, const ValueVisit& visit) {
  for (Property& field : asset.fields) visit(field.type, field.value, "field " + field.name);
}

void ForEachValue(TableAsset& asset, const ValueVisit& visit) {
  const ValueType row{.type = PropertyType::kStruct, .of = asset.row};
  for (auto& [name, value] : asset.rows) visit(row, value, "row " + name);
}

// A map edit's values: a spawn's typed as its class's properties, a game kind's as the kind's.
void ForEachValue(const ModList& mods, MapEditAsset& asset, const ValueVisit& visit) {
  for (MapEditEntry& entry : asset.edits) {
    std::vector<Property> declared;
    if (entry.kind == kSpawnEdit) declared = InheritedProperties(mods, entry.spawn);
    else if (const std::shared_ptr<const MapEditKind> kind = FindMapEditKind(entry.kind)) declared = kind->properties;
    for (auto& [name, value] : entry.values.items()) {
      const auto found = std::find_if(declared.begin(), declared.end(), [&name](const Property& property) { return Same(property.name, name); });
      if (found != declared.end()) visit(found->type, value, "edit " + entry.name + ", " + found->name);
    }
  }
}

// Each place an asset names another asset, as a string to read or change, with where it is: a class's parent, what
// types name (an enum, a struct, an asset's class) and asset values, nested ones too. Values are walked before types
// change, while the types still name what the files have.
using PathVisit = std::function<void(std::string& path, const std::string& where)>;

void VisitAssetValues(const TypeLookup& types, const ValueType& type, nlohmann::json& value, const std::string& where, const PathVisit& visit) {
  WalkValues(types, type, value, [&](const ValueType& single, nlohmann::json& held) {
    if (single.type != PropertyType::kAsset || !held.is_string()) return;
    std::string path = held.get<std::string>();
    visit(path, where);
    held = path;
  });
}

void VisitTypes(std::vector<Property>& declared, const std::string& what, const PathVisit& visit) {
  for (Property& entry : declared) {
    if (!entry.type.of.empty()) visit(entry.type.of, "the type of " + what + " " + entry.name);
    if (!entry.type.key_of.empty()) visit(entry.type.key_of, "the key of " + what + " " + entry.name);
  }
}

void ForEachPath(const ModList& mods, const TypeLookup& types, ClassAsset& asset, const PathVisit& visit) {
  ForEachValue(mods, asset, [&](const ValueType& type, nlohmann::json& value, const std::string& where) { VisitAssetValues(types, type, value, where, visit); });
  visit(asset.parent, "parent");
  VisitTypes(asset.properties, "property", visit);
}

void ForEachPath(const TypeLookup& types, StructAsset& asset, const PathVisit& visit) {
  ForEachValue(asset, [&](const ValueType& type, nlohmann::json& value, const std::string& where) { VisitAssetValues(types, type, value, where, visit); });
  VisitTypes(asset.fields, "field", visit);
}

void ForEachPath(const TypeLookup& types, TableAsset& asset, const PathVisit& visit) {
  ForEachValue(asset, [&](const ValueType& type, nlohmann::json& value, const std::string& where) { VisitAssetValues(types, type, value, where, visit); });
  visit(asset.row, "the struct of its rows");
}

// A material's values, typed as its type's properties.
void ForEachValue(MaterialAsset& asset, const ValueVisit& visit) {
  const std::shared_ptr<const MaterialType> type = FindMaterialType(asset.type);
  if (!type) return;
  for (auto& [name, value] : asset.values.items()) {
    const auto found = std::find_if(type->properties.begin(), type->properties.end(), [&name](const Property& property) { return Same(property.name, name); });
    if (found != type->properties.end()) visit(found->type, value, found->name);
  }
}

void ForEachPath(const TypeLookup& types, MaterialAsset& asset, const PathVisit& visit) {
  ForEachValue(asset, [&](const ValueType& type, nlohmann::json& value, const std::string& where) { VisitAssetValues(types, type, value, where, visit); });
  material::ForEachTexture(asset.graph, visit);
}

void ForEachPath(const ModList& mods, const TypeLookup& types, MapEditAsset& asset, const PathVisit& visit) {
  ForEachValue(mods, asset, [&](const ValueType& type, nlohmann::json& value, const std::string& where) { VisitAssetValues(types, type, value, where, visit); });
  for (MapEditEntry& entry : asset.edits) {
    if (entry.kind == kSpawnEdit) visit(entry.spawn, "the class edit " + entry.name + " spawns");
  }
}

// A packet's look, typed as its base's look struct; its icon is a path of its own.
void ForEachValue(PacketAsset& asset, const ValueVisit& visit) {
  const std::string look = PacketLookStruct(asset.base);
  if (!look.empty()) visit({.type = PropertyType::kStruct, .of = look}, asset.look, "its look");
}

void ForEachPath(const TypeLookup& types, PacketAsset& asset, const PathVisit& visit) {
  ForEachValue(asset, [&](const ValueType& type, nlohmann::json& value, const std::string& where) { VisitAssetValues(types, type, value, where, visit); });
  if (!asset.icon.empty()) visit(asset.icon, "its icon");
}

// A changed asset, ready to write: to its own file, or where it went.
struct Edit {
  std::filesystem::path file;
  std::function<bool(const std::filesystem::path& to, std::string* error)> write;
};

// What EditAssets does with each kind of asset holding values: true when it changed the asset.
struct AssetEditors {
  std::function<bool(const AssetInfo&, ClassAsset&)> on_class;
  std::function<bool(const AssetInfo&, StructAsset&)> on_struct;
  std::function<bool(const AssetInfo&, TableAsset&)> on_table;
  std::function<bool(const AssetInfo&, MapEditAsset&)> on_map_edit;
  std::function<bool(const AssetInfo&, MaterialAsset&)> on_material;
  std::function<bool(const AssetInfo&, PacketAsset&)> on_packet;
};

// Every class, struct, table and map edit asset of every mod, read and given to its editor; the ones changed come back
// as edits.
std::vector<Edit> EditAssets(const ModList& mods, const AssetEditors& editors) {
  const auto& on_class = editors.on_class;
  const auto& on_struct = editors.on_struct;
  std::vector<Edit> edits;
  for (const ModInfo& mod : mods.mods()) {
    for (const AssetInfo& info : ScanContent(mod).assets) {
      if (info.kind == AssetKind::kClass) {
        std::optional<ClassAsset> asset = ReadClassAsset(info.file, nullptr);
        if (asset && on_class(info, *asset)) edits.push_back({info.file, [kept = std::move(*asset)](const std::filesystem::path& to, std::string* error) { return WriteClassAsset(to, kept, error); }});
      } else if (info.kind == AssetKind::kStruct) {
        std::optional<StructAsset> asset = ReadStructAsset(info.file, nullptr);
        if (asset && on_struct(info, *asset)) edits.push_back({info.file, [kept = std::move(*asset)](const std::filesystem::path& to, std::string* error) { return WriteStructAsset(to, kept, error); }});
      } else if (info.kind == AssetKind::kTable) {
        std::optional<TableAsset> asset = ReadTableAsset(info.file, nullptr);
        if (asset && editors.on_table(info, *asset)) edits.push_back({info.file, [kept = std::move(*asset)](const std::filesystem::path& to, std::string* error) { return WriteTableAsset(to, kept, error); }});
      } else if (info.kind == AssetKind::kMapEdit && editors.on_map_edit) {
        std::optional<MapEditAsset> asset = ReadMapEditAsset(info.file, nullptr);
        if (asset && editors.on_map_edit(info, *asset)) edits.push_back({info.file, [kept = std::move(*asset)](const std::filesystem::path& to, std::string* error) { return WriteMapEditAsset(to, kept, error); }});
      } else if (info.kind == AssetKind::kMaterial && editors.on_material) {
        std::optional<MaterialAsset> asset = ReadMaterialAsset(info.file, nullptr);
        if (asset && editors.on_material(info, *asset)) edits.push_back({info.file, [kept = std::move(*asset)](const std::filesystem::path& to, std::string* error) { return WriteMaterialAsset(to, kept, error); }});
      } else if (info.kind == AssetKind::kPacket && editors.on_packet) {
        std::optional<PacketAsset> asset = ReadPacketAsset(info.file, nullptr);
        if (asset && editors.on_packet(info, *asset)) edits.push_back({info.file, [kept = std::move(*asset)](const std::filesystem::path& to, std::string* error) { return WritePacketAsset(to, kept, error); }});
      }
    }
  }
  return edits;
}

// Writes the edits where they are; gives what failed, if anything.
std::string WriteEdits(const std::vector<Edit>& edits) {
  std::string failed;
  for (const Edit& edit : edits) {
    std::string error;
    if (!edit.write(edit.file, &error)) failed = error;
  }
  return failed;
}

// Moves an asset or folder on disk to `to` (a path in the same mod), its companions with it, and repoints what names
// it. The edits are worked out before anything moves, while every asset still reads as it did.
bool Relocate(const ModList& mods, std::string_view from, std::string_view to, std::string* error) {
  const ModInfo* mod = ModFor(mods, from, error);
  if (!mod) return false;
  std::error_code code;
  const std::filesystem::path folder = FolderOnDisk(*mod, from);
  const bool is_folder = std::filesystem::is_directory(folder, code);
  const std::filesystem::path source = is_folder ? folder : AssetFile(*mod, from);
  const std::filesystem::path target = is_folder ? FolderOnDisk(*mod, to) : AssetFile(*mod, to);
  if (!is_folder && !std::filesystem::is_regular_file(source, code)) {
    if (error) *error = "There's no asset or folder " + std::string(from);
    return false;
  }

  const TypeLookup types = FileTypes(mods);
  const auto repoint = [&](bool& changed) {
    return [&, from, to](std::string& path, const std::string&) {
      if (!Within(path, from)) return;
      path = std::string(to) + path.substr(from.size());
      changed = true;
    };
  };
  const auto repointed = [&](auto& asset, auto&& walk) {
    bool changed = false;
    walk(asset, repoint(changed));
    return changed;
  };
  const std::vector<Edit> edits = EditAssets(mods, {
      .on_class = [&](const AssetInfo&, ClassAsset& asset) { return repointed(asset, [&](ClassAsset& held, const PathVisit& visit) { ForEachPath(mods, types, held, visit); }); },
      .on_struct = [&](const AssetInfo&, StructAsset& asset) { return repointed(asset, [&](StructAsset& held, const PathVisit& visit) { ForEachPath(types, held, visit); }); },
      .on_table = [&](const AssetInfo&, TableAsset& asset) { return repointed(asset, [&](TableAsset& held, const PathVisit& visit) { ForEachPath(types, held, visit); }); },
      .on_map_edit = [&](const AssetInfo&, MapEditAsset& asset) { return repointed(asset, [&](MapEditAsset& held, const PathVisit& visit) { ForEachPath(mods, types, held, visit); }); },
      .on_material = [&](const AssetInfo&, MaterialAsset& asset) { return repointed(asset, [&](MaterialAsset& held, const PathVisit& visit) { ForEachPath(types, held, visit); }); },
      .on_packet = [&](const AssetInfo&, PacketAsset& asset) { return repointed(asset, [&](PacketAsset& held, const PathVisit& visit) { ForEachPath(types, held, visit); }); },
  });

  const std::vector<std::filesystem::path> companions = is_folder ? std::vector<std::filesystem::path>{} : Companions(source);
  std::filesystem::create_directories(target.parent_path(), code);
  std::filesystem::rename(source, target, code);
  if (code) {
    if (error) *error = "Couldn't move " + std::string(from) + ": " + code.message();
    return false;
  }
  for (const std::filesystem::path& companion : companions) std::filesystem::rename(companion, target.parent_path() / CompanionName(companion, source, NameOf(to)), code);

  // An edited file that moved is written where it went.
  const std::string old_root = Lower(source.lexically_normal().generic_string());
  for (const Edit& edit : edits) {
    std::filesystem::path written = edit.file;
    const std::string generic = edit.file.lexically_normal().generic_string();
    if (Lower(generic) == old_root) written = target;
    else if (is_folder && Lower(generic).starts_with(old_root + "/")) written = target / std::filesystem::path(generic.substr(old_root.size() + 1));
    std::string failed;
    if (!edit.write(written, &failed) && error) *error = "Moved, but " + failed;
  }
  // Its texts' translations follow it.
  RenameTextKeys(*mod, from, to);
  return true;
}

// Every value of every class and struct asset, walked with `visit`, which says whether it changed one; the assets it
// changed are written.
std::string RenameValues(const ModList& mods, const std::function<bool(const ValueType& single, nlohmann::json& value)>& visit) {
  const TypeLookup types = FileTypes(mods);
  const auto walk = [&](bool& changed) {
    return [&](const ValueType& type, nlohmann::json& value, const std::string&) {
      WalkValues(types, type, value, [&](const ValueType& single, nlohmann::json& held) { changed |= visit(single, held); });
    };
  };
  const auto walked = [&](auto&& each) {
    bool changed = false;
    each(walk(changed));
    return changed;
  };
  return WriteEdits(EditAssets(mods, {
      .on_class = [&](const AssetInfo&, ClassAsset& asset) { return walked([&](const ValueVisit& visit) { ForEachValue(mods, asset, visit); }); },
      .on_struct = [&](const AssetInfo&, StructAsset& asset) { return walked([&](const ValueVisit& visit) { ForEachValue(asset, visit); }); },
      .on_table = [&](const AssetInfo&, TableAsset& asset) { return walked([&](const ValueVisit& visit) { ForEachValue(asset, visit); }); },
      .on_map_edit = [&](const AssetInfo&, MapEditAsset& asset) { return walked([&](const ValueVisit& visit) { ForEachValue(mods, asset, visit); }); },
      .on_material = [&](const AssetInfo&, MaterialAsset& asset) { return walked([&](const ValueVisit& visit) { ForEachValue(asset, visit); }); },
      .on_packet = [&](const AssetInfo&, PacketAsset& asset) { return walked([&](const ValueVisit& visit) { ForEachValue(asset, visit); }); },
  }));
}

bool CreateAsset(const ModInfo& mod, std::string_view folder, std::string_view name, const std::function<bool(const std::filesystem::path&, std::string*)>& write, std::string* error) {
  if (std::string why = NameProblem(mod, folder, name); !why.empty()) {
    if (error) *error = std::move(why);
    return false;
  }
  return write(AssetFile(mod, Join(folder, name)), error);
}

}  // namespace

bool ClassAsset::operator==(const ClassAsset& other) const { return parent == other.parent && help == other.help && defaults == other.defaults && backing == other.backing && components == other.components && rest == other.rest && SameDeclared(properties, other.properties); }

bool EnumAsset::operator==(const EnumAsset& other) const {
  const auto same = [](const EnumValue& a, const EnumValue& b) { return a.name == b.name && a.display == b.display && a.help == b.help; };
  return help == other.help && rest == other.rest && std::equal(values.begin(), values.end(), other.values.begin(), other.values.end(), same);
}

bool StructAsset::operator==(const StructAsset& other) const { return help == other.help && rest == other.rest && SameDeclared(fields, other.fields); }

bool TableAsset::operator==(const TableAsset& other) const { return row == other.row && help == other.help && rows == other.rows && rest == other.rest; }

std::filesystem::path ContentDir(const ModInfo& mod) { return mod.dir / kContentFolder; }

std::filesystem::path FolderOnDisk(const ModInfo& mod, std::string_view folder) {
  const std::string_view inside = InsideMod(folder);
  return inside.empty() ? ContentDir(mod) : ContentDir(mod) / std::filesystem::path(inside);
}

std::filesystem::path AssetFile(const ModInfo& mod, std::string_view path) {
  std::filesystem::path file = FolderOnDisk(mod, path);
  file += kAssetExtension;
  return file;
}

std::string_view ModOf(std::string_view path) { return path.substr(0, path.find('/')); }

std::string_view FolderOf(std::string_view path) {
  const size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(0, slash);
}

std::string_view NameOf(std::string_view path) {
  const size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

ModContent ScanContent(const ModInfo& mod) {
  ModContent content;
  content.mod = mod.folder;
  std::error_code error;
  const std::filesystem::path root = ContentDir(mod);
  if (!std::filesystem::is_directory(root, error)) return content;
  for (auto it = std::filesystem::recursive_directory_iterator(root, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    std::string path = mod.folder;
    for (const auto& part : std::filesystem::relative(it->path(), root, error)) path += "/" + part.string();
    if (it->is_directory(error)) {
      if (ValidName(it->path().filename().string())) content.folders.push_back(std::move(path));
      else it.disable_recursion_pending();
      continue;
    }
    const std::filesystem::path& file = it->path();
    if (!Same(file.extension().string(), kAssetExtension) || !file.stem().extension().empty() || !ValidName(file.stem().string())) continue;
    path.resize(path.size() - file.extension().string().size());
    AssetInfo info{.path = std::move(path), .name = file.stem().string(), .file = file};
    const nlohmann::json json = ReadJson(file);
    const std::string kind = json.is_object() ? json.value("asset", std::string()) : std::string();
    if (kind == kClassKind) {
      info.kind = AssetKind::kClass;
      info.parent = json.value("parent", std::string(kObject));
    } else if (kind == kEnumKind) {
      info.kind = AssetKind::kEnum;
    } else if (kind == kStructKind) {
      info.kind = AssetKind::kStruct;
    } else if (kind == kTableKind) {
      info.kind = AssetKind::kTable;
    } else if (kind == kLanguageKind) {
      info.kind = AssetKind::kLanguage;
    } else if (kind == kTextureKind) {
      info.kind = AssetKind::kTexture;
    } else if (kind == kMapEditKind) {
      info.kind = AssetKind::kMapEdit;
    } else if (kind == kMaterialKind) {
      info.kind = AssetKind::kMaterial;
    } else if (kind == kMeshKind) {
      info.kind = AssetKind::kMesh;
    } else if (kind == kWorldKind) {
      info.kind = AssetKind::kWorld;
    } else if (kind == kPacketKind) {
      info.kind = AssetKind::kPacket;
    }
    content.assets.push_back(std::move(info));
  }
  const auto by_path = [](const auto& a, const auto& b) { return Lower(a) < Lower(b); };
  std::sort(content.folders.begin(), content.folders.end(), by_path);
  std::sort(content.assets.begin(), content.assets.end(), [&by_path](const AssetInfo& a, const AssetInfo& b) { return by_path(a.path, b.path); });
  return content;
}

std::optional<ClassAsset> ReadClassAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kClassKind, error);
  if (!json) return std::nullopt;
  ClassAsset asset;
  asset.parent = json->value("parent", std::string(kObject));
  asset.help = json->value("help", std::string());
  const nlohmann::json properties = json->value("properties", nlohmann::json::array());
  const nlohmann::json defaults = json->value("defaults", nlohmann::json::object());
  if (!properties.is_array() || !defaults.is_object()) {
    if (error) *error = "\"properties\" is a list and \"defaults\" an object";
    return std::nullopt;
  }
  for (const nlohmann::json& entry : properties) {
    std::optional<Property> property = ReadDeclared(entry, error);
    if (!property) return std::nullopt;
    asset.properties.push_back(std::move(*property));
  }
  asset.defaults = defaults;
  asset.backing = json->value("backing", std::string());
  std::optional<std::vector<Component>> components = ReadComponents(json->value("components", nlohmann::json::array()), error);
  if (!components) return std::nullopt;
  asset.components = std::move(*components);
  asset.rest = Rest(*json, kClassKeys);
  return asset;
}

bool WriteClassAsset(const std::filesystem::path& file, const ClassAsset& asset, std::string* error) {
  // In the order a person reads it, so the files diff well.
  nlohmann::ordered_json json;
  json["asset"] = kClassKind;
  json["parent"] = asset.parent;
  if (!asset.help.empty()) json["help"] = asset.help;
  if (!asset.properties.empty()) {
    nlohmann::ordered_json properties = nlohmann::ordered_json::array();
    for (const Property& property : asset.properties) properties.push_back(WriteDeclared(property));
    json["properties"] = std::move(properties);
  }
  if (!asset.defaults.empty()) json["defaults"] = asset.defaults;
  if (!asset.backing.empty()) json["backing"] = asset.backing;
  if (!asset.components.empty()) json["components"] = WriteComponents(asset.components);
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<EnumAsset> ReadEnumAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kEnumKind, error);
  if (!json) return std::nullopt;
  EnumAsset asset;
  asset.help = json->value("help", std::string());
  const nlohmann::json values = json->value("values", nlohmann::json::array());
  if (!values.is_array()) {
    if (error) *error = "\"values\" is a list";
    return std::nullopt;
  }
  for (const nlohmann::json& entry : values) {
    if (!entry.is_object()) {
      if (error) *error = "a value is an object with its name, not " + entry.dump();
      return std::nullopt;
    }
    asset.values.push_back({.name = entry.value("name", std::string()), .display = entry.value("display", std::string()), .help = entry.value("help", std::string())});
  }
  asset.rest = Rest(*json, kEnumKeys);
  return asset;
}

bool WriteEnumAsset(const std::filesystem::path& file, const EnumAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kEnumKind;
  if (!asset.help.empty()) json["help"] = asset.help;
  nlohmann::ordered_json values = nlohmann::ordered_json::array();
  for (const EnumValue& value : asset.values) {
    nlohmann::ordered_json entry{{"name", value.name}};
    if (!value.display.empty()) entry["display"] = value.display;
    if (!value.help.empty()) entry["help"] = value.help;
    values.push_back(std::move(entry));
  }
  json["values"] = std::move(values);
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<StructAsset> ReadStructAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kStructKind, error);
  if (!json) return std::nullopt;
  StructAsset asset;
  asset.help = json->value("help", std::string());
  const nlohmann::json fields = json->value("fields", nlohmann::json::array());
  if (!fields.is_array()) {
    if (error) *error = "\"fields\" is a list";
    return std::nullopt;
  }
  for (const nlohmann::json& entry : fields) {
    std::optional<Property> field = ReadDeclared(entry, error);
    if (!field) return std::nullopt;
    asset.fields.push_back(std::move(*field));
  }
  asset.rest = Rest(*json, kStructKeys);
  return asset;
}

bool WriteStructAsset(const std::filesystem::path& file, const StructAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kStructKind;
  if (!asset.help.empty()) json["help"] = asset.help;
  nlohmann::ordered_json fields = nlohmann::ordered_json::array();
  for (const Property& field : asset.fields) fields.push_back(WriteDeclared(field));
  json["fields"] = std::move(fields);
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<TableAsset> ReadTableAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kTableKind, error);
  if (!json) return std::nullopt;
  TableAsset asset;
  asset.row = json->value("row", std::string());
  asset.help = json->value("help", std::string());
  const nlohmann::json rows = json->value("rows", nlohmann::json::array());
  if (!rows.is_array()) {
    if (error) *error = "\"rows\" is a list of [name, row] pairs";
    return std::nullopt;
  }
  for (const nlohmann::json& entry : rows) {
    if (!entry.is_array() || entry.size() != 2 || !entry[0].is_string()) {
      if (error) *error = "a row is a [name, row] pair, not " + entry.dump();
      return std::nullopt;
    }
    asset.rows.emplace_back(entry[0].get<std::string>(), entry[1]);
  }
  asset.rest = Rest(*json, kTableKeys);
  return asset;
}

bool WriteTableAsset(const std::filesystem::path& file, const TableAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kTableKind;
  json["row"] = asset.row;
  if (!asset.help.empty()) json["help"] = asset.help;
  nlohmann::ordered_json rows = nlohmann::ordered_json::array();
  for (const auto& [name, value] : asset.rows) rows.push_back(nlohmann::ordered_json::array({name, value}));
  json["rows"] = std::move(rows);
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<TextureAsset> ReadTextureAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kTextureKind, error);
  if (!json) return std::nullopt;
  return TextureAsset{.help = json->value("help", std::string()), .rest = Rest(*json, kTextureKeys)};
}

bool WriteTextureAsset(const std::filesystem::path& file, const TextureAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kTextureKind;
  if (!asset.help.empty()) json["help"] = asset.help;
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<MeshAsset> ReadMeshAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kMeshKind, error);
  if (!json) return std::nullopt;
  return MeshAsset{.help = json->value("help", std::string()), .rest = Rest(*json, kMeshKeys)};
}

bool WriteMeshAsset(const std::filesystem::path& file, const MeshAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kMeshKind;
  if (!asset.help.empty()) json["help"] = asset.help;
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<WorldAsset> ReadWorldAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kWorldKind, error);
  if (!json) return std::nullopt;
  return WorldAsset{.base = json->value("base", std::string()), .help = json->value("help", std::string()), .rest = Rest(*json, kWorldKeys)};
}

bool WriteWorldAsset(const std::filesystem::path& file, const WorldAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kWorldKind;
  json["base"] = asset.base;
  if (!asset.help.empty()) json["help"] = asset.help;
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<PacketAsset> ReadPacketAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kPacketKind, error);
  if (!json) return std::nullopt;
  PacketAsset asset{.base = json->value("base", std::string()), .name = json->value("name", std::string()), .help = json->value("help", std::string()), .icon = json->value("icon", std::string()), .look = json->value("look", nlohmann::json::object()), .rest = Rest(*json, kPacketKeys)};
  const std::string unlock = json->value("unlock", std::string(kPacketUnlockDefault));
  if (unlock != kPacketUnlockDefault && unlock != kPacketUnlockShop) {
    if (error) *error = "\"unlock\" is \"" + std::string(kPacketUnlockDefault) + "\" or \"" + std::string(kPacketUnlockShop) + "\", not \"" + unlock + "\"";
    return std::nullopt;
  }
  if (!asset.look.is_object()) {
    if (error) *error = "\"look\" is an object of its base's look";
    return std::nullopt;
  }
  asset.unlock = unlock == kPacketUnlockShop ? PacketUnlock::kShop : PacketUnlock::kDefault;
  const auto number = [&json](const char* key) { return json->contains(key) && (*json)[key].is_number() ? std::optional<nlohmann::json>((*json)[key]) : std::nullopt; };
  if (const auto level = number("level")) asset.level = level->get<int64_t>();
  if (const auto price = number("price")) asset.price = price->get<int64_t>();
  if (const auto order = number("order")) asset.order = order->get<double>();
  return asset;
}

bool WritePacketAsset(const std::filesystem::path& file, const PacketAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kPacketKind;
  json["base"] = asset.base;
  json["name"] = asset.name;
  if (!asset.help.empty()) json["help"] = asset.help;
  if (!asset.icon.empty()) json["icon"] = asset.icon;
  if (!asset.look.empty()) json["look"] = nlohmann::ordered_json::parse(asset.look.dump());
  json["unlock"] = asset.unlock == PacketUnlock::kShop ? kPacketUnlockShop : kPacketUnlockDefault;
  if (asset.unlock == PacketUnlock::kShop) {
    json["level"] = asset.level;
    json["price"] = asset.price;
  }
  if (asset.order != 0.0) json["order"] = asset.order;
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<MapEditAsset> ReadMapEditAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kMapEditKind, error);
  if (!json) return std::nullopt;
  MapEditAsset asset{.world = json->value("world", std::string()), .help = json->value("help", std::string()), .rest = Rest(*json, kMapEditKeys)};
  const nlohmann::json edits = json->value("edits", nlohmann::json::array());
  if (!edits.is_array()) {
    if (error) *error = "\"edits\" is a list";
    return std::nullopt;
  }
  for (const nlohmann::json& entry : edits) {
    if (!entry.is_object()) {
      if (error) *error = "an edit is an object, not " + entry.dump();
      return std::nullopt;
    }
    MapEditEntry edit{.name = entry.value("name", std::string()), .kind = entry.value("kind", std::string(kSpawnEdit)), .spawn = entry.value("class", std::string()), .values = entry.value("values", nlohmann::json::object())};
    if (!edit.values.is_object()) {
      if (error) *error = "edit " + edit.name + ": its values are an object";
      return std::nullopt;
    }
    if (!ReadTransform(entry.value("transform", nlohmann::json::object()), edit.transform, error)) return std::nullopt;
    asset.edits.push_back(std::move(edit));
  }
  return asset;
}

bool WriteMapEditAsset(const std::filesystem::path& file, const MapEditAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kMapEditKind;
  json["world"] = asset.world;
  if (!asset.help.empty()) json["help"] = asset.help;
  nlohmann::ordered_json edits = nlohmann::ordered_json::array();
  for (const MapEditEntry& edit : asset.edits) {
    nlohmann::ordered_json entry{{"name", edit.name}, {"kind", edit.kind}};
    if (edit.kind == kSpawnEdit) entry["class"] = edit.spawn;
    if (nlohmann::ordered_json transform = WriteTransform(edit.transform); !transform.empty()) entry["transform"] = std::move(transform);
    if (!edit.values.empty()) entry["values"] = nlohmann::ordered_json::parse(edit.values.dump());
    edits.push_back(std::move(entry));
  }
  json["edits"] = std::move(edits);
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<MaterialAsset> ReadMaterialAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kMaterialKind, error);
  if (!json) return std::nullopt;
  MaterialAsset asset{.type = json->value("type", std::string()), .help = json->value("help", std::string()), .values = json->value("values", nlohmann::json::object()), .preview = json->value("preview", nlohmann::json()), .graph = json->value("graph", nlohmann::json()), .rest = Rest(*json, kMaterialKeys)};
  if (!asset.values.is_object()) {
    if (error) *error = "its values are an object";
    return std::nullopt;
  }
  return asset;
}

bool WriteMaterialAsset(const std::filesystem::path& file, const MaterialAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kMaterialKind;
  json["type"] = asset.type;
  if (!asset.help.empty()) json["help"] = asset.help;
  if (!asset.values.empty()) json["values"] = nlohmann::ordered_json::parse(asset.values.dump());
  if (!asset.preview.is_null()) json["preview"] = nlohmann::ordered_json::parse(asset.preview.dump());
  if (!asset.graph.is_null()) json["graph"] = nlohmann::ordered_json::parse(asset.graph.dump());
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::filesystem::path TextureImageFile(const std::filesystem::path& asset_file) {
  for (const std::filesystem::path& companion : Companions(asset_file)) {
    // Its own picture: its name and one extension ("Logo.png", not "Logo.old.png").
    if (IsImageFile(companion) && Same(companion.stem().string(), asset_file.stem().string())) return companion;
  }
  return {};
}

std::filesystem::path MeshModelFile(const std::filesystem::path& asset_file) {
  for (const std::filesystem::path& companion : Companions(asset_file)) {
    if (IsMeshFile(companion) && Same(companion.stem().string(), asset_file.stem().string())) return companion;
  }
  return {};
}

bool SetMeshModel(const std::filesystem::path& asset_file, const std::filesystem::path& source, std::string* error) {
  if (!IsMeshFile(source)) {
    if (error) *error = source.filename().string() + " isn't a model RexFabric takes (binary glTF, .glb)";
    return false;
  }
  std::filesystem::path target = asset_file;
  target.replace_extension(Lower(source.extension().string()));
  std::error_code code;
  if (std::filesystem::equivalent(source, target, code)) return true;
  std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, code);
  if (code) {
    if (error) *error = "Couldn't copy " + source.string() + ": " + code.message();
    return false;
  }
  return true;
}

bool SetTextureImage(const std::filesystem::path& asset_file, const std::filesystem::path& source, std::string* error) {
  if (!IsImageFile(source)) {
    if (error) *error = source.filename().string() + " isn't a picture RexFabric reads (PNG, JPEG, BMP, TGA or GIF)";
    return false;
  }
  std::filesystem::path target = asset_file;
  target.replace_extension(Lower(source.extension().string()));
  std::error_code code;
  if (std::filesystem::equivalent(source, target, code)) return true;
  std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, code);
  if (code) {
    if (error) *error = "Couldn't copy " + source.string() + ": " + code.message();
    return false;
  }
  // Its picture in any other format is the old one.
  for (const std::filesystem::path& companion : Companions(asset_file)) {
    if (IsImageFile(companion) && Same(companion.stem().string(), asset_file.stem().string()) && !Same(companion.filename().string(), target.filename().string())) std::filesystem::remove(companion, code);
  }
  return true;
}

std::optional<LanguageAsset> ReadLanguageAsset(const std::filesystem::path& file, std::string* error) {
  std::optional<nlohmann::json> json = ReadKind(file, kLanguageKind, error);
  if (!json) return std::nullopt;
  LanguageAsset asset;
  asset.language = json->value("language", std::string());
  asset.help = json->value("help", std::string());
  const nlohmann::json texts = json->value("texts", nlohmann::json::object());
  if (!texts.is_object()) {
    if (error) *error = "\"texts\" is an object of each text's id to its source and text";
    return std::nullopt;
  }
  for (const auto& [id, entry] : texts.items()) {
    if (entry.is_object()) asset.texts[id] = {entry.value("source", std::string()), entry.value("text", std::string())};
  }
  asset.rest = Rest(*json, kLanguageKeys);
  return asset;
}

bool WriteLanguageAsset(const std::filesystem::path& file, const LanguageAsset& asset, std::string* error) {
  nlohmann::ordered_json json;
  json["asset"] = kLanguageKind;
  json["language"] = asset.language;
  if (!asset.help.empty()) json["help"] = asset.help;
  nlohmann::ordered_json texts = nlohmann::ordered_json::object();
  for (const auto& [id, translation] : asset.texts) {
    if (!translation.text.empty()) texts[id] = {{"source", translation.source}, {"text", translation.text}};
  }
  json["texts"] = std::move(texts);
  for (const auto& [key, value] : asset.rest.items()) json[key] = value;
  return WriteJson(file, json, error);
}

std::optional<std::filesystem::path> MeshModelOf(std::string_view path, std::string* why) {
  const auto fail = [why](std::string reason) -> std::optional<std::filesystem::path> {
    if (why) *why = std::move(reason);
    return std::nullopt;
  };
  const std::optional<std::filesystem::path> dir = LoadedModDir(ModOf(path));
  if (!dir || InsideMod(path).empty()) return fail("no loaded mod has " + std::string(path));
  const ModInfo mod{.folder = std::string(ModOf(path)), .dir = *dir};
  const std::filesystem::path file = AssetFile(mod, path);
  if (!ReadMeshAsset(file, why)) return std::nullopt;
  const std::filesystem::path model = MeshModelFile(file);
  if (model.empty()) return fail(std::string(path) + " has no model beside it");
  return model;
}

std::optional<TextureImage> LoadTextureImage(std::string_view path, std::string* why) {
  const auto fail = [why](std::string reason) -> std::optional<TextureImage> {
    if (why) *why = std::move(reason);
    return std::nullopt;
  };
  const std::optional<std::filesystem::path> dir = LoadedModDir(ModOf(path));
  if (!dir || InsideMod(path).empty()) return fail("no loaded mod has " + std::string(path));
  const ModInfo mod{.folder = std::string(ModOf(path)), .dir = *dir};
  const std::filesystem::path file = AssetFile(mod, path);
  if (!ReadTextureAsset(file, why)) return std::nullopt;
  const std::filesystem::path image = TextureImageFile(file);
  if (image.empty()) return fail(std::string(path) + " has no picture beside it");
  std::ifstream in(image, std::ios::binary);
  const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  int width = 0;
  int height = 0;
  TextureImage loaded;
  loaded.rgba = ui::DecodeImageRGBA(encoded.data(), encoded.size(), width, height);
  if (loaded.rgba.empty() || width <= 0 || height <= 0) return fail(image.filename().string() + " couldn't be read as a picture");
  loaded.width = static_cast<uint32_t>(width);
  loaded.height = static_cast<uint32_t>(height);
  std::error_code code;
  loaded.version = static_cast<uint64_t>(std::filesystem::last_write_time(image, code).time_since_epoch().count());
  return loaded;
}

TypeLookup FileTypes(const ModList& mods) {
  // A mod's path is read from its file; the rest (the game's) from the registry.
  const auto mod_file = [&mods](std::string_view path) -> std::optional<std::filesystem::path> {
    const ModInfo* mod = path.find('/') != std::string_view::npos ? mods.Find(ModOf(path)) : nullptr;
    if (!mod) return std::nullopt;
    return AssetFile(*mod, path);
  };
  return {
      .find_enum = [mod_file](std::string_view path) -> std::optional<Enum> {
        const std::optional<std::filesystem::path> file = mod_file(path);
        if (!file) return FindEnum(path);
        std::optional<EnumAsset> asset = ReadEnumAsset(*file, nullptr);
        if (!asset) return std::nullopt;
        return Enum{.path = std::string(path), .mod = std::string(ModOf(path)), .help = std::move(asset->help), .values = std::move(asset->values)};
      },
      .find_struct = [mod_file](std::string_view path) -> std::optional<Struct> {
        if (std::optional<Struct> known = FindStruct(path); known && known->mod.empty()) return known;
        const std::optional<std::filesystem::path> file = mod_file(path);
        if (!file) return std::nullopt;
        std::optional<StructAsset> asset = ReadStructAsset(*file, nullptr);
        if (!asset) return std::nullopt;
        return Struct{.path = std::string(path), .mod = std::string(ModOf(path)), .help = std::move(asset->help), .fields = std::move(asset->fields)};
      },
  };
}

std::vector<std::string> ClassChain(const ModList& mods, std::string_view path) {
  std::vector<std::string> chain;
  std::set<std::string> seen;
  for (std::string at(path); !at.empty() && seen.insert(Lower(at)).second;) {
    std::optional<ClassView> view = LookUp(mods, at);
    if (!view) break;
    chain.push_back(view->path);
    at = view->parent;
  }
  return chain;
}

InheritedAvatar InheritAvatar(const ModList& mods, std::string_view parent) {
  InheritedAvatar inherited;
  std::vector<std::vector<Component>> trees;
  std::set<std::string> seen;
  for (std::string at(parent); !at.empty() && seen.insert(Lower(at)).second;) {
    std::optional<ClassView> view = LookUp(mods, at);
    if (!view) break;
    if (view->game) {
      inherited.game_class = view->path;
      const std::vector<std::string> offered = BackingsFor(view->path);
      if (inherited.backing.empty() && !offered.empty()) inherited.backing = offered.front();
      break;
    }
    if (inherited.backing.empty()) inherited.backing = view->backing;
    trees.push_back(std::move(view->components));
    at = view->parent;
  }
  for (auto it = trees.rbegin(); it != trees.rend(); ++it) inherited.components.insert(inherited.components.end(), it->begin(), it->end());
  return inherited;
}

std::vector<Property> InheritedProperties(const ModList& mods, std::string_view parent, std::string* problem) {
  std::vector<ClassView> chain;
  std::set<std::string> seen;
  for (std::string at(parent); !at.empty();) {
    if (!seen.insert(Lower(at)).second) {
      if (problem) *problem = "its parents go round in a circle at " + at;
      break;
    }
    std::optional<ClassView> view = LookUp(mods, at);
    if (!view) {
      if (problem) *problem = at + " isn't a class";
      break;
    }
    at = view->parent;
    chain.push_back(std::move(*view));
  }
  std::vector<Property> properties;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (const Property& property : it->properties) properties.push_back(property);
  }
  // Each with the nearest value along the chain, stopping at the class that declares it.
  for (Property& property : properties) {
    for (const ClassView& at : chain) {
      if (const nlohmann::json* set = FindKey(at.defaults, property.name)) {
        property.value = *set;
        break;
      }
      if (Same(at.path, property.declared_by)) break;
    }
  }
  return properties;
}

std::string NameProblem(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view keep) {
  std::string why;
  if (!ValidName(name, &why)) return why;
  const std::string wanted = Lower(name);
  const std::string kept = keep.empty() ? std::string() : Lower(NameOf(keep));
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(FolderOnDisk(mod, folder), error)) {
    const std::filesystem::path& file = entry.path();
    const bool folder_entry = entry.is_directory(error);
    const std::string taken = Lower(folder_entry ? file.filename().string() : file.stem().string());
    const bool asset_entry = !folder_entry && Same(file.extension().string(), kAssetExtension) && file.stem().extension().empty();
    if ((folder_entry || asset_entry) && taken == wanted && taken != kept) return "There's an asset or folder named " + std::string(name) + " here already";
  }
  return {};
}

bool CreateFolder(const ModInfo& mod, std::string_view folder, std::string_view name, std::string* error) {
  if (std::string why = NameProblem(mod, folder, name); !why.empty()) {
    if (error) *error = std::move(why);
    return false;
  }
  std::error_code code;
  std::filesystem::create_directories(FolderOnDisk(mod, Join(folder, name)), code);
  if (code && error) *error = "Couldn't make the folder: " + code.message();
  return !code;
}

bool CreateClass(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view parent, std::string* error) {
  ClassAsset asset;
  asset.parent = std::string(parent);
  return CreateAsset(mod, folder, name, [&asset](const std::filesystem::path& file, std::string* failed) { return WriteClassAsset(file, asset, failed); }, error);
}

bool CreateEnum(const ModInfo& mod, std::string_view folder, std::string_view name, std::string* error) {
  return CreateAsset(mod, folder, name, [](const std::filesystem::path& file, std::string* failed) { return WriteEnumAsset(file, EnumAsset{}, failed); }, error);
}

bool CreateStruct(const ModInfo& mod, std::string_view folder, std::string_view name, std::string* error) {
  return CreateAsset(mod, folder, name, [](const std::filesystem::path& file, std::string* failed) { return WriteStructAsset(file, StructAsset{}, failed); }, error);
}

bool CreateTable(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view row, std::string* error) {
  TableAsset asset;
  asset.row = std::string(row);
  return CreateAsset(mod, folder, name, [&asset](const std::filesystem::path& file, std::string* failed) { return WriteTableAsset(file, asset, failed); }, error);
}

bool CreateLanguage(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view language, std::string* error) {
  LanguageAsset asset;
  asset.language = std::string(language);
  return CreateAsset(mod, folder, name, [&asset](const std::filesystem::path& file, std::string* failed) { return WriteLanguageAsset(file, asset, failed); }, error);
}

bool CreateMapEdit(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view world, std::string* error) {
  const MapEditAsset asset{.world = std::string(world)};
  return CreateAsset(mod, folder, name, [&asset](const std::filesystem::path& file, std::string* failed) { return WriteMapEditAsset(file, asset, failed); }, error);
}

bool CreateMaterial(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view type, std::string* error) {
  MaterialAsset asset{.type = std::string(type)};
  // A graph material starts as its Material node alone.
  if (const std::shared_ptr<const MaterialType> declared = FindMaterialType(type); declared && declared->HasGraph()) asset.graph = material::DefaultGraph(*declared).Write();
  return CreateAsset(mod, folder, name, [&asset](const std::filesystem::path& file, std::string* failed) { return WriteMaterialAsset(file, asset, failed); }, error);
}

bool CreateTexture(const ModInfo& mod, std::string_view folder, std::string_view name, const std::filesystem::path& source, std::string* error) {
  std::error_code code;
  if (!std::filesystem::is_regular_file(source, code)) {
    if (error) *error = "There's no file " + source.string();
    return false;
  }
  return CreateAsset(mod, folder, name, [&source](const std::filesystem::path& file, std::string* failed) { return SetTextureImage(file, source, failed) && WriteTextureAsset(file, TextureAsset{}, failed); }, error);
}

bool CreateMesh(const ModInfo& mod, std::string_view folder, std::string_view name, const std::filesystem::path& source, std::string* error) {
  std::error_code code;
  if (!std::filesystem::is_regular_file(source, code)) {
    if (error) *error = "There's no file " + source.string();
    return false;
  }
  return CreateAsset(mod, folder, name, [&source](const std::filesystem::path& file, std::string* failed) { return SetMeshModel(file, source, failed) && WriteMeshAsset(file, MeshAsset{}, failed); }, error);
}

bool CreateWorld(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view base, std::string* error) {
  const WorldAsset asset{.base = std::string(base)};
  return CreateAsset(mod, folder, name, [&asset](const std::filesystem::path& file, std::string* failed) { return WriteWorldAsset(file, asset, failed); }, error);
}

bool CreatePacket(const ModInfo& mod, std::string_view folder, std::string_view name, std::string_view base, std::string* error) {
  const PacketAsset asset{.base = std::string(base), .name = std::string(name)};
  return CreateAsset(mod, folder, name, [&asset](const std::filesystem::path& file, std::string* failed) { return WritePacketAsset(file, asset, failed); }, error);
}

bool Duplicate(const ModInfo& mod, std::string_view path, std::string* made, std::string* error) {
  const std::filesystem::path source = AssetFile(mod, path);
  const std::string_view folder = FolderOf(path);
  std::string name = std::string(NameOf(path)) + std::string(kCopySuffix);
  for (int number = 2; !NameProblem(mod, folder, name).empty(); ++number) {
    name = std::string(NameOf(path)) + std::string(kCopySuffix) + std::to_string(number);
    if (!ValidName(name)) {
      if (error) *error = "There's no free name for a copy of " + std::string(NameOf(path));
      return false;
    }
  }
  const std::filesystem::path target = AssetFile(mod, Join(folder, name));
  std::error_code code;
  std::filesystem::copy_file(source, target, code);
  if (code) {
    if (error) *error = "Couldn't copy " + std::string(path) + ": " + code.message();
    return false;
  }
  for (const std::filesystem::path& companion : Companions(source)) std::filesystem::copy_file(companion, target.parent_path() / CompanionName(companion, source, name), code);
  if (made) *made = Join(folder, name);
  return true;
}

bool Rename(const ModList& mods, std::string_view path, std::string_view name, std::string* error) {
  const ModInfo* mod = ModFor(mods, path, error);
  if (!mod) return false;
  if (InsideMod(path).empty()) {
    if (error) *error = "A mod is renamed in the mods menu";
    return false;
  }
  if (NameOf(path) == name) return true;
  if (std::string why = NameProblem(*mod, FolderOf(path), name, path); !why.empty()) {
    if (error) *error = std::move(why);
    return false;
  }
  return Relocate(mods, path, Join(FolderOf(path), name), error);
}

bool Move(const ModList& mods, std::string_view path, std::string_view folder, std::string* error) {
  const ModInfo* mod = ModFor(mods, path, error);
  if (!mod) return false;
  const auto fail = [error](std::string why) {
    if (error) *error = std::move(why);
    return false;
  };
  if (InsideMod(path).empty()) return fail("A mod can't be moved into a folder");
  if (!Same(ModOf(folder), ModOf(path))) return fail("Things move within their own mod; to use " + std::string(path) + " in another mod, make that mod depend on " + std::string(ModOf(path)));
  if (Within(folder, path)) return fail("A folder can't go inside itself");
  if (Same(folder, FolderOf(path))) return true;
  if (std::string why = NameProblem(*mod, folder, NameOf(path)); !why.empty()) return fail(std::move(why));
  return Relocate(mods, path, Join(folder, NameOf(path)), error);
}

bool Delete(const ModInfo& mod, std::string_view path, std::string* error) {
  if (InsideMod(path).empty()) {
    if (error) *error = "A mod is deleted from the mods folder";
    return false;
  }
  std::error_code code;
  const std::filesystem::path folder = FolderOnDisk(mod, path);
  if (std::filesystem::is_directory(folder, code)) {
    std::filesystem::remove_all(folder, code);
  } else {
    const std::filesystem::path file = AssetFile(mod, path);
    for (const std::filesystem::path& companion : Companions(file)) std::filesystem::remove(companion, code);
    std::filesystem::remove(file, code);
  }
  if (code && error) *error = "Couldn't delete " + std::string(path) + ": " + code.message();
  return !code;
}

std::vector<Reference> ReferencesTo(const ModList& mods, std::string_view path) {
  std::vector<Reference> found;
  const TypeLookup types = FileTypes(mods);
  const auto note = [&](const AssetInfo& info) {
    return [&, from = info.path](std::string& named, const std::string& where) {
      if (Within(named, path)) found.push_back({from, where});
    };
  };
  EditAssets(mods, {
      .on_class = [&](const AssetInfo& info, ClassAsset& asset) {
        if (!Within(info.path, path)) ForEachPath(mods, types, asset, note(info));
        return false;
      },
      .on_struct = [&](const AssetInfo& info, StructAsset& asset) {
        if (!Within(info.path, path)) ForEachPath(types, asset, note(info));
        return false;
      },
      .on_table = [&](const AssetInfo& info, TableAsset& asset) {
        if (!Within(info.path, path)) ForEachPath(types, asset, note(info));
        return false;
      },
      .on_map_edit = [&](const AssetInfo& info, MapEditAsset& asset) {
        if (!Within(info.path, path)) ForEachPath(mods, types, asset, note(info));
        return false;
      },
      .on_material = [&](const AssetInfo& info, MaterialAsset& asset) {
        if (!Within(info.path, path)) ForEachPath(types, asset, note(info));
        return false;
      },
      .on_packet = [&](const AssetInfo& info, PacketAsset& asset) {
        if (!Within(info.path, path)) ForEachPath(types, asset, note(info));
        return false;
      },
  });
  return found;
}

std::string RenameEnumValues(const ModList& mods, std::string_view enum_path, const std::map<std::string, std::string>& renames) {
  return RenameValues(mods, [&](const ValueType& single, nlohmann::json& value) {
    if (single.type != PropertyType::kEnum || !Same(single.of, enum_path) || !value.is_string()) return false;
    const auto renamed = renames.find(value.get<std::string>());
    if (renamed == renames.end()) return false;
    value = renamed->second;
    return true;
  });
}

std::string RenameStructFields(const ModList& mods, std::string_view struct_path, const std::map<std::string, std::string>& renames) {
  return RenameValues(mods, [&](const ValueType& single, nlohmann::json& value) {
    if (single.type != PropertyType::kStruct || !Same(single.of, struct_path) || !value.is_object()) return false;
    bool changed = false;
    // Out of the way first, so swapped names don't clobber each other.
    nlohmann::json moved = nlohmann::json::object();
    for (const auto& [from, to] : renames) {
      const auto held = value.find(from);
      if (held == value.end()) continue;
      moved[to] = std::move(*held);
      value.erase(from);
      changed = true;
    }
    for (auto& [name, held] : moved.items()) value[name] = std::move(held);
    return changed;
  });
}

}  // namespace rexfabric::content
