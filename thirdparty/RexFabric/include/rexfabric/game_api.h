// The game's own modding API: the functions, types, enums, constants and events a game offers its mods. The
// game declares them once, in C++, and every front end hands them on in its own language: Lua sees tables
// and objects, native mods look them up by name through RexFabricApi. RexFabric itself knows nothing about
// any one game; it only carries what the game declares.
//
//   rexfabric::Namespace game("tip");
//   game.Function("version", [] { return std::string("1.0"); });
//   game.Function("garden.money", [] { return ReadMoney(); });
//   game.Constant("max_pinatas", 64);
//   game.Enum("Species", {{"Whirlm", 0}, {"Sparrowmint", 1}});
//
//   rexfabric::TypeDef& pinata = game.Type("Pinata", 0x3A0);
//   pinata.Field("species", 0x10, rexfabric::kU32);
//   pinata.Field("happiness", 0x1F0, rexfabric::kF32);
//   pinata.Field("garden", 0x20, rexfabric::Pointer(garden));
//   pinata.Method("feed", [](rexfabric::Object self, int64_t item) { Feed(self.address, item); });
//
//   static const rexfabric::Event garden_loaded = game.Event("garden_loaded");
//   garden_loaded.Fire(rexfabric::Object{garden_address, &garden});
//
// Declare everything before rexfabric::Start, which is when mods load and read it. Functions run on whichever
// thread the mod calls from: a guest thread inside a hook, or the frame thread in a frame event.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rexfabric {

class TypeDef;

// A guest object: where it is and what it is. A null address is "no object".
struct Object {
  uint32_t address = 0;
  const TypeDef* type = nullptr;
  explicit operator bool() const { return address != 0; }
  bool operator==(const Object&) const = default;
};

// What crosses between the game and a mod. The index of each alternative is its ValueType.
using Value = std::variant<std::monostate, bool, int64_t, double, std::string, Object>;
enum class ValueType : uint8_t { kNone, kBool, kInt, kFloat, kString, kObject };

inline ValueType TypeOf(const Value& value) { return static_cast<ValueType>(value.index()); }
const char* ValueTypeName(ValueType type);

// How a type's field is laid out in guest memory. Integers and floats are big endian, as the guest keeps
// them.
enum class FieldKind : uint8_t { kBool8, kBool32, kS8, kU8, kS16, kU16, kS32, kU32, kS64, kU64, kF32, kF64, kPointer, kStruct, kCString, kChars };

struct FieldType {
  FieldKind kind;
  const TypeDef* target = nullptr;  // kPointer and kStruct
  uint32_t length = 0;              // kChars
};

inline constexpr FieldType kBool8{FieldKind::kBool8};
inline constexpr FieldType kBool32{FieldKind::kBool32};
inline constexpr FieldType kS8{FieldKind::kS8};
inline constexpr FieldType kU8{FieldKind::kU8};
inline constexpr FieldType kS16{FieldKind::kS16};
inline constexpr FieldType kU16{FieldKind::kU16};
inline constexpr FieldType kS32{FieldKind::kS32};
inline constexpr FieldType kU32{FieldKind::kU32};
inline constexpr FieldType kS64{FieldKind::kS64};
inline constexpr FieldType kU64{FieldKind::kU64};
inline constexpr FieldType kF32{FieldKind::kF32};
inline constexpr FieldType kF64{FieldKind::kF64};
// A pointer to a NUL terminated string.
inline constexpr FieldType kCString{FieldKind::kCString};
// A 32 bit pointer to a `target`. Reads as that object, or none for a null pointer.
constexpr FieldType Pointer(const TypeDef& target) { return {FieldKind::kPointer, &target}; }
// A `target` held inline, at the field's offset.
constexpr FieldType Struct(const TypeDef& target) { return {FieldKind::kStruct, &target}; }
// A fixed size, NUL padded character array.
constexpr FieldType Chars(uint32_t length) { return {FieldKind::kChars, nullptr, length}; }

// Bytes one element of `type` takes.
uint32_t SizeOf(const FieldType& type);

struct FieldDef {
  std::string name;
  uint32_t offset;
  FieldType type;
  uint32_t count = 0;   // 0 for a single value, otherwise an array of this many
  uint32_t stride = 0;  // bytes between array elements
};

// A function a mod can call. Arguments arrive already converted to `params`.
struct FunctionDef {
  std::string name;  // full name, "tip.garden.money"
  std::vector<ValueType> params;
  ValueType result = ValueType::kNone;
  std::function<Value(std::span<const Value>)> invoke;
};

namespace detail {

template <typename T>
struct Marshal;

template <>
struct Marshal<bool> {
  static constexpr ValueType kType = ValueType::kBool;
  static bool From(const Value& value) { return std::get<bool>(value); }
};

template <typename T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
struct Marshal<T> {
  static constexpr ValueType kType = ValueType::kInt;
  static T From(const Value& value) { return static_cast<T>(std::get<int64_t>(value)); }
};

template <typename T>
  requires std::is_floating_point_v<T>
struct Marshal<T> {
  static constexpr ValueType kType = ValueType::kFloat;
  static T From(const Value& value) { return static_cast<T>(std::get<double>(value)); }
};

template <>
struct Marshal<std::string> {
  static constexpr ValueType kType = ValueType::kString;
  static const std::string& From(const Value& value) { return std::get<std::string>(value); }
};

template <>
struct Marshal<std::string_view> {
  static constexpr ValueType kType = ValueType::kString;
  static std::string_view From(const Value& value) { return std::get<std::string>(value); }
};

template <>
struct Marshal<const char*> {
  static constexpr ValueType kType = ValueType::kString;
  static const char* From(const Value& value) { return std::get<std::string>(value).c_str(); }
};

template <>
struct Marshal<Object> {
  static constexpr ValueType kType = ValueType::kObject;
  static Object From(const Value& value) { return std::get<Object>(value); }
};

template <typename T>
Value ToValue(T&& result) {
  using R = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<R, Value>) return std::forward<T>(result);
  else if constexpr (std::is_same_v<R, bool>) return Value(result);
  else if constexpr (std::is_integral_v<R>) return Value(static_cast<int64_t>(result));
  else if constexpr (std::is_floating_point_v<R>) return Value(static_cast<double>(result));
  else if constexpr (std::is_same_v<R, Object>) return Value(result);
  else return Value(std::string(std::forward<T>(result)));
}

template <typename T>
constexpr ValueType ResultType() {
  using R = std::remove_cvref_t<T>;
  if constexpr (std::is_void_v<R>) return ValueType::kNone;
  else if constexpr (std::is_same_v<R, bool>) return ValueType::kBool;
  else if constexpr (std::is_integral_v<R>) return ValueType::kInt;
  else if constexpr (std::is_floating_point_v<R>) return ValueType::kFloat;
  else if constexpr (std::is_same_v<R, Object>) return ValueType::kObject;
  else return ValueType::kString;
}

template <typename F>
struct Signature : Signature<decltype(&F::operator())> {};
template <typename R, typename... A>
struct Signature<R (*)(A...)> {
  using Result = R;
  using Args = std::tuple<std::remove_cvref_t<A>...>;
};
template <typename C, typename R, typename... A>
struct Signature<R (C::*)(A...) const> : Signature<R (*)(A...)> {};
template <typename C, typename R, typename... A>
struct Signature<R (C::*)(A...)> : Signature<R (*)(A...)> {};

template <typename F, typename... A, size_t... I>
Value Invoke(const F& function, std::span<const Value> args, std::index_sequence<I...>) {
  if constexpr (std::is_void_v<std::invoke_result_t<const F&, A...>>) {
    function(Marshal<A>::From(args[I])...);
    return {};
  } else {
    return ToValue(function(Marshal<A>::From(args[I])...));
  }
}

template <typename F, typename Tuple>
struct Binder;
template <typename F, typename... A>
struct Binder<F, std::tuple<A...>> {
  static FunctionDef Make(std::string name, F function) {
    FunctionDef def{std::move(name), {Marshal<A>::kType...}, ResultType<typename Signature<std::decay_t<F>>::Result>()};
    def.invoke = [function = std::move(function)](std::span<const Value> args) { return Invoke<F, A...>(function, args, std::index_sequence_for<A...>{}); };
    return def;
  }
};

// Any callable with marshalable arguments: bool, integers, floats, std::string, std::string_view, const char*
// and Object.
template <typename F>
FunctionDef MakeFunction(std::string name, F function) {
  using Fn = std::decay_t<F>;
  return Binder<Fn, typename Signature<Fn>::Args>::Make(std::move(name), std::move(function));
}

}  // namespace detail

// A guest struct the game describes, so mods can read and write it by field name.
class TypeDef {
 public:
  TypeDef(std::string name, uint32_t size) : name_(std::move(name)), size_(size) {}
  TypeDef(const TypeDef&) = delete;
  TypeDef& operator=(const TypeDef&) = delete;

  TypeDef& Field(std::string name, uint32_t offset, FieldType type);
  // `count` elements of `type`, `stride` bytes apart (0: packed).
  TypeDef& Array(std::string name, uint32_t offset, FieldType type, uint32_t count, uint32_t stride = 0);
  // A function called on an object of this type. Its first argument is the object.
  template <typename F>
  TypeDef& Method(std::string name, F function) {
    AddMethod(detail::MakeFunction(name_ + ":" + name, std::move(function)), name);
    return *this;
  }

  const std::string& name() const { return name_; }  // full name, "tip.Pinata"
  uint32_t size() const { return size_; }
  const std::vector<FieldDef>& fields() const { return fields_; }
  const std::vector<std::pair<std::string, FunctionDef>>& methods() const { return methods_; }
  const FieldDef* FindField(std::string_view name) const;
  const FunctionDef* FindMethod(std::string_view name) const;

 private:
  void AddMethod(FunctionDef function, std::string short_name);

  std::string name_;
  uint32_t size_;
  std::vector<FieldDef> fields_;
  std::vector<std::pair<std::string, FunctionDef>> methods_;
};

// A named event the game fires and mods listen for. Cheap to copy and to fire with nobody listening.
class Event {
 public:
  explicit Event(std::string_view name);

  void Fire(std::span<const Value> args = {}) const;
  template <typename... A>
  void Fire(A&&... args) const {
    if (!HasListeners()) return;
    const Value values[] = {detail::ToValue(std::forward<A>(args))...};
    Fire(std::span<const Value>(values));
  }
  bool HasListeners() const;
  const std::string& name() const;

  struct Cell;

 private:
  Cell* cell_;
};

// Where a game's declarations live. Names given to it are relative: "garden.money" in namespace "tip" is
// "tip.garden.money". Builders for one name refer to the same namespace.
class Namespace {
 public:
  explicit Namespace(std::string name);

  template <typename F>
  Namespace& Function(std::string name, F function) {
    AddFunction(detail::MakeFunction(Qualify(name), std::move(function)));
    return *this;
  }
  Namespace& Constant(std::string name, Value value);
  Namespace& Enum(std::string name, std::vector<std::pair<std::string, int64_t>> values);
  // A type of `size` bytes. Asking again for the same name returns the same type.
  TypeDef& Type(std::string name, uint32_t size);
  rexfabric::Event Event(std::string name);
  // Lua run once the declarations are globals, before any mod: conveniences built on them, such as
  // functions taking tables or callbacks. Front ends without Lua ignore it.
  Namespace& Lua(std::string source);
  // Where the node editor lists declarations whose names start with `prefix` ("parts." or "part_"): `title`, a
  // top-level category of its add menu ("Parts"). The longest matching prefix wins; the rest go under Game.
  Namespace& Category(std::string prefix, std::string title);
  // The node editor's pins for a function's parameters or an event's values, in order: "id", or "part:Part" for an
  // address as one of the namespace's types ("->Vehicle" types a result). A named event has that many values, not three.
  Namespace& Args(std::string name, std::vector<std::string> names);
  // A list of (label, value) a text argument typed with its name in Args ("flag:crates.Flag") is picked from in the
  // node editor, instead of being typed in. An argument typed with an enum is picked from its values the same way.
  Namespace& Choices(std::string name, std::vector<std::pair<std::string, std::string>> choices);
  // A list worked out each time the node editor shows it, from the game as it is then: its flags, its worlds.
  Namespace& Choices(std::string name, std::function<std::vector<std::pair<std::string, std::string>>()> list);

  const std::string& name() const { return name_; }

 private:
  std::string Qualify(std::string_view name) const { return name_ + "." + std::string(name); }
  void AddFunction(FunctionDef function);

  std::string name_;
};

// What the game declared, for the front ends.
struct EnumDef {
  std::string name;
  std::vector<std::pair<std::string, int64_t>> values;
};

struct ConstantDef {
  std::string name;
  Value value;
};

struct NamespaceDef {
  std::string name;
  std::vector<FunctionDef> functions;
  std::vector<ConstantDef> constants;
  std::vector<EnumDef> enums;
  std::vector<std::unique_ptr<TypeDef>> types;
  std::vector<std::string> events;
  std::vector<std::string> lua;  // run in order, after every namespace's declarations
  std::vector<std::pair<std::string, std::string>> categories;  // name prefix (without the namespace), title
  std::vector<std::pair<std::string, std::vector<std::string>>> args;  // full name, its arguments' names
  std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> choices;  // full name, (label, value)
  std::vector<std::pair<std::string, std::function<std::vector<std::pair<std::string, std::string>>()>>> live_choices;
};

const std::vector<std::unique_ptr<NamespaceDef>>& Namespaces();
const FunctionDef* FindFunction(std::string_view name);
const TypeDef* FindType(std::string_view name);

// Native interfaces: a game can publish a plain C table of its own for native mods, which find it by name
// with RexFabricApi::find_interface. Versions only grow; a mod asks for the oldest version it can use.
void PublishInterface(std::string name, uint32_t version, const void* table);
const void* FindInterface(std::string_view name, uint32_t min_version);

}  // namespace rexfabric
