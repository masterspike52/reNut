#include "values.h"

#include <algorithm>
#include <cctype>

namespace rexfabric::content {
namespace {

// How deep structs may hold structs before a value is taken to go round in a circle.
constexpr int kMaxDepth = 16;

bool Fail(std::string* why, std::string reason) {
  if (why) *why = std::move(reason);
  return false;
}

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

// A single type as messages name it: "int", "tip.Weather", "asset of TiP/Pinata".
std::string DescribeSingle(PropertyType type, const std::string& of) {
  if ((type == PropertyType::kEnum || type == PropertyType::kStruct) && !of.empty()) return of;
  if (type == PropertyType::kAsset && !of.empty()) return "asset of " + of;
  if (type == PropertyType::kString && !of.empty()) return "string from " + of;
  return std::string(TypeName(type));
}

bool Fit(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, nlohmann::json* out, std::string* why, int depth);

bool FitSingle(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, nlohmann::json* out, std::string* why, int depth) {
  const auto wrong = [&] { return Fail(why, "a " + DescribeSingle(type.type, type.of) + " can't be " + value.dump()); };
  switch (type.type) {
    case PropertyType::kBool:
      if (!value.is_boolean()) return wrong();
      *out = value;
      return true;
    case PropertyType::kInt:
      if (!value.is_number_integer()) return wrong();
      *out = value;
      return true;
    case PropertyType::kFloat:
      if (!value.is_number()) return wrong();
      *out = value.get<double>();
      return true;
    case PropertyType::kString:
    case PropertyType::kText:
      if (!value.is_string()) return wrong();
      *out = value;
      return true;
    case PropertyType::kAsset:
      if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) {
        *out = nullptr;
        return true;
      }
      if (!value.is_string()) return wrong();
      *out = value;
      return true;
    case PropertyType::kEnum: {
      const std::optional<Enum> found = types.find_enum ? types.find_enum(type.of) : std::nullopt;
      if (!found) return Fail(why, "there's no enum " + type.of);
      if (!value.is_string()) return wrong();
      const std::string name = value.get<std::string>();
      if (std::none_of(found->values.begin(), found->values.end(), [&name](const EnumValue& known) { return known.name == name; })) return Fail(why, type.of + " has no value " + name);
      *out = value;
      return true;
    }
    case PropertyType::kStruct: {
      const std::optional<Struct> found = types.find_struct ? types.find_struct(type.of) : std::nullopt;
      if (!found) return Fail(why, "there's no struct " + type.of);
      if (depth >= kMaxDepth) return Fail(why, type.of + " holds itself too deep");
      if (value.is_null()) {
        *out = EmptyValue(types, type);
        return true;
      }
      if (!value.is_object()) return wrong();
      for (const auto& [key, held] : value.items()) {
        if (std::none_of(found->fields.begin(), found->fields.end(), [&key](const Property& field) { return field.name == key; })) return Fail(why, type.of + " has no field " + key);
      }
      nlohmann::json fitted = nlohmann::json::object();
      for (const Property& field : found->fields) {
        const auto given = value.find(field.name);
        if (given == value.end()) {
          fitted[field.name] = field.value;
          continue;
        }
        std::string inner;
        if (!Fit(types, field.type, *given, &fitted[field.name], &inner, depth + 1)) return Fail(why, field.name + ": " + inner);
      }
      *out = std::move(fitted);
      return true;
    }
  }
  return wrong();
}

bool Fit(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, nlohmann::json* out, std::string* why, int depth) {
  switch (type.container) {
    case Container::kSingle: return FitSingle(types, type, value, out, why, depth);
    case Container::kArray: {
      if (!value.is_array()) return Fail(why, "an " + Describe(type) + " can't be " + value.dump());
      nlohmann::json fitted = nlohmann::json::array();
      for (size_t i = 0; i < value.size(); ++i) {
        std::string inner;
        if (!FitSingle(types, ElementType(type), value[i], &fitted.emplace_back(), &inner, depth)) return Fail(why, "element " + std::to_string(i + 1) + ": " + inner);
      }
      *out = std::move(fitted);
      return true;
    }
    case Container::kMap: {
      if (!value.is_array()) return Fail(why, "a " + Describe(type) + " is a list of [key, value] pairs, not " + value.dump());
      nlohmann::json fitted = nlohmann::json::array();
      for (const nlohmann::json& pair : value) {
        if (!pair.is_array() || pair.size() != 2) return Fail(why, "a map's entry is a [key, value] pair, not " + pair.dump());
        nlohmann::json key;
        nlohmann::json held;
        std::string inner;
        if (!FitSingle(types, KeyType(type), pair[0], &key, &inner, depth)) return Fail(why, "key " + pair[0].dump() + ": " + inner);
        if (!FitSingle(types, ElementType(type), pair[1], &held, &inner, depth)) return Fail(why, "value of " + pair[0].dump() + ": " + inner);
        if (std::any_of(fitted.begin(), fitted.end(), [&key](const nlohmann::json& kept) { return kept[0] == key; })) return Fail(why, "the key " + key.dump() + " is there twice");
        fitted.push_back(nlohmann::json::array({std::move(key), std::move(held)}));
      }
      *out = std::move(fitted);
      return true;
    }
  }
  return Fail(why, "unknown container");
}

void WalkSingle(const TypeLookup& types, const ValueType& type, nlohmann::json& value, const std::function<void(const ValueType&, nlohmann::json&)>& visit, int depth);

void Walk(const TypeLookup& types, const ValueType& type, nlohmann::json& value, const std::function<void(const ValueType&, nlohmann::json&)>& visit, int depth) {
  if (depth >= kMaxDepth) return;
  switch (type.container) {
    case Container::kSingle: WalkSingle(types, type, value, visit, depth); break;
    case Container::kArray:
      if (!value.is_array()) return;
      for (nlohmann::json& element : value) WalkSingle(types, ElementType(type), element, visit, depth);
      break;
    case Container::kMap:
      if (!value.is_array()) return;
      for (nlohmann::json& pair : value) {
        if (!pair.is_array() || pair.size() != 2) continue;
        WalkSingle(types, KeyType(type), pair[0], visit, depth);
        WalkSingle(types, ElementType(type), pair[1], visit, depth);
      }
      break;
  }
}

void WalkSingle(const TypeLookup& types, const ValueType& type, nlohmann::json& value, const std::function<void(const ValueType&, nlohmann::json&)>& visit, int depth) {
  visit(type, value);
  if (type.type != PropertyType::kStruct || !value.is_object() || !types.find_struct) return;
  const std::optional<Struct> found = types.find_struct(type.of);
  if (!found) return;
  for (const Property& field : found->fields) {
    const auto held = value.find(field.name);
    if (held != value.end()) Walk(types, field.type, *held, visit, depth + 1);
  }
}

}  // namespace

std::string_view TypeName(PropertyType type) {
  switch (type) {
    case PropertyType::kBool: return "bool";
    case PropertyType::kInt: return "int";
    case PropertyType::kFloat: return "float";
    case PropertyType::kString: return "string";
    case PropertyType::kText: return "text";
    case PropertyType::kAsset: return "asset";
    case PropertyType::kEnum: return "enum";
    case PropertyType::kStruct: return "struct";
  }
  return "int";
}

std::optional<PropertyType> TypeFromName(std::string_view name) {
  for (const PropertyType type : PropertyTypes()) {
    if (Lower(name) == TypeName(type)) return type;
  }
  return std::nullopt;
}

std::string_view ContainerName(Container container) {
  switch (container) {
    case Container::kSingle: return "single";
    case Container::kArray: return "array";
    case Container::kMap: return "map";
  }
  return "single";
}

std::optional<Container> ContainerFromName(std::string_view name) {
  for (const Container container : {Container::kSingle, Container::kArray, Container::kMap}) {
    if (Lower(name) == ContainerName(container)) return container;
  }
  return std::nullopt;
}

const std::vector<PropertyType>& PropertyTypes() {
  static const std::vector<PropertyType> types = {PropertyType::kBool, PropertyType::kInt, PropertyType::kFloat, PropertyType::kString, PropertyType::kText, PropertyType::kAsset, PropertyType::kEnum, PropertyType::kStruct};
  return types;
}

bool CanBeKey(PropertyType type) { return type == PropertyType::kBool || type == PropertyType::kInt || type == PropertyType::kString || type == PropertyType::kEnum || type == PropertyType::kAsset; }

ValueType ElementType(const ValueType& type) { return {.type = type.type, .of = type.of}; }

ValueType KeyType(const ValueType& type) { return {.type = type.key, .of = type.key_of}; }

std::string Describe(const ValueType& type) {
  const std::string element = DescribeSingle(type.type, type.of);
  switch (type.container) {
    case Container::kSingle: return element;
    case Container::kArray: return "array of " + element;
    case Container::kMap: return "map of " + DescribeSingle(type.key, type.key_of) + " to " + element;
  }
  return element;
}

bool FitValue(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, nlohmann::json* out, std::string* why) {
  nlohmann::json fitted;
  if (!Fit(types, type, value, &fitted, why, 0)) return false;
  if (out) *out = std::move(fitted);
  return true;
}

nlohmann::json EmptyValue(const TypeLookup& types, const ValueType& type) {
  if (type.container != Container::kSingle) return nlohmann::json::array();
  switch (type.type) {
    case PropertyType::kBool: return false;
    case PropertyType::kInt: return 0;
    case PropertyType::kFloat: return 0.0;
    case PropertyType::kString:
    case PropertyType::kText: return "";
    case PropertyType::kAsset: return nullptr;
    case PropertyType::kEnum: {
      const std::optional<Enum> found = types.find_enum ? types.find_enum(type.of) : std::nullopt;
      return found && !found->values.empty() ? nlohmann::json(found->values.front().name) : nlohmann::json("");
    }
    case PropertyType::kStruct: {
      nlohmann::json fields = nlohmann::json::object();
      if (const std::optional<Struct> found = types.find_struct ? types.find_struct(type.of) : std::nullopt) {
        for (const Property& field : found->fields) fields[field.name] = field.value;
      }
      return fields;
    }
  }
  return nullptr;
}

void WalkValues(const TypeLookup& types, const ValueType& type, nlohmann::json& value, const std::function<void(const ValueType& single, nlohmann::json& value)>& visit) { Walk(types, type, value, visit, 0); }

void WriteType(const ValueType& type, nlohmann::ordered_json& entry) {
  entry["type"] = TypeName(type.type);
  if (!type.of.empty()) entry["of"] = type.of;
  if (type.container == Container::kSingle) return;
  entry["container"] = ContainerName(type.container);
  if (type.container != Container::kMap) return;
  entry["key"] = TypeName(type.key);
  if (!type.key_of.empty()) entry["key_of"] = type.key_of;
}

std::optional<ValueType> ReadType(const nlohmann::json& entry, std::string* why) {
  const auto fail = [why](std::string reason) {
    Fail(why, std::move(reason));
    return std::nullopt;
  };
  if (!entry.is_object()) return fail(entry.dump() + " isn't an object");
  ValueType type;
  const std::optional<PropertyType> kind = TypeFromName(entry.value("type", std::string()));
  if (!kind) return fail("\"" + entry.value("type", std::string()) + "\" isn't a type");
  type.type = *kind;
  // Stage 1 files name an asset's class as asset_class.
  type.of = entry.value("of", entry.value("asset_class", std::string()));
  if ((type.type == PropertyType::kEnum || type.type == PropertyType::kStruct) && type.of.empty()) return fail("an " + std::string(TypeName(type.type)) + " says which one in \"of\"");
  const std::optional<Container> container = ContainerFromName(entry.value("container", std::string("single")));
  if (!container) return fail("\"" + entry.value("container", std::string()) + "\" isn't single, array or map");
  type.container = *container;
  if (type.container != Container::kMap) return type;
  const std::optional<PropertyType> key = TypeFromName(entry.value("key", std::string("string")));
  if (!key || !CanBeKey(*key)) return fail("a map's key is a bool, an int, a string, an enum or an asset");
  type.key = *key;
  type.key_of = entry.value("key_of", std::string());
  if (type.key == PropertyType::kEnum && type.key_of.empty()) return fail("an enum key says which enum in \"key_of\"");
  return type;
}

nlohmann::json ToJson(const PropertyValue& value) {
  return std::visit([](const auto& held) -> nlohmann::json {
    if constexpr (std::is_same_v<std::decay_t<decltype(held)>, std::monostate>) return nullptr;
    else return held;
  }, value);
}

PropertyValue ToPropertyValue(const nlohmann::json& value) {
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number_integer()) return value.get<int64_t>();
  if (value.is_number()) return value.get<double>();
  if (value.is_string()) return value.get<std::string>();
  return std::monostate{};
}

}  // namespace rexfabric::content
