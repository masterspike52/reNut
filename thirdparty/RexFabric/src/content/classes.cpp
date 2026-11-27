#include "classes.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "../log.h"
#include "assets.h"
#include "game_texts.h"
#include "localisation.h"
#include "map_edits.h"
#include "materials.h"
#include "packets.h"
#include "tables.h"
#include "types.h"

namespace rexfabric::content {
namespace {

constexpr std::string_view kObject = "Object";
constexpr std::string_view kTexture = "Texture";
constexpr std::string_view kMapEdit = "MapEdit";
constexpr std::string_view kMaterial = "Material";
constexpr std::string_view kMesh = "Mesh";
constexpr std::string_view kWorld = "World";
constexpr std::string_view kPacket = "Packet";
constexpr size_t kMaxName = 64;
// What kind of asset a file is, under "asset". Only classes so far.
constexpr std::string_view kAssetClass = "class";

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

// The mod folder a path starts with ("MyMod" of "MyMod/Garden/Rock"), or the whole path.
std::string_view FirstPart(std::string_view path) { return path.substr(0, path.find('/')); }

struct Registry {
  std::shared_mutex mutex;
  std::vector<Class> classes;
  std::unordered_map<std::string, size_t> by_path;  // lower case path -> index
  size_t declared = 0;  // the classes before this are Object's and the game's
  std::unordered_map<std::string, std::unordered_set<std::string>> dependencies;  // lower case mod -> its dependencies
  std::unordered_map<std::string, std::filesystem::path> mod_dirs;  // lower case mod -> its folder, for the loaded ones

  Registry() {
    Class object;
    object.path = std::string(kObject);
    object.help = "The root of every class";
    object.properties.push_back({.name = "thumbnail", .type = {.type = PropertyType::kAsset}, .value = nullptr, .help = "What the content browser shows for it; its class's icon when unset", .declared_by = std::string(kObject)});
    Add(std::move(object));
    // The class of the pictures mods import: its assets are texture files, an asset property of it picks one.
    Class texture;
    texture.path = std::string(kTexture);
    texture.parent = std::string(kObject);
    texture.help = "A picture a mod imports in the content browser";
    Add(std::move(texture));
    // The class of the changes mods make to the game's worlds as they're entered: its assets are map edit files.
    Class map_edit;
    map_edit.path = std::string(kMapEdit);
    map_edit.parent = std::string(kObject);
    map_edit.help = "Changes to one of the game's worlds, made each time it's entered: avatars spawned in it, and what the game's own kinds of edit do";
    Add(std::move(map_edit));
    // The class of mods' materials: its assets are material files, of one of the game's material types.
    Class material;
    material.path = std::string(kMaterial);
    material.parent = std::string(kObject);
    material.help = "How something the game draws looks: textures, colours and the rest its material type offers";
    Add(std::move(material));
    // The class of the 3D models mods import: its assets are mesh files, each a model beside it.
    Class mesh;
    mesh.path = std::string(kMesh);
    mesh.parent = std::string(kObject);
    mesh.help = "A 3D model a mod imports in the content browser, which the game draws";
    Add(std::move(mesh));
    // The class of mods' worlds: its assets are world files, each one of the game's worlds with map edits of its own.
    Class world;
    world.path = std::string(kWorld);
    world.parent = std::string(kObject);
    world.help = "One of the game's worlds as a mod changes it: its base, with this world's map edits besides its own";
    Add(std::move(world));
    // The class of mods' packets: its assets are packet files, each a kind of ground that pours as one of the game's.
    Class packet;
    packet.path = std::string(kPacket);
    packet.parent = std::string(kObject);
    packet.help = "A kind of ground a player buys and pours: a variant of one of the game's own packets, its base, that looks and sells as it says";
    Add(std::move(packet));
    declared = classes.size();
  }

  void Add(Class added) {
    by_path[Lower(added.path)] = classes.size();
    classes.push_back(std::move(added));
  }

  const Class* Find(std::string_view path) const {
    const auto it = by_path.find(Lower(path));
    return it != by_path.end() ? &classes[it->second] : nullptr;
  }

  // The class an object type names: a class by its path, or the class standing for a game type.
  const Class* Resolve(std::string_view type) const {
    if (const Class* found = Find(type)) return found;
    const auto standing = std::find_if(classes.begin(), classes.end(), [type](const Class& known) { return !known.type.empty() && known.type == type; });
    return standing != classes.end() ? &*standing : nullptr;
  }

  // The backing a mod class below `path`, or `path` itself when it's a mod's, gets: the nearest mod class's that names
  // one, or the game's first for the game class above them.
  std::string BackingFrom(std::string_view path) const {
    for (const Class* at : Chain(path)) {
      if (!at->mod.empty() && !at->backing.empty()) return at->backing;
      if (at->mod.empty()) {
        const std::vector<std::string> offered = BackingsFor(at->path);
        return offered.empty() ? std::string() : offered.front();
      }
    }
    return {};
  }

  // A class's backing: none for a game class, which has no avatars of its own.
  std::string BackingOf(std::string_view path) const {
    const Class* found = Find(path);
    return found && !found->mod.empty() ? BackingFrom(path) : std::string();
  }

  // The class and its ancestors, nearest first.
  std::vector<const Class*> Chain(std::string_view path) const {
    std::vector<const Class*> chain;
    for (const Class* at = Find(path); at && chain.size() <= classes.size(); at = at->parent.empty() ? nullptr : Find(at->parent)) chain.push_back(at);
    return chain;
  }

  const Property* FindProperty(std::string_view path, std::string_view name) const {
    for (const Class* at : Chain(path)) {
      for (const Property& property : at->properties) {
        if (Lower(property.name) == Lower(name)) return &property;
      }
    }
    return nullptr;
  }
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

bool MayReferenceLocked(const Registry& registry, std::string_view from, std::string_view path, std::string* why) {
  // A name without a folder is RexFabric's or the game's: Object, a game type or enum ("tip.Weather").
  if (from.empty() || path.find('/') == std::string_view::npos) return true;
  const Class* target = registry.Find(path);
  if (target && target->mod.empty()) return true;  // RexFabric's or the game's
  const std::string mod = Lower(FirstPart(path));
  // Under a folder of the game's ("TiP/Nothing"), a path no class has is the caller's to find missing, not a mod's.
  const auto game_folder = [&mod](const Class& known) { return known.mod.empty() && Lower(FirstPart(known.path)) == mod; };
  if (!target && std::any_of(registry.classes.begin(), registry.classes.begin() + static_cast<ptrdiff_t>(registry.declared), game_folder)) return true;
  if (mod == Lower(from)) return true;
  const auto deps = registry.dependencies.find(Lower(from));
  if (deps != registry.dependencies.end() && deps->second.contains(mod)) return true;
  if (why) *why = std::string(from) + " doesn't depend on " + std::string(FirstPart(path)) + ", so it can't use " + std::string(path);
  return false;
}

// A class asset as read from its file, before its parent is known.
struct Pending {
  Class added;
  nlohmann::json properties = nlohmann::json::array();
  nlohmann::json defaults = nlohmann::json::object();
  nlohmann::json components = nlohmann::json::array();
};

std::optional<Pending> ReadClassFile(const ModInfo& mod, const std::filesystem::path& file, std::string path) {
  std::ifstream in(file, std::ios::binary);
  const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
  if (!json.is_object()) {
    RF_WARN("content: [{}] {}: not a JSON object, skipped", mod.folder, path);
    return std::nullopt;
  }
  const auto asset = json.find("asset");
  if (asset == json.end() || !asset->is_string() || asset->get<std::string>() != kAssetClass) return std::nullopt;  // another kind of asset
  Pending pending;
  pending.added.path = std::move(path);
  pending.added.mod = mod.folder;
  pending.added.file = file;
  pending.added.parent = json.value("parent", std::string(kObject));
  pending.added.help = json.value("help", std::string());
  pending.properties = json.value("properties", nlohmann::json::array());
  pending.defaults = json.value("defaults", nlohmann::json::object());
  pending.added.backing = json.value("backing", std::string());
  pending.components = json.value("components", nlohmann::json::array());
  return pending;
}

// A mod class's backing and components, checked: a backing the game offers for the game class above it, and components
// whose names its ancestors' don't take, on a class that has a backing. Under the lock.
bool AddAvatarParts(const Registry& registry, Pending& pending, std::string* why) {
  Class& added = pending.added;
  std::optional<std::vector<Component>> components = ReadComponents(pending.components, why);
  if (!components) return false;
  if (added.mod.empty()) return true;
  if (!added.backing.empty()) {
    const std::shared_ptr<const BackingDecl> backing = FindBacking(added.backing);
    if (!backing) {
      *why = "the game has no backing " + added.backing;
      return false;
    }
    const std::vector<const Class*> chain = registry.Chain(added.parent);
    const auto game = std::find_if(chain.begin(), chain.end(), [](const Class* at) { return at->mod.empty(); });
    if (game == chain.end() || Lower((*game)->path) != Lower(backing->for_class)) {
      *why = "its backing " + backing->path + " is for classes below " + backing->for_class + ", and it's below " + (game != chain.end() ? (*game)->path : std::string("no game class"));
      return false;
    }
    added.backing = backing->path;
  }
  if (!components->empty() && added.backing.empty() && registry.BackingFrom(added.parent).empty()) {
    *why = "it has components, but it isn't an avatar class: nothing above it has a backing";
    return false;
  }
  std::vector<std::string> taken;
  for (const Class* at : registry.Chain(added.parent)) {
    for (const Component& inherited : at->components) {
      const std::function<void(const Component&)> collect = [&](const Component& component) {
        taken.push_back(component.name);
        for (const Component& child : component.children) collect(child);
      };
      collect(inherited);
    }
  }
  if (!CheckComponents(*components, taken, why)) return false;
  added.components = std::move(*components);
  return true;
}

// Adds a class whose parent is in, checking its properties and defaults against its ancestors'. Under the lock.
bool AddPending(Registry& registry, Pending& pending) {
  Class& added = pending.added;
  // Who the log names: the mod, or the game for one of its own classes.
  const std::string who = added.mod.empty() ? std::string("the game") : added.mod;
  std::string why;
  if (!MayReferenceLocked(registry, added.mod, added.parent, &why)) {
    RF_WARN("content: [{}] {}: {}", who, added.path, why);
    return false;
  }
  if (!pending.properties.is_array() || !pending.defaults.is_object()) {
    RF_WARN("content: [{}] {}: \"properties\" is a list and \"defaults\" an object", who, added.path);
    return false;
  }
  for (const nlohmann::json& entry : pending.properties) {
    std::optional<ValueType> type = ReadType(entry, &why);
    if (!type) {
      RF_WARN("content: [{}] {}: property {}: {}", who, added.path, entry.dump(), why);
      return false;
    }
    added.properties.push_back({.name = entry.value("name", std::string()), .type = std::move(*type), .value = entry.value("default", nlohmann::json()), .help = entry.value("help", std::string()), .declared_by = added.path});
  }
  const auto may_use = [&](std::string_view path, std::string* reason) { return MayReferenceLocked(registry, added.mod, path, reason); };
  if (!CheckDeclared(added.properties, LoadedTypes(), may_use, &why)) {
    RF_WARN("content: [{}] {}: property {}", who, added.path, why);
    return false;
  }
  for (const Property& property : added.properties) {
    if (!registry.FindProperty(added.parent, property.name)) continue;
    RF_WARN("content: [{}] {}: property {} is inherited already", who, added.path, property.name);
    return false;
  }
  for (const auto& [name, value] : pending.defaults.items()) {
    const Property* inherited = registry.FindProperty(added.parent, name);
    if (!inherited) {
      RF_WARN("content: [{}] {}: default for {}, which {} doesn't have", who, added.path, name, added.parent);
      return false;
    }
    nlohmann::json checked;
    if (!FitValue(LoadedTypes(), inherited->type, value, &checked, &why)) {
      RF_WARN("content: [{}] {}: default for {}: {}", who, added.path, name, why);
      return false;
    }
    bool allowed = true;
    WalkValues(LoadedTypes(), inherited->type, checked, [&](const ValueType& single, nlohmann::json& held) {
      if (allowed && single.type == PropertyType::kAsset && held.is_string()) allowed = MayReferenceLocked(registry, added.mod, held.get<std::string>(), &why);
    });
    if (!allowed) {
      RF_WARN("content: [{}] {}: default for {}: {}", who, added.path, name, why);
      return false;
    }
    added.defaults[inherited->name] = std::move(checked);
  }
  if (!AddAvatarParts(registry, pending, &why)) {
    RF_WARN("content: [{}] {}: {}", who, added.path, why);
    return false;
  }
  registry.Add(std::move(added));
  return true;
}

// Every class asset under the mod's content folder, with its path; bad names are logged and skipped.
std::vector<Pending> ReadModClasses(const ModInfo& mod) {
  std::vector<Pending> found;
  std::error_code error;
  const std::filesystem::path root = mod.dir / kContentFolder;
  if (!std::filesystem::is_directory(root, error)) return found;
  std::string why;
  if (!ValidName(mod.folder, &why)) {
    RF_WARN("content: [{}] the mod's folder name can't name its content ({}), so its content isn't loaded", mod.folder, why);
    return found;
  }
  std::vector<std::filesystem::path> files;
  for (auto it = std::filesystem::recursive_directory_iterator(root, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    const std::string name = it->path().filename().string();
    if (it->is_directory(error)) {
      if (!ValidName(name, &why)) {
        RF_WARN("content: [{}] folder {} skipped: {}", mod.folder, name, why);
        it.disable_recursion_pending();
      }
      continue;
    }
    // Other files (pictures, a class's graph) belong to the assets beside them.
    if (Lower(it->path().extension().string()) == kAssetExtension && it->path().stem().extension().empty()) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  for (const std::filesystem::path& file : files) {
    const std::string name = file.stem().string();
    if (!ValidName(name, &why)) {
      RF_WARN("content: [{}] {} skipped: {}", mod.folder, file.filename().string(), why);
      continue;
    }
    std::string path = mod.folder;
    for (const auto& part : std::filesystem::relative(file.parent_path(), root, error)) {
      if (part != ".") path += "/" + part.string();
    }
    path += "/" + name;
    if (auto pending = ReadClassFile(mod, file, std::move(path))) found.push_back(std::move(*pending));
  }
  return found;
}

}  // namespace

bool ValidName(std::string_view name, std::string* why) {
  const auto fail = [why](std::string reason) {
    if (why) *why = std::move(reason);
    return false;
  };
  if (name.empty()) return fail("a name can't be empty");
  if (name.size() > kMaxName) return fail("a name is at most 64 characters");
  if (!std::isalpha(static_cast<unsigned char>(name.front()))) return fail("a name starts with a letter");
  const auto bad = std::find_if(name.begin(), name.end(), [](unsigned char c) { return !(c < 0x80 && (std::isalnum(c) || c == '_')); });
  if (bad != name.end()) return fail("a name has only letters, digits and _, not '" + std::string(1, *bad) + "'");
  return true;
}

bool DeclareClass(ClassDecl decl) {
  Registry& registry = Get();
  std::unique_lock lock(registry.mutex);
  std::string why;
  const std::string& path = decl.path;
  size_t start = 0;
  for (size_t slash = 0; slash != std::string::npos; start = slash + 1) {
    slash = path.find('/', start);
    if (!ValidName(std::string_view(path).substr(start, slash == std::string::npos ? std::string::npos : slash - start), &why)) {
      RF_ERROR("content: the game's class {}: {}", path, why);
      return false;
    }
  }
  if (registry.Find(path)) {
    RF_ERROR("content: the game's class {} is declared twice", path);
    return false;
  }
  if (!registry.Find(decl.parent)) {
    RF_ERROR("content: the game's class {} has an unknown parent {}", path, decl.parent);
    return false;
  }
  if (!decl.type.empty() && !FindType(decl.type)) {
    RF_ERROR("content: the game's class {} stands for {}, which isn't a type of the game's", path, decl.type);
    return false;
  }
  if (const Class* taken = decl.type.empty() ? nullptr : registry.Resolve(decl.type)) {
    RF_ERROR("content: the game's class {} stands for {}, which {} stands for already", path, decl.type, taken->path);
    return false;
  }
  Pending pending;
  pending.added.path = std::move(decl.path);
  pending.added.parent = std::move(decl.parent);
  pending.added.help = std::move(decl.help);
  pending.added.type = std::move(decl.type);
  pending.added.test = std::move(decl.test);
  for (const PropertyDecl& property : decl.properties) {
    nlohmann::ordered_json entry{{"name", property.name}};
    WriteType(property.type, entry);
    entry["default"] = ToJson(property.value);
    entry["help"] = property.help;
    pending.properties.push_back(nlohmann::json::parse(entry.dump()));
  }
  // Mod classes come after the game's; a declaration after mods loaded goes before them.
  std::vector<Class> mods(registry.classes.begin() + static_cast<ptrdiff_t>(registry.declared), registry.classes.end());
  registry.classes.resize(registry.declared);
  registry.by_path.clear();
  for (size_t i = 0; i < registry.classes.size(); ++i) registry.by_path[Lower(registry.classes[i].path)] = i;
  const bool added = AddPending(registry, pending);
  registry.declared = registry.classes.size();
  for (Class& mod_class : mods) registry.Add(std::move(mod_class));
  return added;
}

void LoadModContent(const std::vector<ModInfo>& mods) {
  Registry& registry = Get();
  {
    std::unique_lock lock(registry.mutex);
    registry.classes.resize(registry.declared);
    registry.by_path.clear();
    for (size_t i = 0; i < registry.classes.size(); ++i) registry.by_path[Lower(registry.classes[i].path)] = i;
    registry.dependencies.clear();
    registry.mod_dirs.clear();
    for (const ModInfo& mod : mods) {
      registry.mod_dirs[Lower(mod.folder)] = mod.dir;
      auto& deps = registry.dependencies[Lower(mod.folder)];
      for (const ModDependency& dependency : mod.dependencies) deps.insert(Lower(dependency.mod));
    }
  }
  // The enums and structs first, then the tables of them, outside the lock, as their loaders ask who may use what.
  LoadModTypes(mods);
  LoadModTables(mods);
  LoadTranslations(mods);
  LoadLanguageAssets(mods);
  std::unique_lock lock(registry.mutex);
  for (const ModInfo& mod : mods) {
    std::vector<Pending> pending = ReadModClasses(mod);
    // A class waits for its parent, which may come later in the same mod; what never fits is reported.
    for (bool progress = true; progress && !pending.empty();) {
      progress = false;
      for (auto it = pending.begin(); it != pending.end();) {
        if (registry.Find(it->added.path)) {
          RF_WARN("content: [{}] {}: the path is taken", mod.folder, it->added.path);
          it = pending.erase(it);
          continue;
        }
        if (!registry.Find(it->added.parent)) {
          ++it;
          continue;
        }
        AddPending(registry, *it);
        it = pending.erase(it);
        progress = true;
      }
    }
    for (const Pending& left : pending) RF_WARN("content: [{}] {}: its parent {} isn't a class (or its parents go round in a circle)", mod.folder, left.added.path, left.added.parent);
  }
  RF_INFO("content: {} classes, {} of them from mods", registry.classes.size(), registry.classes.size() - registry.declared);
  lock.unlock();
  LoadMaterials(mods);
  LoadPackets(mods);
  // Last, as their spawns name the classes.
  LoadMapEdits(mods);
}

std::optional<std::filesystem::path> LoadedModDir(std::string_view folder) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const auto found = registry.mod_dirs.find(Lower(folder));
  return found != registry.mod_dirs.end() ? std::optional<std::filesystem::path>(found->second) : std::nullopt;
}

std::optional<Class> FindClass(std::string_view path) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const Class* found = registry.Find(path);
  return found ? std::optional<Class>(*found) : std::nullopt;
}

std::vector<std::string> ClassPaths() {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  std::vector<std::string> paths;
  for (const Class& known : registry.classes) paths.push_back(known.path);
  return paths;
}

bool IsA(std::string_view path, std::string_view base) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const std::vector<const Class*> chain = registry.Chain(path);
  return std::any_of(chain.begin(), chain.end(), [&](const Class* at) { return Lower(at->path) == Lower(base); });
}

std::vector<Property> Properties(std::string_view path) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  std::vector<const Class*> chain = registry.Chain(path);
  std::vector<Property> properties;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (const Property& property : (*it)->properties) properties.push_back(property);
  }
  // Each with the nearest value along the chain, the class's own first, and the class it's from.
  for (Property& property : properties) {
    property.value_from = property.declared_by;
    for (const Class* at : chain) {
      if (const auto set = at->defaults.find(property.name); set != at->defaults.end()) {
        property.value = *set;
        property.value_from = at->path;
        break;
      }
      if (at->path == property.declared_by) break;
    }
  }
  return properties;
}

std::optional<nlohmann::json> Default(std::string_view path, std::string_view property) {
  for (const Property& known : Properties(path)) {
    if (Lower(known.name) == Lower(property)) return known.value;
  }
  return std::nullopt;
}

std::string BackingOf(std::string_view path) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  return registry.BackingOf(path);
}

std::vector<Component> ComponentsOf(std::string_view path) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const std::vector<const Class*> chain = registry.Chain(path);
  std::vector<Component> components;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) components.insert(components.end(), (*it)->components.begin(), (*it)->components.end());
  return components;
}

std::vector<std::string> AvatarClassPaths(std::string_view from) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  std::vector<std::string> paths;
  for (const Class& known : registry.classes) {
    if (!known.mod.empty() && !registry.BackingOf(known.path).empty() && MayReferenceLocked(registry, from, known.path, nullptr)) paths.push_back(known.path);
  }
  return paths;
}

bool MayReference(std::string_view from, std::string_view path, std::string* why) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  return MayReferenceLocked(registry, from, path, why);
}

std::string TypeOfClass(std::string_view path) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  for (const Class* at : registry.Chain(path)) {
    if (!at->type.empty()) return at->type;
  }
  return {};
}

bool IsType(std::string_view name) {
  if (FindType(name)) return true;
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  return registry.Find(name) != nullptr;
}

bool TypeIsA(std::string_view from, std::string_view to) {
  if (from == to) return true;
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const Class* base = registry.Resolve(to);
  const Class* below = registry.Resolve(from);
  if (!base || !below) return false;
  const std::vector<const Class*> chain = registry.Chain(below->path);
  return std::find(chain.begin(), chain.end(), base) != chain.end();
}

std::vector<std::string> TypeNames(std::string_view from) {
  std::vector<std::string> names;
  for (const auto& space : Namespaces()) {
    for (const auto& type : space->types) names.push_back(type->name());
  }
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  for (const Class& known : registry.classes) {
    if (known.path != kObject && known.type.empty() && MayReferenceLocked(registry, from, known.path, nullptr)) names.push_back(known.path);
  }
  return names;
}

bool IsOfClass(std::string_view path, const Object& object) {
  // The tests are copied out first: one calls into the game, which mustn't wait on the registry.
  std::vector<ClassTest> tests;
  {
    Registry& registry = Get();
    std::shared_lock lock(registry.mutex);
    // A class with no type above it (a data class) has no objects either.
    bool typed = false;
    for (const Class* at : registry.Chain(path)) {
      if (!at->mod.empty()) return false;
      if ((typed = !at->type.empty())) break;
      if (at->test) tests.push_back(at->test);
    }
    if (!typed) return false;
  }
  // From the type down, so a class's test only sees objects its parent's passed.
  return std::all_of(tests.rbegin(), tests.rend(), [&object](const ClassTest& test) { return test(object); });
}

}  // namespace rexfabric::content
