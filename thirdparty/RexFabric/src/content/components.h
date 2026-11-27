// What an avatar class is made of (docs/content_and_classes.md, Avatars): the backings and component types the game
// declares (rexfabric/content.h), and a class's components as its file holds them. A class file's components are a
// tree, each with a name unique along its class's ancestors, a type, a transform relative to the component above it
// (or the avatar) and values for its type's properties:
//
//   "components": [{"name": "Glow", "type": "TiP/FxEmitter", "transform": {"position": [0, 1.2, 0]},
//                   "properties": {"emitter": "pinata_misc_candleflame"}, "children": []}]
//
// Everything here may be called from any thread.

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <rexfabric/content.h>

#include "values.h"

namespace rexfabric::content {

// A component of a class, as the file has it.
struct Component {
  std::string name;
  std::string type;
  Transform transform;
  nlohmann::json properties = nlohmann::json::object();  // only the ones the class sets; the rest are their defaults
  std::vector<Component> children;

  bool operator==(const Component&) const = default;
};

// A component type the game declared, its properties as content keeps them.
struct ComponentType {
  std::string path;
  std::string help;
  bool transform = true;
  std::vector<Property> properties;
  std::function<void(ComponentContext&)> attach;
  std::function<void(ComponentContext&)> tick;
  std::function<void(ComponentContext&)> detach;
};

std::shared_ptr<const ComponentType> FindComponentType(std::string_view path);
std::vector<std::string> ComponentTypePaths();

std::shared_ptr<const BackingDecl> FindBacking(std::string_view path);
// The backings for a game class, in the order the game declared them.
std::vector<std::string> BackingsFor(std::string_view game_class);
std::vector<std::string> BackingPaths();

// A class file's "components", and back. A component without a name or a type is an error.
std::optional<std::vector<Component>> ReadComponents(const nlohmann::json& list, std::string* error);
nlohmann::ordered_json WriteComponents(const std::vector<Component>& components);

// Checks a class's own components as it loads: each type there, each name valid and unique among `taken` (the names of
// its ancestors' components, which this adds to), and each value fitting its property. The values come back fitted.
bool CheckComponents(std::vector<Component>& components, std::vector<std::string>& taken, std::string* why);

// The value of each of a component's properties: the class's, or the type's default.
std::vector<std::pair<std::string, nlohmann::json>> ComponentValues(const ComponentType& type, const Component& component);

// A transform as files keep it: an object of "position", "rotation" and "scale", each three numbers and each left out
// when it's the default. Read into `out` (what's left out stays as it was); false, with `why`, for one that isn't that.
bool ReadTransform(const nlohmann::json& transform, Transform& out, std::string* why);
nlohmann::ordered_json WriteTransform(const Transform& transform);

// Where something at `local` is in the world, when what it's on is at `parent`.
Transform Compose(const Transform& parent, const Transform& local);

// Compose undone: where something at `world` is relative to what it's on, at `parent`.
Transform Relative(const Transform& parent, const Transform& world);

// MatrixOf and TransformOf, a transform as a matrix and back, are in rexfabric/content.h for the game too.

}  // namespace rexfabric::content
