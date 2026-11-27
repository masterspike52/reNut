// Values of content: what a property, a struct's field or a map's key holds, checked against its type
// (docs/content_and_classes.md, Property types, Containers, Structs and enums).
//
// As files keep them: a single value as its type has it (an enum value by its name, a struct as an object of its
// fields), an array as a JSON array, and a map as a JSON array of [key, value] pairs, so keys of any type keep their
// type. The enums and structs a value names are looked up through a TypeLookup: the loaded ones (types.h), or for the
// editors, the files as they are now (assets.h).

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <rexfabric/content.h>

namespace rexfabric::content {

// A property of a class, or a field of a struct.
struct Property {
  std::string name;
  ValueType type;
  nlohmann::json value;  // the default where it is declared
  std::string help;
  std::string declared_by;  // a class's property: the class declaring it
  std::string value_from;   // a class's property as Properties gives it: the class whose value it is
};

struct EnumValue {
  std::string name;     // as values and Lua name it
  std::string display;  // as the player sees it, the name when empty
  std::string help;
};

// A game enum is one the game declared through its API, named as Lua names it ("tip.Weather"); a mod's is an asset.
struct Enum {
  std::string path;
  std::string mod;  // empty for the game's
  std::string help;
  std::vector<EnumValue> values;
  // The game's: each value's number, as the game's functions take it.
  std::vector<int64_t> numbers;
};

struct Struct {
  std::string path;
  std::string mod;  // empty for the game's
  std::string help;
  std::vector<Property> fields;
};

// Where the enums and structs values name are found.
struct TypeLookup {
  std::function<std::optional<Enum>(std::string_view path)> find_enum;
  std::function<std::optional<Struct>(std::string_view path)> find_struct;
};

// The name of a property type or a container as files and Lua write it ("int", "array"), and back.
std::string_view TypeName(PropertyType type);
std::optional<PropertyType> TypeFromName(std::string_view name);
std::string_view ContainerName(Container container);
std::optional<Container> ContainerFromName(std::string_view name);
// Every property type, in the order an editor offers them.
const std::vector<PropertyType>& PropertyTypes();
// Whether a map's keys can be of the type: values that compare exactly.
bool CanBeKey(PropertyType type);
// The type of one element: the type without its container.
ValueType ElementType(const ValueType& type);
// The type of a map's keys.
ValueType KeyType(const ValueType& type);
// "int", "array of MyMod/Colour", "map of string to float": a type as messages name it.
std::string Describe(const ValueType& type);

// A value of `type`, as it's kept (a float always a double, an empty asset null, a struct with every field); false with
// `why` when it doesn't fit.
bool FitValue(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, nlohmann::json* out, std::string* why);
// A value of the type when nothing gives one: the empty value of a single type (an enum's first value, a struct of its
// fields' defaults), an empty array or map.
nlohmann::json EmptyValue(const TypeLookup& types, const ValueType& type);

// Calls `visit` with every single value inside `value`, which is of `type`: itself, each element, each map key and
// value, and each field of a struct, nested ones included, outermost first. A value that doesn't fit its type is
// skipped.
void WalkValues(const TypeLookup& types, const ValueType& type, nlohmann::json& value, const std::function<void(const ValueType& single, nlohmann::json& value)>& visit);

// A declaration's type as files write it, into an entry with "type" and, when they say something, "of", "container",
// "key" and "key_of". And back, with `why` for an entry that doesn't say a type.
void WriteType(const ValueType& type, nlohmann::ordered_json& entry);
std::optional<ValueType> ReadType(const nlohmann::json& entry, std::string* why);

// A value a game gave in a PropertyDecl, as files keep it.
nlohmann::json ToJson(const PropertyValue& value);
// A value as files keep it, as a game is given it: none for what no PropertyValue holds (an array, a map, a struct).
PropertyValue ToPropertyValue(const nlohmann::json& value);

}  // namespace rexfabric::content
