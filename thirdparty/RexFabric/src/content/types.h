// The enums and structs RexFabric knows (docs/content_and_classes.md, Structs and enums): the game's, which are the
// enums it declared through its API and the structs it declared (rexfabric/content.h), and the mods' enum and struct
// assets. Written at Start and on a reload, before the classes, which use them; read from any thread.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../mods.h"
#include "values.h"

namespace rexfabric::content {

// Loads every enum and struct asset of these mods (the ones that can load, in load order), after dropping the ones
// loaded before. A struct waits for the structs its fields hold; problems are logged and the asset skipped.
void LoadModTypes(const std::vector<ModInfo>& mods);

// An enum by its path (a game's by its Lua name, exactly; a mod's ignoring case); null for none.
std::optional<Enum> FindEnum(std::string_view path);
std::optional<Struct> FindStruct(std::string_view path);
// Every enum and struct the mod `from` may use (MayReference): the game's, then the mods' in load order.
std::vector<std::string> EnumPaths(std::string_view from);
std::vector<std::string> StructPaths(std::string_view from);

// The loaded enums and structs, for FitValue.
const TypeLookup& LoadedTypes();

// Checks properties or fields as a class or a struct declares them, and fits each default to its type (an empty value
// for none): names that are valid and different, the enums and structs their types name there, and every path they
// name one `may_use` takes. False with `why`, naming the one that failed.
bool CheckDeclared(std::vector<Property>& declared, const TypeLookup& types, const std::function<bool(std::string_view path, std::string* why)>& may_use, std::string* why);

}  // namespace rexfabric::content
