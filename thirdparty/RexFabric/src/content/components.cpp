#include "components.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <numbers>

#include "../log.h"
#include "classes.h"
#include "types.h"

namespace rexfabric::content {
namespace {

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

struct Registry {
  std::mutex mutex;
  std::vector<std::shared_ptr<const ComponentType>> components;
  std::vector<std::shared_ptr<const BackingDecl>> backings;
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

bool ValidPath(std::string_view path, std::string* why) {
  size_t start = 0;
  for (size_t slash = 0; slash != std::string_view::npos; start = slash + 1) {
    slash = path.find('/', start);
    if (!ValidName(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start), why)) return false;
  }
  return true;
}

// A position, rotation or scale in a file: three numbers, or the default when it isn't there.
bool ReadTriple(const nlohmann::json& transform, const char* key, std::array<double, 3>& out, std::string* why) {
  const auto found = transform.find(key);
  if (found == transform.end()) return true;
  if (!found->is_array() || found->size() != 3 || !std::all_of(found->begin(), found->end(), [](const nlohmann::json& n) { return n.is_number(); })) {
    if (why) *why = std::string("a transform's ") + key + " is three numbers";
    return false;
  }
  for (size_t i = 0; i < 3; ++i) out[i] = (*found)[i].get<double>();
  return true;
}

std::optional<Component> ReadComponent(const nlohmann::json& entry, std::string* why) {
  if (!entry.is_object()) {
    if (why) *why = "a component is an object, not " + entry.dump();
    return std::nullopt;
  }
  Component component;
  component.name = entry.value("name", std::string());
  component.type = entry.value("type", std::string());
  if (component.name.empty() || component.type.empty()) {
    if (why) *why = "a component has a name and a type";
    return std::nullopt;
  }
  if (!ReadTransform(entry.value("transform", nlohmann::json::object()), component.transform, why)) return std::nullopt;
  component.properties = entry.value("properties", nlohmann::json::object());
  if (!component.properties.is_object()) {
    if (why) *why = "a component's properties are an object";
    return std::nullopt;
  }
  std::optional<std::vector<Component>> children = ReadComponents(entry.value("children", nlohmann::json::array()), why);
  if (!children) return std::nullopt;
  component.children = std::move(*children);
  return component;
}

bool CheckOne(Component& component, std::vector<std::string>& taken, std::string* why) {
  std::string reason;
  if (!ValidName(component.name, &reason)) {
    if (why) *why = "component " + component.name + ": " + reason;
    return false;
  }
  if (std::any_of(taken.begin(), taken.end(), [&component](const std::string& name) { return Same(name, component.name); })) {
    if (why) *why = "two components are named " + component.name;
    return false;
  }
  taken.push_back(component.name);
  const std::shared_ptr<const ComponentType> type = FindComponentType(component.type);
  if (!type) {
    if (why) *why = "component " + component.name + " is a " + component.type + ", which the game doesn't have";
    return false;
  }
  component.type = type->path;
  nlohmann::json fitted = nlohmann::json::object();
  for (const auto& [name, value] : component.properties.items()) {
    const auto property = std::find_if(type->properties.begin(), type->properties.end(), [&name](const Property& known) { return Same(known.name, name); });
    if (property == type->properties.end()) {
      if (why) *why = "component " + component.name + ": a " + type->path + " has no property " + name;
      return false;
    }
    if (!FitValue(LoadedTypes(), property->type, value, &fitted[property->name], &reason)) {
      if (why) *why = "component " + component.name + ", " + property->name + ": " + reason;
      return false;
    }
  }
  component.properties = std::move(fitted);
  if (!type->transform && component.transform != Transform{}) {
    if (why) *why = "component " + component.name + " is a " + type->path + ", which has no transform";
    return false;
  }
  return std::all_of(component.children.begin(), component.children.end(), [&](Component& child) { return CheckOne(child, taken, why); });
}

// The rotation of pitch, yaw and roll in degrees, yaw first: rows of a 3 by 3 matrix.
using Matrix = std::array<std::array<double, 3>, 3>;

Matrix RotationOf(const std::array<double, 3>& degrees) {
  constexpr double kRadians = std::numbers::pi / 180.0;
  const double cp = std::cos(degrees[0] * kRadians), sp = std::sin(degrees[0] * kRadians);
  const double cy = std::cos(degrees[1] * kRadians), sy = std::sin(degrees[1] * kRadians);
  const double cr = std::cos(degrees[2] * kRadians), sr = std::sin(degrees[2] * kRadians);
  // Yaw about y, then pitch about x, then roll about z.
  const Matrix yaw{{{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}};
  const Matrix pitch{{{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}}};
  const Matrix roll{{{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}}};
  const auto multiply = [](const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
    }
    return out;
  };
  return multiply(multiply(yaw, pitch), roll);
}

std::array<double, 3> DegreesOf(const Matrix& m) {
  constexpr double kDegrees = 180.0 / std::numbers::pi;
  const double pitch = std::asin(std::clamp(-m[1][2], -1.0, 1.0));
  return {pitch * kDegrees, std::atan2(m[0][2], m[2][2]) * kDegrees, std::atan2(m[1][0], m[1][1]) * kDegrees};
}

}  // namespace

bool DeclareComponent(ComponentDecl decl) {
  std::string why;
  if (!ValidPath(decl.path, &why)) {
    RF_ERROR("content: the game's component {}: {}", decl.path, why);
    return false;
  }
  auto type = std::make_shared<ComponentType>(ComponentType{.path = std::move(decl.path), .help = std::move(decl.help), .transform = decl.transform, .attach = std::move(decl.attach), .tick = std::move(decl.tick), .detach = std::move(decl.detach)});
  for (PropertyDecl& property : decl.properties) type->properties.push_back({.name = std::move(property.name), .type = std::move(property.type), .value = ToJson(property.value), .help = std::move(property.help)});
  const auto anything = [](std::string_view, std::string*) { return true; };
  if (!CheckDeclared(type->properties, LoadedTypes(), anything, &why)) {
    RF_ERROR("content: the game's component {}: {}", type->path, why);
    return false;
  }
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (std::any_of(registry.components.begin(), registry.components.end(), [&type](const auto& known) { return Same(known->path, type->path); })) {
    RF_ERROR("content: the game's component {} is declared twice", type->path);
    return false;
  }
  registry.components.push_back(std::move(type));
  return true;
}

bool DeclareBacking(BackingDecl decl) {
  std::string why;
  if (!ValidPath(decl.path, &why)) {
    RF_ERROR("content: the game's backing {}: {}", decl.path, why);
    return false;
  }
  const std::optional<Class> for_class = FindClass(decl.for_class);
  if (!for_class || !for_class->mod.empty()) {
    RF_ERROR("content: the game's backing {} is for {}, which isn't one of the game's classes", decl.path, decl.for_class);
    return false;
  }
  if (!decl.make || !decl.where || !decl.move || !decl.remove) {
    RF_ERROR("content: the game's backing {} lacks one of make, where, move and remove", decl.path);
    return false;
  }
  decl.for_class = for_class->path;
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (std::any_of(registry.backings.begin(), registry.backings.end(), [&decl](const auto& known) { return Same(known->path, decl.path); })) {
    RF_ERROR("content: the game's backing {} is declared twice", decl.path);
    return false;
  }
  registry.backings.push_back(std::make_shared<const BackingDecl>(std::move(decl)));
  return true;
}

std::shared_ptr<const ComponentType> FindComponentType(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto found = std::find_if(registry.components.begin(), registry.components.end(), [path](const auto& known) { return Same(known->path, path); });
  return found != registry.components.end() ? *found : nullptr;
}

std::vector<std::string> ComponentTypePaths() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> paths;
  for (const auto& known : registry.components) paths.push_back(known->path);
  return paths;
}

std::shared_ptr<const BackingDecl> FindBacking(std::string_view path) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto found = std::find_if(registry.backings.begin(), registry.backings.end(), [path](const auto& known) { return Same(known->path, path); });
  return found != registry.backings.end() ? *found : nullptr;
}

std::vector<std::string> BackingsFor(std::string_view game_class) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> paths;
  for (const auto& known : registry.backings) {
    if (Same(known->for_class, game_class)) paths.push_back(known->path);
  }
  return paths;
}

std::vector<std::string> BackingPaths() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> paths;
  for (const auto& known : registry.backings) paths.push_back(known->path);
  return paths;
}

std::optional<std::vector<Component>> ReadComponents(const nlohmann::json& list, std::string* error) {
  if (!list.is_array()) {
    if (error) *error = "\"components\" is a list";
    return std::nullopt;
  }
  std::vector<Component> components;
  for (const nlohmann::json& entry : list) {
    std::optional<Component> component = ReadComponent(entry, error);
    if (!component) return std::nullopt;
    components.push_back(std::move(*component));
  }
  return components;
}

bool ReadTransform(const nlohmann::json& transform, Transform& out, std::string* why) {
  if (!transform.is_object()) {
    if (why) *why = "a transform is an object";
    return false;
  }
  return ReadTriple(transform, "position", out.position, why) && ReadTriple(transform, "rotation", out.rotation, why) && ReadTriple(transform, "scale", out.scale, why);
}

nlohmann::ordered_json WriteTransform(const Transform& transform) {
  // Only what differs from where it would be without one.
  nlohmann::ordered_json written = nlohmann::ordered_json::object();
  const Transform none;
  if (transform.position != none.position) written["position"] = transform.position;
  if (transform.rotation != none.rotation) written["rotation"] = transform.rotation;
  if (transform.scale != none.scale) written["scale"] = transform.scale;
  return written;
}

nlohmann::ordered_json WriteComponents(const std::vector<Component>& components) {
  nlohmann::ordered_json list = nlohmann::ordered_json::array();
  for (const Component& component : components) {
    nlohmann::ordered_json entry{{"name", component.name}, {"type", component.type}};
    if (nlohmann::ordered_json transform = WriteTransform(component.transform); !transform.empty()) entry["transform"] = std::move(transform);
    if (!component.properties.empty()) entry["properties"] = nlohmann::ordered_json::parse(component.properties.dump());
    if (!component.children.empty()) entry["children"] = WriteComponents(component.children);
    list.push_back(std::move(entry));
  }
  return list;
}

bool CheckComponents(std::vector<Component>& components, std::vector<std::string>& taken, std::string* why) {
  return std::all_of(components.begin(), components.end(), [&](Component& component) { return CheckOne(component, taken, why); });
}

std::vector<std::pair<std::string, nlohmann::json>> ComponentValues(const ComponentType& type, const Component& component) {
  std::vector<std::pair<std::string, nlohmann::json>> values;
  for (const Property& property : type.properties) {
    const auto set = component.properties.find(property.name);
    values.emplace_back(property.name, set != component.properties.end() ? *set : property.value);
  }
  return values;
}

Transform Compose(const Transform& parent, const Transform& local) {
  const Matrix rotation = RotationOf(parent.rotation);
  Transform world;
  for (int r = 0; r < 3; ++r) {
    double offset = 0.0;
    for (int c = 0; c < 3; ++c) offset += rotation[r][c] * local.position[c] * parent.scale[c];
    world.position[r] = parent.position[r] + offset;
    world.scale[r] = parent.scale[r] * local.scale[r];
  }
  const Matrix local_rotation = RotationOf(local.rotation);
  Matrix combined{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) combined[r][c] = rotation[r][0] * local_rotation[0][c] + rotation[r][1] * local_rotation[1][c] + rotation[r][2] * local_rotation[2][c];
  }
  world.rotation = DegreesOf(combined);
  return world;
}

Transform Relative(const Transform& parent, const Transform& world) {
  const Matrix rotation = RotationOf(parent.rotation);
  Transform local;
  for (int c = 0; c < 3; ++c) {
    // The offset turned back into the parent's axes, then unscaled: Compose's position read backwards.
    double along = 0.0;
    for (int r = 0; r < 3; ++r) along += rotation[r][c] * (world.position[r] - parent.position[r]);
    local.position[c] = parent.scale[c] != 0.0 ? along / parent.scale[c] : 0.0;
    local.scale[c] = parent.scale[c] != 0.0 ? world.scale[c] / parent.scale[c] : world.scale[c];
  }
  const Matrix world_rotation = RotationOf(world.rotation);
  Matrix relative{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) relative[r][c] = rotation[0][r] * world_rotation[0][c] + rotation[1][r] * world_rotation[1][c] + rotation[2][r] * world_rotation[2][c];
  }
  local.rotation = DegreesOf(relative);
  return local;
}

std::array<float, 16> MatrixOf(const Transform& transform) {
  const Matrix rotation = RotationOf(transform.rotation);
  std::array<float, 16> matrix{};
  for (int axis = 0; axis < 3; ++axis) {
    for (int r = 0; r < 3; ++r) matrix[axis * 4 + r] = static_cast<float>(rotation[r][axis] * transform.scale[axis]);
    matrix[12 + axis] = static_cast<float>(transform.position[axis]);
  }
  matrix[15] = 1.0f;
  return matrix;
}

Transform TransformOf(const std::array<float, 16>& matrix) {
  Transform transform;
  Matrix rotation{};
  for (int axis = 0; axis < 3; ++axis) {
    const double x = matrix[axis * 4], y = matrix[axis * 4 + 1], z = matrix[axis * 4 + 2];
    const double length = std::sqrt(x * x + y * y + z * z);
    transform.scale[axis] = length;
    for (int r = 0; r < 3; ++r) rotation[r][axis] = length > 0.0 ? matrix[axis * 4 + r] / length : (r == axis ? 1.0 : 0.0);
    transform.position[axis] = matrix[12 + axis];
  }
  transform.rotation = DegreesOf(rotation);
  return transform;
}

}  // namespace rexfabric::content
