#include <rexfabric/game_api.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "events.h"
#include "log.h"

namespace rexfabric {
namespace {

struct Registry {
  std::mutex mutex;
  std::vector<std::unique_ptr<NamespaceDef>> namespaces;
  std::unordered_map<std::string, std::pair<uint32_t, const void*>> interfaces;
};

Registry& GetRegistry() {
  static Registry registry;
  return registry;
}

NamespaceDef& FindOrAdd(std::string_view name) {
  Registry& registry = GetRegistry();
  std::lock_guard lock(registry.mutex);
  for (auto& ns : registry.namespaces) {
    if (ns->name == name) return *ns;
  }
  auto ns = std::make_unique<NamespaceDef>();
  ns->name = std::string(name);
  return *registry.namespaces.emplace_back(std::move(ns));
}

}  // namespace

const char* ValueTypeName(ValueType type) {
  switch (type) {
    case ValueType::kNone: return "nothing";
    case ValueType::kBool: return "boolean";
    case ValueType::kInt: return "integer";
    case ValueType::kFloat: return "number";
    case ValueType::kString: return "string";
    case ValueType::kObject: return "object";
  }
  return "?";
}

uint32_t SizeOf(const FieldType& type) {
  switch (type.kind) {
    case FieldKind::kBool8: case FieldKind::kS8: case FieldKind::kU8: return 1;
    case FieldKind::kS16: case FieldKind::kU16: return 2;
    case FieldKind::kBool32: case FieldKind::kS32: case FieldKind::kU32: case FieldKind::kF32: case FieldKind::kPointer: case FieldKind::kCString: return 4;
    case FieldKind::kS64: case FieldKind::kU64: case FieldKind::kF64: return 8;
    case FieldKind::kStruct: return type.target ? type.target->size() : 0;
    case FieldKind::kChars: return type.length;
  }
  return 0;
}

// TypeDef -----------------------------------------------------------------------------------------------

TypeDef& TypeDef::Field(std::string name, uint32_t offset, FieldType type) {
  fields_.push_back({std::move(name), offset, type});
  return *this;
}

TypeDef& TypeDef::Held(Handles handles) {
  if (!handles.hold || !handles.resolve) {
    RF_ERROR("type {} needs both a hold and a resolve function to be held by handle", name_);
    return *this;
  }
  handles_ = std::move(handles);
  return *this;
}

TypeDef& TypeDef::Array(std::string name, uint32_t offset, FieldType type, uint32_t count, uint32_t stride) {
  fields_.push_back({std::move(name), offset, type, count, stride ? stride : SizeOf(type)});
  return *this;
}

void TypeDef::AddMethod(FunctionDef function, std::string short_name) {
  if (function.params.empty() || function.params.front() != ValueType::kObject) {
    RF_ERROR("method {} must take the object as its first argument", function.name);
    return;
  }
  methods_.emplace_back(std::move(short_name), std::move(function));
}

const FieldDef* TypeDef::FindField(std::string_view name) const {
  for (const FieldDef& field : fields_) {
    if (field.name == name) return &field;
  }
  return nullptr;
}

const FunctionDef* TypeDef::FindMethod(std::string_view name) const {
  for (const auto& [short_name, function] : methods_) {
    if (short_name == name) return &function;
  }
  return nullptr;
}

// Namespace ---------------------------------------------------------------------------------------------

Namespace::Namespace(std::string name) : name_(std::move(name)) { FindOrAdd(name_); }

void Namespace::AddFunction(FunctionDef function) {
  NamespaceDef& ns = FindOrAdd(name_);
  std::erase_if(ns.functions, [&](const FunctionDef& existing) { return existing.name == function.name; });
  ns.functions.push_back(std::move(function));
}

Namespace& Namespace::Constant(std::string name, Value value) {
  FindOrAdd(name_).constants.push_back({Qualify(name), std::move(value)});
  return *this;
}

Namespace& Namespace::Enum(std::string name, std::vector<std::pair<std::string, int64_t>> values) {
  FindOrAdd(name_).enums.push_back({Qualify(name), std::move(values)});
  return *this;
}

TypeDef& Namespace::Type(std::string name, uint32_t size) {
  NamespaceDef& ns = FindOrAdd(name_);
  const std::string full = Qualify(name);
  for (auto& type : ns.types) {
    if (type->name() == full) return *type;
  }
  return *ns.types.emplace_back(std::make_unique<TypeDef>(full, size));
}

rexfabric::Event Namespace::Event(std::string name) {
  const std::string full = Qualify(name);
  NamespaceDef& ns = FindOrAdd(name_);
  if (std::find(ns.events.begin(), ns.events.end(), full) == ns.events.end()) ns.events.push_back(full);
  return rexfabric::Event(full);
}

Namespace& Namespace::Lua(std::string source) {
  FindOrAdd(name_).lua.push_back(std::move(source));
  return *this;
}

Namespace& Namespace::Category(std::string prefix, std::string title) {
  FindOrAdd(name_).categories.emplace_back(std::move(prefix), std::move(title));
  return *this;
}

Namespace& Namespace::Args(std::string name, std::vector<std::string> names) {
  FindOrAdd(name_).args.emplace_back(Qualify(name), std::move(names));
  return *this;
}

Namespace& Namespace::Choices(std::string name, std::vector<std::pair<std::string, std::string>> choices) {
  FindOrAdd(name_).choices.emplace_back(Qualify(name), std::move(choices));
  return *this;
}

Namespace& Namespace::Choices(std::string name, std::function<std::vector<std::pair<std::string, std::string>>()> list) {
  FindOrAdd(name_).live_choices.emplace_back(Qualify(name), std::move(list));
  return *this;
}

// Lookups -----------------------------------------------------------------------------------------------

const std::vector<std::unique_ptr<NamespaceDef>>& Namespaces() { return GetRegistry().namespaces; }

const FunctionDef* FindFunction(std::string_view name) {
  for (const auto& ns : Namespaces()) {
    for (const FunctionDef& function : ns->functions) {
      if (function.name == name) return &function;
    }
  }
  return nullptr;
}

const TypeDef* FindType(std::string_view name) {
  for (const auto& ns : Namespaces()) {
    for (const auto& type : ns->types) {
      if (type->name() == name) return type.get();
    }
  }
  return nullptr;
}

// Guest function names -----------------------------------------------------------------------------------

namespace {

// Named before Start from one thread, then only read. The vector stays sorted by name; the maps index into it, rebuilt
// once the last name is in.
struct FunctionNames {
  std::mutex mutex;
  std::vector<GuestFunctionName> functions;
  std::unordered_map<std::string, uint32_t> by_name;
  std::unordered_map<uint32_t, size_t> by_address;  // index into functions, once sorted
  bool sorted = true;
};

FunctionNames& GetFunctionNames() {
  static FunctionNames names;
  return names;
}

// The names sorted and indexed, the first time anything is looked up after the last one was added.
FunctionNames& SortedFunctionNames() {
  FunctionNames& names = GetFunctionNames();
  std::lock_guard lock(names.mutex);
  if (!names.sorted) {
    std::sort(names.functions.begin(), names.functions.end(), [](const GuestFunctionName& a, const GuestFunctionName& b) { return a.name < b.name; });
    names.by_address.clear();
    for (size_t i = 0; i < names.functions.size(); ++i) names.by_address.emplace(names.functions[i].address, i);
    names.sorted = true;
  }
  return names;
}

}  // namespace

bool NameGuestFunction(GuestFunctionName function) {
  FunctionNames& names = GetFunctionNames();
  std::lock_guard lock(names.mutex);
  if (function.name.empty() || function.address == 0) {
    RF_WARN("a guest function needs a name and an address to be named ({}, {:08X})", function.name, function.address);
    return false;
  }
  if (const auto taken = names.by_name.find(function.name); taken != names.by_name.end()) {
    RF_WARN("guest function name {} is taken by {:08X}; {:08X} stays unnamed", function.name, taken->second, function.address);
    return false;
  }
  names.by_name.emplace(function.name, function.address);
  names.functions.push_back(std::move(function));
  names.sorted = false;
  return true;
}

uint32_t FindGuestFunction(std::string_view name) {
  FunctionNames& names = GetFunctionNames();
  std::lock_guard lock(names.mutex);
  const auto found = names.by_name.find(std::string(name));
  return found != names.by_name.end() ? found->second : 0;
}

const GuestFunctionName* GuestFunctionAt(uint32_t address) {
  FunctionNames& names = SortedFunctionNames();
  const auto found = names.by_address.find(address);
  return found != names.by_address.end() ? &names.functions[found->second] : nullptr;
}

const std::vector<GuestFunctionName>& GuestFunctionNames() { return SortedFunctionNames().functions; }

// Native interfaces --------------------------------------------------------------------------------------

void PublishInterface(std::string name, uint32_t version, const void* table) {
  Registry& registry = GetRegistry();
  std::lock_guard lock(registry.mutex);
  registry.interfaces[std::move(name)] = {version, table};
}

const void* FindInterface(std::string_view name, uint32_t min_version) {
  Registry& registry = GetRegistry();
  std::lock_guard lock(registry.mutex);
  const auto it = registry.interfaces.find(std::string(name));
  return it != registry.interfaces.end() && it->second.first >= min_version ? it->second.second : nullptr;
}

}  // namespace rexfabric
