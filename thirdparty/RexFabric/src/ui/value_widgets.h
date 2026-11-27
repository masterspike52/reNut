// The pieces the content editors type and fill properties with (docs/content_and_classes.md, Property types,
// Containers): a type picker and a value editor that opens arrays, maps and structs as trees.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../content/values.h"

namespace rexfabric::ui {

// What a mod may name in a type or an asset value: its own, its dependencies' and the game's.
struct TypeChoices {
  std::vector<std::string> classes;
  std::vector<std::string> enums;
  std::vector<std::string> structs;
  std::vector<std::string> textures;   // the texture assets, for an asset of the class Texture
  std::vector<std::string> materials;  // the material assets, for an asset of the class Material
  std::vector<std::string> map_edits;  // the map edit assets, for an asset of the class MapEdit
  std::vector<std::string> meshes;     // the mesh assets, for an asset of the class Mesh
  std::vector<std::string> worlds;     // the mods' worlds, for an asset of the class World
  std::vector<std::string> packets;    // the game's packets and the mods', for an asset of the class Packet
};

// Picks from `paths`, with a search box at the list's top; `none` adds an entry for nothing. True when one was picked.
bool PathCombo(const char* id, const char* preview, std::string& picked, const std::vector<std::string>& paths, const char* none, int flags = 0);

// Edits a type as one dropdown showing it ("array of MyMod/Colour"): what it holds (one value, an array or a map, and
// a map's key), the property type or the enum or struct, and an asset's class. True when it changed.
bool TypeField(const char* id, content::ValueType& type, const TypeChoices& choices);

// Edits a value of `type` in place, as wide as there's room for; true when it changed. A value that isn't the shape
// its type needs says so, with a reset to its empty value. Arrays, maps and structs open as a tree, an element, an
// entry or a field a row; an asset is typed as a path or picked from `choices`' classes, and a string from a table typed
// or picked from its rows.
bool ValueField(const char* id, const content::ValueType& type, nlohmann::json& value, const content::TypeLookup& types, const TypeChoices& choices);

// A value as one line of text, for a value shown and not edited: an enum value by its display name, an array as
// [a, b], a map as {key: value}, a struct as {field: value}.
std::string ValueText(const content::ValueType& type, const nlohmann::json& value, const content::TypeLookup& types);

}  // namespace rexfabric::ui
