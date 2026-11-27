#include "types.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <shared_mutex>

#include <rexfabric/game_api.h>

#include "../log.h"
#include "assets.h"
#include "classes.h"

namespace rexfabric::content {
namespace {

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

struct Registry {
  std::shared_mutex mutex;
  std::vector<Struct> structs;  // the game's, then the mods'
  size_t declared = 0;
  std::vector<Enum> enums;  // the mods'; the game's are its API's

  const Struct* FindStructLocked(std::string_view path) const {
    const auto found = std::find_if(structs.begin(), structs.end(), [&path](const Struct& known) { return Lower(known.path) == Lower(path); });
    return found != structs.end() ? &*found : nullptr;
  }

  const Enum* FindModEnumLocked(std::string_view path) const {
    const auto found = std::find_if(enums.begin(), enums.end(), [&path](const Enum& known) { return Lower(known.path) == Lower(path); });
    return found != enums.end() ? &*found : nullptr;
  }
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

// A game enum by its Lua name, as content sees it: each value with its number.
std::optional<Enum> GameEnum(std::string_view name) {
  for (const auto& space : Namespaces()) {
    for (const EnumDef& definition : space->enums) {
      if (definition.name != name) continue;
      Enum found{.path = definition.name};
      for (const auto& [value, number] : definition.values) {
        found.values.push_back({.name = value});
        found.numbers.push_back(number);
      }
      return found;
    }
  }
  return std::nullopt;
}

// The registry's own lookups, for use while its lock is held.
TypeLookup LockedLookup(const Registry& registry) {
  return {
      .find_enum = [&registry](std::string_view path) -> std::optional<Enum> {
        if (std::optional<Enum> game = GameEnum(path)) return game;
        const Enum* found = registry.FindModEnumLocked(path);
        return found ? std::optional<Enum>(*found) : std::nullopt;
      },
      .find_struct = [&registry](std::string_view path) -> std::optional<Struct> {
        const Struct* found = registry.FindStructLocked(path);
        return found ? std::optional<Struct>(*found) : std::nullopt;
      },
  };
}

// The structs a field's type holds, which have to be there first.
std::vector<std::string> HeldStructs(const std::vector<Property>& fields) {
  std::vector<std::string> held;
  for (const Property& field : fields) {
    if (field.type.type == PropertyType::kStruct) held.push_back(field.type.of);
  }
  return held;
}

bool ValidPath(std::string_view path, std::string* why) {
  size_t start = 0;
  for (size_t slash = 0; slash != std::string_view::npos; start = slash + 1) {
    slash = path.find('/', start);
    if (!ValidName(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start), why)) return false;
  }
  return true;
}

}  // namespace

bool CheckDeclared(std::vector<Property>& declared, const TypeLookup& types, const std::function<bool(std::string_view path, std::string* why)>& may_use, std::string* why) {
  const auto fail = [why](const Property& at, const std::string& reason) {
    if (why) *why = at.name + ": " + reason;
    return false;
  };
  std::string reason;
  for (size_t i = 0; i < declared.size(); ++i) {
    Property& at = declared[i];
    if (!ValidName(at.name, &reason)) return fail(at, reason);
    if (std::any_of(declared.begin(), declared.begin() + static_cast<ptrdiff_t>(i), [&at](const Property& other) { return Lower(other.name) == Lower(at.name); })) return fail(at, "the name is there twice");
    // What its type names: there, and of a mod it may use.
    const ValueType& type = at.type;
    if (type.type == PropertyType::kEnum && !types.find_enum(type.of)) return fail(at, "there's no enum " + type.of);
    if (type.type == PropertyType::kStruct && !types.find_struct(type.of)) return fail(at, "there's no struct " + type.of);
    if (type.container == Container::kMap && !CanBeKey(type.key)) return fail(at, "a map's key is a bool, an int, a string, an enum or an asset");
    if (type.container == Container::kMap && type.key == PropertyType::kEnum && !types.find_enum(type.key_of)) return fail(at, "there's no enum " + type.key_of);
    for (const std::string* named : {&type.of, &type.key_of}) {
      if (!named->empty() && !may_use(*named, &reason)) return fail(at, reason);
    }
    nlohmann::json fitted;
    const nlohmann::json given = at.value.is_null() && !(type.type == PropertyType::kAsset && type.container == Container::kSingle) ? EmptyValue(types, type) : at.value;
    if (!FitValue(types, type, given, &fitted, &reason)) return fail(at, reason);
    bool allowed = true;
    WalkValues(types, type, fitted, [&](const ValueType& single, nlohmann::json& value) {
      if (allowed && single.type == PropertyType::kAsset && value.is_string()) allowed = may_use(value.get<std::string>(), &reason);
    });
    if (!allowed) return fail(at, reason);
    at.value = std::move(fitted);
  }
  return true;
}

bool DeclareStruct(StructDecl decl) {
  Registry& registry = Get();
  std::unique_lock lock(registry.mutex);
  std::string why;
  if (!ValidPath(decl.path, &why)) {
    RF_ERROR("content: the game's struct {}: {}", decl.path, why);
    return false;
  }
  if (registry.FindStructLocked(decl.path) || FindClass(decl.path)) {
    RF_ERROR("content: the game's struct {} takes a path that's taken", decl.path);
    return false;
  }
  Struct added{.path = std::move(decl.path), .help = std::move(decl.help)};
  for (PropertyDecl& field : decl.fields) added.fields.push_back({.name = std::move(field.name), .type = std::move(field.type), .value = ToJson(field.value), .help = std::move(field.help)});
  const auto anything = [](std::string_view, std::string*) { return true; };
  if (!CheckDeclared(added.fields, LockedLookup(registry), anything, &why)) {
    RF_ERROR("content: the game's struct {}: {}", added.path, why);
    return false;
  }
  registry.structs.insert(registry.structs.begin() + static_cast<ptrdiff_t>(registry.declared), std::move(added));
  ++registry.declared;
  return true;
}

void LoadModTypes(const std::vector<ModInfo>& mods) {
  Registry& registry = Get();
  std::unique_lock lock(registry.mutex);
  registry.structs.resize(registry.declared);
  registry.enums.clear();
  const TypeLookup lookup = LockedLookup(registry);

  struct PendingStruct {
    std::string mod;
    Struct added;
  };
  std::vector<PendingStruct> pending;
  std::string why;
  for (const ModInfo& mod : mods) {
    if (!ValidName(mod.folder)) continue;  // the classes loader says why
    for (const AssetInfo& asset : ScanContent(mod).assets) {
      if (asset.kind == AssetKind::kEnum) {
        std::optional<EnumAsset> read = ReadEnumAsset(asset.file, &why);
        if (!read) {
          RF_WARN("content: [{}] {}: {}", mod.folder, asset.path, why);
          continue;
        }
        const auto bad = std::find_if(read->values.begin(), read->values.end(), [&](const EnumValue& value) { return !ValidName(value.name, &why); });
        if (bad != read->values.end()) {
          RF_WARN("content: [{}] {}: value {}: {}", mod.folder, asset.path, bad->name, why);
          continue;
        }
        registry.enums.push_back({.path = asset.path, .mod = mod.folder, .help = std::move(read->help), .values = std::move(read->values)});
      } else if (asset.kind == AssetKind::kStruct) {
        std::optional<StructAsset> read = ReadStructAsset(asset.file, &why);
        if (!read) {
          RF_WARN("content: [{}] {}: {}", mod.folder, asset.path, why);
          continue;
        }
        pending.push_back({mod.folder, {.path = asset.path, .mod = mod.folder, .help = std::move(read->help), .fields = std::move(read->fields)}});
      }
    }
  }
  // A struct waits for the structs it holds, which may come later; what never fits holds itself, or one that isn't.
  for (bool progress = true; progress && !pending.empty();) {
    progress = false;
    for (auto it = pending.begin(); it != pending.end();) {
      const std::vector<std::string> held = HeldStructs(it->added.fields);
      const bool ready = std::all_of(held.begin(), held.end(), [&](const std::string& path) { return registry.FindStructLocked(path) != nullptr; });
      if (!ready) {
        ++it;
        continue;
      }
      const std::string mod = it->mod;
      const auto may_use = [&mod](std::string_view path, std::string* reason) { return MayReference(mod, path, reason); };
      if (CheckDeclared(it->added.fields, lookup, may_use, &why)) registry.structs.push_back(std::move(it->added));
      else RF_WARN("content: [{}] {}: field {}", mod, it->added.path, why);
      it = pending.erase(it);
      progress = true;
    }
  }
  for (const PendingStruct& left : pending) RF_WARN("content: [{}] {}: it holds a struct that isn't there, or holds itself", left.mod, left.added.path);
  RF_INFO("content: {} enums and {} structs from mods", registry.enums.size(), registry.structs.size() - registry.declared);
}

std::optional<Enum> FindEnum(std::string_view path) {
  if (std::optional<Enum> game = GameEnum(path)) return game;
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const Enum* found = registry.FindModEnumLocked(path);
  return found ? std::optional<Enum>(*found) : std::nullopt;
}

std::optional<Struct> FindStruct(std::string_view path) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const Struct* found = registry.FindStructLocked(path);
  return found ? std::optional<Struct>(*found) : std::nullopt;
}

std::vector<std::string> EnumPaths(std::string_view from) {
  std::vector<std::string> paths;
  for (const auto& space : Namespaces()) {
    for (const EnumDef& definition : space->enums) paths.push_back(definition.name);
  }
  std::vector<std::string> mods;
  {
    Registry& registry = Get();
    std::shared_lock lock(registry.mutex);
    for (const Enum& known : registry.enums) mods.push_back(known.path);
  }
  for (std::string& path : mods) {
    if (MayReference(from, path, nullptr)) paths.push_back(std::move(path));
  }
  return paths;
}

std::vector<std::string> StructPaths(std::string_view from) {
  std::vector<std::string> paths;
  {
    Registry& registry = Get();
    std::shared_lock lock(registry.mutex);
    for (const Struct& known : registry.structs) paths.push_back(known.path);
  }
  std::erase_if(paths, [&from](const std::string& path) { return !MayReference(from, path, nullptr); });
  return paths;
}

const TypeLookup& LoadedTypes() {
  static const TypeLookup lookup{.find_enum = FindEnum, .find_struct = FindStruct};
  return lookup;
}

}  // namespace rexfabric::content
