// The classes RexFabric knows: Object, the game's (rexfabric/content.h) and the mods' class assets, loaded from each
// mod's content folder (docs/content_and_classes.md). Written at Start and on a reload, read from any thread.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <rexfabric/content.h>

#include "../mods.h"
#include "components.h"
#include "values.h"

namespace rexfabric::content {

struct Class {
  std::string path;     // "Object", "TiP/Decor", "MyMod/Garden/Decor_Rock_1"
  std::string parent;   // empty only for Object
  std::string mod;      // the mod folder of a mod's class; empty for RexFabric's and the game's
  std::string help;
  std::string type;  // the game type it stands for (rexfabric/content.h), empty for none
  ClassTest test;    // which objects of its type are one of it, for a game class without a type of its own
  std::filesystem::path file;
  std::vector<Property> properties;  // the ones it adds
  nlohmann::json defaults = nlohmann::json::object();  // the inherited ones it changes
  // A mod's avatar class's: the backing it names, empty to keep its parent's, and the components it adds.
  std::string backing;
  std::vector<Component> components;
};

// Loads every enum, struct, table and class asset of these mods (the ones that can load, in load order), after
// dropping the ones loaded before: the enums and structs first (types.h), then the tables (tables.h), as the tables'
// rows and the classes' properties use them. Problems are logged and the asset skipped.
void LoadModContent(const std::vector<ModInfo>& mods);

// The folder of a mod whose content loaded, by its folder name; none for one that didn't.
std::optional<std::filesystem::path> LoadedModDir(std::string_view folder);

// A class by its path, ignoring case; null for none.
std::optional<Class> FindClass(std::string_view path);
// Every class's path: Object, the game's, then the mods' in load order.
std::vector<std::string> ClassPaths();
// Whether `path` is `base` or comes from it.
bool IsA(std::string_view path, std::string_view base);
// Every property the class has, its ancestors' first, each with the class's value for it.
std::vector<Property> Properties(std::string_view path);
// The class's value of a property, from itself or the nearest ancestor that sets it; none for an unknown property.
std::optional<nlohmann::json> Default(std::string_view path, std::string_view property);
// Avatars (docs/content_and_classes.md, Avatars). The backing an avatar of a class is made with: its own, its nearest
// mod ancestor's, or the first the game declared for its nearest game class. Empty for a class that can't be spawned:
// a game class, or one with no backing above it.
std::string BackingOf(std::string_view path);
// Every component an avatar of the class has: its ancestors' first, each class's as its tree.
std::vector<Component> ComponentsOf(std::string_view path);
// The classes the mod `from` may spawn avatars of, as MayReference allows.
std::vector<std::string> AvatarClassPaths(std::string_view from);

// Whether the mod `from` may use what `path` names: a path of RexFabric or the game, of `from` itself, or of a mod
// it depends on. `from` empty (the game, a native caller) may use anything. Otherwise `why` says which dependency
// is missing.
bool MayReference(std::string_view from, std::string_view path, std::string* why);

// Object types, as graphs and Lua name them: a game type ("tip.Avatar") or a class's path ("TiP/Pinata",
// "MyMod/Rocks/Rock"). A game type and the class standing for it are the same type.
//
// The game type a class's objects are: its own, or its nearest ancestor's. Empty for none, and for no class.
std::string TypeOfClass(std::string_view path);
// Whether `name` is a type: a game type, or a class.
bool IsType(std::string_view name);
// Whether every `from` is a `to`: the same type, or one below it.
bool TypeIsA(std::string_view from, std::string_view to);
// Every type a mod may use, for a type picker: the game types, then the classes that aren't one of them, Object left
// out. `from` as in MayReference.
std::vector<std::string> TypeNames(std::string_view from);
// Whether an object of the class's type (TypeOfClass) is one of the class: it passes the tests of the classes on the
// way down from the type. A mod's class has no objects until mods can spawn them, so for one this is false. A test
// may throw, as the game's functions do, to refuse.
bool IsOfClass(std::string_view path, const Object& object);

}  // namespace rexfabric::content
