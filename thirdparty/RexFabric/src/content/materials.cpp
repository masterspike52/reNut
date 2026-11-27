#include "materials.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

#include "../log.h"
#include "material_graph.h"
#include "values.h"

namespace rexfabric::content {
namespace {

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

bool ValidPath(std::string_view path, std::string* why) {
  size_t start = 0;
  for (size_t slash = 0; slash != std::string_view::npos; start = slash + 1) {
    slash = path.find('/', start);
    if (!ValidName(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start), why)) return false;
  }
  return true;
}

struct Registry {
  std::mutex mutex;
  std::vector<std::shared_ptr<const MaterialType>> types;
  std::map<std::string, MaterialAsset> loaded;    // by lower case path
  std::map<std::string, MaterialAsset> previews;  // the editors' unsaved ones, by lower case path
  uint64_t version = 1;  // goes up with every load and every changed preview
  // The graphs compiled, by the material's path, each with the graph it was compiled from.
  std::map<std::string, std::pair<nlohmann::json, std::shared_ptr<const MaterialProgram>>> programs;
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

// The view of a material: each of its type's properties with its value or the default, singles and maps apart.
MaterialView View(const MaterialAsset& asset, const MaterialType* type, uint64_t version) {
  MaterialView view{.type = asset.type, .version = version};
  if (!type) return view;
  for (const Property& property : type->properties) {
    const nlohmann::json* set = nullptr;
    for (const auto& [name, value] : asset.values.items()) {
      if (Same(name, property.name)) set = &value;
    }
    const nlohmann::json& value = set ? *set : property.value;
    if (property.type.container == Container::kMap) {
      // A map is a list of [key, value] pairs; a key that isn't a string goes by its JSON.
      std::map<std::string, PropertyValue>& entries = view.maps[property.name];
      if (value.is_array()) {
        for (const nlohmann::json& pair : value) {
          if (!pair.is_array() || pair.size() != 2) continue;
          entries[pair[0].is_string() ? pair[0].get<std::string>() : pair[0].dump()] = ToPropertyValue(pair[1]);
        }
      }
    } else if (property.type.container == Container::kSingle) {
      view.values[property.name] = ToPropertyValue(value);
    }
  }
  return view;
}

}  // namespace

bool DeclareMaterialType(MaterialTypeDecl decl) {
  std::string why;
  if (!ValidPath(decl.path, &why)) {
    RF_ERROR("content: the game's material type {}: {}", decl.path, why);
    return false;
  }
  auto type = std::make_shared<MaterialType>(MaterialType{.path = std::move(decl.path), .help = std::move(decl.help), .preview_component = std::move(decl.preview_component), .preview_values = std::move(decl.preview_values), .preview_material = std::move(decl.preview_material), .preview_choice = std::move(decl.preview_choice), .graph_inputs = std::move(decl.graph_inputs), .shading_models = std::move(decl.shading_models), .graph_nodes = std::move(decl.graph_nodes), .game_textures = std::move(decl.game_textures), .check_program = std::move(decl.check_program)});
  for (PropertyDecl& property : decl.properties) type->properties.push_back({.name = std::move(property.name), .type = std::move(property.type), .value = ToJson(property.value), .help = std::move(property.help)});
  // A container property with no default starts empty rather than none: a map as a list of pairs, as files keep it.
  for (Property& property : type->properties) {
    if (property.type.container != Container::kSingle && property.value.is_null()) property.value = nlohmann::json::array();
  }
  const auto anything = [](std::string_view, std::string*) { return true; };
  if (!CheckDeclared(type->properties, LoadedTypes(), anything, &why)) {
    RF_ERROR("content: the game's material type {}: {}", type->path, why);
    return false;
  }
  if (!material::CheckDeclared(*type, &why)) {
    RF_ERROR("content: the game's material type {}: {}", type->path, why);
    return false;
  }
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (std::any_of(registry.types.begin(), registry.types.end(), [&type](const auto& known) { return Same(known->path, type->path); })) {
    RF_ERROR("content: the game's material type {} is declared twice", type->path);
    return false;
  }
  registry.types.push_back(std::move(type));
  return true;
}

const ShadingModelDecl* MaterialType::FindShadingModel(std::string_view name) const {
  const auto found = std::find_if(shading_models.begin(), shading_models.end(), [name](const ShadingModelDecl& model) { return Same(model.name, name); });
  return found != shading_models.end() ? &*found : nullptr;
}

std::shared_ptr<const MaterialType> FindMaterialType(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto found = std::find_if(registry.types.begin(), registry.types.end(), [path](const auto& known) { return Same(known->path, path); });
  return found != registry.types.end() ? *found : nullptr;
}

std::vector<std::string> MaterialTypePaths() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> paths;
  for (const auto& type : registry.types) paths.push_back(type->path);
  return paths;
}

void LoadMaterials(const std::vector<ModInfo>& mods) {
  std::map<std::string, MaterialAsset> loaded;
  for (const ModInfo& mod : mods) {
    for (const AssetInfo& info : ScanContent(mod).assets) {
      if (info.kind != AssetKind::kMaterial) continue;
      std::string error;
      std::optional<MaterialAsset> asset = ReadMaterialAsset(info.file, &error);
      if (!asset) {
        RF_WARN("content: [{}] {}: {}", mod.folder, info.path, error);
        continue;
      }
      if (!FindMaterialType(asset->type)) RF_WARN("content: [{}] {}: the game has no material type \"{}\"", mod.folder, info.path, asset->type);
      loaded[Lower(info.path)] = std::move(*asset);
    }
  }
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  RF_INFO("content: {} materials", loaded.size());
  registry.loaded = std::move(loaded);
  ++registry.version;
}

void PreviewMaterial(std::string_view path, const MaterialAsset& asset) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  MaterialAsset& preview = registry.previews[Lower(path)];
  if (preview == asset) return;
  preview = asset;
  ++registry.version;
}

void EndMaterialPreview(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (registry.previews.erase(Lower(path)) != 0) ++registry.version;
}

std::optional<MaterialView> FindMaterial(std::string_view path) {
  Registry& registry = Get();
  MaterialAsset asset;
  uint64_t version = 0;
  const std::string key = Lower(path);
  {
    std::lock_guard lock(registry.mutex);
    const auto preview = registry.previews.find(key);
    const auto loaded = registry.loaded.find(key);
    // A preview stands only for a material that's loaded (its mod enabled), as everything else does.
    if (loaded == registry.loaded.end()) return std::nullopt;
    asset = preview != registry.previews.end() ? preview->second : loaded->second;
    version = registry.version;
  }
  const std::shared_ptr<const MaterialType> type = FindMaterialType(asset.type);
  MaterialView view = View(asset, type.get(), version);
  if (!type || !type->HasGraph()) return view;
  // The graph compiled once for each change to it, outside the lock: the game asks for its materials every frame.
  {
    std::lock_guard lock(registry.mutex);
    const auto kept = registry.programs.find(key);
    if (kept != registry.programs.end() && kept->second.first == asset.graph) view.program = kept->second.second;
  }
  if (!view.program) {
    auto program = std::make_shared<MaterialProgram>(material::Compile(*type, material::Graph::Read(asset.graph), path).program);
    std::lock_guard lock(registry.mutex);
    registry.programs[key] = {asset.graph, program};
    view.program = std::move(program);
  }
  return view;
}

}  // namespace rexfabric::content
